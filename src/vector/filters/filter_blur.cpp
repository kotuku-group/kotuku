/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
BlurFX: Applies a Gaussian blur effect to an input source.

The BlurFX class performs a Gaussian blur, or approximation thereof, on the input source.  The Gaussian blur kernel
is an approximation of the normalized convolution `G(x,y) = H(x)I(y)` where `H(x) = exp(-x2/ (2s2)) / sqrt(2* pi*s2)`
and `I(y) = exp(-y2/ (2t2)) / sqrt(2* pi*t2)` with 's' being the standard deviation in the x direction and 't' being
the standard deviation in the y direction, as specified by #SX and #SY.

At least one of #SX or #SY should be greater than 0, otherwise no rendering is performed.

The blur is not restricted to whole pixels.  It changes continuously with the standard deviation and the display
scale, so a blur that covers less than a pixel still softens the input.

-END-

W3C: Frequently this operation will take place on alpha-only images, such as that produced by the built-in input,
SourceAlpha.  The implementation may notice this and optimize the single channel case. If the input has infinite
extent and is constant (e.g  FillPaint where the fill is a solid color), this operation has no effect. If the input
has infinite extent and the filter result is the input to an feTile, the filter is evaluated with periodic boundary
conditions.

*********************************************************************************************************************/

template<class T> struct stack_blur_tables
{
  static uint16_t const g_stack_blur8_mul[255];
  static uint8_t  const g_stack_blur8_shr[255];
};

template<class T>
uint16_t const stack_blur_tables<T>::g_stack_blur8_mul[255] =
{
  512,512,456,512,328,456,335,512,405,328,271,456,388,335,292,512,
  454,405,364,328,298,271,496,456,420,388,360,335,312,292,273,512,
  482,454,428,405,383,364,345,328,312,298,284,271,259,496,475,456,
  437,420,404,388,374,360,347,335,323,312,302,292,282,273,265,512,
  497,482,468,454,441,428,417,405,394,383,373,364,354,345,337,328,
  320,312,305,298,291,284,278,271,265,259,507,496,485,475,465,456,
  446,437,428,420,412,404,396,388,381,374,367,360,354,347,341,335,
  329,323,318,312,307,302,297,292,287,282,278,273,269,265,261,512,
  505,497,489,482,475,468,461,454,447,441,435,428,422,417,411,405,
  399,394,389,383,378,373,368,364,359,354,350,345,341,337,332,328,
  324,320,316,312,309,305,301,298,294,291,287,284,281,278,274,271,
  268,265,262,259,257,507,501,496,491,485,480,475,470,465,460,456,
  451,446,442,437,433,428,424,420,416,412,408,404,400,396,392,388,
  385,381,377,374,370,367,363,360,357,354,350,347,344,341,338,335,
  332,329,326,323,320,318,315,312,310,307,304,302,299,297,294,292,
  289,287,285,282,280,278,275,273,271,269,267,265,263,261,259
};

template<class T>
uint8_t const stack_blur_tables<T>::g_stack_blur8_shr[255] =
{
    9, 11, 12, 13, 13, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17,
   17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 18, 19,
   19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 21,
   21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
   21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22,
   22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
   22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23,
   23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
   23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
   23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
   23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
   24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
   24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
   24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
   24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24
};

class extBlurFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::BLURFX;
   static constexpr CSTRING CLASS_NAME = "BlurFX";
   using create = kt::Create<extBlurFX>;

   double SX = 0, SY = 0;

   extBlurFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   // Computes the stack blur radius of each axis in bitmap pixels.  The radius is twice the standard deviation.

   void radii(double &RX, double &RY) const {
      double scale = 1.0;
      if (Filter->ClientVector) scale = Filter->ClientVector->Transform.scale();

      if (Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
         if (Filter->AspectRatio IS VFA::NONE) {
            // Scaling is applied evenly on both axis.  Uses the same formula as a scaled stroke-width.
            double diag = dist(0, 0, Filter->BoundWidth, Filter->BoundHeight) * INV_SQRT2;
            RX = SX * diag * 2 * scale;
            RY = SY * diag * 2 * scale;
         }
         else {
            // Scaling is stretched independently of each axis
            RX = SX * Filter->BoundWidth * 2 * scale;
            RY = SY * Filter->BoundHeight * 2 * scale;
         }
      }
      else {
         RX = SX * 2 * scale;
         RY = SY * 2 * scale;
      }
   }
};

