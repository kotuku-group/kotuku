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
by the client then the output will be in grey scale.  The `COMBINED` #Type, described below, lights an albedo instead
and produces a finished surface.

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

The light is computed in the colour space of the parent @VectorFilter.  In `SRGB` the light #Colour and the result
are sRGB values.  In `LINEAR_RGB` the light #Colour is converted to linear RGB and the result is encoded to sRGB for
storage, so a result of 0.5 is stored as approximately 0.735.

The SVG algorithm measures the slope of the bump map with a 3x3 kernel.  This softens fine detail and shows 8-bit
steps on gentle slopes.  When the @VectorScene.Compliance of the scene is `NONE` (the default), the enhanced pipeline
applies the features selected by the #Features field.  All of them are enabled by default:

<list type="bullet">
<li>`DEQUANTISE`: Heights are read as floating point values, and areas that vary by no more than one alpha level are
smoothed to remove terracing.</li>
<li>`SHARPEN`: Slopes are measured with a sharper gradient, which keeps more of the detail in features a few pixels
wide.  When the bump map is magnified so that a kernel unit spans more than one pixel, the gradient blends back to the
SVG kernel, reaching it at two pixels per unit.</li>
<li>`CAVITY`: Narrow creases and pits receive less light, in proportion to their curvature and the #Cavity strength.
When the bump map is magnified, curvature is measured at the scale of the kernel unit so that 8-bit steps are not
darkened.</li>
<li>`DETAIL`: Fine detail is given more relief than broad shapes, so that it remains visible at a surface scale that
suits the larger forms.  The #Detail field sets the strength.</li>
<li>`SHADOWS`: Distant lights cast shadows.  A point is unlit when the surface between it and the light rises above
the light's elevation, and the edge of the shadow softens with distance from the surface that casts it, at a rate set
by #Penumbra.</li>
<li>`SPECULAR_AA`: Specular highlights are widened where the surface varies within a pixel, so that high exponents
do not produce isolated bright pixels.</li>
</list>

Flat surfaces and broad shapes are lit the same as the SVG algorithm, but shadows can darken any surface that lies
beside a raised area.  Set the scene's Compliance to `SVG1` or `SVG2` for results that match the SVG specification
exactly, or set #Features to zero to use the SVG algorithm for this effect alone.  The enhanced pipeline is still used
if #Ambient or #Intensity is set.

An #Ambient light can be added to the diffuse result.  It is not blocked by shadows, but it is reduced by `CAVITY`.
The #Intensity of the light multiplies its #Colour.  For the diffuse and specular types the light is limited to full
strength before it is coloured, as the SVG algorithm does with #Constant, so a value above 1 cannot make a coloured
light brighter than full strength.  Neither field has an effect when the scene's Compliance is `SVG1` or `SVG2`.

The `COMBINED` type computes a lit surface in a single pass.  The #Mix input is the albedo, the colour of the surface,
and the result is

<pre>
albedo * (ambient + kd * N.L * light) + ks * pow(N.H, specularExponent) * light
</pre>

where kd is #Constant and ks is #SpecularConstant.  The value is clamped once, when it is written, so light that is
brighter than full strength keeps its colour through the albedo.  This is the result of multiplying the diffuse light
by the albedo and adding the specular light with @CompositeFX, but the surface is analysed once and no intermediate
result is limited to 8 bits.  Shadows and `CAVITY` reduce both light terms.  The alpha of the result is the albedo's
alpha, raised to the brightest channel of the specular light, so that highlights can extend over transparent areas of
the albedo.  If #Mix is not set, the albedo is opaque white and the result is the sum of the light terms.  The
`COMBINED` type always uses the enhanced pipeline, whatever the scene's Compliance, and has no SVG equivalent.

SVG documents can set #Features, #Cavity, #Detail, #Penumbra, #Ambient and #Intensity with `kotuku:` attributes, and
the `COMBINED` type is available as the `kotuku:feLighting` element; refer to the SVG class for details.

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

// Symmetric matrix that maps bitmap gradients to surface normal components.  The cross-axis term preserves the
// orientation of unequal kernel units when the client is rotated.  Heights are normalised to the 0 - 255 alpha range.

struct map_height {
   double x, y, xy;
};

// Default strengths of the enhanced features, also used to decide which values XMLDef writes.

constexpr double DEFAULT_CAVITY   = 0.25; // Occlusion per unit of curvature
constexpr double DEFAULT_DETAIL   = 1.5;  // Slope multiplier for fine detail
constexpr double DEFAULT_PENUMBRA = 2.0;  // Degrees either side of the light's elevation

// Colour of one albedo pixel in the filter's colour space, with straight alpha.

struct albedo_sample {
   float r, g, b, a;
};

//********************************************************************************************************************

class extLightingFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::LIGHTINGFX;
   static constexpr CSTRING CLASS_NAME = "LightingFX";
   using create = kt::Create<extLightingFX>;

   // The exported fields are declared first so that their concrete offsets line up with the field table.
   // The 8-byte aligned members precede the narrower Type field to avoid padding before a direct-access field.
   FRGB   Colour = { 1.0, 1.0, 1.0, 1.0 }; // Colour of the light source.
   double Constant = 1.0;                   // The ks/kd constant value for the light mode.
   double SpecularExponent = 1.0;           // Exponent value for specular lighting only (Exponent)
   double MapHeight = 1.0;                  // Maximum height of the surface for bump map calculations (Scale)
   double UnitX = 1.0, UnitY = 1.0;         // SVG kernel unit - scale value for X/Y
   LT     Type = LT::DIFFUSE;               // Diffuse or Specular light scattering

   FRGB   LinearColour = { 1.0, 1.0, 1.0, 1.0 }; // Colour of the light source in linear sRGB space.
   FRGB   LightColour = { 1.0, 1.0, 1.0, 1.0 };  // Colour of the light source in the filter's colour space.
   bool   Linear = false;                         // True if lighting is computed in linear RGB.
   double X, Y, Z;          // Position of light source.
   LS     LightSource;      // Light source identifier, recorded for SVG output purposes only.
   bool   UnitXProvided = false, UnitYProvided = false;
   LFX    Features = LFX::ALL;  // Enhanced lighting features, applied when the scene's Compliance is NONE
   double Cavity   = DEFAULT_CAVITY;   // Strength of the CAVITY feature
   double Detail   = DEFAULT_DETAIL;   // Strength of the DETAIL feature
   double Penumbra = DEFAULT_PENUMBRA; // Width of the shadow edge in degrees, for the SHADOWS feature
   double SpecularConstant = 1.0;      // ks for the COMBINED type; Constant is kd
   double Intensity = 1.0;             // Multiplier of the light's colour
   FRGB   Ambient = { 0, 0, 0, 0 };    // Ambient light, scaled by its alpha
   FRGB   AmbientLight = { 0, 0, 0, 0 }; // Ambient light in the filter's colour space, set by draw_enhanced()
   double Strength = 1.0;              // Constant x Intensity for the DIFFUSE and SPECULAR types, set per draw

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

   extLightingFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

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

   inline point3 pointToNormal(double X, double Y, const map_height &MapHeight) {
      point3 vector(-X * MapHeight.x - Y * MapHeight.xy, -X * MapHeight.xy - Y * MapHeight.y, 1.0);
      vector.normalise();
      return vector;
   }

   inline point3 leftNormal(const std::array<uint8_t, 9> &m, const map_height &MapHeight) {
      return pointToNormal(sobel(m[1], m[2], m[4], m[5], m[7], m[8], ONE_HALF),
                           sobel(0, 0, m[1], m[7], m[2], m[8], ONE_THIRD),
                           MapHeight);
   }


   inline point3 interiorNormal(const std::array<uint8_t, 9> &m, const map_height &MapHeight) {
      return pointToNormal(sobel(m[0], m[2], m[3], m[5], m[6], m[8], ONE_QUARTER),
                           sobel(m[0], m[6], m[1], m[7], m[2], m[8], ONE_QUARTER),
                           MapHeight);
   }

   inline point3 rightNormal(const std::array<uint8_t, 9> &m, const map_height &MapHeight) {
       return pointToNormal(sobel(m[0], m[1], m[3], m[4], m[6], m[7], ONE_HALF),
                            sobel(m[0], m[6], m[1], m[7], 0, 0, ONE_THIRD),
                            MapHeight);
   }

   // Returns the kernel unit matrix in bitmap coordinates.  An undefined unit is one unit of the client's
   // coordinate system, measured with the client's uniform Scale.  Bounding box units are relative to the filter's
   // bounds on the same terms as BlurFX, so that a blurred bump map lit with the same unit is independent of the
   // display resolution.

   map_height kernel_unit_pixels(double Scale) const {
      map_height result = { Scale, Scale, 0.0 };
      if (not (UnitXProvided or UnitYProvided)) return result;

      const auto &render = *Filter->Render;
      const auto &t = render.ToScene;
      const auto [scale_x, scale_y] = render.axis_scales();
      const auto [bound_x, bound_y] = render.primitive_scales(true);

      if (UnitXProvided) result.x = UnitX * bound_x * scale_x;
      if (UnitYProvided) result.y = UnitY * bound_y * scale_y;

      // Project the bitmap gradient onto each transformed kernel axis, apply its height, then map the normal back
      // to bitmap coordinates.  Axis lengths alone lose the rotation and leave the kernel aligned to the bitmap.

      const double xx = t.sx / scale_x, xy = t.shy / scale_x;
      const double yx = t.shx / scale_y, yy = t.sy / scale_y;
      return {
         result.x * xx * xx + result.y * yx * yx,
         result.x * xy * xy + result.y * yy * yy,
         result.x * xx * xy + result.y * yx * yy
      };
   }

   // The determinant of the kernel unit matrix is the product of the two unit lengths in pixels, so its square root
   // is the mean unit length.

   static double unit_length(const map_height &Unit) {
      return sqrt(std::abs((Unit.x * Unit.y) - (Unit.xy * Unit.xy)));
   }

   ERR draw();
   FRGB colour_spot_light(point3 &Point);
   inline void diffuse_light(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double Occlusion = 1.0);
   inline void specular_light(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double Occlusion = 1.0, double Exponent = 0);
   inline void encode_light(const FRGB &, double, uint8_t *, uint8_t, uint8_t, uint8_t);
   inline void enhanced_diffuse(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double);
   inline void enhanced_specular(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double, double);
   inline void enhanced_ambient(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double, double);
   inline void enhanced_combined(const point3 &, const point3 &, double, const point3 &, const FRGB &,
      const albedo_sample &, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t, double, double);
   inline uint8_t encode_channel(double, bool);
   void render_distant(int, int, const objBitmap *, const point3 &, double, const map_height &, int, int);
   void render_spotlight(int, int, const objBitmap *, const point3 &, double, const map_height &, int, int);
   void render_point(int, int, const objBitmap *, const point3 &, double, const map_height &, int, int);
   double reach() const;

   filter_sampling sampling() const {
      if ((not std::isfinite(UnitX)) or (not std::isfinite(UnitY))) {
         constexpr double invalid = std::numeric_limits<double>::quiet_NaN();
         return { filter_access::BOUNDED, invalid, invalid };
      }
      const double radius = reach();
      return { filter_access::BOUNDED, radius, radius };
   }

   // True if ambient light is added to the result.  The specular type ignores it.

   bool has_ambient() const {
      return (Type != LT::SPECULAR) and (Ambient.Alpha > 0) and
         ((Ambient.Red > 0) or (Ambient.Green > 0) or (Ambient.Blue > 0));
   }

   // True if the enhanced pipeline is used.  The combined type always uses it.  The other types use it when the scene
   // is not bound to an SVG standard and a feature or an extension of the light is in use.

   bool enhanced() const {
      if (Type IS LT::COMBINED) return true;
      if ((not Filter->Scene) or (Filter->Scene->Compliance != VSC::NONE)) return false;
      return (Features != LFX::NIL) or (Intensity != 1.0) or has_ambient();
   }

   // True if the effect reads an albedo from its Mix input.

   bool reads_albedo() const {
      return (Type IS LT::COMBINED) and (MixType != VSF::NONE);
   }

   // True if fine detail receives extra relief, given the radius of the base layer's blur in pixels.

   bool boost_detail(double Sigma) const;
   double cavity_sigma(double UnitSize) const;
   float sharpen_weight(double UnitSize) const;
   ERR draw_enhanced(const objBitmap *, const filter_surface_view *, const point3 &, double, const map_height &,
      double, int, int);
   template <class DELTA, class COLOUR>
   void render_enhanced(int, int, const struct lit_surface &, int, float *, DELTA, COLOUR);
};

