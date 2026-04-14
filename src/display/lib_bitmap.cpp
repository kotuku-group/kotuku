/*********************************************************************************************************************

-CATEGORY-
Name: Bitmap
-END-

*********************************************************************************************************************/

#include "defs.h"

#ifdef _WIN32
using namespace display;
#endif

//********************************************************************************************************************
// NOTE: Please ensure that the Width and Height are already clipped to meet the restrictions of BOTH the source and
// destination bitmaps.

static ERR dither(extBitmap *Bitmap, extBitmap *Dest, ColourFormat *Format, int Width, int Height,
   int SrcX, int SrcY, int DestX, int DestY)
{
   RGB16 *buf1, *buf2, *buffer;
   RGB8 brgb;
   uint8_t *data;
   int x, y;
   int16_t index;

   if ((Width < 1) or (Height < 1)) return ERR::Okay;

   // Punch the developer for stupid mistakes

   if ((Dest->BitsPerPixel >= 24) and (!Format)) return ERR::InvalidData;

   // Do a straight copy if the bitmap is too small for dithering

   if ((Height < 2) or (Width < 2)) {
      for (y=SrcY; y < SrcY+Height; y++) {
         for (x=SrcX; x < SrcX+Width; x++) {
            Bitmap->ReadUCRPixel(Bitmap, x, y, &brgb);
            Dest->DrawUCRPixel(Dest, x, y, &brgb);
         }
      }
      return ERR::Okay;
   }

   auto DITHER_ERROR = [&]<typename T>(uint8_t Src, T RGB16::*Comp, int x, int y) {
      // Dither one colour component
      if (int dif = (buf1[x].*Comp>>3) - (Src<<3)) { // An eighth of the error
         int val3 = buf2[x+1].*Comp + (dif<<1);     // 1/4 down & right
         dif = dif + dif + dif;
         int val1 = buf1[x+1].*Comp + dif;          // 3/8 to the right
         int val2 = buf2[x].*Comp + dif;            // 3/8 to the next row
         if (dif > 0) {                              // Check for overflow
            buf1[x+1].*Comp = std::min(16383, val1);
            buf2[x].*Comp   = std::min(16383, val2);
            buf2[x+1].*Comp = std::min(16383, val3);
         }
         else if (dif < 0) {
            buf1[x+1].*Comp = std::max(0, val1);
            buf2[x].*Comp   = std::max(0, val2);
            buf2[x+1].*Comp = std::max(0, val3);
         }
      }
   };

   std::vector<RGB16> calc_buffer(Width * sizeof(RGB16) * 2);

   buf1 = calc_buffer.data();
   buf2 = buf1 + Width;

   // Prime buf2, which will be copied to buf1 at the start of the loop.  We work with six binary "decimal places" to reduce roundoff errors.

   for (x=0,index=0; x < Width; x++,index+=Bitmap->BytesPerPixel) {
      Bitmap->ReadUCRIndex(Bitmap, Bitmap->Data + index, &brgb);
      buf2[x].Red   = brgb.Red<<6;
      buf2[x].Green = brgb.Green<<6;
      buf2[x].Blue  = brgb.Blue<<6;
      buf2[x].Alpha = brgb.Alpha;
   }

   if (!Format) Format = &Dest->prvColourFormat;

   auto srcdata = Bitmap->Data + ((SrcY+1) * Bitmap->LineWidth);
   auto destdata = Dest->Data + (DestY * Dest->LineWidth);
   uint8_t rmask = Format->RedMask   << Format->RedShift;
   uint8_t gmask = Format->GreenMask << Format->GreenShift;
   uint8_t bmask = Format->BlueMask  << Format->BlueShift;

   for (y=0; y < Height - 1; y++) {
      std::swap(buf1, buf2); // Move line 2 to line 1, line 2 then is empty for reading the next row

      // Read the next source line

      if (Bitmap->BytesPerPixel IS 4) {
         buffer = buf2;
         data = srcdata+(SrcX<<2);
         for (x=0; x < Width; x++, data+=4, buffer++) {
            auto colour = ((uint32_t *)data)[0];
            buffer->Red   = ((uint8_t)(colour >> Bitmap->prvColourFormat.RedPos))<<6;
            buffer->Green = ((uint8_t)(colour >> Bitmap->prvColourFormat.GreenPos))<<6;
            buffer->Blue  = ((uint8_t)(colour >> Bitmap->prvColourFormat.BluePos))<<6;
            buffer->Alpha = ((uint8_t)(colour >> Bitmap->prvColourFormat.AlphaPos));
         }
      }
      else if (Bitmap->BytesPerPixel IS 2) {
         buffer = buf2;
         data = srcdata+(SrcX<<1);
         for (x=0; x < Width; x++, data+=2, buffer++) {
            auto colour = ((uint16_t *)data)[0];
            buffer->Red   = Bitmap->unpackRed(colour)<<6;
            buffer->Green = Bitmap->unpackGreen(colour)<<6;
            buffer->Blue  = Bitmap->unpackBlue(colour)<<6;
         }
      }
      else {
         buffer = buf2;
         data = srcdata + (SrcX * Bitmap->BytesPerPixel);
         for (x=0; x < Width; x++, data+=Bitmap->BytesPerPixel, buffer++) {
            Bitmap->ReadUCRIndex(Bitmap, data, &brgb);
            buffer->Red   = brgb.Red<<6;
            buffer->Green = brgb.Green<<6;
            buffer->Blue  = brgb.Blue<<6;
         }
      }

      // Dither

      buffer = buf1;
      data = destdata + (DestX * Dest->BytesPerPixel);
      if (Dest->BytesPerPixel IS 2) {
         for (x=0; x < Width - 1; x++,data+=2,buffer++) {
            brgb.Red   = (buffer->Red>>6) & rmask;
            brgb.Green = (buffer->Green>>6) & gmask;
            brgb.Blue  = (buffer->Blue>>6) & bmask;
            ((uint16_t *)data)[0] = ((brgb.Red>>Dest->prvColourFormat.RedShift) << Dest->prvColourFormat.RedPos) |
                                 ((brgb.Green>>Dest->prvColourFormat.GreenShift) << Dest->prvColourFormat.GreenPos) |
                                 ((brgb.Blue>>Dest->prvColourFormat.BlueShift) << Dest->prvColourFormat.BluePos);
            DITHER_ERROR(brgb.Red, &RGB16::Red, x, y);
            DITHER_ERROR(brgb.Green, &RGB16::Green, x, y);
            DITHER_ERROR(brgb.Blue, &RGB16::Blue, x, y);
         }
      }
      else if (Dest->BytesPerPixel IS 4) {
         for (x=0; x < Width-1; x++,data+=4,buffer++) {
            brgb.Red   = (buffer->Red>>6) & rmask;
            brgb.Green = (buffer->Green>>6) & gmask;
            brgb.Blue  = (buffer->Blue>>6) & bmask;
            ((uint32_t *)data)[0] = Dest->packPixelWB(brgb.Red, brgb.Green, brgb.Blue, buffer->Alpha);
            DITHER_ERROR(brgb.Red, &RGB16::Red, x, y);
            DITHER_ERROR(brgb.Green, &RGB16::Green, x, y);
            DITHER_ERROR(brgb.Blue, &RGB16::Blue, x, y);
         }
      }
      else {
         for (x=0; x < Width - 1; x++,data+=Dest->BytesPerPixel,buffer++) {
            brgb.Red   = (buffer->Red>>6) & rmask;
            brgb.Green = (buffer->Green>>6) & gmask;
            brgb.Blue  = (buffer->Blue>>6) & bmask;
            Dest->DrawUCRIndex(Dest, data, &brgb);
            DITHER_ERROR(brgb.Red, &RGB16::Red, x, y);
            DITHER_ERROR(brgb.Green, &RGB16::Green, x, y);
            DITHER_ERROR(brgb.Blue, &RGB16::Blue, x, y);
         }
      }

      // Draw the last pixel in the row - no downward propagation

      brgb = { uint8_t(buf1[Width-1].Red>>6), uint8_t(buf1[Width-1].Green>>6), uint8_t(buf1[Width-1].Blue>>6), uint8_t(buf1[Width-1].Alpha) };
      Dest->DrawUCRIndex(Dest, destdata + ((Width - 1) * Dest->BytesPerPixel), &brgb);

      srcdata += Bitmap->LineWidth;
      destdata += Dest->LineWidth;
   }

   // Draw the last row of pixels - no leftward propagation

   if (Bitmap != Dest) {
      for (x=0,index=0; x < Width; x++,index+=Dest->BytesPerPixel) {
         brgb = { uint8_t(buf2[x].Red>>6), uint8_t(buf2[x].Green>>6), uint8_t(buf2[x].Blue>>6), uint8_t(buf2[x].Alpha) };
         Dest->DrawUCRIndex(Dest, destdata+index, &brgb);
      }
   }

   return ERR::Okay;
}

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
CopyArea: Copies a rectangular area from one bitmap to another.

This function copies rectangular areas from one bitmap to another.  It performs a straight region-copy only, using the
fastest method available.  Bitmaps may be of a different type (e.g. bit depth), however this will result in performance
penalties.  The copy process will respect the clipping region defined in both the source and destination bitmap
objects.

If the `TRANSPARENT` flag is set in the source object, all colours that match the @Bitmap.TransIndex field will be
ignored in the copy operation.

To enable dithering, pass `BAF::DITHER` in the Flags parameter.  The drawing algorithm will use dithering if the source
needs to be down-sampled to the target bitmap's bit depth.  To enable alpha blending, set `BAF::BLEND` (the source bitmap
will also need to have the `BMF::ALPHA_CHANNEL` flag set to indicate that an alpha channel is available).

The quality of 32-bit alpha blending can be improved by selecting the `BAF::LINEAR` flag.  This enables an additional
computation whereby each RGB value is converted to linear sRGB colour space before performing the blend.  The
discernible value of using this option largely depends on the level of opaqueness of either bitmap.  Note that this
option is not usable if either bitmap is already in a linear colourspace (`ERR::InvalidState` will be returned if that
is the case).

-INPUT-
obj(Bitmap) Bitmap: The source bitmap.
obj(Bitmap) Dest: Pointer to the destination bitmap.
int(BAF) Flags: Optional flags.
int X:      The horizontal position of the area to be copied.
int Y:      The vertical position of the area to be copied.
int Width:  The width of the area.
int Height: The height of the area.
int XDest:  The horizontal position to copy the area to.
int YDest:  The vertical position to copy the area to.