//********************************************************************************************************************
// SSE2 implementation of the stack blur pass.  The algorithm is identical to the scalar version below, but the
// per-channel running sums are held in packed 32-bit lanes so that all four channels advance per operation.  All
// arithmetic is integral, so the output is bit-identical to the scalar path.  Channel byte order is irrelevant
// because every channel is processed independently and identically.  Samples outside of the line are transparent.

#ifdef FILTER_SSE2

inline uint32_t blur_read32(const uint8_t *Pixel)
{
   uint32_t v;
   memcpy(&v, Pixel, 4);
   return v;
}

inline void blur_write32(uint8_t *Pixel, uint32_t Value)
{
   memcpy(Pixel, &Value, 4);
}

// Spread the four bytes of a pixel into 32-bit lanes, and vice versa.  Packing truncates each lane to its lowest
// byte, matching the scalar code's uint8_t assignment.

inline __m128i blur_unpack(uint32_t Pixel)
{
   auto v = _mm_cvtsi32_si128(int(Pixel));
   v = _mm_unpacklo_epi8(v, _mm_setzero_si128());
   return _mm_unpacklo_epi16(v, _mm_setzero_si128());
}

inline uint32_t blur_pack(__m128i Value)
{
   Value = _mm_and_si128(Value, _mm_set1_epi32(0xff));
   Value = _mm_packs_epi32(Value, Value);
   Value = _mm_packus_epi16(Value, Value);
   return uint32_t(_mm_cvtsi128_si32(Value));
}

// 32-bit low multiply; _mm_mullo_epi32 requires SSE4.1 so it is emulated with paired 32x32->64 multiplies.

inline __m128i blur_mullo32(__m128i A, __m128i B)
{
   auto even = _mm_mul_epu32(A, B);
   auto odd  = _mm_mul_epu32(_mm_srli_si128(A, 4), _mm_srli_si128(B, 4));
   return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),
                             _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));
}

// One full stack blur pass over an axis.  The horizontal pass walks lines of `Count` pixels stepping 4 bytes at a
// time; the vertical pass swaps the roles of LineStep and PixelStep so that lines become columns.

