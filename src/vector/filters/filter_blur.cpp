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

If both #SX and #SY are zero, the effect copies its input.

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

constexpr double MIN_BLUR_RADIUS = 0.5 / 256.0; // Below this, the blend weight rounds to zero
constexpr int MAX_BLUR_RADIUS = 254;            // Limited by the size of the stack blur tables

class extBlurFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::BLURFX;
   static constexpr CSTRING CLASS_NAME = "BlurFX";
   using create = kt::Create<extBlurFX>;

   double SX = 0, SY = 0;
   FEM EdgeMode = FEM::TRANSPARENT;

   extBlurFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   // Computes the stack blur radius of each axis in bitmap pixels.  The radius is twice the standard deviation.

   void radii(double &RX, double &RY) const {
      const auto &render = *Filter->Render;
      const auto [unit_x, unit_y] = render.primitive_scales(true);
      const double scale = render.ToScene.scale();
      RX = SX * unit_x * 2 * scale;
      RY = SY * unit_y * 2 * scale;
   }

   filter_sampling sampling() const {
      double x, y;
      radii(x, y);
      if ((not std::isfinite(x)) or (not std::isfinite(y))) return { filter_access::BOUNDED, x, y };
      if (EdgeMode != FEM::TRANSPARENT) {
         return { filter_access::FULL_INPUT, x >= MIN_BLUR_RADIUS ? filter_sampling::UNBOUNDED : 0,
            y >= MIN_BLUR_RADIUS ? filter_sampling::UNBOUNDED : 0 };
      }
      return { filter_access::BOUNDED, std::ceil(std::clamp(x, 0.0, double(MAX_BLUR_RADIUS))) + 1,
         std::ceil(std::clamp(y, 0.0, double(MAX_BLUR_RADIUS))) + 1 };
   }
};

//********************************************************************************************************************

struct blur_edge {
   int Start, End;
   FEM Mode;

   int address(int64_t Index) const {
      return filter_edge_index(Index, Start, End, Mode);
   }
};

//********************************************************************************************************************
// SSE2 implementation of the stack blur pass.  The algorithm is identical to the scalar version below, but the
// per-channel running sums are held in packed 32-bit lanes so that all four channels advance per operation.  All
// arithmetic is integral, so the output is bit-identical to the scalar path.  Channel byte order is irrelevant
// because every channel is processed independently and identically.

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