-ERRORS-
Okay:
NullArgs: The `Dest` parameter was not specified.
Mismatch: The destination bitmap is not a close enough match to the source bitmap in order to perform the operation.
InvalidState: The `LINEAR` flag was used when at least one bitmap is using a linear colourspace.
-END-

*********************************************************************************************************************/

uint8_t validate_clip(CSTRING Header, CSTRING Name, extBitmap *Bitmap)
{
   pf::Log log(Header);

#ifndef NDEBUG // Force break if clipping is wrong (use gdb)
   if (((Bitmap->Clip.Right) > Bitmap->Width) or
       ((Bitmap->Clip.Bottom) > Bitmap->Height) or
       ((Bitmap->Clip.Left) < 0) or
       (Bitmap->Clip.Left >= Bitmap->Clip.Right) or
       (Bitmap->Clip.Top >= Bitmap->Clip.Bottom)) {
      DEBUG_BREAK
   }
#else
   if ((Bitmap->Clip.Right) > Bitmap->Width) {
      log.warning("#%d %s: Invalid right-clip of %d, limited to width of %d.", Bitmap->UID, Name, Bitmap->Clip.Right, Bitmap->Width);
      Bitmap->Clip.Right = Bitmap->Width;
   }

   if ((Bitmap->Clip.Bottom) > Bitmap->Height) {
      log.warning("#%d %s: Invalid bottom-clip of %d, limited to height of %d.", Bitmap->UID, Name, Bitmap->Clip.Bottom, Bitmap->Height);
      Bitmap->Clip.Bottom = Bitmap->Height;
   }

   if ((Bitmap->Clip.Left) < 0) {
      log.warning("#%d %s: Invalid left-clip of %d.", Bitmap->UID, Name, Bitmap->Clip.Left);
      Bitmap->Clip.Left = 0;
   }

   if ((Bitmap->Clip.Top) < 0) {
      log.warning("#%d %s: Invalid top-clip of %d.", Bitmap->UID, Name, Bitmap->Clip.Top);
      Bitmap->Clip.Top = 0;
   }

   if (Bitmap->Clip.Left >= Bitmap->Clip.Right) {
      log.warning("#%d %s: Left clip >= Right clip (%d >= %d)", Bitmap->UID, Name, Bitmap->Clip.Left, Bitmap->Clip.Right);
      return 1;
   }

   if (Bitmap->Clip.Top >= Bitmap->Clip.Bottom) {
      log.warning("#%d %s: Top clip >= Bottom clip (%d >= %d)", Bitmap->UID, Name, Bitmap->Clip.Top, Bitmap->Clip.Bottom);
      return 1;
   }
#endif

   return 0;
}

ERR CopyArea(objBitmap *Source, objBitmap *Dest, BAF Flags, int X, int Y, int Width, int Height, int DestX, int DestY)
{
   pf::Log log(__FUNCTION__);
   RGB8 pixel, srgb;
   uint8_t *srctable, *desttable;
   int i;
   uint32_t colour;
   uint8_t *data, *srcdata;

   if (!Dest) return ERR::NullArgs;
   if (Dest->classID() != CLASSID::BITMAP) {
      log.warning("Destination #%d is not a Bitmap.", Dest->UID);
      return ERR::InvalidObject;
   }

   auto src = (extBitmap *)Source;
   auto dest = (extBitmap *)Dest;
   if (!src->initialised()) return log.warning(ERR::NotInitialised);

   //log.trace("%dx%d,%dx%d to %dx%d", X, Y, Width, Height, DestX, DestY);

   if (validate_clip(__FUNCTION__, "Source", src)) return ERR::Okay;

   if (Source != Dest) { // Validate the clipping region of the destination
      if (validate_clip(__FUNCTION__, "Dest", (extBitmap *)Dest)) return ERR::Okay;
   }

   if ((Flags & BAF::LINEAR) != BAF::NIL) {
      if ((src->ColourSpace IS CS::LINEAR_RGB) or (Dest->ColourSpace IS CS::LINEAR_RGB)) return log.warning(ERR::InvalidState);
      if ((src->BitsPerPixel != 32) or ((src->Flags & BMF::ALPHA_CHANNEL) IS BMF::NIL)) return log.warning(ERR::InvalidState);
   }

   if (Source IS Dest) { // Use this clipping routine only if we are copying within the same bitmap
      if (X < src->Clip.Left) {
         Width = Width - (src->Clip.Left - X);
         DestX = DestX + (src->Clip.Left - X);
         X = src->Clip.Left;
      }
      else if (X >= src->Clip.Right) {
         log.trace("Clipped: X >= Bitmap->ClipRight (%d >= %d)", X, src->Clip.Right);
         return ERR::Okay;
      }

      if (Y < src->Clip.Top) {
         Height = Height - (src->Clip.Top - Y);
         DestY  = DestY + (src->Clip.Top - Y);
         Y = src->Clip.Top;
      }
      else if (Y >= src->Clip.Bottom) {
         log.trace("Clipped: Y >= Bitmap->ClipBottom (%d >= %d)", Y, src->Clip.Bottom);
         return ERR::Okay;
      }

      // Clip the destination coordinates

      if ((DestX < Dest->Clip.Left)) {
         Width = Width - (Dest->Clip.Left - DestX);
         if (Width < 1) return ERR::Okay;
         X = X + (Dest->Clip.Left - DestX);
         DestX = Dest->Clip.Left;
      }
      else if (DestX >= Dest->Clip.Right) {
         log.trace("Clipped: DestX >= RightClip (%d >= %d)", DestX, Dest->Clip.Right);
         return ERR::Okay;
      }

      if ((DestY < Dest->Clip.Top)) {
         Height = Height - (Dest->Clip.Top - DestY);
         if (Height < 1) return ERR::Okay;
         Y = Y + (Dest->Clip.Top - DestY);
         DestY = Dest->Clip.Top;
      }
      else if (DestY >= Dest->Clip.Bottom) {
         log.trace("Clipped: DestY >= BottomClip (%d >= %d)", DestY, Dest->Clip.Bottom);
         return ERR::Okay;
      }

      // Clip the Width and Height

      if ((DestX + Width)   >= src->Clip.Right)  Width  = src->Clip.Right - DestX;
      if ((DestY + Height)  >= src->Clip.Bottom) Height = src->Clip.Bottom - DestY;

      if ((X + Width)  >= src->Clip.Right)  Width  = src->Clip.Right - X;
      if ((Y + Height) >= src->Clip.Bottom) Height = src->Clip.Bottom - Y;
   }
   else {
      // Check if the destination that we are copying to is within the drawable area.

      if (DestX < Dest->Clip.Left) {
         Width = Width - (Dest->Clip.Left - DestX);
         if (Width < 1) return ERR::Okay;
         X = X + (Dest->Clip.Left - DestX);
         DestX = Dest->Clip.Left;
      }
      else if (DestX >= Dest->Clip.Right) return ERR::Okay;

      if (DestY < Dest->Clip.Top) {
         Height = Height - (Dest->Clip.Top - DestY);
         if (Height < 1) return ERR::Okay;
         Y = Y + (Dest->Clip.Top - DestY);
         DestY = Dest->Clip.Top;
      }
      else if (DestY >= Dest->Clip.Bottom) return ERR::Okay;

      // Check if the source that we are copying from is within its own drawable area.

      if (X < src->Clip.Left) {
         DestX += (src->Clip.Left - X);
         Width = Width - (src->Clip.Left - X);
         if (Width < 1) return ERR::Okay;
         X = src->Clip.Left;
      }
      else if (X >= src->Clip.Right) return ERR::Okay;

      if (Y < src->Clip.Top) {
         DestY += (src->Clip.Top - Y);
         Height = Height - (src->Clip.Top - Y);
         if (Height < 1) return ERR::Okay;
         Y = src->Clip.Top;
      }
      else if (Y >= src->Clip.Bottom) return ERR::Okay;

      // Clip the Width and Height of the source area, based on the imposed clip region.

      if ((DestX + Width)  >= Dest->Clip.Right) Width   = Dest->Clip.Right - DestX;
      if ((DestY + Height) >= Dest->Clip.Bottom) Height = Dest->Clip.Bottom - DestY;
      if ((X + Width)  >= src->Clip.Right)  Width  = src->Clip.Right - X;
      if ((Y + Height) >= src->Clip.Bottom) Height = src->Clip.Bottom - Y;
   }

   if (Width < 1) return ERR::Okay;
   if (Height < 1) return ERR::Okay;

#ifdef _WIN32
   if (dest->win.Drawable) { // Destination is a window

      if (src->win.Drawable) { // Both the source and destination are window areas
         int error;
         if ((error = winBlit(dest->win.Drawable, DestX, DestY, Width, Height, src->win.Drawable, X, Y))) {
            char buffer[80];
            buffer[0] = 0;
            winGetError(error, buffer, sizeof(buffer));
            log.warning("BitBlt(): %s", buffer);
         }
      }
      else { // The source is a software image
         if (((Flags & BAF::BLEND) != BAF::NIL) and (src->BitsPerPixel IS 32) and ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
            uint32_t *srcdata;
            uint8_t destred, destgreen, destblue, red, green, blue, alpha;

            // 32-bit alpha blending is enabled

            srcdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));

            while (Height > 0) {
               for (i=0; i < Width; i++) {
                  alpha = 255 - CFUnpackAlpha(&src->prvColourFormat, srcdata[i]);

                  if (alpha >= BLEND_MAX_THRESHOLD) {
                     red   = srcdata[i] >> src->prvColourFormat.RedPos;
                     green = srcdata[i] >> src->prvColourFormat.GreenPos;
                     blue  = srcdata[i] >> src->prvColourFormat.BluePos;
                     SetPixelV(dest->win.Drawable, DestX+i, DestY, (blue<<16) | (green<<8) | red);
                  }
                  else if (alpha >= BLEND_MIN_THRESHOLD) {
                     colour = GetPixel(dest->win.Drawable, DestX+i, DestY);
                     destred   = colour & 0xff;
                     destgreen = (colour>>8) & 0xff;
                     destblue  = (colour>>16) & 0xff;
                     red   = srcdata[i] >> src->prvColourFormat.RedPos;
                     green = srcdata[i] >> src->prvColourFormat.GreenPos;
                     blue  = srcdata[i] >> src->prvColourFormat.BluePos;
                     red   = destred   + (((red   - destred)   * alpha)>>8);
                     green = destgreen + (((green - destgreen) * alpha)>>8);
                     blue  = destblue  + (((blue  - destblue)  * alpha)>>8);
                     SetPixelV(dest->win.Drawable, DestX+i, DestY, (blue<<16) | (green<<8) | red);
                  }
               }
               srcdata = (uint32_t *)(((uint8_t *)srcdata) + src->LineWidth);
               DestY++;
               Height--;
            }
         }
         else if ((src->Flags & BMF::TRANSPARENT) != BMF::NIL) {
            uint32_t wincolour;
            while (Height > 0) {
               for (i=0; i < Width; i++) {
                  colour = src->ReadUCPixel(src, X + i, Y);
                  if (colour != (uint32_t)src->TransIndex) {
                     wincolour = src->unpackRed(colour);
                     wincolour |= src->unpackGreen(colour)<<8;
                     wincolour |= src->unpackBlue(colour)<<16;
                     SetPixelV(dest->win.Drawable, DestX + i, DestY, wincolour);
                  }
               }
               Y++; DestY++;
               Height--;
            }
         }
         else  {
            winSetDIBitsToDevice(dest->win.Drawable, DestX, DestY, Width, Height, X, Y,
               src->Width, src->Height, src->BitsPerPixel, src->Data,
               src->ColourFormat->RedMask   << src->ColourFormat->RedPos,
               src->ColourFormat->GreenMask << src->ColourFormat->GreenPos,
               src->ColourFormat->BlueMask  << src->ColourFormat->BluePos);
         }
      }

      return ERR::Okay;
   }

