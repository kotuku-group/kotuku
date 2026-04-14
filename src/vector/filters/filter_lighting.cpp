/*********************************************************************************************************************

The light-source rendering code is copyright 2012 The Android Open Source Project.  The use of that source code is
governed as follows.

Copyright (c) 2011 Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above copyright  notice, this list of conditions and the following
disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

**********************************************************************************************************************

-CLASS-
LightingFX: Enables the application of lighting effects.

The lighting effect class applies a diffuse or specular lighting effect to the alpha channel of an input bitmap, which
functions as a bump map.  The output is an RGBA representation of the light effect.  If no light #Colour is specified
by the client then the output will be in grey scale.

For diffuse lighting, the resulting RGBA image is computed as follows:

<pre>
Dr = kd * N.L * Lr
Dg = kd * N.L * Lg
Db = kd * N.L * Lb
Da = 1.0
</pre>

where

<pre>
kd = Diffuse lighting constant
N  = Surface normal unit vector, a function of x and y
L  = Unit vector pointing from surface to light, a function of x and y in the point and spot light cases
Lr,Lg,Lb = RGB components of light, a function of x and y in the spot light case
</pre>

For specular lighting, the resulting RGBA image is computed as follows:

<pre>
Sr = ks * pow(N.H, specularExponent) * Lr
Sg = ks * pow(N.H, specularExponent) * Lg
Sb = ks * pow(N.H, specularExponent) * Lb
Sa = max(Sr, Sg, Sb)
</pre>

where

<pre>
ks = Specular lighting constant
N  = Surface normal unit vector, a function of x and y
H  = "Halfway" unit vector between eye unit vector and light unit vector

Lr,Lg,Lb = RGB components of light
</pre>

The definition of `H` reflects our assumption of the constant eye vector `E = (0,0,1)`:

<pre>
H = (L + E) / Norm(L + E)
</pre>

where `L` is the light unit vector.

-END-

*********************************************************************************************************************/

#define USE_SIMD 1

#ifdef USE_SIMD
#include <immintrin.h>
#endif

#ifdef _MSC_VER
    #include <xmmintrin.h>
    #define PREFETCH(ptr) _mm_prefetch((char*)(ptr), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
    #define PREFETCH(ptr) __builtin_prefetch(ptr, 0, 3)
#else
    #define PREFETCH(ptr) // No-op for other compilers
#endif

class point3 {
   public:
   double  x, y, z;

   point3(double X, double Y, double Z) : x(X), y(Y), z(Z) {};
   point3() {};

   void normalise() {
      double len_sq = dot(*this); // Compute the length squared of the vector
      if (std::abs(len_sq - 1.0) < 1e-6) return; // Already normalised
      double scale = fast_inv_sqrt(len_sq); //  1.0 / sqrt(len_sq);
      x = x * scale;
      y = y * scale;
      z = z * scale;
   }

   friend point3 operator-(const point3 &a, const point3 &b) {
      return { a.x - b.x, a.y - b.y, a.z - b.z };
   }

   friend point3 operator+(const point3 &a, const point3 &b) {
      return { a.x + b.x, a.y + b.y, a.z + b.z };
   }

   double dot(const point3 &vec) const { // Compute the dot product between vectors
      return (this->x * vec.x) + (this->y * vec.y) + (this->z * vec.z);
   }
};

constexpr double ONE_THIRD   = 1.0 / 3.0;
constexpr double TWO_THIRDS  = 2.0 / 3.0;
constexpr double ONE_HALF    = 0.5;
constexpr double ONE_QUARTER = 0.25;

//********************************************************************************************************************

class extLightingFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::LIGHTINGFX;
   static constexpr CSTRING CLASS_NAME = "LightingFX";
   using create = pf::Create<extLightingFX>;

   FRGB   Colour;           // Colour of the light source.
   FRGB   LinearColour;     // Colour of the light source in linear sRGB space.
   double SpecularExponent; // Exponent value for specular lighting only.
   double MapHeight;        // Maximum height of the surface for bump map calculations.
   double Constant;         // The ks/kd constant value for the light mode.
   double UnitX, UnitY;     // SVG kernel unit - scale value for X/Y
   double X, Y, Z;          // Position of light source.
   LT     Type;             // Diffuse or Specular light scattering
   LS     LightSource;      // Light source identifier, recorded for SVG output purposes only.

   // DISTANT LIGHT
   double Azimuth, Elevation;    // Distant light
   point3 Direction;             // Pre-calculated value for distant light.

   // SPOT LIGHT
   double SpotExponent, ConeAngle; // Spot light
   point3 Spotlight;              // Position of spot light source.
   double CosInnerConeAngle;
   double CosOuterConeAngle;
   double ConeScale;
   point3 SpotDelta;

   extLightingFX() {
      SpecularExponent = 1.0;
      Colour    = { 1.0, 1.0, 1.0, 1.0 };
      LinearColour = { 1.0, 1.0, 1.0, 1.0 };
      Type      = LT::DIFFUSE;
      Constant  = 1.0;
      MapHeight = 1.0;
      UnitX     = 1.0;
      UnitY     = 1.0;
   }

   // Shift matrix components to the left, as we advance pixels to the right.

   constexpr void shiftMatrixLeft(std::array<uint8_t, 9> &m) {
      m[0] = m[1];
      m[3] = m[4];
      m[6] = m[7];
      m[1] = m[2];
      m[4] = m[5];
      m[7] = m[8];
   }

   // Prefetch the next row of data to improve cache performance

   inline void prefetchNextRow(const uint8_t* next_row, int width, uint8_t bpp) {
      for (int i = 0; i < width * bpp; i += 64) { // 64-byte cache lines
         PREFETCH(next_row + i);
      }
   }

   constexpr double sobel(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, double MapHeight) {
      return (double(-a + b - 2 * c + 2 * d -e + f)) * MapHeight;
   }

   inline point3 pointToNormal(double x, double y, double MapHeight) {
      point3 vector(-x * MapHeight, -y * MapHeight, 1.0);
      vector.normalise();
      return vector;
   }

   inline point3 leftNormal(const std::array<uint8_t, 9> &m, const double MapHeight) {
      return pointToNormal(sobel(m[1], m[2], m[4], m[5], m[7], m[8], ONE_HALF),
                           sobel(0, 0, m[1], m[7], m[2], m[8], ONE_THIRD),
                           MapHeight);
   }


   inline point3 interiorNormal(const std::array<uint8_t, 9> &m, const double MapHeight) {
      return pointToNormal(sobel(m[0], m[2], m[3], m[5], m[6], m[8], ONE_QUARTER),
                           sobel(m[0], m[6], m[1], m[7], m[2], m[8], ONE_QUARTER),
                           MapHeight);
   }

   inline point3 rightNormal(const std::array<uint8_t, 9> &m, const double MapHeight) {
       return pointToNormal(sobel(m[0], m[1], m[3], m[4], m[6], m[7], ONE_HALF),
                            sobel(m[0], m[6], m[1], m[7], 0, 0, ONE_THIRD),
                            MapHeight);
   }

   void draw();
   FRGB colour_spot_light(point3 &Point);
   inline void diffuse_light(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t);
   inline void specular_light(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t);
   void render_distant(int, int, objBitmap *, const point3 &, double, int, int);
   void render_spotlight(int, int, objBitmap *, const point3 &, double, int, int);
   void render_point(int, int, objBitmap *, const point3 &, double, int, int);
};

//********************************************************************************************************************
// Colour computation for spot light.  Resulting RGB values are 0 - 1.0

FRGB extLightingFX::colour_spot_light(point3 &Point)
{
   if (ConeAngle) {
      const double cosAngle = -Point.dot(SpotDelta);
      if (cosAngle < CosOuterConeAngle) return FRGB(0.0, 0.0, 0.0, 1.0);

      double scale = pow(cosAngle, SpotExponent);
      if (cosAngle < CosInnerConeAngle) {
         scale = scale * (cosAngle - CosOuterConeAngle);
         scale *= ConeScale;
      }
      return FRGB(LinearColour.Red * scale, LinearColour.Green * scale, LinearColour.Blue * scale, LinearColour.Alpha * scale);
   }
   else {
      const double scale = pow(-Point.dot(SpotDelta), SpotExponent);
      return FRGB(LinearColour.Red * scale, LinearColour.Green * scale, LinearColour.Blue * scale, LinearColour.Alpha * scale);
   }
}

