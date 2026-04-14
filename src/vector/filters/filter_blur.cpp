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
   using create = pf::Create<extBlurFX>;

   double SX, SY;
};

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

   double scale = 1.0;
   if (Self->Filter->ClientVector) scale = Self->Filter->ClientVector->Transform.scale();

   int rx, ry;
   if (Self->Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      if (Self->Filter->AspectRatio IS VFA::NONE) {
         // Scaling is applied evenly on both axis.  Uses the same formula as a scaled stroke-width.
         double diag = dist(0, 0, Self->Filter->BoundWidth, Self->Filter->BoundHeight) * INV_SQRT2;
         rx = int(Self->SX * diag * 2 * scale);
         ry = int(Self->SY * diag * 2 * scale);
      }
      else {
         // Scaling is stretched independently of each axis
         rx = int(Self->SX * Self->Filter->BoundWidth * 2 * scale);
         ry = int(Self->SY * Self->Filter->BoundHeight * 2 * scale);
      }
   }
   else {
      rx = int(Self->SX * 2 * scale);
      ry = int(Self->SY * 2 * scale);
   }

   objBitmap *inBmp;

   if ((rx < 1) and (ry < 1)) {
      if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;
      BAF copy_flags = (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) ? BAF::LINEAR : BAF::NIL;
      gfx::CopyArea(inBmp, outBmp, copy_flags, 0, 0, inBmp->Width, inBmp->Height, 0, 0);
      return ERR::Okay;
   }

   if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;

   if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) inBmp->convertToLinear();

   inBmp->premultiply();

   uint8_t *dst_pix_ptr;
   agg::rgba8 *stack_pix_ptr;

   const int w = (outBmp->Clip.Right - outBmp->Clip.Left);
   const int h = (outBmp->Clip.Bottom - outBmp->Clip.Top);
   const int wm = w - 1;
   const int hm = h - 1;

   uint8_t A = inBmp->ColourFormat->AlphaPos>>3;
   uint8_t R = inBmp->ColourFormat->RedPos>>3;
   uint8_t G = inBmp->ColourFormat->GreenPos>>3;
   uint8_t B = inBmp->ColourFormat->BluePos>>3;

   uint8_t *in_data  = inBmp->Data + (outBmp->Clip.Left<<2) + (outBmp->Clip.Top * inBmp->LineWidth);
   uint8_t *out_data = outBmp->Data + (outBmp->Clip.Left<<2) + (outBmp->Clip.Top * inBmp->LineWidth);

   uint32_t sum_r, sum_g, sum_b, sum_a;
   uint32_t sum_in_r, sum_in_g, sum_in_b, sum_in_a;
   uint32_t sum_out_r, sum_out_g, sum_out_b, sum_out_a;
   int x, y, xp, yp;
   uint32_t stack_ptr;
   uint32_t stack_start;
   uint32_t div;
   uint32_t mul_sum;
   uint32_t shr_sum;

   std::vector<agg::rgba8> stack;

   if (rx > 0) {
      if (rx > 254) rx = 254;
      div = rx * 2 + 1;
      mul_sum = stack_blur_tables<int>::g_stack_blur8_mul[rx];
      shr_sum = stack_blur_tables<int>::g_stack_blur8_shr[rx];
      stack.resize(div);

      for (y=0; y < h; y++) {
         sum_r = sum_g = sum_b = sum_a = sum_in_r = sum_in_g = sum_in_b = sum_in_a = sum_out_r = sum_out_g = sum_out_b = sum_out_a = 0;

         const uint8_t * src_pix_ptr = in_data + (outBmp->LineWidth * y);
         for (int i=0; i <= rx; i++) {
             stack_pix_ptr    = &stack[i];
             stack_pix_ptr->r = src_pix_ptr[R];
             stack_pix_ptr->g = src_pix_ptr[G];
             stack_pix_ptr->b = src_pix_ptr[B];
             stack_pix_ptr->a = src_pix_ptr[A];
             sum_r     += src_pix_ptr[R] * (i + 1);
             sum_g     += src_pix_ptr[G] * (i + 1);
             sum_b     += src_pix_ptr[B] * (i + 1);
             sum_a     += src_pix_ptr[A] * (i + 1);
             sum_out_r += src_pix_ptr[R];
             sum_out_g += src_pix_ptr[G];
             sum_out_b += src_pix_ptr[B];
             sum_out_a += src_pix_ptr[A];
         }

         for (int i=1; i <= rx; i++) {
            if (i <= wm) src_pix_ptr += 4;
            stack_pix_ptr = &stack[i + rx];
            stack_pix_ptr->r = src_pix_ptr[R];
            stack_pix_ptr->g = src_pix_ptr[G];
            stack_pix_ptr->b = src_pix_ptr[B];
            stack_pix_ptr->a = src_pix_ptr[A];
            sum_r    += src_pix_ptr[R] * (rx + 1 - i);
            sum_g    += src_pix_ptr[G] * (rx + 1 - i);
            sum_b    += src_pix_ptr[B] * (rx + 1 - i);
            sum_a    += src_pix_ptr[A] * (rx + 1 - i);
            sum_in_r += src_pix_ptr[R];
            sum_in_g += src_pix_ptr[G];
            sum_in_b += src_pix_ptr[B];
            sum_in_a += src_pix_ptr[A];
         }

         stack_ptr = rx;
         xp = rx;
         if (xp > wm) xp = wm;
         src_pix_ptr = in_data + (outBmp->LineWidth * y) + (xp<<2);
         dst_pix_ptr = out_data + (outBmp->LineWidth * y);
         for (int x=0; x < w; x++) {
            dst_pix_ptr[R] = (sum_r * mul_sum) >> shr_sum;
            dst_pix_ptr[G] = (sum_g * mul_sum) >> shr_sum;
            dst_pix_ptr[B] = (sum_b * mul_sum) >> shr_sum;
            dst_pix_ptr[A] = (sum_a * mul_sum) >> shr_sum;
            dst_pix_ptr += 4;

            sum_r -= sum_out_r;
            sum_g -= sum_out_g;
            sum_b -= sum_out_b;
            sum_a -= sum_out_a;

            stack_start = stack_ptr + div - rx;
            if (stack_start >= div) stack_start -= div;
            stack_pix_ptr = &stack[stack_start];

            sum_out_r -= stack_pix_ptr->r;
            sum_out_g -= stack_pix_ptr->g;
            sum_out_b -= stack_pix_ptr->b;
            sum_out_a -= stack_pix_ptr->a;

            if (xp < wm) {
               src_pix_ptr += 4;
               ++xp;
            }

            stack_pix_ptr->r = src_pix_ptr[R];
            stack_pix_ptr->g = src_pix_ptr[G];
            stack_pix_ptr->b = src_pix_ptr[B];
            stack_pix_ptr->a = src_pix_ptr[A];

            sum_in_r += src_pix_ptr[R];
            sum_in_g += src_pix_ptr[G];
            sum_in_b += src_pix_ptr[B];
            sum_in_a += src_pix_ptr[A];
            sum_r    += sum_in_r;
            sum_g    += sum_in_g;
            sum_b    += sum_in_b;
            sum_a    += sum_in_a;

            ++stack_ptr;
            if (stack_ptr >= div) stack_ptr = 0;
            stack_pix_ptr = &stack[stack_ptr];

            sum_out_r += stack_pix_ptr->r;
            sum_out_g += stack_pix_ptr->g;
            sum_out_b += stack_pix_ptr->b;
            sum_out_a += stack_pix_ptr->a;
            sum_in_r  -= stack_pix_ptr->r;
            sum_in_g  -= stack_pix_ptr->g;
            sum_in_b  -= stack_pix_ptr->b;
            sum_in_a  -= stack_pix_ptr->a;
         }
      }
   }

   if (ry > 0) {
      if (rx > 0) {
         in_data = out_data; // If rx was already processed, the dest becomes the source
      }

      if (ry > 254) ry = 254;
      div = ry * 2 + 1;
      mul_sum = stack_blur_tables<int>::g_stack_blur8_mul[ry];
      shr_sum = stack_blur_tables<int>::g_stack_blur8_shr[ry];
      stack.resize(div);

      int stride = outBmp->LineWidth;
      for (x = 0; x < w; x++) {
         sum_r = sum_g = sum_b = sum_a = sum_in_r = sum_in_g = sum_in_b = sum_in_a = sum_out_r = sum_out_g = sum_out_b = sum_out_a = 0;

         const uint8_t * src_pix_ptr = in_data + (x<<2);
         for (int i = 0; i <= ry; i++) {
             stack_pix_ptr    = &stack[i];
             stack_pix_ptr->r = src_pix_ptr[R];
             stack_pix_ptr->g = src_pix_ptr[G];
             stack_pix_ptr->b = src_pix_ptr[B];
             stack_pix_ptr->a = src_pix_ptr[A];
             sum_r += src_pix_ptr[R] * (i + 1);
             sum_g += src_pix_ptr[G] * (i + 1);
             sum_b += src_pix_ptr[B] * (i + 1);
             sum_a += src_pix_ptr[A] * (i + 1);
             sum_out_r += src_pix_ptr[R];
             sum_out_g += src_pix_ptr[G];
             sum_out_b += src_pix_ptr[B];
             sum_out_a += src_pix_ptr[A];
         }

         for (int i = 1; i <= ry; i++) {
             if (i <= hm) src_pix_ptr += stride;
             stack_pix_ptr = &stack[i + ry];
             stack_pix_ptr->r = src_pix_ptr[R];
             stack_pix_ptr->g = src_pix_ptr[G];
             stack_pix_ptr->b = src_pix_ptr[B];
             stack_pix_ptr->a = src_pix_ptr[A];
             sum_r += src_pix_ptr[R] * (ry + 1 - i);
             sum_g += src_pix_ptr[G] * (ry + 1 - i);
             sum_b += src_pix_ptr[B] * (ry + 1 - i);
             sum_a += src_pix_ptr[A] * (ry + 1 - i);
             sum_in_r += src_pix_ptr[R];
             sum_in_g += src_pix_ptr[G];
             sum_in_b += src_pix_ptr[B];
             sum_in_a += src_pix_ptr[A];
         }

         stack_ptr = ry;
         yp = ry;
         if (yp > hm) yp = hm;
         src_pix_ptr = in_data + (x<<2) + (outBmp->LineWidth * yp);
         dst_pix_ptr = out_data + (x<<2);
         for (int y = 0; y < h; y++) {
            dst_pix_ptr[R] = (sum_r * mul_sum) >> shr_sum;
            dst_pix_ptr[G] = (sum_g * mul_sum) >> shr_sum;
            dst_pix_ptr[B] = (sum_b * mul_sum) >> shr_sum;
            dst_pix_ptr[A] = (sum_a * mul_sum) >> shr_sum;
            dst_pix_ptr += stride;

            sum_r -= sum_out_r;
            sum_g -= sum_out_g;
            sum_b -= sum_out_b;
            sum_a -= sum_out_a;

            stack_start = stack_ptr + div - ry;
            if (stack_start >= div) stack_start -= div;

            stack_pix_ptr = &stack[stack_start];
            sum_out_r -= stack_pix_ptr->r;
            sum_out_g -= stack_pix_ptr->g;
            sum_out_b -= stack_pix_ptr->b;
            sum_out_a -= stack_pix_ptr->a;

            if (yp < hm) {
                src_pix_ptr += stride;
                ++yp;
            }

            stack_pix_ptr->r = src_pix_ptr[R];
            stack_pix_ptr->g = src_pix_ptr[G];
            stack_pix_ptr->b = src_pix_ptr[B];
            stack_pix_ptr->a = src_pix_ptr[A];

            sum_in_r += src_pix_ptr[R];
            sum_in_g += src_pix_ptr[G];
            sum_in_b += src_pix_ptr[B];
            sum_in_a += src_pix_ptr[A];
            sum_r    += sum_in_r;
            sum_g    += sum_in_g;
            sum_b    += sum_in_b;
            sum_a    += sum_in_a;

            ++stack_ptr;
            if (stack_ptr >= div) stack_ptr = 0;
            stack_pix_ptr = &stack[stack_ptr];

            sum_out_r += stack_pix_ptr->r;
            sum_out_g += stack_pix_ptr->g;
            sum_out_b += stack_pix_ptr->b;
            sum_out_a += stack_pix_ptr->a;
            sum_in_r  -= stack_pix_ptr->r;
            sum_in_g  -= stack_pix_ptr->g;
            sum_in_b  -= stack_pix_ptr->b;
            sum_in_a  -= stack_pix_ptr->a;
         }
      }
   }

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