#elif __xwindows__

   // Use this routine if the destination is a pixmap (write only memory).  X11 windows are always represented as pixmaps.

   if (((Dest->Flags & BMF::X11_DGA) != BMF::NIL) and (glDGAAvailable) and (Dest != Source)) {
      // We have direct access to the graphics address, so drop through to the software routine
      Dest->Data = (uint8_t *)glDGAVideo;
   }
   else if (dest->x11.drawable) {
      if (!src->x11.drawable) {
         if (((Flags & BAF::BLEND) != BAF::NIL) and (src->BitsPerPixel IS 32) and ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
            auto save_clip = dest->Clip;
            Dest->Clip.Left   = DestX;
            Dest->Clip.Right  = DestX + Width;
            Dest->Clip.Top    = DestY;
            Dest->Clip.Bottom = DestY + Height;
            if (lock_surface(dest, SURFACE_READ|SURFACE_WRITE) IS ERR::Okay) {
               auto srcdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));

               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     uint8_t alpha = 255 - src->unpackAlpha(srcdata[i]);

                     if (alpha >= BLEND_MAX_THRESHOLD) {
                        pixel.Red   = (uint8_t)(srcdata[i] >> src->prvColourFormat.RedPos);
                        pixel.Green = (uint8_t)(srcdata[i] >> src->prvColourFormat.GreenPos);
                        pixel.Blue  = (uint8_t)(srcdata[i] >> src->prvColourFormat.BluePos);
                        dest->DrawUCRPixel(dest, DestX+i, DestY, &pixel);
                     }
                     else if (alpha >= BLEND_MIN_THRESHOLD) {
                        dest->ReadUCRPixel(dest, DestX+i, DestY, &pixel);
                        pixel.Red   += ((((uint8_t)(srcdata[i] >> src->prvColourFormat.RedPos)   - pixel.Red)   * alpha)>>8);
                        pixel.Green += ((((uint8_t)(srcdata[i] >> src->prvColourFormat.GreenPos) - pixel.Green) * alpha)>>8);
                        pixel.Blue  += ((((uint8_t)(srcdata[i] >> src->prvColourFormat.BluePos)  - pixel.Blue)  * alpha)>>8);
                        dest->DrawUCRPixel(dest, DestX+i, DestY, &pixel);
                     }
                  }
                  srcdata = (uint32_t *)(((uint8_t *)srcdata) + src->LineWidth);
                  DestY++;
                  Height--;
               }
               unlock_surface(dest);
            }
            dest->Clip = save_clip;
         }
         else if ((src->Flags & BMF::TRANSPARENT) != BMF::NIL) {
            while (Height > 0) {
               for (auto i=0; i < Width; i++) {
                  colour = src->ReadUCPixel(src, X + i, Y);
                  if (colour != (uint32_t)src->TransIndex) dest->DrawUCPixel(dest, DestX + i, DestY, colour);
               }
               Y++; DestY++;
               Height--;
            }
         }
         else { // Source is an ximage, destination is a pixmap
            if ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) src->premultiply();

            if (src->x11.XShmImage IS true)  {
               XShmPutImage(XDisplay, dest->x11.drawable, dest->getGC(), &src->x11.ximage, X, Y, DestX, DestY, Width, Height, False);
            }
            else XPutImage(XDisplay, dest->x11.drawable, dest->getGC(), &src->x11.ximage, X, Y, DestX, DestY, Width, Height);

            if ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) { // Composite window
               XSync(XDisplay, False);
            }
            else XClearWindow(XDisplay, dest->x11.window); // 'Clear' the window to the pixmap background

            if ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) src->demultiply();
         }
      }
      else { // Both the source and the destination are pixmaps
         XCopyArea(XDisplay, src->x11.drawable, dest->x11.drawable, dest->getGC(), X, Y, Width, Height, DestX, DestY);
      }

      return ERR::Okay;
   }

#elif _GLES_

   if ((dest->DataFlags & MEM::VIDEO) != MEM::NIL) { // Destination is the video display.
      if ((src->DataFlags & MEM::VIDEO) != MEM::NIL) { // Source is the video display.
         // No simple way to support this in OpenGL - we have to copy the display into a texture buffer, then copy the texture back to the display.

         ERR error;
         if (!lock_graphics_active(__func__)) {
            GLuint texture;
            if (alloc_texture(src->Width, src->Height, &texture) IS GL_NO_ERROR) {
               //glViewport(0, 0, src->Width, src->Height);  // Set viewport so it matches texture size of ^2
               glCopyTexImage2D(GL_TEXTURE_2D, 0, src->prvGLPixel, 0, 0, src->Width, src->Height, 0); // Copy screen to texture
               //glViewport(0, 0, src->Width, src->Height);  // Restore viewport to display size
               glDrawTexiOES(DestX, -DestY, 1, src->Width, src->Height);
               glBindTexture(GL_TEXTURE_2D, 0);
               eglSwapBuffers(glEGLDisplay, glEGLSurface);
               glDeleteTextures(1, &texture);
               error = ERR::Okay;
            }
            else error = log.warning(ERR::OpenGL);

            unlock_graphics();
         }
         else error = ERR::LockFailed;

         return error;
      }
      else if ((src->DataFlags & MEM::TEXTURE) != MEM::NIL) {
         // Texture-to-video blitting (


      }
      else {
         // RAM-to-video blitting.  We have to allocate a temporary texture, copy the data to it and then blit that to the display.

         ERR error;
         if (!lock_graphics_active(__func__)) {
            GLuint texture;
            if (alloc_texture(src->Width, src->Height, &texture) IS GL_NO_ERROR) {
               glTexImage2D(GL_TEXTURE_2D, 0, src->prvGLPixel, src->Width, src->Height, 0, src->prvGLPixel, src->prvGLFormat, src->Data); // Copy the bitmap content to the texture.
               if (glGetError() IS GL_NO_ERROR) {
                  glDrawTexiOES(0, 0, 1, src->Width, src->Height);
                  glBindTexture(GL_TEXTURE_2D, 0);
                  eglSwapBuffers(glEGLDisplay, glEGLSurface);
               }
               else error = ERR::OpenGL;

               glDeleteTextures(1, &texture);
               error = ERR::Okay;
            }
            else error = log.warning(ERR::OpenGL);

            unlock_graphics();
         }
         else error = ERR::LockFailed;

         return error;
      }
   }

