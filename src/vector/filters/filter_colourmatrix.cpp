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
defined prior to rendering.

SVG requires that the calculations are performed on non-premultiplied colour values.  If the input graphics consists
of premultiplied colour values, those values are automatically converted into non-premultiplied colour values for
this operation.

*********************************************************************************************************************/

#include <array>

#define CM_SIZE 20

static const double LUMA_R = 0.2125; // These values are as documented in W3C SVG
static const double LUMA_G = 0.7154;
static const double LUMA_B = 0.0721;

static const double ONETHIRD = 1.0 / 3.0;

typedef std::array<double, CM_SIZE> MATRIX;

static const MATRIX IDENTITY = { 1,0,0,0,0,   0,1,0,0,0,   0,0,1,0,0,   0,0,0,1,0 };

class ColourMatrix {
public:
   MATRIX matrix;
   ColourMatrix *preHue;
   ColourMatrix *postHue;

   ColourMatrix(const ColourMatrix &Copy) : preHue(0), postHue(0) {
      matrix = Copy.matrix;
   }

   ColourMatrix(const MATRIX Matrix) : preHue(0), postHue(0) {
      matrix = Matrix;
   }

   ColourMatrix(const double *Values) : preHue(0), postHue(0) {
      for (auto i=0; i < CM_SIZE; i++) matrix[i] = Values[i];
   }

   ColourMatrix() : preHue(nullptr), postHue(nullptr) {
      reset();
   }

   ~ColourMatrix() {
      delete preHue;
      delete postHue;
   }

   void reset() {
      matrix = IDENTITY;
   }

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

   void rotateHue(double degrees) {
      if (initHue() IS ERR::Okay) {
         apply(preHue->matrix);
         rotateBlue(degrees);
         apply(postHue->matrix);
      }
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

   ERR initHue() {
      const double greenRotation = 39.182655;
      uint8_t init = false;

      if (!init) {
         preHue = new (std::nothrow) ColourMatrix();
         postHue = new (std::nothrow) ColourMatrix();
         if ((!preHue) or (!postHue)) return ERR::AllocMemory;

         preHue->rotateRed(45);
         preHue->rotateGreen(-greenRotation);

         std::array<double, 4> lum = { LUMA_R, LUMA_G, LUMA_B, 1.0 };
         preHue->transformVector(lum);

         double red = lum[0] / lum[2];
         double green = lum[1] / lum[2];

         preHue->shearBlue(red, green);

         postHue->shearBlue(-red, -green);
         postHue->rotateGreen(greenRotation);
         postHue->rotateRed(-45.0);

         init = true;
      }
      return ERR::Okay;
   }
};

//********************************************************************************************************************

class extColourFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COLOURFX;
   static constexpr CSTRING CLASS_NAME = "ColourFX";
   using create = pf::Create<extColourFX>;