static void stack_blur_axis_sse2(const uint8_t *InData, uint8_t *OutData, int LineStep, int PixelStep,
   int Lines, int Count, int Radius, uint32_t MulSum, uint32_t ShrSum)
{
   const int max_index = Count - 1;
   const uint32_t div = (Radius * 2) + 1;
   std::vector<uint32_t> stack(div);

   const auto mul = _mm_set1_epi32(int(MulSum));
   const auto shr = _mm_cvtsi32_si128(int(ShrSum));

   for (int line=0; line < Lines; line++) {
      const uint8_t *line_in = InData + (LineStep * line);
      uint8_t *line_out = OutData + (LineStep * line);

      auto sum     = _mm_setzero_si128();
      auto sum_in  = _mm_setzero_si128();
      auto sum_out = _mm_setzero_si128();

      // Pixels outside of the line are transparent, so only the first pixel contributes to the left half.

      auto src = line_in;
      for (int i=0; i < Radius; i++) stack[i] = 0;
      stack[Radius] = blur_read32(src);
      const auto first = blur_unpack(stack[Radius]);
      sum     = blur_mullo32(first, _mm_set1_epi32(Radius + 1));
      sum_out = first;

      for (int i=1; i <= Radius; i++) {
         uint32_t px = 0;
         if (i <= max_index) {
            src += PixelStep;
            px = blur_read32(src);
         }
         stack[i + Radius] = px;
         const auto v = blur_unpack(px);
         sum    = _mm_add_epi32(sum, blur_mullo32(v, _mm_set1_epi32(Radius + 1 - i)));
         sum_in = _mm_add_epi32(sum_in, v);
      }

      uint32_t stack_ptr = Radius;
      int xp = (Radius > max_index) ? max_index : Radius; // Index of the last pixel read from the line
      src = line_in + (xp * PixelStep);
      auto dst = line_out;
      for (int i=0; i < Count; i++) {
         blur_write32(dst, blur_pack(_mm_srl_epi32(blur_mullo32(sum, mul), shr)));
         dst += PixelStep;

         sum = _mm_sub_epi32(sum, sum_out);

         uint32_t stack_start = stack_ptr + div - Radius;
         if (stack_start >= div) stack_start -= div;
         sum_out = _mm_sub_epi32(sum_out, blur_unpack(stack[stack_start]));

         uint32_t px = 0;
         if (xp < max_index) {
            src += PixelStep;
            ++xp;
            px = blur_read32(src);
         }

         stack[stack_start] = px;
         sum_in = _mm_add_epi32(sum_in, blur_unpack(px));
         sum    = _mm_add_epi32(sum, sum_in);

         ++stack_ptr;
         if (stack_ptr >= div) stack_ptr = 0;
         const auto sv = blur_unpack(stack[stack_ptr]);
         sum_out = _mm_add_epi32(sum_out, sv);
         sum_in  = _mm_sub_epi32(sum_in, sv);
      }
   }
}

#endif

//********************************************************************************************************************
// Scalar implementation of the stack blur pass, with the same interface and output as the SSE2 version.  Samples
// outside of the line are treated as transparent black, matching the SVG edge mode of 'none'.

#ifndef FILTER_SSE2

static const uint8_t BLUR_ZERO[4] = { 0, 0, 0, 0 }; // Transparent pixel for samples outside of the line

static void stack_blur_axis(const uint8_t *InData, uint8_t *OutData, int LineStep, int PixelStep,
   int Lines, int Count, int Radius, uint32_t MulSum, uint32_t ShrSum)
{
   const int max_index = Count - 1;
   const uint32_t div = (Radius * 2) + 1;
   std::vector<std::array<uint8_t, 4>> stack(div);

   for (int line=0; line < Lines; line++) {
      const uint8_t *line_in = InData + (LineStep * line);
      uint8_t *line_out = OutData + (LineStep * line);

      uint32_t sum[4] = { 0, 0, 0, 0 }, sum_in[4] = { 0, 0, 0, 0 }, sum_out[4] = { 0, 0, 0, 0 };

      // Pixels outside of the line are transparent, so only the first pixel contributes to the left half.

      auto src = line_in;
      for (int i=0; i < Radius; i++) stack[i] = { 0, 0, 0, 0 };
      for (int c=0; c < 4; c++) {
         stack[Radius][c] = src[c];
         sum[c]     = src[c] * (Radius + 1);
         sum_out[c] = src[c];
      }

      for (int i=1; i <= Radius; i++) {
         const uint8_t *in = BLUR_ZERO;
         if (i <= max_index) {
            src += PixelStep;
            in = src;
         }
         for (int c=0; c < 4; c++) {
            stack[i + Radius][c] = in[c];
            sum[c]    += in[c] * (Radius + 1 - i);
            sum_in[c] += in[c];
         }
      }

      uint32_t stack_ptr = Radius;
      int xp = (Radius > max_index) ? max_index : Radius; // Index of the last pixel read from the line
      src = line_in + (xp * PixelStep);
      auto dst = line_out;
      for (int i=0; i < Count; i++) {
         for (int c=0; c < 4; c++) dst[c] = uint8_t((sum[c] * MulSum) >> ShrSum);
         dst += PixelStep;

         uint32_t stack_start = stack_ptr + div - Radius;
         if (stack_start >= div) stack_start -= div;
         auto &start = stack[stack_start];

         const uint8_t *in = BLUR_ZERO;
         if (xp < max_index) {
            src += PixelStep;
            ++xp;
            in = src;
         }

         for (int c=0; c < 4; c++) {
            sum[c]     -= sum_out[c];
            sum_out[c] -= start[c];
            start[c]    = in[c];
            sum_in[c]  += in[c];
            sum[c]     += sum_in[c];
         }

         ++stack_ptr;
         if (stack_ptr >= div) stack_ptr = 0;
         auto &next = stack[stack_ptr];

         for (int c=0; c < 4; c++) {
            sum_out[c] += next[c];
            sum_in[c]  -= next[c];
         }
      }
   }
}

