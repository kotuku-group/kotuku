/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

This software is based in part on the work of the Independent JPEG Group.  Source code has been derived from the
libjpeg archive, a separate package copyright to Thomas G. Lane.  Libjpeg is publicly available on terms that are not
related to this Package.  The original libjpeg source code can be obtained from http://www.ijg.org.

*********************************************************************************************************************/

#include <array>

#include <kotuku/main.h>
#include <kotuku/modules/picture.h>
#include <kotuku/modules/display.h>

#include "../picture/picture.h"

extern "C" {
#include "jpeglib.h"
#include "jerror.h"
}

JUMPTABLE_CORE
JUMPTABLE_DISPLAY

static OBJECTPTR clJPEG = nullptr;
static OBJECTPTR modDisplay = nullptr;

static ERR JPEG_Activate(extPicture *);
static ERR JPEG_Init(extPicture *);
static ERR JPEG_Query(extPicture *);
static ERR JPEG_SaveImage(extPicture *, struct acSaveImage *);

static void decompress_jpeg(extPicture *, objBitmap *, struct jpeg_decompress_struct *);

// Custom data source manager for Kotuku objFile
typedef struct {
   struct jpeg_source_mgr pub;
   objFile *infile;
   JOCTET *buffer;
   boolean start_of_file;
} kotuku_source_mgr;

typedef kotuku_source_mgr *kotuku_src_ptr;

#define INPUT_BUF_SIZE 4096

// Initialize source
METHODDEF(void) init_kotuku_source(j_decompress_ptr cinfo) {
   kotuku_src_ptr src = (kotuku_src_ptr)cinfo->src;
   src->start_of_file = TRUE;
}

// Fill input buffer
METHODDEF(boolean) fill_kotuku_input_buffer(j_decompress_ptr cinfo) {
   kotuku_src_ptr src = (kotuku_src_ptr)cinfo->src;
   int nbytes;

   if (src->infile->read(src->buffer, INPUT_BUF_SIZE, &nbytes) != ERR::Okay) {
      if (src->start_of_file) ERREXIT(cinfo, JERR_INPUT_EMPTY);
      WARNMS(cinfo, JWRN_JPEG_EOF);
      src->buffer[0] = 0xFF;
      src->buffer[1] = JPEG_EOI;
      nbytes = 2;
   }

   src->pub.next_input_byte = src->buffer;
   src->pub.bytes_in_buffer = nbytes;
   src->start_of_file = FALSE;

   return TRUE;
}

// Skip input data
METHODDEF(void) skip_kotuku_input_data(j_decompress_ptr cinfo, long num_bytes) {
   kotuku_src_ptr src = (kotuku_src_ptr)cinfo->src;

   if (num_bytes > 0) {
      while (num_bytes > (long)src->pub.bytes_in_buffer) {
         num_bytes -= (long)src->pub.bytes_in_buffer;
         (void)fill_kotuku_input_buffer(cinfo);
      }
      src->pub.next_input_byte += (size_t)num_bytes;
      src->pub.bytes_in_buffer -= (size_t)num_bytes;
   }
}

// Terminate source
METHODDEF(void) term_kotuku_source(j_decompress_ptr cinfo) {
   // No work necessary here
}

// Set up data source for reading from Kotuku objFile
static void jpeg_kotuku_src(j_decompress_ptr cinfo, objFile *infile) {
   kotuku_src_ptr src;

   if (cinfo->src IS nullptr) {
      cinfo->src = (struct jpeg_source_mgr *)
         (*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
                                   sizeof(kotuku_source_mgr));
      src = (kotuku_src_ptr)cinfo->src;
      src->buffer = (JOCTET *)
         (*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
                                   INPUT_BUF_SIZE * sizeof(JOCTET));
   }

   src = (kotuku_src_ptr)cinfo->src;
   src->pub.init_source = init_kotuku_source;
   src->pub.fill_input_buffer = fill_kotuku_input_buffer;
   src->pub.skip_input_data = skip_kotuku_input_data;
   src->pub.resync_to_restart = jpeg_resync_to_restart;
   src->pub.term_source = term_kotuku_source;
   src->infile = infile;
   src->pub.bytes_in_buffer = 0;
   src->pub.next_input_byte = nullptr;
}

// Custom destination manager for saving
typedef struct {
   struct jpeg_destination_mgr pub;
   objFile *outfile;
   JOCTET *buffer;
} kotuku_destination_mgr;

typedef kotuku_destination_mgr *kotuku_dest_ptr;

#define OUTPUT_BUF_SIZE 4096