//********************************************************************************************************************
// Colour computation for spot light.  Resulting RGB values are 0 - 1.0

FRGB extLightingFX::colour_spot_light(point3 &Point)
{
   if (ConeAngle) {
      const double cosAngle = -Point.dot(SpotDelta);
      if (cosAngle < CosOuterConeAngle) return FRGB(0.0, 0.0, 0.0, 1.0);

      double scale = pow(std::max(0.0, cosAngle), SpotExponent);
      if (cosAngle < CosInnerConeAngle) {
         scale = scale * (cosAngle - CosOuterConeAngle);
         scale *= ConeScale;
      }
      return FRGB(LightColour.Red * scale, LightColour.Green * scale, LightColour.Blue * scale,
         LightColour.Alpha * scale);
   }
   else {
      const double scale = pow(std::max(0.0, -Point.dot(SpotDelta)), SpotExponent);
      return FRGB(LightColour.Red * scale, LightColour.Green * scale, LightColour.Blue * scale,
         LightColour.Alpha * scale);
   }
}

//********************************************************************************************************************
// Specular/Diffuse drawing functions.  Occlusion is the fraction of light that reaches the surface; the SVG path
// leaves it at 1.0, which does not alter the result.  A specular Exponent of zero selects SpecularExponent.

inline void extLightingFX::diffuse_light(const point3 &Normal, const point3 &STL, const FRGB &Colour, uint8_t *Output,
   uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion)
{
   double scale = std::clamp((Constant * Normal.dot(STL)) * 255.0 * Occlusion, 0.0, 255.0);

   if (Linear) {
      Output[R] = glLinearRGB.invert(int(Colour.Red * scale));
      Output[G] = glLinearRGB.invert(int(Colour.Green * scale));
      Output[B] = glLinearRGB.invert(int(Colour.Blue * scale));
   }
   else {
      Output[R] = int(Colour.Red * scale);
      Output[G] = int(Colour.Green * scale);
      Output[B] = int(Colour.Blue * scale);
   }
   Output[A] = 255;
}

inline void extLightingFX::specular_light(const point3 &Normal, const point3 &STL, const FRGB &Colour, uint8_t *Output,
   uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion, double Exponent)
{
   point3 halfDir(STL);
   halfDir.z += 1.0; // Eye position is always (0, 0, 1)
   halfDir.normalise();

   const double exponent = (Exponent > 0) ? Exponent : SpecularExponent;
   double scale = std::clamp((Constant * std::pow(std::max(0.0, Normal.dot(halfDir)), exponent)) * 255.0
      * Occlusion, 0.0, 255.0);

   const uint8_t r = int(Colour.Red * scale);
   const uint8_t g = int(Colour.Green * scale);
   const uint8_t b = int(Colour.Blue * scale);

   if (Linear) {
      Output[R] = glLinearRGB.invert(r);
      Output[G] = glLinearRGB.invert(g);
      Output[B] = glLinearRGB.invert(b);
   }
   else {
      Output[R] = r;
      Output[G] = g;
      Output[B] = b;
   }

   if (LightSource IS LS::DISTANT) {
      // Alpha is chosen from the max of the linear R,G,B light value
      Output[A] = Output[R] > Output[G] ? (Output[R] > Output[B] ? Output[R] : Output[B]) : (Output[G] > Output[B] ? Output[G] : Output[B]);
   }
   else Output[A] = r > g ? (r > b ? r : b) : (g > b ? g : b);
}

//********************************************************************************************************************
// Output functions of the enhanced pipeline.  The SVG functions above truncate the light to an 8-bit value before
// encoding it, which leaves only a few output codes for dark light in linear filters.  Here the light stays in floating
// point until it is written: linear light is encoded through the 16-bit table, which gives every sRGB code a distinct
// input, and rounded to the nearest code.  Light in sRGB filters is truncated as in the SVG functions, so that a flat
// surface keeps the SVG result.  Scale is the light's strength and each channel of Colour is the light's colour, both
// in the range 0 - 1.  draw_enhanced() limits the light colour to that range.

inline void extLightingFX::encode_light(const FRGB &Colour, double Scale, uint8_t *Output, uint8_t R, uint8_t G,
   uint8_t B)
{
   if (Linear) {
      const float scale = float(Scale * 65535.0);
      Output[R] = glLinearRGB.invert16(uint16_t((Colour.Red * scale) + 0.5f));
      Output[G] = glLinearRGB.invert16(uint16_t((Colour.Green * scale) + 0.5f));
      Output[B] = glLinearRGB.invert16(uint16_t((Colour.Blue * scale) + 0.5f));
   }
   else {
      const float scale = float(Scale * 255.0);
      Output[R] = uint8_t(Colour.Red * scale);
      Output[G] = uint8_t(Colour.Green * scale);
      Output[B] = uint8_t(Colour.Blue * scale);
   }
}

// Strength is Constant multiplied by Intensity, which is exactly Constant when Intensity is 1.

inline void extLightingFX::enhanced_diffuse(const point3 &Normal, const point3 &STL, const FRGB &Colour,
   uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion)
{
   encode_light(Colour, std::clamp(Strength * Normal.dot(STL) * Occlusion, 0.0, 1.0), Output, R, G, B);
   Output[A] = 255;
}

// The alpha rule of specular_light() is kept: the maximum of the encoded channels for distant lights, and of the
// light values otherwise.

