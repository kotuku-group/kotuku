/*********************************************************************************************************************

This source code and its accompanying files are in the public domain and therefore may be distributed without
restriction.  The source is based in part on libpng, authored by Glenn Randers-Pehrson, Andreas Eric Dilger and
Guy Eric Schalnat.

**********************************************************************************************************************

-CLASS-
Picture: Loads and saves picture files in a variety of different data formats.

The Picture class provides a standard API for programs to load picture files of any supported data type.  It is future
proof in that future data formats can be supported by installing class drivers on the user's system.

The default file format for loading and saving pictures is PNG.  Other formats such as JPEG are supported via
sub-classes, which can be loaded into the system at boot time or on demand.  Some rare formats such as TIFF are
also supported, but user preference may dictate whether or not the necessary driver is installed.

<header>Technical Notes</>

To find out general information about a picture before initialising it, #Query() it first so that the picture object
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
#include <kotuku/modules/picture.h>
#include <kotuku/modules/display.h>
#include <kotuku/strings.hpp>
#include "../link/linear_rgb.h"

#include "picture.h"

using namespace pf;

static OBJECTPTR clPicture = nullptr;
static OBJECTPTR modDisplay = nullptr;
static thread_local bool tlError = false;

JUMPTABLE_CORE
JUMPTABLE_DISPLAY

static ERR decompress_png(extPicture *, objBitmap *, int, int, png_structp, png_infop, png_uint_32, png_uint_32);
static void read_row_callback(png_structp, png_uint_32, int);
static void write_row_callback(png_structp, png_uint_32, int);
static void png_error_hook(png_structp png_ptr, png_const_charp message);
static void png_warning_hook(png_structp png_ptr, png_const_charp message);
void kotuku_read_callback(png_structp png, png_bytep data, png_size_t length);
void kotuku_write_callback(png_structp png, png_bytep data, png_size_t length);
void png_read_data(png_structp png, png_bytep data, png_size_t length);
void png_write_data(png_structp png, png_const_bytep data, png_size_t length);
void png_set_read_fn(png_structp png_ptr, png_voidp io_ptr, png_rw_ptr read_data_fn);
void png_set_write_fn(png_structp png_ptr, png_voidp io_ptr, png_rw_ptr write_data_fn, png_flush_ptr output_flush_fn);
static ERR create_picture_class(void);

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

   return(create_picture_class());
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (clPicture)  { FreeResource(clPicture); clPicture = nullptr; }
   if (modDisplay) { FreeResource(modDisplay); modDisplay = nullptr; }
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Activate: Loads image data into a picture object.

Loading an image file requires a call to Activate() after initialisation.  The #Path field will be used to source
the image file.

Pre-setting picture field values will place restrictions on the image file that is to be loaded.  For example, if
the source image is wider than a preset @Bitmap.Width, the image will have its right edge clipped.  The same is
true for the @Bitmap.Height and other restrictions apply to fields such as the @Bitmap.Palette.

Once the picture is loaded, the image data will be held in the picture's #Bitmap object.  Manipulating the #Bitmap
object is permitted.
-END-

*********************************************************************************************************************/