// Initialize destination
METHODDEF(void) init_kotuku_destination(j_compress_ptr cinfo) {
   kotuku_dest_ptr dest = (kotuku_dest_ptr)cinfo->dest;
   dest->buffer = (JOCTET *)
      (*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_IMAGE,
                                OUTPUT_BUF_SIZE * sizeof(JOCTET));
   dest->pub.next_output_byte = dest->buffer;
   dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;
}

// Empty output buffer
METHODDEF(boolean) empty_kotuku_output_buffer(j_compress_ptr cinfo) {
   kotuku_dest_ptr dest = (kotuku_dest_ptr)cinfo->dest;

   if (dest->outfile->write(dest->buffer, OUTPUT_BUF_SIZE) != ERR::Okay) {
      ERREXIT(cinfo, JERR_FILE_WRITE);
   }

   dest->pub.next_output_byte = dest->buffer;
   dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;

   return TRUE;
}

// Terminate destination
METHODDEF(void) term_kotuku_destination(j_compress_ptr cinfo) {
   kotuku_dest_ptr dest = (kotuku_dest_ptr)cinfo->dest;
   int datacount = OUTPUT_BUF_SIZE - dest->pub.free_in_buffer;

   if (datacount > 0) {
      if (dest->outfile->write(dest->buffer, datacount) != ERR::Okay) {
         ERREXIT(cinfo, JERR_FILE_WRITE);
      }
   }
}

// Set up destination for writing to Kotuku objFile
static void jpeg_kotuku_dest(j_compress_ptr cinfo, objFile *outfile) {
   kotuku_dest_ptr dest;

   if (cinfo->dest IS nullptr) {
      cinfo->dest = (struct jpeg_destination_mgr *)
         (*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
                                   sizeof(kotuku_destination_mgr));
   }

   dest = (kotuku_dest_ptr)cinfo->dest;
   dest->pub.init_destination = init_kotuku_destination;
   dest->pub.empty_output_buffer = empty_kotuku_output_buffer;
   dest->pub.term_destination = term_kotuku_destination;
   dest->outfile = outfile;
}

//********************************************************************************************************************