static void stack_blur_axis(const uint8_t *InData, uint8_t *OutData, int LineStep, int PixelStep,
   int Lines, int Count, int Radius, uint32_t MulSum, uint32_t ShrSum, const blur_edge &Edge)
{
   const uint32_t div = (Radius * 2) + 1;
   std::array<uint32_t, MAX_BLUR_RADIUS * 2 + 1> stack;

   const auto mul = _mm_set1_epi32(int(MulSum));
   const auto shr = _mm_cvtsi32_si128(int(ShrSum));

   for (int line=0; line < Lines; line++) {
      const uint8_t *line_in = InData + (LineStep * line);
      uint8_t *line_out = OutData + (LineStep * line);

      auto sum     = _mm_setzero_si128();
      auto sum_in  = _mm_setzero_si128();
      auto sum_out = _mm_setzero_si128();

      auto sample = [&](int64_t Index) {
         const int i = Edge.address(Index);
         return (i < 0) ? uint32_t(0) : blur_read32(line_in + (std::ptrdiff_t(i) * PixelStep));
      };

      for (int i=-Radius; i <= Radius; i++) {
         const auto px = sample(i);
         stack[i + Radius] = px;
         const auto v = blur_unpack(px);
         sum = _mm_add_epi32(sum, blur_mullo32(v, _mm_set1_epi32(Radius + 1 - std::abs(i))));
         if (i <= 0) sum_out = _mm_add_epi32(sum_out, v);
         else sum_in = _mm_add_epi32(sum_in, v);
      }

      uint32_t stack_ptr = Radius;
      for (int i=0; i < Count; i++) {
         const auto blurred = blur_pack(_mm_srl_epi32(blur_mullo32(sum, mul), shr));
         blur_write32(line_out + (std::ptrdiff_t(i) * PixelStep), blurred);

         sum = _mm_sub_epi32(sum, sum_out);

         uint32_t stack_start = stack_ptr + div - Radius;
         if (stack_start >= div) stack_start -= div;
         sum_out = _mm_sub_epi32(sum_out, blur_unpack(stack[stack_start]));

         const auto px = sample(int64_t(i) + Radius + 1);
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

//********************************************************************************************************************
// Scalar implementation of the stack blur pass, with the same interface and output as the SSE2 version.

#else

static const uint8_t BLUR_ZERO[4] = { 0, 0, 0, 0 }; // Transparent pixel for samples outside of the line

static void stack_blur_axis(const uint8_t *InData, uint8_t *OutData, int LineStep, int PixelStep,
   int Lines, int Count, int Radius, uint32_t MulSum, uint32_t ShrSum, const blur_edge &Edge)
{
   const uint32_t div = (Radius * 2) + 1;
   std::array<std::array<uint8_t, 4>, MAX_BLUR_RADIUS * 2 + 1> stack;

   for (int line=0; line < Lines; line++) {
      const uint8_t *line_in = InData + (LineStep * line);
      uint8_t *line_out = OutData + (LineStep * line);

      uint32_t sum[4] = { 0, 0, 0, 0 }, sum_in[4] = { 0, 0, 0, 0 }, sum_out[4] = { 0, 0, 0, 0 };

      auto sample = [&](int64_t Index) {
         const int i = Edge.address(Index);
         return (i < 0) ? BLUR_ZERO : line_in + (std::ptrdiff_t(i) * PixelStep);
      };

      for (int i=-Radius; i <= Radius; i++) {
         const auto in = sample(i);
         const int weight = Radius + 1 - std::abs(i);
         for (int c=0; c < 4; c++) {
            stack[i + Radius][c] = in[c];
            sum[c] += in[c] * weight;
            if (i <= 0) sum_out[c] += in[c];
            else sum_in[c] += in[c];
         }
      }

      uint32_t stack_ptr = Radius;
      for (int i=0; i < Count; i++) {
         auto dst = line_out + (std::ptrdiff_t(i) * PixelStep);
         for (int c=0; c < 4; c++) dst[c] = uint8_t((sum[c] * MulSum) >> ShrSum);

         uint32_t stack_start = stack_ptr + div - Radius;
         if (stack_start >= div) stack_start -= div;
         auto &start = stack[stack_start];

         const auto in = sample(int64_t(i) + Radius + 1);

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
// Transparent addressing also allows InData and OutData to refer to the same region.


static ERR blur_axis(extBlurFX *Effect, const uint8_t *InData, uint8_t *OutData, uint8_t *&Temp,
   int Width, int Height, int Stride, bool Vertical, double Radius, const blur_edge &Edge)
{
   const int line_step  = Vertical ? 4 : Stride;
   const int pixel_step = Vertical ? Stride : 4;
   const int lines      = Vertical ? Width : Height;
   const int count      = Vertical ? Height : Width;

   auto stack_blur = [&](const uint8_t *Src, uint8_t *Dest, int R) {
      stack_blur_axis(Src, Dest, line_step, pixel_step, lines, count, R,
         stack_blur_tables<int>::g_stack_blur8_mul[R], stack_blur_tables<int>::g_stack_blur8_shr[R], Edge);
   };

   if (Radius > MAX_BLUR_RADIUS) Radius = MAX_BLUR_RADIUS;
   const int lower  = int(Radius);
   const int weight = int(std::lround((Radius - lower) * 256.0)); // Weight of the upper radius, out of 256

   if (weight >= 256) {
      stack_blur(InData, OutData, lower + 1);
      return ERR::Okay;
   }

   uint8_t *upper = nullptr;
   if (weight > 0) { // The upper radius is rendered first because the lower pass may overwrite InData
      if (not Temp) {
         if (auto error = get_filter_scratch(Effect, size_t(Stride) * Height, Temp); error != ERR::Okay) return error;
      }
      upper = Temp;
      stack_blur(InData, upper, lower + 1);
   }

   if ((lower > 0) or (Edge.Mode != FEM::TRANSPARENT)) stack_blur(InData, OutData, lower);
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
   return ERR::Okay;
}

//********************************************************************************************************************

static TClipRectangle<int> blur_input_region(const extBlurFX *Self, const filter_surface_view &Source,
   const filter_step *Producer)
{
   const auto &clip = Source.Valid;
   if ((Self->EdgeMode IS FEM::TRANSPARENT) or (not Producer)) {
      return clip;
   }

   const auto &sub = Producer->Subregion;
   const TClipRectangle<int> empty = { clip.left, clip.top, clip.left, clip.top };
   if (not sub.valid()) return empty;

   const auto &transform = Self->Filter->Render->Transform;
   auto path = sub.as_path(transform);
   const auto bounds = get_bounds(path);
   if (not bounds.valid()) return empty;

   // Primitive clipping includes the left and top edges in user space.  Reflection can put those edges on the
   // far side of the bitmap, so account for which bounds include a pixel centre that falls exactly on the edge.
   auto limit = [](double Coordinate, bool RoundPastCentre, int Low, int High) {
      const auto centre = Coordinate - 0.5;
      const auto index = RoundPastCentre ? std::floor(centre) + 1 : std::ceil(centre);
      return int(std::clamp(index, double(Low), double(High)));
   };
   const TClipRectangle<int> region = {
      limit(bounds.left, (transform.sx < 0) or (transform.shx < 0), clip.left, clip.right),
      limit(bounds.top, (transform.shy < 0) or (transform.sy < 0), clip.top, clip.bottom),
      limit(bounds.right, (transform.sx <= 0) and (transform.shx <= 0), clip.left, clip.right),
      limit(bounds.bottom, (transform.shy <= 0) and (transform.sy <= 0), clip.top, clip.bottom)
   };
   return region.valid() ? region : empty;
}

//********************************************************************************************************************
// This is the stack blur algorithm originally implemented in AGG.  It is intended to produce a near identical output
// to that of a standard gaussian blur algorithm.
//
// Note that blurring is always performed with premultiplied colour values; otherwise the function will output pixels
// that are darkly tinted.

static ERR BLURFX_Draw(extBlurFX *Self, struct acDraw *Args)
{
   auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const auto producer = step->Inputs.front().Producer;
   auto outBmp = Self->Target;
   if (outBmp->BytesPerPixel != 4) return ERR::InvalidState;

   double rx, ry;
   Self->radii(rx, ry);

   const bool blur_x = rx >= MIN_BLUR_RADIUS;
   const bool blur_y = ry >= MIN_BLUR_RADIUS;

   filter_surface_view source;
   if (auto error = get_source_view(Self, source, filter_input_role::INPUT); error != ERR::Okay) return error;

   const bool linear = Self->Filter->ColourSpace IS VCS::LINEAR_RGB;
   if ((not blur_x) and (not blur_y)) {
      const auto flags = linear ? BAF::LINEAR : BAF::NIL;
      return copy_filter_bitmap(source.Bitmap, outBmp, flags, 0, 0, source.Width, source.Height, 0, 0);
   }

   filter_surface_view input;
   const auto encoding = linear ? CS::LINEAR_RGB : CS::SRGB;
   if (auto error = premultiplied_input(Self, source, encoding, input); error != ERR::Okay) return error;

   const auto region = blur_input_region(Self, source, producer);
   const auto &clip = outBmp->Clip;
   const int w = clip.Right - clip.Left;
   const int h = clip.Bottom - clip.Top;
   const blur_edge edge_x = { region.left - clip.Left, region.right - clip.Left, Self->EdgeMode };
   const blur_edge edge_y = { region.top - clip.Top, region.bottom - clip.Top, Self->EdgeMode };

   auto bitmap_data = [&](objBitmap *Bitmap) {
      return Bitmap->Data + (std::ptrdiff_t(clip.Top) * Bitmap->LineWidth) + (std::ptrdiff_t(clip.Left) * 4);
   };
   auto in_data = input.row(clip.Top) + std::ptrdiff_t(clip.Left) * 4;
   auto out_data = bitmap_data(outBmp);
   auto horizontal = out_data;
   if (blur_x and blur_y and (Self->EdgeMode != FEM::TRANSPARENT)) {
      // Addressing can revisit a sample that a pass would overwrite if it ran in place.
      auto filter = Self->Filter;
      objBitmap *scratch;
      if (auto error = get_filter_bitmap(filter, filter->Render->WorkingSurfaces, scratch); error != ERR::Okay) {
         return error;
      }
      if (not active_filter_step(Self)) return ERR::InvalidState;
      horizontal = bitmap_data(scratch);
   }

   uint8_t *temp = nullptr;
   if (blur_x) {
      auto error = blur_axis(Self, in_data, horizontal, temp, w, h, outBmp->LineWidth, false, rx, edge_x);
      if (error != ERR::Okay) return error;
   }
   if (blur_y) {
      auto error = blur_axis(Self, blur_x ? horizontal : in_data, out_data, temp, w, h,
         outBmp->LineWidth, true, ry, edge_y);
      if (error != ERR::Okay) return error;
   }

   outBmp->Flags |= BMF::PREMUL; // Need to tell the bitmap it has premultiplied output before Demultiply()
   if (auto error = outBmp->demultiply(); error != ERR::Okay) return error;
   if (linear) {
      outBmp->ColourSpace = CS::LINEAR_RGB;
      return outBmp->convertToRGB();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EdgeMode: Controls how the blur samples beyond the edges of its input.
Lookup: FEM

`TRANSPARENT` (the default) uses transparent black.  `CLAMP` uses the nearest edge sample, `REPEAT` wraps to the
opposite edge and `MIRROR` reflects alternate copies of the input with the edge sample repeated at each join.

For standard inputs the sampling rectangle is the filter bitmap.  For a referenced effect the non-transparent modes
use the pixel-aligned bounding rectangle of its transformed primitive subregion, intersected with the filter bitmap.
The addressing mode applies along each axis with an active blur.

SVG reads and writes `CLAMP` as `edgeMode="duplicate"` and `REPEAT` as `edgeMode="wrap"`.
`MIRROR` uses the Kōtuku extension `kotuku:edgeMode="mirror"`.

-FIELD-
SX: The standard deviation of the blur on the x axis.

The (SX,SY) field values define the standard deviation of the gaussian blur along each axis.

If either value is 0 or less, the effect is disabled on that axis.

-FIELD-
SY: The standard deviation of the blur on the y axis.

The (SX,SY) field values define the standard deviation of the gaussian blur along each axis.

If either value is 0 or less, the effect is disabled on that axis.

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR BLURFX_SET_EdgeMode(extBlurFX *Self, FEM Value)
{
   if ((Value < FEM::TRANSPARENT) or (Value > FEM::MIRROR)) return ERR::OutOfRange;
   Self->EdgeMode = Value;
   return ERR::Okay;
}

static ERR BLURFX_GET_XMLDef(extBlurFX *Self, std::string &Value)
{
   std::stringstream stream;
   stream << "feGaussianBlur stdDeviation=\"" << Self->SX << " " << Self->SY << "\"";
   switch (Self->EdgeMode) {
      case FEM::CLAMP:  stream << " edgeMode=\"duplicate\""; break;
      case FEM::REPEAT: stream << " edgeMode=\"wrap\""; break;
      case FEM::MIRROR: stream << " kotuku:edgeMode=\"mirror\""; break;
      case FEM::TRANSPARENT: break;
   }
   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_blur_def.c"

static const FieldArray clBlurFXFields[] = {
   { "SX",       FDF_DOUBLE|FDF_RW },
   { "SY",       FDF_DOUBLE|FDF_RW },
   { "EdgeMode", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, BLURFX_SET_EdgeMode, &clBlurFXFEM },
   { "XMLDef",   FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, BLURFX_GET_XMLDef },
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
