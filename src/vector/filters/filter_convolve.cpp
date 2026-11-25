/*********************************************************************************************************************

-CLASS-
ConvolveFX: Applies a matrix convolution filter effect.

Convolve applies a matrix convolution filter effect to an input source.  A convolution combines pixels in the input
image with neighbouring pixels to produce a resulting image.  A wide variety of imaging operations can be achieved
through convolutions, including blurring, edge detection, sharpening, embossing and beveling.

A matrix convolution is based on an `n-by-m` matrix (the convolution kernel) which describes how a given pixel value in
the input image is combined with its neighbouring pixel values to produce a resulting pixel value. Each result pixel
is determined by applying the kernel matrix to the corresponding source pixel and its neighbouring pixels.  The basic
convolution formula which is applied to each colour value for a given pixel is:

<pre>
COLOUR(x,y) = (
     SUM I=0 to [MatrixRows-1] {
       SUM J=0 to [MatrixColumns-1] {
         SOURCE(x - TargetX + J, y - TargetY + I) * Matrix(MatrixColumns - J - 1, MatrixRows - I - 1)
       }
     }
   ) / Divisor + Bias * ALPHA(x,y)
</pre>

Colour values are premultiplied by alpha.  `ALPHA(x,y)` is the result alpha of the pixel.  When #PreserveAlpha is
FALSE, the alpha channel is computed with the same formula, adding Bias in place of `Bias * ALPHA(x,y)`.  When
#PreserveAlpha is TRUE, the result alpha is the source alpha, and the kernel is applied to unpremultiplied colour
values that are premultiplied again afterwards.

Note in the above formula that the values in the kernel matrix are applied such that the kernel matrix is rotated
180 degrees relative to the source and destination images in order to match convolution theory as described in many
computer graphics textbooks.

Convolution uses the device-pixel grid by default.  Set #UnitX and #UnitY to specify the kernel spacing in
primitive units.  When that grid differs from the device-pixel grid, the effect resamples the input for convolution
and then resamples the result back to device pixels.

-END-

When the image must be resampled to match the coordinate system defined by UnitX/Y prior to
convolution, or resampled to match the device coordinate system after convolution, it is recommended that
high quality viewers make use of appropriate interpolation techniques, for example bilinear or bicubic.
Depending on the speed of the available interpolants, this choice may be affected by the image-rendering
property setting. Note that implementations might choose approaches that minimize or eliminate resampling
when not necessary to produce proper results, such as when the document is zoomed out such that
UnitX/Y is considerably smaller than a device pixel.

*********************************************************************************************************************/

#include <array>
#include <bs_thread_pool.h>

constexpr int MAX_DIM = 9; // Maximum matrix dimension for convolution filter effects.

//********************************************************************************************************************

class extConvolveFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONVOLVEFX;
   static constexpr CSTRING CLASS_NAME = "ConvolveFX";
   using create = kt::Create<extConvolveFX>;

   double Bias = 0;
   double Divisor = 0;
   EM     EdgeMode = EM::DUPLICATE;
   int    MatrixRows = 3, MatrixColumns = 3;
   int    PreserveAlpha = false;
   int    TargetX = -1, TargetY = -1; // If -ve, the target will be computed as the centre of the matrix.
   double UnitX = 1, UnitY = 1;
   int    MatrixSize = 0;
   bool   MatrixProvided = false, DivisorProvided = false, TargetXProvided = false, TargetYProvided = false;
   bool   UnitXProvided = false, UnitYProvided = false;
   double Matrix[MAX_DIM * MAX_DIM] = {};

   extConvolveFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   FEM edge_policy() const {
      if (EdgeMode IS EM::DUPLICATE) return FEM::CLAMP;
      if (EdgeMode IS EM::WRAP) return FEM::REPEAT;
      return FEM::TRANSPARENT;
   }

   const uint8_t * getPixel(const objBitmap *Bitmap, int X, int Y) const {
      return filter_sampler(Bitmap).pixel(X, Y, edge_policy());
   }

   std::pair<double, double> unit_pixel_spacing() const {
      const auto &render = *Filter->Render;
      const auto [unit_x, unit_y] = render.primitive_scales(false);
      const auto [scale_x, scale_y] = render.axis_scales();
      return { UnitXProvided ? UnitX * unit_x * scale_x : 1.0, UnitYProvided ? UnitY * unit_y * scale_y : 1.0 };
   }

   filter_sampling sampling() const {
      const auto [x, y] = unit_pixel_spacing();
      if ((not std::isfinite(x)) or (not std::isfinite(y))) return { filter_access::BOUNDED, x, y };
      if (EdgeMode IS EM::WRAP) {
         return { filter_access::FULL_INPUT, filter_sampling::UNBOUNDED, filter_sampling::UNBOUNDED };
      }
      return { filter_access::BOUNDED, std::ceil(MatrixColumns * x) + 1, std::ceil(MatrixRows * y) + 1 };
   }

   // Writes one result pixel from the channel sums.  SVG defines the result as COLOUR = SUM / Divisor + Bias * ALPHA,
   // where ALPHA is the result alpha.  Scaling the colour bias by the result alpha applies Bias to the straight colour
   // values.  Without PreserveAlpha the sums are premultiplied, and colour is clamped to alpha so that the pixel
   // remains valid premultiplied data.  With PreserveAlpha the sums are straight colour, so Bias is added at full
   // scale and the output is straight colour with the source alpha.

   static void writePixel(uint8_t *Out, const uint8_t R, const uint8_t G, const uint8_t B, const uint8_t A,
      const double Factor, const double Bias, const bool PreserveAlpha, const double Red, const double Green,
      const double Blue, const double Alpha, const uint8_t SourceAlpha) {
      if (PreserveAlpha) {
         if (not SourceAlpha) {
            Out[R] = Out[G] = Out[B] = Out[A] = 0;
            return;
         }

         const double rgb_bias = Bias * 255.0;
         Out[R] = std::clamp(int(std::lrint((Factor * Red) + rgb_bias)), 0, 255);
         Out[G] = std::clamp(int(std::lrint((Factor * Green) + rgb_bias)), 0, 255);
         Out[B] = std::clamp(int(std::lrint((Factor * Blue) + rgb_bias)), 0, 255);
         Out[A] = SourceAlpha;
         return;
      }

      const int alpha = std::clamp(int(std::lrint((Factor * Alpha) + (Bias * 255.0))), 0, 255);
      const double rgb_bias = Bias * double(alpha);
      Out[R] = std::clamp(int(std::lrint((Factor * Red) + rgb_bias)), 0, alpha);
      Out[G] = std::clamp(int(std::lrint((Factor * Green) + rgb_bias)), 0, alpha);
      Out[B] = std::clamp(int(std::lrint((Factor * Blue) + rgb_bias)), 0, alpha);
      Out[A] = alpha;
   }

   // Converts a premultiplied RGBA8 grid to straight colour in place.  Transparent pixels have no colour.

   static void unpremultiplyGrid(uint8_t *Grid, size_t Pixels, const objBitmap *Format) {
      const uint8_t A = Format->ColourFormat->AlphaPos>>3;
      const uint8_t R = Format->ColourFormat->RedPos>>3;
      const uint8_t G = Format->ColourFormat->GreenPos>>3;
      const uint8_t B = Format->ColourFormat->BluePos>>3;
      for (size_t i=0; i < Pixels; i++, Grid += 4) {
         const int alpha = Grid[A];
         for (auto c : { R, G, B }) {
            Grid[c] = alpha ? uint8_t(std::min(255, ((Grid[c] * 255) + (alpha>>1)) / alpha)) : 0;
         }
      }
   }

   // Converts a straight RGBA8 grid to premultiplied colour in place.

   static void premultiplyGrid(uint8_t *Grid, size_t Pixels, const objBitmap *Format) {
      const uint8_t A = Format->ColourFormat->AlphaPos>>3;
      const uint8_t R = Format->ColourFormat->RedPos>>3;
      const uint8_t G = Format->ColourFormat->GreenPos>>3;
      const uint8_t B = Format->ColourFormat->BluePos>>3;
      for (size_t i=0; i < Pixels; i++, Grid += 4) {
         const int alpha = Grid[A];
         for (auto c : { R, G, B }) Grid[c] = uint8_t(((Grid[c] * alpha) + 127) / 255);
      }
   }

   // Convolves a grid in the working colour space.  The grid holds premultiplied colour, or straight colour when
   // PreserveAlpha is set, and the output uses the same representation.

   void convolveGrid(const uint8_t *Input, uint8_t *Output, int Width, int Height, const objBitmap *SourceBitmap) {
      const uint8_t A = SourceBitmap->ColourFormat->AlphaPos>>3;
      const uint8_t R = SourceBitmap->ColourFormat->RedPos>>3;
      const uint8_t G = SourceBitmap->ColourFormat->GreenPos>>3;
      const uint8_t B = SourceBitmap->ColourFormat->BluePos>>3;

      const double factor = 1.0 / Divisor;
      const double bias = Bias;
      const bool preserve_alpha = PreserveAlpha;

      const filter_rgba_sampler grid { Input, Width * 4, 0, 0, Width, Height };
      const auto edge = edge_policy();

      for (int y=0; y < Height; y++) {
         for (int x=0; x < Width; x++) {
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            auto source_alpha = Input[((y * Width) + x) * 4 + A];

            for (int fy=0; fy < MatrixRows; fy++) {
               int isrc_y = y - TargetY + fy;
               for (int fx=0; fx < MatrixColumns; fx++) {
                  int isrc_x = x - TargetX + fx;
                  int matrix_index = ((MatrixRows - fy - 1) * MatrixColumns) + (MatrixColumns - fx - 1);
                  if (auto pixel = grid.pixel(isrc_x, isrc_y, edge)) {
                     r += pixel[R] * Matrix[matrix_index];
                     g += pixel[G] * Matrix[matrix_index];
                     b += pixel[B] * Matrix[matrix_index];
                     a += pixel[A] * Matrix[matrix_index];
                  }
               }
            }

            auto out = Output + ((y * Width) + x) * 4;
            writePixel(out, R, G, B, A, factor, bias, preserve_alpha, r, g, b, a, source_alpha);
         }
      }
   }

   ERR processResampled(const objBitmap *InputBitmap, uint8_t *Output, int pLeft, int pTop, int pRight, int pBottom,
      double UnitXPixel, double UnitYPixel) {

      const int width = pRight - pLeft;
      const int height = pBottom - pTop;
      if ((width <= 0) or (height <= 0)) return ERR::Okay;

      const double grid_width_f = double(width) / UnitXPixel;
      const double grid_height_f = double(height) / UnitYPixel;
      if ((!std::isfinite(grid_width_f)) or (!std::isfinite(grid_height_f)) or
          (grid_width_f > 4096.0 * 4096.0) or (grid_height_f > 4096.0 * 4096.0)) {
         return ERR::OutOfRange;
      }

      const int grid_width = std::max(1, int(std::lrint(grid_width_f)));
      const int grid_height = std::max(1, int(std::lrint(grid_height_f)));
      const size_t grid_pixels = size_t(grid_width) * size_t(grid_height);
      if (grid_pixels > size_t(4096) * size_t(4096)) return ERR::OutOfRange;

      uint8_t *grid_in, *grid_out;
      if (auto error = get_filter_scratch(this, grid_pixels * 4, grid_in); error != ERR::Okay) return error;
      if (auto error = get_filter_scratch(this, grid_pixels * 4, grid_out); error != ERR::Okay) return error;

      const auto source = filter_sampler(InputBitmap);
      const double source_scale_x = double(width) / double(grid_width);
      const double source_scale_y = double(height) / double(grid_height);
      for (int y=0; y < grid_height; y++) {
         const double sy = double(pTop) + (filter_pixel_centre(y) * source_scale_y) - 0.5;
         for (int x=0; x < grid_width; x++) {
            const double sx = double(pLeft) + (filter_pixel_centre(x) * source_scale_x) - 0.5;
            source.sample<filter_interpolation::BILINEAR, FEM::CLAMP>(
               grid_in + ((y * grid_width) + x) * 4, sx, sy);
         }
      }

      // Sampling uses premultiplied colour so that transparent pixels do not bleed.  PreserveAlpha convolves
      // straight colour, so the grid is unpremultiplied before the kernel is applied and premultiplied again
      // before it is resampled to device pixels.

      if (PreserveAlpha) unpremultiplyGrid(grid_in, grid_pixels, InputBitmap);
      convolveGrid(grid_in, grid_out, grid_width, grid_height, InputBitmap);
      if (PreserveAlpha) premultiplyGrid(grid_out, grid_pixels, InputBitmap);

      const filter_rgba_sampler grid { grid_out, grid_width * 4, 0, 0, grid_width, grid_height };
      const double grid_scale_x = double(grid_width) / double(width);
      const double grid_scale_y = double(grid_height) / double(height);
      const uint8_t A = InputBitmap->ColourFormat->AlphaPos>>3;
      const uint8_t R = InputBitmap->ColourFormat->RedPos>>3;
      const uint8_t G = InputBitmap->ColourFormat->GreenPos>>3;
      const uint8_t B = InputBitmap->ColourFormat->BluePos>>3;
      uint8_t *outline = Output;
      const uint8_t *alpha_input = InputBitmap->Data + (pTop * InputBitmap->LineWidth);
      for (int y=0; y < height; y++) {
         auto out = outline;
         const double gy = (filter_pixel_centre(y) * grid_scale_y) - 0.5;
         for (int x=0; x < width; x++) {
            const double gx = (filter_pixel_centre(x) * grid_scale_x) - 0.5;
            grid.sample<filter_interpolation::BILINEAR, FEM::CLAMP>(out, gx, gy);
            if (PreserveAlpha) {
               // Restore the exact source alpha and keep fully transparent pixels black.
               const auto source_alpha = (alpha_input + ((pLeft + x) * InputBitmap->BytesPerPixel))[A];
               const int sampled = source_alpha ? out[A] : 0;
               for (auto c : { R, G, B }) {
                  out[c] = sampled ? uint8_t(std::min(255, ((out[c] * 255) + (sampled>>1)) / sampled)) : 0;
               }
               out[A] = source_alpha;
            }
            out += 4;
         }
         alpha_input += InputBitmap->LineWidth;
         outline += width * 4;
      }

      return ERR::Okay;
   }

   // Standard algorithm that uses edge detection at the borders (see getPixel()).
   //
   // Interior pixels, where the entire kernel is within the clip region, take a fast path with direct pixel
   // addressing and (where available) SSE2 accumulation of all four channels in packed-double lanes.  Border
   // pixels retain the per-tap getPixel() edge handling.  Both paths accumulate taps in the same order with
   // the same double-precision operations, so the output is bit-identical to the original implementation.

   void processClipped(const objBitmap *InputBitmap, uint8_t *output, int pLeft, int pTop, int pRight, int pBottom,
      double UnitXPixel, double UnitYPixel) {
      if ((pRight <= pLeft) or (pBottom <= pTop)) return;

      const uint8_t A = InputBitmap->ColourFormat->AlphaPos>>3;
      const uint8_t R = InputBitmap->ColourFormat->RedPos>>3;
      const uint8_t G = InputBitmap->ColourFormat->GreenPos>>3;
      const uint8_t B = InputBitmap->ColourFormat->BluePos>>3;

      const double factor = 1.0 / Divisor;
      const double bias = Bias;
      const bool preserve_alpha = PreserveAlpha;
      const int stride = InputBitmap->LineWidth;
      const int out_stride = (Target->Clip.Right - Target->Clip.Left) * Target->BytesPerPixel;

      // The kernel is rotated 180 degrees as per convolution theory; hoist the index reversal out of the
      // inner loops by reordering the weights to match ascending (fy,fx) tap order.

      double weights[MAX_DIM * MAX_DIM];
      for (int fy=0; fy < MatrixRows; fy++) {
         for (int fx=0; fx < MatrixColumns; fx++) {
            weights[(fy * MatrixColumns) + fx] = Matrix[((MatrixRows - fy - 1) * MatrixColumns) + (MatrixColumns - fx - 1)];
         }
      }

      // The fast path requires the implicit pixel grid to map 1:1 so that tap coordinates are exact integers.
      // Interior column range: every tap x - TargetX + fx must fall within the horizontal clip.

      const bool unit_grid = (UnitXPixel IS 1.0) and (UnitYPixel IS 1.0);
      const int x_lo = InputBitmap->Clip.Left + TargetX;
      const int x_hi = InputBitmap->Clip.Right - MatrixColumns + TargetX;

      const uint8_t *alpha_input = InputBitmap->Data + (pTop * stride);
      uint8_t *outline = output;
      for (int y=pTop; y < pBottom; y++) {
         uint8_t *out = outline;
         const bool y_interior = unit_grid and (y - TargetY >= InputBitmap->Clip.Top) and
            (y - TargetY + MatrixRows <= InputBitmap->Clip.Bottom);

         for (int x=pLeft; x < pRight; x++) {
            double r, g, b, a;
            auto source_alpha = (alpha_input + (x * InputBitmap->BytesPerPixel))[A];

            if (y_interior and (x >= x_lo) and (x <= x_hi)) {
               const uint8_t *base = InputBitmap->Data + ((y - TargetY) * stride) + ((x - TargetX) * 4);
               double sums[4]; // Channel sums indexed by byte position within the pixel.
               const double *w = weights;
#ifdef FILTER_SSE2
               auto acc01 = _mm_setzero_pd();
               auto acc23 = _mm_setzero_pd();
               for (int fy=0; fy < MatrixRows; fy++) {
                  auto pix = base + (fy * stride);
                  for (int fx=0; fx < MatrixColumns; fx++, pix += 4) {
                     uint32_t px;
                     memcpy(&px, pix, 4);
                     auto bytes = _mm_cvtsi32_si128(int(px));
                     bytes = _mm_unpacklo_epi8(bytes, _mm_setzero_si128());
                     bytes = _mm_unpacklo_epi16(bytes, _mm_setzero_si128());
                     const auto wv = _mm_set1_pd(*w++);
                     acc01 = _mm_add_pd(acc01, _mm_mul_pd(_mm_cvtepi32_pd(bytes), wv));
                     acc23 = _mm_add_pd(acc23, _mm_mul_pd(_mm_cvtepi32_pd(_mm_shuffle_epi32(bytes, _MM_SHUFFLE(1, 0, 3, 2))), wv));
                  }
               }
               _mm_storeu_pd(sums, acc01);
               _mm_storeu_pd(sums + 2, acc23);
#else
               sums[0] = sums[1] = sums[2] = sums[3] = 0.0;
               for (int fy=0; fy < MatrixRows; fy++) {
                  auto pix = base + (fy * stride);
                  for (int fx=0; fx < MatrixColumns; fx++, pix += 4) {
                     const double wt = *w++;
                     sums[0] += pix[0] * wt;
                     sums[1] += pix[1] * wt;
                     sums[2] += pix[2] * wt;
                     sums[3] += pix[3] * wt;
                  }
               }
#endif
               r = sums[R]; g = sums[G]; b = sums[B]; a = sums[A];
            }
            else {
               r = g = b = a = 0.0;
               for (int fy = 0; fy < MatrixRows; fy++) {
                  int isrc_y = std::lrint(y - (TargetY * UnitYPixel) + (fy * UnitYPixel));
                  for (int fx = 0; fx < MatrixColumns; fx++) {
                     int isrc_x = std::lrint(x - (TargetX * UnitXPixel) + (fx * UnitXPixel));
                     if (auto pixel = getPixel(InputBitmap, isrc_x, isrc_y)) {
                        const double wt = weights[(fy * MatrixColumns) + fx];
                        r += pixel[R] * wt;
                        g += pixel[G] * wt;
                        b += pixel[B] * wt;
                        a += pixel[A] * wt;
                     }
                  }
               }
            }

            writePixel(out, R, G, B, A, factor, bias, preserve_alpha, r, g, b, a, source_alpha);
            out += 4;
         }
         alpha_input += stride;
         outline += out_stride;
      }
   }
};

