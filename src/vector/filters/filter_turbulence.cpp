/*********************************************************************************************************************

-CLASS-
TurbulenceFX: A filter effect that utilises the Perlin turbulence function.

This filter effect creates an image using the Perlin turbulence function. It allows the synthesis of artificial
textures like clouds or marble.  For a detailed description the of the Perlin turbulence function, see "Texturing
and Modeling", Ebert et al, AP Professional, 1994. The resulting image will fill the entire filter primitive
subregion for this filter primitive.

It is possible to create bandwidth-limited noise by synthesizing only one octave.

The following order is used for applying the pseudo random numbers.  An initial seed value is computed based on
#Seed.  Then the implementation computes the lattice points for R, then continues getting additional pseudo random
numbers relative to the last generated pseudo random number and computes the lattice points for G, and so on for B
and A.

-END-

*********************************************************************************************************************/

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
   objBitmap *Bitmap = nullptr;
   double Gradient[GSIZE][LSIZE][GSUBSIZE];
   int    Lattice[LSIZE];
   bool   Dirty = true;
   agg::trans_affine CacheTransform;
   TClipRectangle<double> CacheRegion = {};
   VCS CacheColourSpace = VCS::INHERIT;

   double noise2(uint8_t Channel, double VX, double VY, bool StitchNoise = false, int StitchWidth = 0,
      int StitchHeight = 0, int WrapX = 0, int WrapY = 0) {
      int bx0, bx1, by0, by1, b00, b10, b01, b11;
      double rx0, rx1, ry0, ry1, *q, sx, sy, a, b, t, u, v;

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

      bx0 &= BM;
      bx1 &= BM;
      by0 &= BM;
      by1 &= BM;
      int i = Lattice[bx0];
      int j = Lattice[bx1];
      b00 = Lattice[i + by0];
      b10 = Lattice[j + by0];
      b01 = Lattice[i + by1];
      b11 = Lattice[j + by1];
      sx = s_curve(rx0);
      sy = s_curve(ry0);
      q = Gradient[Channel][b00]; u = rx0 * q[0] + ry0 * q[1];
      q = Gradient[Channel][b10]; v = rx1 * q[0] + ry0 * q[1];
      a = std::lerp(u, v, sx);
      q = Gradient[Channel][b01]; u = rx0 * q[0] + ry1 * q[1];
      q = Gradient[Channel][b11]; v = rx1 * q[0] + ry1 * q[1];
      b = std::lerp(u, v, sx);
      return std::lerp(a, b, sy);
   }

   double noise_sum(uint8_t Channel, double X, double Y, double FrequencyX, double FrequencyY,
      int StitchWidth, int StitchHeight, int WrapX, int WrapY) {
      double sum = 0.0;
      double vx = X * FrequencyX;
      double vy = Y * FrequencyY;
      double ratio = 1.0;
      for (int n=0; n < Octaves; n++) {
         double noise = noise2(Channel, vx, vy, Stitch, StitchWidth, StitchHeight, WrapX, WrapY);
         if (Type IS TB::NOISE) sum += noise * ratio;
         else sum += fabs(noise) * ratio;
         vx *= 2.0;
         vy *= 2.0;
         ratio *= 0.5;
         if (Stitch) {
            StitchWidth *= 2;
            StitchHeight *= 2;
            WrapX = 2 * WrapX - PerlinN;
            WrapY = 2 * WrapY - PerlinN;
         }
      }

      return sum;
   }

   uint8_t image_channel(double Sum) const {
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

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR TURBULENCEFX_Draw(extTurbulenceFX *Self, struct acDraw *Args)
{
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

      if ((not Self->CacheTransform.is_equal(sampling, 0.0)) or (Self->CacheColourSpace != effect_colour_space(Self)) or
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
      // Leave room for PerlinN, the adjacent lattice point and the stitched subtraction.
      constexpr double limit = std::numeric_limits<int>::max() / 4.0;
      const auto lattice_fits = [octaves](double Start, double End, double Frequency) {
         const double extent = std::max(std::abs(Start), std::abs(End)) * std::abs(Frequency);
         return std::ldexp(extent, std::max(octaves - 1, 0)) < limit;
      };
      if ((not lattice_fits(samples.left, samples.right, fx)) or
          (not lattice_fits(samples.top, samples.bottom, fy))) return ERR::OutOfRange;

      if (Self->Stitch) {
         // The counters use the full authored tile and are doubled even after the final sample.
         const auto prepare_axis = [octaves](double Origin, double Size, double Frequency, int &Period, int &Wrap) {
            const double period = std::trunc(Size * Frequency + 0.5);
            const double wrap = std::trunc(Origin * Frequency + PerlinN + period);
            if ((not (std::ldexp(std::abs(period), octaves) < limit)) or
                (not (std::ldexp(std::abs(wrap - PerlinN), octaves) < limit))) return false;
            Period = int(period);
            Wrap = int(wrap);
            return true;
         };
         if ((not prepare_axis(region_x, region_width, fx, stitch_width, wrap_x)) or
             (not prepare_axis(region_y, region_height, fy, stitch_height, wrap_y))) return ERR::OutOfRange;
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
                     auto sum = [&](uint8_t Channel) {
                        return Self->noise_sum(Channel, px, py, fx, fy, stitch_width, stitch_height, wrap_x, wrap_y);
                     };
                     WritePixel(x, y, covered, sum);
                  }
               }
            };

            if (scalar) {
               render([&](int X, int Y, bool Covered, auto Sum) {
                  auto row = heights + std::ptrdiff_t(Y) * (layout.Stride / sizeof(float));
                  double value = 0;
                  if (Covered) {
                     value = Sum(3);
                     if (Self->Type IS TB::NOISE) value = (value + 1.0) * 0.5;
                  }
                  row[X] = float(std::clamp(value, 0.0, 1.0));
               });
            }
            else {
               const auto bitmap = Self->Bitmap;
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
                  const auto red = Self->image_channel(Sum(0));
                  const auto green = Self->image_channel(Sum(1));
                  const auto blue = Self->image_channel(Sum(2));
                  pixel[r] = linear_rgb ? glLinearRGB.invert(red) : red;
                  pixel[g] = linear_rgb ? glLinearRGB.invert(green) : green;
                  pixel[b] = linear_rgb ? glLinearRGB.invert(blue) : blue;
                  pixel[a] = Self->image_channel(Sum(3));
               });
            }
         });
      }
      pool.wait();
      if (scalar) return ERR::Okay;
      Self->CacheTransform = sampling;
      Self->CacheRegion = region;
      Self->CacheColourSpace = effect_colour_space(Self);
      Self->Dirty = false;
   }

   return gfx::CopyArea(Self->Bitmap, Self->Target, BAF::NIL, 0, 0, width, height, 0, 0);
}