#endif

//********************************************************************************************************************
// Blurs one axis of a Width x Height region with a fractional Radius.  The stack blur only supports integral radii,
// so the results of the two nearest radii are blended according to the fraction.  This keeps the blur continuous as
// the radius changes, so that a magnified graphic is blurred in proportion and small radii are not lost to
// truncation.  A radius below 1 is blended with the unblurred input.
//
// InData and OutData can refer to the same region.  Temp is resized as necessary to hold the larger radius.

constexpr double MIN_BLUR_RADIUS = 0.5 / 256.0; // Below this, the blend weight rounds to zero
constexpr double MAX_BLUR_RADIUS = 254.0;       // Limited by the size of the stack blur tables

static void blur_axis(const uint8_t *InData, uint8_t *OutData, std::vector<uint8_t> &Temp, int Width, int Height,
   int Stride, bool Vertical, double Radius)
{
   const int line_step  = Vertical ? 4 : Stride;
   const int pixel_step = Vertical ? Stride : 4;
   const int lines      = Vertical ? Width : Height;
   const int count      = Vertical ? Height : Width;

   auto stack_blur = [&](const uint8_t *Src, uint8_t *Dest, int R) {
#ifdef FILTER_SSE2
      stack_blur_axis_sse2(Src, Dest, line_step, pixel_step, lines, count, R,
         stack_blur_tables<int>::g_stack_blur8_mul[R], stack_blur_tables<int>::g_stack_blur8_shr[R]);
#else
      stack_blur_axis(Src, Dest, line_step, pixel_step, lines, count, R,
         stack_blur_tables<int>::g_stack_blur8_mul[R], stack_blur_tables<int>::g_stack_blur8_shr[R]);
#endif
   };

   if (Radius > MAX_BLUR_RADIUS) Radius = MAX_BLUR_RADIUS;
   const int lower  = int(Radius);
   const int weight = int(std::lround((Radius - lower) * 256.0)); // Weight of the upper radius, out of 256

   if (weight >= 256) {
      stack_blur(InData, OutData, lower + 1);
      return;
   }

   uint8_t *upper = nullptr;
   if (weight > 0) { // The upper radius is rendered first because the lower pass may overwrite InData
      Temp.resize(size_t(Stride) * Height);
      upper = Temp.data();
      stack_blur(InData, upper, lower + 1);
   }

   if (lower > 0) stack_blur(InData, OutData, lower);
   else if (InData != OutData) {
      for (int y=0; y < Height; y++) copymem(InData + (y * Stride), OutData + (y * Stride), Width * 4);
   }

   if (upper) {
      for (int y=0; y < Height; y++) {
         auto dst = OutData + (y * Stride);
         auto src = upper + (y * Stride);
         for (int i=0; i < Width * 4; i++) {
            dst[i] = uint8_t(dst[i] + ((((int(src[i]) - int(dst[i])) * weight) + 128) >> 8));
         }
      }
   }
}

//********************************************************************************************************************
// This is the stack blur algorithm originally implemented in AGG.  It is intended to produce a near identical output
// to that of a standard gaussian blur algorithm.
//
// Note that blurring is always performed with premultiplied colour values; otherwise the function will output pixels
// that are darkly tinted.