//********************************************************************************************************************

static ERR CONVOLVEFX_Draw(extConvolveFX *Self, struct acDraw *Args)
{
   if (Self->Target->BytesPerPixel != 4) return ERR::InvalidValue;

   const int canvas_width  = Self->Target->Clip.Right - Self->Target->Clip.Left;
   const int canvas_height = Self->Target->Clip.Bottom - Self->Target->Clip.Top;

   if (canvas_width * canvas_height > 4096 * 4096) return ERR::OutOfRange; // Bail on really large bitmaps.

   filter_surface_view source;
   if (auto error = get_source_view(Self, source, filter_input_role::INPUT); error != ERR::Okay) return error;

   auto [unit_x_pixel, unit_y_pixel] = Self->unit_pixel_spacing();
   if ((not std::isfinite(unit_x_pixel)) or (not std::isfinite(unit_y_pixel)) or
       (unit_x_pixel <= 0.0) or (unit_y_pixel <= 0.0)) return ERR::InvalidValue;

   if (std::abs(unit_x_pixel - 1.0) < 0.001) unit_x_pixel = 1.0;
   if (std::abs(unit_y_pixel - 1.0) < 0.001) unit_y_pixel = 1.0;

   // PreserveAlpha convolves straight colour.  The resampled path takes premultiplied input in all cases because it
   // must interpolate before it can unpremultiply.

   const bool resampled = (unit_x_pixel != 1.0) or (unit_y_pixel != 1.0);
   const auto input_alpha = (Self->PreserveAlpha and (not resampled)) ? filter_alpha::STRAIGHT :
      filter_alpha::PREMULTIPLIED;
   filter_surface_view input;
   const auto encoding = (effect_colour_space(Self) IS VCS::LINEAR_RGB) ? CS::LINEAR_RGB : CS::SRGB;
   if (auto error = working_input(Self, source, encoding, input_alpha, input); error != ERR::Okay) return error;
   auto in_bmp = input.Bitmap;
   auto output = Self->Target->Data + (Self->Target->Clip.Top * Self->Target->LineWidth) +
      Self->Target->Clip.Left * Self->Target->BytesPerPixel;

   if (resampled) {
      auto error = Self->processResampled(in_bmp, output, Self->Target->Clip.Left, Self->Target->Clip.Top,
         Self->Target->Clip.Right, Self->Target->Clip.Bottom, unit_x_pixel, unit_y_pixel);
      if (error != ERR::Okay) return error;
   }
   else {
      int thread_count = std::thread::hardware_concurrency();
      if (thread_count < 1) thread_count = 1;
      if (canvas_height < 4) thread_count = 1; // Prevent division issues with small images
      else if (thread_count > canvas_height / 4) thread_count = canvas_height / 4;

      BS::thread_pool pool(thread_count);

      int lines_per_thread = canvas_height / thread_count;
      for (int i=0; i < thread_count; i++) {
         int top = Self->Target->Clip.Top + (i * lines_per_thread);
         int bottom = (i IS thread_count - 1) ? Self->Target->Clip.Bottom : top + lines_per_thread;

         // Calculate output offset for each thread to prevent race conditions
         int output_offset = i * lines_per_thread * canvas_width * Self->Target->BytesPerPixel;
         auto thread_output = output + output_offset;

         const auto left = Self->Target->Clip.Left, right = Self->Target->Clip.Right;
         pool.detach_task([Self, in_bmp, thread_output, unit_x_pixel, unit_y_pixel, left, top, right, bottom]() {
            Self->processClipped(in_bmp, thread_output, left, top, right, bottom, unit_x_pixel, unit_y_pixel);
         });
      }

      pool.wait();
   }

   // The pipeline expects straight sRGB output from each effect.  PreserveAlpha output is already straight.

   if (not Self->PreserveAlpha) {
      Self->Target->Flags |= BMF::PREMUL;
      if (auto error = Self->Target->demultiply(); error != ERR::Okay) return error;
   }
   if (encoding IS CS::LINEAR_RGB) {
      Self->Target->ColourSpace = CS::LINEAR_RGB;
      return Self->Target->convertToRGB();
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CONVOLVEFX_Init(extConvolveFX *Self)
{
   kt::Log log;

   if ((Self->UnitX <= 0.0) or (Self->UnitY <= 0.0)) {
      log.warning("UnitX and UnitY must be positive values.");
      return ERR::InvalidValue;
   }

   if (!Self->MatrixProvided) {
      log.warning("The Matrix field must be set before initialising ConvolveFX.");
      return ERR::FieldNotSet;
   }

   if (Self->MatrixColumns * Self->MatrixRows > MAX_DIM * MAX_DIM) {
      log.warning("Size of matrix exceeds internally imposed limits.");
      return ERR::BufferOverflow;
   }

   const int filter_size = Self->MatrixColumns * Self->MatrixRows;

   if (Self->MatrixSize != filter_size) {
      log.warning("Matrix size of %d does not match the filter size of %dx%d", Self->MatrixSize, Self->MatrixColumns, Self->MatrixRows);
      return ERR::Mismatch;
   }

   // Use client-provided tx/ty values, otherwise default according to the SVG standard.

   if (Self->TargetXProvided and ((Self->TargetX < 0) or (Self->TargetX >= Self->MatrixColumns))) {
      log.warning("TargetX value of %d is outside the matrix width of %d.", Self->TargetX, Self->MatrixColumns);
      return ERR::OutOfRange;
   }

   if (!Self->TargetXProvided) {
      Self->TargetX = Self->MatrixColumns>>1;
   }

   if (Self->TargetYProvided and ((Self->TargetY < 0) or (Self->TargetY >= Self->MatrixRows))) {
      log.warning("TargetY value of %d is outside the matrix height of %d.", Self->TargetY, Self->MatrixRows);
      return ERR::OutOfRange;
   }

   if (!Self->TargetYProvided) {
      Self->TargetY = Self->MatrixRows>>1;
   }

   if (!Self->Divisor) {
      double divisor = 0;
      for (int i=0; i < filter_size; i++) divisor += Self->Matrix[i];
      if (!divisor) divisor = 1;
      Self->Divisor = divisor;
   }

   log.trace("Convolve Size: (%d,%d), Divisor: %g, Bias: %g", Self->MatrixColumns, Self->MatrixRows, Self->Divisor, Self->Bias);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Bias: Used to adjust the final result of each computed RGB value.

After applying the #Matrix to the input image to yield a number and applying the #Divisor, the Bias value is added to
each component.  One application of Bias is when it is desirable to have .5 grey value be the zero response of the
filter.  The Bias value shifts the range of the filter.  This allows representation of values that would otherwise be
clamped to 0 or 1.  The default is 0.

Bias is added to the alpha channel unless #PreserveAlpha is TRUE.  The colour channels receive Bias multiplied by the
result alpha, which is equivalent to adding Bias to the straight colour values.  For example, a Bias of 1 with
#PreserveAlpha disabled produces opaque white wherever the convolved alpha is not negative.

-FIELD-
Divisor: Defines the divisor value in the convolution algorithm.

After applying the #Matrix to the input image to yield a number, that number is divided by #Divisor to yield the
final destination colour value.  A divisor that is the sum of all the matrix values tends to have an evening effect
on the overall colour intensity of the result.  The default value is the sum of all values in #Matrix, with the
exception that if the sum is zero, then the divisor is set to `1`.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_Divisor(extConvolveFX *Self, double Value)
{
   kt::Log log;
   if (!Value) return log.warning(ERR::InvalidValue);
   Self->Divisor = Value;
   Self->DivisorProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EdgeMode: Defines the behaviour of the convolve algorithm around the edges of the input image.

The EdgeMode determines how to extend the input image with colour values so that the matrix operations can be applied
when the #Matrix is positioned at or near the edge of the input image.

-FIELD-
Matrix: A list of numbers that make up the kernel matrix for the convolution.

A list of numbers that make up the kernel matrix for the convolution.  The number of entries in the list must equal
`MatrixColumns * MatrixRows`.

*********************************************************************************************************************/

static ERR CONVOLVEFX_GET_Matrix(extConvolveFX *Self, std::span<double> &Array)
{
   Array = std::span<double>(Self->Matrix, Self->MatrixSize);
   return ERR::Okay;
}

static ERR CONVOLVEFX_SET_Matrix(extConvolveFX *Self, std::span<const double> &Array)
{
   if ((Array.size() > 0) and (std::ssize(Array) <= std::ssize(Self->Matrix))) {
      Self->MatrixSize = Array.size();
      Self->MatrixProvided = true;
      copymem(Array.data(), Self->Matrix, sizeof(double) * Array.size());
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
MatrixRows: The number of rows in the Matrix.

Indicates the number of rows represented in #Matrix.  A typical value is `3`.  It is recommended that only small
values are used; higher values may result in very high CPU overhead and usually do not produce results that justify
the impact on performance.  The default value is 3.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_MatrixRows(extConvolveFX *Self, int Value)
{
   if (Value <= 0) return ERR::InvalidValue;
   Self->MatrixRows = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MatrixColumns: The number of columns in the Matrix.

Indicates the number of columns represented in #Matrix.  A typical value is `3`.  It is recommended that only small
values are used; higher values may result in very high CPU overhead and usually do not produce results that justify
the impact on performance.  The default value is `3`.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_MatrixColumns(extConvolveFX *Self, int Value)
{
   if (Value <= 0) return ERR::InvalidValue;
   Self->MatrixColumns = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PreserveAlpha: If TRUE, the alpha channel is protected from the effects of the convolve algorithm.

When PreserveAlpha is TRUE, the kernel is applied only to the colour channels.  The colour values are unpremultiplied
before the kernel is applied, so translucent pixels contribute their full colour, and Bias is added to the
unpremultiplied colour.  The result alpha is the source alpha.  When FALSE, the default, all four channels are
convolved as premultiplied values.

-FIELD-
TargetX: The X position of the matrix in relation to the input image.

Determines the positioning in X of the convolution matrix relative to a given target pixel in the input image.  The
left-most column of the matrix is column number zero.  The value must be such that `0 &lt;= TargetX &lt; MatrixColumns`.  By
default, the convolution matrix is centered in X over each pixel of the input image, i.e.
`TargetX = floor(MatrixColumns / 2)`.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_TargetX(extConvolveFX *Self, int Value)
{
   if (Self->initialised()) {
      if ((Value < 0) or (Value >= Self->MatrixColumns)) return kt::Log().warning(ERR::OutOfRange);
   }

   Self->TargetX = Value;
   Self->TargetXProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TargetY: The Y position of the matrix in relation to the input image.

Determines the positioning in Y of the convolution matrix relative to a given target pixel in the input image.  The
left-most column of the matrix is column number zero.  The value must be such that `0 &lt;= TargetY &lt; MatrixRows`.  By
default, the convolution matrix is centered in Y over each pixel of the input image, i.e.
`TargetY = floor(MatrixRows / 2)`.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_TargetY(extConvolveFX *Self, int Value)
{
   if (Self->initialised()) {
      if ((Value < 0) or (Value >= Self->MatrixRows)) return kt::Log().warning(ERR::OutOfRange);
   }

   Self->TargetY = Value;
   Self->TargetYProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
UnitX: The distance between successive columns of the Matrix.

This spacing uses @VectorFilter.PrimitiveUnits.  It lets the kernel scale with the client.  If omitted, spacing
on this axis defaults to one device pixel.

The most consistent results and the fastest performance will be achieved if the pixel grid of the offscreen bitmap
aligns with the pixel grid of the kernel.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_UnitX(extConvolveFX *Self, double Value)
{
   if (Value <= 0.0) return ERR::InvalidValue;

   Self->UnitX = Value;
   Self->UnitXProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
UnitY: The distance between successive rows of the Matrix.

This spacing uses @VectorFilter.PrimitiveUnits.  It lets the kernel scale with the client.  If omitted, spacing
on this axis defaults to one device pixel.

The most consistent results and the fastest performance will be achieved if the pixel grid of the offscreen bitmap
aligns with the pixel grid of the kernel.

*********************************************************************************************************************/

static ERR CONVOLVEFX_SET_UnitY(extConvolveFX *Self, double Value)
{
   if (Value <= 0.0) return ERR::InvalidValue;

   Self->UnitY = Value;
   Self->UnitYProvided = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR CONVOLVEFX_GET_XMLDef(extConvolveFX *Self, std::string &Value)
{
   std::stringstream stream;

   stream << "feConvolveMatrix";

   if ((Self->MatrixColumns != 3) or (Self->MatrixRows != 3)) {
      stream << " order=\"";
      if (Self->MatrixColumns IS Self->MatrixRows) stream << Self->MatrixColumns;
      else stream << Self->MatrixColumns << " " << Self->MatrixRows;
      stream << "\"";
   }

   if (Self->MatrixProvided) {
      stream << " kernelMatrix=\"";
      for (int i=0; i < Self->MatrixSize; i++) {
         if (i) stream << " ";
         stream << Self->Matrix[i];
      }
      stream << "\"";
   }

   double default_divisor = 0.0;
   for (int i=0; i < Self->MatrixSize; i++) default_divisor += Self->Matrix[i];
   if (!default_divisor) default_divisor = 1.0;

   if (Self->DivisorProvided and (std::abs(Self->Divisor - default_divisor) > 1e-12)) {
      stream << " divisor=\"" << Self->Divisor << "\"";
   }

   if (Self->Bias != 0.0) stream << " bias=\"" << Self->Bias << "\"";
   if (Self->TargetXProvided and (Self->TargetX != (Self->MatrixColumns>>1))) {
      stream << " targetX=\"" << Self->TargetX << "\"";
   }

   if (Self->TargetYProvided and (Self->TargetY != (Self->MatrixRows>>1))) {
      stream << " targetY=\"" << Self->TargetY << "\"";
   }

   switch (Self->EdgeMode) {
      case EM::WRAP:
         stream << " edgeMode=\"wrap\"";
         break;

      case EM::NONE:
         stream << " edgeMode=\"none\"";
         break;

      default:
         break;
   }

   if (Self->UnitXProvided or Self->UnitYProvided) {
      stream << " kernelUnitLength=\"" << Self->UnitX;
      if (Self->UnitX != Self->UnitY) stream << " " << Self->UnitY;
      stream << "\"";
   }

   if (Self->PreserveAlpha) stream << " preserveAlpha=\"true\"";

   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_convolve_def.c"

//********************************************************************************************************************
// Writable fields that would otherwise be written directly report their changes, so that buffered viewports redraw
// the filter's clients.

static ERR CONVOLVEFX_SET_PreserveAlpha(extConvolveFX *Self, int Value)
{
   Self->PreserveAlpha = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldArray clConvolveFXFields[] = {
   { "Bias",          FDF_DOUBLE|FDF_RI },
   { "Divisor",       FDF_DOUBLE|FDF_RI,         nullptr, CONVOLVEFX_SET_Divisor },
   { "EdgeMode",      FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clConvolveFXEM },
   { "MatrixRows",    FDF_INT|FDF_RI,            nullptr, CONVOLVEFX_SET_MatrixRows },
   { "MatrixColumns", FDF_INT|FDF_RI,            nullptr, CONVOLVEFX_SET_MatrixColumns },
   { "Matrix",        FDF_VIRTUAL|FDF_DOUBLE|FDF_ARRAY|FDF_RI|FDF_PURE, CONVOLVEFX_GET_Matrix, CONVOLVEFX_SET_Matrix },
   { "PreserveAlpha", FDF_INT|FDF_RW, nullptr, CONVOLVEFX_SET_PreserveAlpha },
   { "TargetX",       FDF_INT|FDF_RI,    nullptr, CONVOLVEFX_SET_TargetX },
   { "TargetY",       FDF_INT|FDF_RI,    nullptr, CONVOLVEFX_SET_TargetY },
   { "UnitX",         FDF_DOUBLE|FDF_RI, nullptr, CONVOLVEFX_SET_UnitX },
   { "UnitY",         FDF_DOUBLE|FDF_RI, nullptr, CONVOLVEFX_SET_UnitY },
   { "XMLDef",        FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE,  CONVOLVEFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_convolvefx(void)
{
   clConvolveFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::CONVOLVEFX),
      fl::Name("ConvolveFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clConvolveFXActions),
      fl::Fields(clConvolveFXFields),
      fl::Size(sizeof(extConvolveFX)),
      fl::Path(MOD_PATH));

   return clConvolveFX ? ERR::Okay : ERR::AddClass;
}
