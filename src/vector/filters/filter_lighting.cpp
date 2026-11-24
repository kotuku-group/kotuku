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

The lighting effect uses the alpha channel of an input bitmap as a bump map.  Enhanced lighting also accepts linear
floating-point heights from @TurbulenceFX or @BlurFX in `HEIGHT` output mode.  The output is an RGBA representation
of the light effect.  If the client leaves #Colour at its default, the output is greyscale.  The `COMBINED` #Type
can use a separate image as its albedo to produce a finished surface.

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

The specular result is a premultiplied colour, in keeping with web browsers: it is stored with an alpha of `Sa` and an
unpremultiplied colour of `(Sr, Sg, Sb) / Sa`, which is the hue of the light.  A white light gives a white result
whose alpha is the strength of the highlight, so the result brightens the content that it is composited over.

The light is computed in the colour space of the parent @VectorFilter.  In `SRGB` the light #Colour and the result
are sRGB values.  In `LINEAR_RGB` the light #Colour is converted to linear RGB and the result is encoded to sRGB for
storage, so a result of 0.5 is stored as approximately 0.735.

The SVG algorithm measures the slope of the bump map with a 3x3 kernel.  This softens fine detail and shows 8-bit
steps on gentle slopes.  When the @VectorScene.Compliance of the scene is `NONE` (the default), the enhanced pipeline
applies the features selected by the #Features field.  The default is `ALL`.

<list type="bullet">
<li>`DEQUANTISE`: Heights are read as floating point values, and areas that vary by no more than one alpha level are
smoothed to remove terracing.</li>
<li>`DEQUANTISE_WIDE`: Reconstruct a gradual slope across flat runs of up to sixteen pixels bracketed by successive
one-level steps in 8-bit heights.  Enable this mode explicitly.  When both modes are enabled, `DEQUANTISE` smooths
samples the wide pass leaves unresolved.</li>
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

An #Ambient light can be added to the diffuse result.  It is not blocked by shadows, but it is reduced by `CAVITY`
and by an ambient occlusion map bound to #OcclusionMap.  The #Intensity of the light multiplies its #Colour.  For the
diffuse and specular types the light is limited to full strength before it is coloured, as the SVG algorithm does with
#Constant, so a value above 1 cannot make a coloured light brighter than full strength.  Both fields require the
enhanced pipeline.  A diffuse effect with ambient light and an active #OcclusionMap uses that pipeline at any
Compliance setting.

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

The `NORMAL` type writes the enhanced pipeline's per-pixel unit normals before lighting.  It takes the normals
from #NormalMap where available and derives them from the height elsewhere.  Red, green and blue store the X, Y and Z
components in image axes,
with each encoded as the nearest byte to <code>128 + 127 * component</code>.  The result is opaque within the
primitive subregion.  Flat areas produce RGB (128, 128, 255).

Use this result as the Mix input of @DisplacementFX with red for X and green for Y, and use the image to bend as
its primary input.  Match #Scale, #UnitX, #UnitY and the slope features on both lighting effects to align
refraction with the shading of the same surface.  `NORMAL` uses dequantisation, sharpening and the detail boost.
It works under any scene compliance and is available as `kotuku:feNormalMap`.

The `COMBINED` type also accepts three material terms, all of which are disabled by default.  With #Metalness m,
#Fresnel f0 and #Wrap w, the result is

<pre>
albedo * (ambient + kd * (1 - m) * max(0, (N.L + w) / (1 + w)) * light) + ks * F * pow(N.H, specularExponent) * light
</pre>

where F is the reflectance.  It is `F0 = lerp(base, albedo, m)`, where base is f0 if it is set and 1 otherwise, and
if f0 is set it rises towards 1 as the surface turns away from the viewer: `F = F0 + (1 - F0) * pow(1 - N.z, 5)`.
This is the usual metallic workflow.  A dielectric reflects specular light in the light's colour, weakly when it
faces the viewer and strongly at grazing angles, and scatters diffuse light in its albedo's colour.  A metal has no
diffuse light and reflects in its albedo's colour.  Wrap lets diffuse light reach past the terminator, as on wax and
skin, and changes the brightness of flat surfaces.

A matcap adds reflections of an environment that the light model cannot produce, such as those of chrome, glass or
glazed ceramic.  It is an image of a lit sphere, set with #MatcapPath or #Matcap, and each pixel takes the colour of
the point on the sphere that faces the same way as the surface.  The reflection is added to the specular light as
`matcap * F * albedo.alpha`, where F is the reflectance above, computed with the albedo's colour.  It is reduced by
`CAVITY` but not by shadows, and it is independent of the light, so it remains if #SpecularConstant is zero.

A #RoughnessMap varies the highlight across the surface for the `SPECULAR` and `COMBINED` types.  Each pixel's
roughness sets its own specular exponent in place of #Exponent, and smooth areas receive narrower, brighter highlights
than rough ones.  Packed ORM maps can supply the roughness and an #OcclusionMap from different channels of one image.

The `COMBINED` type can be lit by several lights, such as a key light and fill lights.  #SetDistantLight(),
#SetPointLight() and #SetSpotLight() configure a single light, replacing any others, and #AddDistantLight(),
#AddPointLight() and #AddSpotLight() add further lights in order.  #SetLightOptions() gives each light its own
colour, intensity, range and shadow switch.  The surface is analysed once and the diffuse and specular light of every
light is summed before it is applied to the albedo, so the result is the sum of the lights' separate results until it
reaches full strength.  Ambient light and the matcap are added once.  Each distant light casts its own shadows, so
the cost of the `SHADOWS` feature rises with the number of distant lights that cast them.

A point or spot light with a range fades to nothing at that distance.  It is at its full strength at the point of the
surface nearest to it, and falls from there with the square of the distance, multiplied by a smooth window that
reaches zero at the range.  A light without a range is not attenuated, as in SVG.

SVG documents can set #Features, #Cavity, #Detail, #Penumbra, #Ambient and the options of each light with `kotuku:`
attributes, and the `COMBINED` type is available as the `kotuku:feLighting` element, which also accepts the material
terms and several light sources; refer to the SVG class for details.

-END-

*********************************************************************************************************************/

#include <charconv>

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

// Per-pixel shading functions that must be inlined into each instantiation of render_enhanced().  GCC stops inlining
// enhanced_specular() once render_enhanced() has five instantiations (one per kind of light, ranged or not), which
// cost the specular type 6.5% more instructions per pixel.

#ifdef _MSC_VER
   #define LIGHT_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
   #define LIGHT_INLINE inline __attribute__((always_inline))