static ERR BLURFX_Draw(extBlurFX *Self, struct acDraw *Args)
{
   auto outBmp = Self->Target;
   if (outBmp->BytesPerPixel != 4) return ERR::InvalidState;

   double rx, ry;
   Self->radii(rx, ry);

   const bool blur_x = rx >= MIN_BLUR_RADIUS;
   const bool blur_y = ry >= MIN_BLUR_RADIUS;

   objBitmap *inBmp;

   if ((not blur_x) and (not blur_y)) {
      if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;
      BAF copy_flags = (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) ? BAF::LINEAR : BAF::NIL;
      gfx::CopyArea(inBmp, outBmp, copy_flags, 0, 0, inBmp->Width, inBmp->Height, 0, 0);
      return ERR::Okay;
   }

   if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;
   if ((Self->SourceType IS VSF::REFERENCE) and (Self->Input)) {
      objBitmap *copy_bmp;
      if (auto error = get_banked_bitmap(Self->Filter, &copy_bmp); !error) {
         gfx::CopyArea(inBmp, copy_bmp, BAF::NIL,
            inBmp->Clip.Left,
            inBmp->Clip.Top,
            inBmp->Clip.Right - inBmp->Clip.Left,
            inBmp->Clip.Bottom - inBmp->Clip.Top,
            copy_bmp->Clip.Left,
            copy_bmp->Clip.Top);

         inBmp = copy_bmp;
      }
      else return error;
   }

   if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) inBmp->convertToLinear();

   inBmp->premultiply();

   const int w = (outBmp->Clip.Right - outBmp->Clip.Left);
   const int h = (outBmp->Clip.Bottom - outBmp->Clip.Top);

   uint8_t *in_data  = inBmp->Data + (outBmp->Clip.Left<<2) + (outBmp->Clip.Top * inBmp->LineWidth);
   uint8_t *out_data = outBmp->Data + (outBmp->Clip.Left<<2) + (outBmp->Clip.Top * inBmp->LineWidth);

   std::vector<uint8_t> temp;
   if (blur_x) blur_axis(in_data, out_data, temp, w, h, outBmp->LineWidth, false, rx);

   // If the X axis was already processed, the dest becomes the source
   if (blur_y) blur_axis(blur_x ? out_data : in_data, out_data, temp, w, h, outBmp->LineWidth, true, ry);

   //bmpDemultiply(inBmp);

   outBmp->Flags |= BMF::PREMUL; // Need to tell the bitmap it has premultiplied output before Demultiply()
   outBmp->demultiply();

   if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) {
      outBmp->ColourSpace = CS::LINEAR_RGB;
      outBmp->convertToRGB();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SX: The standard deviation of the blur on the x axis.

The (SX,SY) field values define the standard deviation of the gaussian blur along each axis.

If either value is 0 or less, the effect is disabled on that axis.

-FIELD-
SY: The standard deviation of the blur on the x axis.

The (SX,SY) field values define the standard deviation of the gaussian blur along each axis.

If either value is 0 or less, the effect is disabled on that axis.

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR BLURFX_GET_XMLDef(extBlurFX *Self, std::string &Value)
{
   std::stringstream stream;
   stream << "feGaussianBlur stdDeviation=\"" << Self->SX << " " << Self->SY << "\"";
   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_blur_def.c"

static const FieldArray clBlurFXFields[] = {
   { "SX",     FDF_DOUBLE|FDF_RW },
   { "SY",     FDF_DOUBLE|FDF_RW },
   { "XMLDef", FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, BLURFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_blurfx(void)
{
   clBlurFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::BLURFX),
      fl::Name("BlurFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clBlurFXActions),
      fl::Fields(clBlurFXFields),
      fl::Size(sizeof(extBlurFX)),
      fl::Path(MOD_PATH));

   return clBlurFX ? ERR::Okay : ERR::AddClass;
}
