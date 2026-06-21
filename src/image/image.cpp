/*********************************************************************************************************************

This source code and its accompanying files are in the public domain and therefore may be distributed without
restriction.  The source is based in part on libpng, authored by Glenn Randers-Pehrson, Andreas Eric Dilger and
Guy Eric Schalnat.

**********************************************************************************************************************

-CLASS-
Image: Loads and saves image files in a variety of different data formats.

The Image class provides a standard API for programs to load image files of any supported data type.  It is future
proof in that future data formats can be supported by installing class drivers on the user's system.

The default file format for loading and saving images is PNG.  Other formats such as JPEG are supported via
derived classes, which can be loaded into the system at boot time or on demand.  Some rare formats such as TIFF are
also supported, but user preference may dictate whether or not the necessary driver is installed.

<header>Technical Notes</>

To find out general information about an image before initialising it, #Query() it first so that the image object
can load initial details on the file format.

Images are also remapped automatically if the source palette and destination palettes do not match, or if there are
significant differences between the source and destination bitmap types.

Dynamically sized image formats like SVG will use the #DisplayWidth and #DisplayHeight values to determine the
rendered image size.

-END-

*********************************************************************************************************************/

#define PNG_INTERNAL
#define PRV_PNG
#include "lib/png.h"
#include "lib/pngpriv.h"

#include <kotuku/main.h>
#include <kotuku/modules/image.h>
#include <kotuku/modules/display.h>
#include <kotuku/strings.hpp>
#include "../link/linear_rgb.h"

#include "image.h"

using namespace kt;

static OBJECTPTR clImage = nullptr;
static OBJECTPTR modDisplay = nullptr;
static thread_local bool tlError = false;

JUMPTABLE_CORE
JUMPTABLE_DISPLAY

static ERR decompress_png(extImage *, objBitmap *, int, int, png_structp, png_infop, png_uint_32, png_uint_32);
static void read_row_callback(png_structp, png_uint_32, int);
static void write_row_callback(png_structp, png_uint_32, int);
static void png_error_hook(png_structp png_ptr, png_const_charp message);
static void png_warning_hook(png_structp png_ptr, png_const_charp message);
static void kotuku_flush_callback(png_structp png);
void kotuku_read_callback(png_structp png, png_bytep data, png_size_t length);
void kotuku_write_callback(png_structp png, png_bytep data, png_size_t length);
static ERR create_image_class(void);

//********************************************************************************************************************

static void conv_l2r_row32(uint8_t *Row, int Width) {
   for (int x=0; x < Width; x++) {
      Row[0] = glLinearRGB.invert(Row[0]);
      Row[1] = glLinearRGB.invert(Row[1]);
      Row[2] = glLinearRGB.invert(Row[2]);
      Row += 4;
   }
}

//********************************************************************************************************************