static ERR JPEG_Activate(extPicture *Self)
{
   pf::Log log;
   struct jpeg_decompress_struct cinfo;
   struct jpeg_error_mgr jerr;

   // Return if the picture object has already been activated

   if (Self->Bitmap->initialised()) return ERR::Okay;

   if (!Self->prvFile) {
      CSTRING path;
      if (Self->get(FID_Location, path) != ERR::Okay) return log.warning(ERR::GetField);

      if (!(Self->prvFile = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
         log.warning("Failed to open file \"%s\".", path);
         return ERR::File;
      }
   }

   // Read the JPEG file

   acSeek(Self->prvFile, 0.0, SEEK::START);

   auto bmp = Self->Bitmap;
   cinfo.err = jpeg_std_error((struct jpeg_error_mgr *)&jerr);
   jpeg_create_decompress(&cinfo);
   jpeg_kotuku_src(&cinfo, Self->prvFile);
   jpeg_read_header(&cinfo, TRUE);

   bmp->Width  = cinfo.image_width;
   bmp->Height = cinfo.image_height;
   if (!Self->DisplayWidth)   Self->DisplayWidth  = bmp->Width;
   if (!Self->DisplayHeight)  Self->DisplayHeight = bmp->Height;
   if (bmp->Type IS BMP::NIL) bmp->Type           = BMP::CHUNKY;
   if (!bmp->BitsPerPixel)    bmp->BitsPerPixel   = 32;

   if (((Self->Flags & PCF::NO_PALETTE) != PCF::NIL) and (bmp->BitsPerPixel <= 8)) {
      bmp->BitsPerPixel = 32;
   }

   if (acQuery(bmp) IS ERR::Okay) {
      if (InitObject(bmp) != ERR::Okay) {
         jpeg_destroy_decompress(&cinfo);
         return ERR::Init;
      }
   }
   else {
      jpeg_destroy_decompress(&cinfo);
      return ERR::Query;
   }

   if (bmp->BitsPerPixel >= 24) {
      decompress_jpeg(Self, bmp, &cinfo);
   }
   else {
      log.trace("Dest BPP of %d requires dithering.", bmp->BitsPerPixel);

      objBitmap::create tmp = { fl::Width(bmp->Width), fl::Height(bmp->Height), fl::BitsPerPixel(24) };
      if (tmp.ok()) {
         decompress_jpeg(Self, *tmp, &cinfo);
         gfx::CopyArea(*tmp, bmp, BAF::DITHER, 0, 0, bmp->Width, bmp->Height, 0, 0);
      }
   }

   FreeResource(Self->prvFile);
   Self->prvFile = nullptr;

   return ERR::Okay;
}

static void decompress_jpeg(extPicture *Self, objBitmap *Bitmap, struct jpeg_decompress_struct *Cinfo)
{
   pf::Log log;
   RGB8 rgb;

   jpeg_start_decompress(Cinfo);

   log.trace("Unpacking data to a %dbpp Bitmap...", Bitmap->BitsPerPixel);

   int row_stride = Cinfo->output_width * Cinfo->output_components;
   JSAMPARRAY buffer = (*Cinfo->mem->alloc_sarray)((j_common_ptr) Cinfo, JPOOL_IMAGE, row_stride, 1);
   for (JDIMENSION y=0; Cinfo->output_scanline < Cinfo->output_height; y++) {
      jpeg_read_scanlines(Cinfo, buffer, 1);
      JSAMPROW row = buffer[0];

      if (Cinfo->output_components IS 3) {
         for (JDIMENSION x=0; x < Cinfo->output_width; x++) {
            rgb.Red   = *row++;
            rgb.Green = *row++;
            rgb.Blue  = *row++;
            rgb.Alpha = 255;
            Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
         }
      }
      else if (Cinfo->out_color_space IS JCS_RGB) {
         for (JDIMENSION x=0; x < Cinfo->output_width; x++) {
            int16_t i = *row++;
            rgb.Red   = GETJSAMPLE(Cinfo->colormap[0][i]);
            rgb.Green = GETJSAMPLE(Cinfo->colormap[1][i]);
            rgb.Blue  = GETJSAMPLE(Cinfo->colormap[2][i]);
            rgb.Alpha = 255;
            Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
         }
      }
      else {
         // Greyscale
         for (JDIMENSION x=0; x < Cinfo->output_width; x++) {
            rgb.Red = rgb.Green = rgb.Blue = *row++;
            rgb.Alpha = 255;
            Bitmap->DrawUCRPixel(Bitmap, x, y, &rgb);
         }
      }
   }

   log.trace("Decompression complete.");
   jpeg_finish_decompress(Cinfo);
   jpeg_destroy_decompress(Cinfo);
}

//********************************************************************************************************************

static ERR JPEG_Init(extPicture *Self)
{
   pf::Log log;
   uint8_t *buffer;
   CSTRING path = nullptr;

   Self->get(FID_Location, path);

   if ((!path) or ((Self->Flags & PCF::NEW) != PCF::NIL)) {
      // If no location has been specified, assume that the picture is being created from scratch (e.g. to save an image to disk).  The
      // programmer is required to specify the dimensions and colours of the Bitmap so that we can initialise it.

      if (!Self->Bitmap->Width) Self->Bitmap->Width = Self->DisplayWidth;
      if (!Self->Bitmap->Height) Self->Bitmap->Height = Self->DisplayHeight;

      if ((Self->Bitmap->Width) and (Self->Bitmap->Height)) {
         if (InitObject(Self->Bitmap) IS ERR::Okay) {
            return ERR::Okay;
         }
         else return log.warning(ERR::Init);
      }
      else return log.warning(ERR::FieldNotSet);
   }
   else if (Self->get(FID_Header, buffer) IS ERR::Okay) {
      if ((buffer[0] IS 0xff) and (buffer[1] IS 0xd8) and (buffer[2] IS 0xff) and
          ((buffer[3] IS 0xe0) or (buffer[3] IS 0xe1) or (buffer[3] IS 0xfe))) {
         log.msg("The file is a JPEG picture.");
         if ((Self->Flags & PCF::LAZY) IS PCF::NIL) acActivate(Self);
         return ERR::Okay;
      }
      else log.msg("The \"%s\" file is not a JPEG picture.", path);
   }

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR JPEG_Query(extPicture *Self)
{
   pf::Log log;
   struct jpeg_decompress_struct *cinfo;
   struct jpeg_error_mgr jerr;

   log.branch();

   if (!Self->prvFile) {
      CSTRING path;
      if (Self->get(FID_Location, path) != ERR::Okay) return log.warning(ERR::GetField);

      if (!(Self->prvFile = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
         return log.warning(ERR::CreateObject);
      }
   }

   acSeek(Self->prvFile, 0.0, SEEK::START);
   if (AllocMemory(sizeof(struct jpeg_decompress_struct), MEM::DATA, &cinfo) IS ERR::Okay) {
      auto bmp = Self->Bitmap;
      cinfo->err = jpeg_std_error((struct jpeg_error_mgr *)&jerr);
      jpeg_create_decompress(cinfo);
      jpeg_kotuku_src(cinfo, Self->prvFile);
      jpeg_read_header(cinfo, FALSE);

      if (!bmp->Width)           bmp->Width          = cinfo->image_width;
      if (!bmp->Height)          bmp->Height         = cinfo->image_height;
      if (!Self->DisplayWidth)   Self->DisplayWidth  = bmp->Width;
      if (!Self->DisplayHeight)  Self->DisplayHeight = bmp->Height;
      if (bmp->Type IS BMP::NIL) bmp->Type           = BMP::CHUNKY;
      if (!bmp->BitsPerPixel) {
         bmp->BitsPerPixel = 24;
         bmp->BytesPerPixel = 3;
      }

      jpeg_destroy_decompress(cinfo);
      FreeResource(cinfo);
      return acQuery(bmp);
   }
   else return log.warning(ERR::Memory);
}

//********************************************************************************************************************

static ERR JPEG_SaveImage(extPicture *Self, struct acSaveImage *Args)
{
   pf::Log log;

   log.branch();

   OBJECTPTR file = nullptr;

   if ((Args) and (Args->Dest)) file = Args->Dest;
   else {
      CSTRING path;
      if (Self->get(FID_Location, path) != ERR::Okay) return log.warning(ERR::MissingPath);

      if (!(file = objFile::create::local(fl::Path(path), fl::Flags(FL::NEW|FL::WRITE)))) {
         return log.warning(ERR::CreateObject);
      }
   }

   // Allocate jpeg structures

   struct jpeg_compress_struct cinfo;
   struct jpeg_error_mgr jerr;
   cinfo.err = jpeg_std_error((struct jpeg_error_mgr *)&jerr);
   jpeg_create_compress(&cinfo);
   jpeg_kotuku_dest(&cinfo, (objFile *)file);

   cinfo.image_width      = Self->Bitmap->Width; 	// image width and height, in pixels
   cinfo.image_height     = Self->Bitmap->Height;
   cinfo.input_components = 3;	// # of color components per pixel
   cinfo.in_color_space   = JCS_RGB; // colorspace of input image
   jpeg_set_defaults(&cinfo);

   jpeg_set_quality(&cinfo, (Self->Quality * 255) / 100, TRUE); // Quality is between 1 and 255 (lower is better)

   jpeg_start_compress(&cinfo, TRUE);

   {
      auto buffer = std::make_unique<uint8_t[]>(3 * Self->Bitmap->Width);
      JSAMPROW row_pointer[1];
      RGB8 rgb;

      for (int y=0; y < Self->Bitmap->Height; y++) {
         row_pointer[0] = buffer.get();
         int16_t index = 0;
         for (int x=0; x < Self->Bitmap->Width; x++) {
            Self->Bitmap->ReadUCRPixel(Self->Bitmap, x, y, &rgb);
            buffer[index++] = rgb.Red;
            buffer[index++] = rgb.Green;
            buffer[index++] = rgb.Blue;
         }
         jpeg_write_scanlines(&cinfo, row_pointer, 1);
      }
   }

   jpeg_finish_compress(&cinfo);
   jpeg_destroy_compress(&cinfo);

   if (file) {
      if ((Args) and (Args->Dest));
      else FreeResource(file);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ActionArray clActions[] = {
   { AC::Activate,  JPEG_Activate },
   { AC::Init,      JPEG_Init },
   { AC::Query,     JPEG_Query },
   { AC::SaveImage, JPEG_SaveImage },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;

   objModule::create pic = { fl::Name("picture") }; // Load our dependency ahead of class registration

   clJPEG = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::PICTURE),
      fl::ClassID(CLASSID::JPEG),
      fl::Name("JPEG"),
      fl::Category(CCF::GRAPHICS),
      fl::FileExtension("*.jpeg|*.jpeg|*.jfif"),
      fl::FileDescription("JPEG Picture"),
      fl::FileHeader("[0:$ffd8ffe0]|[0:$ffd8ffe1]|[0:$ffd8fffe]"),
      fl::Actions(clActions),
      fl::Path(MOD_PATH));

   return clJPEG ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (modDisplay) { FreeResource(modDisplay); modDisplay = nullptr; }
   if (clJPEG)     { FreeResource(clJPEG);     clJPEG = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_jpeg_module() { return &ModHeader; }
