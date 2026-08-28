#include "x11_native.h"

namespace display {

static GC x11_gc(extBitmap *Bitmap)
{
   auto record = x11_bitmap(Bitmap);
   return record->WindowGraphicsContext ? record->WindowGraphicsContext : record->DefaultGraphicsContext;
}

/*********************************************************************************************************************
** CHUNKY32
*/

static void VideoDrawPixel32(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XSetForeground(record->Connection, x11_gc((extBitmap *)Bitmap), Colour);
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBPixel32(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XSetForeground(record->Connection, x11_gc((extBitmap *)Bitmap), Bitmap->packPixelWB(*RGB));
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBIndex32(objBitmap *Bitmap, uint32_t *Data, RGB8 *RGB)
{

}

static uint32_t VideoReadPixel32(objBitmap *Bitmap, int X, int Y)
{
   auto readable = x11_bitmap((extBitmap *)Bitmap)->Readable;
   return ((uint32_t *)((uint8_t *)readable->data + (readable->bytes_per_line * Y) + (X<<2)))[0];
}

static void VideoReadRGBPixel32(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   auto readable = x11_bitmap((extBitmap *)Bitmap)->Readable;
   uint32_t colour = ((uint32_t *)((uint8_t *)readable->data + (readable->bytes_per_line * Y) + (X<<2)))[0];
   RGB->Red   = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.RedPos);
   RGB->Green = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.GreenPos);
   RGB->Blue  = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.BluePos);
   RGB->Alpha = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.AlphaPos);
}

static void VideoReadRGBIndex32(objBitmap *Bitmap, uint32_t *Data, RGB8 *RGB)
{
   uint32_t colour = Data[0];
   RGB->Red   = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.RedPos);
   RGB->Green = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.GreenPos);
   RGB->Blue  = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.BluePos);
   RGB->Alpha = (uint8_t)(colour >> ((extBitmap *)Bitmap)->prvColourFormat.AlphaPos);
}

/*********************************************************************************************************************
** CHUNKY24
*/