static void conv_l2r_row24(uint8_t *Row, int Width) {
   for (int x=0; x < Width; x++) {
      Row[0] = glLinearRGB.invert(Row[0]);
      Row[1] = glLinearRGB.invert(Row[1]);
      Row[2] = glLinearRGB.invert(Row[2]);
      Row += 3;
   }
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;

   return(create_image_class());
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (clImage)    { FreeResource(clImage); clImage = nullptr; }
   if (modDisplay) { FreeResource(modDisplay); modDisplay = nullptr; }
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Activate: Loads image data into an image object.

Loading an image file requires a call to Activate() after initialisation.  The #Path field will be used to source
the image file.

Pre-setting image field values will place restrictions on the image file that is to be loaded.  For example, if
the source image is wider than a preset @Bitmap.Width, the image will have its right edge clipped.  The same is
true for the @Bitmap.Height and other restrictions apply to fields such as the @Bitmap.Palette.

Once the image is loaded, the image data will be held in the image's #Bitmap object.  Manipulating the #Bitmap
object is permitted.
-END-

*********************************************************************************************************************/

static ERR IMAGE_Activate(extImage *Self)
{
   kt::Log log;

   if (Self->Bitmap->initialised()) return ERR::Okay;

   log.branch();

   ERR error = ERR::Failed;
   tlError = false;

   auto bmp = Self->Bitmap;
   png_structp read_ptr = nullptr;
   png_infop info_ptr = nullptr;
   png_infop end_info = nullptr;
   bool file_opened = false;
   volatile bool mask_created = false;

   if (!Self->prvFile) {
      std::string_view path;
      if (Self->getPath(path) != ERR::Okay) return log.warning(ERR::GetField);

      if (!(Self->prvFile = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) goto exit;
      file_opened = true;
   }

   if ((error = Self->prvFile->seekStart(0)) != ERR::Okay) goto exit;

   // Allocate PNG structures

   if (!(read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, Self, &png_error_hook, &png_warning_hook))) goto exit;
   if (!(info_ptr = png_create_info_struct(read_ptr))) goto exit;
   if (!(end_info = png_create_info_struct(read_ptr))) goto exit;

   if (setjmp(png_jmpbuf(read_ptr))) {
      error = ERR::Read;
      goto exit;
   }

   // Setup the PNG file

   png_set_read_fn(read_ptr, Self->prvFile, kotuku_read_callback);

   png_set_read_status_fn(read_ptr, read_row_callback); if (tlError) goto exit;
   png_read_info(read_ptr, info_ptr); if (tlError) goto exit;

   int bit_depth, total_bit_depth, color_type;
   png_uint_32 png_width, png_height;
   png_get_IHDR(read_ptr, info_ptr, &png_width, &png_height, &bit_depth, &color_type, nullptr, nullptr, nullptr);
   if (tlError) goto exit;

   bmp->Width  = png_width;
   bmp->Height = png_height;
   if (bmp->Type IS BMP::NIL) bmp->Type = BMP::CHUNKY;

   if (!Self->DisplayWidth)  Self->DisplayWidth  = png_width;
   if (!Self->DisplayHeight) Self->DisplayHeight = png_height;

   // If the image contains a palette, load the palette into our Bitmap

   if (info_ptr->valid & PNG_INFO_PLTE) {
      for (int i=0; (i < info_ptr->num_palette) and (i < 256); i++) {
         bmp->Palette->Col[i].Red   = info_ptr->palette[i].red;
         bmp->Palette->Col[i].Green = info_ptr->palette[i].green;
         bmp->Palette->Col[i].Blue  = info_ptr->palette[i].blue;
         bmp->Palette->Col[i].Alpha = 255;
      }
   }
   else if (color_type IS PNG_COLOR_TYPE_GRAY) {
      for (int i=0; i < 256; i++) {
         bmp->Palette->Col[i].Red   = i;
         bmp->Palette->Col[i].Green = i;
         bmp->Palette->Col[i].Blue  = i;
         bmp->Palette->Col[i].Alpha = 255;
      }
   }

   if ((Self->Flags & PCF::FORCE_ALPHA_32) != PCF::NIL) {
      // Force the image to 32-bit and store the alpha channel in the alpha byte of the pixel data.

      bmp->BitsPerPixel  = 32;
      bmp->BytesPerPixel = 4;
      bmp->Flags |= BMF::ALPHA_CHANNEL;
   }
   else if (color_type & PNG_COLOR_MASK_ALPHA) {
      // An image can have an alpha channel that is separate to the RGB image data.
      if ((Self->Mask = objBitmap::create::local(
            fl::Width(Self->Bitmap->Width), fl::Height(Self->Bitmap->Height),
            fl::AmtColours(256), fl::Flags(BMF::MASK)))) {
         Self->Flags |= PCF::MASK|PCF::ALPHA;
         mask_created = true;
      }
      else goto exit;
   }

   // If a background colour has been specified for the image (instead of an alpha channel), read it and create the
   // mask based on the data that we have read.

   if (info_ptr->valid & PNG_INFO_tRNS) {
      // The first colour index in the list is taken as the background, any others are ignored

      RGB8 rgb;
      png_bytep trans_alpha = nullptr;
      png_color_16p trans_colour = nullptr;
      int num_trans = 0;

      if (png_get_tRNS(read_ptr, info_ptr, &trans_alpha, &num_trans, &trans_colour)) {
         if (info_ptr->color_type IS PNG_COLOR_TYPE_PALETTE) {
            int trans_index = -1;
            int trans_count = 0;
            bool partial_alpha = false;

            if (trans_alpha) {
               for (int i=0; i < num_trans; i++) {
                  if (trans_alpha[i] < 255) {
                     if (trans_index < 0) trans_index = i;
                     trans_count++;
                     if (trans_alpha[i] > 0) partial_alpha = true;
                  }
               }
            }

            if ((partial_alpha or (trans_count > 1)) and ((Self->Flags & PCF::FORCE_ALPHA_32) IS PCF::NIL)) {
               if (!Self->Mask) {
                  if ((Self->Mask = objBitmap::create::local(
                        fl::Width(Self->Bitmap->Width), fl::Height(Self->Bitmap->Height),
                        fl::AmtColours(256), fl::Flags(BMF::MASK)))) {
                     mask_created = true;
                  }
                  else goto exit;
               }

               Self->Flags |= PCF::MASK|PCF::ALPHA;
            }
            else if ((trans_index >= 0) and (trans_index < bmp->Palette->AmtColours)) {
               bmp->TransIndex = trans_index;
               rgb = bmp->Palette->Col[bmp->TransIndex];
               rgb.Alpha = 255;
               bmp->set(FID_Transparence, &rgb);
            }
         }
         else if ((info_ptr->color_type IS PNG_COLOR_TYPE_GRAY) or
                  (info_ptr->color_type IS PNG_COLOR_TYPE_GRAY_ALPHA)) {
            if (trans_colour) {
               rgb.Red   = trans_colour->gray;
               rgb.Green = trans_colour->gray;
               rgb.Blue  = trans_colour->gray;
               rgb.Alpha = 255;
               bmp->set(FID_Transparence, &rgb);
            }
         }
         else if (trans_colour) {
            rgb.Red   = trans_colour->red;
            rgb.Green = trans_colour->green;
            rgb.Blue  = trans_colour->blue;
            rgb.Alpha = 255;
            bmp->set(FID_Transparence, &rgb);
         }
      }
   }

   if (info_ptr->valid & PNG_INFO_bKGD) {
      png_color_16p prgb;
      prgb = &(info_ptr->background);
      if (color_type IS PNG_COLOR_TYPE_PALETTE) {
         int bkgd_index = prgb->index;
         if ((bkgd_index >= 0) and (bkgd_index < bmp->Palette->AmtColours)) {
            bmp->Bkgd.Red   = bmp->Palette->Col[bkgd_index].Red;
            bmp->Bkgd.Green = bmp->Palette->Col[bkgd_index].Green;
            bmp->Bkgd.Blue  = bmp->Palette->Col[bkgd_index].Blue;
            bmp->Bkgd.Alpha = 255;
         }
      }
      else if ((color_type IS PNG_COLOR_TYPE_GRAY) or (color_type IS PNG_COLOR_TYPE_GRAY_ALPHA)) {
         bmp->Bkgd.Red   = prgb->gray;
         bmp->Bkgd.Green = prgb->gray;
         bmp->Bkgd.Blue  = prgb->gray;
         bmp->Bkgd.Alpha = 255;
      }
      else {
         bmp->Bkgd.Red   = prgb->red;
         bmp->Bkgd.Green = prgb->green;
         bmp->Bkgd.Blue  = prgb->blue;
         bmp->Bkgd.Alpha = 255;
      }
      log.trace("Background Colour: %d,%d,%d", bmp->Bkgd.Red, bmp->Bkgd.Green, bmp->Bkgd.Blue);
   }

   // Set the bits per pixel value

   switch (color_type) {
      case PNG_COLOR_TYPE_GRAY:       total_bit_depth = std::max(bit_depth, 8); break;
      case PNG_COLOR_TYPE_PALETTE:    total_bit_depth = std::max(bit_depth, 8); break;
      case PNG_COLOR_TYPE_RGB:        total_bit_depth = std::max(bit_depth, 8) * 3; break;
      case PNG_COLOR_TYPE_RGB_ALPHA:  total_bit_depth = std::max(bit_depth, 8) * 4; break;
      case PNG_COLOR_TYPE_GRAY_ALPHA: total_bit_depth = std::max(bit_depth, 8) * 2; break;
      default:
         log.warning("Unrecognised colour type 0x%x.", color_type);
         total_bit_depth = std::max(bit_depth, 8);
   }

   if (!bmp->BitsPerPixel) {
      if ((color_type IS PNG_COLOR_TYPE_GRAY) or (color_type IS PNG_COLOR_TYPE_PALETTE)) {
         bmp->BitsPerPixel = 8;
      }
      else bmp->BitsPerPixel = 24;
   }

   if (((Self->Flags & PCF::NO_PALETTE) != PCF::NIL) and (bmp->BitsPerPixel <= 8)) {
      bmp->BitsPerPixel = 32;
   }

   if ((bmp->BitsPerPixel < 24) and
       ((bmp->BitsPerPixel < total_bit_depth) or
        ((total_bit_depth <= 8) and (bmp->BitsPerPixel > 8)))) {

      log.msg("Destination Depth %d < Image Depth %d - Dithering.", bmp->BitsPerPixel, total_bit_depth);

      // Init our bitmap, since decompress_png() won't in this case.

      if (error = bmp->query(); error != ERR::Okay) goto exit;
      if (!bmp->initialised()) {
         if (error = bmp->init(); error != ERR::Okay) goto exit;
      }

      objBitmap::create tmp_bitmap = {
         fl::Width(bmp->Width), fl::Height(bmp->Height), fl::BitsPerPixel(total_bit_depth)
      };

      if (tmp_bitmap.ok()) {
         if (!(error = decompress_png(Self, *tmp_bitmap, bit_depth, color_type, read_ptr, info_ptr, png_width, png_height))) {
            error = gfx::CopyArea(*tmp_bitmap, bmp, BAF::DITHER, 0, 0, bmp->Width, bmp->Height, 0, 0);
         }
      }
   }
   else error = decompress_png(Self, bmp, bit_depth, color_type, read_ptr, info_ptr, png_width, png_height);

   if (!error) {
      if (setjmp(png_jmpbuf(read_ptr))) {
         error = ERR::Read;
         goto exit;
      }

      png_read_end(read_ptr, end_info);
      if (Self->prvFile) { FreeResource(Self->prvFile); Self->prvFile = nullptr; }
   }
   else {
exit:
      if (mask_created and Self->Mask) {
         FreeResource(Self->Mask);
         Self->Mask = nullptr;
         Self->Flags &= ~(PCF::MASK|PCF::ALPHA);
      }

      if (file_opened and Self->prvFile) {
         FreeResource(Self->prvFile);
         Self->prvFile = nullptr;
      }

      log.warning(error);
   }

   png_destroy_read_struct(&read_ptr, &info_ptr, &end_info);

   return error;
}

//********************************************************************************************************************

static ERR IMAGE_Free(extImage *Self)
{
   if (Self->prvFile) { FreeResource(Self->prvFile); Self->prvFile = nullptr; }
   if (Self->Bitmap)  { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }
   if (Self->Mask)    { FreeResource(Self->Mask); Self->Mask = nullptr; }
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Init: Prepares the object for use.

Objects that belong to the Image class can be initialised in two possible ways.  If you have not set the
#Path field or have chosen to use the `NEW` flag, the initialisation routine will create a
#Bitmap area that contains no image data.  This allows you to fill the image with your own image data and
save it using the #SaveImage() or #SaveToObject() actions.  You must set the @Bitmap.Width, @Bitmap.Height
and colour specifications at a minimum, or the initialisation process will fail.

If you have set the #Path field and avoided the `NEW` flag, the initialisation process will analyse the
file location to determine whether or not the data is in fact a valid image file.  If the file does not match up
with a registered data format, an error code of `ERR::NoSupport` is returned.  You will need to use the #Activate() or
#Query() actions to load or find out more information about the image format.
-END-

*********************************************************************************************************************/

static ERR IMAGE_Init(extImage *Self)
{
   kt::Log log;

   if ((Self->Path.empty()) or ((Self->Flags & PCF::NEW) != PCF::NIL)) {
      // If no path has been specified, assume that the image is being created from scratch (e.g. to save an
      // image to disk).  The programmer is required to specify the dimensions and colours of the Bitmap so that we can
      // initialise it.

      if ((Self->Flags & PCF::FORCE_ALPHA_32) != PCF::NIL) {
         Self->Bitmap->BitsPerPixel  = 32;
         Self->Bitmap->BytesPerPixel = 4;
         Self->Bitmap->Flags |= BMF::ALPHA_CHANNEL;
      }

      Self->Flags &= ~(PCF::LAZY|PCF::SCALABLE); // Turn off irrelevant flags that don't match these

      if (!Self->Bitmap->Width) Self->Bitmap->Width = Self->DisplayWidth;
      if (!Self->Bitmap->Height) Self->Bitmap->Height = Self->DisplayHeight;

      if ((Self->Bitmap->Width) and (Self->Bitmap->Height)) {
         if (!InitObject(Self->Bitmap)) {
            if ((Self->Flags & PCF::FORCE_ALPHA_32) != PCF::NIL) Self->Flags &= ~(PCF::ALPHA|PCF::MASK);

            if ((Self->Flags & (PCF::ALPHA|PCF::MASK)) != PCF::NIL) {
               if ((Self->Mask = objBitmap::create::local(fl::Width(Self->Bitmap->Width),
                     fl::Height(Self->Bitmap->Height),
                     fl::Flags(BMF::MASK),
                     fl::BitsPerPixel(((Self->Flags & PCF::ALPHA) != PCF::NIL) ? 8 : 1)))) {
                  Self->Flags |= PCF::MASK;
               }
               else return log.warning(ERR::Init);
            }

            if (Self->isDerived()) return ERR::Okay; // Break here to let the derived class continue initialisation

            return ERR::Okay;
         }
         else return log.warning(ERR::Init);
      }
      else return log.warning(ERR::InvalidDimension);
   }
   else {
      if (Self->isDerived()) return ERR::Okay; // Break here to let the derived class continue initialisation

      // Test the given path to see if it matches our supported file format.

      if (!ResolvePath(Self->Path, RSF::APPROXIMATE, &Self->Path)) {
         int result;

         if (!ReadFileToBuffer(Self->Path, Self->prvHeader, sizeof(Self->prvHeader)-1, &result)) {
            Self->prvHeader[result] = 0;

            auto buffer = (uint8_t *)Self->prvHeader;

            if ((result >= 8) and
                (buffer[0] IS 0x89) and (buffer[1] IS 0x50) and (buffer[2] IS 0x4e) and (buffer[3] IS 0x47) and
                (buffer[4] IS 0x0d) and (buffer[5] IS 0x0a) and (buffer[6] IS 0x1a) and (buffer[7] IS 0x0a)) {
               if ((Self->Flags & PCF::LAZY) != PCF::NIL) return ERR::Okay;
               return acActivate(Self);
            }
            else return ERR::NoSupport;
         }
         else {
            log.warning("Failed to read '%s'", Self->Path.c_str());
            return ERR::File;
         }
      }
      else {
         log.warning("Failed to find '%s'", Self->Path.c_str());
         return ERR::FileNotFound;
      }
   }

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR IMAGE_NewObject(extImage *Self)
{
   return NewLocalObject(CLASSID::BITMAP, &Self->Bitmap);
}

//********************************************************************************************************************

static ERR IMAGE_Query(extImage *Self)
{
   kt::Log log;
   std::string_view path;
   png_uint_32 width, height;
   int bit_depth, color_type;

   if ((Self->Bitmap->Flags & BMF::QUERIED) != BMF::NIL) return ERR::Okay;

   log.branch();

   objBitmap *Bitmap = Self->Bitmap;
   ERR error = ERR::Failed;
   png_structp read_ptr = nullptr;
   png_infop info_ptr = nullptr;
   png_infop end_info = nullptr;
   tlError = false;

   // Open the data file

   if (!Self->prvFile) {
      if (Self->getPath(path) != ERR::Okay) return log.warning(ERR::GetField);

      if (!(Self->prvFile = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) goto exit;
   }

   if ((error = Self->prvFile->seekStart(0)) != ERR::Okay) goto exit;

   // Allocate PNG structures

   if (!(read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, Self, &png_error_hook, &png_warning_hook))) goto exit;
   if (!(info_ptr = png_create_info_struct(read_ptr))) goto exit;
   if (!(end_info = png_create_info_struct(read_ptr))) goto exit;

   if (setjmp(png_jmpbuf(read_ptr))) {
      error = ERR::Read;
      goto exit;
   }

   // Read the PNG description

   png_set_read_fn(read_ptr, Self->prvFile, kotuku_read_callback);

   png_set_read_status_fn(read_ptr, read_row_callback); if (tlError) goto exit;
   png_read_info(read_ptr, info_ptr); if (tlError) goto exit;
   png_get_IHDR(read_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr); if (tlError) goto exit;

   if (!Bitmap->Width)  Bitmap->Width  = width;
   if (!Bitmap->Height) Bitmap->Height = height;
   if (Bitmap->Type IS BMP::NIL) Bitmap->Type = BMP::CHUNKY;

   if (!Self->DisplayWidth)  Self->DisplayWidth  = width;
   if (!Self->DisplayHeight) Self->DisplayHeight = height;
   if (color_type & PNG_COLOR_MASK_ALPHA) Self->Flags |= PCF::ALPHA;

   if (!Bitmap->BitsPerPixel) {
      if ((color_type IS PNG_COLOR_TYPE_GRAY) or (color_type IS PNG_COLOR_TYPE_PALETTE)) {
         Bitmap->BitsPerPixel = 8;
         Bitmap->BytesPerPixel = 1;
      }
      else {
         Bitmap->BitsPerPixel = 24;
         Bitmap->BytesPerPixel = 3;
      }
   }

//   acQuery(Bitmap);

   error = ERR::Okay;

exit:
   png_destroy_read_struct(&read_ptr, &info_ptr, &end_info);
   return error;
}

/*********************************************************************************************************************
-ACTION-
Read: Reads raw image data from an Image object.
-END-
*********************************************************************************************************************/

static ERR IMAGE_Read(extImage *Self, struct acRead *Args)
{
   return Action(AC::Read, Self->Bitmap, Args);
}

/*********************************************************************************************************************
-ACTION-
Refresh: Refreshes a loaded image - draws the next frame.
-END-
*********************************************************************************************************************/

static ERR IMAGE_Refresh(extImage *Self)
{
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveImage: Saves the image to a data object.

If no destination is specified then the image will be saved as a new file targeting #Path.

-END-
*********************************************************************************************************************/

static ERR IMAGE_SaveImage(extImage *Self, struct acSaveImage *Args)
{
   kt::Log log;
   std::string_view path;
   png_bytep row_pointers;
   uint8_t *row_buffer = nullptr;
   png_color palette[256];

   log.branch();

   objBitmap *bmp        = Self->Bitmap;
   OBJECTPTR file        = nullptr;
   png_structp write_ptr = nullptr;
   png_infop info_ptr    = nullptr;
   ERR error = ERR::Failed;
   tlError = false;

   bool alpha_mask = ((Self->Flags & PCF::ALPHA) != PCF::NIL);

   if (((Self->Flags & (PCF::ALPHA|PCF::MASK)) != PCF::NIL) and (!Self->Mask)) {
      log.warning("Illegal use of the ALPHA/MASK flags without an accompanying mask bitmap.");
      return log.warning(ERR::Args);
   }

   if ((Args) and (Args->Dest)) file = Args->Dest;
   else {
      if (Self->getPath(path) != ERR::Okay) return log.warning(ERR::MissingPath);

      if (!(file = objFile::create::global(fl::Path(path), fl::Flags(FL::NEW|FL::WRITE)))) return ERR::CreateObject;
   }

   // Allocate PNG structures

   if (!(write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, Self, &png_error_hook, &png_warning_hook))) {
      log.warning("png_create_write_struct() failed.");
      goto exit;
   }

   png_set_error_fn(write_ptr, Self, &png_error_hook, &png_warning_hook);

   if (!(info_ptr = png_create_info_struct(write_ptr))) {
      log.warning("png_create_info_struct() failed.");
      goto exit;
   }

   // Setup the PNG file

   png_set_write_fn(write_ptr, file, kotuku_write_callback, kotuku_flush_callback);

   png_set_write_status_fn(write_ptr, write_row_callback);
   if (tlError) {
      log.warning("png_set_write_status_fn() failed.");
      goto exit;
   }

   if (alpha_mask or (bmp->BitsPerPixel IS 32) or (bmp->BytesPerPixel IS 2)) {
      if ((error = AllocMemory(size_t(bmp->Width) * 4, MEM::DATA|MEM::NO_CLEAR, (APTR *)&row_buffer)) != ERR::Okay) {
         goto exit;
      }
   }

   if (setjmp(png_jmpbuf(write_ptr))) {
      error = ERR::Write;
      goto exit;
   }

   if ((bmp->AmtColours > 256) or alpha_mask) {
      if ((bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
         log.trace("Saving as 32-bit alpha.");
         png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
            PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      }
      else if (alpha_mask) {
         log.trace("Saving with alpha-mask.");
         png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
            PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      }
      else {
         log.trace("Saving in standard chunky graphics mode (no alpha).");
         png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_RGB,
            PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      }
   }
   else {
      int palette_count = bmp->Palette ? bmp->Palette->AmtColours : 0;

      if (palette_count > bmp->AmtColours) palette_count = bmp->AmtColours;
      if (palette_count > 256) palette_count = 256;
      if (palette_count <= 0) {
         error = ERR::Args;
         goto exit;
      }

      png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_PALETTE,
         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

      for (int i=0; i < palette_count; i++) {
         palette[i] = { bmp->Palette->Col[i].Red, bmp->Palette->Col[i].Green, bmp->Palette->Col[i].Blue };
      }

      png_set_PLTE(write_ptr, info_ptr, palette, palette_count);
   }

   // On Intel CPU's the pixel format is BGR

   png_set_bgr(write_ptr);

   // Set the background colour

   if (bmp->Bkgd.Alpha) {
      png_color_16 rgb;
      if (bmp->AmtColours < 256) rgb.index = bmp->BkgdIndex;
      else rgb.index = 0;
      rgb.red   = bmp->Bkgd.Red;
      rgb.green = bmp->Bkgd.Green;
      rgb.blue  = bmp->Bkgd.Blue;
      png_set_bKGD(write_ptr, info_ptr, &rgb);
   }

   // Set the transparent colour

   if (bmp->TransColour.Alpha) {
      png_color_16 rgb;
      if (bmp->AmtColours < 256) rgb.index = bmp->TransIndex;
      else rgb.index = 0;
      rgb.red   = bmp->TransColour.Red;
      rgb.green = bmp->TransColour.Green;
      rgb.blue  = bmp->TransColour.Blue;
      png_set_tRNS(write_ptr, info_ptr, &rgb.index, 1, &rgb);
   }

   // Write the header to the PNG file

   png_write_info(write_ptr, info_ptr);
   if (tlError) {
      log.warning("png_write_info() failed.");
      goto exit;
   }

   // Write the image data to the PNG file

   if (bmp->BitsPerPixel IS 8) {
      if (alpha_mask) {
         row_pointers = row_buffer;
         uint8_t *data = bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         int palette_count = bmp->Palette ? bmp->Palette->AmtColours : 0;

         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < bmp->Width; x++) {
               if (data[x] >= palette_count) {
                  error = ERR::Args;
                  goto exit;
               }

               auto &colour = bmp->Palette->Col[data[x]];
               row_buffer[i++] = colour.Blue;
               row_buffer[i++] = colour.Green;
               row_buffer[i++] = colour.Red;
               row_buffer[i++] = mask[x];
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data += bmp->LineWidth;
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         for (int y=0; y < bmp->Height; y++) {
            row_pointers = bmp->Data + (y * bmp->LineWidth);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
         }
      }
   }
   else if (bmp->BitsPerPixel IS 24) {
      if (alpha_mask) {
         row_pointers = row_buffer;
         uint8_t *data = bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < bmp->Width; x++) {
               int data_x = x * 3;
               row_buffer[i++] = data[data_x+0];  // Blue
               row_buffer[i++] = data[data_x+1];  // Green
               row_buffer[i++] = data[data_x+2];  // Red
               row_buffer[i++] = mask[x];         // Alpha
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data += bmp->LineWidth;
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         for (int y=0; y < bmp->Height; y++) {
            row_pointers = bmp->Data + (y * bmp->LineWidth);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
         }
      }
   }
   else if (bmp->BitsPerPixel IS 32) {
      if ((bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
         row_pointers = row_buffer;
         uint8_t *data = bmp->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < (bmp->Width<<2); x+=4) {
               row_buffer[i++] = data[x+0];  // Blue
               row_buffer[i++] = data[x+1];  // Green
               row_buffer[i++] = data[x+2];  // Red
               row_buffer[i++] = data[x+3];  // Alpha
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data += bmp->LineWidth;
         }
      }
      else if (alpha_mask) {
         row_pointers = row_buffer;
         uint8_t *data = bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            int mask_x = 0;
            for (int x=0; x < (bmp->Width<<2); x+=4) {
               row_buffer[i++] = data[x+0];       // Blue
               row_buffer[i++] = data[x+1];       // Green
               row_buffer[i++] = data[x+2];       // Red
               row_buffer[i++] = mask[mask_x++];  // Alpha
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data += bmp->LineWidth;
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         row_pointers = row_buffer;
         uint8_t *data = bmp->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < (bmp->Width<<2); x+=4) {
               row_buffer[i++] = data[x+0];  // Blue
               row_buffer[i++] = data[x+1];  // Green
               row_buffer[i++] = data[x+2];  // Red
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row24(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data += bmp->LineWidth;
         }
      }
   }
   else if (bmp->BytesPerPixel IS 2) {
      if (alpha_mask) {
         row_pointers = row_buffer;
         uint16_t *data = (uint16_t *)bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            int mask_x = 0;
            for (int x=0; x < bmp->Width; x++) {
               row_buffer[i++] = bmp->unpackBlue(data[x]);
               row_buffer[i++] = bmp->unpackGreen(data[x]);
               row_buffer[i++] = bmp->unpackRed(data[x]);
               row_buffer[i++] = mask[mask_x++];
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data = (uint16_t *)(((uint8_t *)data) + bmp->LineWidth);
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         row_pointers = row_buffer;
         uint16_t *data = (uint16_t *)bmp->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < bmp->Width; x++) {
               row_buffer[i++] = bmp->unpackBlue(data[x]);
               row_buffer[i++] = bmp->unpackGreen(data[x]);
               row_buffer[i++] = bmp->unpackRed(data[x]);
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row24(row_buffer, bmp->Width);
            png_write_row(write_ptr, row_pointers);
            if (tlError) { error = ERR::Write; goto exit; }
            data = (uint16_t *)(((uint8_t *)data) + bmp->LineWidth);
         }
      }
   }

   png_write_end(write_ptr, nullptr);
   if (tlError) { error = ERR::Write; goto exit; }

   error = ERR::Okay;

exit:
   if (row_buffer) FreeResource(row_buffer);
   png_destroy_write_struct(&write_ptr, &info_ptr);

   if ((Args) and (Args->Dest));
   else if (file) FreeResource(file);

   if (error != ERR::Okay) return log.warning(error);
   else return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves the image to a data object.
-END-
*********************************************************************************************************************/

static ERR IMAGE_SaveToObject(extImage *Self, struct acSaveToObject *Args)
{
   kt::Log log;
   ERR (**routine)(OBJECTPTR, APTR);

   if (!Args) return log.warning(ERR::NullArgs);

   if ((Args->ClassID != CLASSID::NIL) and (Args->ClassID != CLASSID::IMAGE)) {
      auto mc = (objMetaClass *)FindClass(Args->ClassID);
      if (!mc) return log.warning(ERR::NoSupport);

      if ((!mc->get(FID_ActionTable, routine)) and (routine)) {
         if ((routine[int(AC::SaveToObject)]) and (routine[int(AC::SaveToObject)] != (APTR)IMAGE_SaveToObject)) {
            return routine[int(AC::SaveToObject)](Self, Args);
         }
         else if ((routine[int(AC::SaveImage)]) and (routine[int(AC::SaveImage)] != (APTR)IMAGE_SaveImage)) {
            struct acSaveImage saveimage;
            saveimage.Dest = Args->Dest;
            return routine[int(AC::SaveImage)](Self, &saveimage);
         }
         else return log.warning(ERR::NoSupport);
      }
      else return log.warning(ERR::GetField);
   }
   else return acSaveImage(Self, Args->Dest, Args->ClassID);
}

/*********************************************************************************************************************
-ACTION-
Seek: Seeks to a new read/write position within an Image object.
-END-
*********************************************************************************************************************/

static ERR IMAGE_Seek(extImage *Self, struct acSeek *Args)
{
   return Action(AC::Seek, Self->Bitmap, Args);
}

/*********************************************************************************************************************
-ACTION-
Write: Writes raw image data to an image object.
-END-
*********************************************************************************************************************/

static ERR IMAGE_Write(extImage *Self, struct acWrite *Args)
{
   return Action(AC::Write, Self->Bitmap, Args);
}

/*********************************************************************************************************************

-FIELD-
Author: The name of the person or company that created the image.

*********************************************************************************************************************/

static ERR GET_Author(extImage *Self, std::string_view &Value)
{
   if (not Self->prvAuthor.empty()) {
      Value = Self->prvAuthor;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Author(extImage *Self, std::string_view &Value)
{
   Self->prvAuthor.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Bitmap: Represents image data.

The image graphics data is defined in its associated bitmap object.  It contains
information on the image dimensions and palette for example.  The @Bitmap.Palette can be preset if you want to
remap the  source image to a specific set of colour values.

Please refer to the @Bitmap class for more details on the structure of bitmap objects.

-FIELD-
Copyright: Copyright details of an image.

Copyright details related to an image may be specified here.  The copyright should be short and to the point, for
example `Copyright J. Bloggs (c) 1992.`

*********************************************************************************************************************/

static ERR GET_Copyright(extImage *Self, std::string_view &Value)
{
   if (not Self->prvCopyright.empty()) {
      Value = Self->prvCopyright;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Copyright(extImage *Self, std::string_view &Value)
{
   Self->prvCopyright.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Description: Long description for an image.

A long description for an image may be entered in this field.  There is no strict limit on the length of the
description.

*********************************************************************************************************************/

static ERR GET_Description(extImage *Self, std::string_view &Value)
{
   if (not Self->prvDescription.empty()) {
      Value = Self->prvDescription;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Description(extImage *Self, std::string_view &Value)
{
   Self->prvDescription.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Disclaimer: The disclaimer associated with an image.

If it is necessary to associate a disclaimer with an image, the legal text may be entered in this field.

-FIELD-
DisplayHeight: The preferred height to use when displaying the image.

The #DisplayWidth and DisplayHeight fields define the preferred pixel dimensions to use for the display when viewing the
image in a 96DPI environment.  Both fields will be set automatically when the image source is loaded.  If
the source does not specify a suitable value for these fields, they may be initialised to a value based on the
image's @Bitmap.Width and @Bitmap.Height.

In the case of a scalable image source such as SVG, the #DisplayWidth and DisplayHeight can be pre-configured by the
client, and the loader will scale the source image to the preferred dimensions on load.

-FIELD-
DisplayWidth: The preferred width to use when displaying the image.

The DisplayWidth and #DisplayHeight fields define the preferred pixel dimensions to use for the display when viewing the
image in a 96DPI environment.  Both fields will be set automatically when the image source is loaded.  If
the source does not specify a suitable value for these fields, they may be initialised to a value based on the
image's @Bitmap.Width and @Bitmap.Height.

In the case of a scalable image source such as SVG, the DisplayWidth and #DisplayHeight can be pre-configured by the
client, and the loader will scale the source image to the preferred dimensions on load.

-FIELD-
Flags:  Optional initialisation flags.

-FIELD-
Header: Contains the first 32 bytes of data in an image's file header.

The Header field is a pointer to a 32 byte buffer that contains the first 32 bytes of information read from an image
file on initialisation.  This special field is considered to be helpful only to developers writing add on components
for the image class.

The buffer that is referred to by the Header field is not populated until the Init action is called on the image object.

*********************************************************************************************************************/

static ERR GET_Header(extImage *Self, APTR *Value)
{
   *Value = Self->prvHeader;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Path: The location of source image data.

-FIELD-
Mask: Refers to a Bitmap that imposes a mask on the image.

If a source image includes a mask, the Mask field will refer to a Bitmap object that contains the mask image once the
image source has been loaded.  The mask will be expressed as either a 256 colour alpha bitmap, or a 1-bit mask with
8 pixels per byte.

If creating an image from scratch that needs to support a mask, set the `MASK` flag prior to initialisation
and the image class will allocate the mask bitmap automatically.

-FIELD-
Quality: Defines the quality level to use when saving the image.

The quality level to use when saving the image is defined here.  The value is expressed as a percentage between 0 and
100%, with 100% being of the highest quality.  If the image format is loss-less, such as PNG, then the quality level
may be used to determine the compression factor.

In all cases, the impact of selecting a high level of quality will increase the time it takes to save the image.

-FIELD-
Software: The name of the application that was used to draw the image.

-FIELD-
Title: The title of the image.
-END-
*********************************************************************************************************************/

static void read_row_callback(png_structp read_ptr, png_uint_32 row, int pass)
{

}

static void write_row_callback(png_structp write_ptr, png_uint_32 row, int pass)
{

}

//********************************************************************************************************************
// Read functions

void kotuku_read_callback(png_structp png, png_bytep data, png_size_t length)
{
   struct acRead read = { data, (int)length };
   if ((Action(AC::Read, (OBJECTPTR)png_get_io_ptr(png), &read) != ERR::Okay) or ((png_size_t)read.Result != length)) {
      png_error(png, "File read error");
   }
}

//********************************************************************************************************************
// Write functions.

void kotuku_write_callback(png_structp png, png_bytep data, png_size_t length)
{
   struct acWrite write = { data, (int)length };
   if ((Action(AC::Write, (OBJECTPTR)png_get_io_ptr(png), &write) != ERR::Okay) or ((png_size_t)write.Result != length)) {
      png_error(png, "File write error");
   }
}

static void kotuku_flush_callback(png_structp png)
{
}



//********************************************************************************************************************
// PNG Error Handling Functions

static void png_error_hook(png_structp png_ptr, png_const_charp message)
{
   kt::Log log;
   log.warning("%s", message);
   tlError = true;
   png_longjmp(png_ptr, 1);
}

static void png_warning_hook(png_structp png_ptr, png_const_charp message)
{
   kt::Log log;
   log.msg("libpng: %s", message); // PNG warnings aren't serious enough to warrant logging beyond the info level
}

//********************************************************************************************************************

static ERR decompress_png(extImage *Self, objBitmap *Bitmap, int BitDepth, int ColourType, png_structp ReadPtr,
                            png_infop InfoPtr, png_uint_32 PngWidth, png_uint_32 PngHeight)
{
   ERR error = ERR::Failed;
   uint8_t *row = nullptr;
   uint8_t *image_data = nullptr;
   uint8_t *scratch_row = nullptr;
   png_bytep row_pointers;
   RGB8 rgb;
   int i;
   kt::Log log(__FUNCTION__);

   // Read the image data into our Bitmap

   if (setjmp(png_jmpbuf(ReadPtr))) return ERR::Read;

   if (ColourType & PNG_COLOR_MASK_ALPHA) png_set_expand(ReadPtr); // Alpha channel
   if (BitDepth IS 16) png_set_strip_16(ReadPtr); // Reduce bit depth to 24bpp if the image is 48bpp
   if (BitDepth < 8) {
      if (ColourType IS PNG_COLOR_TYPE_GRAY) png_set_expand_gray_1_2_4_to_8(ReadPtr);
      else png_set_packing(ReadPtr);
   }

   auto interlace_type = png_get_interlace_type(ReadPtr, InfoPtr);

   log.branch("Size: %dx%dx%d, Interlace: %d", (int)PngWidth, (int)PngHeight, BitDepth, interlace_type);

   int passes = 1;
   if (interlace_type IS PNG_INTERLACE_ADAM7) passes = png_set_interlace_handling(ReadPtr);

   double file_gamma;
   if (png_get_gAMA(ReadPtr, InfoPtr, &file_gamma)) png_set_gamma(ReadPtr, 2.2, file_gamma);

   png_read_update_info(ReadPtr, InfoPtr);

   auto row_size = png_get_rowbytes(ReadPtr, InfoPtr);
   if (row_size > size_t(0x7fffffff)) return ERR::DataSize;

   if ((error = acQuery(Bitmap)) != ERR::Okay) return error;
   if (!Bitmap->initialised()) {
      if ((error = InitObject(Bitmap)) != ERR::Okay) return error;
   }

   Bitmap->clear();

   if (ColourType IS PNG_COLOR_TYPE_PALETTE) {
      png_colorp palette = nullptr;
      int palette_count = 0;
      png_bytep trans_alpha = nullptr;
      png_color_16p trans_colour = nullptr;
      int num_trans = 0;

      if (png_get_PLTE(ReadPtr, InfoPtr, &palette, &palette_count)) {
         for (int i=0; (i < palette_count) and (i < 256); i++) {
            Bitmap->Palette->Col[i].Red   = palette[i].red;
            Bitmap->Palette->Col[i].Green = palette[i].green;
            Bitmap->Palette->Col[i].Blue  = palette[i].blue;
            Bitmap->Palette->Col[i].Alpha = 255;
         }
      }

      if (png_get_tRNS(ReadPtr, InfoPtr, &trans_alpha, &num_trans, &trans_colour) and trans_alpha) {
         for (int i=0; (i < num_trans) and (i < 256); i++) {
            Bitmap->Palette->Col[i].Alpha = trans_alpha[i];
         }
      }
   }

   // Chop the image to the bitmap dimensions

   auto source_height = PngHeight;
   if (PngWidth > (png_uint_32)Bitmap->Width) PngWidth = Bitmap->Width;
   if (PngHeight > (png_uint_32)Bitmap->Height) PngHeight = Bitmap->Height;

   bool interlaced = interlace_type IS PNG_INTERLACE_ADAM7;

   if (interlaced) {
      if (PngHeight > 0) {
         if (row_size > (size_t(0x7fffffff) / size_t(PngHeight))) return ERR::DataSize;
         auto image_size = row_size * size_t(PngHeight);
         if ((error = AllocMemory((int)image_size, MEM::DATA, (APTR *)&image_data)) != ERR::Okay) return error;
      }

      if (source_height > PngHeight) {
         if ((error = AllocMemory((int)row_size, MEM::DATA, (APTR *)&scratch_row)) != ERR::Okay) goto exit;
      }
   }
   else if ((error = AllocMemory((int)row_size, MEM::DATA|MEM::NO_CLEAR, (APTR *)&row)) != ERR::Okay) return error;

   if (setjmp(png_jmpbuf(ReadPtr))) {
      error = ERR::Read;
      goto exit;
   }

   row_pointers = row;

   if (interlaced) {
      for (int pass=0; pass < passes; pass++) {
         for (png_uint_32 y=0; y < source_height; y++) {
            png_bytep output_row = (y < PngHeight) ? image_data + (size_t(y) * row_size) : scratch_row;
            png_read_row(ReadPtr, output_row, nullptr); if (tlError) goto exit;
         }
      }

      passes = 1;
   }

   if (ColourType IS PNG_COLOR_TYPE_GRAY) {
      log.trace("Greyscale image source.");
      rgb.Alpha = 255;

      while (passes > 0) {
         for (png_uint_32 y=0; y < PngHeight; y++) {
            auto source_row = interlaced ? image_data + (size_t(y) * row_size) : row;
            if (!interlaced) { png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit; }
            for (png_uint_32 x=0; x < PngWidth; x++) {
               rgb.Red   = source_row[x];
               rgb.Green = source_row[x];
               rgb.Blue  = source_row[x];
               Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
            }
         }
         passes--;
      }
   }
   else if (ColourType IS PNG_COLOR_TYPE_PALETTE) {
      log.trace("Palette-based image source.");

      while (passes > 0) {
         if (Bitmap->BitsPerPixel IS 8) {
            for (png_uint_32 y=0; y < PngHeight; y++) {
               auto source_row = interlaced ? image_data + (size_t(y) * row_size) : row;
               if (!interlaced) { png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit; }
               for (png_uint_32 x=0; x < PngWidth; x++) {
                  Bitmap->DrawUCPixel(Bitmap, x, y, source_row[x]);
                  if (Self->Mask) Self->Mask->Data[(y * Self->Mask->LineWidth) + x] =
                     Bitmap->Palette->Col[source_row[x]].Alpha;
               }
            }
         }
         else {
            rgb.Alpha = 255;
            for (png_uint_32 y=0; y < PngHeight; y++) {
               auto source_row = interlaced ? image_data + (size_t(y) * row_size) : row;
               if (!interlaced) { png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit; }
               for (png_uint_32 x=0; x < PngWidth; x++) {
                  auto &palette_colour = Bitmap->Palette->Col[source_row[x]];

                  Bitmap->DrawUCRPixel(Bitmap, x, y, &palette_colour);
                  if (Self->Mask) Self->Mask->Data[(y * Self->Mask->LineWidth) + x] = palette_colour.Alpha;
               }
            }
         }
         passes--;
      }
   }
   else if (ColourType IS PNG_COLOR_TYPE_GRAY_ALPHA) {
      log.trace("Greyscale + alpha image source.");

      while (passes > 0) {
         for (png_uint_32 y=0; y < PngHeight; y++) {
            auto source_row = interlaced ? image_data + (size_t(y) * row_size) : row;
            if (!interlaced) { png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit; }
            i = 0;
            for (png_uint_32 x=0; x < PngWidth; x++) {
               rgb.Red   = source_row[i];
               rgb.Green = source_row[i];
               rgb.Blue  = source_row[i++];
               rgb.Alpha = source_row[i++];

               Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);

               if (Self->Mask) Self->Mask->Data[(y * Self->Mask->LineWidth) + x] = rgb.Alpha;
            }
         }
         passes--;
      }
   }
   else if (ColourType & PNG_COLOR_MASK_ALPHA) {
      // When decompressing images that support an alpha channel, the fourth byte of each pixel will contain the alpha data.

      log.trace("32-bit + alpha image source.");

      while (passes > 0) {
         for (png_uint_32 y=0; y < PngHeight; y++) {
            auto source_row = interlaced ? image_data + (size_t(y) * row_size) : row;
            if (!interlaced) { png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit; }
            i = 0;
            for (png_uint_32 x=0; x < PngWidth; x++) {
               Bitmap->DrawUCRPixel(Bitmap, x, y, (RGB8 *)(source_row+i));

               // Set the alpha byte in the alpha mask (nb: refer to png_set_invert_alpha() if you want to reverse the alpha bytes)

               if (Self->Mask) Self->Mask->Data[(y * Self->Mask->LineWidth) + x] = source_row[i+3];

               i += 4;
            }
         }
         passes--;
      }
   }
   else {
      log.trace("24-bit image source.");

      while (passes > 0) {
         rgb.Alpha = 255;
         for (png_uint_32 y=0; y < PngHeight; y++) {
            auto source_row = interlaced ? image_data + (size_t(y) * row_size) : row;
            if (!interlaced) { png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit; }
            i = 0;
            for (png_uint_32 x=0; x < PngWidth; x++) {
               rgb.Red   = source_row[i++];
               rgb.Green = source_row[i++];
               rgb.Blue  = source_row[i++];
               Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
            }
         }
         passes--;
      }
   }

   error = ERR::Okay;

exit:
   FreeResource(row);
   FreeResource(image_data);
   FreeResource(scratch_row);
   return error;
}

//********************************************************************************************************************

#include "image_def.c"

static const FieldArray clFields[] = {
   { "Path",          FDF_CPPSTRING|FDF_RI },
   { "Src",           FDF_SYNONYM },
   { "Bitmap",        FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "Mask",          FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "Flags",         FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clImageFlags },
   { "DisplayHeight", FDF_INT|FDF_RW },
   { "DisplayWidth",  FDF_INT|FDF_RW },
   { "Quality",       FDF_INT|FDF_RW },
   { "FrameRate",     FDF_SYSTEM|FDF_INT|FDF_R },
   // Extended fields
   { "Author",        FDF_CPPSTRING|FDF_RW },
   { "Copyright",     FDF_CPPSTRING|FDF_RW },
   { "Title",         FDF_CPPSTRING|FDF_RW },
   { "Software",      FDF_CPPSTRING|FDF_RW },
   { "Description",   FDF_CPPSTRING|FDF_RW },
   { "Disclaimer",    FDF_CPPSTRING|FDF_RW },
   { "Header",        FDF_VIRTUAL|FDF_POINTER|FDF_RI|FDF_PURE, GET_Header },
   END_FIELD
};

static ERR create_image_class(void)
{
   clImage = objMetaClass::create::global(
      fl::ClassVersion(VER_IMAGE),
      fl::Name("Image"),
      fl::Category(CCF::GRAPHICS),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::FileExtension("png"),
      fl::FileDescription("PNG Image"),
      fl::FileHeader("[0:$89504e470d0a1a0a]"),
      fl::Icon("filetypes/image"),
      fl::Actions(clImageActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extImage)),
      fl::Path(MOD_PATH));

   return clImage ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_image_module() { return &ModHeader; }