//********************************************************************************************************************

static ERR TURBULENCEFX_Init(extTurbulenceFX *Self)
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

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FX: The base frequency for noise on the X axis.

A negative value for base frequency is an error.  The default value is zero.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_FX(extTurbulenceFX *Self, double Value)
{
   if (Value >= 0) {
      Self->FX = Value;
      Self->Dirty = true;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
FY: The base frequency for noise on the Y axis.

A negative value for base frequency is an error.  The default value is zero.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_FY(extTurbulenceFX *Self, double Value)
{
   if (Value >= 0) {
      Self->FY = Value;
      Self->Dirty = true;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Octaves: The numOctaves parameter for the noise function.

Defaults to `1` if not specified.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Octaves(extTurbulenceFX *Self, int Value)
{
   Self->Octaves = Value;
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Seed: The starting number for the pseudo random number generator.

If the value is undefined, the effect is as if a value of `0` were specified.  When the seed number is handed over to
the algorithm it must first be truncated, i.e. rounded to the closest integer value towards zero.

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Seed(extTurbulenceFX *Self, int Value)
{
   Self->Seed = Value;
   Self->Dirty = true;
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
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Type: Can be set to 'noise' or 'turbulence'.


*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_Type(extTurbulenceFX *Self, TB Value)
{
   Self->Type = Value;
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputMode: Selects an image or a linear height result from the noise's alpha channel.
Lookup: FOM

Use `HEIGHT` when passing the result to enhanced @LightingFX or height-output @BlurFX.

-END-

*********************************************************************************************************************/

static ERR TURBULENCEFX_SET_OutputMode(extTurbulenceFX *Self, FOM Value)
{
   if ((Value != FOM::IMAGE) and (Value != FOM::HEIGHT)) return ERR::OutOfRange;
   if (Self->OutputMode != Value) {
      Self->OutputMode = Value;
      if (Self->Filter) Self->Filter->GraphRevision++;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR TURBULENCEFX_GET_XMLDef(extTurbulenceFX *Self, std::string &Value)
{
   std::stringstream stream;

   stream.precision(std::numeric_limits<double>::max_digits10);
   stream << "feTurbulence baseFrequency=\"" << Self->FX << ' ' << Self->FY
      << "\" numOctaves=\"" << Self->Octaves << "\" seed=\"" << Self->Seed
      << "\" stitchTiles=\"" << (Self->Stitch ? "stitch" : "noStitch")
      << "\" type=\"" << ((Self->Type IS TB::NOISE) ? "fractalNoise" : "turbulence") << '"';
   if (Self->OutputMode IS FOM::HEIGHT) stream << " kotuku:outputMode=\"height\"";

   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_turbulence_def.c"

static const FieldArray clTurbulenceFXFields[] = {
   { "FX",      FDF_DOUBLE|FDF_RI,         nullptr, TURBULENCEFX_SET_FX },
   { "FY",      FDF_DOUBLE|FDF_RI,         nullptr, TURBULENCEFX_SET_FY },
   { "Octaves", FDF_INT|FDF_RI,            nullptr, TURBULENCEFX_SET_Octaves },
   { "Seed",    FDF_INT|FDF_RI,            nullptr, TURBULENCEFX_SET_Seed },
   { "Stitch",  FDF_INT|FDF_RI,            nullptr, TURBULENCEFX_SET_Stitch },
   { "Type",    FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, TURBULENCEFX_SET_Type, &clTurbulenceFXTB },
   { "OutputMode", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, TURBULENCEFX_SET_OutputMode, &clTurbulenceFXFOM },
   { "XMLDef",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE,      TURBULENCEFX_GET_XMLDef },
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