static void VideoDrawPixel24(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XSetForeground(record->Connection, x11_gc((extBitmap *)Bitmap), Colour);
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBPixel24(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   uint32_t Colour = (RGB->Red<<16) | (RGB->Green<<8) | (RGB->Blue);
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XSetForeground(record->Connection, x11_gc((extBitmap *)Bitmap), Colour);
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBIndex24(objBitmap *Bitmap, uint8_t *Data, RGB8 *RGB)
{

}

static uint32_t VideoReadPixel24(objBitmap *Bitmap, int X, int Y)
{
   auto data = (uint8_t *)x11_bitmap((extBitmap *)Bitmap)->Readable->data + (Bitmap->LineWidth * Y) + (X + X + X);
   return (data[2]<<16)|(data[1]<<8)|data[0];
}

static void VideoReadRGBPixel24(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   auto data = (uint8_t *)x11_bitmap((extBitmap *)Bitmap)->Readable->data;
   data += x11_bitmap((extBitmap *)Bitmap)->Readable->bytes_per_line * Y;
   data += X + X + X;
   RGB->Red   = data[2];
   RGB->Green = data[1];
   RGB->Blue  = data[0];
   RGB->Alpha = 0;
}

static void VideoReadRGBIndex24(objBitmap *Bitmap, uint8_t *Data, RGB8 *RGB)
{
   RGB->Red   = Data[2];
   RGB->Green = Data[1];
   RGB->Blue  = Data[0];
   RGB->Alpha = 0;
}

/*********************************************************************************************************************
** CHUNKY16
*/

static void VideoDrawPixel16(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XSetForeground(record->Connection, x11_gc((extBitmap *)Bitmap), Colour);
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBPixel16(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XSetForeground(record->Connection, x11_gc((extBitmap *)Bitmap), Bitmap->packPixel(*RGB));
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBIndex16(objBitmap *Bitmap, uint16_t *Data, RGB8 *RGB)
{

}

static uint32_t VideoReadPixel16(objBitmap *Bitmap, int X, int Y)
{
   auto readable = x11_bitmap((extBitmap *)Bitmap)->Readable;
   return ((uint16_t *)((int8_t *)readable->data + (readable->bytes_per_line * Y) + (X<<1)))[0];
}

static void VideoReadRGBPixel16(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   auto readable = x11_bitmap((extBitmap *)Bitmap)->Readable;
   uint16_t data = ((uint16_t *)((int8_t *)readable->data + (readable->bytes_per_line * Y) + (X<<1)))[0];
   RGB->Red   = Bitmap->unpackRed(data);
   RGB->Green = Bitmap->unpackGreen(data);
   RGB->Blue  = Bitmap->unpackBlue(data);
   RGB->Alpha = 0;
}

static void VideoReadRGBIndex16(objBitmap *Bitmap, uint16_t *Data, RGB8 *RGB)
{
   RGB->Red   = Bitmap->unpackRed(Data[0]);
   RGB->Green = Bitmap->unpackGreen(Data[0]);
   RGB->Blue  = Bitmap->unpackBlue(Data[0]);
   RGB->Alpha = 0;
}

/*********************************************************************************************************************
** CHUNKY8
*/

static void VideoDrawPixel8(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBPixel8(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   //ULONG colour = RGBToValue(RGB, Bitmap->Palette);
   auto record = x11_bitmap((extBitmap *)Bitmap);
   XDrawPoint(record->Connection, record->DrawableID, x11_gc((extBitmap *)Bitmap), X, Y);
}

static void VideoDrawRGBIndex8(objBitmap *Bitmap, uint8_t *Data, RGB8 *RGB)
{

}

static uint32_t VideoReadPixel8(objBitmap *Bitmap, int X, int Y)
{
   auto readable = x11_bitmap((extBitmap *)Bitmap)->Readable;
   return (readable->data + (readable->bytes_per_line * Y) + X)[0];
}

static void VideoReadRGBPixel8(objBitmap *Bitmap, int X, int Y, RGB8 *RGB)
{
   auto data  = (uint8_t *)x11_bitmap((extBitmap *)Bitmap)->Readable->data;
   auto index = data[(x11_bitmap((extBitmap *)Bitmap)->Readable->bytes_per_line * Y) + X];
   RGB->Red   = Bitmap->Palette->Col[index].Red;
   RGB->Green = Bitmap->Palette->Col[index].Green;
   RGB->Blue  = Bitmap->Palette->Col[index].Blue;
   RGB->Alpha = 0;
}

static void VideoReadRGBIndex8(objBitmap *Bitmap, uint8_t *Data, RGB8 *RGB)
{
   RGB->Red   = Bitmap->Palette->Col[*Data].Red;
   RGB->Green = Bitmap->Palette->Col[*Data].Green;
   RGB->Blue  = Bitmap->Palette->Col[*Data].Blue;
   RGB->Alpha = 0;
}

void x11_install_bitmap_routines(extBitmap *Bitmap)
{
   if (Bitmap->BitsPerPixel IS 32) {
      Bitmap->DrawUCPixel = VideoDrawPixel32; Bitmap->DrawUCRPixel = VideoDrawRGBPixel32;
      Bitmap->DrawUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))VideoDrawRGBIndex32;
      Bitmap->ReadUCPixel = VideoReadPixel32; Bitmap->ReadUCRPixel = VideoReadRGBPixel32;
      Bitmap->ReadUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))VideoReadRGBIndex32;
   }
   else if (Bitmap->BitsPerPixel IS 24) {
      Bitmap->DrawUCPixel = VideoDrawPixel24; Bitmap->DrawUCRPixel = VideoDrawRGBPixel24;
      Bitmap->DrawUCRIndex = VideoDrawRGBIndex24; Bitmap->ReadUCPixel = VideoReadPixel24;
      Bitmap->ReadUCRPixel = VideoReadRGBPixel24; Bitmap->ReadUCRIndex = VideoReadRGBIndex24;
   }
   else if (Bitmap->BitsPerPixel IS 16) {
      Bitmap->DrawUCPixel = VideoDrawPixel16; Bitmap->DrawUCRPixel = VideoDrawRGBPixel16;
      Bitmap->DrawUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))VideoDrawRGBIndex16;
      Bitmap->ReadUCPixel = VideoReadPixel16; Bitmap->ReadUCRPixel = VideoReadRGBPixel16;
      Bitmap->ReadUCRIndex = (void (*)(objBitmap *, uint8_t *, RGB8 *))VideoReadRGBIndex16;
   }
   else {
      Bitmap->DrawUCPixel = VideoDrawPixel8; Bitmap->DrawUCRPixel = VideoDrawRGBPixel8;
      Bitmap->DrawUCRIndex = VideoDrawRGBIndex8; Bitmap->ReadUCPixel = VideoReadPixel8;
      Bitmap->ReadUCRPixel = VideoReadRGBPixel8; Bitmap->ReadUCRIndex = VideoReadRGBIndex8;
   }
}

} // namespace display