static ERR PICTURE_Activate(extPicture *Self)
{
   pf::Log log;

   if (Self->Bitmap->initialised()) return ERR::Okay;

   log.branch();

   ERR error = ERR::Failed;
   tlError = false;

   auto bmp = Self->Bitmap;
   png_structp read_ptr = nullptr;
   png_infop info_ptr = nullptr;
   png_infop end_info = nullptr;

   if (!Self->prvFile) {
      CSTRING path;
      if (Self->get(FID_Path, path) != ERR::Okay) return log.warning(ERR::GetField);

      if (!(Self->prvFile = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) goto exit;
   }

   Self->prvFile->seekStart(0);

   // Allocate PNG structures

   if (!(read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, Self, &png_error_hook, &png_warning_hook))) goto exit;
   if (!(info_ptr = png_create_info_struct(read_ptr))) goto exit;
   if (!(end_info = png_create_info_struct(read_ptr))) goto exit;

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
      // A picture can have an alpha channel that is separate to the RGB image data.
      if ((Self->Mask = objBitmap::create::local(
            fl::Width(Self->Bitmap->Width), fl::Height(Self->Bitmap->Height),
            fl::AmtColours(256), fl::Flags(BMF::MASK)))) {
         Self->Flags |= PCF::MASK|PCF::ALPHA;
      }
      else goto exit;
   }

   // If a background colour has been specified for the image (instead of an alpha channel), read it and create the
   // mask based on the data that we have read.

   if (info_ptr->valid & PNG_INFO_tRNS) {
      // The first colour index in the list is taken as the background, any others are ignored

      RGB8 rgb;
      if ((info_ptr->color_type IS PNG_COLOR_TYPE_PALETTE) or
          (info_ptr->color_type IS PNG_COLOR_TYPE_GRAY) or
          (info_ptr->color_type IS PNG_COLOR_TYPE_GRAY_ALPHA)) {
         bmp->TransIndex = info_ptr->trans_alpha[0];
         rgb = bmp->Palette->Col[bmp->TransIndex];
         rgb.Alpha = 255;
         bmp->set(FID_Transparence, &rgb);
      }
      else {
         rgb.Red   = info_ptr->trans_color.red;
         rgb.Green = info_ptr->trans_color.green;
         rgb.Blue  = info_ptr->trans_color.blue;
         rgb.Alpha = 255;
         bmp->set(FID_Transparence, &rgb);
      }
   }

   if (info_ptr->valid & PNG_INFO_bKGD) {
      png_color_16p prgb;
      prgb = &(info_ptr->background);
      if (color_type IS PNG_COLOR_TYPE_PALETTE) {
         bmp->Bkgd.Red   = bmp->Palette->Col[info_ptr->trans_alpha[0]].Red;
         bmp->Bkgd.Green = bmp->Palette->Col[info_ptr->trans_alpha[0]].Green;
         bmp->Bkgd.Blue  = bmp->Palette->Col[info_ptr->trans_alpha[0]].Blue;
         bmp->Bkgd.Alpha = 255;
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
         if ((error = decompress_png(Self, *tmp_bitmap, bit_depth, color_type, read_ptr, info_ptr, png_width, png_height)) IS ERR::Okay) {
            gfx::CopyArea(*tmp_bitmap, bmp, BAF::DITHER, 0, 0, bmp->Width, bmp->Height, 0, 0);
         }
      }
   }
   else error = decompress_png(Self, bmp, bit_depth, color_type, read_ptr, info_ptr, png_width, png_height);

   if (error IS ERR::Okay) {
      png_read_end(read_ptr, end_info);
      if (Self->prvFile) { FreeResource(Self->prvFile); Self->prvFile = nullptr; }
   }
   else {
exit:
      log.warning(error);
   }

   png_destroy_read_struct(&read_ptr, &info_ptr, &end_info);

   return error;
}

//********************************************************************************************************************

