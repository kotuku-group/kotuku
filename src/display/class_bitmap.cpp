/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Bitmap: Represents a pixel buffer used for drawing, image transfer and display backing.

The Bitmap class describes a rectangular block of pixel data together with its dimensions, colour format, palette,
clipping region and drawing state.  Bitmaps are used directly by @Display and @Image objects and provide the
low-level pixel storage behind much of Kōtuku's 2D graphics pipeline.

To create a bitmap, set #Width and #Height before initialisation.  The pixel format can be selected explicitly with
#BitsPerPixel, #BytesPerPixel, #AmtColours and #Type, or left for #Query() and #Init() to derive from the current
display environment.  #MemType controls whether the bitmap uses regular CPU-accessible memory or a platform-specific
video or texture resource where supported.

Direct CPU access is reliable for regular data bitmaps.  Bitmaps backed by video or texture resources may require
#Lock() before reading or writing #Data, and #Unlock() after direct access is complete.  Code that uses the drawing
methods exposed by this class does not normally need to manage locking itself.

Bitmap methods are intentionally low-level and operate on immediate pixel data.  Use the Vector module when retained
scene graphs, paths, gradients, filters or higher-level drawing composition are required.  Use @Image when decoding
or encoding image formats is the main concern.

Raw image bytes can be read and written with #Read() and #Write().  #SaveImage() writes the clipped bitmap image as PCX
data to a destination object that supports writing.
-END-

*********************************************************************************************************************/

#include "defs.h"

static ERR calc_pixel_routines(extBitmap *);

//********************************************************************************************************************
// Pixel and pen based functions.

// Video Pixel Routines


// Memory Pixel Routines

static void MemDrawPixel32(objBitmap *, int, int, uint32_t);
static void MemDrawLSBPixel24(objBitmap *, int, int, uint32_t);
static void MemDrawMSBPixel24(objBitmap *, int, int, uint32_t);
static void MemDrawPixel16(objBitmap *, int, int, uint32_t);
static void MemDrawPixel8(objBitmap *, int, int, uint32_t);

static uint32_t MemReadPixel32(objBitmap *, int, int);
static uint32_t MemReadLSBPixel24(objBitmap *, int, int);
static uint32_t MemReadMSBPixel24(objBitmap *, int, int);
static uint32_t MemReadPixel16(objBitmap *, int, int);
static uint32_t MemReadPixel8(objBitmap *, int, int);

static void MemDrawRGBPixel32(objBitmap *, int, int, RGB8 *);
static void MemDrawLSBRGBPixel24(objBitmap *, int, int, RGB8 *);
static void MemDrawMSBRGBPixel24(objBitmap *, int, int, RGB8 *);
static void MemDrawRGBPixel16(objBitmap *, int, int, RGB8 *);
static void MemDrawRGBPixel8(objBitmap *, int, int, RGB8 *);

static void MemDrawRGBIndex32(objBitmap *, uint32_t *, RGB8 *);
static void MemDrawLSBRGBIndex24(objBitmap *, uint8_t *, RGB8 *);
static void MemDrawMSBRGBIndex24(objBitmap *, uint8_t *, RGB8 *);
static void MemDrawRGBIndex16(objBitmap *, uint16_t *, RGB8 *);
static void MemDrawRGBIndex8(objBitmap *, uint8_t *, RGB8 *);

static void MemReadRGBPixel32(objBitmap *, int, int, RGB8 *);
static void MemReadLSBRGBPixel24(objBitmap *, int, int, RGB8 *);
static void MemReadMSBRGBPixel24(objBitmap *, int, int, RGB8 *);
static void MemReadRGBPixel16(objBitmap *, int, int, RGB8 *);
static void MemReadRGBPixel8(objBitmap *, int, int, RGB8 *);

static void MemReadRGBIndex32(objBitmap *, uint32_t *, RGB8 *);
static void MemReadLSBRGBIndex24(objBitmap *, uint8_t *, RGB8 *);
static void MemReadMSBRGBIndex24(objBitmap *, uint8_t *, RGB8 *);
static void MemReadRGBIndex16(objBitmap *, uint16_t *, RGB8 *);
static void MemReadRGBIndex8(objBitmap *, uint8_t *, RGB8 *);

static void MemReadRGBPixelPlanar(objBitmap *, int, int, RGB8 *);
static void MemReadRGBIndexPlanar(objBitmap *, uint8_t *, RGB8 *);
static void MemDrawPixelPlanar(objBitmap *, int, int, uint32_t);
static uint32_t MemReadPixelPlanar(objBitmap *, int, int);

static void DrawRGBPixelPlanar(objBitmap *, int X, int Y, RGB8 *);

//********************************************************************************************************************

static ERR GET_Handle(extBitmap *, APTR *);
static ERR GET_Data(extBitmap *, std::span<uint8_t> &);

static ERR SET_Bkgd(extBitmap *, RGB8 *);
static ERR SET_BkgdIndex(extBitmap *, int);
static ERR SET_Trans(extBitmap *, RGB8 *);
static ERR SET_TransIndex(extBitmap *, int);
static ERR SET_Data(extBitmap *, std::span<const uint8_t> &);
static ERR SET_Handle(extBitmap *, APTR);
static ERR SET_Palette(extBitmap *, RGBPalette *);

static const FieldDef clMemType[] = {
   { "Data", int(BMT::DATA) }, { "Video", int(BMT::VIDEO) }, { "Texture", int(BMT::TEXTURE) },
   { nullptr, 0 }
};

//********************************************************************************************************************
// Surface locking routines.  These should only be called on occasions where you need to use the CPU to access graphics
// memory.  These functions are internal, if the user wants to lock a bitmap surface then the Lock() action must be
// called on the bitmap.
//
// Please note: Regarding SURFACE_READ, using this flag will cause the video content to be copied to the bitmap buffer.
// If you do not need this overhead because the bitmap content is going to be refreshed, then specify SURFACE_WRITE
// only.  You will still be able to read the bitmap content with the CPU, it just avoids the copy overhead.

ERR lock_surface(extBitmap *Bitmap, int16_t Access)
{
   // A driver reports NoSupport when it has no host drawable standing behind the bitmap.  CPU access to such a
   // bitmap only requires a data area, so the request falls through to the data check rather than failing.

   if (glDriver) {
      if (auto error = glDriver->lockBitmap(Bitmap, Access); error != ERR::NoSupport) return error;
   }

   if (not Bitmap->Data) return kt::Log(__FUNCTION__).warning(ERR::FieldNotSet);

   return ERR::Okay;
}

ERR unlock_surface(extBitmap *Bitmap)
{
   if (glDriver) glDriver->unlockBitmap(Bitmap);
   return ERR::Okay;
}

//********************************************************************************************************************


//********************************************************************************************************************
// Score = Abs(BB1 - BB2) + Abs(GG1 - GG2) + Abs(RR1 - RR2)
// The closer the score is to zero, the better the colour match.

static uint32_t RGBToValue(RGB8 *RGB, RGBPalette *Palette)
{
   int BestMatch  = 0x7fffffff; // Highest possible value
   uint32_t best = 0;
   int16_t mred   = RGB->Red;
   int16_t mgreen = RGB->Green;
   int16_t mblue  = RGB->Blue;

   int16_t i;
   for (i=Palette->AmtColours-1; i > 0; i--) {
      int Match = mred - Palette->Col[i].Red; // R1 - R2
      if (Match < 0) Match = -Match; // Abs(R1 - R2)

      int16_t g = mgreen - Palette->Col[i].Green;
      if (g < 0) Match -= g; else Match += g;

      int16_t b = mblue - Palette->Col[i].Blue;
      if (b < 0) Match -= b; else Match += b;

      if (Match < BestMatch) {
         if (not Match) return i;
         BestMatch  = Match;
         best = i;
      }
   }

   return best;
}

/*********************************************************************************************************************

-ACTION-
Clear: Clears the bitmap image to #BkgdIndex.

Clear fills the full bitmap with the current background colour.  The colour used by the operation is #BkgdIndex, which
is derived from #Bkgd when the background colour is set through the RGB field.

To clear a bitmap to a different colour without changing the background fields, call #DrawRectangle() with `BAF::FILL`.
For alpha-capable bitmaps, setting #BkgdIndex to zero is an efficient way to clear the image to transparent black.

-ERRORS-
Okay
LockFailed

*********************************************************************************************************************/

static ERR BITMAP_Clear(extBitmap *Self)
{

   // Clear any alignment padding first - some clients may expect the Data to be completely clear.

   if (Self->MemType IS BMT::DATA) {
      if (Self->LineWidth > Self->Width * Self->BytesPerPixel) {
         int offset = 0;
         for (int y=0; y < Self->Height; y++) {
            for (int x = Self->Width * Self->BytesPerPixel; x < Self->LineWidth; x++) Self->Data[offset + x] = 0;
            offset += Self->LineWidth;
         }
      }
   }

   auto opacity = Self->Opacity;
   Self->Opacity = 255;
   gfx::DrawRectangle(Self, 0, 0, Self->Width, Self->Height, Self->BkgdIndex, BAF::FILL);
   Self->Opacity = opacity;
   return ERR::Okay;
}

/*********************************************************************************************************************
-METHOD-
ConvertToLinear: Converts a bitmap's colour space to linear RGB.

ConvertToLinear() converts the bitmap's clipped region from sRGB to linear RGB.  If `BMF::ALPHA_CHANNEL` is set, pixels
with an alpha value of zero are left unchanged.

#ColourSpace is set to `CS::LINEAR_RGB` on completion.  The method returns `ERR::NothingDone` if the bitmap is already
marked as linear RGB.

This method currently requires a 32-bit bitmap.

-ERRORS-
Okay
NothingDone: The Bitmap's content is already in linear RGB format.
InvalidState: The Bitmap is not in the expected state.
InvalidDimension: The clipping region is invalid.

-TAGS-
mutates-object
-END-
*********************************************************************************************************************/

