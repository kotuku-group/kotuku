/*********************************************************************************************************************

-CLASS-
TurbulenceFX: A filter effect that utilises the Perlin turbulence function.

This filter effect creates an image using the Perlin turbulence function. It allows the synthesis of artificial
textures like clouds or marble.  For a detailed description the of the Perlin turbulence function, see "Texturing
and Modeling", Ebert et al, AP Professional, 1994. The resulting image will fill the entire filter primitive
subregion for this filter primitive.

It is possible to create bandwidth-limited noise by synthesizing only one octave.

The effect implements the SVG `feTurbulence` primitive.  Kōtuku extends it with control over the octave stack
(#Persistence, #Lacunarity and #Normalise), the mapping of the result (#Amplitude and #Offset), the noise domain
(#OriginX, #OriginY and #Angle), seamless repetition (#PeriodX and #PeriodY), animation (#Phase and #PhasePeriod),
colour ramps (#ColourMap and #Stops) and two further #Type functions, `BILLOW` and `RIDGED`.  Each extension field
defaults to the value that reproduces SVG output exactly.  The #Compliance field declares whether the extensions are
permitted.

The following order is used for applying the pseudo random numbers.  An initial seed value is computed based on
#Seed.  Then the implementation computes the lattice points for R, then continues getting additional pseudo random
numbers relative to the last generated pseudo random number and computes the lattice points for G, and so on for B
and A.

-END-

TODO: Support ParameterMaps (a filter input that varies a field per sample) for Persistence, Amplitude, Offset and
Phase, once a binding facility shared by the filter classes has been designed (CellularFX has the same TODO).  Draw
fixes these values once per draw: noise_sum() takes Persistence as its octave ratio, the mapping holds Amplitude,
Offset and the Normalise total derived from Persistence, and Phase is reduced by PhasePeriod and range-checked before
a zero phase is dispatched to noise2().  They must become per-sample arguments.  A Phase map must force the
animated noise3() path, which accepts a per-pixel phase at no extra cost, and the OutOfRange check must use the
largest phase that the map can supply in the drawn area.  A mapped field counts as an extension under Compliance,
and the mapping's Extended flag must be set whenever Amplitude or Offset is mapped.

*********************************************************************************************************************/

#include "filter_colour_ramp.h"

constexpr int RAND_m = 2147483647; // 2**31 - 1
constexpr int RAND_a = 16807;      // 7**5; primitive root of m
constexpr int RAND_q = 127773;     // m / a
constexpr int RAND_r = 2836;       // m % a

constexpr int BSIZE    = 0x100;
constexpr int BM       = 0xff;
constexpr int PerlinN  = 0x1000;
constexpr int NP       = 12; // 2^PerlinN
constexpr int NM       = 0xfff;
constexpr int GSIZE    = 4;
constexpr int GSUBSIZE = 2;
constexpr int LSIZE    = (BSIZE + BSIZE + 2);

constexpr double s_curve(double t) { return (t * t * (3.0 - 2.0 * t)); }

constexpr int TURBULENCE_MAX_PERIOD = 1<<20;

// Wraps a lattice index into [0, Period).  Unlike the stitch wrap, any distance from the origin is supported.

constexpr int floor_mod(int Index, int Period)
{
   const int result = Index % Period;
   return (result < 0) ? result + Period : result;
}

// Scales SVG gradient noise, which spans about +/- sqrt(2)/2, to about +/- 1 for the BILLOW and RIDGED terms.

constexpr double NOISE_SCALE = 1.41421356237309504880;

// Maps an octave sum to its output value when any of the mapping extensions are in use.  The default mapping is
// computed by the original expressions instead, which round differently.

struct turbulence_mapping {
   bool   Extended  = false;
   bool   Centred   = false; // NOISE and BILLOW sums are centred on zero
   double Total     = 0;     // Octave weight total if normalising, else zero
   double Amplitude = 1.0;
   double Offset    = 0.0;

   double value(double Sum) const {
      if (Total > 0) Sum /= Total;
      return Amplitude * (Centred ? (Sum + 1.0) * 0.5 : Sum) + Offset;
   }
};

class extTurbulenceFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TURBULENCEFX;
   static constexpr CSTRING CLASS_NAME = "TurbulenceFX";
   using create = kt::Create<extTurbulenceFX>;

   double FX = 0, FY = 0;
   int    Octaves = 1;
   int    Seed = 0;
   int    Stitch = false;
   TB     Type = TB::TURBULENCE;
   FOM    OutputMode = FOM::IMAGE;
   FXC    Compliance = FXC::INHERIT;
   double Persistence = 0.5;
   double Lacunarity = 2.0;
   double Amplitude = 1.0;
   double Offset = 0.0;
   Unit   OriginX = Unit(0.0);
   Unit   OriginY = Unit(0.0);
   double Angle = 0;
   std::string ColourMap;
   kt::vector<GradientStop> Stops;
   int    Normalise = false;
   int    PeriodX = 0;
   int    PeriodY = 0;
   int    PhasePeriod = 0;
   double Phase = 0;
   std::unique_ptr<GradientColours> Colours;
   objBitmap *Bitmap = nullptr;
   double Gradient[GSIZE][LSIZE][GSUBSIZE];
   int    Lattice[LSIZE];
   bool   Dirty = true;
   agg::trans_affine CacheTransform;
   agg::trans_affine CacheDomain;
   TClipRectangle<double> CacheRegion = {};
   VCS CacheColourSpace = VCS::INHERIT;

   // Phase animation samples a third lattice axis.  Layer k hashes to the 2-D gradients shifted by ZOffset[k], with
   // an additional z component from GradientZ.  ZOffset[0] is zero, so the layer at z = 0 is the 2-D noise.
   int    ZOffset[BSIZE];
   double GradientZ[GSIZE][BSIZE];

   // The lattice cell that contains a sample, after stitching and periodic wrapping.

   struct noise_cell {
      int b00, b10, b01, b11;
      double rx0, rx1, ry0, ry1, sx, sy;
   };

   // Periodic sampling is a separate instantiation so that the SVG and Stitch paths are unchanged.  A period of zero
   // leaves its axis unwrapped.

   template <bool Periodic = false>
   noise_cell lattice_cell(double VX, double VY, bool StitchNoise, int StitchWidth, int StitchHeight, int WrapX,
      int WrapY, int PeriodX, int PeriodY) const {
      int bx0, bx1, by0, by1;
      double rx0, rx1, ry0, ry1, t;

      t = VX + PerlinN;
      bx0 = int(std::floor(t));
      bx1 = bx0 + 1;
      rx0 = t - bx0;
      rx1 = rx0 - 1.0;

      t = VY + PerlinN;
      by0 = int(std::floor(t));
      by1 = by0 + 1;
      ry0 = t - by0;
      ry1 = ry0 - 1.0;

      // If stitching, adjust lattice points accordingly.

      if (StitchNoise) {
         if (bx0 >= WrapX) bx0 -= StitchWidth;
         if (bx1 >= WrapX) bx1 -= StitchWidth;
         if (by0 >= WrapY) by0 -= StitchHeight;
         if (by1 >= WrapY) by1 -= StitchHeight;
      }

      if constexpr (Periodic) {
         // PerlinN is a multiple of BSIZE, so removing it does not change the masked index.
         if (PeriodX > 0) {
            bx0 = floor_mod(bx0 - PerlinN, PeriodX);
            bx1 = floor_mod(bx1 - PerlinN, PeriodX);
         }
         if (PeriodY > 0) {
            by0 = floor_mod(by0 - PerlinN, PeriodY);
            by1 = floor_mod(by1 - PerlinN, PeriodY);
         }
      }

      bx0 &= BM;
      bx1 &= BM;
      by0 &= BM;
      by1 &= BM;
      const int i = Lattice[bx0];
      const int j = Lattice[bx1];
      return { Lattice[i + by0], Lattice[j + by0], Lattice[i + by1], Lattice[j + by1],
         rx0, rx1, ry0, ry1, s_curve(rx0), s_curve(ry0) };
   }

   template <bool Periodic = false>
   double noise2(uint8_t Channel, double VX, double VY, bool StitchNoise = false, int StitchWidth = 0,
      int StitchHeight = 0, int WrapX = 0, int WrapY = 0, int PeriodX = 0, int PeriodY = 0) const {
      const auto c = lattice_cell<Periodic>(VX, VY, StitchNoise, StitchWidth, StitchHeight, WrapX, WrapY,
         PeriodX, PeriodY);
      const double *q;
      double u, v;
      q = Gradient[Channel][c.b00]; u = c.rx0 * q[0] + c.ry0 * q[1];
      q = Gradient[Channel][c.b10]; v = c.rx1 * q[0] + c.ry0 * q[1];
      const double a = std::lerp(u, v, c.sx);
      q = Gradient[Channel][c.b01]; u = c.rx0 * q[0] + c.ry1 * q[1];
      q = Gradient[Channel][c.b11]; v = c.rx1 * q[0] + c.ry1 * q[1];
      const double b = std::lerp(u, v, c.sx);
      return std::lerp(a, b, c.sy);
   }

   // 3-D gradient noise for Phase animation.  A ZPeriod of zero leaves the z axis unwrapped.  At an integer VZ whose
   // layer wraps to zero, the result equals noise2() exactly.

   template <bool Periodic = false>
   double noise3(uint8_t Channel, double VX, double VY, double VZ, int ZPeriod, bool StitchNoise, int StitchWidth,
      int StitchHeight, int WrapX, int WrapY, int PeriodX, int PeriodY) const {
      const auto c = lattice_cell<Periodic>(VX, VY, StitchNoise, StitchWidth, StitchHeight, WrapX, WrapY,
         PeriodX, PeriodY);

      const double t = VZ + PerlinN;
      int bz0 = int(std::floor(t));
      int bz1 = bz0 + 1;
      const double rz0 = t - bz0;
      const double rz1 = rz0 - 1.0;
      if (ZPeriod > 0) {
         bz0 = floor_mod(bz0 - PerlinN, ZPeriod);
         bz1 = floor_mod(bz1 - PerlinN, ZPeriod);
      }

      auto layer = [&](int Layer, double RZ) {
         const int offset = ZOffset[Layer & BM];
         auto corner = [&](int Index, double RX, double RY) {
            const int g = (Index + offset) & BM;
            return RX * Gradient[Channel][g][0] + RY * Gradient[Channel][g][1] + RZ * GradientZ[Channel][g];
         };
         const double a = std::lerp(corner(c.b00, c.rx0, c.ry0), corner(c.b10, c.rx1, c.ry0), c.sx);
         const double b = std::lerp(corner(c.b01, c.rx0, c.ry1), corner(c.b11, c.rx1, c.ry1), c.sx);
         return std::lerp(a, b, c.sy);
      };

      return std::lerp(layer(bz0, rz0), layer(bz1, rz1), s_curve(rz0));
   }

   // Animated sums sample noise3() at Z, which is scaled by the lacunarity per octave like X and Y.

   template <bool Periodic = false, bool Animated = false>
   double noise_sum(uint8_t Channel, double X, double Y, double FrequencyX, double FrequencyY,
      int StitchWidth, int StitchHeight, int WrapX, int WrapY, int PeriodX = 0, int PeriodY = 0, double Z = 0,
      int ZPeriod = 0) const {
      double sum = 0.0;
      double vx = X * FrequencyX;
      double vy = Y * FrequencyY;
      double vz = Z;
      double ratio = 1.0;
      double weight = 1.0; // RIDGED weights each octave by the one before it
      // Draw rejects a stitched or periodic stack of several octaves with a fractional lacunarity.
      const int stitch_lacunarity = int(Lacunarity);
      for (int n=0; n < Octaves; n++) {
         double noise;
         if constexpr (Animated) {
            if constexpr (Periodic) {
               noise = noise3<true>(Channel, vx, vy, vz, ZPeriod, false, 0, 0, 0, 0, PeriodX, PeriodY);
            }
            else noise = noise3(Channel, vx, vy, vz, ZPeriod, Stitch, StitchWidth, StitchHeight, WrapX, WrapY, 0, 0);
         }
         else if constexpr (Periodic) noise = noise2<true>(Channel, vx, vy, false, 0, 0, 0, 0, PeriodX, PeriodY);
         else noise = noise2(Channel, vx, vy, Stitch, StitchWidth, StitchHeight, WrapX, WrapY);
         switch (Type) {
            case TB::NOISE:  sum += noise * ratio; break;
            case TB::BILLOW: sum += (2.0 * NOISE_SCALE * fabs(noise) - 1.0) * ratio; break;
            case TB::RIDGED: {
               const double ridge = 1.0 - NOISE_SCALE * fabs(noise);
               const double term = ridge * ridge * weight;
               sum += term * ratio;
               weight = std::clamp(2.0 * term, 0.0, 1.0);
               break;
            }
            default: sum += fabs(noise) * ratio;
         }
         vx *= Lacunarity;
         vy *= Lacunarity;
         ratio *= Persistence;
         if (Stitch) {
            // Equivalent to the SVG update '2 * Wrap - PerlinN' when the lacunarity is 2.
            StitchWidth *= stitch_lacunarity;
            StitchHeight *= stitch_lacunarity;
            WrapX = stitch_lacunarity * (WrapX - PerlinN) + PerlinN;
            WrapY = stitch_lacunarity * (WrapY - PerlinN) + PerlinN;
         }
         if constexpr (Periodic) {
            // Draw confirms that the period of the final octave is representable.
            if (n + 1 < Octaves) {
               PeriodX *= stitch_lacunarity;
               PeriodY *= stitch_lacunarity;
            }
         }
         if constexpr (Animated) {
            vz *= Lacunarity;
            // Draw rejects a phase period with several octaves and a fractional lacunarity.
            if (n + 1 < Octaves) ZPeriod *= stitch_lacunarity;
         }
      }

      return sum;
   }

   // Maps the alpha-lattice sum to the scalar value drawn by HEIGHT output and colour ramps, before clamping.

   double scalar_value(double Sum, const turbulence_mapping &Mapping) const {
      if (Mapping.Extended) return Mapping.value(Sum);
      if (Type IS TB::NOISE) return (Sum + 1.0) * 0.5;
      return Sum;
   }

   uint8_t image_channel(double Sum, const turbulence_mapping &Mapping) const {
      if (Mapping.Extended) return uint8_t(std::clamp(Mapping.value(Sum), 0.0, 1.0) * 255.0);
      int channel;
      if (Type IS TB::NOISE) channel = ((Sum * 255.0) + 255.0) * 0.5;
      else channel = Sum * 255.0;
      return std::clamp(channel, 0, 255);
   }

   ~extTurbulenceFX() {
      if (Bitmap) FreeResource(Bitmap);
   }

   extTurbulenceFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
   }
};

