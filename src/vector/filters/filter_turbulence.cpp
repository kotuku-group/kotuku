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
   using create = pf::Create<extTurbulenceFX>;

   objBitmap *Bitmap;
   double Gradient[GSIZE][LSIZE][GSUBSIZE];
   int    Lattice[LSIZE];
   double FX, FY;
   int    Octaves;
   int    Seed;
   TB     Type;
   bool   Stitch;
   bool   Dirty;

   private:
   int stitch_width, stitch_height;
   int wrap_x, wrap_y;

   public:

   double noise2(uint8_t Channel, double VX, double VY) {
      int bx0, bx1, by0, by1, b00, b10, b01, b11;
      double rx0, rx1, ry0, ry1, *q, sx, sy, a, b, t, u, v;

      t = VX + PerlinN;
      bx0 = (int)t;
      bx1 = bx0 + 1;
      rx0 = t - (int)t;
      rx1 = rx0 - 1.0;

      t = VY + PerlinN;
      by0 = (int)t;
      by1 = by0 + 1;
      ry0 = t - (int)t;
      ry1 = ry0 - 1.0;

      // If stitching, adjust lattice points accordingly.

      if (Stitch) {
         if (bx0 >= wrap_x) bx0 -= stitch_width;
         if (bx1 >= wrap_x) bx1 -= stitch_width;
         if (by0 >= wrap_y) by0 -= stitch_height;
         if (by1 >= wrap_y) by1 -= stitch_height;
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

   // Standard turbulence (non-stitched)

   uint8_t turbulence(uint8_t Channel, int x, int y) {
      double sum = 0.0;
      double vx = x * FX;
      double vy = y * FY;
      double ratio = 1.0;
      for (int n=0; n < Octaves; n++) {
         double noise = noise2(Channel, vx, vy);
         if (Type IS TB::NOISE) sum += noise * ratio;
         else sum += fabs(noise) * ratio;
         vx *= 2.0;
         vy *= 2.0;
         ratio *= 0.5;
      }

      int col;
      if (Type IS TB::NOISE) col = ((sum * 255.0) + 255.0) * 0.5;
      else col = sum * 255.0;

      return (col < 0) ? 0 : (col > 255) ? 255 : col;
   }

   // Stitched turbulence

   uint8_t turbulence_stitch(uint8_t Channel, int x, int y, double FX, double FY, int StitchWidth, int StitchHeight) {
      wrap_x = (x % StitchWidth) * FX + PerlinN + stitch_width;
      wrap_y = (y % StitchHeight) * FY + PerlinN + stitch_height;

      double sum = 0;
      double vx = x * FX;
      double vy = y * FY;
      double ratio = 1;
      for (int n=0; n < Octaves; n++) {
         double noise = noise2(Channel, vx, vy);

         if (Type IS TB::NOISE) sum += noise * ratio;
         else sum += fabs(noise) * ratio;

         vx *= 2;
         vy *= 2;
         ratio *= 0.5;
         // Update stitch values. Subtracting PerlinN before the multiplication and adding it
         // afterwards simplifies to subtracting it once.
         stitch_width  *= 2;
         wrap_x         = 2 * wrap_x - PerlinN;
         stitch_height *= 2;
         wrap_y         = 2 * wrap_y - PerlinN;
      }

      int col;
      if (Type IS TB::NOISE) col = ((sum * 255.0) + 255.0) * 0.5;
      else col = sum * 255.0;

      return (col < 0) ? 0 : (col > 255) ? 255 : col;
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
   if (Self->Target->BytesPerPixel != 4) return ERR::InvalidState;

   const int width = F2I(Self->Filter->TargetWidth);
   const int height = F2I(Self->Filter->TargetHeight);

   if ((width <= 0) or (height <= 0)) return ERR::Okay;

   if (!Self->Bitmap) {
      Self->Dirty = true;
      if (!(Self->Bitmap = objBitmap::create::local(fl::Name("turbulence_bmp"),
         fl::Width(width),
         fl::Height(height),
         fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL),
         fl::BlendMode(BLM::NONE),
         fl::ColourSpace(CS::SRGB)))) return ERR::CreateObject;
   }
   else if ((Self->Bitmap->Width != width) or (Self->Bitmap->Height != height)) {
      Self->Dirty = true;
      Self->Bitmap->resize(width, height);
   }

   if (Self->Dirty) {
      Self->Dirty = false;

      int thread_count = std::thread::hardware_concurrency();
      if (thread_count > height/4) thread_count = height/4;
      if (thread_count < 1) thread_count = 1;

      BS::thread_pool pool(thread_count);

      for (int i=0; i < thread_count; i++) {
         pool.detach_task([Self, Start = (height * i) / thread_count, End = (height * (i + 1)) / thread_count]() {
            const uint8_t A = Self->Bitmap->ColourFormat->AlphaPos>>3;
            const uint8_t R = Self->Bitmap->ColourFormat->RedPos>>3;
            const uint8_t G = Self->Bitmap->ColourFormat->GreenPos>>3;
            const uint8_t B = Self->Bitmap->ColourFormat->BluePos>>3;

            uint8_t *data = Self->Bitmap->Data;

            if (Self->Stitch) {
               TClipRectangle<double> bounds = { Self->Filter->ClientViewport->vpFixedWidth, Self->Filter->ClientViewport->vpFixedHeight, 0, 0 };
               calc_full_boundary(Self->Filter->ClientVector, bounds, false, false);
               const int tile_width  = F2I(bounds.width());
               const int tile_height = F2I(bounds.height());

               // When stitching tiled turbulence, the frequencies must be adjusted so that the tile borders will be continuous.

               auto fx = Self->FX;
               auto fy = Self->FY;

               if (fx != 0.0) {
                  double fLoFreq = double(floor(tile_width * fx)) / tile_width;
                  double fHiFreq = double(ceil(tile_width * fx)) / tile_width;
                  if (fx / fLoFreq < fHiFreq / fx) fx = fLoFreq;
                  else fx = fHiFreq;
               }

               if (fy != 0.0) {
                  double fLoFreq = double(floor(tile_height * fy)) / tile_height;
                  double fHiFreq = double(ceil(tile_height * fy)) / tile_height;
                  if (fy / fLoFreq < fHiFreq / fy) fy = fLoFreq;
                  else fy = fHiFreq;
               }

               auto stitch_width  = F2I(tile_width * fx);
               auto stitch_height = F2I(tile_height * fy);

               for (int y=Start; y < End; y++) {
                  uint8_t *pixel = data + (Self->Bitmap->LineWidth * y);
                  for (int x=0; x < Self->Bitmap->Width; x++, pixel += 4) {
                     pixel[R] = glLinearRGB.invert(Self->turbulence_stitch(0, x, y, fx, fy, stitch_width, stitch_height));
                     pixel[G] = glLinearRGB.invert(Self->turbulence_stitch(1, x, y, fx, fy, stitch_width, stitch_height));
                     pixel[B] = glLinearRGB.invert(Self->turbulence_stitch(2, x, y, fx, fy, stitch_width, stitch_height));
                     pixel[A] = Self->turbulence_stitch(3, x, y, fx, fy, stitch_width, stitch_height);
                  }
               }
            }
            else {
               for (int y=Start; y < End; y++) {
                  uint8_t *pixel = data + (Self->Bitmap->LineWidth * y);
                  for (int x=0; x < Self->Bitmap->Width; x++, pixel += 4) {
                     pixel[R] = glLinearRGB.invert(Self->turbulence(0, x, y));
                     pixel[G] = glLinearRGB.invert(Self->turbulence(1, x, y));
                     pixel[B] = glLinearRGB.invert(Self->turbulence(2, x, y));
                     pixel[A] = Self->turbulence(3, x, y);
                  }
               }
            }
         });
      }

      pool.wait();
   }

   render_to_filter(Self, Self->Bitmap, ARF::NONE, Self->Filter->Scene->SampleMethod);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TURBULENCEFX_Free(extTurbulenceFX *Self)
{
   if (Self->Bitmap) { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }
   return ERR::Okay;
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
         gradient[k][i][0] /= s;
         gradient[k][i][1] /= s;
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

//********************************************************************************************************************

static ERR TURBULENCEFX_NewObject(extTurbulenceFX *Self)
{
   Self->Octaves    = 1;
   Self->Stitch     = false;
   Self->Seed       = 0;
   Self->Type       = TB::TURBULENCE;
   Self->FX         = 0;
   Self->FY         = 0;
   Self->SourceType = VSF::NONE;
   Self->Dirty      = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FX: The base frequency for noise on the X axis.

A negative value for base frequency is an error.  The default value is zero.

*********************************************************************************************************************/

static ERR TURBULENCEFX_GET_FX(extTurbulenceFX *Self, double *Value)
{
   *Value = Self->FX;
   return ERR::Okay;
}

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

static ERR TURBULENCEFX_GET_FY(extTurbulenceFX *Self, double *Value)
{
   *Value = Self->FY;
   return ERR::Okay;
}

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

static ERR TURBULENCEFX_GET_Octaves(extTurbulenceFX *Self, int *Value)
{
   *Value = Self->Octaves;
   return ERR::Okay;
}

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

static ERR TURBULENCEFX_GET_Seed(extTurbulenceFX *Self, int *Value)
{
   *Value = Self->Seed;
   return ERR::Okay;
}

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

static ERR TURBULENCEFX_GET_Stitch(extTurbulenceFX *Self, int *Value)
{
   *Value = Self->Stitch;
   return ERR::Okay;
}

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

static ERR TURBULENCEFX_GET_Type(extTurbulenceFX *Self, TB *Value)
{
   *Value = Self->Type;
   return ERR::Okay;
}

static ERR TURBULENCEFX_SET_Type(extTurbulenceFX *Self, TB Value)
{
   Self->Type = Value;
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR TURBULENCEFX_GET_XMLDef(extTurbulenceFX *Self, STRING *Value)
{
   std::stringstream stream;

   stream << "feTurbulence";

   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_turbulence_def.c"

static const FieldDef clTurbulenceType[] = {
   { "Turbulence", TB::TURBULENCE },
   { "Noise",      TB::NOISE },
   { nullptr, 0 }
};
static const FieldArray clTurbulenceFXFields[] = {
   { "FX",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RI,          TURBULENCEFX_GET_FX,      TURBULENCEFX_SET_FX },
   { "FY",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RI,          TURBULENCEFX_GET_FY,      TURBULENCEFX_SET_FY },
   { "Octaves", FDF_VIRTUAL|FDF_INT|FDF_RI,             TURBULENCEFX_GET_Octaves, TURBULENCEFX_SET_Octaves },
   { "Seed",    FDF_VIRTUAL|FDF_INT|FDF_RI,             TURBULENCEFX_GET_Seed,    TURBULENCEFX_SET_Seed },
   { "Stitch",  FDF_VIRTUAL|FDF_INT|FDF_RI,             TURBULENCEFX_GET_Stitch,  TURBULENCEFX_SET_Stitch },
   { "Type",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RI,  TURBULENCEFX_GET_Type,    TURBULENCEFX_SET_Type, &clTurbulenceType },
   { "XMLDef",  FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, TURBULENCEFX_GET_XMLDef,  nullptr },
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