#endif

   // GENERIC SOFTWARE BLITTING ROUTINES

   if (((Flags & BAF::BLEND) != BAF::NIL) and (src->BitsPerPixel IS 32) and ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
      // 32-bit alpha blending support

      if (lock_surface(src, SURFACE_READ) IS ERR::Okay) {
         if (lock_surface(dest, SURFACE_WRITE) IS ERR::Okay) {
            uint8_t red, green, blue, *dest_lookup;
            uint16_t alpha;

            dest_lookup = glAlphaLookup.data() + (255<<8);

            if (dest->BitsPerPixel IS 32) { // Both bitmaps are 32 bit
               const uint8_t sA = src->ColourFormat->AlphaPos>>3;
               const uint8_t sR = src->ColourFormat->RedPos>>3;
               const uint8_t sG = src->ColourFormat->GreenPos>>3;
               const uint8_t sB = src->ColourFormat->BluePos>>3;
               const uint8_t dA = dest->ColourFormat->AlphaPos>>3;
               const uint8_t dR = dest->ColourFormat->RedPos>>3;
               const uint8_t dG = dest->ColourFormat->GreenPos>>3;
               const uint8_t dB = dest->ColourFormat->BluePos>>3;

               uint8_t *sdata = src->Data + (Y * src->LineWidth) + (X<<2);
               uint8_t *ddata = dest->Data + (DestY * dest->LineWidth) + (DestX<<2);

               if ((Flags & BAF::COPY) != BAF::NIL) { // Avoids blending in cases where the destination pixel is zero alpha.
                  for (int y=0; y < Height; y++) {
                     uint8_t *sp = sdata, *dp = ddata;
                     if ((Flags & BAF::LINEAR) != BAF::NIL) {
                        for (int x=0; x < Width; x++) {
                           if (dp[dA]) {
                              if (sp[sA] IS 0xff) ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];
                              else if (auto a = sp[sA]) {
                                 auto slR = glLinearRGB.convert(sp[sR]);
                                 auto slG = glLinearRGB.convert(sp[sG]);
                                 auto slB = glLinearRGB.convert(sp[sB]);

                                 auto dlR = glLinearRGB.convert(dp[dR]);
                                 auto dlG = glLinearRGB.convert(dp[dG]);
                                 auto dlB = glLinearRGB.convert(dp[dB]);

                                 const uint8_t ca = 0xff - a;

                                 dp[dR] = glLinearRGB.invert(((slR * a) + (dlR * ca) + 0xff)>>8);
                                 dp[dG] = glLinearRGB.invert(((slG * a) + (dlG * ca) + 0xff)>>8);
                                 dp[dB] = glLinearRGB.invert(((slB * a) + (dlB * ca) + 0xff)>>8);
                                 dp[dA] = 0xff - ((ca * (0xff - dp[dA]))>>8);
                              }
                           }
                           else ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];

                           sp += 4;
                           dp += 4;
                        }
                     }
                     else {
                        for (int x=0; x < Width; x++) {
                           if (dp[dA]) {
                              if (sp[sA] IS 0xff) ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];
                              else if (auto a = sp[sA]) {
                                 const uint8_t ca = 0xff - a;
                                 dp[dR] = ((sp[sR] * a) + (dp[dR] * ca) + 0xff)>>8;
                                 dp[dG] = ((sp[sG] * a) + (dp[dG] * ca) + 0xff)>>8;
                                 dp[dB] = ((sp[sB] * a) + (dp[dB] * ca) + 0xff)>>8;
                                 dp[dA] = 0xff - ((ca * (0xff - dp[dA]))>>8);
                              }
                           }
                           else ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];

                           sp += 4;
                           dp += 4;
                        }
                     }
                     sdata += src->LineWidth;
                     ddata += dest->LineWidth;
                  }
               }
               else {
                  while (Height > 0) {
                     uint8_t *sp = sdata, *dp = ddata;
                     if (src->Opacity IS 0xff) {
                        if ((Flags & BAF::LINEAR) != BAF::NIL) {
                           for (i=0; i < Width; i++) {
                              if (sp[sA] IS 0xff) ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];
                              else if (auto a = sp[sA]) {
                                 auto slR = glLinearRGB.convert(sp[sR]);
                                 auto slG = glLinearRGB.convert(sp[sG]);
                                 auto slB = glLinearRGB.convert(sp[sB]);

                                 auto dlR = glLinearRGB.convert(dp[dR]);
                                 auto dlG = glLinearRGB.convert(dp[dG]);
                                 auto dlB = glLinearRGB.convert(dp[dB]);

                                 const uint8_t ca = 0xff - a;

                                 dp[dR] = glLinearRGB.invert(((slR * a) + (dlR * ca) + 0xff)>>8);
                                 dp[dG] = glLinearRGB.invert(((slG * a) + (dlG * ca) + 0xff)>>8);
                                 dp[dB] = glLinearRGB.invert(((slB * a) + (dlB * ca) + 0xff)>>8);
                                 dp[dA] = 0xff - ((ca * (0xff - dp[dA]))>>8);
                              }

                              sp += 4;
                              dp += 4;
                           }
                        }
                        else {
                           for (i=0; i < Width; i++) {
                              if (sp[sA] IS 0xff) ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];
                              else if (auto a = sp[sA]) {
                                 const uint8_t ca = 0xff - a;
                                 dp[dR] = ((sp[sR] * a) + (dp[dR] * ca) + 0xff)>>8;
                                 dp[dG] = ((sp[sG] * a) + (dp[dG] * ca) + 0xff)>>8;
                                 dp[dB] = ((sp[sB] * a) + (dp[dB] * ca) + 0xff)>>8;
                                 dp[dA] = 0xff - ((ca * (0xff - dp[dA]))>>8);
                              }

                              sp += 4;
                              dp += 4;
                           }
                        }
                     }
                     else if ((Flags & BAF::LINEAR) != BAF::NIL) {
                        for (i=0; i < Width; i++) {
                           if (auto a = sp[sA]) {
                              a = (a * src->Opacity + 0xff)>>8;
                              auto slR = glLinearRGB.convert(sp[sR]);
                              auto slG = glLinearRGB.convert(sp[sG]);
                              auto slB = glLinearRGB.convert(sp[sB]);

                              auto dlR = glLinearRGB.convert(dp[dR]);
                              auto dlG = glLinearRGB.convert(dp[dG]);
                              auto dlB = glLinearRGB.convert(dp[dB]);

                              const uint8_t ca = 0xff - a;

                              dp[dR] = glLinearRGB.invert(((slR * a) + (dlR * ca) + 0xff)>>8);
                              dp[dG] = glLinearRGB.invert(((slG * a) + (dlG * ca) + 0xff)>>8);
                              dp[dB] = glLinearRGB.invert(((slB * a) + (dlB * ca) + 0xff)>>8);
                              dp[dA] = 0xff - ((ca * (0xff - dp[dA]))>>8);
                           }

                           sp += 4;
                           dp += 4;
                        }
                     }
                     else {
                        for (i=0; i < Width; i++) {
                           if (auto oa = sp[sA]) {
                              const uint8_t a = (oa * src->Opacity + 0xff)>>8;
                              const uint8_t ca = 0xff - a;
                              dp[dR] = ((sp[sR] * a) + (dp[dR] * ca) + 0xff)>>8;
                              dp[dG] = ((sp[sG] * a) + (dp[dG] * ca) + 0xff)>>8;
                              dp[dB] = ((sp[sB] * a) + (dp[dB] * ca) + 0xff)>>8;
                              dp[dA] = 0xff - ((ca * (0xff - dp[dA]))>>8);
                           }

                           sp += 4;
                           dp += 4;
                        }
                     }
                     sdata += src->LineWidth;
                     ddata += dest->LineWidth;
                     Height--;
                  }
               }
            }
            else if (dest->BytesPerPixel IS 2) {
               uint16_t *ddata;
               uint32_t *sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
               ddata = (uint16_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<1));
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = sdata[i];
                     alpha = ((uint8_t)(colour >> src->prvColourFormat.AlphaPos));
                     alpha = (glAlphaLookup.data() + (alpha<<8))[src->Opacity]<<8; // Multiply the source pixel by overall translucency level

                     if (alpha >= BLEND_MAX_THRESHOLD<<8) {
                        ddata[i] = dest->packPixel((uint8_t)(colour >> src->prvColourFormat.RedPos),
                                                   (uint8_t)(colour >> src->prvColourFormat.GreenPos),
                                                   (uint8_t)(colour >> src->prvColourFormat.BluePos));
                     }
                     else if (alpha >= BLEND_MIN_THRESHOLD<<8) {
                        red   = colour >> src->prvColourFormat.RedPos;
                        green = colour >> src->prvColourFormat.GreenPos;
                        blue  = colour >> src->prvColourFormat.BluePos;
                        srctable  = glAlphaLookup.data() + (alpha);
                        desttable = dest_lookup - (alpha);
                        ddata[i] = dest->packPixel((uint8_t)(srctable[red]   + desttable[dest->unpackRed(ddata[i])]),
                                                   (uint8_t)(srctable[green] + desttable[dest->unpackGreen(ddata[i])]),
                                                   (uint8_t)(srctable[blue]  + desttable[dest->unpackBlue(ddata[i])]));
                     }
                  }
                  sdata = (uint32_t *)(((uint8_t *)sdata) + src->LineWidth);
                  ddata = (uint16_t *)(((uint8_t *)ddata) + dest->LineWidth);
                  Height--;
               }
            }
            else {
               uint32_t *sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = sdata[i];
                     alpha = ((uint8_t)(colour >> src->prvColourFormat.AlphaPos));
                     alpha = (glAlphaLookup.data() + (alpha<<8))[src->Opacity]; // Multiply the source pixel by overall translucency level

                     if (alpha >= BLEND_MAX_THRESHOLD) {
                        pixel.Red   = colour >> src->prvColourFormat.RedPos;
                        pixel.Green = colour >> src->prvColourFormat.GreenPos;
                        pixel.Blue  = colour >> src->prvColourFormat.BluePos;
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                     else if (alpha >= BLEND_MIN_THRESHOLD) {
                        red   = colour >> src->prvColourFormat.RedPos;
                        green = colour >> src->prvColourFormat.GreenPos;
                        blue  = colour >> src->prvColourFormat.BluePos;

                        srctable  = glAlphaLookup.data() + (alpha<<8);
                        desttable = glAlphaLookup.data() + ((255-alpha)<<8);

                        dest->ReadUCRPixel(dest, DestX + i, DestY, &pixel);
                        pixel.Red   = srctable[red]   + desttable[pixel.Red];
                        pixel.Green = srctable[green] + desttable[pixel.Green];
                        pixel.Blue  = srctable[blue]  + desttable[pixel.Blue];
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                  }
                  sdata = (uint32_t *)(((uint8_t *)sdata) + src->LineWidth);
                  DestY++;
                  Height--;
               }
            }

            unlock_surface(dest);
         }
         unlock_surface(src);
      }

      return ERR::Okay;
   }
   else if ((src->Flags & BMF::TRANSPARENT) != BMF::NIL) {
      // Transparent colour copying.  In this mode, the alpha component of individual source pixels is ignored

      if (lock_surface(src, SURFACE_READ) IS ERR::Okay) {
         if (lock_surface(dest, SURFACE_WRITE) IS ERR::Okay) {
            if (src->Opacity < 255) { // Transparent mask with translucent pixels (consistent blend level)
               srctable  = glAlphaLookup.data() + (src->Opacity<<8);
               desttable = glAlphaLookup.data() + ((255-src->Opacity)<<8);
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = src->ReadUCPixel(src, X + i, Y);
                     if (colour != (uint32_t)src->TransIndex) {
                        dest->ReadUCRPixel(dest, DestX + i, DestY, &pixel);

                        pixel.Red   = srctable[src->unpackRed(colour)]   + desttable[pixel.Red];
                        pixel.Green = srctable[src->unpackGreen(colour)] + desttable[pixel.Green];
                        pixel.Blue  = srctable[src->unpackBlue(colour)]  + desttable[pixel.Blue];

                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                  }
                  Y++; DestY++;
                  Height--;
               }
            }
            else if (src->BitsPerPixel IS dest->BitsPerPixel) {
               if (src->BytesPerPixel IS 4) {
                  uint32_t *ddata, *sdata;

                  sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
                  ddata = (uint32_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<2));
                  colour = src->TransIndex;
                  while (Height > 0) {
                     for (i=0; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                     ddata = (uint32_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint32_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else if (src->BytesPerPixel IS 2) {
                  uint16_t *ddata, *sdata;

                  sdata = (uint16_t *)(src->Data + (Y * src->LineWidth) + (X<<1));
                  ddata = (uint16_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<1));
                  colour = src->TransIndex;
                  while (Height > 0) {
                     for (i=0; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                     ddata = (uint16_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint16_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else {
                  while (Height > 0) {
                     for (int i=0; i < Width; i++) {
                        colour = src->ReadUCPixel(src, X + i, Y);
                        if (colour != (uint32_t)src->TransIndex) dest->DrawUCPixel(dest, DestX + i, DestY, colour);
                     }
                     Y++; DestY++;
                     Height--;
                  }
               }
            }
            else if (src->BitsPerPixel IS 8) {
               while (Height > 0) {
                  for (int i=0; i < Width; i++) {
                     colour = src->ReadUCPixel(src, X + i, Y);
                     if (colour != (uint32_t)src->TransIndex) {
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &src->Palette->Col[colour]);
                     }
                  }
                  Y++; DestY++;
                  Height--;
               }
            }
            else while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  src->ReadUCRPixel(src, X + i, Y, &pixel);
                  if ((pixel.Red != src->TransColour.Red) or (pixel.Green != src->TransColour.Green) or (pixel.Blue != src->TransColour.Blue)) {
                     dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                  }
               }
               Y++; DestY++;
               Height--;
            }

            unlock_surface(dest);
         }
         unlock_surface(src);
      }

      return ERR::Okay;
   }
   else { // Straight copy operation
      if (lock_surface(src, SURFACE_READ) IS ERR::Okay) {
         if (lock_surface(dest, SURFACE_WRITE) IS ERR::Okay) {
            if (src->Opacity < 255) { // Translucent draw
               srctable  = glAlphaLookup.data() + (src->Opacity<<8);
               desttable = glAlphaLookup.data() + ((255-src->Opacity)<<8);

               if ((src->BytesPerPixel IS 4) and (dest->BytesPerPixel IS 4)) {
                  uint32_t *ddata, *sdata;
                  uint32_t cmp_alpha;

                  sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
                  ddata = (uint32_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<2));
                  cmp_alpha = 255 << src->prvColourFormat.AlphaPos;
                  while (Height > 0) {
                     for (i=0; i < Width; i++) {
                        ddata[i] = ((srctable[(uint8_t)(sdata[i]>>src->prvColourFormat.RedPos)]   + desttable[(uint8_t)(ddata[i]>>dest->prvColourFormat.RedPos)]) << dest->prvColourFormat.RedPos) |
                                   ((srctable[(uint8_t)(sdata[i]>>src->prvColourFormat.GreenPos)] + desttable[(uint8_t)(ddata[i]>>dest->prvColourFormat.GreenPos)]) << dest->prvColourFormat.GreenPos) |
                                   ((srctable[(uint8_t)(sdata[i]>>src->prvColourFormat.BluePos)]  + desttable[(uint8_t)(ddata[i]>>dest->prvColourFormat.BluePos)]) << dest->prvColourFormat.BluePos) |
                                   cmp_alpha;
                     }
                     ddata = (uint32_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint32_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else if ((src->BytesPerPixel IS 2) and (dest->BytesPerPixel IS 2)) {
                  uint16_t *ddata, *sdata;

                  sdata = (uint16_t *)(src->Data + (Y * src->LineWidth) + (X<<1));
                  ddata = (uint16_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<1));
                  while (Height > 0) {
                     for (i=0; i < Width; i++) {
                        ddata[i] = dest->packPixel(srctable[src->unpackRed(sdata[i])]   + desttable[dest->unpackRed(ddata[i])],
                                                   srctable[src->unpackGreen(sdata[i])] + desttable[dest->unpackGreen(ddata[i])],
                                                   srctable[src->unpackBlue(sdata[i])]  + desttable[dest->unpackBlue(ddata[i])]);
                     }
                     ddata = (uint16_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint16_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     src->ReadUCRPixel(src, X + i, Y, &srgb);
                     dest->ReadUCRPixel(dest, DestX + i, DestY, &pixel);

                     pixel.Red   = srctable[srgb.Red]   + desttable[pixel.Red];
                     pixel.Green = srctable[srgb.Green] + desttable[pixel.Green];
                     pixel.Blue  = srctable[srgb.Blue]  + desttable[pixel.Blue];

                     dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                  }
                  Y++; DestY++;
                  Height--;
               }
            }
            else if (src->BitsPerPixel IS dest->BitsPerPixel) {
               // Use this fast routine for identical bitmaps

               srcdata = src->Data + (X * src->BytesPerPixel) + (Y * src->LineWidth);
               data    = dest->Data + (DestX  * dest->BytesPerPixel) + (DestY * dest->LineWidth);
               Width   = Width * src->BytesPerPixel;

               if ((src IS dest) and (DestY >= Y) and (DestY < Y+Height)) {
                  // Copy backwards when we are copying within the same bitmap and there is an overlap.

                  srcdata += src->LineWidth * (Height-1);
                  data    += dest->LineWidth * (Height-1);

                  while (Height > 0) {
                     for (i=Width-1; i >= 0; i--) data[i] = srcdata[i];
                     srcdata -= src->LineWidth;
                     data    -= dest->LineWidth;
                     Height--;
                  }
               }
               else {
                  while (Height > 0) {
                     for (i=0; (size_t)i > sizeof(int); i += sizeof(int)) {
                        ((int *)(data+i))[0] = ((int *)(srcdata+i))[0];
                     }
                     while (i < Width) { data[i] = srcdata[i]; i++; }
                     srcdata += src->LineWidth;
                     data    += dest->LineWidth;
                     Height--;
                  }
               }
            }
            else {
               // If the bitmaps do not match then we need to use this slower RGB translation subroutine.

               bool dithered = false;
               if ((Flags & BAF::DITHER) != BAF::NIL) {
                  if ((dest->BitsPerPixel < 24) and
                      ((src->BitsPerPixel > dest->BitsPerPixel) or
                       ((src->BitsPerPixel <= 8) and (dest->BitsPerPixel > 8)))) {
                     if ((src->Flags & BMF::TRANSPARENT) != BMF::NIL);
                     else {
                        dither(src, dest, nullptr, Width, Height, X, Y, DestX, DestY);
                        dithered = TRUE;
                     }
                  }
               }

               if (dithered IS false) {
                  if ((src IS dest) and (DestY >= Y) and (DestY < Y+Height)) {
                     while (Height > 0) {
                        Y += Height - 1;
                        DestY  += Height - 1;
                        for (i=0; i < Width; i++) {
                           src->ReadUCRPixel(src, X + i, Y, &pixel);
                           dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                        }
                        Y--; DestY--;
                        Height--;
                     }
                  }
                  else {
                     while (Height > 0) {
                        for (i=0; i < Width; i++) {
                           src->ReadUCRPixel(src, X + i, Y, &pixel);
                           dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                        }
                        Y++; DestY++;
                        Height--;
                     }
                  }
               }
            }

            unlock_surface(dest);
         }
         unlock_surface(src);
      }

      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FUNCTION-
CopyRawBitmap: Copies graphics data from an arbitrary surface to a bitmap.

This function will copy data from a described surface to a destination bitmap object.  You are required to provide the
function with a full description of the source in a !BitmapSurface structure.

The `X`, `Y`, `Width` and `Height` parameters define the area from the source that you wish to copy.  The `XDest` and
`YDest` parameters define the top left corner that you will blit the graphics to in the destination.

-INPUT-
struct(*BitmapSurface) Surface: Description of the surface source.
obj(Bitmap) Dest: Destination bitmap.
int(CSRF) Flags:  Optional flags.
int X:      Horizontal source coordinate.
int Y:      Vertical source coordinate.
int Width:  Source width.
int Height: Source height.
int XDest:  Horizontal destination coordinate.
int YDest:  Vertical destination coordinate.

-ERRORS-
Okay:
Args:
NullArgs:

*********************************************************************************************************************/

template <class INT> uint8_t UnpackSRed(BITMAPSURFACE *S, INT C)  { return (((C >> S->Format.RedPos)   & S->Format.RedMask) << S->Format.RedShift); }
template <class INT> uint8_t UnpackSGreen(BITMAPSURFACE *S,INT C) { return (((C >> S->Format.GreenPos) & S->Format.GreenMask) << S->Format.GreenShift); }
template <class INT> uint8_t UnpackSBlue(BITMAPSURFACE *S, INT C) { return (((C >> S->Format.BluePos)  & S->Format.BlueMask) << S->Format.BlueShift); }
template <class INT> uint8_t UnpackSAlpha(BITMAPSURFACE *S,INT C) { return (((C >> S->Format.AlphaPos) & S->Format.AlphaMask)); }

static uint32_t read_surface8(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   return ((uint8_t *)Surface->Data)[(Surface->LineWidth * Y) + X];
}

static uint32_t read_surface16(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   return ((uint16_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + X + X))[0];
}

static uint32_t read_surface_lsb24(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   uint8_t *data;
   data = (uint8_t *)Surface->Data + (Surface->LineWidth * Y) + (X + X + X);
   return (data[2]<<16) | (data[1]<<8) | data[0];
}

static uint32_t read_surface_msb24(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   uint8_t *data;
   data = (uint8_t *)Surface->Data + (Surface->LineWidth * Y) + (X + X + X);
   return (data[0]<<16) | (data[1]<<8) | data[2];
}

static uint32_t read_surface32(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   return ((uint32_t *)((uint8_t *)Surface->Data + (Surface->LineWidth * Y) + (X<<2)))[0];
}

ERR CopyRawBitmap(BITMAPSURFACE *Surface, objBitmap *Bitmap, CSRF Flags, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   pf::Log log(__FUNCTION__);
   RGB8 pixel, src;
   uint8_t *srctable, *desttable;
   int i;
   int16_t srcwidth;
   uint32_t colour;
   uint8_t *data, *srcdata;
   uint32_t (*read_surface)(BITMAPSURFACE *, int16_t, int16_t);

   if ((!Surface) or (!Bitmap)) return log.warning(ERR::NullArgs);

   if ((!Surface->Data) or (Surface->LineWidth < 1) or (!Surface->BitsPerPixel)) {
      return log.warning(ERR::Args);
   }

   auto dest = (extBitmap *)Bitmap;
   srcwidth = Surface->LineWidth / Surface->BytesPerPixel;

   // Check if the destination that we are copying to is within the drawable area.

   if ((XDest < Bitmap->Clip.Left)) {
      Width = Width - (Bitmap->Clip.Left - X);
      if (Width < 1) return ERR::Okay;
      X = X + (Bitmap->Clip.Left - X);
      XDest = Bitmap->Clip.Left;
   }
   else if (XDest >= Bitmap->Clip.Right) return ERR::Okay;

   if ((YDest < Bitmap->Clip.Top)) {
      Height = Height - (Bitmap->Clip.Top - YDest);
      if (Height < 1) return ERR::Okay;
      Y = Y + (Bitmap->Clip.Top - YDest);
      YDest = Bitmap->Clip.Top;
   }
   else if (YDest >= Bitmap->Clip.Bottom) return ERR::Okay;

   // Check if the source that we are blitting from is within its own drawable area.

   if ((Flags & CSRF::CLIP) != CSRF::NIL) {
      if (X < 0) {
         if ((Width += X) < 1) return ERR::Okay;
         X = 0;
      }
      else if (X >= srcwidth) return ERR::Okay;

      if (Y < 0) {
         if ((Height += Y) < 1) return ERR::Okay;
         Y = 0;
      }
      else if (Y >= Surface->Height) return ERR::Okay;
   }

   // Clip the width and height

   if ((XDest + Width)  >= Bitmap->Clip.Right)  Width  = Bitmap->Clip.Right - XDest;
   if ((YDest + Height) >= Bitmap->Clip.Bottom) Height = Bitmap->Clip.Bottom - YDest;

   if ((Flags & CSRF::CLIP) != CSRF::NIL) {
      if ((X + Width)  >= Surface->Clip.Right)  Width  = Surface->Clip.Right - X;
      if ((Y + Height) >= Surface->Clip.Bottom) Height = Surface->Clip.Bottom - Y;
   }

   if (Width < 1) return ERR::Okay;
   if (Height < 1) return ERR::Okay;

   // Adjust coordinates by offset values

   if ((Flags & CSRF::OFFSET) != CSRF::NIL) {
      X += Surface->XOffset;
      Y += Surface->YOffset;
   }

   if ((Flags & CSRF::DEFAULT_FORMAT) != CSRF::NIL) gfx::GetColourFormat(&Surface->Format, Surface->BitsPerPixel, 0, 0, 0, 0);;

   switch(Surface->BytesPerPixel) {
      case 1: read_surface = read_surface8; break;
      case 2: read_surface = read_surface16; break;
      case 3: if (Surface->Format.RedPos IS 16) read_surface = read_surface_lsb24;
              else read_surface = read_surface_msb24;
              break;
      case 4: read_surface = read_surface32; break;
      default: return log.warning(ERR::Args);
   }

#ifdef __xwindows__

   // Use this routine if the destination is a pixmap (write only memory).  X11 windows are always represented as pixmaps.

   if (dest->x11.drawable) {
      // Source is an ximage, destination is a pixmap.  NB: If DGA is enabled, we will avoid using these routines because mem-copying from software
      // straight to video RAM is a lot faster.

      int16_t alignment;

      if (dest->LineWidth & 0x0001) alignment = 8;
      else if (dest->LineWidth & 0x0002) alignment = 16;
      else alignment = 32;

      XImage ximage;
      ximage.width            = Surface->LineWidth / Surface->BytesPerPixel;
      ximage.height           = Surface->Height;
      ximage.xoffset          = 0;               // Number of pixels offset in X direction
      ximage.format           = ZPixmap;         // XYBitmap, XYPixmap, ZPixmap
      ximage.data             = (char *)Surface->Data;
      ximage.byte_order       = LSBFirst;        // LSBFirst / MSBFirst
      ximage.bitmap_unit      = alignment;       // Quant. of scanline - 8, 16, 32
      ximage.bitmap_bit_order = LSBFirst;        // LSBFirst / MSBFirst
      ximage.bitmap_pad       = alignment;       // 8, 16, 32, either XY or Zpixmap
      if ((Surface->BitsPerPixel IS 32) and ((dest->Flags & BMF::ALPHA_CHANNEL) IS BMF::NIL)) ximage.depth = 24;
      else ximage.depth = Surface->BitsPerPixel;
      ximage.bytes_per_line   = Surface->LineWidth;
      ximage.bits_per_pixel   = Surface->BytesPerPixel * 8;
      ximage.red_mask         = 0;
      ximage.green_mask       = 0;
      ximage.blue_mask        = 0;
      XInitImage(&ximage);

      XPutImage(XDisplay, dest->x11.drawable, dest->getGC(),
         &ximage, X, Y, XDest, YDest, Width, Height);

      return ERR::Okay;
   }

#endif // __xwindows__

   if (lock_surface((extBitmap *)Bitmap, SURFACE_WRITE) IS ERR::Okay) {
      if (((Flags & CSRF::ALPHA) != CSRF::NIL) and (Surface->BitsPerPixel IS 32)) { // 32-bit alpha blending support
         uint32_t *sdata = (uint32_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<2));

         if (Bitmap->BitsPerPixel IS 32) {
            uint32_t *ddata = (uint32_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<2));
            while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = sdata[i];

                  uint8_t alpha = ((uint8_t)(colour >> Surface->Format.AlphaPos));
                  alpha = (glAlphaLookup.data() + (alpha<<8))[Surface->Opacity]; // Multiply the source pixel by overall translucency level

                  if (alpha >= BLEND_MAX_THRESHOLD) ddata[i] = colour;
                  else if (alpha >= BLEND_MIN_THRESHOLD) {

                     uint8_t red   = colour >> Surface->Format.RedPos;
                     uint8_t green = colour >> Surface->Format.GreenPos;
                     uint8_t blue  = colour >> Surface->Format.BluePos;

                     colour = ddata[i];
                     uint8_t destred   = colour >> ((extBitmap *)Bitmap)->prvColourFormat.RedPos;
                     uint8_t destgreen = colour >> ((extBitmap *)Bitmap)->prvColourFormat.GreenPos;
                     uint8_t destblue  = colour >> ((extBitmap *)Bitmap)->prvColourFormat.BluePos;

                     srctable  = glAlphaLookup.data() + (alpha<<8);
                     desttable = glAlphaLookup.data() + ((255-alpha)<<8);
                     ddata[i] = ((extBitmap *)Bitmap)->packPixelWB(srctable[red] + desttable[destred],
                                                  srctable[green] + desttable[destgreen],
                                                  srctable[blue] + desttable[destblue]);
                  }
               }
               sdata = (uint32_t *)(((uint8_t *)sdata) + Surface->LineWidth);
               ddata = (uint32_t *)(((uint8_t *)ddata) + Bitmap->LineWidth);
               Height--;
            }
         }
         else while (Height > 0) {
            for (int i=0; i < Width; i++) {
               colour = sdata[i];
               uint8_t alpha = ((uint8_t)(colour >> Surface->Format.AlphaPos));
               alpha = (glAlphaLookup.data() + (alpha<<8))[Surface->Opacity]; // Multiply the source pixel by overall translucency level

               if (alpha >= BLEND_MAX_THRESHOLD) {
                  pixel.Red   = colour >> Surface->Format.RedPos;
                  pixel.Green = colour >> Surface->Format.GreenPos;
                  pixel.Blue  = colour >> Surface->Format.BluePos;
                  Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
               }
               else if (alpha >= BLEND_MIN_THRESHOLD) {
                  uint8_t red   = colour >> Surface->Format.RedPos;
                  uint8_t green = colour >> Surface->Format.GreenPos;
                  uint8_t blue  = colour >> Surface->Format.BluePos;

                  srctable  = glAlphaLookup.data() + (alpha<<8);
                  desttable = glAlphaLookup.data() + ((255-alpha)<<8);

                  Bitmap->ReadUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  pixel.Red   = srctable[red]   + desttable[pixel.Red];
                  pixel.Green = srctable[green] + desttable[pixel.Green];
                  pixel.Blue  = srctable[blue]  + desttable[pixel.Blue];
                  Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
               }
            }
            sdata = (uint32_t *)(((uint8_t *)sdata) + Surface->LineWidth);
            YDest++;
            Height--;
         }
      }
      else if ((Flags & CSRF::TRANSPARENT) != CSRF::NIL) {
         // Transparent colour blitting

         if (((Flags & CSRF::TRANSLUCENT) != CSRF::NIL) and (Surface->Opacity < 255)) {
            // Transparent mask with translucent pixels

            srctable  = glAlphaLookup.data() + (Surface->Opacity<<8);
            desttable = glAlphaLookup.data() + ((255-Surface->Opacity)<<8);

            while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  if (colour != (uint32_t)Surface->Colour) {
                     Bitmap->ReadUCRPixel(Bitmap, XDest + i, YDest, &pixel);

                     pixel.Red   = srctable[UnpackSRed(Surface, colour)]   + desttable[pixel.Red];
                     pixel.Green = srctable[UnpackSGreen(Surface, colour)] + desttable[pixel.Green];
                     pixel.Blue  = srctable[UnpackSBlue(Surface, colour)]  + desttable[pixel.Blue];

                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
               }
               Y++; YDest++;
               Height--;
            }
         }
         else if (Surface->BitsPerPixel IS Bitmap->BitsPerPixel) {
            if (Surface->BytesPerPixel IS 4) {
               uint32_t *sdata = (uint32_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<2));
               uint32_t *ddata = (uint32_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<2));
               colour = Surface->Colour;
               while (Height > 0) {
                  for (i=0; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                  ddata = (uint32_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint32_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else if (Surface->BytesPerPixel IS 2) {
               uint16_t *ddata, *sdata;

               sdata = (uint16_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<1));
               ddata = (uint16_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<1));
               colour = Surface->Colour;
               while (Height > 0) {
                  for (i=0; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                  ddata = (uint16_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint16_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else {
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = read_surface(Surface, X + i, Y);
                     if (colour != (uint32_t)Surface->Colour) Bitmap->DrawUCPixel(Bitmap, XDest + i, YDest, colour);
                  }
                  Y++; YDest++;
                  Height--;
               }
            }
         }
         else {
            while (Height > 0) {
               for (i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  if (colour != (uint32_t)Surface->Colour) {
                     pixel.Red   = UnpackSRed(Surface, colour);
                     pixel.Green = UnpackSGreen(Surface, colour);
                     pixel.Blue  = UnpackSBlue(Surface, colour);
                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
               }
               Y++; YDest++;
               Height--;
            }
         }
      }
      else { // Straight copy operation
         if (((Flags & CSRF::TRANSLUCENT) != CSRF::NIL) and (Surface->Opacity < 255)) { // Straight translucent blit
            srctable  = glAlphaLookup.data() + (Surface->Opacity<<8);
            desttable = glAlphaLookup.data() + ((255-Surface->Opacity)<<8);

            if ((Surface->BytesPerPixel IS 4) and (Bitmap->BytesPerPixel IS 4)) {
               uint32_t *ddata, *sdata;

               sdata = (uint32_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<2));
               ddata = (uint32_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<2));
               while (Height > 0) {
                  for (int i=0; i < Width; i++) {
                     ddata[i] = ((srctable[(uint8_t)(sdata[i]>>Surface->Format.RedPos)]   + desttable[(uint8_t)(ddata[i]>>dest->prvColourFormat.RedPos)]) << dest->prvColourFormat.RedPos) |
                                ((srctable[(uint8_t)(sdata[i]>>Surface->Format.GreenPos)] + desttable[(uint8_t)(ddata[i]>>dest->prvColourFormat.GreenPos)]) << dest->prvColourFormat.GreenPos) |
                                ((srctable[(uint8_t)(sdata[i]>>Surface->Format.BluePos)]  + desttable[(uint8_t)(ddata[i]>>dest->prvColourFormat.BluePos)]) << dest->prvColourFormat.BluePos);
                  }
                  ddata = (uint32_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint32_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else if ((Surface->BytesPerPixel IS 2) and (Bitmap->BytesPerPixel IS 2)) {
               uint16_t *ddata, *sdata;

               sdata = (uint16_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<1));
               ddata = (uint16_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<1));
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     ddata[i] = Bitmap->packPixel(srctable[UnpackSRed(Surface, sdata[i])] + desttable[Bitmap->unpackRed(ddata[i])],
                                                  srctable[UnpackSGreen(Surface, sdata[i])] + desttable[Bitmap->unpackGreen(ddata[i])],
                                                  srctable[UnpackSBlue(Surface, sdata[i])] + desttable[Bitmap->unpackBlue(ddata[i])]);
                  }
                  ddata = (uint16_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint16_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  src.Red   = UnpackSRed(Surface, colour);
                  src.Green = UnpackSGreen(Surface, colour);
                  src.Blue  = UnpackSBlue(Surface, colour);

                  Bitmap->ReadUCRPixel(Bitmap, XDest + i, YDest, &pixel);

                  pixel.Red   = srctable[src.Red]   + desttable[pixel.Red];
                  pixel.Green = srctable[src.Green] + desttable[pixel.Green];
                  pixel.Blue  = srctable[src.Blue]  + desttable[pixel.Blue];

                  Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
               }
               Y++; YDest++;
               Height--;
            }
         }
         else if (Surface->BitsPerPixel IS Bitmap->BitsPerPixel) {
            // Use this fast routine for identical bitmaps

            srcdata = (uint8_t *)Surface->Data + (X * Surface->BytesPerPixel) + (Y * Surface->LineWidth);
            data    = Bitmap->Data + (XDest  * Bitmap->BytesPerPixel) + (YDest * Bitmap->LineWidth);
            Width   = Width * Surface->BytesPerPixel;

            while (Height > 0) {
               for (i=0; (size_t)i > sizeof(int); i += sizeof(int)) {
                  ((int *)(data+i))[0] = ((int *)(srcdata+i))[0];
               }
               while (i < Width) { data[i] = srcdata[i]; i++; }
               srcdata += Surface->LineWidth;
               data    += Bitmap->LineWidth;
               Height--;
            }
         }
         else {
            // If the bitmaps do not match then we need to use this slower RGB translation subroutine.

            while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  src.Red   = UnpackSRed(Surface, colour);
                  src.Green = UnpackSGreen(Surface, colour);
                  src.Blue  = UnpackSBlue(Surface, colour);
                  Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &src);
               }
               Y++; YDest++;
               Height--;
            }
         }
      }

      unlock_surface((extBitmap *)Bitmap);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