//********************************************************************************************************************

inline int setup_seed(int lSeed)
{
  if (lSeed <= 0) lSeed = -(lSeed % (RAND_m - 1)) + 1;
  if (lSeed > RAND_m - 1) lSeed = RAND_m - 1;
  return lSeed;
}

inline int random(int lSeed)
{
  int result = RAND_a * (lSeed % RAND_q) - RAND_r * (lSeed / RAND_q);
  if (result <= 0) result += RAND_m;
  return result;
}

//********************************************************************************************************************
// Returns the name of the first extension field that is not at its default value, or nullptr if the effect uses no
// Kotuku extensions.

static CSTRING turbulence_extension(const extTurbulenceFX *Self)
{
   if ((Self->Type IS TB::BILLOW) or (Self->Type IS TB::RIDGED)) return "Type";
   if (Self->Persistence != 0.5) return "Persistence";
   if (Self->Lacunarity != 2.0) return "Lacunarity";
   if (Self->Normalise) return "Normalise";
   if (Self->Amplitude != 1.0) return "Amplitude";
   if (Self->Offset != 0.0) return "Offset";
   if (Self->OriginX.Value != 0) return "OriginX";
   if (Self->OriginY.Value != 0) return "OriginY";
   if (Self->Angle != 0) return "Angle";
   if (Self->PeriodX) return "PeriodX";
   if (Self->PeriodY) return "PeriodY";
   if (Self->Phase != 0) return "Phase";
   if (Self->PhasePeriod) return "PhasePeriod";
   if (not Self->ColourMap.empty()) return "ColourMap";
   if (not Self->Stops.empty()) return "Stops";
   return nullptr;
}

static constexpr bool svg_compliance(FXC Value) { return (Value IS FXC::SVG1) or (Value IS FXC::SVG2); }

//********************************************************************************************************************
// Resolves INHERIT through the scene that owns the filter.

static VSC turbulence_compliance(const extTurbulenceFX *Self)
{
   switch (Self->Compliance) {
      case FXC::NONE: return VSC::NONE;
      case FXC::SVG1: return VSC::SVG1;
      case FXC::SVG2: return VSC::SVG2;
      default:
         if ((Self->Filter) and (Self->Filter->Scene)) return Self->Filter->Scene->Compliance;
         return VSC::NONE;
   }
}

//********************************************************************************************************************
// Checks the combinations of fields that individual setters cannot, because fields may be set in any order or the
// compliance is inherited from the scene.