#else
   #define LIGHT_INLINE inline
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

   // Returns the unit vector halfway between the light, unit vector STL, and the eye at (0, 0, 1).  It is undefined
   // for a light directly behind the surface, opposite the eye.  A zero vector is returned in that case so that no
   // specular light is reflected, as in web browsers.  The limit catches only rounding error: a light a tenth of a
   // degree away has a squared length of about 3e-6 and is lit normally.

   static point3 half_vector(const point3 &STL) {
      point3 half(STL.x, STL.y, STL.z + 1.0);
      if (half.dot(half) < 1e-12) return { 0, 0, 0 };
      half.normalise();
      return half;
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

// Limits of the Exponent field, which also bound the exponents derived from a roughness map.

constexpr double MIN_SPECULAR_EXPONENT = 1.0;
constexpr double MAX_SPECULAR_EXPONENT = 128.0;

// Converts a perceptual roughness r, from 0 to 1, to a Blinn-Phong exponent with the usual approximation: alpha = r^2
// and n = 2 / alpha^2 - 2.  The exponent is limited to the range of the Exponent field, so roughness below 0.35 gives
// the sharpest highlight and roughness above 0.9 the broadest.

static double roughness_exponent(double Roughness)
{
   const double r2 = Roughness * Roughness;
   const double a2 = r2 * r2;
   if (not (a2 > 0)) return MAX_SPECULAR_EXPONENT;
   return std::clamp((2.0 / a2) - 2.0, MIN_SPECULAR_EXPONENT, MAX_SPECULAR_EXPONENT);
}

// The inverse of roughness_exponent() for exponents within the field's range.

static double exponent_roughness(double Exponent)
{
   return std::pow(2.0 / (Exponent + 2.0), 0.25);
}

// Colour of one albedo pixel in the filter's colour space, with straight alpha.

struct albedo_sample {
   float r, g, b, a;
};

// A configured light source.  Geometry is in the filter's primitive units.  The first light of an effect also holds the
// value of the Intensity field.  An effect that has no configured light uses the implicit light, a distant light whose
// direction is zero, which lights nothing.

struct light_record {
   LS     Kind = LS::DISTANT;
   double Azimuth = 0, Elevation = 0;     // DISTANT, in degrees
   point3 Direction = { 0, 0, 0 };        // DISTANT: unit vector from the surface to the light
   point3 Position = { 0, 0, 0 };         // POINT and SPOT
   point3 Target = { 0, 0, 0 };           // SPOT: the point that the light is aimed at
   double SpotExponent = 1, ConeAngle = 0; // SPOT
   FRGB   Colour = { 1, 1, 1, 1 };        // Overrides the effect's Colour if HasColour is set
   FRGB   LinearColour = { 1, 1, 1, 1 };  // Colour in linear RGB
   double Intensity = 1.0;                // Multiplier of the light's colour
   double Range = 0;                      // POINT and SPOT: distance at which the light reaches zero, or 0
   bool   HasColour = false;
   bool   Shadows = true;                 // Distant lights cast shadows if the SHADOWS feature is enabled

   static light_record distant(double Azimuth, double Elevation) {
      light_record result;
      result.Azimuth   = Azimuth;
      result.Elevation = Elevation;
      result.Direction = point3(cos(Azimuth * DEG2RAD) * cos(Elevation * DEG2RAD),
         sin(Azimuth * DEG2RAD) * cos(Elevation * DEG2RAD), sin(Elevation * DEG2RAD));
      return result;
   }

   static light_record point(double X, double Y, double Z) {
      light_record result;
      result.Kind = LS::POINT;
      result.Position = point3(X, Y, Z);
      return result;
   }

   static light_record spot(double X, double Y, double Z, double PX, double PY, double PZ, double Exponent,
      double ConeAngle) {
      light_record result;
      result.Kind = LS::SPOT;
      result.Position = point3(X, Y, Z);
      result.Target = point3(PX, PY, PZ);
      result.SpotExponent = Exponent;
      result.ConeAngle = ConeAngle;
      return result;
   }
};

// Squared distance in pixels below which a sample is treated as coincident with a ranged light.

constexpr double LIGHT_COINCIDENCE_SQUARED = 1e-12;

// A light prepared for one draw.  Positions are in pixels relative to the target's clip region and the colour is in
// the filter's colour space.  The shadow maps are owned by the filter's scratch memory until the draw completes.

struct lit_light {
   LS     kind = LS::DISTANT;
   point3 position = { 0, 0, 0 };   // POINT and SPOT
   point3 direction = { 0, 0, 0 };  // DISTANT: unit vector from the surface to the light
   point3 half = { 0, 0, 1 };       // DISTANT: half vector between the light and the eye
   point3 spot_delta = { 0, 0, -1 }; // SPOT: unit vector from the light to its target
   double spot_exponent = 1, cone_angle = 0, cos_inner = 0, cos_outer = 0, cone_scale = 0;
   FRGB   colour = { 1, 1, 1, 1 };  // Colour in the filter's colour space
   double strength = 1.0;           // Constant x Intensity, for the DIFFUSE and SPECULAR types
   double elevation = 0;            // DISTANT, in radians
   double range = 0;                // In pixels, or 0 if the light is unlimited
   double nearest = 0;              // Distance in pixels from the light to the nearest sample within range
   double nearest_term = 1;         // Squared distance to the nearest sample, floored at the coincidence threshold
   bool   reaches = true;           // False if a ranged light reaches no sample of the surface
   bool   wrap_shadows = false;     // True if wrapped faces take shadows from the raised light
   double wrap_blend = 0;           // N.L below which faces take the raised shadow
   const float *shadow = nullptr;   // Fraction of the light reaching each pixel, or nullptr
   const float *raised = nullptr;   // As shadow, with the light raised by the wrap angle

   // Colour of the light arriving along STL, which is reduced outside of a spot light's cone.  Resulting RGB values
   // are 0 - 1.0 if the light's colour is in that range.

   FRGB spot_colour(const point3 &STL) const {
      if (cone_angle) {
         const double cos_angle = -STL.dot(spot_delta);
         if (cos_angle < cos_outer) return FRGB(0.0, 0.0, 0.0, 1.0);

         double scale = pow(std::max(0.0, cos_angle), spot_exponent);
         if (cos_angle < cos_inner) {
            scale = scale * (cos_angle - cos_outer);
            scale *= cone_scale;
         }
         return FRGB(colour.Red * scale, colour.Green * scale, colour.Blue * scale, colour.Alpha * scale);
      }
      else {
         const double scale = pow(std::max(0.0, -STL.dot(spot_delta)), spot_exponent);
         return FRGB(colour.Red * scale, colour.Green * scale, colour.Blue * scale, colour.Alpha * scale);
      }
   }

   // Attenuation of a ranged light at the squared distance D2 in pixels.  The smooth window (1 - t)^2 (1 + 2t)
   // falls from 1 at the nearest sample to zero, with zero slope, at the range.  It multiplies the inverse-square
   // fall-off, normalised to 1 at the nearest sample.  The result is zero at and beyond the range.

   double attenuation(double D2) const {
      if (not reaches) return 0;
      const double distance = sqrt(D2);
      if (not (distance < range)) return 0;
      const double t = std::clamp((distance - nearest) / (range - nearest), 0.0, 1.0);
      const double window = (1.0 - t) * (1.0 - t) * (1.0 + (2.0 * t));
      return window * (nearest_term / std::max(D2, LIGHT_COINCIDENCE_SQUARED));
   }
};

//********************************************************************************************************************

class extLightingFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::LIGHTINGFX;
   static constexpr CSTRING CLASS_NAME = "LightingFX";
   using create = kt::Create<extLightingFX>;

   // The exported fields are declared first so that their concrete offsets line up with the field table.
   // The 8-byte aligned members precede the narrower Type field to avoid padding before a direct-access field.
   FRGB   Colour = { 1.0, 1.0, 1.0, 1.0 };  // Colour of the light source.
   double Constant = 1.0;                   // The ks/kd constant value for the light mode.
   double SpecularExponent = 1.0;           // Exponent value for specular lighting only (Exponent)
   double MapHeight = 1.0;                  // Maximum height of the surface for bump map calculations (Scale)
   double UnitX = 1.0, UnitY = 1.0;         // SVG kernel unit - scale value for X/Y
   LT     Type = LT::DIFFUSE;               // Diffuse or Specular light scattering

   FRGB   LinearColour = { 1.0, 1.0, 1.0, 1.0 }; // Colour of the light source in linear sRGB space.
   FRGB   LightColour = { 1.0, 1.0, 1.0, 1.0 };  // Colour of the light source in the filter's colour space.
   bool   Linear = false;                        // True if lighting is computed in linear RGB.
   std::vector<light_record> Lights = { light_record() }; // Configured lights in order; never empty
   bool   Configured = false;          // True once a light method has replaced the implicit light
   LS     LightSource = LS::DISTANT;   // Kind of the first light, set per draw
   bool   UnitXProvided = false, UnitYProvided = false;
   LFX    Features = LFX::ALL;         // Enhanced lighting features, applied when the scene's Compliance is NONE
   double Cavity   = DEFAULT_CAVITY;   // Strength of the CAVITY feature
   double Detail   = DEFAULT_DETAIL;   // Strength of the DETAIL feature
   double Penumbra = DEFAULT_PENUMBRA; // Width of the shadow edge in degrees, for the SHADOWS feature
   double SpecularConstant = 1.0;      // ks for the COMBINED type; Constant is kd
   FRGB   Ambient = { 0,0,0,0 };       // Ambient light, scaled by its alpha
   FRGB   AmbientLight = { 0,0,0,0 };  // Ambient light in the filter's colour space, set by draw_enhanced()
   double Strength = 1.0;              // Constant x Intensity for the DIFFUSE and SPECULAR types, set per draw
   double Metalness = 0;               // COMBINED: 0 for a dielectric, 1 for a metal
   double Fresnel = 0;                 // COMBINED: reflectance at normal incidence, or 0 to disable
   double Wrap = 0;                    // COMBINED: extends diffuse light past the terminator
   double WrapScale = 1.0;             // 1 / (1 + Wrap), set per draw
   double WrapBlend = 0;               // N.L below which faces take the raised shadow, set per draw
   bool   Material = false;            // True if any material term is in use, set per draw
   bool   WrapShadows = false;         // True if wrapped faces take shadows from the raised light, set per draw

   // MATCAP (COMBINED type only)
   objBitmap *Matcap = nullptr;         // Matcap image, from MatcapImage or the client
   objImage *MatcapImage = nullptr;     // Owns Matcap if it was loaded from MatcapPath
   std::string MatcapPath;              // Path used to load the image
   std::string MatcapRef;               // Document-relative path supplied by the SVG parser
   bool   MatcapPinned = false;         // True if Matcap is a client bitmap that holds our strong pin
   const float *MatcapTexels = nullptr; // Premultiplied RGB0 in the filter's colour space, or nullptr; per draw
   int    MatcapWidth = 0, MatcapHeight = 0; // Dimensions of MatcapTexels, set per draw

   // Named input slots.  The planner keeps their order, and the members below refer to them by name.

   std::array<filter_named_input, 3> MapInputs = {{
      { "NormalMap", { .ContributesRegion = false, .Optional = true } },
      { "OcclusionMap", { .Kind = filter_input_kind::SCALAR_MAP, .ContributesRegion = false, .Optional = true,
         .XChannel = filter_map_channel::RED, .Transfer = filter_map_transfer::STORED, .Fallback = { 1.0f, 0.0f },
         .TransparentFallback = true } },
      // The fallback is replaced per draw with the roughness of the Exponent field (see exponent_roughness()).
      { "RoughnessMap", { .Kind = filter_input_kind::SCALAR_MAP, .ContributesRegion = false, .Optional = true,
         .XChannel = filter_map_channel::RED, .Transfer = filter_map_transfer::STORED, .Fallback = { 0.0f, 0.0f },
         .TransparentFallback = true } }
   }};
   filter_named_input &NormalMap = MapInputs[0];
   filter_named_input &OcclusionMap = MapInputs[1];
   filter_named_input &RoughnessMap = MapInputs[2];
   filter_effect_extension Extension = { std::span(MapInputs) };

   NMC    NormalConvention = NMC::Y_DOWN; // Axis convention of NormalMap's green channel
   double NormalStrength = 1.0;           // Multiplies the X and Y components of NormalMap before normalisation
   MCH    OcclusionChannel = MCH::RED;    // Channel of OcclusionMap that supplies the occlusion
   double OcclusionStrength = 1.0;        // Blends the occlusion from none (0) to as supplied (1)
   MCH    RoughnessChannel = MCH::RED;    // Channel of RoughnessMap that supplies the roughness

   extLightingFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      FilterExtension = &Extension;
   }

   ~extLightingFX() {
      release_matcap();
   }

   // Releases the matcap, whether it was loaded from a path or supplied by the client.

   void release_matcap() {
      if (MatcapPinned) {
         Matcap->unpin(true);
         MatcapPinned = false;
      }

      if (MatcapImage) {
         FreeResource(MatcapImage);
         MatcapImage = nullptr;
      }

      Matcap = nullptr;
      MatcapPath.clear();
      MatcapRef.clear();
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
   void prepare_light(const light_record &, lit_light &, double) const;
   double scaled_range(double) const;
   inline void diffuse_light(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double Occlusion = 1.0);
   inline void specular_light(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double Occlusion = 1.0, double Exponent = 0);
   inline void encode_light(const FRGB &, double, uint8_t *, uint8_t, uint8_t, uint8_t);
   inline void encode_hue(const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t);
   inline void enhanced_diffuse(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double);
   LIGHT_INLINE void enhanced_specular(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t,
      uint8_t, uint8_t, double, double, double);
   inline void enhanced_ambient(const point3 &, const point3 &, const FRGB &, uint8_t *, uint8_t, uint8_t, uint8_t,
      uint8_t, double, double);
   inline void enhanced_combined(const point3 &, const point3 &, double, double, const point3 &, const FRGB &,
      const albedo_sample &, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t, double, double);
   inline void enhanced_material(const point3 &, const point3 &, double, double, const point3 &, const FRGB &,
      const albedo_sample &, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t, double, double, double);
   inline void enhanced_lights(const struct lit_surface &, const point3 &, const point3 &, double, double,
      const albedo_sample &, int, int, float, float, float, uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t);
   inline void sample_matcap(const point3 &, float *) const;
   inline uint8_t encode_channel(double, bool);
   void render_distant(int, int, const objBitmap *, const lit_light &, double, const map_height &, int, int);
   void render_spotlight(int, int, const objBitmap *, const lit_light &, double, const map_height &, int, int);
   void render_point(int, int, const objBitmap *, const lit_light &, double, const map_height &, int, int);
   double reach(filter_surface_format) const;

   // The number of lights that a draw renders.  Only the COMBINED type accepts more than one; the guard covers a
   // type that was changed by direct access to the field.

   size_t light_count() const {
      return (Type IS LT::COMBINED) ? Lights.size() : 1;
   }

   // True if the first light uses an extension that only the enhanced pipeline supports.

   bool extended_light() const {
      const auto &first = Lights.front();
      return (first.Intensity != 1.0) or (first.HasColour) or ((first.Range > 0) and (first.Kind != LS::DISTANT));
   }

   filter_sampling sampling(filter_surface_format PrimaryFormat) const {
      if ((not std::isfinite(UnitX)) or (not std::isfinite(UnitY))) {
         constexpr double invalid = std::numeric_limits<double>::quiet_NaN();
         return { filter_access::BOUNDED, invalid, invalid };
      }
      const double radius = reach(PrimaryFormat);
      return { filter_access::BOUNDED, radius, radius };
   }

   // True if ambient light is added to the result.  The specular type ignores it.

   bool has_ambient() const {
      return ((Type IS LT::DIFFUSE) or (Type IS LT::COMBINED)) and (Ambient.Alpha > 0) and
         ((Ambient.Red > 0) or (Ambient.Green > 0) or (Ambient.Blue > 0));
   }

   // Combined shading, normal maps, active occlusion maps and roughness maps opt in under any compliance.
   // Native heights opt in for Kōtuku scenes.

   bool enhanced(filter_surface_format PrimaryFormat) const {
      if ((Type IS LT::COMBINED) or (Type IS LT::NORMAL) or NormalMap.Reference or reads_occlusion() or
         reads_roughness()) return true;
      if ((not Filter->Scene) or (Filter->Scene->Compliance != VSC::NONE)) return false;
      return (PrimaryFormat IS filter_surface_format::SCALAR_F32) or (Features != LFX::NIL) or
         extended_light() or has_ambient();
   }

   // An active map requires positive strength and indirect light from the ambient colour or the combined matcap.

   bool reads_occlusion() const {
      return OcclusionMap.Reference and (OcclusionStrength > 0) and
         (has_ambient() or ((Type IS LT::COMBINED) and Matcap));
   }

   // A roughness map varies the specular highlight, so only the specular and combined types read it.

   bool reads_roughness() const {
      return RoughnessMap.Reference and ((Type IS LT::SPECULAR) or (Type IS LT::COMBINED));
   }

   // True if the effect reads an albedo from its Mix input.

   bool reads_albedo() const {
      return (Type IS LT::COMBINED) and (MixType != VSF::NONE);
   }

   // True if the combined type uses a material term.  With every term at its default, enhanced_combined() is used so
   // that the result is unchanged.

   bool has_material() const {
      return (Type IS LT::COMBINED) and ((Metalness != 0) or (Fresnel != 0) or (Wrap != 0) or (Matcap));
   }

   // True if fine detail receives extra relief, given the radius of the base layer's blur in pixels.

   bool boost_detail(double Sigma) const;
   double cavity_sigma(double UnitSize) const;
   float sharpen_weight(double UnitSize) const;
   ERR draw_enhanced(const filter_surface_view &, const filter_surface_view *, const filter_surface_view *,
      const filter_map_view *, const filter_map_view *, std::span<lit_light>, double, const map_height &, double, int,
      int);
   ERR prepare_matcap();
   template <class DELTA, class COLOUR, class FALLOFF>
   void render_enhanced(int, int, const struct lit_surface &, int, float *, DELTA, COLOUR, FALLOFF);
};

