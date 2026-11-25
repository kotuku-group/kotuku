// This source file manages basic pixel drawing routines.  For more pixel routines, see the other pixel
// files in the machine specific directories.

//********************************************************************************************************************
// CHUNKY32

static void MemDrawPixel32(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   ((uint32_t *)((uint8_t *)Bitmap->Data + (Bitmap->LineWidth * Y) + (X<<2)))[0] = Colour;
}

static void MemDrawRGBPixel32(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   ((uint32_t *)((uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + (X<<2)))[0] = Bitmap->packPixelWB(*RGB);
}

static void MemDrawRGBIndex32(objBitmap *Bitmap, uint32_t *Data, struct RGB8 *RGB)
{
   Data[0] = Bitmap->packPixelWB(*RGB);
}

static uint32_t MemReadPixel32(objBitmap *Bitmap, int X, int Y)
{
   return ((uint32_t *)((uint8_t *)Bitmap->Data + (Bitmap->LineWidth * Y) + (X<<2)))[0];
}

static void MemReadRGBPixel32(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint32_t colour = ((uint32_t *)((uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + (X<<2)))[0];
   RGB->Red   = colour >> ((extBitmap *)Bitmap)->prvColourFormat.RedPos;
   RGB->Green = colour >> ((extBitmap *)Bitmap)->prvColourFormat.GreenPos;
   RGB->Blue  = colour >> ((extBitmap *)Bitmap)->prvColourFormat.BluePos;
   RGB->Alpha = colour >> ((extBitmap *)Bitmap)->prvColourFormat.AlphaPos;
}

static void MemReadRGBIndex32(objBitmap *Bitmap, uint32_t *Data, struct RGB8 *RGB)
{
   uint32_t colour = Data[0];
   RGB->Red   = colour >> ((extBitmap *)Bitmap)->prvColourFormat.RedPos;
   RGB->Green = colour >> ((extBitmap *)Bitmap)->prvColourFormat.GreenPos;
   RGB->Blue  = colour >> ((extBitmap *)Bitmap)->prvColourFormat.BluePos;
   RGB->Alpha = colour >> ((extBitmap *)Bitmap)->prvColourFormat.AlphaPos;
}

//********************************************************************************************************************
// CHUNKY24 LSB

static void MemDrawLSBPixel24(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   uint8_t *data = (uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + (X + X + X);
   data[0] = Colour;
   data[1] = Colour>>8;
   data[2] = Colour>>16;
}

static void MemDrawLSBRGBPixel24(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint8_t *data = (uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + (X + X + X);
   data[0] = RGB->Blue;
   data[1] = RGB->Green;
   data[2] = RGB->Red;
}

static void MemDrawLSBRGBIndex24(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   Data[0] = RGB->Blue;
   Data[1] = RGB->Green;
   Data[2] = RGB->Red;
}

static uint32_t MemReadLSBPixel24(objBitmap *Bitmap, int X, int Y)
{
   uint8_t *data = (uint8_t *)Bitmap->Data + (Bitmap->LineWidth * Y) + (X + X + X);
   return (data[2]<<16) | (data[1]<<8) | data[0];
}

static void MemReadLSBRGBPixel24(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint8_t *data = (uint8_t *)Bitmap->Data + (Bitmap->LineWidth * Y) + (X + X + X);
   RGB->Red   = data[2];
   RGB->Green = data[1];
   RGB->Blue  = data[0];
   RGB->Alpha = 255;
}

static void MemReadLSBRGBIndex24(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   RGB->Red   = Data[2];
   RGB->Green = Data[1];
   RGB->Blue  = Data[0];
   RGB->Alpha = 255;
}

//********************************************************************************************************************
// CHUNKY24 MSB

static void MemDrawMSBPixel24(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   uint8_t *Data;

   Data    = (uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + (X + X + X);
   Data[2] = Colour;
   Data[1] = Colour>>8;
   Data[0] = Colour>>16;
}

static void MemDrawMSBRGBPixel24(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint8_t *Data;

   Data    = (uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + (X + X + X);
   Data[2] = RGB->Blue;
   Data[1] = RGB->Green;
   Data[0] = RGB->Red;
}

static void MemDrawMSBRGBIndex24(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   Data[2] = RGB->Blue;
   Data[1] = RGB->Green;
   Data[0] = RGB->Red;
}

static uint32_t MemReadMSBPixel24(objBitmap *Bitmap, int X, int Y)
{
   uint8_t *Data;

   Data = (uint8_t *)Bitmap->Data + (Bitmap->LineWidth * Y) + (X + X + X);
   return (Data[0]<<16) | (Data[1]<<8) | Data[2];
}

static void MemReadMSBRGBPixel24(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint8_t *data;

   data = (uint8_t *)Bitmap->Data + (Bitmap->LineWidth * Y) + (X + X + X);
   RGB->Red   = data[0];
   RGB->Green = data[1];
   RGB->Blue  = data[2];
   RGB->Alpha = 255;
}

static void MemReadMSBRGBIndex24(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   RGB->Red   = Data[0];
   RGB->Green = Data[1];
   RGB->Blue  = Data[2];
   RGB->Alpha = 255;
}

//********************************************************************************************************************
// CHUNKY16

static void MemDrawPixel16(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   ((uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth) + X + X))[0] = Colour;
}

static void MemDrawRGBPixel16(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   ((uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth) + X + X))[0] = Bitmap->packPixel(RGB->Red, RGB->Green, RGB->Blue);
}

static void MemDrawRGBIndex16(objBitmap *Bitmap, uint16_t *Data, struct RGB8 *RGB)
{
   Data[0] = Bitmap->packPixel(RGB->Red, RGB->Green, RGB->Blue);
}

static uint32_t MemReadPixel16(objBitmap *Bitmap, int X, int Y)
{
   return ((uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth) + X + X))[0];
}

static void MemReadRGBPixel16(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint16_t data;
   data = ((uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth) + X + X))[0];
   RGB->Red   = Bitmap->unpackRed(data);
   RGB->Green = Bitmap->unpackGreen(data);
   RGB->Blue  = Bitmap->unpackBlue(data);
   RGB->Alpha = 255;
}

static void MemReadRGBIndex16(objBitmap *Bitmap, uint16_t *Data, struct RGB8 *RGB)
{
   RGB->Red   = Bitmap->unpackRed(Data[0]);
   RGB->Green = Bitmap->unpackGreen(Data[0]);
   RGB->Blue  = Bitmap->unpackBlue(Data[0]);
   RGB->Alpha = 255;
}

//********************************************************************************************************************
// GREY16: BMF::GREYSCALE bitmaps.  Colours are written as 16-bit luminance and read back as the 8-bit grey in the high
// byte.  MemDrawPixel16() and MemReadPixel16() serve for raw values.

static void MemDrawRGBPixelGrey16(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   ((uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth) + X + X))[0] =
      objBitmap::packGrey(RGB->Red, RGB->Green, RGB->Blue);
}

static void MemDrawRGBIndexGrey16(objBitmap *Bitmap, uint16_t *Data, struct RGB8 *RGB)
{
   Data[0] = objBitmap::packGrey(RGB->Red, RGB->Green, RGB->Blue);
}

static void MemReadRGBPixelGrey16(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   const uint8_t grey = objBitmap::unpackGrey(((uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth) + X + X))[0]);
   RGB->Red   = grey;
   RGB->Green = grey;
   RGB->Blue  = grey;
   RGB->Alpha = 255;
}