static ERR validate_turbulence(const extTurbulenceFX *Self)
{
   kt::Log log("TurbulenceFX");

   if (turbulence_compliance(Self) != VSC::NONE) {
      if (auto field = turbulence_extension(Self)) {
         log.warning("#%d: The %s extension is not permitted under SVG compliance.", Self->UID, field);
         return ERR::InvalidValue;
      }
   }

   const bool periodic = (Self->PeriodX > 0) or (Self->PeriodY > 0);
   if (Self->Stitch and periodic) {
      log.warning("#%d: Stitch cannot be combined with PeriodX or PeriodY.", Self->UID);
      return ERR::InvalidValue;
   }

   if ((Self->Stitch or periodic) and (std::fmod(Self->Angle, 360.0) != 0)) {
      log.warning("#%d: A stitched or periodic noise domain cannot be rotated.", Self->UID);
      return ERR::InvalidValue;
   }

   if ((Self->Stitch or periodic) and (Self->Octaves > 1) and (Self->Lacunarity != std::trunc(Self->Lacunarity))) {
      log.warning("#%d: Stitching or repeating several octaves requires an integer Lacunarity.", Self->UID);
      return ERR::InvalidValue;
   }

   if ((Self->PhasePeriod > 0) and (Self->Octaves > 1) and (Self->Lacunarity != std::trunc(Self->Lacunarity))) {
      log.warning("#%d: A PhasePeriod with several octaves requires an integer Lacunarity.", Self->UID);
      return ERR::InvalidValue;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.

Draw fails with `ERR::InvalidValue` if an extension field is in use while the resolved #Compliance is `SVG1` or
`SVG2`, if #Stitch is combined with #PeriodX or #PeriodY, if #Stitch or a period is combined with an #Angle that is
not a multiple of 360, or if #Stitch, a period or #PhasePeriod is combined with several octaves and a fractional
#Lacunarity.  A warning is logged in each case.  It fails with `ERR::OutOfRange` if the drawn area or #Phase lies so
far from the origin, at the finest octave, that lattice coordinates can no longer be represented, or if a period of
the finest octave cannot be represented.
-END-
*********************************************************************************************************************/

static ERR TURBULENCEFX_Draw(extTurbulenceFX *Self, struct acDraw *Args)
{
   if (auto error = validate_turbulence(Self); error != ERR::Okay) return error;

   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const bool scalar = step->Format IS filter_surface_format::SCALAR_F32;
   filter_surface_view layout;
   float *heights = nullptr;
   if (scalar) {
      if (auto error = get_numeric_target(Self, layout, heights); error != ERR::Okay) return error;
   }
   else if ((not Self->Target) or (Self->Target->BytesPerPixel != 4)) return ERR::InvalidState;

   auto filter = Self->Filter;
   const auto &render = *filter->Render;
   const auto clip = render.scratch_clip();
   const int width = clip.right;
   const int height = clip.bottom;
   if ((width <= 0) or (height <= 0)) return ERR::Okay;

   // Express the primitive subregion and the inverse sampling transform in primitiveUnits.  Each output pixel
   // samples the noise at its centre in that coordinate system, independently of the filter bitmap's origin.

   double origin_x = 0, origin_y = 0, unit_width = 1, unit_height = 1;
   if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      const auto &bounds = render.ClientBounds;
      origin_x = bounds.left;
      origin_y = bounds.top;
      unit_width = bounds.width();
      unit_height = bounds.height();
      if ((unit_width <= 0) or (unit_height <= 0)) return ERR::Okay;
   }

   const double region_x = (step->Subregion.left - origin_x) / unit_width;
   const double region_y = (step->Subregion.top - origin_y) / unit_height;
   const double region_width = step->Subregion.width() / unit_width;
   const double region_height = step->Subregion.height() / unit_height;
   const TClipRectangle<double> region = { region_x, region_y, region_x + region_width, region_y + region_height };

   agg::trans_affine sampling;
   sampling.scale(unit_width, unit_height);
   sampling.translate(origin_x, origin_y);
   sampling *= render.Transform;
   TClipRectangle<double> samples = { filter_pixel_centre(0), filter_pixel_centre(0),
      filter_pixel_centre(width - 1), filter_pixel_centre(height - 1) };
   if ((not finite_region(region)) or (not invert_filter_transform(sampling)) or
       (not transform_filter_region(samples, sampling))) return ERR::InvalidValue;

   // Only pixel centres inside both the processing bitmap and the primitive can reach the lattice.

   if ((not region.valid()) or (samples.right < region.left) or (samples.bottom < region.top) or
       (samples.left >= region.right) or (samples.top >= region.bottom)) return ERR::Okay;
   samples.shrinking(region);

   // The noise domain is anchored at primitive-unit (0, 0), as for SVG.  Percentage origins refer to the size of the
   // filter region.  The transform is applied after the coverage test and is skipped when it is the identity, so
   // that the SVG path samples exactly as before.

   auto resolve = [](const Unit &Length, double Reference) {
      return Length.scaled() ? Length.Value * Reference : Length.Value;
   };
   const double domain_x = resolve(Self->OriginX, render.FilterRegion.width() / unit_width);
   const double domain_y = resolve(Self->OriginY, render.FilterRegion.height() / unit_height);
   const double angle = std::fmod(Self->Angle, 360.0);
   if ((not std::isfinite(domain_x)) or (not std::isfinite(domain_y))) return ERR::InvalidValue;

   const bool has_domain = (domain_x != 0) or (domain_y != 0) or (angle != 0);
   agg::trans_affine domain;
   if (has_domain) {
      domain.translate(-domain_x, -domain_y);
      if (angle != 0) domain.rotate(-angle * agg::pi / 180.0);
   }

   if (not scalar) {
      if (not Self->Bitmap) {
         Self->Dirty = true;
         if (not (Self->Bitmap = objBitmap::create::local(fl::Name("turbulence_bmp"),
            fl::Width(width),
            fl::Height(height),
            fl::BitsPerPixel(32),
            fl::Flags(BMF::ALPHA_CHANNEL),
            fl::BlendMode(BLM::NONE),
            fl::ColourSpace(CS::SRGB)))) return ERR::CreateObject;
      }
      else if ((Self->Bitmap->Width != width) or (Self->Bitmap->Height != height)) {
         Self->Dirty = true;
         if (auto error = Self->Bitmap->resize(width, height); error != ERR::Okay) return error;
      }

      if ((not Self->CacheTransform.is_equal(sampling, 0.0)) or (not Self->CacheDomain.is_equal(domain, 0.0)) or
          (Self->CacheColourSpace != effect_colour_space(Self)) or
          (Self->CacheRegion.left != region.left) or (Self->CacheRegion.top != region.top) or
          (Self->CacheRegion.right != region.right) or (Self->CacheRegion.bottom != region.bottom)) Self->Dirty = true;
   }

   if (scalar or Self->Dirty) {
      const bool linear_rgb = effect_colour_space(Self) IS VCS::LINEAR_RGB;
      double fx = Self->FX, fy = Self->FY;
      int stitch_width = 0, stitch_height = 0, wrap_x = PerlinN, wrap_y = PerlinN;
      if (Self->Stitch) {
         // The tile origin is the primitive subregion's top-left and its period is its width and height,
         // both in primitive units.  Neither depends on the processing bitmap's clip or origin.
         auto stitch_frequency = [](double Frequency, double Size) {
            if (Frequency IS 0) return Frequency;
            const double low = std::floor(Size * Frequency) / Size;
            const double high = std::ceil(Size * Frequency) / Size;
            return ((low > 0) and (Frequency / low < high / Frequency)) ? low : high;
         };
         fx = stitch_frequency(fx, region_width);
         fy = stitch_frequency(fy, region_height);
      }

      const int octaves = std::max(Self->Octaves, 0);
      const double lacunarity = Self->Lacunarity;

      // Leave room for PerlinN, the adjacent lattice point and the stitched subtraction.

      constexpr double limit = std::numeric_limits<int>::max() / 4.0;
      const double finest = std::pow(lacunarity, std::max(octaves - 1, 0));
      const auto lattice_fits = [finest](double Extent, double Frequency) {
         return Extent * std::abs(Frequency) * finest < limit;
      };

      // Measure the sampled area in domain coordinates.  Without a domain transform the corners give the same
      // extents as the sample bounds.

      double extent_x = 0, extent_y = 0;
      for (auto [x, y] : { std::pair(samples.left, samples.top), std::pair(samples.right, samples.top),
            std::pair(samples.left, samples.bottom), std::pair(samples.right, samples.bottom) }) {
         if (has_domain) domain.transform(&x, &y);
         extent_x = std::max(extent_x, std::abs(x));
         extent_y = std::max(extent_y, std::abs(y));
      }
      if ((not lattice_fits(extent_x, fx)) or (not lattice_fits(extent_y, fy))) return ERR::OutOfRange;

      const bool periodic = (Self->PeriodX > 0) or (Self->PeriodY > 0);
      if ((double(Self->PeriodX) * finest >= limit) or (double(Self->PeriodY) * finest >= limit)) {
         return ERR::OutOfRange;
      }

      // A phase period loops the z lattice exactly, so the phase can be reduced to a single loop.  The z = 0 layer is
      // the 2-D noise, so a phase of zero takes the 2-D path.

      const int phase_period = Self->PhasePeriod;
      const double phase = phase_period ? std::fmod(Self->Phase, double(phase_period)) : Self->Phase;
      const bool animated = phase != 0;
      if ((not (std::abs(phase) * finest < limit)) or (double(phase_period) * finest >= limit)) return ERR::OutOfRange;

      if (Self->Stitch) {
         // The counters use the full authored tile and are scaled by the lacunarity even after the final sample.

         const double growth = std::pow(lacunarity, octaves);
         const auto prepare_axis = [growth](double Origin, double Size, double Frequency, int &Period, int &Wrap) {
            const double period = std::trunc(Size * Frequency + 0.5);
            const double wrap = std::trunc(Origin * Frequency + PerlinN + period);
            if ((not (std::abs(period) * growth < limit)) or
                (not (std::abs(wrap - PerlinN) * growth < limit))) return false;
            Period = int(period);
            Wrap = int(wrap);
            return true;
         };

         // Stitching requires an unrotated domain, so the tile origin is the subregion relative to the origin.

         if ((not prepare_axis(region_x - domain_x, region_width, fx, stitch_width, wrap_x)) or
             (not prepare_axis(region_y - domain_y, region_height, fy, stitch_height, wrap_y))) return ERR::OutOfRange;
      }

      turbulence_mapping mapping;
      mapping.Extended = (Self->Type IS TB::BILLOW) or (Self->Type IS TB::RIDGED) or (Self->Normalise) or
         (Self->Amplitude != 1.0) or (Self->Offset != 0.0);
      mapping.Centred = (Self->Type IS TB::NOISE) or (Self->Type IS TB::BILLOW);
      mapping.Amplitude = Self->Amplitude;
      mapping.Offset = Self->Offset;
      if (Self->Normalise) {
         double weight = 1.0;
         for (int n=0; n < octaves; n++) {
            mapping.Total += weight;
            weight *= Self->Persistence;
         }
      }

      int thread_count = std::thread::hardware_concurrency();
      if (thread_count > height/4) thread_count = height/4;
      if (thread_count < 1) thread_count = 1;
      BS::thread_pool pool(thread_count);

      for (int i=0; i < thread_count; i++) {
         pool.detach_task([=, start = (height * i) / thread_count, end = (height * (i + 1)) / thread_count]() {
            auto render = [&](auto WritePixel) {
               for (int y=start; y < end; ++y) {
                  for (int x=0; x < width; ++x) {
                     double px, py;
                     map_filter_pixel_centre(sampling, x, y, px, py);
                     const bool covered = (px >= region.left) and (px < region.right) and
                        (py >= region.top) and (py < region.bottom);

                     if (has_domain) domain.transform(&px, &py);

                     auto sum = [&](uint8_t Channel) {
                        if (animated) {
                           if (periodic) {
                              return Self->noise_sum<true, true>(Channel, px, py, fx, fy, 0, 0, 0, 0,
                                 Self->PeriodX, Self->PeriodY, phase, phase_period);
                           }

                           return Self->noise_sum<false, true>(Channel, px, py, fx, fy, stitch_width, stitch_height,
                              wrap_x, wrap_y, 0, 0, phase, phase_period);
                        }

                        if (periodic) {
                           return Self->noise_sum<true>(Channel, px, py, fx, fy, 0, 0, 0, 0,
                              Self->PeriodX, Self->PeriodY);
                        }

                        return Self->noise_sum(Channel, px, py, fx, fy, stitch_width, stitch_height, wrap_x, wrap_y);
                     };

                     WritePixel(x, y, covered, sum);
                  }
               }
            };

            if (scalar) {
               render([&](int X, int Y, bool Covered, auto Sum) {
                  auto row = heights + std::ptrdiff_t(Y) * (layout.Stride / sizeof(float));
                  const double value = Covered ? Self->scalar_value(Sum(3), mapping) : 0.0;
                  row[X] = float(std::clamp(value, 0.0, 1.0));
               });
            }
            else {
               const auto bitmap = Self->Bitmap;
               const auto colours = Self->Colours.get();
               const uint8_t a = bitmap->ColourFormat->AlphaPos>>3;
               const uint8_t r = bitmap->ColourFormat->RedPos>>3;
               const uint8_t g = bitmap->ColourFormat->GreenPos>>3;
               const uint8_t b = bitmap->ColourFormat->BluePos>>3;
               render([&](int X, int Y, bool Covered, auto Sum) {
                  auto pixel = bitmap->Data + std::ptrdiff_t(Y) * bitmap->LineWidth + std::ptrdiff_t(X) * 4;
                  if (not Covered) {
                     pixel[r] = pixel[g] = pixel[b] = pixel[a] = 0;
                     return;
                  }

                  if (colours) {
                     // A ramp is drawn from the alpha lattice only, as for HEIGHT output.
                     write_colour_ramp_pixel(*colours, Self->scalar_value(Sum(3), mapping), pixel, r, g, b, a);
                     return;
                  }

                  const auto red = Self->image_channel(Sum(0), mapping);
                  const auto green = Self->image_channel(Sum(1), mapping);
                  const auto blue = Self->image_channel(Sum(2), mapping);
                  pixel[r] = linear_rgb ? glLinearRGB.invert(red) : red;
                  pixel[g] = linear_rgb ? glLinearRGB.invert(green) : green;
                  pixel[b] = linear_rgb ? glLinearRGB.invert(blue) : blue;
                  pixel[a] = Self->image_channel(Sum(3), mapping);
               });
            }
         });
      }

      pool.wait();
      if (scalar) return ERR::Okay;
      Self->CacheTransform = sampling;
      Self->CacheDomain = domain;
      Self->CacheRegion = region;
      Self->CacheColourSpace = effect_colour_space(Self);
      Self->Dirty = false;
   }

   return gfx::CopyArea(Self->Bitmap, Self->Target, BAF::NIL, 0, 0, width, height, 0, 0);
}

//********************************************************************************************************************

static void build_turbulence_lattice(extTurbulenceFX *Self)
{
   int lSeed = setup_seed(Self->Seed);
   auto &gradient = Self->Gradient;
   auto &lattice  = Self->Lattice;

   int i;
   for (int k=0; k < GSIZE; k++) {
      for (i=0; i < BSIZE; i++) {
         lattice[i] = i;
         for (int j=0; j < 2; j++) {
            gradient[k][i][j] = (double)(((lSeed = random(lSeed)) % (BSIZE + BSIZE)) - BSIZE) / BSIZE;
         }
         double s = double(sqrt(gradient[k][i][0] * gradient[k][i][0] + gradient[k][i][1] * gradient[k][i][1]));
         if (s > 0.0) {
            gradient[k][i][0] /= s;
            gradient[k][i][1] /= s;
         }
      }
   }

   while (--i) {
      int j;
      auto tmp = lattice[i];
      lattice[i] = lattice[j = (lSeed = random(lSeed)) % BSIZE];
      lattice[j] = tmp;
   }

   for (i=0; i < BSIZE + 2; i++) {
      lattice[BSIZE + i] = lattice[i];
      for (int k=0; k < GSIZE; k++) {
         for (int j=0; j < GSUBSIZE; j++) {
            gradient[k][BSIZE + i][j] = gradient[k][i][j];
         }
      }
   }

   // The Phase tables continue the same sequence, so the SVG lattice is unchanged and seeds that share a lattice also
   // share its animation.  Layer 0 must not be shifted.  A z component in [-1, 1] keeps the contrast steadiest
   // between layers.

   Self->ZOffset[0] = 0;
   for (i=1; i < BSIZE; i++) Self->ZOffset[i] = (lSeed = random(lSeed)) % BSIZE;
   for (int k=0; k < GSIZE; k++) {
      for (i=0; i < BSIZE; i++) {
         Self->GradientZ[k][i] = double(((lSeed = random(lSeed)) % (BSIZE + BSIZE)) - BSIZE) / BSIZE;
      }
   }
}

//********************************************************************************************************************

static ERR TURBULENCEFX_Init(extTurbulenceFX *Self)
{
   kt::Log log;

   if (svg_compliance(Self->Compliance)) {
      if (auto field = turbulence_extension(Self)) {
         log.warning("The %s extension is not permitted under SVG compliance.", field);
         return ERR::InvalidValue;
      }
   }

   build_turbulence_lattice(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static void turbulence_modified(extTurbulenceFX *Self)
{
   Self->Dirty = true;
   effect_modified(Self);
}

//********************************************************************************************************************
// Writes an extension field.  Once initialised with an explicit SVG compliance, only the default value is accepted.

template <class T> static ERR update_extension(extTurbulenceFX *Self, T &Field, T Value, T Default)
{
   if ((Value != Default) and Self->initialised() and svg_compliance(Self->Compliance)) return ERR::NoSupport;
   if (Field != Value) {
      Field = Value;
      turbulence_modified(Self);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Writes an origin field.  Values that differ only in their FD_SCALED flag are distinct.

static ERR update_origin(extTurbulenceFX *Self, Unit &Field, const Unit &Value)
{
   if (not std::isfinite(Value.Value)) return ERR::InvalidValue;
   if ((Value.Value != 0) and Self->initialised() and svg_compliance(Self->Compliance)) return ERR::NoSupport;
   if ((Field.Value != Value.Value) or (Field.scaled() != Value.scaled())) {
      Field = Unit(Value.Value, Value.scaled() ? FD_SCALED : 0);
      turbulence_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Amplitude: Scales the mapped noise value.

The noise value is mapped to the range [0, 1] as described for #Type, multiplied by Amplitude and then added to
#Offset.  The result is clamped to [0, 1] for output.  The value must be finite and defaults to 1.  This is a Kōtuku
extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Amplitude(extTurbulenceFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_extension(Self, Self->Amplitude, Value, 1.0);
}

/*********************************************************************************************************************

-FIELD-
Angle: Rotates the noise domain clockwise about its origin, in degrees.

The rotation is applied about the point given by #OriginX and #OriginY.  The value must be finite and defaults to 0.
A stitched or periodic domain cannot be rotated: if #Stitch, #PeriodX or #PeriodY is set, the Angle must be a
multiple of 360 or the effect will fail to draw.

If @VectorFilter.PrimitiveUnits is `BOUNDING_BOX`, the rotation applies in bounding-box units and therefore appears
skewed for a bounding box that is not square.  This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Angle(extTurbulenceFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_extension(Self, Self->Angle, Value, 0.0);
}

/*********************************************************************************************************************

-FIELD-
ColourMap: Draws `IMAGE` output through a pre-defined colour ramp.

The noise value of each sample is computed from the alpha lattice, as for `HEIGHT` output, and clamped to [0, 1].  It
then selects a colour from the named colourmap, from the first colour at 0 to the last at 1.  The following colourmaps
from the matplotlib and seaborn projects are supported: `cmap:crest`, `cmap:flare`, `cmap:icefire`, `cmap:inferno`,
`cmap:magma`, `cmap:mako`, `cmap:plasma`, `cmap:rocket`, `cmap:viridis`.

Setting a colourmap replaces custom #Stops.  Set an empty string to clear the ramp and restore the four independent
noise channels.  Ramp colours are drawn as authored in either colour space.  `HEIGHT` output ignores the ramp.
Unknown names fail with `ERR::NotFound`.  This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_ColourMap(extTurbulenceFX *Self, const std::string_view &Value)
{
   if ((not Value.empty()) and Self->initialised() and svg_compliance(Self->Compliance)) return ERR::NoSupport;
   if (auto error = set_colour_ramp_map(*Self, Value); error != ERR::Okay) return error;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Compliance: Declares whether SVG restrictions apply to the effect.
Lookup: FXC

The default, `INHERIT`, uses the @VectorScene.Compliance of the scene that owns the filter, or `NONE` if there is
no scene.  An explicit value always overrides the scene.

The extension fields are #Persistence, #Lacunarity, #Normalise, #Amplitude, #Offset, #OriginX, #OriginY, #Angle,
#PeriodX, #PeriodY, #Phase, #PhasePeriod, #ColourMap, #Stops and the `BILLOW` and `RIDGED` values of #Type.  When the
compliance is `SVG1` or `SVG2`, each extension field must remain at its default:

<list type="bullet">
<li>Initialisation fails with `ERR::InvalidValue` if the compliance is set explicitly and an extension field is in
use.</li>
<li>After initialisation, writing a value other than the default to an extension field returns `ERR::NoSupport`.
Writing the default is always permitted.</li>
<li>Setting `SVG1` or `SVG2` after initialisation returns `ERR::InvalidState` while an extension field is in use.</li>
<li>If the compliance is inherited from an SVG scene and an extension field is in use, `Draw` logs a warning and fails
with `ERR::InvalidValue`.</li>
</list>

`SVG1` and `SVG2` currently apply the same restrictions.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Compliance(extTurbulenceFX *Self, FXC Value)
{
   if ((Value < FXC::INHERIT) or (Value > FXC::SVG2)) return ERR::OutOfRange;
   if (Self->initialised() and svg_compliance(Value) and turbulence_extension(Self)) return ERR::InvalidState;
   if (Self->Compliance != Value) {
      Self->Compliance = Value;
      turbulence_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FX: The base frequency for noise on the X axis.

The frequency must be finite and non-negative.  The default value is zero.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_FX(extTurbulenceFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->FX = Value;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FY: The base frequency for noise on the Y axis.

The frequency must be finite and non-negative.  The default value is zero.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_FY(extTurbulenceFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->FY = Value;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Lacunarity: The frequency multiplier between octaves.

Each octave samples the noise Lacunarity times more finely than the previous one.  The value must lie in the range
[1, 8] and defaults to 2, the fixed multiplier of SVG.  If #Stitch is enabled and there is more than one octave, the
value must be an integer, otherwise `Draw` fails with `ERR::InvalidValue`.  The same applies if #PeriodX or #PeriodY
is set.  This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Lacunarity(extTurbulenceFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   if ((Value < 1.0) or (Value > 8.0)) return ERR::OutOfRange;
   return update_extension(Self, Self->Lacunarity, Value, 2.0);
}

/*********************************************************************************************************************

-FIELD-
Normalise: Divides the octave sum by its total weight.

If `TRUE`, the sum of the octaves is divided by the sum of their weights, `Persistence^0 + ... +
Persistence^(Octaves-1)`, before it is mapped.  This keeps several octaves within the range of a single octave, so
that changing #Octaves or #Persistence does not change the overall contrast.  Defaults to `FALSE`.  This is a Kōtuku
extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Normalise(extTurbulenceFX *Self, int Value)
{
   return update_extension(Self, Self->Normalise, Value ? 1 : 0, 0);
}

/*********************************************************************************************************************

-FIELD-
Octaves: The numOctaves parameter for the noise function.

Defaults to 1.  Runtime writes accept values from 0 to 30.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Octaves(extTurbulenceFX *Self, int Value)
{
   if (Self->initialised() and ((Value < 0) or (Value > 30))) return ERR::OutOfRange;
   Self->Octaves = Value;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Offset: Adds to the scaled noise value.

See #Amplitude.  The value must be finite and defaults to 0.  This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Offset(extTurbulenceFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_extension(Self, Self->Offset, Value, 0.0);
}

/*********************************************************************************************************************

-FIELD-
OriginX: The horizontal origin of the noise domain.

The noise is sampled relative to this point, so moving the origin translates the noise without changing it.  #Angle
rotates the domain about the origin, and #PeriodX and #PeriodY measure their repeats from it.

The origin is expressed in primitive units and is measured from primitive-unit (0, 0), which is also the origin of SVG
`feTurbulence`.  A percentage refers to the width of the filter region.  The value must be finite and defaults to 0.
This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_OriginX(extTurbulenceFX *Self, Unit &Value)
{
   return update_origin(Self, Self->OriginX, Value);
}

/*********************************************************************************************************************

-FIELD-
OriginY: The vertical origin of the noise domain.

A percentage refers to the height of the filter region.  The value must be finite and defaults to 0.  See #OriginX.
This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_OriginY(extTurbulenceFX *Self, Unit &Value)
{
   return update_origin(Self, Self->OriginY, Value);
}

/*********************************************************************************************************************

-FIELD-
PeriodX: Repeats the noise horizontally after this number of lattice cells.

The first octave repeats exactly every `PeriodX / FX` primitive units, measured from #OriginX, and octave `k` repeats
every `PeriodX * Lacunarity^k` cells of its own lattice, so the full octave stack repeats with no seam.  Unlike
#Stitch, the period does not adjust the frequency or depend on the primitive subregion.

The value must lie in the range [0, 1048576], and 0, the default, disables horizontal repetition.  A period cannot be
combined with #Stitch or a rotated domain, and a period with several octaves requires an integer #Lacunarity; `Draw`
fails with `ERR::InvalidValue` otherwise.  This is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_PeriodX(extTurbulenceFX *Self, int Value)
{
   if ((Value < 0) or (Value > TURBULENCE_MAX_PERIOD)) return ERR::OutOfRange;
   return update_extension(Self, Self->PeriodX, Value, 0);
}

/*********************************************************************************************************************

-FIELD-
PeriodY: Repeats the noise vertically after this number of lattice cells.

The value must lie in the range [0, 1048576], and 0, the default, disables vertical repetition.  See #PeriodX.  This
is a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_PeriodY(extTurbulenceFX *Self, int Value)
{
   if ((Value < 0) or (Value > TURBULENCE_MAX_PERIOD)) return ERR::OutOfRange;
   return update_extension(Self, Self->PeriodY, Value, 0);
}

/*********************************************************************************************************************

-FIELD-
Phase: Animates the noise by moving through a third lattice dimension.

The noise is a slice of 3-D gradient noise, and Phase selects the position of the slice in lattice cells.  Changing it
gradually makes the pattern evolve smoothly, and a change of about one cell gives an unrelated pattern.  Octave `k`
moves #Lacunarity`^k` times faster, so fine detail changes faster than coarse detail.

A Phase of 0, the default, reproduces the noise exactly as drawn without animation.  Other values cost about twice as
much to draw.  The noise does not repeat unless #PhasePeriod is set.  The value must be finite.  This is a Kōtuku
extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Phase(extTurbulenceFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_extension(Self, Self->Phase, Value, 0.0);
}

/*********************************************************************************************************************

-FIELD-
PhasePeriod: Loops the #Phase animation after this number of lattice cells.

If set, the noise at `Phase + PhasePeriod` is identical to the noise at #Phase, so an animation that advances #Phase
from 0 to PhasePeriod loops without a seam.  Octave `k` repeats after `PhasePeriod * Lacunarity^k` cells of its own
lattice.  Small periods give short loops with less variety: a period of 1 returns to its starting pattern after a
single cell.

The value must lie in the range [0, 1048576], and 0, the default, disables looping.  A period with several octaves
requires an integer #Lacunarity; `Draw` fails with `ERR::InvalidValue` otherwise.  This is a Kōtuku extension; see
#Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_PhasePeriod(extTurbulenceFX *Self, int Value)
{
   if ((Value < 0) or (Value > TURBULENCE_MAX_PERIOD)) return ERR::OutOfRange;
   return update_extension(Self, Self->PhasePeriod, Value, 0);
}

/*********************************************************************************************************************

-FIELD-
Persistence: The weight of each octave relative to the previous one.

Octave `k` is weighted by `Persistence^k`.  The value must lie in the range [0, 1] and defaults to 0.5, the fixed
weight of SVG.  Lower values give smoother results, and a value of 0 leaves the first octave only.  This is a Kōtuku
extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Persistence(extTurbulenceFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   if ((Value < 0.0) or (Value > 1.0)) return ERR::OutOfRange;
   return update_extension(Self, Self->Persistence, Value, 0.5);
}

/*********************************************************************************************************************

-FIELD-
Seed: The starting number for the pseudo random number generator.

If the value is undefined, the effect is as if a value of `0` were specified.  When the seed number is handed over to
the algorithm it must first be truncated, i.e. rounded to the closest integer value towards zero.  Changing
Seed rebuilds the noise lattice and invalidates the cached result.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Seed(extTurbulenceFX *Self, int Value)
{
   if (Self->Seed IS Value) return ERR::Okay;
   Self->Seed = Value;
   if (Self->initialised()) build_turbulence_lattice(Self);
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stitch: If `TRUE`, stitching will be enabled at the tile's edges.

By default, the turbulence algorithm will sometimes show discontinuities at the tile borders.  If Stitch is set to
`TRUE` then the algorithm will automatically adjust base frequency values such that the node's width and height
(i.e., the width and height of the current subregion) contains an integral number of the Perlin tile width and height
for the first octave.

The baseFrequency will be adjusted up or down depending on which way has the smallest relative (not absolute) change
as follows:  Given the frequency, calculate `lowFreq = floor(width*frequency) / width` and
`hiFreq = ceil(width * frequency) / width`. If `frequency/lowFreq < hiFreq/frequency` then use lowFreq, else use
hiFreq.  While generating turbulence values, generate lattice vectors as normal for Perlin Noise, except for those
lattice  points that lie on the right or bottom edges of the active area (the size of the resulting tile).  In those
cases, copy the lattice vector from the opposite edge of the active area.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Stitch(extTurbulenceFX *Self, int Value)
{
   Self->Stitch = Value;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stops: Draws `IMAGE` output through a custom colour ramp.

The noise value of each sample is computed from the alpha lattice, as for `HEIGHT` output, and clamped to [0, 1].  It
then selects a colour from the gradient defined by the stops, including the opacity of each stop.  At least two stops
are required, or the setter fails with `ERR::InvalidValue`.  A ramp from opaque black to opaque white draws the noise
as greyscale.

Setting stops replaces any named #ColourMap.  Set an empty array to clear the ramp and restore the four independent
noise channels.  Ramp colours are drawn as authored in either colour space.  `HEIGHT` output ignores the ramp.  This is
a Kōtuku extension; see #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Stops(extTurbulenceFX *Self, std::span<const GradientStop> &Value)
{
   if ((not Value.empty()) and Self->initialised() and svg_compliance(Self->Compliance)) return ERR::NoSupport;
   if (auto error = set_colour_ramp_stops(*Self, Value); error != ERR::Okay) return error;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Type: Selects the function that combines the octaves of noise.
Lookup: TB

Each octave `k` samples gradient noise `n` at #Lacunarity`^k` times the base frequency and weights it by
#Persistence`^k`.  The weighted terms are summed and the sum is mapped to the range [0, 1]:

<types type="Type">
<type name="TURBULENCE">The term is `abs(n)` and the sum is used directly.  This is the default.</type>
<type name="NOISE">The term is `n` and the sum `s` is mapped to `(s + 1) / 2`.</type>
<type name="BILLOW">The term is `2 * sqrt(2) * abs(n) - 1` and the sum `s` is mapped to `(s + 1) / 2`.  The rounded
lobes resemble cumulus cloud.</type>
<type name="RIDGED">The term is `(1 - sqrt(2) * abs(n))^2`, multiplied by twice the previous octave's term clamped to
[0, 1].  The sum is used directly.  The sharp crests resemble mountain ridges or veins.</type>
</types>

`BILLOW` and `RIDGED` are Kōtuku extensions that are not available under SVG #Compliance.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Type(extTurbulenceFX *Self, TB Value)
{
   if ((Value != TB::NOISE) and (Value != TB::TURBULENCE) and (Value != TB::BILLOW) and (Value != TB::RIDGED)) {
      return ERR::InvalidValue;
   }
   if (Self->initialised() and svg_compliance(Self->Compliance) and
       ((Value IS TB::BILLOW) or (Value IS TB::RIDGED))) return ERR::NoSupport;
   Self->Type = Value;
   turbulence_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputMode: Selects an image or a linear height result from the noise's alpha channel.
Lookup: FOM

Use `HEIGHT` when passing the result to enhanced @LightingFX or height-output @BlurFX.  `HEIGHT` output ignores
#ColourMap and #Stops.

-END-

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_OutputMode(extTurbulenceFX *Self, FOM Value)
{
   if ((Value != FOM::IMAGE) and (Value != FOM::HEIGHT)) return ERR::OutOfRange;

   if (Self->OutputMode != Value) {
      Self->OutputMode = Value;
      if (Self->Filter) Self->Filter->GraphRevision++;
   }

   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.

The element that is written depends on the resolved #Compliance.  If the compliance resolves to `SVG1` or `SVG2`, the
primitive is written as a standard `feTurbulence` element.  If #Compliance is explicitly `NONE`, it is always written
as `xvg:feFractalNoise`, so that the declaration survives a round trip.  If #Compliance is `INHERIT` and resolves to
`NONE`, `feTurbulence` is written when every extension field is at its default, and `xvg:feFractalNoise` otherwise.

In each case, `HEIGHT` output is described by an `xvg:outputMode` attribute.  The `xvg:feFractalNoise` element only
includes attributes whose fields differ from their defaults.
-END-

*********************************************************************************************************************/

static ERR turbulence_xml(const extTurbulenceFX &Effect, filter_xml_element &Element)
{
   const bool extended = (Effect.Compliance IS FXC::NONE) or
      ((turbulence_compliance(&Effect) IS VSC::NONE) and turbulence_extension(&Effect));

   if (not extended) {
      Element.Name = "feTurbulence";
      Element.attribute("baseFrequency", Element.number(Effect.FX) + ' ' + Element.number(Effect.FY));
      Element.attribute("numOctaves", Effect.Octaves);
      Element.attribute("seed", Effect.Seed);
      Element.attribute("stitchTiles", Effect.Stitch ? "stitch" : "noStitch");
      Element.attribute("type", Effect.Type IS TB::NOISE ? "fractalNoise" : "turbulence");
      if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("xvg:outputMode", "height");
      return ERR::Okay;
   }

   auto length = [&](const Unit &Value) {
      // Invert the percentage conversion in Unit::read() so that the value reloads unchanged.
      return Value.scaled() ? Element.number(Value.Value / 0.01) + '%' : Element.number(Value.Value);
   };

   Element.Name = "xvg:feFractalNoise";
   if ((Effect.FX != 0) or (Effect.FY != 0)) {
      if (Effect.FX IS Effect.FY) Element.attribute("baseFrequency", Effect.FX);
      else Element.attribute("baseFrequency", Element.number(Effect.FX) + ' ' + Element.number(Effect.FY));
   }
   if (Effect.Octaves != 1) Element.attribute("numOctaves", Effect.Octaves);
   if (Effect.Seed) Element.attribute("seed", Effect.Seed);
   if (Effect.Stitch) Element.attribute("stitchTiles", "stitch");

   static constexpr std::string_view types[] = { "turbulence", "fractalNoise", "billow", "ridged" };
   if (Effect.Type != TB::TURBULENCE) Element.attribute("type", types[int(Effect.Type)]);

   if (Effect.Persistence != 0.5) Element.attribute("persistence", Effect.Persistence);
   if (Effect.Lacunarity != 2.0) Element.attribute("lacunarity", Effect.Lacunarity);
   if (Effect.Normalise) Element.attribute("normalise", "true");
   if (Effect.Amplitude != 1.0) Element.attribute("amplitude", Effect.Amplitude);
   if (Effect.Offset != 0) Element.attribute("offset", Effect.Offset);

   if ((Effect.OriginX.Value != 0) or (Effect.OriginY.Value != 0)) {
      Element.attribute("origin", length(Effect.OriginX) + ' ' + length(Effect.OriginY));
   }

   if (Effect.Angle != 0) Element.attribute("angle", Effect.Angle);

   if ((Effect.PeriodX != 0) or (Effect.PeriodY != 0)) {
      if (Effect.PeriodX IS Effect.PeriodY) Element.attribute("period", Effect.PeriodX);
      else Element.attribute("period", Element.number(Effect.PeriodX) + ' ' + Element.number(Effect.PeriodY));
   }

   if (Effect.Phase != 0) Element.attribute("phase", Effect.Phase);
   if (Effect.PhasePeriod) Element.attribute("phasePeriod", Effect.PhasePeriod);

   if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("xvg:outputMode", "height");
   return colour_ramp_xml(Effect, Element);
}

//********************************************************************************************************************

#include "filter_turbulence_def.c"

static const FieldArray clTurbulenceFXFields[] = {
   { "FX",          FDF_DOUBLE|FDF_RW,         nullptr, TURBULENCEFX_SET_FX },
   { "FY",          FDF_DOUBLE|FDF_RW,         nullptr, TURBULENCEFX_SET_FY },
   { "Octaves",     FDF_INT|FDF_RW,            nullptr, TURBULENCEFX_SET_Octaves },
   { "Seed",        FDF_INT|FDF_RW,            nullptr, TURBULENCEFX_SET_Seed },
   { "Stitch",      FDF_INT|FDF_RW,            nullptr, TURBULENCEFX_SET_Stitch },
   { "Type",        FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, TURBULENCEFX_SET_Type, &clTurbulenceFXTB },
   { "OutputMode",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, TURBULENCEFX_SET_OutputMode, &clTurbulenceFXFOM },
   { "Compliance",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, TURBULENCEFX_SET_Compliance, &clTurbulenceFXFXC },
   { "Persistence", FDF_DOUBLE|FDF_RW,     nullptr, TURBULENCEFX_SET_Persistence },
   { "Lacunarity",  FDF_DOUBLE|FDF_RW,     nullptr, TURBULENCEFX_SET_Lacunarity },
   { "Amplitude",   FDF_DOUBLE|FDF_RW,     nullptr, TURBULENCEFX_SET_Amplitude },
   { "Offset",      FDF_DOUBLE|FDF_RW,     nullptr, TURBULENCEFX_SET_Offset },
   { "OriginX",     FDF_UNIT|FDF_RW,       nullptr, TURBULENCEFX_SET_OriginX },
   { "OriginY",     FDF_UNIT|FDF_RW,       nullptr, TURBULENCEFX_SET_OriginY },
   { "Angle",       FDF_DOUBLE|FDF_RW,     nullptr, TURBULENCEFX_SET_Angle },
   { "ColourMap",   FDF_CPPSTRING|FDF_RW,  nullptr, TURBULENCEFX_SET_ColourMap },
   { "Stops",       FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, TURBULENCEFX_SET_Stops, "GradientStop" },
   { "Normalise",   FDF_INT|FDF_RW,        nullptr, TURBULENCEFX_SET_Normalise },
   { "PeriodX",     FDF_INT|FDF_RW,        nullptr, TURBULENCEFX_SET_PeriodX },
   { "PeriodY",     FDF_INT|FDF_RW,        nullptr, TURBULENCEFX_SET_PeriodY },
   { "PhasePeriod", FDF_INT|FDF_RW,        nullptr, TURBULENCEFX_SET_PhasePeriod },
   { "Phase",       FDF_DOUBLE|FDF_RW,     nullptr, TURBULENCEFX_SET_Phase },
   { "XMLDef",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE,      FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_turbulencefx(void)
{
   clTurbulenceFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::TURBULENCEFX),
      fl::Name("TurbulenceFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clTurbulenceFXActions),
      fl::Fields(clTurbulenceFXFields),
      fl::Size(sizeof(extTurbulenceFX)),
      fl::Path(MOD_PATH));

   return clTurbulenceFX ? ERR::Okay : ERR::AddClass;
}