ERR BITMAP_ConvertToLinear(extBitmap *Self)
{
   kt::Log log;

   if (Self->ColourSpace IS CS::LINEAR_RGB) return log.warning(ERR::NothingDone);
   if (Self->BytesPerPixel != 4) return log.warning(ERR::InvalidState);

   const auto w = int(Self->Clip.Right - Self->Clip.Left);
   const auto h = int(Self->Clip.Bottom - Self->Clip.Top);

   if (Self->Clip.Left + w > Self->Width) return log.warning(ERR::InvalidDimension);
   if (Self->Clip.Top + h > Self->Height) return log.warning(ERR::InvalidDimension);

   if ((Self->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
      const uint8_t R = Self->ColourFormat->RedPos>>3;
      const uint8_t G = Self->ColourFormat->GreenPos>>3;
      const uint8_t B = Self->ColourFormat->BluePos>>3;
      const uint8_t A = Self->ColourFormat->AlphaPos>>3;

      uint8_t *data = Self->Data + (Self->LineWidth * Self->Clip.Top) + (Self->Clip.Left * Self->BytesPerPixel);
      for (int y=0; y < h; y++) {
         uint8_t *pixel = data;
         for (int x=0; x < w; x++) {
            if (pixel[A]) {
               pixel[R] = glLinearRGB.convert(pixel[R]);
               pixel[G] = glLinearRGB.convert(pixel[G]);
               pixel[B] = glLinearRGB.convert(pixel[B]);
            }
            pixel += Self->BytesPerPixel;
         }
         data += Self->LineWidth;
      }
   }
   else {
      const uint8_t R = Self->ColourFormat->RedPos>>3;
      const uint8_t G = Self->ColourFormat->GreenPos>>3;
      const uint8_t B = Self->ColourFormat->BluePos>>3;

      uint8_t *data = Self->Data + (Self->LineWidth * Self->Clip.Top) + (Self->Clip.Left * Self->BytesPerPixel);
      for (int y=0; y < h; y++) {
         uint8_t *pixel = data;
         for (int x=0; x < w; x++) {
            pixel[R] = glLinearRGB.convert(pixel[R]);
            pixel[G] = glLinearRGB.convert(pixel[G]);
            pixel[B] = glLinearRGB.convert(pixel[B]);
            pixel += Self->BytesPerPixel;
         }
         data += Self->LineWidth;
      }
   }

   Self->ColourSpace = CS::LINEAR_RGB;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ConvertToRGB: Converts a bitmap's colour space to standard RGB.

ConvertToRGB() converts the bitmap's clipped region from linear RGB to sRGB.  If `BMF::ALPHA_CHANNEL` is set, pixels
with an alpha value of zero are left unchanged.

#ColourSpace is set to `CS::SRGB` on completion.  The method returns `ERR::NothingDone` if the bitmap is already marked
as sRGB.

This method currently requires a 32-bit bitmap.

-ERRORS-
Okay
NothingDone: The bitmap's content is already in sRGB format.
InvalidState: The bitmap is not in the expected state.
InvalidDimension: The clipping region is invalid.

-TAGS-
mutates-object

*********************************************************************************************************************/

ERR BITMAP_ConvertToRGB(extBitmap *Self)
{
   kt::Log log(__FUNCTION__);

   if (Self->ColourSpace IS CS::SRGB) return log.warning(ERR::NothingDone);
   if (Self->BytesPerPixel != 4) return log.warning(ERR::InvalidState);

   const auto w = (int)(Self->Clip.Right - Self->Clip.Left);
   const auto h = (int)(Self->Clip.Bottom - Self->Clip.Top);

   if (Self->Clip.Left + w > Self->Width) return log.warning(ERR::InvalidDimension);
   if (Self->Clip.Top + h > Self->Height) return log.warning(ERR::InvalidDimension);

   if ((Self->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
      const uint8_t R = Self->ColourFormat->RedPos>>3;
      const uint8_t G = Self->ColourFormat->GreenPos>>3;
      const uint8_t B = Self->ColourFormat->BluePos>>3;
      const uint8_t A = Self->ColourFormat->AlphaPos>>3;

      uint8_t *data = Self->Data + (Self->LineWidth * Self->Clip.Top) + (Self->Clip.Left * Self->BytesPerPixel);
      for (int y=0; y < h; y++) {
         uint8_t *pixel = data;
         for (int x=0; x < w; x++) {
            if (pixel[A]) {
               pixel[R] = glLinearRGB.invert(pixel[R]);
               pixel[G] = glLinearRGB.invert(pixel[G]);
               pixel[B] = glLinearRGB.invert(pixel[B]);
            }
            pixel += Self->BytesPerPixel;
         }
         data += Self->LineWidth;
      }
   }
   else {
      const uint8_t R = Self->ColourFormat->RedPos>>3;
      const uint8_t G = Self->ColourFormat->GreenPos>>3;
      const uint8_t B = Self->ColourFormat->BluePos>>3;

      uint8_t *data = Self->Data + (Self->LineWidth * Self->Clip.Top) + (Self->Clip.Left * Self->BytesPerPixel);
      for (int y=0; y < h; y++) {
         uint8_t *pixel = data;
         for (int x=0; x < w; x++) {
            pixel[R] = glLinearRGB.invert(pixel[R]);
            pixel[G] = glLinearRGB.invert(pixel[G]);
            pixel[B] = glLinearRGB.invert(pixel[B]);
            pixel += Self->BytesPerPixel;
         }
         data += Self->LineWidth;
      }
   }

   Self->ColourSpace = CS::SRGB;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
CopyArea: Copies a rectangular area from one bitmap to another.

CopyArea() copies a rectangular region from this bitmap to `DestBitmap`.  The source rectangle starts at `X`, `Y` and
has the supplied `Width` and `Height`; the destination position is `XDest`, `YDest`.

The operation is implemented by ~Display.CopyArea() and supports the same !BAF options.

-INPUT-
obj(Bitmap) DestBitmap: The target bitmap.
int(BAF) Flags:  Optional flags.
int X: The horizontal position of the area to be copied.
int Y: The vertical position of the area to be copied.
int Width:  The width of the area.
int Height: The height of the area.
int XDest:  The horizontal position to copy the area to.
int YDest:  The vertical position to copy the area to.

-ERRORS-
Okay
NullArgs
Mismatch: The target bitmap is not a close enough match to the source bitmap in order to perform the operation.

-TAGS-
mutates-input

*********************************************************************************************************************/

static ERR BITMAP_CopyArea(objBitmap *Self, struct bmp::CopyArea *Args)
{
   if (Args) return gfx::CopyArea((extBitmap *)Self, (extBitmap *)Args->DestBitmap, Args->Flags, Args->X, Args->Y, Args->Width, Args->Height, Args->XDest, Args->YDest);
   else return ERR::NullArgs;
}

/*********************************************************************************************************************

-ACTION-
CopyData: Copies bitmap image data to other bitmaps with colour remapping enabled.

CopyData copies this bitmap into another initialised @Bitmap object.  Other destination classes are not supported.

The copy is clipped to the destination dimensions.  If the destination is wider or taller than the source, the exposed
area is cleared to the destination bitmap's background colour.

-ERRORS-
Okay
NullArgs
Args

*********************************************************************************************************************/

static ERR BITMAP_CopyData(extBitmap *Self, struct acCopyData *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);
   if ((Args->Dest->classID() != CLASSID::BITMAP)) return log.warning(ERR::Args);

   auto target = (extBitmap *)Args->Dest;

   int max_height = Self->Height > target->Height ? target->Height : Self->Height;

   if (Self->Width >= target->Width) { // Source is wider or equal to the target
      gfx::CopyArea(Self, target, BAF::NIL, 0, 0, target->Width, max_height, 0, 0);
   }
   else { // The target is wider than the source.  Cpoy the source first, then clear the exposed region on the right.
      gfx::CopyArea(Self, target, BAF::NIL, 0, 0, Self->Width, max_height, 0, 0);
      gfx::DrawRectangle(target, Self->Width, 0, target->Width - Self->Width, max_height, target->BkgdIndex, BAF::FILL);
   }

   // If the target height is greater, we will need to clear the pixels trailing at the bottom.

   if (Self->Height < target->Height) {
      gfx::DrawRectangle(target, 0, Self->Height, target->Width, target->Height - Self->Height, target->BkgdIndex, BAF::FILL);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Demultiply: Reverses the conversion process performed by Premultiply().

Demultiply() restores straight RGB channel values after #Premultiply() has converted them to premultiplied alpha.  The
method returns `ERR::NothingDone` if `BMF::PREMUL` is not set in #Flags.

This method operates only on 32-bit bitmaps that have an alpha channel, and it processes only the current clipping
region.

-ERRORS-
Okay
NothingDone: The content is already normalised.
InvalidState: The Bitmap is not in the expected state (32-bit with an alpha channel).
InvalidDimension: The clipping region is invalid.
AllocMemory

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR BITMAP_Demultiply(extBitmap *Self)
{
   kt::Log log;

   static std::mutex mutex;
   {
      const std::lock_guard<std::mutex> lock(mutex);
      if (not glDemultiply) {
         auto demultiply = std::unique_ptr<std::array<uint16_t, 256 * 256>>(
            new (std::nothrow) std::array<uint16_t, 256 * 256>());
         if (not demultiply) return ERR::AllocMemory;

         for (int a=1; a <= 255; a++) {
            for (int i=0; i <= 255; i++) {
               (*demultiply)[(a<<8) + i] = uint16_t((i * 0xff) / a);
            }
         }

         glDemultiply = std::move(demultiply);
      }
   }

   if ((Self->Flags & BMF::PREMUL) IS BMF::NIL) return log.warning(ERR::NothingDone);
   if (Self->BitsPerPixel != 32) return log.warning(ERR::InvalidState);
   if ((Self->Flags & BMF::ALPHA_CHANNEL) IS BMF::NIL) return log.warning(ERR::InvalidState);

   const auto w = int(Self->Clip.Right - Self->Clip.Left);
   const auto h = int(Self->Clip.Bottom - Self->Clip.Top);

   if (Self->Clip.Left + w > Self->Width) return log.warning(ERR::InvalidDimension);
   if (Self->Clip.Top + h > Self->Height) return log.warning(ERR::InvalidDimension);

   const uint8_t A = Self->ColourFormat->AlphaPos>>3;
   const uint8_t R = Self->ColourFormat->RedPos>>3;
   const uint8_t G = Self->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->ColourFormat->BluePos>>3;

   uint8_t *data = Self->Data + (Self->Clip.Left * Self->BytesPerPixel) + (Self->Clip.Top * Self->LineWidth);
   for (int y=0; y < h; y++) {
      uint8_t *pixel = data;
      for (int x=0; x < w; x++) {
         const uint8_t a = pixel[A];
         if (a < 0xff) {
            if (a == 0) pixel[R] = pixel[G] = pixel[B] = 0;
            else {
               uint32_t r = (*glDemultiply)[(a<<8) + pixel[R]]; //(uint32_t(pixel[R]) * 0xff) / a;
               uint32_t g = (*glDemultiply)[(a<<8) + pixel[G]]; //(uint32_t(pixel[G]) * 0xff) / a;
               uint32_t b = (*glDemultiply)[(a<<8) + pixel[B]]; //(uint32_t(pixel[B]) * 0xff) / a;
               pixel[R] = uint8_t((r > 0xff) ? 0xff : r);
               pixel[G] = uint8_t((g > 0xff) ? 0xff : g);
               pixel[B] = uint8_t((b > 0xff) ? 0xff : b);
            }
         }
         pixel += 4;
      }
      data += Self->LineWidth;
   }

   Self->Flags &= ~BMF::PREMUL;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Draw: Clears the bitmap image to #BkgdIndex.

Draw fills the full bitmap with the current background colour.  It is equivalent to drawing a filled rectangle over the
entire bitmap with #BkgdIndex.

*********************************************************************************************************************/

static ERR BITMAP_Draw(extBitmap *Self)
{
   gfx::DrawRectangle(Self, 0, 0, Self->Width, Self->Height, Self->BkgdIndex, BAF::FILL);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DrawRectangle: Draws rectangles, both filled and unfilled.

This method draws both filled and unfilled rectangles.  The rectangle is drawn to the target bitmap at position
`(X, Y)` with dimensions determined by the specified `Width` and `Height`.  If the `Flags` parameter sets the `FILL`
flag then the rectangle will be filled, otherwise the rectangle's outline will be drawn.  The colour of the rectangle
is determined by the pixel value in the `Colour` parameter.

The draw operation is clipped to the bitmap's current clipping region.

-INPUT-
int X: The left-most coordinate of the rectangle.
int Y: The top-most coordinate of the rectangle.
int Width:  The width of the rectangle.
int Height: The height of the rectangle.
uint Colour: The colour index to use for the rectangle.
int(BAF) Flags:  Supports `FILL` and `BLEND`.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR BITMAP_DrawRectangle(extBitmap *Self, struct bmp::DrawRectangle *Args)
{
   if (not Args) return ERR::NullArgs;
   gfx::DrawRectangle(Self, Args->X, Args->Y, Args->Width, Args->Height, Args->Colour, Args->Flags);
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Flush: Flushes pending graphics operations and returns when the accelerator is idle.

Flush synchronises pending graphics operations with the active graphics backend.  Synchronisation is required before
direct CPU access to accelerator-managed bitmap memory.

Clients do not need to call this function if solely using the graphics methods provided in the @Bitmap class.
-END-

*********************************************************************************************************************/

static ERR BITMAP_Flush(extBitmap *Self)
{
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetColour: Converts Red, Green, Blue components into a single colour value.

The GetColour() method is used to convert `Red`, `Green`, `Blue` and `Alpha` colour components into a single colour
index that can be used for directly writing colours to the bitmap.  The result is returned in the `Colour` parameter.

-INPUT-
int Red:    Red component from 0 - 255.
int Green:  Green component from 0 - 255.
int Blue:   Blue component value from 0 - 255.
int Alpha:  Alpha component value from 0 - 255.
&uint Colour: The resulting colour value will be returned here.

-ERRORS-
Okay
NullArgs

-TAGS-
pure-query

*********************************************************************************************************************/

static ERR BITMAP_GetColour(extBitmap *Self, struct bmp::GetColour *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Self->BitsPerPixel > 8) {
      Args->Colour = Self->packPixel(Args->Red, Args->Green, Args->Blue, Args->Alpha);
   }
   else {
      struct RGB8 rgb;
      rgb.Red   = Args->Red;
      rgb.Green = Args->Green;
      rgb.Blue  = Args->Blue;
      rgb.Alpha = Args->Alpha;
      Args->Colour = RGBToValue(&rgb, Self->Palette);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Init: Initialises a bitmap.

Init prepares a queried bitmap for use.  It validates the calculated bitmap state, allocates #Data when required,
configures platform-specific backing resources and selects the pixel access routines used by drawing operations.

If #Data has already been supplied, Init uses the caller-provided memory.  Otherwise allocation is controlled by
#MemType and #Flags.  #Width and #Height must be set before this action is called.

-ERRORS-
Okay
Query
FieldNotSet
AllocMemory
SystemCall
NoSupport

*********************************************************************************************************************/

static ERR BITMAP_Init(extBitmap *Self)
{
   kt::Log log;

   if (acQuery(Self) != ERR::Okay) return log.warning(ERR::Query);

   log.branch("Size: %dx%d @ %d bit, %d bytes, Flags: $%.8x", Self->Width, Self->Height, Self->BitsPerPixel, Self->BytesPerPixel, int(Self->Flags));

   if (Self->Clip.Left < 0) Self->Clip.Left = 0;
   if (Self->Clip.Top < 0)  Self->Clip.Top  = 0;
   if ((Self->Clip.Right > Self->Width)  or (Self->Clip.Right < 1)) Self->Clip.Right = Self->Width;
   if ((Self->Clip.Bottom > Self->Height) or (Self->Clip.Bottom < 1)) Self->Clip.Bottom = Self->Height;

   // If the Bitmap is 15 or 16 bit, make corrections to the background values

   if (Self->BitsPerPixel IS 16) {
      Self->TransColour.Red   &= 0xf8;
      Self->TransColour.Green &= 0xfc;
      Self->TransColour.Blue  &= 0xf8;

      Self->Bkgd.Red   &= 0xf8;
      Self->Bkgd.Green &= 0xfc;
      Self->Bkgd.Blue  &= 0xf8;
   }
   else if (Self->BitsPerPixel IS 15) {
      Self->TransColour.Red   &= 0xf8;
      Self->TransColour.Green &= 0xf8;
      Self->TransColour.Blue  &= 0xf8;

      Self->Bkgd.Red   &= 0xf8;
      Self->Bkgd.Green &= 0xf8;
      Self->Bkgd.Blue  &= 0xf8;
   }

   if ((glDriver) and (not glHeadless)) {
      if (auto error = glDriver->allocBitmap(Self); (error != ERR::Okay) and (error != ERR::NoSupport)) {
         return log.warning(error);
      }

      if ((Self->MemType IS BMT::DATA) and (not Self->Data) and ((Self->Flags & BMF::NO_DATA) IS BMF::NIL)) {
         if (not Self->Size) return log.warning(ERR::FieldNotSet);
         Self->Data = (uint8_t *)malloc(Self->Size);
         if (not Self->Data) return log.warning(ERR::AllocMemory);
         Self->prvAFlags |= BF_DATA;
      }
   }
   else if (glHeadless) {
      Self->MemType = BMT::DATA;
      if ((not Self->Data) and ((Self->Flags & BMF::NO_DATA) IS BMF::NIL)) {
         if (not Self->Size) return log.warning(ERR::FieldNotSet);
         Self->Data = (uint8_t *)malloc(Self->Size);
         if (not Self->Data) return log.warning(ERR::AllocMemory);
         Self->prvAFlags |= BF_DATA;
      }
   }
   else {
   Self->MemType = BMT::DATA;

   if (not Self->Data) {
      if ((Self->Flags & BMF::NO_DATA) IS BMF::NIL) {
         if (not Self->Size) return log.warning(ERR::FieldNotSet);
         Self->Data = (uint8_t *)malloc(Self->Size);
         if (not Self->Data) return log.warning(ERR::AllocMemory);
         Self->prvAFlags |= BF_DATA;
      }
   }
   }

   // Determine the correct pixel format for the bitmap

   if ((glDriver) and (Self->MemType IS BMT::VIDEO)) {
      if (glDriver->pixelFormat(*Self->ColourFormat) != ERR::Okay) {
         gfx::GetColourFormat(Self->ColourFormat, Self->BitsPerPixel, 0, 0, 0, 0);
      }
   }
   else gfx::GetColourFormat(Self->ColourFormat, Self->BitsPerPixel, 0, 0, 0, 0);


   if (auto error = calc_pixel_routines(Self); error != ERR::Okay) return error;

   if (Self->BitsPerPixel > 8) {
      Self->TransIndex = (((Self->TransColour.Red   >> Self->prvColourFormat.RedShift)   & Self->prvColourFormat.RedMask)   << Self->prvColourFormat.RedPos) |
                         (((Self->TransColour.Green >> Self->prvColourFormat.GreenShift) & Self->prvColourFormat.GreenMask) << Self->prvColourFormat.GreenPos) |
                         (((Self->TransColour.Blue  >> Self->prvColourFormat.BlueShift)  & Self->prvColourFormat.BlueMask)  << Self->prvColourFormat.BluePos) |
                         (((255 >> Self->prvColourFormat.AlphaShift) & Self->prvColourFormat.AlphaMask) << Self->prvColourFormat.AlphaPos);

      Self->BkgdIndex = (((Self->Bkgd.Red   >> Self->prvColourFormat.RedShift)   & Self->prvColourFormat.RedMask)   << Self->prvColourFormat.RedPos) |
                        (((Self->Bkgd.Green >> Self->prvColourFormat.GreenShift) & Self->prvColourFormat.GreenMask) << Self->prvColourFormat.GreenPos) |
                        (((Self->Bkgd.Blue  >> Self->prvColourFormat.BlueShift)  & Self->prvColourFormat.BlueMask)  << Self->prvColourFormat.BluePos) |
                        (((255 >> Self->prvColourFormat.AlphaShift) & Self->prvColourFormat.AlphaMask) << Self->prvColourFormat.AlphaPos);
   }

   if (((Self->Flags & BMF::NO_DATA) IS BMF::NIL) and ((Self->Flags & BMF::CLEAR) != BMF::NIL)) {
      acClear(Self);
   }

   // Sanitise the Flags field

   if (Self->BitsPerPixel < 32) Self->Flags &= ~BMF::ALPHA_CHANNEL;

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Lock: Locks the bitmap surface for direct read/write access.

Lock makes bitmap memory available through #Data for direct CPU access.  It is mainly required for bitmaps backed by a
video or platform drawable resource; data-backed bitmaps are already CPU-accessible.

Call #Unlock() when direct access is complete so platform resources can be released or synchronised.

-ERRORS-
Okay
AllocMemory
CreateResource
FieldNotSet
LockFailed
NoData
SystemCall
NoSupport
*********************************************************************************************************************/

static ERR BITMAP_Lock(extBitmap *Self)
{

   return lock_surface(Self, SURFACE_READWRITE);

}

/*********************************************************************************************************************

-METHOD-
Premultiply: Premultiplies RGB channel values by the alpha channel.

Premultiply() converts RGB values in the current clipping region to premultiplied-alpha form.  The formula applied to
each colour channel is `(Colour * Alpha + 0xff)>>8`.  The alpha channel is not changed.

This method operates only on 32-bit bitmaps that have an alpha channel.  If the bitmap is already marked as
premultiplied, the method returns `ERR::NothingDone`.

The process can be reversed with a call to #Demultiply().

-ERRORS-
Okay
NothingDone: The content is already premultiplied.
InvalidState: The Bitmap is not in the expected state (32-bit with an alpha channel)
InvalidDimension: The clipping region is invalid.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR BITMAP_Premultiply(extBitmap *Self)
{
   kt::Log log;

   if ((Self->Flags & BMF::PREMUL) != BMF::NIL) {
      return log.warning(ERR::NothingDone);
   }

   if (Self->BitsPerPixel != 32) return log.warning(ERR::InvalidState);
   if ((Self->Flags & BMF::ALPHA_CHANNEL) IS BMF::NIL) return log.warning(ERR::InvalidState);

   const auto w = (int)(Self->Clip.Right - Self->Clip.Left);
   const auto h = (int)(Self->Clip.Bottom - Self->Clip.Top);

   if (Self->Clip.Left + w > Self->Width) return log.warning(ERR::InvalidDimension);
   if (Self->Clip.Top + h > Self->Height) return log.warning(ERR::InvalidDimension);

   const uint8_t A = Self->ColourFormat->AlphaPos>>3;
   const uint8_t R = Self->ColourFormat->RedPos>>3;
   const uint8_t G = Self->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->ColourFormat->BluePos>>3;

   uint8_t *data = Self->Data + (Self->Clip.Left * Self->BytesPerPixel) + (Self->Clip.Top * Self->LineWidth);
   for (int y=0; y < h; y++) {
      uint8_t *pixel = data;
      for (int x=0; x < w; x++) {
         const uint8_t a = pixel[A];
         if (a < 0xff) {
             if (a == 0) pixel[R] = pixel[G] = pixel[B] = 0;
             else {
                pixel[R] = uint8_t((pixel[R] * a + 0xff) >> 8);
                pixel[G] = uint8_t((pixel[G] * a + 0xff) >> 8);
                pixel[B] = uint8_t((pixel[B] * a + 0xff) >> 8);
             }
         }
         pixel += 4;
      }
      data += Self->LineWidth;
   }

   Self->Flags |= BMF::PREMUL;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Query: Populates a bitmap with pre-initialised/default values prior to initialisation.

Query calculates the bitmap's derived fields without allocating image memory.  It resolves values such as #Type,
#BytesPerPixel, #BitsPerPixel, #AmtColours, #ByteWidth, #LineWidth, #PlaneMod and #Size from the fields already set by
the caller.

At minimum, #Width and #Height must be positive.  If format fields are incomplete, Query derives a compatible format
where possible; for example, #BytesPerPixel set to `2` implies a 16-bit bitmap.

-ERRORS-
Okay
InvalidDimension

*********************************************************************************************************************/

static ERR BITMAP_Query(extBitmap *Self)
{
   kt::Log log;
   OBJECTID display_id;
   int i;

   log.msg(VLF::BRANCH|VLF::DETAIL, "Bitmap: %p, Depth: %d, Width: %d, Height: %d", Self, Self->BitsPerPixel, Self->Width, Self->Height);

   if ((Self->Width <= 0) or (Self->Height <= 0)) {
      return log.warning(ERR::InvalidDimension);
   }


   // If the BMF::MASK flag is set then the programmer wants to use the Bitmap object as a 1 or 8-bit mask.

   if ((Self->Flags & BMF::MASK) != BMF::NIL) {
      if ((not Self->BitsPerPixel) and (not Self->AmtColours)) {
         Self->BitsPerPixel = 1;
         Self->AmtColours = 2;
         Self->Type = BMP::PLANAR;
      }
      else if (Self->AmtColours >= 256) {
         Self->AmtColours = 256;
         Self->Type = BMP::CHUNKY;
         // Change the palette to grey scale for alpha channel masks
         for (i=0; i < 256; i++) {
            Self->Palette->Col[i].Red   = i;
            Self->Palette->Col[i].Green = i;
            Self->Palette->Col[i].Blue  = i;
         }
      }
      Self->BytesPerPixel = 1;
   }

   // If no type has been set, use the type that is native to the system that Kōtuku is running on.

   if (Self->Type IS BMP::NIL) Self->Type = BMP::CHUNKY;

   if (Self->BitsPerPixel) {
      switch(Self->BitsPerPixel) {
         case 1:  Self->BytesPerPixel = 1; Self->AmtColours = 2; Self->Type = BMP::PLANAR; break;
         case 2:  Self->BytesPerPixel = 1; Self->AmtColours = 4; break;
         case 8:  Self->BytesPerPixel = 1; Self->AmtColours = 256; break;
         case 15: Self->BytesPerPixel = 2; Self->AmtColours = 32768; break;
         case 16: Self->BytesPerPixel = 2; Self->AmtColours = 65536; break;
         case 24: Self->BytesPerPixel = 3; Self->AmtColours = 16777216; break;
         case 32: Self->BytesPerPixel = 4; Self->AmtColours = 16777216; break;
      }
   }
   else if (Self->BytesPerPixel) {
      switch(Self->BytesPerPixel) {
         case 1:  Self->BitsPerPixel  = 8;  Self->AmtColours = 256; break;
         case 2:  Self->BitsPerPixel  = 16; Self->AmtColours = 65536; break;
         case 3:  Self->BitsPerPixel  = 24; Self->AmtColours = 16777216; break;
         case 4:  Self->BitsPerPixel  = 32; Self->AmtColours = 16777216; break;
         default: Self->BytesPerPixel = 1;  Self->BitsPerPixel = 8; Self->AmtColours = 256;
      }
   }

   // Ensure values for BitsPerPixel, AmtColours, BytesPerPixel are correct

   if (not Self->AmtColours) {
      if (Self->BitsPerPixel) {
         if (Self->BitsPerPixel <= 24) {
            Self->AmtColours = 1<<Self->BitsPerPixel;
            if (Self->AmtColours <= 256) Self->BytesPerPixel = 1;
            else if (Self->AmtColours <= 65536) Self->BytesPerPixel = 2;
            else Self->BytesPerPixel = 3;
         }
         else {
            Self->AmtColours = 16777216;
            Self->BytesPerPixel = 4;
         }
      }
      else {
         Self->AmtColours    = 16777216;
         Self->BitsPerPixel  = 32;
         Self->BytesPerPixel = 4;
#if 1
         if (!FindObject("SystemDisplay", CLASSID::DISPLAY, &display_id)) {
            if (ScopedObjectLock<objDisplay> display(display_id, 3000); display.granted()) {
               Self->AmtColours    = display->Bitmap->AmtColours;
               Self->BytesPerPixel = display->Bitmap->BytesPerPixel;
               Self->BitsPerPixel  = display->Bitmap->BitsPerPixel;
            }
         }
#else
         DisplayInfo info;
         if (!get_display_info(0, &info)) {
            Self->AmtColours    = info.AmtColours;
            Self->BytesPerPixel = info.BytesPerPixel;
            Self->BitsPerPixel  = info.BitsPerPixel;
         }
#endif
      }
   }

   // Calculate ByteWidth, make sure it's word aligned

   if (Self->Type IS BMP::PLANAR) {
      Self->ByteWidth = (Self->Width + 7) / 8;
   }
   else Self->ByteWidth = Self->Width * Self->BytesPerPixel;

   // Initialise the line and plane module fields

   Self->LineWidth = Self->ByteWidth;
   Self->LineWidth = ALIGN32(Self->LineWidth);
   Self->PlaneMod = Self->LineWidth * Self->Height;



   // Calculate the total size of the bitmap

   if (Self->Type IS BMP::PLANAR) {
      Self->Size = Self->LineWidth * Self->Height * Self->BitsPerPixel;
   }
   else Self->Size = Self->LineWidth * Self->Height;

   Self->Flags |= BMF::QUERIED;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Read: Reads raw image data from a bitmap object.

Read copies bytes from #Data into the supplied output buffer, starting at #Position.  #Position is advanced by the
number of bytes copied and the result count is returned in the action arguments.

If the requested length would pass the end of the bitmap data, Read truncates the transfer to the remaining byte count.

-ERRORS-
Okay
NoData
NullArgs
OutOfRange
*********************************************************************************************************************/

static ERR BITMAP_Read(extBitmap *Self, struct acRead *Args)
{
   if (not Self->Data) return ERR::NoData;
   if ((not Args) or (not Args->Buffer.data())) return ERR::NullArgs;
   if (Args->Buffer.size() > size_t(INT_MAX)) return ERR::OutOfRange;

   int len = int(Args->Buffer.size());
   if (Self->Position + len > Self->Size) len = Self->Size - Self->Position;
   copymem(Self->Data + Self->Position, Args->Buffer.data(), len);
   Self->Position += len;
   Args->Result = len;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Resize: Resizes a bitmap object's dimensions.

Resize changes #Width, #Height and, unless `BMF::FIXED_DEPTH` is set, #BitsPerPixel.  Existing image content is not
preserved.

If `BMF::NEVER_SHRINK` is set, requested dimensions smaller than the current bitmap are raised to the current size.  If
`BMF::CLEAR` is set, the resized bitmap is cleared to #Bkgd.

-ERRORS-
Okay
NullArgs
Args
AllocMemory
NoSupport
UndefinedField
Notified


*********************************************************************************************************************/

static ERR BITMAP_Resize(extBitmap *Self, struct acResize *Args)
{
   kt::Log log;
   int width, height, bytewidth, bpp, amtcolours, size;

   if (not Args) return log.warning(ERR::NullArgs);

   auto origbpp = Self->BitsPerPixel;

   if (Args->Width > 0) width = (int)Args->Width;
   else width = Self->Width;

   if (Args->Height > 0) height = (int)Args->Height;
   else height = Self->Height;

   if ((Args->Depth > 0) and ((Self->Flags & BMF::FIXED_DEPTH) IS BMF::NIL)) bpp = (int)Args->Depth;
   else bpp = Self->BitsPerPixel;

   // If the NEVER_SHRINK option is set, the width and height may not be set to anything less than what is current.

   if ((Self->Flags & BMF::NEVER_SHRINK) != BMF::NIL) {
      if (width < Self->Width) width = Self->Width;
      if (height < Self->Height) height = Self->Height;
   }

   // Return if there is no change in the bitmap size

   if ((Self->Width IS width) and (Self->Height IS height) and (Self->BitsPerPixel IS bpp)) {
      return ERR::Okay|ERR::Notified;
   }

   // Calculate type-dependent values

   int16_t bytesperpixel;
   switch(bpp) {
      case 1:  bytesperpixel = 1; amtcolours = 2; break;
      case 8:  bytesperpixel = 1; amtcolours = 256; break;
      case 15: bytesperpixel = 2; amtcolours = 32768; break;
      case 16: bytesperpixel = 2; amtcolours = 65536; break;
      case 24: bytesperpixel = 3; amtcolours = 16777216; break;
      case 32: bytesperpixel = 4; amtcolours = 16777216; break;
      default: return log.warning(ERR::Args);
   }

   if (Self->Type IS BMP::PLANAR) bytewidth = (width + (width % 16))/8;
   else bytewidth = width * bytesperpixel;

   int linewidth = ALIGN32(bytewidth);
   int planemod = bytewidth * height;

   if (Self->Type IS BMP::PLANAR) size = linewidth * height * bpp;
   else size = linewidth * height;

   if ((Self->Owner) and (Self->Owner->classID() IS CLASSID::DISPLAY)) {
      // A display's bitmap is backed by a host surface, so the driver is given the opportunity to resize its own
      // storage (e.g. the X11 background pixmap).  Drivers that do not support the operation are ignored because
      // the field values below are recalculated regardless.

      if ((glDriver) and (Self->prvAFlags & (BF_WINVIDEO|BF_DRIVER_DATA))) {
         glDriver->resizeBitmap(Self, width, height);
      }
      goto setfields;
   }

   if ((Self->prvAFlags & (BF_WINVIDEO|BF_DRIVER_DATA)) and (glDriver)) {
      if (auto error = glDriver->resizeBitmap(Self, width, height); error != ERR::NoSupport) return error;
      return ERR::NoSupport;
   }


   if ((Self->Flags & BMF::NO_DATA) != BMF::NIL);
   else if ((Self->Data) and (Self->prvAFlags & BF_DATA)) {
      uint8_t *data;
      if ((size <= Self->Size) and (size / Self->Size > 0.5)) { // Do nothing when shrinking unless able to save considerable resources
         size = Self->Size;
      }
      else if ((data = (uint8_t *)malloc(size))) {
         if (Self->Data) free(Self->Data);
         Self->Data = data;
      }
      else return log.warning(ERR::AllocMemory);
   }
   else return log.warning(ERR::UndefinedField);

setfields:
   Self->Width         = width;
   Self->Height        = height;
   Self->Size          = size;
   Self->BitsPerPixel  = bpp;
   Self->AmtColours    = amtcolours;
   Self->BytesPerPixel = bytesperpixel;
   Self->ByteWidth     = bytewidth;
   Self->LineWidth     = linewidth;
   Self->PlaneMod      = planemod;
   Self->Clip.Left      = 0;
   Self->Clip.Top       = 0;
   Self->Clip.Right     = width;
   Self->Clip.Bottom    = height;


   if (origbpp != Self->BitsPerPixel) {
      gfx::GetColourFormat(Self->ColourFormat, Self->BitsPerPixel, 0, 0, 0, 0);
   }

   calc_pixel_routines(Self);

   if ((Self->Flags & BMF::CLEAR) != BMF::NIL) {
      gfx::DrawRectangle(Self, 0, 0, Self->Width, Self->Height, Self->getColour(Self->Bkgd), BAF::FILL);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveImage: Saves the bitmap image to a writable object in PCX format.

SaveImage writes the current clipping region to `Dest` as PCX image data.  Paletted bitmaps are written with a palette;
true-colour bitmaps are written as three colour planes.  If #ColourSpace is `CS::LINEAR_RGB`, RGB values are converted
to sRGB while the image is written.

Errors returned by the destination object's Write action are propagated to the caller.

-ERRORS-
Okay
NullArgs
BufferOverflow
AllocMemory: The read buffer for a host drawable could not be allocated.
NoSupport: The bitmap surface cannot be read by the CPU.
*********************************************************************************************************************/

static ERR BITMAP_SaveImage(extBitmap *Self, struct acSaveImage *Args)
{
   kt::Log log;
   struct {
      int8_t  Signature;
      int8_t  Version;
      int8_t  Encoding;
      int8_t  BitsPixel;
      int16_t XMin, YMin;
      int16_t XMax, YMax;
      int16_t XDPI, YDPI; // DPI
      uint8_t palette[48];
      int8_t  Reserved;
      int8_t  NumPlanes;
      int16_t BytesLine;
      int16_t PalType;
      int16_t XRes;
      int16_t YRes;
      uint8_t dummy[54];
   } pcx;
   RGB8 rgb;
   uint8_t lastpixel, newpixel;
   int i, j, p;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);

   log.branch("Save To #%d", Args->Dest->UID);

   int width = Self->Clip.Right - Self->Clip.Left;
   int height = Self->Clip.Bottom - Self->Clip.Top;

   // Create PCX Header

   clearmem(&pcx, sizeof(pcx));
   pcx.Signature = 10;       // ZSoft PCX-files
   pcx.Version   = 5;        // Version
   pcx.Encoding  = 1;        // Run Length Encoding=ON
   pcx.XMin      = 0;
   pcx.YMin      = 0;
   pcx.BitsPixel = 8;
   pcx.BytesLine = width;
   pcx.XMax      = width - 1;
   pcx.YMax      = height - 1;
   pcx.XDPI      = 300;
   pcx.YDPI      = 300;
   pcx.PalType   = 1;
   pcx.XRes      = width;
   pcx.YRes      = height;
   if (Self->AmtColours <= 256) pcx.NumPlanes = 1;
   else pcx.NumPlanes = 3;

   // The bitmap may be backed by a host drawable with no CPU accessible data area, so a read lock is required
   // before the pixel readers can be used.

   if (auto error = lock_surface(Self, SURFACE_READ); error != ERR::Okay) return log.warning(error);

   auto error = [&]() -> ERR {
      const auto buffer_size = size_t(width) * size_t(height) * size_t(pcx.NumPlanes) * 2;
      std::vector<uint8_t> buffer(buffer_size);
      auto write_error = acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)&pcx, sizeof(pcx)));
      if (write_error != ERR::Okay) return log.warning(write_error);

      int dp = 0;
      auto append_byte = [&](uint8_t Value) {
         if (size_t(dp) >= buffer.size()) return false;
         buffer[dp++] = Value;
         return true;
      };

      for (i=Self->Clip.Top; i < (Self->Clip.Bottom); i++) {
         if (pcx.NumPlanes IS 1) { // Save as a 256 colour image
            lastpixel = Self->ReadUCPixel(Self, Self->Clip.Left, i);
            uint8_t counter = 1;
            for (j=Self->Clip.Left+1; j < Self->Clip.Right; j++) {
               newpixel = Self->ReadUCPixel(Self, j, i);

               if ((newpixel IS lastpixel) and (counter < 63)) {
                  counter++;
               }
               else {
                  if (not ((counter IS 1) and (lastpixel < 192))) {
                     if (not append_byte(192 + counter)) return log.warning(ERR::BufferOverflow);
                  }
                  if (not append_byte(lastpixel)) return log.warning(ERR::BufferOverflow);
                  lastpixel = newpixel;
                  counter = 1;
               }
            }

            if (not ((counter IS 1) and (lastpixel < 192))) {
               if (not append_byte(192 + counter)) return log.warning(ERR::BufferOverflow);
            }
            if (not append_byte(lastpixel)) return log.warning(ERR::BufferOverflow);
         }
         else { // Save as a true colour image with run-length encoding
            auto read_pixel = [&](int X, int Y) {
               Self->ReadUCRPixel(Self, X, Y, &rgb);
               if (Self->ColourSpace IS CS::LINEAR_RGB) glLinearRGB.invert(rgb);
            };

            for (p=0; p < 3; p++) {
               read_pixel(Self->Clip.Left, i);

               switch(p) {
                  case 0:  lastpixel = rgb.Red;   break;
                  case 1:  lastpixel = rgb.Green; break;
                  default: lastpixel = rgb.Blue;
               }
               uint8_t counter = 1;

               for (j=Self->Clip.Left+1; j < Self->Clip.Right; j++) {
                  read_pixel(j, i);
                  switch(p) {
                     case 0:  newpixel = rgb.Red;   break;
                     case 1:  newpixel = rgb.Green; break;
                     default: newpixel = rgb.Blue;
                  }

                  if (newpixel IS lastpixel) {
                     counter++;
                     if (counter IS 63) {
                        if ((not append_byte(0xc0 | counter)) or (not append_byte(lastpixel))) {
                           return log.warning(ERR::BufferOverflow);
                        }
                        counter = 0;
                     }
                  }
                  else {
                     if ((counter IS 1) and (0xc0 != (0xc0 & lastpixel))) {
                        if (not append_byte(lastpixel)) return log.warning(ERR::BufferOverflow);
                     }
                     else if (counter) {
                        if ((not append_byte(0xc0 | counter)) or (not append_byte(lastpixel))) {
                           return log.warning(ERR::BufferOverflow);
                        }
                     }
                     lastpixel = newpixel;
                     counter = 1;
                  }
               }

               // Finish line if necessary

               if ((counter IS 1) and (0xc0 != (0xc0 & lastpixel))) {
                  if (not append_byte(lastpixel)) return log.warning(ERR::BufferOverflow);
               }
               else if (counter) {
                  if ((not append_byte(0xc0 | counter)) or (not append_byte(lastpixel))) {
                     return log.warning(ERR::BufferOverflow);
                  }
               }
            }
         }
      }

      write_error = acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)buffer.data(), dp));
      if (write_error != ERR::Okay) return log.warning(write_error);

      // Setup palette

      if (Self->AmtColours <= 256) {
         uint8_t palette[(256 * 3) + 1];
         int j = 0;
         palette[j++] = 12;          // Palette identifier
         for (int i=0; i < 256; i++) {
            palette[j++] = Self->Palette->Col[i].Red;
            palette[j++] = Self->Palette->Col[i].Green;
            palette[j++] = Self->Palette->Col[i].Blue;
         }

         write_error = acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)palette, sizeof(palette)));
         if (write_error != ERR::Okay) return log.warning(write_error);
      }

      return ERR::Okay;
   }();

   unlock_surface(Self);
   return error;
}

/*********************************************************************************************************************
-ACTION-
Seek: Changes the current byte position for read/write operations.

Seek sets #Position from the supplied byte offset and origin.  Positions before the start of the bitmap are clamped to
zero, and positions beyond #Size are clamped to #Size.

-ERRORS-
Okay
NullArgs
Args

*********************************************************************************************************************/

static ERR BITMAP_Seek(extBitmap *Self, struct acSeek *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Args->Position IS SEEK::START) Self->Position = (int)Args->Offset;
   else if (Args->Position IS SEEK::END) Self->Position = (int)(Self->Size - Args->Offset);
   else if (Args->Position IS SEEK::CURRENT) Self->Position = (int)(Self->Position + Args->Offset);
   else return ERR::Args;

   if (Self->Position > Self->Size) Self->Position = Self->Size;
   else if (Self->Position < 0) Self->Position = 0;

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetClipRegion: Sets a clipping region for a bitmap object.

SetClipRegion() updates the bitmap's clipping region.  Drawing operations are restricted to the
combined region.

This method is implemented by ~Display.SetClipRegion().

-INPUT-
int Left:      The horizontal start of the clip region.
int Top:       The vertical start of the clip region.
int Right:     The exclusive right edge of the clip region.
int Bottom:    The exclusive bottom edge of the clip region.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR BITMAP_SetClipRegion(extBitmap *Self, struct bmp::SetClipRegion *Args)
{
   if (not Args) return ERR::NullArgs;

   gfx::SetClipRegion(Self, Args->Left, Args->Top, Args->Right, Args->Bottom);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Unlock: Unlocks the bitmap surface once direct access is no longer required.

Unlock releases or synchronises any platform resources held for direct CPU access after #Lock().

-ERRORS-
Okay

*********************************************************************************************************************/

static ERR BITMAP_Unlock(extBitmap *Self)
{
   unlock_surface(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Write: Writes raw image data to a bitmap object.

Write copies bytes from the supplied input buffer into #Data, starting at #Position.  #Position is advanced by the
number of bytes written and the result count is returned in the action arguments.

The write must fit within the bitmap's allocated #Size.  Use #Seek() to change the target position before writing.

-ERRORS-
Okay
NoData
NullArgs
OutOfSpace
*********************************************************************************************************************/

static ERR BITMAP_Write(extBitmap *Self, struct acWrite *Args)
{
   if (not Args) return ERR::NullArgs;

   Args->Result = 0;

   if (not Self->Data) return ERR::NoData;
   if (Args->Buffer.empty()) return ERR::Okay;
   if (not Args->Buffer.data()) return ERR::NullArgs;
   if (Args->Buffer.size() > size_t(INT_MAX)) return ERR::OutOfRange;
   const int length = int(Args->Buffer.size());

   int available = Self->Size - Self->Position;
   if (available <= 0) return ERR::OutOfSpace;
   if (length > available) return ERR::OutOfSpace;

   copymem(Args->Buffer.data(), Self->Data + Self->Position, length);
   Self->Position += length;
   Args->Result = length;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AmtColours: The maximum number of colours represented by the bitmap format.

For indexed bitmaps, this is the size of the usable palette.  For direct-colour bitmaps, it reflects the colour range
implied by #BitsPerPixel and the selected #ColourFormat.

-FIELD-
BitsPerPixel: The number of bits used to represent each pixel.

This includes all bits used by the pixel format, including alpha bits where present.

-FIELD-
Bkgd: Background colour in RGB format.

The background colour is used by operations that need a default fill colour, such as #Clear(), #Draw() and some resize
paths.  The default background colour is black.

The #BkgdIndex will be updated as a result of setting this field.

*********************************************************************************************************************/

static ERR SET_Bkgd(extBitmap *Self, RGB8 *Value)
{
   Self->Bkgd = *Value;

   if (Self->BitsPerPixel > 8) {
      Self->BkgdIndex = (((Self->Bkgd.Red   >>Self->prvColourFormat.RedShift)   & Self->prvColourFormat.RedMask)   << Self->prvColourFormat.RedPos) |
                         (((Self->Bkgd.Green>>Self->prvColourFormat.GreenShift) & Self->prvColourFormat.GreenMask) << Self->prvColourFormat.GreenPos) |
                         (((Self->Bkgd.Blue >>Self->prvColourFormat.BlueShift)  & Self->prvColourFormat.BlueMask)  << Self->prvColourFormat.BluePos) |
                         (((Self->Bkgd.Alpha>>Self->prvColourFormat.AlphaShift) & Self->prvColourFormat.AlphaMask) << Self->prvColourFormat.AlphaPos);
   }
   else Self->BkgdIndex = RGBToValue(&Self->Bkgd, Self->Palette);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BkgdIndex: Background colour as a packed pixel value or palette index.

Use #Bkgd for most updates.  Set BkgdIndex directly only when the caller has already calculated the target bitmap's
native pixel value or palette index.

*********************************************************************************************************************/

static ERR SET_BkgdIndex(extBitmap *Self, int Index)
{
   if ((Index < 0) or (Index > 255)) return ERR::OutOfRange;
   Self->BkgdIndex = Index;
   Self->Bkgd   = Self->Palette->Col[Self->BkgdIndex];
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BlendMode: Defines the blending algorithm to use when rendering transparent pixels.

The default value is `BLM::AUTO`, which selects the preferred blending path for the current bitmap and graphics
backend.

-FIELD-
BytesPerPixel: The number of bytes per pixel.

This field reflects the byte count used by one chunky pixel.  Values normally range from 1 to 4.  For planar bitmaps,
#BitsPerPixel is the more useful format indicator.

-FIELD-
ByteWidth: The width of the bitmap, in bytes.

ByteWidth is calculated from #Width, #Type and #BytesPerPixel.  It describes the meaningful pixel bytes in a row and
does not include alignment padding.

The formulas used to calculate the value of this field are:

<pre>
Planar      = Width/8
Chunky/8    = Width
Chunky/15   = Width * 2
Chunky/16   = Width * 2
Chunky/24   = Width * 3
Chunky/32   = Width * 4
</pre>

To learn the total byte-width per line including any additional padded bytes, refer to the #LineWidth field.

-FIELD-
ClipBottom: The exclusive bottom edge of the bitmap clipping region.

The default clipping region matches the bitmap dimensions.  Drawing operations are limited to the active clipping
region.

-FIELD-
ClipLeft: The left-most edge of a bitmap's clipping region.

The default clipping region matches the bitmap dimensions.  Drawing operations are limited to the active clipping
region.

-FIELD-
ClipRight: The exclusive right edge of the bitmap clipping region.

The default clipping region matches the bitmap dimensions.  Drawing operations are limited to the active clipping
region.

-FIELD-
ClipTop: The top-most edge of a bitmap's clipping region.

The default clipping region matches the bitmap dimensions.  Drawing operations are limited to the active clipping
region.

-FIELD-
Clip: Defines the bitmap's clipping region.

Clip is a shorthand reference for #ClipLeft, #ClipTop, #ClipRight and #ClipBottom, returning all four values as a
single !ClipRectangle structure.

*********************************************************************************************************************/

static ERR GET_Clip(extBitmap *Self, ClipRectangle **Value)
{
   *Value = &Self->Clip;
   return ERR::Okay;
}

static ERR SET_Clip(extBitmap *Self, ClipRectangle *Value)
{
   Self->Clip = *Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ColourFormat: Describes the colour format used to construct each bitmap pixel.

ColourFormat points to the structure that describes how packed pixel values map to red, green, blue and alpha channels.
It is relevant for direct-colour bitmaps, normally those with two or more bytes per pixel.

!ColourFormat

The following C++ helper methods can be called on a bitmap to build packed colour values from channel components:

<pre>
packPixel(Red, Green, Blue)
packPixel(Red, Green, Blue, Alpha)
packAlpha(Alpha)
packPixelRGB(RGB8 &RGB)
packPixelRGBA(RGB8 &RGB)
</pre>

The following C macros are optimised forms for 24 and 32-bit bitmaps:

<pre>
PackPixelWB(Red, Green, Blue)
PackPixelWBA(Red, Green, Blue, Alpha)
</pre>

The following C++ helper methods unpack individual colour components from a packed colour value:

<pre>
unpackRed(Colour)
unpackGreen(Colour)
unpackBlue(Colour)
unpackAlpha(Colour)
</pre>

-FIELD-
Data: Provides direct access to the bitmap's data area.

Data points to the first byte of the bitmap's pixel buffer when CPU-visible memory is available.  Caller-supplied
memory can be used for data-backed bitmaps, but most callers should let #Init() allocate the correctly sized buffer.

For video or texture-backed bitmaps, #Data may be unavailable until #Lock() succeeds.

*********************************************************************************************************************/

static ERR GET_Data(extBitmap *Self, std::span<uint8_t> &Value)
{
   if ((not Self->Data) or (Self->Size <= 0)) return ERR::FieldNotSet;

   Value = std::span<uint8_t>(Self->Data, size_t(Self->Size));
   return ERR::Okay;
}

static ERR SET_Data(extBitmap *Self, std::span<const uint8_t> &Value)
{

   Self->Data = const_cast<uint8_t *>(Value.data());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DrawUCPixel: Points to a C function that draws pixels to the bitmap using colour indexes.

DrawUCPixel points to the active low-level pixel writer for packed colour or palette-index values.  It is intended for
C callers that need direct pixel access.  No clipping or bounds checks are performed.

The prototype of the DrawUCPixel function is `Function(*Bitmap, LONG X, LONG Y, UINT Colour)`.

The new pixel value is supplied in the `Colour` parameter.

-FIELD-
DrawUCRIndex: Points to a C function that draws pixels to the bitmap in RGB format.

DrawUCRIndex points to the active low-level RGB pixel writer for a caller-supplied address inside #Data.  It is
intended for C callers that need direct pixel access.  No clipping, bounds or address validation is performed.

The prototype of the DrawUCRIndex function is `Function(*Bitmap, BYTE *Data, RGB8 *RGB)`.

The Data parameter must point to a location within the Bitmap's graphical address space.  The new pixel value must be
defined in the `RGB` parameter.

There is no colour-index equivalent because callers can write indexed pixel bytes directly through #Data.

-FIELD-
DrawUCRPixel: Points to a C function that draws pixels to the bitmap in RGB format.

DrawUCRPixel points to the active low-level RGB pixel writer for `X`, `Y` coordinates.  It is intended for C callers
that need direct pixel access.  No clipping or bounds checks are performed.

The prototype of the DrawUCRPixel function is `Function(*Bitmap, LONG X, LONG Y, RGB8 *RGB)`.

The new pixel value must be defined in the `RGB` parameter.

-FIELD-
Flags: Optional flags.

-FIELD-
Handle: Platform-dependent field for referencing video memory.
-END-

*********************************************************************************************************************/

static ERR GET_Handle(extBitmap *Self, APTR *Value)
{
   if (glDriver) {
      *Value = Self->DriverData;
      return ERR::Okay;
   }
   return ERR::NoSupport;
}

static ERR SET_Handle(extBitmap *Self, APTR Value)
{
   // Note: The only area of the system allowed to set this field are the Display/Surface classes for video management.

   if (glDriver) {
      Self->DriverData = Value;
      return ERR::Okay;
   }
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
Height: The height of the bitmap, in pixels.

-FIELD-
LineWidth: The length of each bitmap line in bytes, including alignment.

LineWidth includes any row padding required by the active bitmap type or platform backend.  Use #ByteWidth for the
number of meaningful pixel bytes in a row.

-FIELD-
MemType: Defines the memory type used to host a bitmap's data area.

MemType controls the kind of backing storage requested during initialisation.  The available values are `BMT::DATA`,
`BMT::VIDEO` and `BMT::TEXTURE`.

Video or texture-backed bitmaps can be faster for some drawing paths, but direct CPU access is platform dependent.  Use
#Lock() before reading or writing #Data directly when the bitmap is not a regular data bitmap.

-FIELD-
Opacity: Determines the translucency setting to use in drawing operations.

Opacity is an 8-bit alpha multiplier used by drawing operations that support translucent bitmap copies.  A value of
`255` is fully opaque and disables additional translucency.  Lower values make copied pixels more transparent.

This value is separate from any per-pixel alpha channel stored in the bitmap.

-FIELD-
Palette: Points to a bitmap's colour palette.

Palette points to the bitmap's colour table.  Indexed bitmaps use this table to map pixel values to RGB colours, and
some conversion paths use it even when the bitmap itself is direct-colour.

The structure starts with the palette header and colour count, followed by colour entries in index order.  There is no
terminating entry.

The following example is for a 32 colour palette:

<pre>
RGBPalette Palette = {
  ID_PALETTE, VER_PALETTE, 32,
  {{ 0x00,0x00,0x00 }, { 0x10,0x10,0x10 }, { 0x17,0x17,0x17 }, { 0x20,0x20,0x20 },
   { 0x27,0x27,0x27 }, { 0x30,0x30,0x30 }, { 0x37,0x37,0x37 }, { 0x40,0x40,0x40 },
   { 0x47,0x47,0x47 }, { 0x50,0x50,0x50 }, { 0x57,0x57,0x57 }, { 0x60,0x60,0x60 },
   { 0x67,0x67,0x67 }, { 0x70,0x70,0x70 }, { 0x77,0x77,0x77 }, { 0x80,0x80,0x80 },
   { 0x87,0x87,0x87 }, { 0x90,0x90,0x90 }, { 0x97,0x97,0x97 }, { 0xa0,0xa0,0xa0 },
   { 0xa7,0xa7,0xa7 }, { 0xb0,0xb0,0xb0 }, { 0xb7,0xb7,0xb7 }, { 0xc0,0xc0,0xc0 },
   { 0xc7,0xc7,0xc7 }, { 0xd0,0xd0,0xd0 }, { 0xd7,0xd7,0xd7 }, { 0xe0,0xe0,0xe0 },
   { 0xe0,0xe0,0xe0 }, { 0xf0,0xf0,0xf0 }, { 0xf7,0xf7,0xf7 }, { 0xff,0xff,0xff }
   }
};
</pre>

Palettes are created for all bitmap types, including RGB bitmaps above 8-bit colour, because several drawing functions
use a palette table when converting between bitmap formats.

Parent objects such as @Display may need to be updated separately before palette changes are reflected by the visible
display.

*********************************************************************************************************************/

ERR SET_Palette(extBitmap *Self, RGBPalette *SrcPalette)
{
   if (not SrcPalette) return ERR::Okay;

   if (SrcPalette->AmtColours <= 256) {
      Self->Palette->AmtColours = SrcPalette->AmtColours;
      int16_t i = SrcPalette->AmtColours-1;
      while (i > 0) {
         Self->Palette->Col[i] = SrcPalette->Col[i];
         i--;
      }
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::BufferOverflow);
}

/*********************************************************************************************************************

-FIELD-
PlaneMod: The differential between each bitmap plane.

PlaneMod specifies the byte distance between each bitplane in planar bitmaps.  For chunky bitmaps, it reflects the
total size of the bitmap buffer.

-FIELD-
Position: The current read/write data position.

Position is the byte offset used by #Read() and #Write().  Use #Seek() to change it.

-FIELD-
ReadUCRIndex: Points to a C function that reads pixels from the bitmap in RGB format.

ReadUCRIndex points to the active low-level RGB pixel reader for a caller-supplied address inside #Data.  It is
intended for C callers that need direct pixel access.  No clipping, bounds or address validation is performed.

The prototype of the ReadUCRIndex function is `Function(*Bitmap, BYTE *Data, RGB8 *RGB)`.

The `Data` parameter must point to a location within the Bitmap's graphical address space.  The pixel value will be
returned in the `RGB` parameter.

There is no colour-index equivalent because callers can read indexed pixel bytes directly through #Data.

-FIELD-
ReadUCPixel: Points to a C function that reads pixels from the bitmap in colour index format.

ReadUCPixel points to the active low-level pixel reader for packed colour or palette-index values.  It is intended for
C callers that need direct pixel access.  No clipping or bounds checks are performed.

The prototype of the ReadUCPixel function is `Function(*Bitmap, LONG X, LONG Y, LONG *Index)`.

The pixel value will be returned in the `Index` parameter.

-FIELD-
ReadUCRPixel: Points to a C function that reads pixels from the bitmap in RGB format.

ReadUCRPixel points to the active low-level RGB pixel reader for `X`, `Y` coordinates.  It is intended for C callers
that need direct pixel access.  No clipping or bounds checks are performed.

The prototype of the ReadUCRPixel function is `Function(*Bitmap, LONG X, LONG Y, RGB8 *RGB)`.

The pixel value is returned in the `RGB` parameter.  Because this function expands the pixel value to RGB components,
#ReadUCPixel or #ReadUCRIndex may be faster when RGB decomposition is not required.

-FIELD-
Size: The total size of the bitmap, in bytes.

-FIELD-
TransColour: The transparent colour of the bitmap, in RGB format.

Pixels matching this colour are skipped by drawing operations that honour colour-key transparency.

Do not use colour-key transparency on bitmaps that use alpha transparency.

*********************************************************************************************************************/

static ERR SET_Trans(extBitmap *Self, RGB8 *Value)
{
   Self->TransColour = *Value;

   if (Self->BitsPerPixel > 8) {
      Self->TransIndex = (((Self->TransColour.Red  >>Self->prvColourFormat.RedShift)   & Self->prvColourFormat.RedMask)   << Self->prvColourFormat.RedPos) |
                         (((Self->TransColour.Green>>Self->prvColourFormat.GreenShift) & Self->prvColourFormat.GreenMask) << Self->prvColourFormat.GreenPos) |
                         (((Self->TransColour.Blue >>Self->prvColourFormat.BlueShift)  & Self->prvColourFormat.BlueMask)  << Self->prvColourFormat.BluePos) |
                         (((Self->TransColour.Alpha>>Self->prvColourFormat.AlphaShift) & Self->prvColourFormat.AlphaMask) << Self->prvColourFormat.AlphaPos);
   }
   else Self->TransIndex = RGBToValue(&Self->TransColour, Self->Palette);

   if (Self->MemType != BMT::VIDEO) Self->Flags |= BMF::TRANSPARENT;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TransIndex: The transparent colour of the bitmap, represented as an index.

TransIndex stores the transparent colour as a packed pixel value or palette index.  Pixels matching this value are
skipped by drawing operations that honour colour-key transparency.

Use #TransColour for most updates.  Set TransIndex directly only when the caller has already calculated the target
bitmap's native pixel value or palette index.  Do not use colour-key transparency on bitmaps that use alpha
transparency.

*********************************************************************************************************************/

static ERR SET_TransIndex(extBitmap *Self, int Index)
{
   if ((Index < 0) or (Index > 255)) return ERR::OutOfRange;

   Self->TransIndex = Index;
   Self->TransColour   = Self->Palette->Col[Self->TransIndex];

   if (Self->MemType != BMT::VIDEO) Self->Flags |= BMF::TRANSPARENT;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Type: Defines the data type of the bitmap.

Type defines the bitmap layout, either `BMP::PLANAR` for planar bitmaps or `BMP::CHUNKY` for interleaved pixel data.
Chunky is the default.

-FIELD-
Width: The width of the bitmap, in pixels.

Width must be set before #Query() or #Init() can derive the bitmap layout.

*********************************************************************************************************************/

static ERR calc_pixel_routines(extBitmap *Self)
{
   kt::Log log;

   if (Self->Type IS BMP::PLANAR) {
      Self->ReadUCPixel  = MemReadPixelPlanar;
      Self->ReadUCRPixel = MemReadRGBPixelPlanar;
      Self->ReadUCRIndex = MemReadRGBIndexPlanar;
      Self->DrawUCPixel  = MemDrawPixelPlanar;
      Self->DrawUCRPixel = DrawRGBPixelPlanar;
      Self->DrawUCRIndex = nullptr;
      return ERR::Okay;
   }

   if (Self->Type != BMP::CHUNKY) {
      log.warning("Unsupported Bitmap->Type %d.", int(Self->Type));
      return ERR::NoSupport;
   }

   if ((glDriver) and (Self->prvAFlags & (BF_WINVIDEO|BF_DRIVER_DATA))) {
      // Driver-owned storage can still be ordinary RAM, such as an X11 shared image.
      // Use the memory routines when the driver has no specialised pixel access for it.

      if (auto error = glDriver->bitmapRoutines(Self); error != ERR::NoSupport) return error;
      if (Self->prvAFlags & BF_WINVIDEO) return ERR::NoSupport;
   }


   switch(Self->BytesPerPixel) {
      case 1:
        Self->ReadUCPixel  = MemReadPixel8;
        Self->ReadUCRPixel = MemReadRGBPixel8;
        Self->ReadUCRIndex = MemReadRGBIndex8;
        Self->DrawUCPixel  = MemDrawPixel8;
        Self->DrawUCRPixel = MemDrawRGBPixel8;
        Self->DrawUCRIndex = MemDrawRGBIndex8;
        break;

      case 2:
         Self->ReadUCPixel  = MemReadPixel16;
         Self->ReadUCRPixel = MemReadRGBPixel16;
         Self->ReadUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))MemReadRGBIndex16;
         Self->DrawUCPixel  = MemDrawPixel16;
         Self->DrawUCRPixel = MemDrawRGBPixel16;
         Self->DrawUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))MemDrawRGBIndex16;
         break;

      case 3:
         if (Self->prvColourFormat.RedPos IS 16) {
            Self->ReadUCPixel  = MemReadLSBPixel24;
            Self->ReadUCRPixel = MemReadLSBRGBPixel24;
            Self->ReadUCRIndex = MemReadLSBRGBIndex24;
            Self->DrawUCPixel  = MemDrawLSBPixel24;
            Self->DrawUCRPixel = MemDrawLSBRGBPixel24;
            Self->DrawUCRIndex = MemDrawLSBRGBIndex24;
         }
         else {
            Self->ReadUCPixel  = MemReadMSBPixel24;
            Self->ReadUCRPixel = MemReadMSBRGBPixel24;
            Self->ReadUCRIndex = MemReadMSBRGBIndex24;
            Self->DrawUCPixel  = MemDrawMSBPixel24;
            Self->DrawUCRPixel = MemDrawMSBRGBPixel24;
            Self->DrawUCRIndex = MemDrawMSBRGBIndex24;
         }
         break;

      case 4:
         Self->ReadUCPixel  = MemReadPixel32;
         Self->ReadUCRPixel = MemReadRGBPixel32;
         Self->ReadUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))MemReadRGBIndex32;
         Self->DrawUCPixel  = MemDrawPixel32;
         Self->DrawUCRPixel = MemDrawRGBPixel32;
         Self->DrawUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))MemDrawRGBIndex32;
         break;

      default:
        log.warning("Unsupported Bitmap->BytesPerPixel %d.", Self->BytesPerPixel);
        return ERR::NoSupport;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

extBitmap::extBitmap(objMetaClass *ClassPtr, OBJECTID ObjectID) : objBitmap(ClassPtr, ObjectID)
{
   constexpr int CBANK = 5;
   RGB8 *RGB;
   int i, j;

   Palette      = &prvPaletteArray;
   ColourFormat = &prvColourFormat;
   ColourSpace  = CS::SRGB;
   BlendMode    = BLM::AUTO;
   Opacity      = 255;
   DriverData   = nullptr;

   // Generate the standard colour palette

   Palette = &prvPaletteArray;
   Palette->AmtColours = 256;

   RGB = Palette->Col;
   RGB++; // Skip the black pixel at the start

   for (i=0; i < 6; i++) {
      for (j=0; j < CBANK; j++) {
         RGB[(i*CBANK) + j].Red   = (i * 255/CBANK);
         RGB[(i*CBANK) + j].Green = 0;
         RGB[(i*CBANK) + j].Blue  = (j + 1) * 255/CBANK;
      }
   }

   for (i=6; i < 12; i++) {
      for (j=0; j < 5; j++) {
         RGB[(i*CBANK) + j].Red   = ((i-6) * 255/CBANK);
         RGB[(i*CBANK) + j].Green = 51;
         RGB[(i*CBANK) + j].Blue  = (j + 1) * 255/CBANK;
      }
   }

   for (i=12; i < 18; i++) {
      for (j=0; j < 5; j++) {
         RGB[(i*CBANK) + j].Blue  = (j + 1) * 255/CBANK;
         RGB[(i*CBANK) + j].Red   = ((i-12) * 255/CBANK);
         RGB[(i*CBANK) + j].Green = 102;
      }
   }

   for (i=18; i < 24; i++) {
      for (j=0; j < 5; j++) {
         RGB[(i*CBANK) + j].Blue  = (j + 1) * 255/CBANK;
         RGB[(i*CBANK) + j].Red   = ((i-18) * 255/CBANK);
         RGB[(i*CBANK) + j].Green = 153;
      }
   }

   for (i=24; i < 30; i++) {
      for (j=0; j < 5; j++) {
         RGB[(i*CBANK) + j].Blue  = (j + 1) * 255/CBANK;
         RGB[(i*CBANK) + j].Red   = ((i-24) * 255/CBANK);
         RGB[(i*CBANK) + j].Green = 204;
      }
   }

   for (i=30; i < 36; i++) {
      for (j=0; j < 5; j++) {
         RGB[(i*CBANK) + j].Blue  = (j + 1) * 255/CBANK;
         RGB[(i*CBANK) + j].Red   = ((i-30) * 255/CBANK);
         RGB[(i*CBANK) + j].Green = 255;
      }
   }
}

//********************************************************************************************************************

extBitmap::~extBitmap()
{
   if (glDriver) glDriver->freeBitmap(this);

   if ((Data) and (prvAFlags & BF_DATA)) free(Data);
   if (ResolutionChangeHandle) UnsubscribeEvent(ResolutionChangeHandle);

}

//********************************************************************************************************************

#include "lib_mempixels.cpp"



#include "class_bitmap_def.c"

static const FieldArray clBitmapFields[] = {
   { "Palette",       FDF_POINTER|FDF_RW, nullptr, SET_Palette },
   { "ColourFormat",  FDF_POINTER|FDF_STRUCT|FDF_R, nullptr, nullptr, "ColourFormat" },
   { "DrawUCPixel",   FDF_POINTER|FDF_R },
   { "DrawUCRPixel",  FDF_POINTER|FDF_R },
   { "ReadUCPixel",   FDF_POINTER|FDF_R },
   { "ReadUCRPixel",  FDF_POINTER|FDF_R },
   { "ReadUCRIndex",  FDF_POINTER|FDF_R },
   { "DrawUCRIndex",  FDF_POINTER|FDF_R },
   { "Data",          FDF_ARRAY|FDF_BYTE|FDF_RI, GET_Data, SET_Data },
   { "Width",         FDF_INT|FDF_RI, nullptr, nullptr },
   { "ByteWidth",     FDF_INT|FDF_R, nullptr, nullptr },
   { "Height",        FDF_INT|FDF_RI, nullptr, nullptr },
   { "Type",          FDF_INT|FDF_RI|FDF_LOOKUP, nullptr, nullptr, &clBitmapType },
   { "LineWidth",     FDF_INT|FDF_R },
   { "PlaneMod",      FDF_INT|FDF_R },
   { "ClipLeft",      FDF_INT|FDF_RW },
   { "ClipRight",     FDF_INT|FDF_RW },
   { "ClipBottom",    FDF_INT|FDF_RW },
   { "ClipTop",       FDF_INT|FDF_RW },
   { "Size",          FDF_INT|FDF_R },
   { "MemType",       FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clMemType },
   { "AmtColours",    FDF_INT|FDF_RI },
   { "Flags",         FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clBitmapFlags },
   { "TransIndex",    FDF_INT|FDF_RW, nullptr, SET_TransIndex },
   { "BytesPerPixel", FDF_INT|FDF_RI },
   { "BitsPerPixel",  FDF_INT|FDF_RI },
   { "Position",      FDF_INT|FDF_R },
   { "Opacity",       FDF_INT|FDF_RW },
   { "BlendMode",     FDF_INT|FDF_RW|FDF_LOOKUP, nullptr, nullptr, &clBitmapBlendMode },
   { "DataID",        FDF_INT|FDF_SYSTEM|FDF_R },
   { "TransColour",   FDF_STRUCT|FDF_RW, nullptr, SET_Trans, "RGB8" },
   { "Bkgd",          FDF_STRUCT|FDF_RW, nullptr, SET_Bkgd, "RGB8" },
   { "BkgdIndex",     FDF_INT|FDF_RW, nullptr, SET_BkgdIndex },
   { "ColourSpace",   FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clBitmapColourSpace },
   // Virtual fields
   { "Clip",          FDF_POINTER|FDF_STRUCT|FDF_RW|FDF_PURE, GET_Clip, SET_Clip },
   { "Handle",        FDF_POINTER|FDF_RW|FDF_PURE, GET_Handle, SET_Handle },
   END_FIELD
};

//********************************************************************************************************************

ERR create_bitmap_class(void)
{
   clBitmap = objMetaClass::create::global(
      fl::ClassVersion(VER_BITMAP),
      fl::Name("Bitmap"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clBitmapActions),
      fl::Methods(clBitmapMethods),
      fl::Fields(clBitmapFields),
      fl::Size(sizeof(extBitmap)),
      fl::Path("modules:display"));

   return clBitmap ? ERR::Okay : ERR::AddClass;
}