//********************************************************************************************************************
// Specular/Diffuse drawing functions.

inline void extLightingFX::diffuse_light(const point3 &Normal, const point3 &STL, const FRGB &Colour, uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
   double scale = std::clamp((Constant * Normal.dot(STL)) * 255.0, 0.0, 255.0);

   Output[R] = glLinearRGB.invert(int(Colour.Red * scale));
   Output[G] = glLinearRGB.invert(int(Colour.Green * scale));
   Output[B] = glLinearRGB.invert(int(Colour.Blue * scale));
   Output[A] = 255;
}

inline void extLightingFX::specular_light(const point3 &Normal, const point3 &STL, const FRGB &Colour, uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
   point3 halfDir(STL);
   halfDir.z += 1.0; // Eye position is always (0, 0, 1)
   halfDir.normalise();

   double scale = std::clamp((Constant * std::pow(Normal.dot(halfDir), SpecularExponent)) * 255.0, 0.0, 255.0);

   const uint8_t r = int(Colour.Red * scale);
   const uint8_t g = int(Colour.Green * scale);
   const uint8_t b = int(Colour.Blue * scale);

   Output[R] = glLinearRGB.invert(r);
   Output[G] = glLinearRGB.invert(g);
   Output[B] = glLinearRGB.invert(b);
   if (LightSource IS LS::DISTANT) {
      // Alpha is chosen from the max of the linear R,G,B light value
      Output[A] = Output[R] > Output[G] ? (Output[R] > Output[B] ? Output[R] : Output[B]) : (Output[G] > Output[B] ? Output[G] : Output[B]);
   }
   else Output[A] = r > g ? (r > b ? r : b) : (g > b ? g : b);
}

//********************************************************************************************************************