DrawRectangle: Draws rectangles, both filled and unfilled.

This function draws both filled and unfilled rectangles.  The rectangle is drawn to the target bitmap at position
(X, Y) with dimensions determined by the specified `Width` and `Height`.  If the `Flags` parameter defines `BAF::FILL` then
the rectangle will be filled, otherwise only the outline will be drawn.  The colour of the rectangle is determined by
the pixel value in the `Colour` parameter.  Alpha blending is not supported.

-INPUT-
obj(Bitmap) Bitmap: Pointer to the target @Bitmap.
int X:       The left-most coordinate of the rectangle.
int Y:       The top-most coordinate of the rectangle.
int Width:   The width of the rectangle.
int Height:  The height of the rectangle.
uint Colour: The colour value to use for the rectangle.
int(BAF) Flags: Use `FILL` to fill the rectangle.

*********************************************************************************************************************/

void DrawRectangle(objBitmap *Target, int X, int Y, const int Width, const int Height, uint32_t Colour, BAF Flags)
{
   pf::Log log(__FUNCTION__);
   uint8_t *data;
   uint16_t *word;
   uint32_t *longdata;
   int xend, x, EX, EY, i;

   auto Bitmap = (extBitmap *)Target;
   if (!Bitmap) return;

   // If we are not going to fill the rectangle, use this routine to draw an outline.

   if (((Flags & BAF::FILL) IS BAF::NIL) and (Width > 1) and (Height > 1)) {
      EX = X + Width - 1;
      EY = Y + Height - 1;
      if (X >= Bitmap->Clip.Left) gfx::DrawRectangle(Bitmap, X, Y, 1, Height, Colour, Flags|BAF::FILL); // Left
      if (Y >= Bitmap->Clip.Top)  gfx::DrawRectangle(Bitmap, X, Y, Width, 1, Colour, Flags|BAF::FILL); // Top
      if (Y + Height <= Bitmap->Clip.Bottom) gfx::DrawRectangle(Bitmap, X, EY, Width, 1, Colour, Flags|BAF::FILL); // Bottom
      if (X + Width <= Bitmap->Clip.Right)   gfx::DrawRectangle(Bitmap, X+Width-1, Y, 1, Height, Colour, Flags|BAF::FILL);
      return;
   }

   if (!Bitmap->initialised()) { log.warning(ERR::NotInitialised); return; }

   if (X >= Bitmap->Clip.Right) return;
   if (Y >= Bitmap->Clip.Bottom) return;
   if (X + Width <= Bitmap->Clip.Left) return;
   if (Y + Height <= Bitmap->Clip.Top) return;

   auto w = Width;
   auto h = Height;
   if (X < Bitmap->Clip.Left) {
      w -= Bitmap->Clip.Left - X;
      X = Bitmap->Clip.Left;
   }

   if (Y < Bitmap->Clip.Top) {
      h -= Bitmap->Clip.Top - Y;
      Y = Bitmap->Clip.Top;
   }

   if ((X + w) >= Bitmap->Clip.Right)   w = Bitmap->Clip.Right - X;
   if ((Y + h) >= Bitmap->Clip.Bottom) h = Bitmap->Clip.Bottom - Y;

   uint16_t red   = Bitmap->unpackRed(Colour);
   uint16_t green = Bitmap->unpackGreen(Colour);
   uint16_t blue  = Bitmap->unpackBlue(Colour);

   // Translucent rectangle support

   // Standard rectangle (no translucency) video support

   #ifdef _GLES_
      if ((Bitmap->DataFlags & MEM::VIDEO) != MEM::NIL) {
      log.warning("TODO: Draw rectangles to opengl");
         glClearColor(0.5, 0.5, 0.5, 1.0);
         glClear(GL_COLOR_BUFFER_BIT);
         return;
      }
   #endif

   #ifdef _WIN32
      if (Bitmap->win.Drawable) {
         winDrawRectangle(Bitmap->win.Drawable, X, Y, w, h, red, green, blue);
         return;
      }
   #endif

   #ifdef __xwindows__
      if ((Bitmap->DataFlags & (MEM::VIDEO|MEM::TEXTURE)) != MEM::NIL) {
         XSetForeground(XDisplay, Bitmap->getGC(), Colour);
         XFillRectangle(XDisplay, Bitmap->x11.drawable, Bitmap->getGC(), X, Y, w, h);
         return;
      }
   #endif

   // Standard rectangle data support

   if (lock_surface(Bitmap, SURFACE_WRITE) IS ERR::Okay) {
      if (!Bitmap->Data) {
         unlock_surface(Bitmap);
         return;
      }

      if (Bitmap->Type IS BMP::CHUNKY) {
         if (Bitmap->BitsPerPixel IS 32) {
            longdata = (uint32_t *)(Bitmap->Data + (Bitmap->LineWidth * Y));
            while (h > 0) {
               for (x=X; x < (X+w); x++) longdata[x] = Colour;
               longdata = (uint32_t *)(((uint8_t *)longdata) + Bitmap->LineWidth);
               h--;
            }
         }
         else if (Bitmap->BitsPerPixel IS 24) {
            data = Bitmap->Data + (Bitmap->LineWidth * Y);
            X = X + X + X;
            xend = X + w + w + w;
            while (h > 0) {
               for (x=X; x < xend;) {
                  data[x++] = blue; data[x++] = green; data[x++] = red;
               }
               data += Bitmap->LineWidth;
               h--;
            }
         }
         else if ((Bitmap->BitsPerPixel IS 16) or (Bitmap->BitsPerPixel IS 15)) {
            word = (uint16_t *)(Bitmap->Data + (Bitmap->LineWidth * Y));
            xend = X + w;
            while (h > 0) {
               for (x=X; x < xend; x++) word[x] = (uint16_t)Colour;
               word = (uint16_t *)(((int8_t *)word) + Bitmap->LineWidth);
               h--;
            }
         }
         else if (Bitmap->BitsPerPixel IS 8) {
            data = Bitmap->Data + (Bitmap->LineWidth * Y);
            xend = X + w;
            while (h > 0) {
               for (x=X; x < xend;) data[x++] = Colour;
               data += Bitmap->LineWidth;
               h--;
            }
         }
         else while (h > 0) {
            for (i=X; i < X + w; i++) Bitmap->DrawUCPixel(Bitmap, i, Y, Colour);
            Y++;
            h--;
         }
      }
      else while (h > 0) {
         for (i=X; i < X + w; i++) Bitmap->DrawUCPixel(Bitmap, i, Y, Colour);
         Y++;
         h--;
      }

      unlock_surface(Bitmap);
   }

   return;
}