   double Values[CM_SIZE];
   ColourMatrix *Matrix;
   int TotalValues;
   CM Mode;
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR COLOURFX_Draw(extColourFX *Self, struct acDraw *Args)
{
   if (Self->Target->BytesPerPixel != 4) return ERR::InvalidState;
   if (!Self->Matrix) return ERR::FieldNotSet;

   const uint8_t A = Self->Target->ColourFormat->AlphaPos>>3;
   const uint8_t R = Self->Target->ColourFormat->RedPos>>3;
   const uint8_t G = Self->Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->Target->ColourFormat->BluePos>>3;

   ColourMatrix &matrix = *Self->Matrix;

   objBitmap *inBmp;
   if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;

   auto out_line = Self->Target->Data + (Self->Target->Clip.Left<<2) + (Self->Target->Clip.Top * Self->Target->LineWidth);
   auto in_line  = inBmp->Data + (inBmp->Clip.Left<<2) + (inBmp->Clip.Top * inBmp->LineWidth);

   for (int y=0; y < inBmp->Clip.Bottom - inBmp->Clip.Top; y++) {
      uint8_t *pixel = in_line;
      uint8_t *out = out_line;
      for (int x=0; x < inBmp->Clip.Right - inBmp->Clip.Left; x++, pixel += 4, out += 4) {
         double a = pixel[A];
         if (a) {
            double r = glLinearRGB.convert(pixel[R]);
            double g = glLinearRGB.convert(pixel[G]);
            double b = glLinearRGB.convert(pixel[B]);

            int r2 = 0.5 + (r * matrix[0]) + (g * matrix[1]) + (b * matrix[2]) + (a * matrix[3]) + matrix[4];
            int g2 = 0.5 + (r * matrix[5]) + (g * matrix[6]) + (b * matrix[7]) + (a * matrix[8]) + matrix[9];
            int b2 = 0.5 + (r * matrix[10]) + (g * matrix[11]) + (b * matrix[12]) + (a * matrix[13]) + matrix[14];
            int a2 = 0.5 + (r * matrix[15]) + (g * matrix[16]) + (b * matrix[17]) + (a * matrix[18]) + matrix[19];

            if (a2 < 0) out[A] = 0;
            else if (a2 > 255) out[A] = 255;
            else out[A] = a2;

            if (r2 < 0)   out[R] = 0;
            else if (r2 > 255) out[R] = glLinearRGB.invert(255);
            else out[R] = glLinearRGB.invert(r2);

            if (g2 < 0) out[G] = 0;
            else if (g2 > 255) out[G] = glLinearRGB.invert(255);
            else out[G] = glLinearRGB.invert(g2);

            if (b2 < 0) out[B] = 0;
            else if (b2 > 255) out[B] = glLinearRGB.invert(255);
            else out[B] = glLinearRGB.invert(b2);
         }
      }
      out_line += Self->Target->LineWidth;
      in_line += inBmp->LineWidth;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COLOURFX_Free(extColourFX *Self)
{
   if (Self->Matrix) { delete Self->Matrix; Self->Matrix = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COLOURFX_Init(extColourFX *Self)
{
   pf::Log log;

   if (Self->SourceType IS VSF::NIL) return log.warning(ERR::UndefinedField);

   // If a special colour mode was selected, convert the provided value(s) to the matrix format.

   ColourMatrix *matrix;

   switch (Self->Mode) {
      case CM::SATURATE:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->adjustSaturation(Self->Values[0]);
         break;

      case CM::HUE_ROTATE:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->rotateHue(Self->Values[0]);
         break;

      case CM::LUMINANCE_ALPHA:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->luminance2Alpha();
         break;

      case CM::CONTRAST:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->adjustContrast(Self->Values[0]);
         break;

      case CM::BRIGHTNESS:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->adjustBrightness(Self->Values[0]);
         break;

      case CM::HUE:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->adjustHue(Self->Values[0]);
         break;

      case CM::COLOURISE:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->colourise(Self->Values[0], Self->Values[1], Self->Values[2], Self->Values[3] < 0.001 ? 1.0 : Self->Values[3]);
         break;

      case CM::DESATURATE:
         matrix = new (std::nothrow) ColourMatrix();
         if (matrix) matrix->adjustSaturation(0);
         break;

      case CM::NONE: // Accept default of identity matrix
         matrix = new (std::nothrow) ColourMatrix();
         break;

      default:
         matrix = new (std::nothrow) ColourMatrix(Self->Values);
         break;
   }

   if (!matrix) return log.warning(ERR::AllocMemory);
   else Self->Matrix = matrix;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COLOURFX_NewObject(extColourFX *Self)
{
   // Configure identity matrix
   Self->Values[0] = 1;
   Self->Values[6] = 1;
   Self->Values[12] = 1;
   Self->Values[18] = 1;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Mode: Defines the algorithm that will process the input source.
Lookup: CM

*********************************************************************************************************************/

static ERR COLOURFX_GET_Mode(extColourFX *Self, CM *Value)
{
   *Value = Self->Mode;
   return ERR::Okay;
}

static ERR COLOURFX_SET_Mode(extColourFX *Self, CM Value)
{
   Self->Mode = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Values: A list of input values for the algorithm defined by #Mode.

The meaning of the input values is dependent on the selected #Mode.  Each mode documents the total number of values
that must be defined for them to work properly.

When values are not defined, they default to 0.

*********************************************************************************************************************/

static ERR COLOURFX_GET_Values(extColourFX *Self, double **Array, int *Elements)
{
   *Array = Self->Values;
   *Elements = Self->TotalValues;
   return ERR::Okay;
}

static ERR COLOURFX_SET_Values(extColourFX *Self, double *Array, int Elements)
{
   if (Elements > std::ssize(Self->Values)) return ERR::InvalidValue;
   if (Array) copymem(Array, Self->Values, Elements * sizeof(double));
   clearmem(Self->Values + Elements, (std::ssize(Self->Values) - Elements) * sizeof(double));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR COLOURFX_GET_XMLDef(extColourFX *Self, STRING *Value)
{
   std::stringstream stream;

   stream << "feColorMatrix";

   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_colourmatrix_def.c"

static const FieldDef clMode[] = {
   { "None",           CM::NONE },
   { "Saturate",       CM::SATURATE },
   { "HueRotate",      CM::HUE_ROTATE },
   { "LuminanceAlpha", CM::LUMINANCE_ALPHA },
   { "Contrast",       CM::CONTRAST },
   { "Brightness",     CM::BRIGHTNESS },
   { "Hue",            CM::HUE },
   { "Desaturate",     CM::DESATURATE },
   { "Colourise",      CM::COLOURISE },
   { nullptr, 0 }
};

static const FieldArray clColourFXFields[] = {
   { "Mode",   FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RI,  COLOURFX_GET_Mode, COLOURFX_SET_Mode, &clMode },
   { "Values", FDF_VIRTUAL|FDF_DOUBLE|FDF_ARRAY|FDF_RI, COLOURFX_GET_Values, COLOURFX_SET_Values },
   { "XMLDef", FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R,  COLOURFX_GET_XMLDef },
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
      fl::Fields(clColourFXFields),
      fl::Size(sizeof(extColourFX)),
      fl::Path(MOD_PATH));

   return clColourFX ? ERR::Okay : ERR::AddClass;
}