//********************************************************************************************************************
// Prepares a light for a draw.  Positions are converted to pixels relative to the target's clip region, and ranges
// use the lengths of the transformed axes.  Scale is the z-coordinate scale.  The colour, strength, shadows and
// the nearest distance of a ranged light are set by the caller.

void extLightingFX::prepare_light(const light_record &Record, lit_light &Light, double Scale) const
{
   const auto &render = *Filter->Render;
   Light.kind = Record.Kind;

   if (Record.Kind IS LS::DISTANT) {
      Light.direction = Record.Direction;
      Light.half = point3::half_vector(Record.Direction);
      Light.elevation = Record.Elevation * DEG2RAD;
      return;
   }

   auto lt = Record.Position; // Light source
   auto pt = Record.Target;   // Target for the light source, used by LS::SPOT only.

   if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      // Light positions are relative to the client's own path bounds.
      const auto &bounds = render.ClientPathBounds;
      const double width = bounds.width(), height = bounds.height();
      const auto diagonal = filter_render_context::normalised_diagonal(width, height);

      lt.x = (lt.x * width) + bounds.left;
      lt.y = (lt.y * height) + bounds.top;
      lt.z *= diagonal;

      if (Record.Kind IS LS::SPOT) {
         pt.x = (pt.x * width) + bounds.left;
         pt.y = (pt.y * height) + bounds.top;
         pt.z *= diagonal;
      }
   }

   const auto &t = render.Transform;
   lt.z *= Scale;
   t.transform(&lt.x, &lt.y);
   if (Record.Range > 0) Light.range = scaled_range(Record.Range);

   // Rendering algorithm requires light source coordinates to be relative to the exposed bitmap.

   if (Record.Kind IS LS::SPOT) {
      t.transform(&pt.x, &pt.y);
      pt.z *= Scale;

      // The spot delta is the direction from the light to its target.

      Light.spot_delta = point3(pt.x, pt.y, pt.z) - point3(lt.x, lt.y, lt.z);

      if (Light.spot_delta.dot(Light.spot_delta) > 1e-10) Light.spot_delta.normalise();
      else Light.spot_delta = point3(0, 0, -1);

      Light.spot_exponent = Record.SpotExponent;
      Light.cone_angle = Record.ConeAngle;
      if (Record.ConeAngle) {
         Light.cos_outer = cos(Record.ConeAngle * DEG2RAD);
         const double AA_THRESHOLD = 0.016;
         Light.cos_inner = Light.cos_outer + AA_THRESHOLD;
         Light.cone_scale = 1.0 / AA_THRESHOLD;
      }
   }

   lt.x -= Target->Clip.Left; // Re-orient the light source coordinates to (0,0)
   lt.y -= Target->Clip.Top;
   Light.position = lt;
}

//********************************************************************************************************************
// Converts a range from primitive units to pixels using the lengths of the transformed axes.  This also works
// before the clip region is known.

double extLightingFX::scaled_range(double Range) const
{
   const auto &render = *Filter->Render;
   if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      const auto &bounds = render.ClientPathBounds;
      Range *= filter_render_context::normalised_diagonal(bounds.width(), bounds.height());
   }
   const auto [sx, sy] = render.axis_scales();
   return Range * std::hypot(sx * INV_SQRT2, sy * INV_SQRT2);
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

//********************************************************************************************************************