/*********************************************************************************************************************

-FUNCTION-
DrawRGBPixel: Draws a 24 bit pixel to a @Bitmap.

This function draws an !RGB8 colour to the `(X, Y)` position of a target @Bitmap.  The function will check the given
coordinates to ensure that the pixel is inside the bitmap's clipping area.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap object.
int X: Horizontal coordinate of the pixel.
int Y: Vertical coordinate of the pixel.
struct(*RGB8) RGB: The colour to be drawn, in RGB format.

*********************************************************************************************************************/

void DrawRGBPixel(objBitmap *Bitmap, int X, int Y, RGB8 *Pixel)
{
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left)) return;
   if ((Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) return;
   Bitmap->DrawUCRPixel(Bitmap, X, Y, Pixel);
}

/*********************************************************************************************************************

-FUNCTION-
DrawPixel: Draws a single pixel to a bitmap.

This function draws a pixel to the coordinates `(X, Y)` on a bitmap with a colour determined by the `Colour` index.
This function will check the given coordinates to make sure that the pixel is inside the bitmap's clipping area.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap object.
int X: The horizontal coordinate of the pixel.
int Y: The vertical coordinate of the pixel.
uint Colour: The colour value to use for the pixel.

*********************************************************************************************************************/

void DrawPixel(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left)) return;
   if ((Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) return;
   Bitmap->DrawUCPixel(Bitmap, X, Y, Colour);
}