*********************************************************************************************************************/

static ERR BLURFX_GET_SX(extBlurFX *Self, double *Value)
{
   *Value = Self->SX;
   return ERR::Okay;
}

static ERR BLURFX_SET_SX(extBlurFX *Self, double Value)
{
   Self->SX = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SY: The standard deviation of the blur on the x axis.

The (SX,SY) field values define the standard deviation of the gaussian blur along each axis.

If either value is 0 or less, the effect is disabled on that axis.

*********************************************************************************************************************/

static ERR BLURFX_GET_SY(extBlurFX *Self, double *Value)
{
   *Value = Self->SY;
   return ERR::Okay;
}

static ERR BLURFX_SET_SY(extBlurFX *Self, double Value)
{
   Self->SY = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR BLURFX_GET_XMLDef(extBlurFX *Self, STRING *Value)
{
   std::stringstream stream;
   stream << "feGaussianBlur stdDeviation=\"" << Self->SX << " " << Self->SY << "\"";
   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_blur_def.c"

static const FieldArray clBlurFXFields[] = {
   { "SX",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, BLURFX_GET_SX, BLURFX_SET_SX },
   { "SY",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, BLURFX_GET_SY, BLURFX_SET_SY },
   { "XMLDef", FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, BLURFX_GET_XMLDef, nullptr },
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