inline void extLightingFX::specular_light(const point3 &Normal, const point3 &STL, const FRGB &Colour, uint8_t *Output,
   uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion, double Exponent)
{
   const point3 halfDir = point3::half_vector(STL);

   const double exponent = (Exponent > 0) ? Exponent : SpecularExponent;
   double scale = std::clamp((Constant * std::pow(std::max(0.0, Normal.dot(halfDir)), exponent)) * 255.0
      * Occlusion, 0.0, 255.0);

   // The light (S, max(S)) is premultiplied, as browsers interpret it, and is stored unpremultiplied.  The alpha is
   // the brightest channel of the light in the filter's colour space and the colour is the light's hue.

   const uint8_t r = int(Colour.Red * scale);
   const uint8_t g = int(Colour.Green * scale);
   const uint8_t b = int(Colour.Blue * scale);
   Output[A] = std::max(std::max(r, g), b);
   encode_hue(Colour, Output, R, G, B, A);
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

//********************************************************************************************************************
// Writes the straight colour of specular light.  At zero stored alpha, clear RGB so a later filter cannot read a hue
// from an unlit pixel.

inline void extLightingFX::encode_hue(const FRGB &Colour, uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
   const double peak = std::max(std::max(Colour.Red, Colour.Green), Colour.Blue);
   if ((not Output[A]) or (peak <= 0)) {
      Output[R] = Output[G] = Output[B] = 0;
      return;
   }

   const double inverse_peak = 1.0 / peak;
   Output[R] = encode_channel(Colour.Red * inverse_peak, true);
   Output[G] = encode_channel(Colour.Green * inverse_peak, true);
   Output[B] = encode_channel(Colour.Blue * inverse_peak, true);
}

//********************************************************************************************************************
// Strength is Constant multiplied by Intensity, which is exactly Constant when Intensity is 1.

inline void extLightingFX::enhanced_diffuse(const point3 &Normal, const point3 &STL, const FRGB &Colour,
   uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion)
{
   encode_light(Colour, std::clamp(Strength * Normal.dot(STL) * Occlusion, 0.0, 1.0), Output, R, G, B);
   Output[A] = 255;
}

//********************************************************************************************************************
// The light is premultiplied as in specular_light().  The alpha is truncated in sRGB filters, so that a flat surface
// keeps the SVG result, and rounded in linear filters.  Gain scales the highlight for a roughness map, and is exactly
// 1 without one, which leaves the result unchanged.

LIGHT_INLINE void extLightingFX::enhanced_specular(const point3 &Normal, const point3 &STL, const FRGB &Colour,
   uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A, double Occlusion, double Exponent, double Gain)
{
   const point3 half_dir = point3::half_vector(STL);

   const double scale = std::clamp(Strength * Gain * std::pow(std::max(0.0, Normal.dot(half_dir)), Exponent) *
      Occlusion, 0.0, 1.0);

   const double level = std::max(std::max(Colour.Red, Colour.Green), Colour.Blue) * scale * 255.0;
   Output[A] = Linear ? uint8_t(level + 0.5) : uint8_t(level);
   encode_hue(Colour, Output, R, G, B, A);
}

//********************************************************************************************************************
// Samples the matcap for a shading normal with bilinear filtering.  The normal's X and Y, from -1 to 1, map to the
// centres of the first and last pixels of the matcap, so a flat surface takes the colour at its centre.  Normals are
// in image orientation, so the top of the matcap colours faces that turn towards the top of the image.  The result is
// premultiplied by the matcap's alpha.

// Texels are stored as four floats, red, green, blue and an unused fourth, so that the channels can be interpolated
// together as one vector.

inline void extLightingFX::sample_matcap(const point3 &Normal, float *Result) const
{
   if ((not std::isfinite(Normal.x)) or (not std::isfinite(Normal.y))) {
      std::fill_n(Result, 4, 0.0f);
      return;
   }

   const int last_x = MatcapWidth - 1, last_y = MatcapHeight - 1;
   const float u = std::clamp((float(Normal.x) * 0.5f) + 0.5f, 0.0f, 1.0f) * float(last_x);
   const float v = std::clamp((float(Normal.y) * 0.5f) + 0.5f, 0.0f, 1.0f) * float(last_y);
   const int x0 = int(u), y0 = int(v);
   const int x1 = std::min(x0 + 1, last_x), y1 = std::min(y0 + 1, last_y);
   const float fx = u - float(x0), fy = v - float(y0);

   const float *r0 = MatcapTexels + (std::ptrdiff_t(y0) * MatcapWidth * 4);
   const float *r1 = MatcapTexels + (std::ptrdiff_t(y1) * MatcapWidth * 4);
   const float *t00 = r0 + (x0 * 4), *t01 = r0 + (x1 * 4);
   const float *t10 = r1 + (x0 * 4), *t11 = r1 + (x1 * 4);
   for (int c=0; c < 4; c++) {
      const float top = t00[c] + ((t01[c] - t00[c]) * fx);
      const float bottom = t10[c] + ((t11[c] - t10[c]) * fx);
      Result[c] = top + ((bottom - top) * fy);
   }
}

//********************************************************************************************************************
// Encodes one channel of light, clamped to 0 - 1, for output.  Round selects rounding in sRGB filters; otherwise the
// value is truncated as the SVG functions do.  Linear light is always rounded.

inline uint8_t extLightingFX::encode_channel(double Value, bool Round)
{
   const float value = float(std::clamp(Value, 0.0, 1.0));
   if (Linear) return glLinearRGB.invert16(uint16_t((value * 65535.0f) + 0.5f));
   else return uint8_t((value * 255.0f) + (Round ? 0.5f : 0.0f));
}

//********************************************************************************************************************
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

//********************************************************************************************************************
// The combined type.  Normal is the pixel's own normal, used for diffuse light, and Mean is the normal with which the
// specular light is shaded, at the given Exponent (see row_toksvig()), and Gain scales the specular light for a
// roughness map.  Light is not limited to full strength; the result is clamped once, on output.  The albedo is
// premultiplied for the sum, so the specular light is added as the arithmetic composite would add a premultiplied
// specular map.  The result's alpha is the albedo's alpha raised to the brightest channel of the specular light, and
// its colour is divided by that alpha for storage.

inline void extLightingFX::enhanced_combined(const point3 &Normal, const point3 &Mean, double Exponent, double Gain,
   const point3 &STL, const FRGB &Light, const albedo_sample &Albedo, uint8_t *Output, uint8_t R, uint8_t G,
   uint8_t B, uint8_t A, double Occlusion, double Cavity)
{
   const point3 half_dir = point3::half_vector(STL);

   const double diffuse = Constant * std::max(0.0, Normal.dot(STL)) * Occlusion;
   const double specular = SpecularConstant * Gain * std::pow(std::max(0.0, Mean.dot(half_dir)), Exponent) *
      Occlusion;

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
// The combined type with material terms, selected per draw when any term differs from its default (see
// has_material()).  The specular light is multiplied by a reflectance per channel, F0 = lerp(base, albedo, Metalness),
// where base is Fresnel if it is set and 1 otherwise.  The albedo is premultiplied here, so that a metal's
// reflection fades with the albedo's coverage at anti-aliased edges and is absent where the albedo is transparent.
// With Fresnel set, the reflectance rises towards 1 as the shading normal turns away from the eye: Schlick's term on
// Mean.z, the cosine between the normal and the fixed eye vector.  Metalness removes the diffuse light, and Wrap
// replaces N.L with (N.L + w) / (1 + w).  Ambient light and the output rule are those of enhanced_combined().
//
// Raised is the occlusion with shadows cast by the light raised by the wrap angle (see WrapShadows).  The diffuse light
// of a face that turns away from the light takes it in place of Occlusion, so that the face is not darkened by its own
// slope.  The change is blended in from N.L = WrapBlend, the sine of the penumbra, to the terminator, over which the
// ordinary shadow fades out on the face's own slope.  Specular light keeps the ordinary shadow.
//
// A matcap adds matcap(Mean) x F x Cavity x coverage to the specular light.  Its F uses the straight albedo, because
// the term is weighted by the albedo's coverage separately.  It is independent of the light and is not shadowed.

inline void extLightingFX::enhanced_material(const point3 &Normal, const point3 &Mean, double Exponent, double Gain,
   const point3 &STL, const FRGB &Light, const albedo_sample &Albedo, uint8_t *Output, uint8_t R, uint8_t G,
   uint8_t B, uint8_t A, double Occlusion, double Cavity, double Raised)
{
   const point3 half_dir = point3::half_vector(STL);

   const double nl = Normal.dot(STL);
   double diffuse_occlusion = Occlusion;
   if ((WrapShadows) and (nl < WrapBlend)) {
      const double turned = (nl <= 0) ? 1.0 : 1.0 - (nl / WrapBlend);
      diffuse_occlusion += turned * (Raised - Occlusion);
   }

   const double lit = std::max(0.0, (nl + Wrap) * WrapScale);
   const double diffuse = Constant * (1.0 - Metalness) * lit * diffuse_occlusion;
   const double specular = SpecularConstant * Gain * std::pow(std::max(0.0, Mean.dot(half_dir)), Exponent) *
      Occlusion;

   const double coverage = Albedo.a;
   const double base = (Fresnel > 0) ? Fresnel : 1.0;
   double fr = base + (Metalness * ((Albedo.r * coverage) - base));
   double fg = base + (Metalness * ((Albedo.g * coverage) - base));
   double fb = base + (Metalness * ((Albedo.b * coverage) - base));
   double grazing = 0;

   if (Fresnel > 0) {
      const double q = 1.0 - std::clamp(Mean.z, 0.0, 1.0);
      const double q2 = q * q;
      grazing = q2 * q2 * q;
      fr += (1.0 - fr) * grazing;
      fg += (1.0 - fg) * grazing;
      fb += (1.0 - fb) * grazing;
   }

   double sr = specular * fr * Light.Red, sg = specular * fg * Light.Green, sb = specular * fb * Light.Blue;

   if ((MatcapTexels) and (coverage > 0)) {
      alignas(16) float matcap[4];
      sample_matcap(Mean, matcap);

      const double weight = Cavity * coverage;
      double cr = base + (Metalness * (Albedo.r - base));
      double cg = base + (Metalness * (Albedo.g - base));
      double cb = base + (Metalness * (Albedo.b - base));

      cr += (1.0 - cr) * grazing;
      cg += (1.0 - cg) * grazing;
      cb += (1.0 - cb) * grazing;
      sr += matcap[0] * cr * weight;
      sg += matcap[1] * cg * weight;
      sb += matcap[2] * cb * weight;
   }

   const double alpha = std::max(coverage, std::min(1.0, std::max(std::max(sr, sg), sb)));
   if (alpha <= 0) {
      Output[R] = Output[G] = Output[B] = Output[A] = 0;
      return;
   }

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

void extLightingFX::render_distant(int StartY, int EndY, const objBitmap *Bitmap, const lit_light &Light,
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
         diffuse_light(leftNormal(m, NormalHeight), Light.direction, Light.colour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            diffuse_light(interiorNormal(m, NormalHeight), Light.direction, Light.colour, dptr, R, G, B, A);
            dptr += bpp;
         }

         // Process right edge pixel
         if (Width > 1) {
            shiftMatrixLeft(m);
            diffuse_light(rightNormal(m, NormalHeight), Light.direction, Light.colour, dptr, R, G, B, A);
         }
      }
      else { // SPECULAR
         specular_light(leftNormal(m, NormalHeight), Light.direction, Light.colour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            specular_light(interiorNormal(m, NormalHeight), Light.direction, Light.colour, dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            specular_light(rightNormal(m, NormalHeight), Light.direction, Light.colour, dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

void extLightingFX::render_spotlight(int StartY, int EndY, const objBitmap *Bitmap, const lit_light &Light,
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
      const auto &lt = Light.position;
      point3 direction(lt.x - X, lt.y - Y, lt.z - (double(alpha_val) * (1.0 / 255.0) * SpotHeight));
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
         diffuse_light(leftNormal(m, NormalHeight), stl, Light.spot_colour(stl), dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            diffuse_light(interiorNormal(m, NormalHeight), stl, Light.spot_colour(stl), dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            diffuse_light(rightNormal(m, NormalHeight), stl, Light.spot_colour(stl), dptr, R, G, B, A);
         }
      }
      else { // SPECULAR
         point3 stl = read_light_delta(0, y, m[4]);
         specular_light(leftNormal(m, NormalHeight), stl, Light.spot_colour(stl), dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            specular_light(interiorNormal(m, NormalHeight), stl, Light.spot_colour(stl), dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            specular_light(rightNormal(m, NormalHeight), stl, Light.spot_colour(stl), dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

void extLightingFX::render_point(int StartY, int EndY, const objBitmap *Bitmap, const lit_light &Light,
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
      const auto &lt = Light.position;
      point3 direction(lt.x - X, lt.y - Y, lt.z - (double(alpha_val) * (1.0 / 255.0) * SpotHeight));
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
         diffuse_light(leftNormal(m, NormalHeight), stl, Light.colour, dptr, R, G, B, A);
         if (Width <= 1) continue;

         dptr += bpp;

         for (int x=1; x < Width-1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            diffuse_light(interiorNormal(m, NormalHeight), stl, Light.colour, dptr, R, G, B, A);
            dptr += bpp;
         }

         shiftMatrixLeft(m);
         stl = read_light_delta(Width-1, y, m[4]);
         diffuse_light(rightNormal(m, NormalHeight), stl, Light.colour, dptr, R, G, B, A);
      }
      else { // SPECULAR
         point3 stl = read_light_delta(0, y, m[4]);
         specular_light(leftNormal(m, NormalHeight), stl, Light.colour, dptr, R, G, B, A);
         dptr += bpp;

         for (int x=1; x < Width - 1; ++x) {
            shiftMatrixLeft(m);
            m[2] = row0[A]; row0 += bpp;
            m[5] = row1[A]; row1 += bpp;
            m[8] = row2[A]; row2 += bpp;
            stl = read_light_delta(x, y, m[4]);
            specular_light(interiorNormal(m, NormalHeight), stl, Light.colour, dptr, R, G, B, A);
            dptr += bpp;
         }

         if (Width > 1) {
            shiftMatrixLeft(m);
            stl = read_light_delta(Width-1, y, m[4]);
            specular_light(rightNormal(m, NormalHeight), stl, Light.colour, dptr, R, G, B, A);
         }
      }
   }
}

//********************************************************************************************************************

static void write_lfx_features(std::ostream &Stream, LFX Features)
{
   static const std::pair<LFX, CSTRING> names[] = {
      { LFX::DEQUANTISE, "dequantise" }, { LFX::SHARPEN, "sharpen" }, { LFX::CAVITY, "cavity" },
      { LFX::DETAIL, "detail" }, { LFX::SHADOWS, "shadows" }, { LFX::SPECULAR_AA, "specular-aa" },
      { LFX::DEQUANTISE_WIDE, "dequantise-wide" }
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

//********************************************************************************************************************
// Writes a light setting as a kotuku: attribute, in the shortest form that reads back as the same value.

static void write_light_number(std::ostream &Stream, CSTRING Name, double Value)
{
   char buffer[32];
   auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), Value);
   Stream << " kotuku:" << Name << "=\"" << std::string_view(buffer, (error IS std::errc()) ? end - buffer : 0)
      << '"';
}

//********************************************************************************************************************
// Converts a public channel selection to the channel of a map input contract.

static filter_map_channel map_channel(MCH Channel)
{
   switch (Channel) {
      case MCH::GREEN:     return filter_map_channel::GREEN;
      case MCH::BLUE:      return filter_map_channel::BLUE;
      case MCH::ALPHA:     return filter_map_channel::ALPHA;
      case MCH::LUMINANCE: return filter_map_channel::LUMINANCE;
      default:             return filter_map_channel::RED;
   }
}

//*********************************************************************************************************************

ERR extLightingFX::draw()
{
   filter_surface_view primary;
   if (auto error = get_source_view(this, primary, filter_input_role::INPUT); error != ERR::Okay) return error;
   const auto bmp = primary.Bitmap;

   // Light is computed in the filter's colour space.  Bitmaps are always sRGB encoded, so linear results are encoded
   // on output.

   Linear = effect_colour_space(this) IS VCS::LINEAR_RGB;
   LightColour = Linear ? LinearColour : Colour;
   LightSource = Lights.front().Kind;

   const auto &render = *Filter->Render;
   const auto &t = render.Transform;
   const double scale = (t.sx IS t.sy) ? t.sx : filter_render_context::normalised_diagonal(t.sx, t.sy);

   // Light positions are prepared for this draw.  The SVG algorithm uses the first light and the effect's colour.

   std::vector<lit_light> lights((Type IS LT::NORMAL) ? 0 : light_count());
   for (size_t i=0; i < lights.size(); i++) {
      prepare_light(Lights[i], lights[i], scale);
      lights[i].colour = LightColour;
   }

   // The input height is linear and uses the stored alpha channel when the source is a bitmap.

   int width  = Target->Clip.Right - Target->Clip.Left;
   int height = Target->Clip.Bottom - Target->Clip.Top;
   if (bmp) {
      width = std::min(width, bmp->Clip.Right - bmp->Clip.Left);
      height = std::min(height, bmp->Clip.Bottom - bmp->Clip.Top);
   }

   if ((width < 1) or (height < 1)) return ERR::Okay;

   // The surface height is in client units, converted to pixels for locating the surface relative to the light.  The
   // Sobel gradients are measured per pixel, so the heights for the surface normal are expressed per kernel unit
   // instead.  This approximates the resampling of the bump map to the kernel unit grid that SVG describes.

   const double spot_height = MapHeight * scale;
   const auto unit = kernel_unit_pixels(scale);
   const double normal_scale = MapHeight * (1.0 / 255.0);
   const map_height normal_height = { unit.x * normal_scale, unit.y * normal_scale, unit.xy * normal_scale };

   if (enhanced(primary.Format)) {
      filter_surface_view albedo;
      if (reads_albedo()) {
         if (auto error = get_source_view(this, albedo, filter_input_role::MIX); error != ERR::Okay) return error;
         if ((not albedo.Bitmap) or (albedo.Bitmap->BytesPerPixel != 4)) return ERR::InvalidState;
      }

      filter_surface_view normals;
      if (NormalMap.Reference) {
         filter_surface_view source;
         if (auto error = get_named_filter_bitmap(this, NormalMap.Name, source); error != ERR::Okay) return error;
         if (source.Bitmap->BytesPerPixel != 4) return ERR::InvalidState;
         if (auto error = working_input(this, source, source.Encoding, filter_alpha::STRAIGHT, normals);
             error != ERR::Okay) return error;
      }

      filter_map_view occlusion;
      if (reads_occlusion()) {
         if (auto error = get_filter_map(this, OcclusionMap.Name, occlusion); error != ERR::Okay) return error;
         occlusion.Contract.XChannel = map_channel(OcclusionChannel);
      }

      // Pixels that the roughness map does not cover read the roughness of the Exponent field, so that they blend
      // into the constant highlight at the edges of the map.

      filter_map_view roughness;
      if (reads_roughness()) {
         if (auto error = get_filter_map(this, RoughnessMap.Name, roughness); error != ERR::Okay) return error;
         roughness.Contract.XChannel = map_channel(RoughnessChannel);
         roughness.Contract.Fallback = { float(exponent_roughness(SpecularExponent)), 0.0f };
      }

      const double unit_size = unit_length(unit);
      return draw_enhanced(primary, reads_albedo() ? &albedo : nullptr, normals.Bitmap ? &normals : nullptr,
         occlusion.Source.Data ? &occlusion : nullptr, roughness.Source.Data ? &roughness : nullptr, lights,
         spot_height, normal_height, unit_size, width, height);
   }

   if (not bmp) return ERR::NoSupport;

   int num_threads = std::min(int(std::thread::hardware_concurrency()), height);
   if (num_threads < 1) num_threads = 1;

   const int min_rows_per_chunk = 4; // Minimum work per thread to avoid overhead
   const int chunk_size = std::max(min_rows_per_chunk, height / num_threads);
   const int num_chunks = (height + chunk_size - 1) / chunk_size;

   BS::thread_pool pool(num_threads);
   const auto &light = lights.front();

   for (int chunk=0; chunk < num_chunks; ++chunk) {
      const int start_y = chunk * chunk_size;
      const int end_y = std::min(start_y + chunk_size, height);

      pool.detach_task([&, start_row = start_y, end_row = end_y]() {
         if (LightSource IS LS::DISTANT) {
            render_distant(start_row, end_row, bmp, light, spot_height, normal_height, width, height);
         }
         else if (LightSource IS LS::SPOT) {
            render_spotlight(start_row, end_row, bmp, light, spot_height, normal_height, width, height);
         }
         else if (LightSource IS LS::POINT) {
            render_point(start_row, end_row, bmp, light, spot_height, normal_height, width, height);
         }
      });
   }

   pool.wait();
   return ERR::Okay;
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

//********************************************************************************************************************
// Replaces the light list with one light, which keeps the first light's intensity.

static void replace_lights(extLightingFX *Self, light_record Light)
{
   Light.Intensity = Self->Lights.front().Intensity;
   Self->Lights.assign(1, Light);
   Self->Configured = true;
}

// Appends a light.  The first append replaces the implicit light and keeps its intensity.  Further lights require the
// COMBINED type.

static ERR append_light(extLightingFX *Self, light_record Light)
{
   kt::Log log;

   if (not Self->Configured) {
      replace_lights(Self, Light);
      return ERR::Okay;
   }

   if (Self->Type != LT::COMBINED) {
      log.warning("Multiple lights require the COMBINED type.");
      return ERR::InvalidState;
   }

   Self->Lights.push_back(Light);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddDistantLight: Add a distant light source to the effect.

This method adds a distant light to the effect, after any lights that are already configured.  The arguments are
those of #SetDistantLight().

If no light has been configured, the new light replaces the default light and keeps the value of #Intensity.  Each
further light requires a #Type of `COMBINED`, which is the only type that renders more than one light; otherwise
`ERR::InvalidState` is returned and the lights are unchanged.  The new light uses the effect's #Colour, an intensity
of 1, an unlimited range and shadows.  Use #SetLightOptions() to change them.

-INPUT-
double Azimuth: Direction angle for the light source on the XY plane (clockwise), in degrees from the X axis.
double Elevation: Direction angle for the light source from the XY plane towards the Z axis, in degrees.  A positive value points towards the viewer of the content.

-RESULT-
Okay:
NullArgs:
InvalidState: The effect already has a light and its type is not `COMBINED`.

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_AddDistantLight(extLightingFX *Self, struct lt::AddDistantLight *Args)
{
   kt::Log log;
   if (not Args) return log.warning(ERR::NullArgs);
   return append_light(Self, light_record::distant(Args->Azimuth, Args->Elevation));
}

/*********************************************************************************************************************

-METHOD-
AddPointLight: Add a point light source to the effect.

This method adds a point light to the effect, after any lights that are already configured.  The arguments are those
of #SetPointLight(), and the rules of #AddDistantLight() apply.

-INPUT-
double X: X location for the light source.
double Y: Y location for the light source.
double Z: Z location for the light source.

-RESULT-
Okay:
NullArgs:
InvalidState: The effect already has a light and its type is not `COMBINED`.

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_AddPointLight(extLightingFX *Self, struct lt::AddPointLight *Args)
{
   kt::Log log;
   if (not Args) return log.warning(ERR::NullArgs);
   return append_light(Self, light_record::point(Args->X, Args->Y, Args->Z));
}

/*********************************************************************************************************************

-METHOD-
AddSpotLight: Add a spot light source to the effect.

This method adds a spot light to the effect, after any lights that are already configured.  The arguments are those
of #SetSpotLight(), and the rules of #AddDistantLight() apply.

-INPUT-
double X: X location for the light source.
double Y: Y location for the light source.
double Z: Z location for the light source.
double PX: X location of the light source's target.
double PY: Y location of the light source's target.
double PZ: Z location of the light source's target.
double Exponent: Exponent value controlling the focus for the light source.
double ConeAngle: A limiting cone which restricts the region where the light is projected, or 0 to disable.  Specified in degrees.

-RESULT-
Okay:
NullArgs:
InvalidState: The effect already has a light and its type is not `COMBINED`.

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_AddSpotLight(extLightingFX *Self, struct lt::AddSpotLight *Args)
{
   kt::Log log;
   if (not Args) return log.warning(ERR::NullArgs);
   return append_light(Self, light_record::spot(Args->X, Args->Y, Args->Z, Args->PX, Args->PY, Args->PZ,
      Args->Exponent, Args->ConeAngle));
}

/*********************************************************************************************************************

-METHOD-
SetDistantLight: Configure lighting with a distant light source.

This method applies a distant light configuration to the lighting effect.  It replaces every configured light with
the new light, which keeps the intensity of the first light and uses the effect's #Colour, an unlimited range and
shadows.

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

   replace_lights(Self, light_record::distant(Args->Azimuth, Args->Elevation));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetLightOptions: Set the colour, intensity, range and shadows of a light.

This method sets the options of the light at Index, where 0 is the first light that was configured with a light
method.  All of the options in the !LightOptions structure are applied together.  The options are validated before
the light is changed, so the light is unchanged if an error is returned.

<list type="bullet">
<li>`Colour` replaces the effect's #Colour for this light if `Flags` includes `COLOUR`.  It is in the same colour space
as #Colour, and its alpha is ignored.  Without the flag the light uses the effect's #Colour.</li>
<li>`Intensity` multiplies the light's colour, as #Intensity does for the first light.  It must be zero or greater.</li>
<li>`Range` is the distance at which a point or spot light fades to nothing, in the units of its z coordinate.  Zero
means that the light is unlimited.  A distant light cannot have a range.</li>
<li>The `NO_SHADOWS` flag stops a distant light from casting shadows.</li>
</list>

A light with a range is brightest at the point of the surface nearest to it, where its strength is unchanged.  Its
strength falls with the square of the distance from there, and is multiplied by a smooth window that reaches zero at
the range.  If no point of the surface lies within range, the light has no effect.

The options are extensions of the SVG lighting model.  They take effect when the enhanced pipeline is in use, which is
always the case for the `COMBINED` type; refer to #Features.  #SetDistantLight() and its siblings reset the options
of the light they configure, except for the intensity of the first light.

-INPUT-
int Index: Index of the light, from zero.
struct(*LightOptions) Options: The options to apply.

-RESULT-
Okay:
NullArgs:
OutOfRange: The Index does not refer to a light, or the Intensity or Range is negative or not finite.
InvalidValue: A colour channel is not finite, the Flags are not recognised or a distant light was given a Range.

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_SetLightOptions(extLightingFX *Self, struct lt::SetLightOptions *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Options)) return log.warning(ERR::NullArgs);

   if ((Args->Index < 0) or (size_t(Args->Index) >= Self->Lights.size())) return log.warning(ERR::OutOfRange);

   const auto &options = *Args->Options;
   auto &light = Self->Lights[Args->Index];

   if ((not std::isfinite(options.Intensity)) or (options.Intensity < 0)) return log.warning(ERR::OutOfRange);
   if ((not std::isfinite(options.Range)) or (options.Range < 0)) return log.warning(ERR::OutOfRange);
   if ((options.Flags & ~(LOF::COLOUR | LOF::NO_SHADOWS)) != LOF::NIL) return log.warning(ERR::InvalidValue);

   const bool has_colour = (options.Flags & LOF::COLOUR) != LOF::NIL;
   if ((has_colour) and ((not std::isfinite(options.Colour.Red)) or (not std::isfinite(options.Colour.Green)) or
       (not std::isfinite(options.Colour.Blue)))) return log.warning(ERR::InvalidValue);

   if ((options.Range > 0) and (light.Kind IS LS::DISTANT)) {
      log.warning("A distant light cannot have a range.");
      return ERR::InvalidValue;
   }

   light.HasColour = has_colour;
   if (has_colour) {
      light.Colour = { options.Colour.Red, options.Colour.Green, options.Colour.Blue, 1.0f };
      light.LinearColour = light.Colour;
      glLinearRGB.convert(light.LinearColour);
   }
   else light.Colour = light.LinearColour = { 1.0f, 1.0f, 1.0f, 1.0f };

   light.Intensity = options.Intensity;
   light.Range = options.Range;
   light.Shadows = (options.Flags & LOF::NO_SHADOWS) IS LOF::NIL;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetPointLight: Configure lighting with a pointed light source.

This method applies a pointed light configuration to the lighting effect.  It replaces every configured light with
the new light, which keeps the intensity of the first light and uses the effect's #Colour and an unlimited range.

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

   replace_lights(Self, light_record::point(Args->X, Args->Y, Args->Z));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetSpotLight: Configure lighting with a spot light source.

This method applies a spot light configuration to the lighting effect.  It replaces every configured light with the
new light, which keeps the intensity of the first light and uses the effect's #Colour and an unlimited range.

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

   replace_lights(Self, light_record::spot(Args->X, Args->Y, Args->Z, Args->PX, Args->PY, Args->PZ, Args->Exponent,
      Args->ConeAngle));
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

This field requires the enhanced pipeline.  The `COMBINED` type always uses it.  For `DIFFUSE`, an active
#OcclusionMap enables it even when the scene's Compliance is `SVG1` or `SVG2`.

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

If a #RoughnessMap is bound, the map supplies the exponent of each pixel and this field is the reference exponent at
which the specular constant is calibrated.  Pixels that the map does not cover use this exponent.

*********************************************************************************************************************/

static ERR LIGHTINGFX_SET_Exponent(extLightingFX *Self, double Value)
{
   if ((Value >= MIN_SPECULAR_EXPONENT) and (Value <= MAX_SPECULAR_EXPONENT)) {
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

Both dequantisation modes apply to 8-bit alpha inputs only.  Use `DEQUANTISE_WIDE` for shallow slopes in existing
8-bit height maps.  Select it alone to keep isolated ledges and terraces without successive one-level steps at
their original heights.

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
   if ((Value & ~(LFX::ALL | LFX::DEQUANTISE_WIDE)) != LFX::NIL) return ERR::InvalidValue;
   Self->Features = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Fresnel: Reflectance at normal incidence for the `COMBINED` type, which enables a Fresnel term.

Real surfaces reflect more light at grazing angles than when they are seen head on.  When this field is set, the
specular light of the `COMBINED` type is multiplied by a reflectance that equals this value where the surface faces
the viewer and rises towards 1 as the surface turns away, using Schlick's approximation.  As the viewer is always
directly in front of the content, the angle is that between the shading normal and the viewer, so the term brightens
the highlights of steep slopes and edges.  It is a facing-ratio term rather than a microfacet one: the microfacet form
depends on the angle between the light and the viewer, which is constant over the image for a distant light.  A
highlight peaks where the surface turns no more than 45 degrees from the viewer, where the reflectance is still close
to this value, so on the highlight alone the term chiefly affects the broad highlights of low exponents.

The term is most visible with a #Matcap, whose reflection is also multiplied by the reflectance.  The reflection is
then faint where the surface faces the viewer and strong towards its edges, as on glazed ceramic, varnish and glass.

Typical values are about 0.04 for dielectrics such as plastic, glass and glazed ceramic.  Note that the highlight of a
flat surface is multiplied by this value, so content that enables Fresnel needs a larger #SpecularConstant or #Intensity
for the same highlight: about `1 / Fresnel` times as large.  The matcap's reflection is not scaled by #SpecularConstant;
use a brighter matcap if it is too faint.  With #Metalness, the reflectance at normal incidence blends from this value
to the albedo's colour.

The default is 0, which disables the term.  The value must be between 0 and 1.  The diffuse and specular types ignore
this field.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Fresnel(extLightingFX *Self, double &Value)
{
   Value = Self->Fresnel;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Fresnel(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0) or (Value > 1.0)) return ERR::OutOfRange;
   Self->Fresnel = Value;
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

If the effect has more than one light, this field is the intensity of the first light; use #SetLightOptions() for the
others.  The value can be set before or after the light is configured, as #SetDistantLight() and its siblings keep
the intensity of the first light.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Intensity(extLightingFX *Self, double &Value)
{
   Value = Self->Lights.front().Intensity;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Intensity(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->Lights.front().Intensity = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Matcap: A matcap image for the `COMBINED` type, supplied as a bitmap.

A matcap ('material capture') is an image of a sphere that reflects an environment.  When the `COMBINED` type has a
matcap, each pixel adds the colour of the point on the sphere that faces the same way as the surface to its specular
light, so that the surface reflects the environment.  Matcaps produce reflections that the light model cannot, such
as those of chrome, glass, glazed ceramic or brushed metal.

The surface normal indexes the image directly: a flat surface takes the colour at the centre of the image, and a
surface that faces up, down, left or right takes the colour at the middle of the top, bottom, left or right edge.  The
corners of the image are never read, so the area outside the sphere is unused.  The image is sampled with bilinear
filtering, using the same anti-aliased normal as the specular highlight.  Its alpha channel scales the reflection, so
a translucent matcap gives a weaker reflection.  A 32-bit bitmap without an alpha channel is treated as opaque.

The reflection is multiplied by the reflectance of the surface, so it takes the albedo's colour as #Metalness rises
and, if #Fresnel is set, it is weak where the surface faces the viewer and strong at grazing angles.  It is weighted by
the albedo's alpha, reduced by the `CAVITY` feature, and is not affected by shadows or by the light.  To show the
reflection alone, set #SpecularConstant to zero.  The diffuse and specular types ignore the matcap.

Setting this field to a bitmap replaces any matcap that was loaded from #MatcapPath, and clears #MatcapPath.  The
bitmap must be initialised and use 32 bits per pixel; otherwise `ERR::InvalidValue` is returned and the field is
unchanged.  The bitmap is pinned until it is replaced or the effect is freed, so an attempt to free it earlier is
deferred.  Changes to its content are used on the next draw of the filter.  Set the field to `NULL`, or #MatcapPath
to an empty string, to remove the matcap.

When the matcap was loaded from #MatcapPath, reading this field returns the loaded bitmap.  A client-supplied bitmap
cannot be written by #XMLDef.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Matcap(extLightingFX *Self, objBitmap **Value)
{
   *Value = Self->Matcap;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Matcap(extLightingFX *Self, objBitmap *Value)
{
   kt::Log log;

   if (not Value) {
      Self->release_matcap();
      return ERR::Okay;
   }

   if (Value IS Self->Matcap) return ERR::Okay;

   if (Value->classID() != CLASSID::BITMAP) return log.warning(ERR::WrongClass);
   if (not Value->initialised()) return log.warning(ERR::NotInitialised);
   if (Value->BytesPerPixel != 4) {
      log.warning("The matcap bitmap #%d has %d bytes per pixel; 4 are required.", Value->UID, Value->BytesPerPixel);
      return ERR::InvalidValue;
   }

   Value->pin();
   Self->release_matcap();
   Self->Matcap = Value;
   Self->MatcapPinned = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MatcapPath: Loads a matcap image for the `COMBINED` type from a file.

Set this field to the path of an image file that the @Image class supports to use it as the #Matcap.  The image is
loaded immediately and converted to 32 bits per pixel with an alpha channel.  If it cannot be loaded, the error is
returned and the current matcap, if any, is kept.  Setting the field to an empty string removes the matcap.  Setting
#Matcap clears this field.

Matcaps are usually square images of 256 x 256 pixels or more.  Refer to #Matcap for details of how the image is
used.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_MatcapPath(extLightingFX *Self, std::string_view &Value)
{
   Value = Self->MatcapPath;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_MatcapPath(extLightingFX *Self, const std::string_view &Value)
{
   kt::Log log;

   if (Value.empty()) {
      Self->release_matcap();
      return ERR::Okay;
   }

   objImage::create image({ fl::Path(Value), fl::BitsPerPixel(32), fl::Flags(PCF::FORCE_ALPHA_32) }, NF::LOCAL);
   if (not image.ok()) {
      log.warning("Failed to load matcap '%.*s': %s", int(Value.size()), Value.data(), GetErrorMsg(image.error));
      return image.error;
   }

   auto loaded = image.detach();
   Self->release_matcap();
   Self->MatcapImage = loaded;
   Self->Matcap = loaded->Bitmap;
   Self->MatcapPath = Value;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_MatcapRef(extLightingFX *Self, const std::string_view &Value)
{
   Self->MatcapRef = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Metalness: Blends the `COMBINED` type from a dielectric surface (0) to a metal (1).

A dielectric surface, such as paint or plastic, scatters diffuse light in the colour of its albedo and reflects
specular light in the colour of the light.  A metal has no diffuse light and reflects in the colour of its albedo.
This field blends between the two: the diffuse light is multiplied by `1 - Metalness`, and the colour of the specular
light is blended from the light's colour towards the albedo's.  Ambient light is unchanged, as it stands in for the
reflection of the surroundings, which a metal colours with its albedo.

As a metal's highlights take the albedo's colour, they do not extend over transparent areas of the albedo.  If
#Fresnel is set, the reflectance at normal incidence blends from #Fresnel to the albedo's colour, and the highlights of
a metal still turn white at grazing angles.

The default is 0, and the value must be between 0 and 1.  The diffuse and specular types ignore this field.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Metalness(extLightingFX *Self, double &Value)
{
   Value = Self->Metalness;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Metalness(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0) or (Value > 1.0)) return ERR::OutOfRange;
   Self->Metalness = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NormalMap: An optional filter result supplying RGB surface directions.

Bind an earlier image-producing effect in the same filter.  Red, green and blue encode X, Y and Z.  By default Y is
in image axes, increasing downwards, as in DirectX normal maps.  Maps exported for OpenGL, including those of Blender
and many texture libraries, increase upwards; set #NormalConvention to `Y_UP` for them.  The channels are read as data
in either filter colour space.  The vector is centred on `rgb(128,128,128)`, its X and Y components are multiplied by
#NormalStrength, and it is normalised before shading, so `rgb(128,128,255)` faces the viewer.  Transparent pixels,
zero vectors and pixels outside the map use the height-derived normal.

The map replaces the slopes that #Scale would derive from the height.  As in SVG, a height that rises along X by
`g` alpha levels per pixel has a slope of `2 * Scale * g / 255`, because the Sobel kernel doubles the gradient, and so
a normal of `(-2 * Scale * g / 255, 0, 1)` before normalisation.  This assumes a 1:1 display scale and the default
kernel unit; refer to #UnitX.

Shadows, cavity occlusion and the position of the surface relative to point and spot lights still come from the
height, so #Scale must match the normal map for them to line up with its shading.  When a height map and a normal
map are supplied together, the matching Scale is the least-squares fit of the slopes encoded by the normals to the
gradients of the height.  With `gx` the central difference of the height in alpha levels per pixel and
`sx = -nx / nz` from the normal map, `Scale = 255 * sum(gx * sx) / (2 * sum(gx * gx))`, and likewise for Y.  If the
fits for X and Y disagree, the map does not match the height or is read in the wrong convention.  A fit that is
negative for Y alone indicates a `Y_UP` map read as `Y_DOWN`.

The primary @FilterEffect.Input supplies that height.  Set #Scale to zero to use a flat height field.  Removing the
producer clears this binding.  Binding a map enables enhanced lighting.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_NormalMap(extLightingFX *Self, objFilterEffect **Value)
{
   *Value = Self->NormalMap.Reference;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_NormalMap(extLightingFX *Self, objFilterEffect *Value)
{
   return set_named_filter_input(Self, Self->NormalMap.Name, Value ? VSF::REFERENCE : VSF::NONE, Value);
}

/*********************************************************************************************************************

-FIELD-
NormalConvention: The axis convention of the green channel of #NormalMap.

Normal maps disagree on the direction of Y.  `Y_DOWN`, the default, reads green as Y in image axes, increasing
downwards, as written by DirectX tools and by the `NORMAL` type of this class.  `Y_UP` reads green as Y increasing
upwards, as written by OpenGL tools such as Blender, and negates it.  `AUTO` is not valid for this class.

A map read in the wrong convention gives plausible but wrong lighting: slopes facing up and down the image swap their
shading, while slopes facing left and right are unaffected.  To check a map, light a raised feature from above.  Its
upper edge should be bright.  The field has no effect unless #NormalMap is set.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_NormalConvention(extLightingFX *Self, NMC &Value)
{
   Value = Self->NormalConvention;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_NormalConvention(extLightingFX *Self, NMC Value)
{
   if ((Value != NMC::Y_DOWN) and (Value != NMC::Y_UP)) return ERR::OutOfRange;
   Self->NormalConvention = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NormalStrength: Scales the relief of #NormalMap.

The X and Y components of each normal are multiplied by this value before the normal is normalised.  The default of 1
uses the map as supplied, 0 flattens it so that every mapped pixel faces the viewer, and values above 1 exaggerate it.
The value must be zero or greater.  The field has no effect unless #NormalMap is set.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_NormalStrength(extLightingFX *Self, double &Value)
{
   Value = Self->NormalStrength;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_NormalStrength(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->NormalStrength = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OcclusionChannel: Selects the channel of #OcclusionMap that supplies the occlusion.

The default is `RED`.  Packed maps that store occlusion, roughness and metalness in the red, green and blue channels
(ORM maps, as used by glTF) can be bound without extracting a channel, because their occlusion is in red.
`LUMINANCE` combines the stored red, green and blue values and suits greyscale maps that carry a slight tint.  With
`ALPHA`, transparent pixels are fully occluded.  Numerical maps have a single value and ignore this field.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_OcclusionChannel(extLightingFX *Self, MCH &Value)
{
   Value = Self->OcclusionChannel;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_OcclusionChannel(extLightingFX *Self, MCH Value)
{
   if ((Value < MCH::RED) or (Value > MCH::LUMINANCE)) return ERR::OutOfRange;
   Self->OcclusionChannel = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OcclusionMap: An optional filter result supplying ambient occlusion.

Bind an earlier effect in the same filter whose result is an ambient occlusion map, in which 1 (white) is fully
exposed and 0 (black) is fully occluded.  The map darkens indirect light only: the #Ambient light of the `DIFFUSE` and
`COMBINED` types, and the #Matcap reflection of the `COMBINED` type.  It multiplies the cavity term of the `CAVITY`
feature, so ambient light is `Ambient * cavity * occlusion`.  Light from the light sources is not occluded by the map,
because the `CAVITY` and `SHADOWS` features already account for the surface's own occlusion of direct light.  Without
ambient light or a matcap, the map has no effect.

The value is read from the channel selected by #OcclusionChannel, red by default, and adjusted by
#OcclusionStrength.  Channels are read as stored data in either filter colour space, without colour conversion.  A
producer with a numerical result, such as a TurbulenceFX in `HEIGHT` mode, supplies the value directly.  Values are
limited to the range 0 - 1.  Pixels outside of the producer's result, and fully transparent pixels unless the `ALPHA`
channel is selected, read as 1 and are not occluded.  The map is aligned with the scene and sampled at each output
pixel, as for #NormalMap.

A `DIFFUSE` effect with ambient light, a bound map and a positive #OcclusionStrength uses enhanced lighting
at any Compliance setting.  The `COMBINED` type always uses enhanced lighting.  The `SPECULAR` and `NORMAL` types
ignore the map.  Removing the producer clears this binding.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_OcclusionMap(extLightingFX *Self, objFilterEffect **Value)
{
   *Value = Self->OcclusionMap.Reference;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_OcclusionMap(extLightingFX *Self, objFilterEffect *Value)
{
   return set_named_filter_input(Self, Self->OcclusionMap.Name, Value ? VSF::REFERENCE : VSF::NONE, Value);
}

/*********************************************************************************************************************

-FIELD-
OcclusionStrength: Scales the effect of #OcclusionMap.

The occlusion `ao` read from the map is applied as `1 + OcclusionStrength * (ao - 1)`.  The default of 1 uses the map
as supplied, 0 disables it, and values in between reduce its effect.  Values above 1 deepen the occlusion, and the
result is limited to zero.  The value must be zero or greater.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_OcclusionStrength(extLightingFX *Self, double &Value)
{
   Value = Self->OcclusionStrength;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_OcclusionStrength(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::OutOfRange;
   Self->OcclusionStrength = Value;
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
RoughnessChannel: Selects the channel of #RoughnessMap that supplies the roughness.

The default is `RED`, which suits greyscale maps.  Packed ORM maps, as used by glTF, store roughness in the green
channel, so select `GREEN` to bind one without extracting a channel.  The channels are those of #OcclusionChannel.
With `ALPHA`, transparent pixels have zero roughness and the sharpest highlight.  Numerical maps have a single value
and ignore this field.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_RoughnessChannel(extLightingFX *Self, MCH &Value)
{
   Value = Self->RoughnessChannel;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_RoughnessChannel(extLightingFX *Self, MCH Value)
{
   if ((Value < MCH::RED) or (Value > MCH::LUMINANCE)) return ERR::OutOfRange;
   Self->RoughnessChannel = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RoughnessMap: An optional filter result supplying surface roughness for specular highlights.

Bind an earlier effect in the same filter whose result is a roughness map, in which 0 (black) is perfectly smooth and
1 (white) is fully rough.  Each pixel's roughness r replaces the constant #Exponent with a Blinn-Phong exponent, using
the usual approximation <code>n = 2 / r<sup>4</sup> - 2</code>, limited to the range of #Exponent: 1 to 128.  A
roughness of 0.4 gives an exponent of about 76, 0.64 gives about 10, and roughness above 0.9 gives 1.

#Exponent remains the reference exponent at which the specular constant is calibrated.  Each pixel's highlight is
scaled by <code>(n + 2) / (Exponent + 2)</code>, so smooth areas have narrow, bright highlights and rough areas broad,
dim ones.  A map whose exponent equals #Exponent everywhere reproduces the result without a map when both renders
use enhanced lighting.  With the `SPECULAR_AA` feature, surface variation reduces each pixel's exponent in both
cases.

The value is read from the channel selected by #RoughnessChannel, red by default.  Channels are read as stored data in
either filter colour space, without colour conversion.  A producer with a numerical result supplies the value
directly.  Values are limited to the range 0 - 1.  Pixels outside of the producer's result, and fully transparent
pixels unless the `ALPHA` channel is selected, use #Exponent.  The map is aligned with the scene and sampled at each
output pixel, as for #NormalMap.

The map applies to the `SPECULAR` and `COMBINED` types, and binding it selects enhanced lighting at any Compliance
setting.  In an `SVG1` or `SVG2` scene, this can change the `SPECULAR` result even when every pixel uses #Exponent.
It does not affect the matcap reflection.  The `DIFFUSE` and `NORMAL` types ignore it.  Removing the producer clears
this binding.

-END-

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_RoughnessMap(extLightingFX *Self, objFilterEffect **Value)
{
   *Value = Self->RoughnessMap.Reference;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_RoughnessMap(extLightingFX *Self, objFilterEffect *Value)
{
   return set_named_filter_input(Self, Self->RoughnessMap.Name, Value ? VSF::REFERENCE : VSF::NONE, Value);
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
Type: Selects diffuse lighting, specular lighting, combined shading or normal map output.
Lookup: LT

The `COMBINED` type reads an albedo from the #Mix input; refer to the class description.  It is the only type that
renders more than one light, so the type cannot be changed while the effect has more than one light.  Call
#SetDistantLight() or one of its siblings first to reduce the lights to one.

*********************************************************************************************************************/

static ERR LIGHTINGFX_SET_Type(extLightingFX *Self, LT Value)
{
   if ((Value != LT::COMBINED) and (Self->Lights.size() > 1)) {
      kt::Log log;
      log.warning("The type must be COMBINED while the effect has %d lights.", int(Self->Lights.size()));
      return ERR::InvalidState;
   }
   if (Self->Type != Value) {
      Self->Type = Value;
      if (Self->Filter) Self->Filter->GraphRevision++;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

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
Wrap: Extends the diffuse light of the `COMBINED` type past the terminator.

Wrap lighting softens the transition from light to shade, as on wax, paper and skin, where light scattered within the
material reaches areas that face slightly away from the light.  The diffuse term `N.L` is replaced with
`(N.L + Wrap) / (1 + Wrap)`, so that faces turned up to `asin(Wrap)` past the terminator receive some light.

Unlike the other lighting extensions, a non-zero value changes the brightness of flat surfaces: a flat surface under a
distant light at elevation E receives `(sin E + Wrap) / (1 + Wrap)` of the diffuse light instead of `sin E`.

Faces that turn away from a distant light lie in the surface's own shadow, which would cancel the wrapped light when
the `SHADOWS` feature is enabled.  For the diffuse light of those faces, shadows are instead cast by the light raised
by the wrap angle, `asin(Wrap)`, so that a face is not darkened by its own slope.  Surfaces that face the light keep
their shadows unchanged.  As a surface cannot tell its own shadow from one cast by a distant feature, a face that
turns away from the light is also lit inside a cast shadow, if the feature that casts it rises less than the wrap
angle above the light.  At a `Wrap` of 1, or wherever the raised light is within 1 degree of overhead, such faces
receive no shadow.  The extra shadow pass increases the cost of the effect.

The default is 0, and the value must be between 0 and 1.  The diffuse and specular types ignore this field.

*********************************************************************************************************************/

static ERR LIGHTINGFX_GET_Wrap(extLightingFX *Self, double &Value)
{
   Value = Self->Wrap;
   return ERR::Okay;
}

static ERR LIGHTINGFX_SET_Wrap(extLightingFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0) or (Value > 1.0)) return ERR::OutOfRange;
   Self->Wrap = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG element that describes the effect.

The result is a complete element.  The lighting types use `feDiffuseLighting`, `feSpecularLighting` or
`kotuku:feLighting` and include their light sources.  The `NORMAL` type uses `kotuku:feNormalMap` and includes
the primitive subregion and surface settings.  Settings that SVG cannot express are written as `kotuku:`
attributes when they differ from their defaults.  These include `kotuku:features` if #Features is not `ALL`, and
`kotuku:cavity`,
`kotuku:detail` and `kotuku:penumbra` for #Cavity, #Detail and #Penumbra, `kotuku:normalConvention` and
`kotuku:normalStrength` for #NormalConvention and #NormalStrength, `kotuku:occlusionChannel` and
`kotuku:occlusionStrength` for #OcclusionChannel and #OcclusionStrength, `kotuku:roughnessChannel` for
#RoughnessChannel, and `kotuku:ambient` for #Ambient.  The
`COMBINED` type also writes `metalness`, `fresnel` and `wrap` for #Metalness, #Fresnel and #Wrap when they are not
zero, and `matcap` for #MatcapPath when it is set.  An SVG-loaded matcap keeps its original relative reference so the
image can be reloaded from the same document folder.  For API-loaded images the supplied #MatcapPath is written.  A
matcap supplied as a #Matcap bitmap is omitted.  The attributes of `kotuku:feLighting` and `kotuku:feNormalMap` are
written without the prefix because the elements are in the Kōtuku namespace.  The document must declare the
prefix, for example `xmlns:kotuku="http://www.kotuku.dev/xmlns/svg"`.

Each light is written as a child element, in the order in which the lights were added.  The options of
#SetLightOptions() are written on the light's element as `kotuku:colour`, `kotuku:intensity`, `kotuku:range` and
`kotuku:shadows`, when they differ from their defaults.  The intensity of the first light is #Intensity.

The parent filter writes the `in`, `in2`, `result`, `kotuku:normalMap`, `kotuku:occlusionMap` and
`kotuku:roughnessMap` attributes separately.

*********************************************************************************************************************/

static void write_normal_options(std::ostream &Stream, const extLightingFX &Self, CSTRING Prefix)
{
   if (Self.NormalConvention IS NMC::Y_UP) Stream << ' ' << Prefix << "normalConvention=\"y-up\"";
   if (Self.NormalStrength != 1.0) Stream << ' ' << Prefix << "normalStrength=\"" << Self.NormalStrength << '"';
}

static void write_map_channel(std::ostream &Stream, CSTRING Prefix, CSTRING Name, MCH Channel)
{
   static constexpr CSTRING channels[] = { "R", "G", "B", "A", "luminance" };
   if ((Channel > MCH::RED) and (Channel <= MCH::LUMINANCE)) {
      Stream << ' ' << Prefix << Name << "=\"" << channels[int(Channel)] << '"';
   }
}

static void write_map_options(std::ostream &Stream, const extLightingFX &Self, CSTRING Prefix)
{
   write_map_channel(Stream, Prefix, "occlusionChannel", Self.OcclusionChannel);
   if (Self.OcclusionStrength != 1.0) {
      Stream << ' ' << Prefix << "occlusionStrength=\"" << Self.OcclusionStrength << '"';
   }
   write_map_channel(Stream, Prefix, "roughnessChannel", Self.RoughnessChannel);
}

static ERR LIGHTINGFX_GET_XMLDef(extLightingFX *Self, std::string &Value)
{
   std::stringstream stream;
   if (Self->Type IS LT::NORMAL) {
      stream.precision(std::numeric_limits<double>::max_digits10);
      stream << "<kotuku:feNormalMap surfaceScale=\"" << Self->MapHeight << '"';
      write_filter_xml_subregion(stream, *Self);
      if (Self->UnitXProvided and Self->UnitYProvided) {
         stream << " kernelUnitLength=\"" << Self->UnitX << ' ' << Self->UnitY << '"';
      }
      else if (Self->UnitXProvided) stream << " kernelUnitX=\"" << Self->UnitX << '"';
      else if (Self->UnitYProvided) stream << " kernelUnitY=\"" << Self->UnitY << '"';
      if (Self->Features != LFX::ALL) {
         stream << " features=\"";
         write_lfx_features(stream, Self->Features);
         stream << '"';
      }
      if (Self->Detail != DEFAULT_DETAIL) stream << " detail=\"" << Self->Detail << '"';
      write_normal_options(stream, *Self, "");
      stream << "/>";
      Value = stream.str();
      return ERR::Okay;
   }

   const bool combined = Self->Type IS LT::COMBINED;
   CSTRING element = combined ? "kotuku:feLighting" : (Self->Type IS LT::DIFFUSE) ? "feDiffuseLighting" : "feSpecularLighting";
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
   write_normal_options(stream, *Self, prefix);
   write_map_options(stream, *Self, prefix);

   const auto &a = Self->Ambient;
   if ((a.Red != 0.0f) or (a.Green != 0.0f) or (a.Blue != 0.0f) or (a.Alpha != 0.0f)) {
      stream << ' ' << prefix << "ambient=\"";
      write_rgb(stream, a);
      stream << '"';
   }

   if (combined) {
      if (Self->Metalness != 0) stream << " metalness=\"" << Self->Metalness << '"';
      if (Self->Fresnel != 0) stream << " fresnel=\"" << Self->Fresnel << '"';
      if (Self->Wrap != 0) stream << " wrap=\"" << Self->Wrap << '"';
      const auto &reference = Self->MatcapRef.empty() ? Self->MatcapPath : Self->MatcapRef;
      if (not reference.empty()) write_filter_xml_attribute(stream, "matcap", reference);
   }

   stream << '>';

   for (const auto &light : Self->Lights) {
      switch (light.Kind) {
         case LS::POINT:
            stream << "<fePointLight x=\"" << light.Position.x << "\" y=\"" << light.Position.y << "\" z=\""
               << light.Position.z << '"';
            break;

         case LS::SPOT:
            stream << "<feSpotLight x=\"" << light.Position.x << "\" y=\"" << light.Position.y << "\" z=\""
               << light.Position.z << '"';
            stream << " pointsAtX=\"" << light.Target.x << "\" pointsAtY=\"" << light.Target.y;
            stream << "\" pointsAtZ=\"" << light.Target.z << "\" specularExponent=\"" << light.SpotExponent << '"';
            if (light.ConeAngle) stream << " limitingConeAngle=\"" << light.ConeAngle << '"';
            break;

         default:
            stream << "<feDistantLight azimuth=\"" << light.Azimuth << "\" elevation=\"" << light.Elevation << '"';
            break;
      }

      if (light.HasColour) {
         stream << " kotuku:colour=\"";
         write_rgb(stream, light.Colour);
         stream << '"';
      }
      if (light.Intensity != 1.0) write_light_number(stream, "intensity", light.Intensity);
      if (light.Range > 0) write_light_number(stream, "range", light.Range);
      if (not light.Shadows) stream << " kotuku:shadows=\"false\"";
      stream << "/>";
   }

   stream << "</" << element << '>';
   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_lighting_enhanced.cpp"
#include "filter_lighting_def.c"

static const FieldArray clLightingFXFields[] = {
   { "Colour",    FDF_STRUCT|FDF_RW, nullptr, LIGHTINGFX_SET_Colour, "FRGB" },
   { "Constant",  FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_Constant },
   { "Exponent",  FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_Exponent },
   { "Features",  FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, LIGHTINGFX_GET_Features, LIGHTINGFX_SET_Features, &clLightingFXLFX },
   { "Cavity",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Cavity, LIGHTINGFX_SET_Cavity },
   { "Detail",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Detail, LIGHTINGFX_SET_Detail },
   { "Penumbra",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Penumbra, LIGHTINGFX_SET_Penumbra },
   { "Ambient",   FDF_VIRTUAL|FDF_STRUCT|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Ambient, LIGHTINGFX_SET_Ambient, "FRGB" },
   { "Intensity", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Intensity, LIGHTINGFX_SET_Intensity },
   { "SpecularConstant", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_SpecularConstant, LIGHTINGFX_SET_SpecularConstant },
   { "Metalness",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Metalness, LIGHTINGFX_SET_Metalness },
   { "Fresnel",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Fresnel, LIGHTINGFX_SET_Fresnel },
   { "Wrap",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Wrap, LIGHTINGFX_SET_Wrap },
   { "Matcap",     FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, LIGHTINGFX_GET_Matcap, LIGHTINGFX_SET_Matcap, CLASSID::BITMAP },
   { "MatcapPath", FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW|FDF_PURE, LIGHTINGFX_GET_MatcapPath, LIGHTINGFX_SET_MatcapPath },
   { "MatcapRef",  FDF_SYSTEM|FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, LIGHTINGFX_SET_MatcapRef },
   { "NormalMap",  FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, LIGHTINGFX_GET_NormalMap, LIGHTINGFX_SET_NormalMap, CLASSID::FILTEREFFECT },
   { "NormalConvention", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, LIGHTINGFX_GET_NormalConvention, LIGHTINGFX_SET_NormalConvention, &clLightingFXNMC },
   { "NormalStrength", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_NormalStrength, LIGHTINGFX_SET_NormalStrength },
   { "OcclusionChannel", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, LIGHTINGFX_GET_OcclusionChannel, LIGHTINGFX_SET_OcclusionChannel, &clLightingFXMCH },
   { "OcclusionMap", FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, LIGHTINGFX_GET_OcclusionMap, LIGHTINGFX_SET_OcclusionMap, CLASSID::FILTEREFFECT },
   { "OcclusionStrength", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, LIGHTINGFX_GET_OcclusionStrength, LIGHTINGFX_SET_OcclusionStrength },
   { "RoughnessChannel", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, LIGHTINGFX_GET_RoughnessChannel, LIGHTINGFX_SET_RoughnessChannel, &clLightingFXMCH },
   { "RoughnessMap", FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, LIGHTINGFX_GET_RoughnessMap, LIGHTINGFX_SET_RoughnessMap, CLASSID::FILTEREFFECT },
   { "Scale",      FDF_DOUBLE|FDF_RW },
   { "UnitX",      FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_UnitX },
   { "UnitY",      FDF_DOUBLE|FDF_RW, nullptr, LIGHTINGFX_SET_UnitY },
   { "Type",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, LIGHTINGFX_SET_Type, &clLightingFXLT },
   { "XMLDef",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, LIGHTINGFX_GET_XMLDef },
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