/*********************************************************************************************************************

-FUNCTION-
GetColourFormat: Generates the values for a !ColourFormat structure for a given bit depth.

This function will generate the values for a !ColourFormat structure, for either a given bit depth or
customised colour bit values.  The !ColourFormat structure is used by internal bitmap routines to pack and unpack bit
values to and from bitmap memory.

The !ColourFormat structure is supported by the following macros for packing and unpacking colour bit values:

<pre>
Colour = CFPackPixel(Format,Red,Green,Blue)
Colour = CFPackPixelA(Format,Red,Green,Blue,Alpha)
Colour = CFPackAlpha(Format,Alpha)
Red    = CFUnpackRed(Format,Colour)
Green  = CFUnpackGreen(Format,Colour)
Blue   = CFUnpackBlue(Format,Colour)
Alpha  = CFUnpackAlpha(Format,Colour)
</pre>

-INPUT-
struct(*ColourFormat) Format: Pointer to an empty !ColourFormat structure.
int BitsPerPixel: The depth that you would like to generate colour values for.  Ignored if mask values are set.
int RedMask:      Red component bit mask value.  Set this value to zero if the `BitsPerPixel` parameter is used.
int GreenMask:    Green component bit mask value.
int BlueMask:     Blue component bit mask value.
int AlphaMask:    Alpha component bit mask value.

*********************************************************************************************************************/

