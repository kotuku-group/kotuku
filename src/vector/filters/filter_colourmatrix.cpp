/*********************************************************************************************************************

This code was originally ported from Javascript.  The source code license follows.

**********************************************************************************************************************

ColourMatrix Class v2.1
released under MIT License (X11)
http://www.opensource.org/licenses/mit-license.php

Author: Mario Klingemann
http://www.quasimondo.com

Copyright (c) 2008 Mario Klingemann

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**********************************************************************************************************************

-CLASS-
ColourFX: Support for applying colour transformation effects.

Use ColourFX to perform colour transformations on an input source.  A #Mode must be selected and any required #Values
defined before initialisation.  Use #SetAdjustment() to replace the mode and values together at runtime.

SVG requires that the calculations are performed on non-premultiplied colour values.  If the input graphics consists
of premultiplied colour values, those values are automatically converted into non-premultiplied colour values for
this operation.

The matrix is applied in the effect's colour space, which is the parent @VectorFilter's ColourSpace unless
@FilterEffect.ColourSpace overrides it.  If set to
`LINEAR_RGB` (the SVG default) then pixel values are converted to linear RGB prior to transformation and back to
sRGB on output, otherwise the transformation is performed directly on the sRGB values.

*********************************************************************************************************************/

#include <array>
#include <iterator>

constexpr int CM_SIZE = 20;

constexpr double LUMA_R = 0.2125; // These values are as documented in W3C SVG
constexpr double LUMA_G = 0.7154;
constexpr double LUMA_B = 0.0721;

constexpr double ONETHIRD = 1.0 / 3.0;

typedef std::array<double, CM_SIZE> MATRIX;

static const MATRIX IDENTITY = { 1,0,0,0,0,   0,1,0,0,0,   0,0,1,0,0,   0,0,0,1,0 };

class ColourMatrix {
public:
   MATRIX matrix;
   ColourMatrix() : matrix(IDENTITY) { }

   double & operator[] (int x) {
      return matrix[x];
   }

   void apply(MATRIX mat) {
      MATRIX temp;
      int i = 0;
      for (int y = 0; y < 4; y++) {
         for (int x = 0; x < 5; x++) {
            temp[i+x] = mat[i] * matrix[x] + mat[(i+1)] * matrix[(x+5)] + mat[(i+2)] * matrix[(x+10)] +
                        mat[(i+3)] * matrix[(x + 15)] + (x == 4 ? mat[(i+4)] : 0);
         }
         i += 5;
      }
      matrix = temp;
   }

   void invert() {
      apply(MATRIX {
         -1 ,  0,  0, 0, 255,
         0 , -1,  0, 0, 255,
         0 ,  0, -1, 0, 255,
         0,   0,  0, 1,   0
      });
   }

   // s: Typical values come in the range 0.0 ... 2.0
   //    0.0 means 0% Saturation
   //    0.5 means 50% Saturation
   //    1.0 is 100% Saturation (aka no change)
   //    2.0 is 200% Saturation
   //
   //    Other values outside of this range are possible: -1.0 will invert the hue but keep the luminance

   void adjustSaturation(double s) {
      apply(MATRIX {
         LUMA_R+(1.0-LUMA_R)*s, LUMA_G-(LUMA_G*s),     LUMA_B-(LUMA_B*s), 0, 0,
         LUMA_R-(LUMA_R*s),     LUMA_G+(1.0-LUMA_G)*s, LUMA_B-(LUMA_B*s), 0, 0,
         LUMA_R-(LUMA_R*s),     LUMA_G-(LUMA_G*s),     LUMA_B+(1.0-LUMA_B)*s, 0, 0,
         0, 0, 0, 1, 0
      });
   }

   // Changes the contrast
   // s: Typical values come in the range -1.0 ... 1.0
   //    -1.0 means no contrast (grey)
   //       0 means no change
   //     1.0 is high contrast

   void adjustContrast(double r, double g = NAN, double b = NAN) {
      if (isnan(g)) g = r;
      if (isnan(b)) b = r;
      r += 1;
      g += 1;
      b += 1;
      apply(MATRIX {
         r, 0, 0, 0, (128 * (1 - r)),
         0, g, 0, 0, (128 * (1 - g)),
         0, 0, b, 0, (128 * (1 - b)),
         0, 0, 0, 1, 0
      });
   }