inline void extLightingFX::enhanced_specular(const point3 &Normal, const point3 &STL, const FRGB &Colour,
   uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion, double Exponent)
{
   point3 half_dir(STL);
   half_dir.z += 1.0; // Eye position is always (0, 0, 1)
   half_dir.normalise();

   const double scale = std::clamp(Strength * std::pow(std::max(0.0, Normal.dot(half_dir)), Exponent) * Occlusion,
      0.0, 1.0);
   encode_light(Colour, scale, Output, R, G, B);

   if ((LightSource IS LS::DISTANT) or (not Linear)) Output[A] = std::max(std::max(Output[R], Output[G]), Output[B]);
   else {
      const float peak = std::max(std::max(Colour.Red, Colour.Green), Colour.Blue);
      Output[A] = uint8_t((peak * float(scale * 255.0)) + 0.5f);
   }
}

// Encodes one channel of light, clamped to 0 - 1, for output.  Round selects rounding in sRGB filters; otherwise the
// value is truncated as the SVG functions do.  Linear light is always rounded.

inline uint8_t extLightingFX::encode_channel(double Value, bool Round)
{
   const float value = float(std::clamp(Value, 0.0, 1.0));
   if (Linear) return glLinearRGB.invert16(uint16_t((value * 65535.0f) + 0.5f));
   else return uint8_t((value * 255.0f) + (Round ? 0.5f : 0.0f));
}

// Diffuse light with ambient light added.  The ambient light is reduced by the cavity term (Cavity), but not by
// shadows; Occlusion includes both.  The sum is clamped per channel.

inline void extLightingFX::enhanced_ambient(const point3 &Normal, const point3 &STL, const FRGB &Colour,
   uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion, double Cavity)
{
   const double scale = std::clamp(Strength * Normal.dot(STL) * Occlusion, 0.0, 1.0);
   Output[R] = encode_channel((AmbientLight.Red * Cavity) + (Colour.Red * scale), false);
   Output[G] = encode_channel((AmbientLight.Green * Cavity) + (Colour.Green * scale), false);
   Output[B] = encode_channel((AmbientLight.Blue * Cavity) + (Colour.Blue * scale), false);
   Output[A] = 255;
}

// The combined type.  Normal is the pixel's own normal, used for diffuse light, and Mean is the normal with which the
// specular light is shaded, at the given Exponent (see row_toksvig()).  Light is not limited to full strength; the
// result is clamped once, on output.  The albedo is premultiplied for the sum, so the specular light is added as the
// arithmetic composite would add a premultiplied specular map.  The result's alpha is the albedo's alpha raised to the
// brightest channel of the specular light, and its colour is divided by that alpha for storage.

inline void extLightingFX::enhanced_combined(const point3 &Normal, const point3 &Mean, double Exponent,
   const point3 &STL, const FRGB &Light, const albedo_sample &Albedo, uint8_t *Output, uint8_t R, uint8_t G,
   uint8_t B, uint8_t A, double Occlusion, double Cavity)
{
   point3 half_dir(STL);
   half_dir.z += 1.0; // Eye position is always (0, 0, 1)
   half_dir.normalise();

   const double diffuse = Constant * std::max(0.0, Normal.dot(STL)) * Occlusion;
   const double specular = SpecularConstant * std::pow(std::max(0.0, Mean.dot(half_dir)), Exponent) * Occlusion;

   const double sr = specular * Light.Red, sg = specular * Light.Green, sb = specular * Light.Blue;
   const double alpha = std::max(double(Albedo.a), std::min(1.0, std::max(std::max(sr, sg), sb)));
   if (alpha <= 0) {
      Output[R] = Output[G] = Output[B] = Output[A] = 0;
      return;
   }

   const double coverage = Albedo.a;
   const double demul = 1.0 / alpha;
   Output[R] = encode_channel(((Albedo.r * coverage * ((AmbientLight.Red * Cavity) + (diffuse * Light.Red))) + sr)
      * demul, true);
   Output[G] = encode_channel(((Albedo.g * coverage * ((AmbientLight.Green * Cavity) + (diffuse * Light.Green))) + sg)
      * demul, true);
   Output[B] = encode_channel(((Albedo.b * coverage * ((AmbientLight.Blue * Cavity) + (diffuse * Light.Blue))) + sb)
      * demul, true);
   Output[A] = uint8_t((alpha * 255.0) + 0.5);
}

//********************************************************************************************************************