void GetColourFormat(ColourFormat *Format, int BPP, int RedMask, int GreenMask, int BlueMask, int AlphaMask)
{
   int mask;

   //log.function("R: $%.8x G: $%.8x, B: $%.8x, A: $%.8x", RedMask, GreenMask, BlueMask, AlphaMask);

   if (!RedMask) {
      if (BPP IS 15) {
         RedMask   = 0x7c00;
         GreenMask = 0x03e0;
         BlueMask  = 0x001f;
         AlphaMask = 0x0000;
      }
      else if (BPP IS 16) {
         RedMask   = 0xf800;
         GreenMask = 0x07e0;
         BlueMask  = 0x001f;
         AlphaMask = 0x0000;
      }
      else {
         BPP = 32;
         AlphaMask = 0xff000000;
         RedMask   = 0x00ff0000;
         GreenMask = 0x0000ff00;
         BlueMask  = 0x000000ff;
      }
   }

   // Calculate the lower byte mask and the position (left shift) of the colour

   mask = RedMask;
   Format->RedPos = 0;
   Format->RedShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->RedPos++; }
   Format->RedMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->RedMask)); mask=mask>>1) Format->RedShift++;

   mask = BlueMask;
   Format->BluePos = 0;
   Format->BlueShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->BluePos++; }
   Format->BlueMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->BlueMask)); mask=mask>>1) Format->BlueShift++;

   mask = GreenMask;
   Format->GreenPos = 0;
   Format->GreenShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->GreenPos++; }
   Format->GreenMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->GreenMask)); mask=mask>>1) Format->GreenShift++;

   mask = AlphaMask;
   Format->AlphaPos = 0;
   Format->AlphaShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->AlphaPos++; }
   Format->AlphaMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->AlphaMask)); mask=mask>>1) Format->AlphaShift++;

   Format->BitsPerPixel = BPP;
}

/*********************************************************************************************************************

-FUNCTION-
ReadRGBPixel: Reads a pixel's colour from the target bitmap.

This function reads a pixel from a bitmap surface and returns the value in an !RGB8 structure that remains good up until
the next call to this function.  Zero is returned in the alpha component if the pixel is out of bounds.

This function is thread-safe if the target @Bitmap is locked.

-INPUT-
obj(Bitmap) Bitmap: Pointer to a bitmap object.
int X: The horizontal coordinate of the pixel.
int Y: The vertical coordinate of the pixel.
&struct(RGB8) RGB: The colour values will be stored in this !RGB8 structure.

*********************************************************************************************************************/

void ReadRGBPixel(objBitmap *Bitmap, int X, int Y, RGB8 **Pixel)
{
   static thread_local RGB8 pixel;
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left) or
       (Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) {
      pixel.Red = 0; pixel.Green = 0; pixel.Blue = 0; pixel.Alpha = 0;
   }
   else {
      pixel.Alpha = 255;
      Bitmap->ReadUCRPixel(Bitmap, X, Y, &pixel);
   }
   *Pixel = &pixel;
}

/*********************************************************************************************************************

-FUNCTION-
ReadPixel: Reads a pixel's colour from the target bitmap.

This function reads a pixel from a bitmap area and returns its colour index (if the @Bitmap is indexed with a palette)
or its packed pixel value.  Zero is returned if the pixel is out of bounds.

-INPUT-
obj(Bitmap) Bitmap: Pointer to a bitmap object.
int X: The horizontal coordinate of the pixel.
int Y: The vertical coordinate of the pixel.

-RESULT-
uint: The colour value of the pixel will be returned.  Zero is returned if the pixel is out of bounds.

*********************************************************************************************************************/

uint32_t ReadPixel(objBitmap *Bitmap, int X, int Y)
{
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left) or
       (Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) return 0;
   else return Bitmap->ReadUCPixel(Bitmap, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
Resample: Resamples a bitmap by dithering it to a new set of colour masks.

The Resample() function provides a means for resampling a bitmap to a new colour format without changing the actual
bit depth of the image. It uses dithering so as to retain the quality of the image when down-sampling.  This function
is generally used to 'pre-dither' true colour bitmaps in preparation for copying to bitmaps with lower colour quality.

You are required to supply a !ColourFormat structure that describes the colour format that you would like to apply to
the bitmap's image data.

-INPUT-
obj(Bitmap) Bitmap: The bitmap object to be resampled.
struct(*ColourFormat) ColourFormat: The new colour format to be applied to the bitmap.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR Resample(objBitmap *Bitmap, ColourFormat *Format)
{
   if ((!Bitmap) or (!Format)) return ERR::NullArgs;

   dither((extBitmap *)Bitmap, (extBitmap *)Bitmap, Format, Bitmap->Width, Bitmap->Height, 0, 0, 0, 0);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetClipRegion: Sets a clipping region for a bitmap object.

The SetClipRegion() method is used to manage the clipping regions assigned to a bitmap object.  Each new bitmap that is
created has at least one clip region assigned to it, but by using SetClipRegion() you can also define multiple clipping
areas, which is useful for complex graphics management.

Each clipping region that you set is assigned a Number, starting from zero which is the default.  Each time that you
set a new clip region you must specify the number of the region that you wish to set.  If you attempt to 'skip'
regions - for instance, if you set regions 0, 1, 2 and 3, then skip 4 and set 5, the routine will set region 4 instead.
If you have specified multiple clip regions and want to lower the count or reset the list, set the number of the last
region that you want in your list and set the `Terminate` parameter to `true` to kill the regions specified beyond it.

The `ClipLeft`, `ClipTop`, `ClipRight` and `ClipBottom` fields in the target `Bitmap` will be updated to reflect
the overall area that is covered by the clipping regions that have been set.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap.
int Number:    The number of the clip region to set.
int Left:      The horizontal start of the clip region.
int Top:       The vertical start of the clip region.
int Right:     The right-most edge of the clip region.
int Bottom:    The bottom-most edge of the clip region.
int Terminate: Set to `true` if this is the last clip region in the list, otherwise `false`.

*********************************************************************************************************************/

void SetClipRegion(objBitmap *Bitmap, int Number, int Left, int Top, int Right, int Bottom,
   int Terminate)
{
   Bitmap->Clip.Left   = Left;
   Bitmap->Clip.Top    = Top;
   Bitmap->Clip.Right  = Right;
   Bitmap->Clip.Bottom = Bottom;

   if (Bitmap->Clip.Left < 0) Bitmap->Clip.Left = 0;
   if (Bitmap->Clip.Top  < 0) Bitmap->Clip.Top = 0;
   if (Bitmap->Clip.Right  > Bitmap->Width)  Bitmap->Clip.Right = Bitmap->Width;
   if (Bitmap->Clip.Bottom > Bitmap->Height) Bitmap->Clip.Bottom = Bitmap->Height;
}

/*********************************************************************************************************************

-FUNCTION-
Sync: Waits for the completion of all active bitmap operations.

The Sync() function will wait for all current video operations to complete before it returns.  This ensures that it is
safe to write to video memory with the CPU, preventing any possibility of clashes with the onboard graphics chip.

-INPUT-
obj(Bitmap) Bitmap: Pointer to the bitmap that you want to synchronise or `NULL` to sleep on the graphics accelerator.
-END-

*********************************************************************************************************************/

void Sync(objBitmap *Bitmap)
{

}

} // namespace