static void MemReadRGBIndexGrey16(objBitmap *Bitmap, uint16_t *Data, struct RGB8 *RGB)
{
   const uint8_t grey = objBitmap::unpackGrey(Data[0]);
   RGB->Red   = grey;
   RGB->Green = grey;
   RGB->Blue  = grey;
   RGB->Alpha = 255;
}

//********************************************************************************************************************
// CHUNKY8

static void MemDrawPixel8(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   ((uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + X)[0] = Colour;
}

static void MemDrawRGBPixel8(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   ((uint8_t *)Bitmap->Data + (Y * Bitmap->LineWidth) + X)[0] = RGBToValue(RGB, Bitmap->Palette);
}

static void MemDrawRGBIndex8(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   Data[0] = RGBToValue(RGB, Bitmap->Palette);
}

static uint32_t MemReadPixel8(objBitmap *Bitmap, int X, int Y)
{
   return ((uint8_t *)Bitmap->Data)[(Bitmap->LineWidth * Y) + X];
}

static void MemReadRGBPixel8(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint8_t *data   = Bitmap->Data;
   uint8_t colour = data[(Bitmap->LineWidth * Y) + X];
   RGB->Red   = Bitmap->Palette->Col[colour].Red;
   RGB->Green = Bitmap->Palette->Col[colour].Green;
   RGB->Blue  = Bitmap->Palette->Col[colour].Blue;
   RGB->Alpha = 255;
}

static void MemReadRGBIndex8(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   RGB->Red   = Bitmap->Palette->Col[*Data].Red;
   RGB->Green = Bitmap->Palette->Col[*Data].Green;
   RGB->Blue  = Bitmap->Palette->Col[*Data].Blue;
   RGB->Alpha = 255;
}

//********************************************************************************************************************
// PLANAR

static uint32_t MemReadPixelPlanar(objBitmap *Bitmap, int X, int Y)
{
   int XOffset = X % 8;
   uint8_t *Data = Bitmap->Data;
   Data += (Y * Bitmap->LineWidth) + (X>>3);

   uint32_t Colour = 0;
   for (int i=0; i < Bitmap->BitsPerPixel; i++) {
      if (*Data & (0x80>>XOffset)) Colour |= 0x01<<i;
      Data += Bitmap->PlaneMod;
   }

   return Colour;
}

static void MemDrawPixelPlanar(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{

}

static void MemReadRGBPixelPlanar(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   int XOffset = X % 8;
   uint8_t *Data = Bitmap->Data + (Y * Bitmap->LineWidth) + (X>>3);

   uint32_t Colour = 0;
   for (int i = 0; i < Bitmap->BitsPerPixel; i++) {
      if (*Data & (0x80>>XOffset)) Colour |= 0x01<<i;
      Data += Bitmap->PlaneMod;
   }

   RGB->Red   = Bitmap->Palette->Col[Colour].Red;
   RGB->Green = Bitmap->Palette->Col[Colour].Green;
   RGB->Blue  = Bitmap->Palette->Col[Colour].Blue;
   RGB->Alpha = 255;
}

static void MemReadRGBIndexPlanar(objBitmap *Bitmap, uint8_t *Data, struct RGB8 *RGB)
{
   int XOffset = 0;
   uint32_t Colour = 0;
   for (int i=0; i < Bitmap->BitsPerPixel; i++) {
      if (*Data & (0x80>>XOffset)) Colour |= 0x01<<i;
      Data += Bitmap->PlaneMod;
   }

   RGB->Red   = Bitmap->Palette->Col[Colour].Red;
   RGB->Green = Bitmap->Palette->Col[Colour].Green;
   RGB->Blue  = Bitmap->Palette->Col[Colour].Blue;
   RGB->Alpha = 255;
}

static void DrawRGBPixelPlanar(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB)
{
   uint32_t Colour;
   Colour = RGBToValue(RGB, Bitmap->Palette);
   Bitmap->DrawUCPixel(Bitmap, X, Y, Colour);
}