void extLightingFX::render_distant(int StartY, int EndY, objBitmap *Bitmap, const point3 &Light, double spot_height, int Width, int Height)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;
   const auto map_height = spot_height * (1.0 / 255.0); // Normalise the map height to 0 - 1.0

   auto input_base = (uint8_t *)(Bitmap->Data + (Bitmap->Clip.Left * bpp) + (Bitmap->Clip.Top * Bitmap->LineWidth));
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   for (int y=StartY; y < EndY; ++y) {
      uint8_t *input_row = input_base + y * Bitmap->LineWidth;
      uint8_t *dest_row = dest_base + y * Target->LineWidth;

      // Prefetch the next few rows while processing current row
      if (y + 2 < std::min(Height, EndY)) prefetchNextRow(input_base + (y + 2) * Bitmap->LineWidth, Width, Bitmap->BytesPerPixel);
      if (y + 3 < std::min(Height, EndY)) prefetchNextRow(input_base + (y + 3) * Bitmap->LineWidth, Width, Bitmap->BytesPerPixel);

      const uint8_t *row0 = (y IS 0) ? input_row : input_row - Bitmap->LineWidth;
      const uint8_t *row1 = input_row;
      const uint8_t *row2 = (y IS Height-1) ? input_row : input_row + Bitmap->LineWidth;

      auto dptr = dest_row;
      std::array<uint8_t, 9> m;

      // Initialize matrix for first pixel
      m[1] = row0[A]; row0 += bpp;
      m[2] = row0[A]; row0 += bpp;
      m[4] = row1[A]; row1 += bpp;
      m[5] = row1[A]; row1 += bpp;
      m[7] = row2[A]; row2 += bpp;
      m[8] = row2[A]; row2 += bpp;

      if (Type IS LT::DIFFUSE) {
         // Process left edge pixel
         diffuse_light(leftNormal(m, map_height), Direction, LinearColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            diffuse_light(interiorNormal(m, map_height), Direction, LinearColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         // Process right edge pixel
         if (Width > 1) {
            shiftMatrixLeft(m);
            diffuse_light(rightNormal(m, map_height), Direction, LinearColour, dptr, R, G, B, A);
         }
      }
      else { // SPECULAR
         specular_light(leftNormal(m, map_height), Direction, LinearColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            specular_light(interiorNormal(m, map_height), Direction, LinearColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            specular_light(rightNormal(m, map_height), Direction, LinearColour, dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

void extLightingFX::render_spotlight(int StartY, int EndY, objBitmap *Bitmap, const point3 &Light, double spot_height, int Width, int Height)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;
   const auto map_height = spot_height * (1.0 / 255.0); // Normalise the map height to 0 - 1.0

   auto input_base = (uint8_t *)(Bitmap->Data + (Bitmap->Clip.Left * bpp) + (Bitmap->Clip.Top * Bitmap->LineWidth));
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   // Compute the light direction vector based on the light source position and the alpha value of the pixel.
   auto read_light_delta = [&Light, spot_height](double X, double Y, uint8_t alpha_val) -> point3 {
      point3 direction(Light.x - X, Light.y - Y, Light.z - (double(alpha_val) * (1.0 / 255.0) * spot_height));
      direction.normalise();
      return direction;
   };

   for (int y=StartY; y < EndY; ++y) {
      uint8_t *input_row = input_base + y * Bitmap->LineWidth;
      uint8_t *dest_row = dest_base + y * Target->LineWidth;

      // Prefetch the next few rows while processing current row
      if (y + 2 < std::min(Height, EndY)) prefetchNextRow(input_base + (y + 2) * Bitmap->LineWidth, Width, Bitmap->BytesPerPixel);
      if (y + 3 < std::min(Height, EndY)) prefetchNextRow(input_base + (y + 3) * Bitmap->LineWidth, Width, Bitmap->BytesPerPixel);

      const uint8_t *row0 = (y IS 0) ? input_row : input_row - Bitmap->LineWidth;
      const uint8_t *row1 = input_row;
      const uint8_t *row2 = (y IS Height-1) ? input_row : input_row + Bitmap->LineWidth;

      auto dptr = dest_row;
      std::array<uint8_t, 9> m;

      m[1] = row0[A]; row0 += bpp;
      m[2] = row0[A]; row0 += bpp;
      m[4] = row1[A]; row1 += bpp;
      m[5] = row1[A]; row1 += bpp;
      m[7] = row2[A]; row2 += bpp;
      m[8] = row2[A]; row2 += bpp;

      if (Type IS LT::DIFFUSE) {
         point3 stl = read_light_delta(0, y, m[4]);
         diffuse_light(leftNormal(m, map_height), stl, colour_spot_light(stl), dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            diffuse_light(interiorNormal(m, map_height), stl, colour_spot_light(stl), dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            diffuse_light(rightNormal(m, map_height), stl, colour_spot_light(stl), dptr, R, G, B, A);
         }
      }
      else { // SPECULAR
         point3 stl = read_light_delta(0, y, m[4]);
         specular_light(leftNormal(m, map_height), stl, colour_spot_light(stl), dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            specular_light(interiorNormal(m, map_height), stl, colour_spot_light(stl), dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            specular_light(rightNormal(m, map_height), stl, colour_spot_light(stl), dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

void extLightingFX::render_point(int StartY, int EndY, objBitmap *Bitmap, const point3 &Light, double spot_height, int Width, int height)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;
   const auto map_height = spot_height * (1.0 / 255.0); // Normalise the map height to 0 - 1.0

   auto input_base = (uint8_t *)(Bitmap->Data + (Bitmap->Clip.Left * bpp) + (Bitmap->Clip.Top * Bitmap->LineWidth));
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   // Compute the light direction vector based on the light source position and the alpha value of the pixel.
   auto read_light_delta = [&Light, spot_height](double X, double Y, uint8_t alpha_val) -> point3 {
      point3 direction(Light.x - X, Light.y - Y, Light.z - (double(alpha_val) * (1.0 / 255.0) * spot_height));
      direction.normalise();
      return direction;
   };

   for (int y=StartY; y < EndY; ++y) {
      uint8_t *input_row = input_base + y * Bitmap->LineWidth;
      uint8_t *dest_row = dest_base + y * Target->LineWidth;

      // Prefetch the next few rows while processing current row
      if (y + 2 < std::min(height, EndY)) prefetchNextRow(input_base + (y + 2) * Bitmap->LineWidth, Width, Bitmap->BytesPerPixel);
      if (y + 3 < std::min(height, EndY)) prefetchNextRow(input_base + (y + 3) * Bitmap->LineWidth, Width, Bitmap->BytesPerPixel);

      const uint8_t *row0 = (y IS 0) ? input_row : input_row - Bitmap->LineWidth;
      const uint8_t *row1 = input_row;
      const uint8_t *row2 = (y IS height-1) ? input_row : input_row + Bitmap->LineWidth;

      auto dptr = dest_row;
      std::array<uint8_t, 9> m;

      m[1] = row0[A]; row0 += bpp;
      m[2] = row0[A]; row0 += bpp;
      m[4] = row1[A]; row1 += bpp;
      m[5] = row1[A]; row1 += bpp;
      m[7] = row2[A]; row2 += bpp;
      m[8] = row2[A]; row2 += bpp;

      if (Type IS LT::DIFFUSE) {
         point3 stl = read_light_delta(0, y, m[4]);
         diffuse_light(leftNormal(m, map_height), stl, LinearColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width-1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            diffuse_light(interiorNormal(m, map_height), stl, LinearColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         shiftMatrixLeft(m);
         stl = read_light_delta(Width-1, y, m[4]);
         diffuse_light(rightNormal(m, map_height), stl, LinearColour, dptr, R, G, B, A);
      }
      else { // SPECULAR
         point3 stl = read_light_delta(0, y, m[4]);
         specular_light(leftNormal(m, map_height), stl, LinearColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            specular_light(interiorNormal(m, map_height), stl, LinearColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            specular_light(rightNormal(m, map_height), stl, LinearColour, dptr, R, G, B, A);
         }
      }
   }
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR LIGHTINGFX_Draw(extLightingFX *Self, struct acDraw *Args)
{
   if (Self->Target->BytesPerPixel != 4) return ERR::InvalidState;
   Self->draw();
   return ERR::Okay;
}

//*********************************************************************************************************************

void extLightingFX::draw()
{
   auto lt = point3(X, Y, Z); // Light source
   auto pt = Spotlight; // Target for the light source, used by LS::SPOT only.

   if (Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      // Light source coordinates are expressed as relative to the client vector's bounding box in this mode.
      auto &client = Filter->ClientVector;
      const double c_width  = client->Bounds.width();
      const double c_height = client->Bounds.height();

      lt.x = (lt.x * c_width) + client->Bounds.left;
      lt.y = (lt.y * c_height) + client->Bounds.top;
      lt.z = lt.z * sqrt((c_width * c_width) + (c_height * c_height)) * SQRT2DIV2;

      if (LightSource IS LS::SPOT) {
         pt.x = (pt.x * c_width) + client->Bounds.left;
         pt.y = (pt.y * c_height) + client->Bounds.top;
         pt.z = pt.z * sqrt((c_width * c_width) + (c_height * c_height)) * SQRT2DIV2;
      }
   }

   auto &t = Filter->ClientVector->Transform;

   const double scale = (t.sx IS t.sy) ? t.sx : sqrt((t.sx*t.sx) + (t.sy*t.sy)) * SQRT2DIV2;

   lt.z *= scale;
   t.transform(&lt.x, &lt.y);

   // Rendering algorithm requires light source coordinates to be relative to the exposed bitmap.

   if (LightSource IS LS::SPOT) {
      t.transform(&pt.x, &pt.y);
      pt.z *= scale;

      // SpotDelta gives the center of the rendered light, expressed in relative coordinates 0 - 1.0

      SpotDelta = point3(pt.x, pt.y, pt.z) - point3(lt.x, lt.y, lt.z);

      if (SpotDelta.dot(SpotDelta) > 1e-10) SpotDelta.normalise();
      else SpotDelta = point3(0, 0, -1);

      if (ConeAngle) {
         CosOuterConeAngle = cos(ConeAngle * DEG2RAD);
         const double AA_THRESHOLD = 0.016;
         CosInnerConeAngle = CosOuterConeAngle + AA_THRESHOLD;
         ConeScale = 1.0 / AA_THRESHOLD;
      }
   }

   lt.x -= Target->Clip.Left; // Re-orient the light source coordinates to (0,0)
   lt.y -= Target->Clip.Top;

   objBitmap *bmp;
   if (get_source_bitmap(Filter, &bmp, SourceType, Input, false) != ERR::Okay) return;

   // Note! Linear conversion of the source bitmap is unnecessary because only the alpha channel is used.

   // The alpha channel of the source bitmap will function as the Z value for the bump map.  The RGB components
   // are ignored for input purposes.

   int width  = Target->Clip.Right - Target->Clip.Left;
   int height = Target->Clip.Bottom - Target->Clip.Top;
   if (bmp->Clip.Right - bmp->Clip.Left < width) width = bmp->Clip.Right - bmp->Clip.Left;
   if (bmp->Clip.Bottom - bmp->Clip.Top < height) height = bmp->Clip.Bottom - bmp->Clip.Top;

   const double spot_height = MapHeight * scale;

   const int num_threads = std::min(std::thread::hardware_concurrency(), static_cast<unsigned int>(height));
   const int min_rows_per_chunk = 4; // Minimum work per thread to avoid overhead
   const int chunk_size = std::max(min_rows_per_chunk, height / num_threads);
   const int num_chunks = (height + chunk_size - 1) / chunk_size;

   BS::thread_pool pool(num_threads);

   for (int chunk=0; chunk < num_chunks; ++chunk) {
      const int start_y = chunk * chunk_size;
      const int end_y = std::min(start_y + chunk_size, height);

      pool.detach_task([&, start_row = start_y, end_row = end_y]() {
         if (LightSource IS LS::DISTANT) {
            render_distant(start_row, end_row, bmp, lt, spot_height, width, height);
         }
         else if (LightSource IS LS::SPOT) {
            render_spotlight(start_row, end_row, bmp, lt, spot_height, width, height);
         }
         else if (LightSource IS LS::POINT) {
            render_point(start_row, end_row, bmp, lt, spot_height, width, height);
         }
      });
   }

   pool.wait();
}

//********************************************************************************************************************

static ERR LIGHTINGFX_Free(extLightingFX *Self)
{
   Self->~extLightingFX();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR LIGHTINGFX_NewPlacement(extLightingFX *Self)
{
   new (Self) extLightingFX;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetDistantLight: Configure lighting with a distant light source.

This method applies a distant light configuration to the lighting effect.  It will override any previously defined
light source setting.

A distant light can be thought of as like the light from the sun.  An infinite amount of parallel light rays travel
in the direction that the distant light points to.  Distant lights are handy when you want equal illumination on
objects in a scene.

-INPUT-
double Azimuth: Direction angle for the light source on the XY plane (clockwise), in degrees from the X axis.
double Elevation: Direction angle for the light source from the XY plane towards the Z axis, in degrees.  A positive value points towards the viewer of the content.

-RESULT-
Okay:
Args:
NullArgs:

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetDistantLight(extLightingFX *Self, struct lt::SetDistantLight *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Self->Azimuth     = Args->Azimuth;
   Self->Elevation   = Args->Elevation;
   Self->LightSource = LS::DISTANT;
   Self->Direction   = point3(cos(Self->Azimuth * DEG2RAD) * cos(Self->Elevation * DEG2RAD), sin(Self->Azimuth * DEG2RAD) * cos(Self->Elevation * DEG2RAD), sin(Self->Elevation * DEG2RAD));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetPointLight: Configure lighting with a pointed light source.

This method applies a pointed light configuration to the lighting effect.  It will override any previously defined
light source setting.

A point light sends light out from the specified (X, Y, Z) location equally in all directions.  A light bulb or open
flame is a good example of a point light.  The intensity of the light can be controlled by altering the alpha
component of the light #Colour.

-INPUT-
double X: X location for the light source.
double Y: Y location for the light source.
double Z: Z location for the light source.

-RESULT-
Okay:
Args:
NullArgs:

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetPointLight(extLightingFX *Self, struct lt::SetPointLight *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.function("Source: %.2fx%.2fx%.2f", Args->X, Args->Y, Args->Z);

   Self->LightSource = LS::POINT;

   Self->X = Args->X;
   Self->Y = Args->Y;
   Self->Z = Args->Z;

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetSpotLight: Configure lighting with a spot light source.

This method applies a spot light configuration to the lighting effect.  It will override any previously defined
light source setting.

A spot light beams light rays from the defined (X, Y, Z) position to the (PX, PY, PZ) position.  The Exponent and
ConeAngle work together to constrain the edge of the light projection.

-INPUT-
double X: X location for the light source.
double Y: Y location for the light source.
double Z: Z location for the light source.  The positive Z-axis comes out towards the person viewing the content and assuming that one unit along the Z-axis equals one unit in X and Y.
double PX: X location of the light source's target.
double PY: Y location of the light source's target.
double PZ: Z location of the light source's target.
double Exponent: Exponent value controlling the focus for the light source.
double ConeAngle: A limiting cone which restricts the region where the light is projected, or 0 to disable.  Specified in degrees.

-RESULT-
Okay:
Args:
NullArgs:

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetSpotLight(extLightingFX *Self, struct lt::SetSpotLight *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.function("Source: %.2fx%.2fx%.2f, Target: %.2fx%.2fx%.2f, Exp: %.2f, Cone Angle: %.2f", Args->X, Args->Y, Args->Z, Args->PX, Args->PY, Args->PZ, Args->Exponent, Args->ConeAngle);

   Self->LightSource = LS::SPOT;

   Self->X  = Args->X;
   Self->Y  = Args->Y;
   Self->Z  = Args->Z;
   Self->Spotlight.x = Args->PX;
   Self->Spotlight.y = Args->PY;
   Self->Spotlight.z = Args->PZ;

   Self->SpotExponent = Args->Exponent;
   Self->ConeAngle    = Args->ConeAngle;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Colour: Defines the colour of the light source.

Set the Colour field to define the colour of the light source.  The colour is defined as an array of four 32-bit
floating point values between 0 and 1.0.  The array elements consist of Red, Green, Blue and Alpha values in that
order.

If the algorithm supports it, the Alpha component defines the intensity of the light source.

The default colour is pure white, `1.0,1.0,1.0,1.0`.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Colour(extLightingFX *Self, float **Value, int *Elements)
{
   *Value = (float *)&Self->Colour;
   *Elements = 4;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Colour(extLightingFX *Self, float *Value, int Elements)
{
   if (Value) {
      if (Elements >= 1) Self->Colour.Red   = Value[0];
      if (Elements >= 2) Self->Colour.Green = Value[1];
      if (Elements >= 3) Self->Colour.Blue  = Value[2];
      if (Elements >= 4) Self->Colour.Alpha = Value[3];
      else Self->Colour.Alpha = 1;
   }
   else Self->Colour.Alpha = 0;

   Self->LinearColour = Self->Colour;
   glLinearRGB.convert(Self->LinearColour);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Constant: Specifies the ks/kd value in Phong lighting model.

In the Phong lighting model, this field specifies the kd value in diffuse mode, or ks value in specular mode.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Constant(extLightingFX *Self, double *Value)
{
   *Value = Self->Constant;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Constant(extLightingFX *Self, double Value)
{
   if (Value >= 0) {
      Self->Constant = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Exponent: Exponent for specular lighting, larger is more "shiny".  Ranges from 1.0 to 128.0.

This field defines the exponent value for specular lighting, within a range of 1.0 to 128.0.  The larger the value,
shinier the end result.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Exponent(extLightingFX *Self, double *Value)
{
   *Value = Self->SpecularExponent;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Exponent(extLightingFX *Self, double Value)
{
   if ((Value >= 1.0) and (Value <= 128.0)) {
      Self->SpecularExponent = Value;
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
Scale: The maximum height of the input surface (bump map) when the alpha input is 1.0.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Scale(extLightingFX *Self, double *Value)
{
   *Value = Self->MapHeight;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Scale(extLightingFX *Self, double Value)
{
   Self->MapHeight = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Type: Defines the type of surface light scattering, which can be specular or diffuse.
Lookup: LT

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Type(extLightingFX *Self, LT *Value)
{
   *Value = Self->Type;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Type(extLightingFX *Self, LT Value)
{
   Self->Type = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
UnitX: The intended distance in current filter units for dx in the surface normal calculation formulas.

Indicates the intended distance in current filter units (i.e. as determined by the value of PrimitiveUnits)
for dx in the surface normal calculation formulas.

By specifying value(s) for #UnitX, the kernel becomes defined in a scalable, abstract coordinate system.  If #UnitX is
not specified, the default value is one pixel in the offscreen bitmap, which is a pixel-based coordinate system, and
thus potentially not scalable.  For some level of consistency across display media and user agents, it is necessary
that a value be provided for at least one of ResX and #UnitX.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_UnitX(extLightingFX *Self, double *Value)
{
   *Value = Self->UnitX;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_UnitX(extLightingFX *Self, double Value)
{
   if (Value < 0) return ERR::InvalidValue;
   Self->UnitX = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
UnitY: The intended distance in current filter units for dy in the surface normal calculation formulas.

Indicates the intended distance in current filter units (i.e. as determined by the value of PrimitiveUnits)
for dy in the surface normal calculation formulas.

By specifying value(s) for #UnitY, the kernel becomes defined in a scalable, abstract coordinate system.  If #UnitY is
not specified, the default value is one pixel in the offscreen bitmap, which is a pixel-based coordinate system, and
thus potentially not scalable.  For some level of consistency across display media and user agents, it is necessary
that a value be provided for at least one of ResY and #UnitY.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_UnitY(extLightingFX *Self, double *Value)
{
   *Value = Self->UnitY;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_UnitY(extLightingFX *Self, double Value)
{
   if (Value < 0) return ERR::InvalidValue;
   Self->UnitY = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_XMLDef(extLightingFX *Self, STRING *Value)
{
   std::stringstream stream;
   std::string type(Self->Type IS LT::DIFFUSE ? "feDiffuseLighting" : "feSpecularLighting");

   // TODO
   stream << "<" << type << ">";
   stream << "</" << type << ">";
   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clLightingType[] = {
   { "Diffuse",  LT::DIFFUSE },
   { "Specular", LT::SPECULAR },
   { nullptr, 0 }
};

#include "filter_lighting_def.c"

static const FieldArray clLightingFXFields[] = {
   { "Colour",   FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FDF_RW,  LIGHTINGFX_GET_Colour, LIGHTINGFX_SET_Colour },
   { "Constant", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,          LIGHTINGFX_GET_Constant, LIGHTINGFX_SET_Constant },
   { "Exponent", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,          LIGHTINGFX_GET_Exponent, LIGHTINGFX_SET_Exponent },
   { "Scale",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,          LIGHTINGFX_GET_Scale, LIGHTINGFX_SET_Scale },
   { "Type",     FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW,  LIGHTINGFX_GET_Type, LIGHTINGFX_SET_Type, &clLightingType },
   { "UnitX",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,          LIGHTINGFX_GET_UnitX, LIGHTINGFX_SET_UnitX },
   { "UnitY",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,          LIGHTINGFX_GET_UnitY, LIGHTINGFX_SET_UnitY },
   { "XMLDef",   FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, LIGHTINGFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_lightingfx(void)
{
   clLightingFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::LIGHTINGFX),
      fl::Name("LightingFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clLightingFXActions),
      fl::Methods(clLightingFXMethods),
      fl::Fields(clLightingFXFields),
      fl::Size(sizeof(extLightingFX)),
      fl::Path(MOD_PATH));

   return clLightingFX ? ERR::Okay : ERR::AddClass;
}