void extLightingFX::render_distant(int StartY, int EndY, const objBitmap *Bitmap, const point3 &Light,
   double SpotHeight, const map_height &NormalHeight, int Width, int Height)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;

   const uint8_t *input_base = Bitmap->Data + (Bitmap->Clip.Left * bpp) +
      (Bitmap->Clip.Top * Bitmap->LineWidth);
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   for (int y=StartY; y < EndY; ++y) {
      const uint8_t *input_row = input_base + y * Bitmap->LineWidth;
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
         diffuse_light(leftNormal(m, NormalHeight), Direction, LightColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            diffuse_light(interiorNormal(m, NormalHeight), Direction, LightColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         // Process right edge pixel
         if (Width > 1) {
            shiftMatrixLeft(m);
            diffuse_light(rightNormal(m, NormalHeight), Direction, LightColour, dptr, R, G, B, A);
         }
      }
      else { // SPECULAR
         specular_light(leftNormal(m, NormalHeight), Direction, LightColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            specular_light(interiorNormal(m, NormalHeight), Direction, LightColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            specular_light(rightNormal(m, NormalHeight), Direction, LightColour, dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

void extLightingFX::render_spotlight(int StartY, int EndY, const objBitmap *Bitmap, const point3 &Light,
   double SpotHeight, const map_height &NormalHeight, int Width, int Height)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;

   const uint8_t *input_base = Bitmap->Data + (Bitmap->Clip.Left * bpp) +
      (Bitmap->Clip.Top * Bitmap->LineWidth);
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   // Compute the light direction vector based on the light source position and the alpha value of the pixel.
   auto read_light_delta = [&Light, SpotHeight](double X, double Y, uint8_t alpha_val) -> point3 {
      point3 direction(Light.x - X, Light.y - Y, Light.z - (double(alpha_val) * (1.0 / 255.0) * SpotHeight));
      direction.normalise();
      return direction;
   };

   for (int y=StartY; y < EndY; ++y) {
      const uint8_t *input_row = input_base + y * Bitmap->LineWidth;
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
         diffuse_light(leftNormal(m, NormalHeight), stl, colour_spot_light(stl), dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            diffuse_light(interiorNormal(m, NormalHeight), stl, colour_spot_light(stl), dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            diffuse_light(rightNormal(m, NormalHeight), stl, colour_spot_light(stl), dptr, R, G, B, A);
         }
      }
      else { // SPECULAR
         point3 stl = read_light_delta(0, y, m[4]);
         specular_light(leftNormal(m, NormalHeight), stl, colour_spot_light(stl), dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            specular_light(interiorNormal(m, NormalHeight), stl, colour_spot_light(stl), dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            specular_light(rightNormal(m, NormalHeight), stl, colour_spot_light(stl), dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

void extLightingFX::render_point(int StartY, int EndY, const objBitmap *Bitmap, const point3 &Light,
   double SpotHeight, const map_height &NormalHeight, int Width, int Height)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;

   const uint8_t *input_base = Bitmap->Data + (Bitmap->Clip.Left * bpp) +
      (Bitmap->Clip.Top * Bitmap->LineWidth);
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   // Compute the light direction vector based on the light source position and the alpha value of the pixel.
   auto read_light_delta = [&Light, SpotHeight](double X, double Y, uint8_t alpha_val) -> point3 {
      point3 direction(Light.x - X, Light.y - Y, Light.z - (double(alpha_val) * (1.0 / 255.0) * SpotHeight));
      direction.normalise();
      return direction;
   };

   for (int y=StartY; y < EndY; ++y) {
      const uint8_t *input_row = input_base + y * Bitmap->LineWidth;
      uint8_t *dest_row = dest_base + y * Target->LineWidth;

      // Prefetch the next few rows while processing current row
      if (y + 2 < std::min(Height, EndY)) prefetchNextRow(input_base + (y + 2) * Bitmap->LineWidth, Width, bpp);
      if (y + 3 < std::min(Height, EndY)) prefetchNextRow(input_base + (y + 3) * Bitmap->LineWidth, Width, bpp);

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
         diffuse_light(leftNormal(m, NormalHeight), stl, LightColour, dptr, R, G, B, A);
         if (Width <= 1) continue;

         dptr += bpp;

         for (int x=1; x < Width-1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            diffuse_light(interiorNormal(m, NormalHeight), stl, LightColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         shiftMatrixLeft(m);
         stl = read_light_delta(Width-1, y, m[4]);
         diffuse_light(rightNormal(m, NormalHeight), stl, LightColour, dptr, R, G, B, A);
      }
      else { // SPECULAR
         point3 stl = read_light_delta(0, y, m[4]);
         specular_light(leftNormal(m, NormalHeight), stl, LightColour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            specular_light(interiorNormal(m, NormalHeight), stl, LightColour, dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            specular_light(rightNormal(m, NormalHeight), stl, LightColour, dptr, R, G, B, A);
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
   if ((not Self->Target) or (Self->Target->BytesPerPixel != 4)) return ERR::InvalidState;
   return Self->draw();
}

//*********************************************************************************************************************

ERR extLightingFX::draw()
{
   const objBitmap *bmp;
   if (auto error = get_source_bitmap(this, &bmp, filter_input_role::INPUT); error != ERR::Okay) {
      return error;
   }

   // Light is computed in the filter's colour space.  Bitmaps are always sRGB encoded, so linear results are encoded
   // on output.

   Linear = Filter->ColourSpace IS VCS::LINEAR_RGB;
   LightColour = Linear ? LinearColour : Colour;

   auto lt = point3(X, Y, Z); // Light source
   auto pt = Spotlight; // Target for the light source, used by LS::SPOT only.

   const auto &render = *Filter->Render;
   if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      // Light positions are relative to the client's own path bounds.
      const auto &bounds = render.ClientPathBounds;
      const double width = bounds.width(), height = bounds.height();
      const auto diagonal = filter_render_context::normalised_diagonal(width, height);

      lt.x = (lt.x * width) + bounds.left;
      lt.y = (lt.y * height) + bounds.top;
      lt.z *= diagonal;

      if (LightSource IS LS::SPOT) {
         pt.x = (pt.x * width) + bounds.left;
         pt.y = (pt.y * height) + bounds.top;
         pt.z *= diagonal;
      }
   }

   const auto &t = render.Transform;
   const double scale = (t.sx IS t.sy) ? t.sx : filter_render_context::normalised_diagonal(t.sx, t.sy);

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

   // Note! Linear conversion of the source bitmap is unnecessary because only the alpha channel is used.

   // The alpha channel of the source bitmap will function as the Z value for the bump map.  The RGB components
   // are ignored for input purposes.

   int width  = Target->Clip.Right - Target->Clip.Left;
   int height = Target->Clip.Bottom - Target->Clip.Top;
   if (bmp->Clip.Right - bmp->Clip.Left < width) width = bmp->Clip.Right - bmp->Clip.Left;
   if (bmp->Clip.Bottom - bmp->Clip.Top < height) height = bmp->Clip.Bottom - bmp->Clip.Top;
   if ((width < 1) or (height < 1)) return ERR::Okay;

   // The surface height is in client units, converted to pixels for locating the surface relative to the light.  The
   // Sobel gradients are measured per pixel, so the heights for the surface normal are expressed per kernel unit
   // instead.  This approximates the resampling of the bump map to the kernel unit grid that SVG describes.

   const double spot_height = MapHeight * scale;
   const auto unit = kernel_unit_pixels(scale);
   const double normal_scale = MapHeight * (1.0 / 255.0);
   const map_height normal_height = { unit.x * normal_scale, unit.y * normal_scale, unit.xy * normal_scale };

   // Scenes that are not bound to an SVG standard use the enhanced pipeline, which resolves finer surface detail.

   if (enhanced()) {
      filter_surface_view albedo;
      if (reads_albedo()) {
         if (auto error = get_source_view(this, albedo, filter_input_role::MIX); error != ERR::Okay) return error;
         if (albedo.Bitmap->BytesPerPixel != 4) return ERR::InvalidState;
      }
      const double unit_size = unit_length(unit);
      return draw_enhanced(bmp, reads_albedo() ? &albedo : nullptr, lt, spot_height, normal_height, unit_size, width,
         height);
   }

   int num_threads = std::min(int(std::thread::hardware_concurrency()), height);
   if (num_threads < 1) num_threads = 1;
   const int min_rows_per_chunk = 4; // Minimum work per thread to avoid overhead
   const int chunk_size = std::max(min_rows_per_chunk, height / num_threads);
   const int num_chunks = (height + chunk_size - 1) / chunk_size;

   BS::thread_pool pool(num_threads);

   for (int chunk=0; chunk < num_chunks; ++chunk) {
      const int start_y = chunk * chunk_size;
      const int end_y = std::min(start_y + chunk_size, height);

      pool.detach_task([&, start_row = start_y, end_row = end_y]() {
         if (LightSource IS LS::DISTANT) {
            render_distant(start_row, end_row, bmp, lt, spot_height, normal_height, width, height);
         }
         else if (LightSource IS LS::SPOT) {
            render_spotlight(start_row, end_row, bmp, lt, spot_height, normal_height, width, height);
         }
         else if (LightSource IS LS::POINT) {
            render_point(start_row, end_row, bmp, lt, spot_height, normal_height, width, height);
         }
      });
   }

   pool.wait();
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

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetDistantLight(extLightingFX *Self, struct lt::SetDistantLight *Args)
{
   kt::Log log;

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
flame is a good example of a point light.  The strength of the light is controlled by #Constant and #Intensity.

-INPUT-
double X: X location for the light source.
double Y: Y location for the light source.
double Z: Z location for the light source.

-RESULT-
Okay:
Args:
NullArgs:

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetPointLight(extLightingFX *Self, struct lt::SetPointLight *Args)
{
   kt::Log log;

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

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetSpotLight(extLightingFX *Self, struct lt::SetSpotLight *Args)
{
   kt::Log log;

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
Ambient: Ambient light added to the diffuse and combined types.

Ambient light reaches every point of the surface equally, from no particular direction, so that areas facing away from
the light or lying in shadow are not black.  It is added to the diffuse light, and for the `COMBINED` type it is
multiplied by the albedo.  It is not blocked by shadows, but it is reduced in creases and pits by the `CAVITY`
feature.  The specular type ignores it.

The light is the RGB value of the colour multiplied by its alpha.  The colour is in the same colour space as #Colour.
The default is transparent black, which adds no light.

This field has no effect when the scene's Compliance is `SVG1` or `SVG2`, unless the #Type is `COMBINED`.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Ambient(extLightingFX *Self, FRGB **Value)
{
   *Value = &Self->Ambient;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Ambient(extLightingFX *Self, FRGB *Value)
{
   if (Value) {
      if ((not std::isfinite(Value->Red)) or (not std::isfinite(Value->Green)) or (not std::isfinite(Value->Blue)) or
          (not std::isfinite(Value->Alpha))) return ERR::InvalidValue;
      Self->Ambient = *Value;
   }
   else Self->Ambient = { 0, 0, 0, 0 };
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Cavity: Strength of the darkening applied to creases and pits by the `CAVITY` feature.

Concave areas of the surface receive less light in proportion to their curvature, multiplied by this value.  The
light is never reduced below 40% of its unoccluded level.  The default is 0.25, and a value of 0 has the same effect
as disabling `CAVITY` in #Features.  The value must be zero or greater.

This field has no effect unless the enhanced pipeline is in use; refer to #Features.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Cavity(extLightingFX *Self, double &Value)
{
   Value = Self->Cavity;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Cavity(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->Cavity = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Colour: Defines the colour of the light source.

Set the Colour field to define the colour of the light source.  The alpha component is not used; the strength of the
light is set by #Constant and #Intensity.

The default colour is pure white.

*********************************************************************************************************************/

static ERR LIGHTINGFX_SET_Colour(extLightingFX *Self, FRGB *Value)
{
   if (Value) Self->Colour = *Value;
   else Self->Colour.Alpha = 0;

   Self->LinearColour = Self->Colour;
   glLinearRGB.convert(Self->LinearColour);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Constant: Specifies the ks/kd value in Phong lighting model.

In the Phong lighting model, this field specifies the kd value in diffuse mode, or ks value in specular mode.  For the
`COMBINED` type it is kd, and #SpecularConstant is ks.

*********************************************************************************************************************/

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
Detail: Strength of the relief given to fine detail by the `DETAIL` feature.

The slope of fine detail, meaning features of a few kernel units, is multiplied by this value before the surface is
lit.  Steep slopes are compressed so that strong values do not turn small features into hard edges.  Broad shapes are
not affected.  The default is 1.5, and a value of 1 has the same effect as disabling `DETAIL` in #Features.  The value
must be 1 or greater.

This field has no effect unless the enhanced pipeline is in use; refer to #Features.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Detail(extLightingFX *Self, double &Value)
{
   Value = Self->Detail;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Detail(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 1.0)) return ERR::OutOfRange;
   Self->Detail = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Exponent: Exponent for specular lighting, larger is more "shiny".  Ranges from 1.0 to 128.0.

This field defines the exponent value for specular lighting, within a range of 1.0 to 128.0.  The larger the value,
shinier the end result.

*********************************************************************************************************************/

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
Features: Selects the enhancements applied to the SVG lighting algorithm.
Lookup: LFX

When the @VectorScene.Compliance of the scene is `NONE`, the lighting algorithm is extended with the features
enabled here.  The default is `ALL`.  Each feature can be disabled independently, for instance to remove shadows
from content that is lit from a low angle, or to tune the appearance of an effect that was designed for the SVG
algorithm.  Disabling `SHARPEN` measures slopes with the same kernel as the SVG algorithm.

The strength of `CAVITY` and `DETAIL`, and the width of the shadow edge, are set by the #Cavity, #Detail and
#Penumbra fields.

If no features are enabled, and neither #Ambient nor #Intensity is set, the SVG algorithm is used without
modification.  Features have no effect when the Compliance of the scene is `SVG1` or `SVG2`, except for the `COMBINED`
type, which always uses the enhanced pipeline.  The SVG class renders its own scene with a Compliance of `SVG2`, unless
the document declares a Kōtuku namespace, in which case the Compliance is `NONE` and the features apply.  In SVG
markup, features are selected with the `kotuku:features` attribute of `feDiffuseLighting` and `feSpecularLighting`.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Features(extLightingFX *Self, LFX &Value)
{
   Value = Self->Features;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Features(extLightingFX *Self, LFX Value)
{
   if ((Value & (~LFX::ALL)) != LFX::NIL) return ERR::InvalidValue;
   Self->Features = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Intensity: Multiplies the colour of the light.

The light's #Colour is multiplied by this value.  For the diffuse and specular types, the light is limited to full
strength before it is coloured, as it is for #Constant, so a value above 1 brightens the light only until it reaches
full strength.  For the `COMBINED` type the light is not limited until the result is written, so a value above 1 can
make an albedo brighter while it keeps its colour.

The default is 1.  The value must be zero or greater.  It has no effect when the scene's Compliance is `SVG1` or
`SVG2`, unless the #Type is `COMBINED`.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Intensity(extLightingFX *Self, double &Value)
{
   Value = Self->Intensity;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Intensity(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->Intensity = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Penumbra: Width of the soft edge of shadows cast by the `SHADOWS` feature, in degrees.

A point on the surface passes from light into shadow as the elevation at which the surface hides the light rises
through the light's own elevation.  The transition is smoothed over this many degrees either side of the light's
elevation, so the shadow's edge widens with distance from the area that casts it.  The width is limited to half of the
light's elevation.  The default is 2, and a value of 0 produces hard shadows.  The value must be zero or greater.

This field has no effect unless the enhanced pipeline is in use; refer to #Features.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Penumbra(extLightingFX *Self, double &Value)
{
   Value = Self->Penumbra;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Penumbra(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->Penumbra = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Scale: The maximum height of the input surface (bump map) when the alpha input is 1.0.

The height is measured in kernel units when computing the surface normal, so the steepness of the lit surface is
determined by Scale in combination with #UnitX and #UnitY.  Refer to #UnitX for details.

*********************************************************************************************************************/

/*********************************************************************************************************************

-FIELD-
SpecularConstant: The ks value of the `COMBINED` type.

For the `COMBINED` type, this field specifies the ks value of the Phong lighting model, and #Constant specifies kd.
It is not used by the diffuse and specular types, for which #Constant is the only constant.  The default is 1, and
the value must be zero or greater.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_SpecularConstant(extLightingFX *Self, double &Value)
{
   Value = Self->SpecularConstant;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_SpecularConstant(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->SpecularConstant = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Type: Defines the type of surface light scattering, which can be diffuse, specular or both combined.
Lookup: LT

The `COMBINED` type reads an albedo from the #Mix input; refer to the class description.

-FIELD-
UnitX: The intended distance in current filter units for dx in the surface normal calculation formulas.

Indicates the intended distance in current filter units (i.e. as determined by the value of
@VectorFilter.PrimitiveUnits) for dx in the surface normal calculation formulas.  This is equivalent to the first
value of the SVG `kernelUnitLength` attribute.

If #UnitX is not specified, the kernel unit is one unit of the client vector's coordinate system, regardless of
@VectorFilter.PrimitiveUnits.  The appearance of the lighting then depends on the size of the bump map in those
units: the same graphic will look flatter when it is drawn larger in its own coordinate system, but not when it is
magnified by a parent transform.

By specifying #UnitX in bounding box units, the kernel becomes relative to the filter's bounds, which are measured on
the same terms as @BlurFX.  A bump map that is blurred and lit in bounding box units will then have the same
appearance at any size and display resolution.

The value must be greater than zero.  Rather than resampling the bump map to the kernel grid, the surface gradient is
measured per pixel and scaled to the kernel unit.

*********************************************************************************************************************/

static ERR LIGHTINGFX_SET_UnitX(extLightingFX *Self, double Value)
{
   if (Value <= 0.0) return ERR::InvalidValue;
   Self->UnitX = Value;
   Self->UnitXProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
UnitY: The intended distance in current filter units for dy in the surface normal calculation formulas.

Indicates the intended distance in current filter units (i.e. as determined by the value of
@VectorFilter.PrimitiveUnits) for dy in the surface normal calculation formulas.  This is equivalent to the second
value of the SVG `kernelUnitLength` attribute.  The value must be greater than zero.  Refer to #UnitX for further
details.

*********************************************************************************************************************/

static ERR LIGHTINGFX_SET_UnitY(extLightingFX *Self, double Value)
{
   if (Value <= 0.0) return ERR::InvalidValue;
   Self->UnitY = Value;
   Self->UnitYProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG element that describes the effect.

The result is a complete element, including its light source: `feDiffuseLighting` or `feSpecularLighting`, or
`kotuku:feLighting` for the `COMBINED` type.  Settings that SVG cannot express are written as `kotuku:` attributes, and
only when they differ from their defaults: `kotuku:features` if #Features is not `ALL`, `kotuku:cavity`,
`kotuku:detail` and `kotuku:penumbra` for #Cavity, #Detail and #Penumbra, `kotuku:ambient` for #Ambient, and
`kotuku:intensity` on the light source for #Intensity.  The attributes of `kotuku:feLighting` are written without the
prefix, as the element is already in the Kōtuku namespace.  A document that includes any of them must declare the
prefix, for example `xmlns:kotuku="http://www.kotuku.dev/xmlns/svg"`.

The `in`, `in2`, `result` and primitive subregion attributes are not included.

*********************************************************************************************************************/

static void write_lfx_features(std::ostream &Stream, LFX Features)
{
   static const std::pair<LFX, CSTRING> names[] = {
      { LFX::DEQUANTISE, "dequantise" }, { LFX::SHARPEN, "sharpen" }, { LFX::CAVITY, "cavity" },
      { LFX::DETAIL, "detail" }, { LFX::SHADOWS, "shadows" }, { LFX::SPECULAR_AA, "specular-aa" }
   };

   if (Features IS LFX::NIL) {
      Stream << "none";
      return;
   }

   bool first = true;
   for (auto &[flag, name] : names) {
      if ((Features & flag) IS LFX::NIL) continue;
      if (not first) Stream << ' ';
      Stream << name;
      first = false;
   }
}

// Writes a colour in CSS rgb() notation.  Channels are written as 8-bit values if they are exactly representable, and
// as percentages otherwise, so that the colour survives a round trip.

static void write_rgb(std::ostream &Stream, const FRGB &Colour)
{
   auto exact = [](float Value) {
      const float level = Value * 255.0f;
      return (Value >= 0.0f) and (Value <= 1.0f) and (std::abs(level - std::round(level)) < 1e-3f);
   };

   Stream << "rgb(";
   if (exact(Colour.Red) and exact(Colour.Green) and exact(Colour.Blue)) {
      Stream << std::lround(Colour.Red * 255.0f) << ' ' << std::lround(Colour.Green * 255.0f) << ' '
         << std::lround(Colour.Blue * 255.0f);
   }
   else {
      Stream << (Colour.Red * 100.0) << "% " << (Colour.Green * 100.0) << "% " << (Colour.Blue * 100.0) << '%';
   }
   if (Colour.Alpha != 1.0f) Stream << " / " << Colour.Alpha;
   Stream << ')';
}

static ERR LIGHTINGFX_GET_XMLDef(extLightingFX *Self, std::string &Value)
{
   std::stringstream stream;
   const bool combined = Self->Type IS LT::COMBINED;
   CSTRING element = combined ? "kotuku:feLighting" : (Self->Type IS LT::DIFFUSE) ? "feDiffuseLighting" :
      "feSpecularLighting";
   CSTRING prefix = combined ? "" : "kotuku:"; // Extension attributes of kotuku:feLighting are unprefixed

   stream << '<' << element << " surfaceScale=\"" << Self->MapHeight << '"';
   if (Self->Type IS LT::DIFFUSE) stream << " diffuseConstant=\"" << Self->Constant << '"';
   else if (Self->Type IS LT::SPECULAR) stream << " specularConstant=\"" << Self->Constant << '"';
   else {
      stream << " diffuseConstant=\"" << Self->Constant << '"';
      stream << " specularConstant=\"" << Self->SpecularConstant << '"';
   }
   if (Self->Type != LT::DIFFUSE) stream << " specularExponent=\"" << Self->SpecularExponent << '"';

   if (Self->UnitXProvided or Self->UnitYProvided) {
      stream << " kernelUnitLength=\"" << Self->UnitX << ' ' << Self->UnitY << '"';
   }

   const auto &c = Self->Colour;
   if ((c.Red != 1.0f) or (c.Green != 1.0f) or (c.Blue != 1.0f) or (c.Alpha != 1.0f)) {
      stream << " lighting-color=\"";
      write_rgb(stream, c);
      stream << '"';
   }

   if (Self->Features != LFX::ALL) {
      stream << ' ' << prefix << "features=\"";
      write_lfx_features(stream, Self->Features);
      stream << '"';
   }
   if (Self->Cavity != DEFAULT_CAVITY) stream << ' ' << prefix << "cavity=\"" << Self->Cavity << '"';
   if (Self->Detail != DEFAULT_DETAIL) stream << ' ' << prefix << "detail=\"" << Self->Detail << '"';
   if (Self->Penumbra != DEFAULT_PENUMBRA) stream << ' ' << prefix << "penumbra=\"" << Self->Penumbra << '"';

   const auto &a = Self->Ambient;
   if ((a.Red != 0.0f) or (a.Green != 0.0f) or (a.Blue != 0.0f) or (a.Alpha != 0.0f)) {
      stream << ' ' << prefix << "ambient=\"";
      write_rgb(stream, a);
      stream << '"';
   }
   stream << '>';

   switch (Self->LightSource) {
      case LS::POINT:
         stream << "<fePointLight x=\"" << Self->X << "\" y=\"" << Self->Y << "\" z=\"" << Self->Z << '"';
         break;

      case LS::SPOT:
         stream << "<feSpotLight x=\"" << Self->X << "\" y=\"" << Self->Y << "\" z=\"" << Self->Z << '"';
         stream << " pointsAtX=\"" << Self->Spotlight.x << "\" pointsAtY=\"" << Self->Spotlight.y;
         stream << "\" pointsAtZ=\"" << Self->Spotlight.z << "\" specularExponent=\"" << Self->SpotExponent << '"';
         if (Self->ConeAngle) stream << " limitingConeAngle=\"" << Self->ConeAngle << '"';
         break;

      default:
         stream << "<feDistantLight azimuth=\"" << Self->Azimuth << "\" elevation=\"" << Self->Elevation << '"';
         break;
   }
   if (Self->Intensity != 1.0) stream << " kotuku:intensity=\"" << Self->Intensity << '"';
   stream << "/>";

   stream << "</" << element << '>';
   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_lighting_enhanced.cpp"
#include "filter_lighting_def.c"

static const FieldArray clLightingFXFields[] = {
   { "Colour",   FDF_STRUCT|FDF_RW, nullptr, LIGHTINGFX_SET_Colour, "FRGB" },
   { "Constant", FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_Constant },
   { "Exponent", FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_Exponent },
   { "Features", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, LIGHTINGFX_GET_Features, LIGHTINGFX_SET_Features, &clLightingFXLFX },
   { "Cavity",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Cavity, LIGHTINGFX_SET_Cavity },
   { "Detail",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Detail, LIGHTINGFX_SET_Detail },
   { "Penumbra", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Penumbra, LIGHTINGFX_SET_Penumbra },
   { "Ambient",  FDF_VIRTUAL|FDF_STRUCT|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Ambient, LIGHTINGFX_SET_Ambient, "FRGB" },
   { "Intensity", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Intensity, LIGHTINGFX_SET_Intensity },
   { "SpecularConstant", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_SpecularConstant,
     LIGHTINGFX_SET_SpecularConstant },
   { "Scale",    FDF_DOUBLE|FDF_RW },
   { "UnitX",    FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_UnitX },
   { "UnitY",    FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_UnitY },
   { "Type",     FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clLightingFXLT },
   { "XMLDef",   FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, LIGHTINGFX_GET_XMLDef },
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