   void adjustBrightness(double r, double g = NAN, double b = NAN) {
      if (isnan(g)) g = r;
      if (isnan(b)) b = r;
      apply(MATRIX {
         1, 0, 0, 0, r,
         0, 1, 0, 0, g,
         0, 0, 1, 0, b,
         0, 0, 0, 1, 0
      });
   }

   void adjustHue(double degrees) {
       degrees *= DEG2RAD;
       double ccos = cos(degrees);
       double csin = sin(degrees);
       apply(MATRIX {
          ((LUMA_R + (ccos * (1 - LUMA_R))) + (csin * -(LUMA_R))), ((LUMA_G + (ccos * -(LUMA_G))) + (csin * -(LUMA_G))), ((LUMA_B + (ccos * -(LUMA_B))) + (csin * (1 - LUMA_B))), 0, 0,
          ((LUMA_R + (ccos * -(LUMA_R))) + (csin * 0.143)), ((LUMA_G + (ccos * (1 - LUMA_G))) + (csin * 0.14)), ((LUMA_B + (ccos * -(LUMA_B))) + (csin * -0.283)), 0, 0,
          ((LUMA_R + (ccos * -(LUMA_R))) + (csin * -((1 - LUMA_R)))), ((LUMA_G + (ccos * -(LUMA_G))) + (csin * LUMA_G)), ((LUMA_B + (ccos * (1 - LUMA_B))) + (csin * LUMA_B)), 0, 0,
          0, 0, 0, 1, 0
       });
   }

   void rotateHue(double Degrees) {
      constexpr double green_rotation = 39.182655;
      ColourMatrix pre_hue, post_hue;
      pre_hue.rotateRed(45);
      pre_hue.rotateGreen(-green_rotation);

      std::array<double, 4> lum = { LUMA_R, LUMA_G, LUMA_B, 1.0 };
      pre_hue.transformVector(lum);
      const double red = lum[0] / lum[2];
      const double green = lum[1] / lum[2];
      pre_hue.shearBlue(red, green);

      post_hue.shearBlue(-red, -green);
      post_hue.rotateGreen(green_rotation);
      post_hue.rotateRed(-45.0);

      apply(pre_hue.matrix);
      rotateBlue(Degrees);
      apply(post_hue.matrix);
   }

   void luminance2Alpha() {
      apply(MATRIX {
         0, 0, 0, 0, 1,
         0, 0, 0, 0, 1,
         0, 0, 0, 0, 1,
         LUMA_R, LUMA_G, LUMA_B, 0, 0
      });
   }

   void adjustAlphaContrast(double amount) {
       amount += 1;
       apply(MATRIX {
          1, 0, 0, 0, 0,
          0, 1, 0, 0, 0,
          0, 0, 1, 0, 0,
          0, 0, 0, amount, (128 * (1 - amount))
       });
   }

   // Values should tend to be between 0 and 1 for colourise, e.g. "1 0 0" will redden the entire image and eliminate
   // the G and B channels.  Values greater than 1 will tend to over-expose the image.  Lowering the amount parameter < 1
   // will allow you to tint the image.

   void colourise(double r, double g, double b, double amount = 1) {
      if (amount > 1) amount = 1;
      else if (amount < 0) amount = 0;

      double inv_amount = (1.0 - amount);

      apply(MATRIX {
         (inv_amount + ((amount * r) * LUMA_R)), ((amount * r) * LUMA_G), ((amount * r) * LUMA_B), 0, 0,
         ((amount * g) * LUMA_R), (inv_amount + ((amount * g) * LUMA_G)), ((amount * g) * LUMA_B), 0, 0,
         ((amount * b) * LUMA_R), ((amount * b) * LUMA_G), (inv_amount + ((amount * b) * LUMA_B)), 0, 0,
         0, 0, 0, 1, 0
      });
   }

   void average(double r=ONETHIRD, double g=ONETHIRD, double b=ONETHIRD) {
      apply(MATRIX {
         r, g, b, 0, 0,
         r, g, b, 0, 0,
         r, g, b, 0, 0,
         0, 0, 0, 1, 0
      });
   }

   void invertAlpha() {
      apply(MATRIX {
         1, 0, 0, 0, 0,
         0, 1, 0, 0, 0,
         0, 0, 1, 0, 0,
         0, 0, 0, -1, 255 });
   }