static ERR PICTURE_Free(extPicture *Self)
{
   if (Self->prvFile) { FreeResource(Self->prvFile); Self->prvFile = nullptr; }
   if (Self->Bitmap)  { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }
   if (Self->Mask)    { FreeResource(Self->Mask); Self->Mask = nullptr; }
   Self->~extPicture();
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Init: Prepares the object for use.

Objects that belong to the Picture class can be initialised in two possible ways.  If you have not set the
#Path field or have chosen to use the `NEW` flag, the initialisation routine will create a
#Bitmap area that contains no image data.  This allows you to fill the picture with your own image data and
save it using the #SaveImage() or #SaveToObject() actions.  You must set the @Bitmap.Width, @Bitmap.Height
and colour specifications at a minimum, or the initialisation process will fail.

If you have set the #Path field and avoided the `NEW` flag, the initialisation process will analyse the
file location to determine whether or not the data is in fact a valid image file.  If the file does not match up
with a registered data format, an error code of `ERR::NoSupport` is returned.  You will need to use the #Activate() or
#Query() actions to load or find out more information about the image format.
-END-

*********************************************************************************************************************/

static ERR PICTURE_Init(extPicture *Self)
{
   pf::Log log;

   if ((Self->prvPath.empty()) or ((Self->Flags & PCF::NEW) != PCF::NIL)) {
      // If no path has been specified, assume that the picture is being created from scratch (e.g. to save an
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
         if (InitObject(Self->Bitmap) IS ERR::Okay) {
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

            if (Self->isSubClass()) return ERR::Okay; // Break here to let the sub-class continue initialisation

            return ERR::Okay;
         }
         else return log.warning(ERR::Init);
      }
      else return log.warning(ERR::InvalidDimension);
   }
   else {
      if (Self->isSubClass()) return ERR::Okay; // Break here to let the sub-class continue initialisation

      // Test the given path to see if it matches our supported file format.

      if (ResolvePath(Self->prvPath, RSF::APPROXIMATE, &Self->prvPath) IS ERR::Okay) {
         int result;

         if (ReadFileToBuffer(Self->prvPath.c_str(), Self->prvHeader, sizeof(Self->prvHeader)-1, &result) IS ERR::Okay) {
            Self->prvHeader[result] = 0;

            auto buffer = (uint8_t *)Self->prvHeader;

            if ((buffer[0] IS 0x89) and (buffer[1] IS 0x50) and (buffer[2] IS 0x4e) and (buffer[3] IS 0x47) and
                (buffer[4] IS 0x0d) and (buffer[5] IS 0x0a) and (buffer[6] IS 0x1a) and (buffer[7] IS 0x0a)) {
               if ((Self->Flags & PCF::LAZY) != PCF::NIL) return ERR::Okay;
               return acActivate(Self);
            }
            else return ERR::NoSupport;
         }
         else {
            log.warning("Failed to read '%s'", Self->prvPath.c_str());
            return ERR::File;
         }
      }
      else {
         log.warning("Failed to find '%s'", Self->prvPath.c_str());
         return ERR::FileNotFound;
      }
   }

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR PICTURE_NewObject(extPicture *Self)
{
   Self->Quality = 80; // 80% quality rating when saving
   return NewLocalObject(CLASSID::BITMAP, &Self->Bitmap);
}

static ERR PICTURE_NewPlacement(extPicture *Self)
{
   new (Self) extPicture;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PICTURE_Query(extPicture *Self)
{
   pf::Log log;
   CSTRING path;
   png_uint_32 width, height;
   int bit_depth, color_type;

   if ((Self->Bitmap->Flags & BMF::QUERIED) != BMF::NIL) return ERR::Okay;
   if (!Self->prvFile) return ERR::NotInitialised;

   log.branch();

   objBitmap *Bitmap = Self->Bitmap;
   ERR error = ERR::Failed;
   png_structp read_ptr = nullptr;
   png_infop info_ptr = nullptr;
   png_infop end_info = nullptr;
   tlError = false;

   // Open the data file

   if (!Self->prvFile) {
      if (Self->get(FID_Path, path) != ERR::Okay) return log.warning(ERR::GetField);

      if (!(Self->prvFile = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) goto exit;
   }

   Self->prvFile->seekStart(0);

   // Allocate PNG structures

   if (!(read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, Self, &png_error_hook, &png_warning_hook))) goto exit;
   if (!(info_ptr = png_create_info_struct(read_ptr))) goto exit;
   if (!(end_info = png_create_info_struct(read_ptr))) goto exit;

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
Read: Reads raw image data from a Picture object.
-END-
*********************************************************************************************************************/

static ERR PICTURE_Read(extPicture *Self, struct acRead *Args)
{
   return Action(AC::Read, Self->Bitmap, Args);
}

/*********************************************************************************************************************
-ACTION-
Refresh: Refreshes a loaded picture - draws the next frame.
-END-
*********************************************************************************************************************/

static ERR PICTURE_Refresh(extPicture *Self)
{
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveImage: Saves the picture image to a data object.

If no destination is specified then the image will be saved as a new file targeting #Path.

-END-
*********************************************************************************************************************/

static ERR PICTURE_SaveImage(extPicture *Self, struct acSaveImage *Args)
{
   pf::Log log;
   CSTRING path;
   int y, i;
   png_bytep row_pointers;

   log.branch();

   objBitmap *bmp        = Self->Bitmap;
   OBJECTPTR file        = nullptr;
   png_structp write_ptr = nullptr;
   png_infop info_ptr    = nullptr;
   ERR error = ERR::Failed;
   tlError = false;

   if ((Args) and (Args->Dest)) file = Args->Dest;
   else {
      if (Self->get(FID_Path, path) != ERR::Okay) return log.warning(ERR::MissingPath);

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

   png_set_write_fn(write_ptr, file, kotuku_write_callback, nullptr);

   png_set_write_status_fn(write_ptr, write_row_callback);
   if (tlError) {
      log.warning("png_set_write_status_fn() failed.");
      goto exit;
   }

   if (((Self->Flags & (PCF::ALPHA|PCF::MASK)) != PCF::NIL) and (!Self->Mask)) {
      log.warning("Illegal use of the ALPHA/MASK flags without an accompanying mask bitmap.");
      Self->Flags &= ~(PCF::ALPHA|PCF::MASK);
   }

   if (bmp->AmtColours > 256) {
      if ((bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
         log.trace("Saving as 32-bit alpha.");
         png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      }
      else if ((Self->Flags & PCF::ALPHA) != PCF::NIL) {
         log.trace("Saving with alpha-mask.");
         png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      }
      else {
         log.trace("Saving in standard chunky graphics mode (no alpha).");
         png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      }
   }
   else {
      png_set_IHDR(write_ptr, info_ptr, bmp->Width, bmp->Height, 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

      std::vector<png_color> p(bmp->Palette->AmtColours);
      for (int i=0; i < bmp->Palette->AmtColours; i++) {
         p[i] = { bmp->Palette->Col[i].Red, bmp->Palette->Col[i].Green, bmp->Palette->Col[i].Blue };
      }

      png_set_PLTE(write_ptr, info_ptr, p.data(), bmp->AmtColours);
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

   if ((bmp->BitsPerPixel IS 8) or (bmp->BitsPerPixel IS 24)) {
      if ((Self->Flags & PCF::ALPHA) != PCF::NIL) {
         auto row = std::make_unique<uint8_t[]>(bmp->Width * 4);
         row_pointers = row.get();
         uint8_t *data = bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            int16_t maskx = 0;
            for (int x=0; x < bmp->ByteWidth; x+=3) {
               row[i++] = data[x+0];  // Blue
               row[i++] = data[x+1];  // Green
               row[i++] = data[x+2];  // Red
               row[i++] = mask[maskx++];  // Alpha
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row.get(), bmp->Width);
            png_write_row(write_ptr, row_pointers);
            data += bmp->LineWidth;
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         for (y=0; y < bmp->Height; y++) {
            row_pointers = bmp->Data + (y * bmp->LineWidth);
            png_write_row(write_ptr, row_pointers);
         }
      }
   }
   else if (bmp->BitsPerPixel IS 32) {
      if ((bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
         auto row = std::make_unique<uint8_t[]>(bmp->Width * 4);
         row_pointers = row.get();
         uint8_t *data = bmp->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < (bmp->Width<<2); x+=4) {
               row[i++] = data[x+0];  // Blue
               row[i++] = data[x+1];  // Green
               row[i++] = data[x+2];  // Red
               row[i++] = data[x+3];  // Alpha
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row.get(), bmp->Width);
            png_write_row(write_ptr, row_pointers);
            data += bmp->LineWidth;
         }
      }
      else if ((Self->Flags & PCF::ALPHA) != PCF::NIL) {
         auto row = std::make_unique<uint8_t[]>(bmp->Width * 4);

         row_pointers = row.get();
         uint8_t *data = bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            int16_t maskx = 0;
            for (int x=0; x < (bmp->Width<<2); x+=4) {
               row[i++] = data[x+0];     // Blue
               row[i++] = data[x+1];     // Green
               row[i++] = data[x+2];     // Red
               row[i++] = mask[maskx++]; // Alpha
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row.get(), bmp->Width);
            png_write_row(write_ptr, row_pointers);
            data += bmp->LineWidth;
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         auto row = std::make_unique<uint8_t[]>(bmp->Width * 3);
         row_pointers = row.get();
         uint8_t *data = bmp->Data;
         for (int y=0; y < bmp->Height; y++) {
            i = 0;
            for (int x=0; x < (bmp->Width<<2); x+=4) {
               row[i++] = data[x+0];  // Blue
               row[i++] = data[x+1];  // Green
               row[i++] = data[x+2];  // Red
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row24(row.get(), bmp->Width);
            png_write_row(write_ptr, row_pointers);
            data += bmp->LineWidth;
         }
      }
   }
   else if (bmp->BytesPerPixel IS 2) {
      if ((Self->Flags & PCF::ALPHA) != PCF::NIL) {
         auto row = std::make_unique<uint8_t[]>(bmp->Width * 4);
         row_pointers = row.get();
         uint16_t *data = (uint16_t *)bmp->Data;
         uint8_t *mask = Self->Mask->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            int16_t maskx = 0;
            for (int x=0; x < bmp->Width; x++) {
               row[i++] = bmp->unpackBlue(data[x]);
               row[i++] = bmp->unpackGreen(data[x]);
               row[i++] = bmp->unpackRed(data[x]);
               row[i++] = mask[maskx++];
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row32(row.get(), bmp->Width);
            png_write_row(write_ptr, row_pointers);
            data = (uint16_t *)(((uint8_t *)data) + bmp->LineWidth);
            mask += Self->Mask->LineWidth;
         }
      }
      else {
         auto row = std::make_unique<uint8_t[]>(bmp->Width * 3);
         row_pointers = row.get();
         uint16_t *data = (uint16_t *)bmp->Data;
         for (int y=0; y < bmp->Height; y++) {
            int i = 0;
            for (int x=0; x < bmp->Width; x++) {
               row[i++] = bmp->unpackBlue(data[x]);
               row[i++] = bmp->unpackGreen(data[x]);
               row[i++] = bmp->unpackRed(data[x]);
            }
            if (bmp->ColourSpace IS CS::LINEAR_RGB) conv_l2r_row24(row.get(), bmp->Width);
            png_write_row(write_ptr, row_pointers);
            data = (uint16_t *)(((uint8_t *)data) + bmp->LineWidth);
         }
      }
   }

   png_write_end(write_ptr, nullptr);

   error = ERR::Okay;

exit:
   png_destroy_write_struct(&write_ptr, &info_ptr);

   if ((Args) and (Args->Dest));
   else if (file) FreeResource(file);

   if (error != ERR::Okay) return log.warning(error);
   else return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves the picture image to a data object.
-END-
*********************************************************************************************************************/

static ERR PICTURE_SaveToObject(extPicture *Self, struct acSaveToObject *Args)
{
   pf::Log log;
   ERR (**routine)(OBJECTPTR, APTR);

   if ((Args->ClassID != CLASSID::NIL) and (Args->ClassID != CLASSID::PICTURE)) {
      auto mc = (objMetaClass *)FindClass(Args->ClassID);
      if ((mc->get(FID_ActionTable, routine) IS ERR::Okay) and (routine)) {
         if ((routine[int(AC::SaveToObject)]) and (routine[int(AC::SaveToObject)] != (APTR)PICTURE_SaveToObject)) {
            return routine[int(AC::SaveToObject)](Self, Args);
         }
         else if ((routine[int(AC::SaveImage)]) and (routine[int(AC::SaveImage)] != (APTR)PICTURE_SaveImage)) {
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
Seek: Seeks to a new read/write position within a Picture object.
-END-
*********************************************************************************************************************/

static ERR PICTURE_Seek(extPicture *Self, struct acSeek *Args)
{
   return Action(AC::Seek, Self->Bitmap, Args);
}

/*********************************************************************************************************************
-ACTION-
Write: Writes raw image data to a picture object.
-END-
*********************************************************************************************************************/

static ERR PICTURE_Write(extPicture *Self, struct acWrite *Args)
{
   return Action(AC::Write, Self->Bitmap, Args);
}

/*********************************************************************************************************************

-FIELD-
Author: The name of the person or company that created the image.

*********************************************************************************************************************/

static ERR GET_Author(extPicture *Self, STRING *Value)
{
   if (!Self->prvAuthor.empty()) {
      *Value = Self->prvAuthor.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Author(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvAuthor.assign(Value);
   else Self->prvAuthor.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Bitmap: Represents a picture's image data.

The details of a picture's graphical image and data are defined in its associated bitmap object.  It contains
information on the image dimensions and palette for example.  The @Bitmap.Palette can be preset if you want to
remap the  source image to a specific set of colour values.

Please refer to the @Bitmap class for more details on the structure of bitmap objects.

-FIELD-
Copyright: Copyright details of an image.

Copyright details related to an image may be specified here.  The copyright should be short and to the point, for
example `Copyright J. Bloggs (c) 1992.`

*********************************************************************************************************************/

static ERR GET_Copyright(extPicture *Self, STRING *Value)
{
   if (!Self->prvCopyright.empty()) {
      *Value = Self->prvCopyright.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Copyright(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvCopyright.assign(Value);
   else Self->prvCopyright.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Description: Long description for an image.

A long description for an image may be entered in this field.  There is no strict limit on the length of the
description.

*********************************************************************************************************************/

static ERR GET_Description(extPicture *Self, STRING *Value)
{
   if (!Self->prvDescription.empty()) {
      *Value = Self->prvDescription.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Description(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvDescription.assign(Value);
   else Self->prvDescription.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Disclaimer: The disclaimer associated with an image.

If it is necessary to associate a disclaimer with an image, the legal text may be entered in this field.

*********************************************************************************************************************/

static ERR GET_Disclaimer(extPicture *Self, STRING *Value)
{
   if (!Self->prvDisclaimer.empty()) {
      *Value = Self->prvDisclaimer.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Disclaimer(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvDisclaimer.assign(Value);
   else Self->prvDisclaimer.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DisplayHeight: The preferred height to use when displaying the image.

The #DisplayWidth and DisplayHeight fields define the preferred pixel dimensions to use for the display when viewing the
image in a 96DPI environment.  Both fields will be set automatically when the picture source is loaded.  If
the source does not specify a suitable value for these fields, they may be initialised to a value based on the
picture's @Bitmap.Width and @Bitmap.Height.

In the case of a scalable image source such as SVG, the #DisplayWidth and DisplayHeight can be pre-configured by the
client, and the loader will scale the source image to the preferred dimensions on load.

-FIELD-
DisplayWidth: The preferred width to use when displaying the image.

The DisplayWidth and #DisplayHeight fields define the preferred pixel dimensions to use for the display when viewing the
image in a 96DPI environment.  Both fields will be set automatically when the picture source is loaded.  If
the source does not specify a suitable value for these fields, they may be initialised to a value based on the
picture's @Bitmap.Width and @Bitmap.Height.

In the case of a scalable image source such as SVG, the DisplayWidth and #DisplayHeight can be pre-configured by the
client, and the loader will scale the source image to the preferred dimensions on load.

-FIELD-
Flags:  Optional initialisation flags.

-FIELD-
Header: Contains the first 32 bytes of data in a picture's file header.

The Header field is a pointer to a 32 byte buffer that contains the first 32 bytes of information read from a picture
file on initialisation.  This special field is considered to be helpful only to developers writing add on components
for the picture class.

The buffer that is referred to by the Header field is not populated until the Init action is called on the picture object.

*********************************************************************************************************************/

static ERR GET_Header(extPicture *Self, APTR *Value)
{
   *Value = Self->prvHeader;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Path: The location of source image data.

*********************************************************************************************************************/

static ERR GET_Path(extPicture *Self, STRING *Value)
{
   if (!Self->prvPath.empty()) {
      *Value = Self->prvPath.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Path(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvPath.assign(Value);
   else Self->prvPath.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Mask: Refers to a Bitmap that imposes a mask on the image.

If a source picture includes a mask, the Mask field will refer to a Bitmap object that contains the mask image once the
picture source has been loaded.  The mask will be expressed as either a 256 colour alpha bitmap, or a 1-bit mask with
8 pixels per byte.

If creating a picture from scratch that needs to support a mask, set the `MASK` flag prior to initialisation
and the picture class will allocate the mask bitmap automatically.

-FIELD-
Quality: Defines the quality level to use when saving the image.

The quality level to use when saving the image is defined here.  The value is expressed as a percentage between 0 and
100%, with 100% being of the highest quality.  If the picture format is loss-less, such as PNG, then the quality level
may be used to determine the compression factor.

In all cases, the impact of selecting a high level of quality will increase the time it takes to save the image.

-FIELD-
Software: The name of the application that was used to draw the image.

*********************************************************************************************************************/

static ERR GET_Software(extPicture *Self, STRING *Value)
{
   if (!Self->prvPath.empty()) {
      *Value = Self->prvSoftware.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Software(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvSoftware.assign(Value);
   else Self->prvSoftware.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Title: The title of the image.
-END-
*********************************************************************************************************************/

static ERR GET_Title(extPicture *Self, STRING *Value)
{
   if (!Self->prvTitle.empty()) {
      *Value = Self->prvTitle.data();
      return ERR::Okay;
   }
   else {
      *Value = NULL;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Title(extPicture *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->prvTitle.assign(Value);
   else Self->prvTitle.clear();
   return ERR::Okay;
}

//********************************************************************************************************************

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

void png_flush(png_structp png_ptr)
{

}

// These functions are expected by the embedded libpng library
void png_read_data(png_structp png, png_bytep data, png_size_t length)
{
   struct acRead read = { data, (int)length };
   if ((Action(AC::Read, (OBJECTPTR)png_get_io_ptr(png), &read) != ERR::Okay) or ((png_size_t)read.Result != length)) {
      png_error(png, "File read error");
   }
}

void png_write_data(png_structp png, png_const_bytep data, png_size_t length)
{
   struct acWrite write = { data, (int)length };
   if ((Action(AC::Write, (OBJECTPTR)png_get_io_ptr(png), &write) != ERR::Okay) or ((png_size_t)write.Result != length)) {
      png_error(png, "File write error");
   }
}

void png_set_read_fn(png_structp png_ptr, png_voidp io_ptr, png_rw_ptr read_data_fn)
{
   if (png_ptr IS nullptr) return;
   png_ptr->io_ptr = io_ptr;
   png_ptr->read_data_fn = read_data_fn;
   png_ptr->output_flush_fn = nullptr;
}

void png_set_write_fn(png_structp png_ptr, png_voidp io_ptr, png_rw_ptr write_data_fn, png_flush_ptr output_flush_fn)
{
   if (png_ptr IS nullptr) return;
   png_ptr->io_ptr = io_ptr;
   png_ptr->write_data_fn = write_data_fn;
   png_ptr->output_flush_fn = output_flush_fn;
}



//********************************************************************************************************************
// PNG Error Handling Functions

static void png_error_hook(png_structp png_ptr, png_const_charp message)
{
   pf::Log log;
   log.warning("%s", message);
   tlError = true;
}

static void png_warning_hook(png_structp png_ptr, png_const_charp message)
{
   pf::Log log;
   log.msg("libpng: %s", message); // PNG warnings aren't serious enough to warrant logging beyond the info level
}

ZEXTERN uLong ZEXPORT crc32   OF((uLong crc, const Bytef *buf, uInt len))
{
   return GenCRC32(crc, (APTR)buf, len);
}

//********************************************************************************************************************

static ERR decompress_png(extPicture *Self, objBitmap *Bitmap, int BitDepth, int ColourType, png_structp ReadPtr,
                            png_infop InfoPtr, png_uint_32 PngWidth, png_uint_32 PngHeight)
{
   ERR error;
   uint8_t *row;
   png_bytep row_pointers;
   RGB8 rgb;
   int i;
   pf::Log log(__FUNCTION__);

   // Read the image data into our Bitmap

   if (ColourType & PNG_COLOR_MASK_ALPHA) png_set_expand(ReadPtr); // Alpha channel
   if (BitDepth IS 16) png_set_strip_16(ReadPtr); // Reduce bit depth to 24bpp if the image is 48bpp
   if (BitDepth < 8) png_set_packing(ReadPtr);

   auto interlace_type = png_get_interlace_type(ReadPtr, InfoPtr);

   log.branch("Size: %dx%dx%d, Interlace: %d", (int)PngWidth, (int)PngHeight, BitDepth, interlace_type);

   auto row_size = png_get_rowbytes(ReadPtr, InfoPtr);
   if ((error = acQuery(Bitmap)) != ERR::Okay) return error;
   if (!Bitmap->initialised()) {
      if ((error = InitObject(Bitmap)) != ERR::Okay) return error;
   }
   if ((error = AllocMemory(row_size, MEM::DATA|MEM::NO_CLEAR, &row)) != ERR::Okay) return error;

   // Chop the image to the bitmap dimensions

   if (PngWidth > (png_uint_32)Bitmap->Width) PngWidth = Bitmap->Width;
   if (PngHeight > (png_uint_32)Bitmap->Height) PngHeight = Bitmap->Height;

   int passes = 1;
   if (interlace_type == PNG_INTERLACE_ADAM7) passes = png_set_interlace_handling(ReadPtr);

   row_pointers = row;
   if (ColourType IS PNG_COLOR_TYPE_GRAY) {
      log.trace("Greyscale image source.");
      rgb.Alpha = 255;
      for (png_uint_32 y=0; y < PngHeight; y++) {
         png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit;
         for (png_uint_32 x=0; x < PngWidth; x++) {
            rgb.Red   = row[x];
            rgb.Green = row[x];
            rgb.Blue  = row[x];
            Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
         }
      }
   }
   else if (ColourType IS PNG_COLOR_TYPE_PALETTE) {
      log.trace("Palette-based image source.");

      while (passes > 0) {
         if (Bitmap->BitsPerPixel IS 8) {
            for (png_uint_32 y=0; y < PngHeight; y++) {
               png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit;
               for (png_uint_32 x=0; x < PngWidth; x++) Bitmap->DrawUCPixel(Bitmap, x, y, row[x]);
            }
         }
         else {
            rgb.Alpha = 255;
            for (png_uint_32 y=0; y < PngHeight; y++) {
               png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit;
               for (png_uint_32 x=0; x < PngWidth; x++) {
                  Bitmap->DrawUCRPixel(Bitmap, x, y, &Bitmap->Palette->Col[row[x]]);
               }
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
            png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit;
            i = 0;
            for (png_uint_32 x=0; x < PngWidth; x++) {
               Bitmap->DrawUCRPixel(Bitmap, x, y, (RGB8 *)(row+i));

               // Set the alpha byte in the alpha mask (nb: refer to png_set_invert_alpha() if you want to reverse the alpha bytes)

               if (Self->Mask) Self->Mask->Data[(y * Self->Mask->LineWidth) + x] = row[3];

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
            png_read_row(ReadPtr, row_pointers, nullptr); if (tlError) goto exit;
            i = 0;
            for (png_uint_32 x=0; x < PngWidth; x++) {
               rgb.Red   = row[i++];
               rgb.Green = row[i++];
               rgb.Blue  = row[i++];
               Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
            }
         }
         passes--;
      }
   }

exit:
   FreeResource(row);
   return error;
}

//********************************************************************************************************************

#include "picture_def.c"

static const FieldArray clFields[] = {
   { "Bitmap",        FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "Mask",          FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "Flags",         FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clPictureFlags },
   { "DisplayHeight", FDF_INT|FDF_RW },
   { "DisplayWidth",  FDF_INT|FDF_RW },
   { "Quality",       FDF_INT|FDF_RW },
   { "FrameRate",     FDF_SYSTEM|FDF_INT|FDF_R },
   // Virtual fields
   { "Author",        FDF_STRING|FDF_RW,  GET_Author, SET_Author },
   { "Copyright",     FDF_STRING|FDF_RW,  GET_Copyright, SET_Copyright },
   { "Description",   FDF_STRING|FDF_RW,  GET_Description, SET_Description },
   { "Disclaimer",    FDF_STRING|FDF_RW,  GET_Disclaimer, SET_Disclaimer },
   { "Header",        FDF_POINTER|FDF_RI, GET_Header },
   { "Path",          FDF_STRING|FDF_RI,  GET_Path, SET_Path },
   { "Location",      FDF_SYNONYM|FDF_STRING|FDF_RI, GET_Path, SET_Path },
   { "Src",           FDF_SYNONYM|FDF_STRING|FDF_RI, GET_Path, SET_Path },
   { "Software",      FDF_STRING|FDF_RW,  GET_Software, SET_Software },
   { "Title",         FDF_STRING|FDF_RW,  GET_Title, SET_Title },
   END_FIELD
};

static ERR create_picture_class(void)
{
   clPicture = objMetaClass::create::global(
      fl::ClassVersion(VER_PICTURE),
      fl::Name("Picture"),
      fl::Category(CCF::GRAPHICS),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::FileExtension("*.png"),
      fl::FileDescription("PNG Picture"),
      fl::FileHeader("[0:$89504e470d0a1a0a]"),
      fl::Icon("filetypes/image"),
      fl::Actions(clPictureActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extPicture)),
      fl::Path(MOD_PATH));

   return clPicture ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_picture_module() { return &ModHeader; }