   void rotateRed(double degrees) {
      rotateColour(degrees, 2, 1);
   }

   void rotateGreen(double degrees) {
      rotateColour(degrees, 0, 2);
   }

   void rotateBlue(double degrees) {
      rotateColour(degrees, 1, 0);
   }

   void rotateColour(double degrees, int x, int y) {
      degrees *= DEG2RAD;
      auto mat = IDENTITY;
      mat[x + x * 5] = mat[y + y * 5] = cos(degrees);
      mat[y + x * 5] = sin(degrees);
      mat[x + y * 5] = -sin(degrees);
      apply(mat);
   }

   void shearRed(double green, double blue) {
      shearColour( 0, 1, green, 2, blue );
   }

   void shearGreen(double red, double blue) {
      shearColour( 1, 0, red, 2, blue );
   }

   void shearBlue(double red, double green) {
      shearColour( 2, 0, red, 1, green );
   }

   void shearColour(int x, int y1, double d1, int y2, double d2) {
      MATRIX mat = IDENTITY;
      mat[y1 + x * 5] = d1;
      mat[y2 + x * 5] = d2;
      apply( mat );
   }

   void transformVector(std::array<double, 4> &values) {
      double r = values[0] * matrix[0] + values[1] * matrix[1] + values[2] * matrix[2] + values[3] * matrix[3] + matrix[4];
      double g = values[0] * matrix[5] + values[1] * matrix[6] + values[2] * matrix[7] + values[3] * matrix[8] + matrix[9];
      double b = values[0] * matrix[10] + values[1] * matrix[11] + values[2] * matrix[12] + values[3] * matrix[13] + matrix[14];
      double a = values[0] * matrix[15] + values[1] * matrix[16] + values[2] * matrix[17] + values[3] * matrix[18] + matrix[19];

      values[0] = r;
      values[1] = g;
      values[2] = b;
      values[3] = a;
   }
};

//********************************************************************************************************************

class extColourFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COLOURFX;
   static constexpr CSTRING CLASS_NAME = "ColourFX";
   using create = kt::Create<extColourFX>;

   CM Mode = CM::NONE;
   std::array<double,CM_SIZE> Values = {};
   MATRIX Matrix = IDENTITY;
   int TotalValues;

   extColourFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      // Configure identity matrix
      Values[0]   = 1;
      Values[6]   = 1;
      Values[12]  = 1;
      Values[18]  = 1;
      TotalValues = CM_SIZE;
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR COLOURFX_Draw(extColourFX *Self, struct acDraw *Args)
{
   if ((not Self->Target) or (Self->Target->BytesPerPixel != 4)) return ERR::InvalidState;

   if (not Self->initialised()) return ERR::FieldNotSet;

   const uint8_t A = Self->Target->ColourFormat->AlphaPos>>3;
   const uint8_t R = Self->Target->ColourFormat->RedPos>>3;
   const uint8_t G = Self->Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->Target->ColourFormat->BluePos>>3;

   const auto &matrix = Self->Matrix;

   // The matrix is applied in the colour space defined by the parent filter; LINEAR_RGB is the SVG default and
   // requires sRGB values to be converted to linear before transformation, then inverted on output.

   const bool linear_rgb = effect_colour_space(Self) IS VCS::LINEAR_RGB;

   const objBitmap *in_bmp;
   if (auto error = get_source_bitmap(Self, &in_bmp, filter_input_role::INPUT); error != ERR::Okay) return error;

   auto out_line = Self->Target->Data + (Self->Target->Clip.Left<<2) + (Self->Target->Clip.Top * Self->Target->LineWidth);
   const auto *in_line  = in_bmp->Data + (in_bmp->Clip.Left<<2) + (in_bmp->Clip.Top * in_bmp->LineWidth);

   const int width  = in_bmp->Clip.Right - in_bmp->Clip.Left;
   const int height = in_bmp->Clip.Bottom - in_bmp->Clip.Top;

#ifdef FILTER_SSE2
   // The 4x5 matrix is repacked into column-major register pairs so that the four row dot products are computed
   // in parallel; lane pairs are (red,green) and (blue,alpha) rows.  The accumulation order matches the scalar
   // path below, keeping the output bit-identical across both implementations.

   __m128d col_rg[5], col_ba[5];
   for (int c=0; c < 5; c++) {
      col_rg[c] = _mm_set_pd(matrix[5+c], matrix[c]);
      col_ba[c] = _mm_set_pd(matrix[15+c], matrix[10+c]);
   }
   const __m128d half = _mm_set1_pd(0.5);
   const __m128d byte_max = _mm_set1_pd(255.0);

   for (int y=0; y < height; y++) {
      const uint8_t *pixel = in_line;
      uint8_t *out = out_line;
      for (int x=0; x < width; x++, pixel += 4, out += 4) {
         const auto a = pixel[A];
         const auto red = a ? pixel[R] : 0;
         const auto green = a ? pixel[G] : 0;
         const auto blue = a ? pixel[B] : 0;
         const __m128d r  = _mm_set1_pd(double(linear_rgb ? glLinearRGB.convert(red) : red));
         const __m128d g  = _mm_set1_pd(double(linear_rgb ? glLinearRGB.convert(green) : green));
         const __m128d b  = _mm_set1_pd(double(linear_rgb ? glLinearRGB.convert(blue) : blue));
         const __m128d av = _mm_set1_pd(double(a));

         __m128d rg = _mm_add_pd(half, _mm_mul_pd(r, col_rg[0]));
         rg = _mm_add_pd(rg, _mm_mul_pd(g, col_rg[1]));
         rg = _mm_add_pd(rg, _mm_mul_pd(b, col_rg[2]));
         rg = _mm_add_pd(rg, _mm_mul_pd(av, col_rg[3]));
         rg = _mm_add_pd(rg, col_rg[4]);

         __m128d ba = _mm_add_pd(half, _mm_mul_pd(r, col_ba[0]));
         ba = _mm_add_pd(ba, _mm_mul_pd(g, col_ba[1]));
         ba = _mm_add_pd(ba, _mm_mul_pd(b, col_ba[2]));
         ba = _mm_add_pd(ba, _mm_mul_pd(av, col_ba[3]));
         ba = _mm_add_pd(ba, col_ba[4]);

         // Clamp before conversion so that large matrix values still saturate to the byte range.
         rg = _mm_min_pd(_mm_max_pd(rg, _mm_setzero_pd()), byte_max);
         ba = _mm_min_pd(_mm_max_pd(ba, _mm_setzero_pd()), byte_max);

         // Truncation towards zero matches the scalar double-to-int conversion.

         const int r2 = _mm_cvttsd_si32(rg);
         const int g2 = _mm_cvttsd_si32(_mm_unpackhi_pd(rg, rg));
         const int b2 = _mm_cvttsd_si32(ba);
         const int a2 = _mm_cvttsd_si32(_mm_unpackhi_pd(ba, ba));

         out[A] = uint8_t(a2);
         out[R] = linear_rgb ? glLinearRGB.invert(uint8_t(r2)) : uint8_t(r2);
         out[G] = linear_rgb ? glLinearRGB.invert(uint8_t(g2)) : uint8_t(g2);
         out[B] = linear_rgb ? glLinearRGB.invert(uint8_t(b2)) : uint8_t(b2);
      }
      out_line += Self->Target->LineWidth;
      in_line += in_bmp->LineWidth;
   }
#else
   for (int y=0; y < height; y++) {
      const uint8_t *pixel = in_line;
      uint8_t *out = out_line;
      for (int x=0; x < width; x++, pixel += 4, out += 4) {
         double a = pixel[A];
         const auto red = a ? pixel[R] : 0;
         const auto green = a ? pixel[G] : 0;
         const auto blue = a ? pixel[B] : 0;
         double r = linear_rgb ? glLinearRGB.convert(red) : red;
         double g = linear_rgb ? glLinearRGB.convert(green) : green;
         double b = linear_rgb ? glLinearRGB.convert(blue) : blue;

         double r2 = 0.5 + (r * matrix[0]) + (g * matrix[1]) + (b * matrix[2]) + (a * matrix[3]) + matrix[4];
         double g2 = 0.5 + (r * matrix[5]) + (g * matrix[6]) + (b * matrix[7]) + (a * matrix[8]) + matrix[9];
         double b2 = 0.5 + (r * matrix[10]) + (g * matrix[11]) + (b * matrix[12]) + (a * matrix[13]) + matrix[14];
         double a2 = 0.5 + (r * matrix[15]) + (g * matrix[16]) + (b * matrix[17]) + (a * matrix[18]) + matrix[19];

         const auto red_byte = uint8_t(std::clamp(r2, 0.0, 255.0));
         const auto green_byte = uint8_t(std::clamp(g2, 0.0, 255.0));
         const auto blue_byte = uint8_t(std::clamp(b2, 0.0, 255.0));
         out[A] = uint8_t(std::clamp(a2, 0.0, 255.0));
         out[R] = linear_rgb ? glLinearRGB.invert(red_byte) : red_byte;
         out[G] = linear_rgb ? glLinearRGB.invert(green_byte) : green_byte;
         out[B] = linear_rgb ? glLinearRGB.invert(blue_byte) : blue_byte;
      }
      out_line += Self->Target->LineWidth;
      in_line += in_bmp->LineWidth;
   }
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR build_colour_matrix(CM Mode, const MATRIX &Values, MATRIX &Result)
{
   ColourMatrix matrix;

   switch (Mode) {
      case CM::SATURATE:        matrix.adjustSaturation(Values[0]); break;
      case CM::HUE_ROTATE:      matrix.rotateHue(Values[0]); break;
      case CM::LUMINANCE_ALPHA: matrix.luminance2Alpha(); break;
      case CM::CONTRAST:        matrix.adjustContrast(Values[0]); break;
      case CM::BRIGHTNESS:      matrix.adjustBrightness(Values[0]); break;
      case CM::HUE:             matrix.adjustHue(Values[0]); break;
      case CM::DESATURATE:      matrix.adjustSaturation(0); break;
      case CM::NONE:            break;
      case CM::COLOURISE:
         matrix.colourise(Values[0], Values[1], Values[2], Values[3] < 0.001 ? 1.0 : Values[3]);
         break;
      case CM::MATRIX:
         matrix.matrix = Values;
         // User matrices use normalised offsets; internal presets already use byte units.
         for (int i=4; i < CM_SIZE; i += 5) matrix[i] *= 255.0;
         break;
      default: return ERR::InvalidValue;
   }

   // Keep the sum of four byte products and the offset within the range of a double.
   constexpr double max_coefficient = std::numeric_limits<double>::max() / 2048.0;
   for (double coefficient : matrix.matrix) {
      if (not (std::abs(coefficient) <= max_coefficient)) return ERR::OutOfRange;
   }
   Result = matrix.matrix;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COLOURFX_Init(extColourFX *Self)
{
   if (Self->SourceType IS VSF::NIL) return ERR::FieldNotSet;
   return build_colour_matrix(Self->Mode, Self->Values, Self->Matrix);
}

//********************************************************************************************************************

static ERR set_colour_adjustment(extColourFX *Self, CM Mode, std::span<const double> Values, bool ValidateCount)
{
   if (Values.size() > CM_SIZE) return ERR::BufferOverflow;

   if (ValidateCount) {
      switch (Mode) {
         case CM::MATRIX:
            if (Values.size() != CM_SIZE) return ERR::Mismatch;
            break;
         case CM::SATURATE:
         case CM::HUE_ROTATE:
         case CM::CONTRAST:
         case CM::BRIGHTNESS:
         case CM::HUE:
            if (Values.size() != 1) return ERR::Mismatch;
            break;
         case CM::COLOURISE:
            if ((Values.size() != 3) and (Values.size() != 4)) return ERR::Mismatch;
            break;
         default: break;
      }
   }

   MATRIX values {};
   for (size_t i=0; i < Values.size(); i++) {
      if (not std::isfinite(Values[i])) return ERR::InvalidValue;
      values[i] = Values[i];
   }

   if (ValidateCount) {
      auto error = build_colour_matrix(Mode, values, Self->Matrix);
      if (error != ERR::Okay) return error;
   }
   Self->Mode = Mode;
   Self->Values = values;
   Self->TotalValues = Values.size();
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetAdjustment: Replaces the mode and values together.

The supplied values must match the selected mode as described in #Values.  If validation fails, the previous
adjustment remains active.

-INPUT-
int(CM) Mode: The colour adjustment to apply.
vector(double) Values: The parameters for the adjustment.

-ERRORS-
Okay
NullArgs
InvalidValue
BufferOverflow
Mismatch
OutOfRange

-END-

*********************************************************************************************************************/

static ERR COLOURFX_SetAdjustment(extColourFX *Self, struct cfx::SetAdjustment *Args)
{
   if ((not Args) or (not Args->Values)) return ERR::NullArgs;
   return set_colour_adjustment(Self, Args->Mode, *Args->Values, true);
}

/*********************************************************************************************************************

-FIELD-
Mode: Defines the algorithm that will process the input source.
Lookup: CM

After initialisation, changing Mode requires the current #Values to match the new mode.  #SetAdjustment() changes
both fields in one call.

-FIELD-
Values: A list of input values for the algorithm defined by #Mode.

At runtime, MATRIX requires 20 finite values in row-major RGBA order, with normalised offsets.  SATURATE,
HUE_ROTATE, CONTRAST, BRIGHTNESS and HUE each take one finite value.  COLOURISE takes three colour values with
an optional fourth strength value.  NONE, DESATURATE and LUMINANCE_ALPHA ignore the values.

*********************************************************************************************************************/

static ERR COLOURFX_GET_Values(extColourFX *Self, std::span<double> &Array)
{
   Array = std::span<double>(Self->Values.data(), Self->TotalValues);
   return ERR::Okay;
}

static ERR COLOURFX_SET_Values(extColourFX *Self, std::span<const double> &Array)
{
   return set_colour_adjustment(Self, Self->Mode, Array, Self->initialised());
}

//********************************************************************************************************************

static ERR COLOURFX_SET_Mode(extColourFX *Self, CM Value)
{
   if ((Value < CM::NONE) or (Value > CM::COLOURISE)) return ERR::InvalidValue;
   if (Self->Mode IS Value) return ERR::Okay;
   if (Self->initialised()) {
      return set_colour_adjustment(Self, Value, std::span(Self->Values.data(), size_t(Self->TotalValues)), true);
   }
   Self->Mode = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR colour_xml(const extColourFX &Effect, filter_xml_element &Element)
{
   Element.Name = "feColorMatrix";
   int standard_count = 0;
   bool extension = false;
   switch (Effect.Mode) {
      case CM::MATRIX: Element.attribute("type", "matrix"); standard_count = CM_SIZE; break;
      case CM::SATURATE: Element.attribute("type", "saturate"); standard_count = 1; break;
      case CM::HUE_ROTATE: Element.attribute("type", "hueRotate"); standard_count = 1; break;
      case CM::LUMINANCE_ALPHA: Element.attribute("type", "luminanceToAlpha"); break;
      case CM::CONTRAST: Element.attribute("xvg:type", "contrast"); extension = true; break;
      case CM::BRIGHTNESS: Element.attribute("xvg:type", "brightness"); extension = true; break;
      case CM::HUE: Element.attribute("xvg:type", "hue"); extension = true; break;
      case CM::DESATURATE: Element.attribute("xvg:type", "desaturate"); extension = true; break;
      case CM::COLOURISE: Element.attribute("xvg:type", "colourise"); extension = true; break;
      case CM::NONE: Element.attribute("xvg:type", "none"); extension = true; break;
      default: return ERR::InvalidValue;
   }

   if (standard_count) Element.list("values", std::span(Effect.Values.data(), size_t(standard_count)));
   if (extension or (Effect.TotalValues != standard_count)) {
      // Preserve the authored array length, including entries unused by the selected mode.
      Element.list("xvg:values", std::span(Effect.Values.data(), size_t(Effect.TotalValues)));
   }
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_colourmatrix_def.c"

static const FieldArray clColourFXFields[] = {
   { "Mode",   FDF_INT|FDF_LOOKUP|FDF_RW,  nullptr, COLOURFX_SET_Mode, &clColourFXCM },
   { "Values", FDF_VIRTUAL|FDF_DOUBLE|FDF_ARRAY|FDF_RW|FDF_PURE, COLOURFX_GET_Values, COLOURFX_SET_Values },
   { "XMLDef", FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE,  FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_colourfx(void)
{
   clColourFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::COLOURFX),
      fl::Name("ColourFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clColourFXActions),
      fl::Methods(clColourFXMethods),
      fl::Fields(clColourFXFields),
      fl::Size(sizeof(extColourFX)),
      fl::Path(MOD_PATH));

   return clColourFX ? ERR::Okay : ERR::AddClass;
}
