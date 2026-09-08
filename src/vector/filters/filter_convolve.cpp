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

Note in the above formula that the values in the kernel matrix are applied such that the kernel matrix is rotated
180 degrees relative to the source and destination images in order to match convolution theory as described in many
computer graphics textbooks.

Because they operate on pixels, matrix convolutions are inherently resolution-dependent.  To make
resolution-independent results, an explicit value should be provided for either the `ResX` and `ResY` attributes
on the parent @VectorFilter and/or #UnitX and #UnitY.

-END-

TODO: As per the SVG spec...

Because they operate on pixels, matrix convolutions are inherently resolution-dependent.  To make convolve produce
resolution-independent results, an explicit value should be provided for either the ResX/Y on the filter element
and/or UnitX/Y.

UnitX/Y, in combination with the other attributes, defines an implicit pixel grid in the filter
effects coordinate system (i.e., the coordinate system established by the primitiveUnits attribute).
If the pixel grid established by UnitX/Y is not scaled to match the pixel grid established by
attribute filterRes (implicitly or explicitly), then the input image will be temporarily rescaled to
match its pixels with UnitX/Y. The convolution happens on the resampled image. After applying
the convolution, the image is resampled back to the original resolution.

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

   inline uint8_t * getPixel(objBitmap *Bitmap, int X, int Y) const {
      if ((X >= Bitmap->Clip.Left) and (X < Bitmap->Clip.Right) and
          (Y >= Bitmap->Clip.Top) and (Y < Bitmap->Clip.Bottom)) {
         return Bitmap->Data + (Y * Bitmap->LineWidth) + (X * Bitmap->BytesPerPixel);
      }

      switch (EdgeMode) {
         case EM::DUPLICATE:
            if (X < Bitmap->Clip.Left) X = Bitmap->Clip.Left;
            else if (X >= Bitmap->Clip.Right) X = Bitmap->Clip.Right - 1;
            if (Y < Bitmap->Clip.Top) Y = Bitmap->Clip.Top;
            else if (Y >= Bitmap->Clip.Bottom) Y = Bitmap->Clip.Bottom - 1;
            return Bitmap->Data + (Y * Bitmap->LineWidth) + (X * Bitmap->BytesPerPixel);

         case EM::WRAP: {
            auto width = Bitmap->Clip.Right - Bitmap->Clip.Left;
            auto height = Bitmap->Clip.Bottom - Bitmap->Clip.Top;

            X -= Bitmap->Clip.Left;
            Y -= Bitmap->Clip.Top;

            if (X < 0) X = ((X % width) + width) % width;
            else if (X >= width) X %= width;

            if (Y < 0) Y = ((Y % height) + height) % height;
            else if (Y >= height) Y %= height;

            return Bitmap->Data + ((Y + Bitmap->Clip.Top)  * Bitmap->LineWidth) + ((X + Bitmap->Clip.Left) * Bitmap->BytesPerPixel);
         }

         default: return nullptr;
      }
   }

   inline double unitPixelSpacingX() const {
      if (!UnitXProvided) return 1.0;

      double scale_x = 1.0;
      if (Filter->ClientVector) {
         auto &t = Filter->ClientVector->Transform;
         scale_x = sqrt((t.sx * t.sx) + (t.shy * t.shy));
         if (scale_x <= 0.0) scale_x = 1.0;
      }

      if (Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) return UnitX * Filter->BoundWidth * scale_x;
      else return UnitX * scale_x;
   }

   inline double unitPixelSpacingY() const {
      if (!UnitYProvided) return 1.0;

      double scale_y = 1.0;
      if (Filter->ClientVector) {
         auto &t = Filter->ClientVector->Transform;
         scale_y = sqrt((t.shx * t.shx) + (t.sy * t.sy));
         if (scale_y <= 0.0) scale_y = 1.0;
      }

      if (Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) return UnitY * Filter->BoundHeight * scale_y;
      else return UnitY * scale_y;
   }

   inline const uint8_t * getClampedBitmapPixel(objBitmap *Bitmap, int X, int Y) const {
      X = std::clamp(X, Bitmap->Clip.Left, Bitmap->Clip.Right - 1);
      Y = std::clamp(Y, Bitmap->Clip.Top, Bitmap->Clip.Bottom - 1);
      return Bitmap->Data + (Y * Bitmap->LineWidth) + (X * Bitmap->BytesPerPixel);
   }

   void sampleBitmap(objBitmap *Bitmap, double X, double Y, uint8_t *Output) const {
      const int x0 = int(std::floor(X));
      const int y0 = int(std::floor(Y));
      const int x1 = x0 + 1;
      const int y1 = y0 + 1;
      const double tx = X - double(x0);
      const double ty = Y - double(y0);

      auto p00 = getClampedBitmapPixel(Bitmap, x0, y0);
      auto p10 = getClampedBitmapPixel(Bitmap, x1, y0);
      auto p01 = getClampedBitmapPixel(Bitmap, x0, y1);
      auto p11 = getClampedBitmapPixel(Bitmap, x1, y1);

      for (int i=0; i < Bitmap->BytesPerPixel; i++) {
         const double top = (double(p00[i]) * (1.0 - tx)) + (double(p10[i]) * tx);
         const double bottom = (double(p01[i]) * (1.0 - tx)) + (double(p11[i]) * tx);
         Output[i] = uint8_t(std::clamp(int(std::lrint((top * (1.0 - ty)) + (bottom * ty))), 0, 255));
      }
   }

   inline uint8_t * getGridPixel(uint8_t *Data, int Width, int Height, int X, int Y) const {
      if ((X >= 0) and (X < Width) and (Y >= 0) and (Y < Height)) return Data + ((Y * Width) + X) * 4;

      switch (EdgeMode) {
         case EM::DUPLICATE:
            X = std::clamp(X, 0, Width - 1);
            Y = std::clamp(Y, 0, Height - 1);
            return Data + ((Y * Width) + X) * 4;

         case EM::WRAP:
            if (X < 0) X = ((X % Width) + Width) % Width;
            else if (X >= Width) X %= Width;
            if (Y < 0) Y = ((Y % Height) + Height) % Height;
            else if (Y >= Height) Y %= Height;
            return Data + ((Y * Width) + X) * 4;

         default:
            return nullptr;
      }
   }

   inline const uint8_t * getClampedGridPixel(const uint8_t *Data, int Width, int Height, int X, int Y) const {
      X = std::clamp(X, 0, Width - 1);
      Y = std::clamp(Y, 0, Height - 1);
      return Data + ((Y * Width) + X) * 4;
   }

   void sampleGrid(const uint8_t *Data, int Width, int Height, double X, double Y, uint8_t *Output) const {
      const int x0 = int(std::floor(X));
      const int y0 = int(std::floor(Y));
      const int x1 = x0 + 1;
      const int y1 = y0 + 1;
      const double tx = X - double(x0);
      const double ty = Y - double(y0);

      auto p00 = getClampedGridPixel(Data, Width, Height, x0, y0);
      auto p10 = getClampedGridPixel(Data, Width, Height, x1, y0);
      auto p01 = getClampedGridPixel(Data, Width, Height, x0, y1);
      auto p11 = getClampedGridPixel(Data, Width, Height, x1, y1);

      for (int i=0; i < 4; i++) {
         const double top = (double(p00[i]) * (1.0 - tx)) + (double(p10[i]) * tx);
         const double bottom = (double(p01[i]) * (1.0 - tx)) + (double(p11[i]) * tx);
         Output[i] = uint8_t(std::clamp(int(std::lrint((top * (1.0 - ty)) + (bottom * ty))), 0, 255));
      }
   }

   void convolveGrid(const uint8_t *Input, uint8_t *Output, int Width, int Height, objBitmap *SourceBitmap) {
      const uint8_t A = SourceBitmap->ColourFormat->AlphaPos>>3;
      const uint8_t R = SourceBitmap->ColourFormat->RedPos>>3;
      const uint8_t G = SourceBitmap->ColourFormat->GreenPos>>3;
      const uint8_t B = SourceBitmap->ColourFormat->BluePos>>3;

      const double factor = 1.0 / Divisor;
      const double alpha_bias = Bias * 255.0;
      const bool linear_rgb = Filter->ColourSpace IS VCS::LINEAR_RGB;

      auto grid = (uint8_t *)Input;

      for (int y=0; y < Height; y++) {
         for (int x=0; x < Width; x++) {
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            auto source_alpha = Input[((y * Width) + x) * 4 + A];

            for (int fy=0; fy < MatrixRows; fy++) {
               int isrc_y = y - TargetY + fy;
               for (int fx=0; fx < MatrixColumns; fx++) {
                  int isrc_x = x - TargetX + fx;
                  int matrix_index = ((MatrixRows - fy - 1) * MatrixColumns) + (MatrixColumns - fx - 1);
                  if (auto pixel = getGridPixel(grid, Width, Height, isrc_x, isrc_y)) {
                     r += pixel[R] * Matrix[matrix_index];
                     g += pixel[G] * Matrix[matrix_index];
                     b += pixel[B] * Matrix[matrix_index];
                     a += pixel[A] * Matrix[matrix_index];
                  }
               }
            }

            auto out = Output + ((y * Width) + x) * 4;
            double rgb_bias = Bias * double(source_alpha);
            int lr = std::clamp(int(std::lrint((factor * r) + rgb_bias)), 0, 255);
            int lg = std::clamp(int(std::lrint((factor * g) + rgb_bias)), 0, 255);
            int lb = std::clamp(int(std::lrint((factor * b) + rgb_bias)), 0, 255);
            out[R] = linear_rgb ? glLinearRGB.invert(lr) : lr;
            out[G] = linear_rgb ? glLinearRGB.invert(lg) : lg;
            out[B] = linear_rgb ? glLinearRGB.invert(lb) : lb;
            if (!PreserveAlpha) out[A] = std::clamp(int(std::lrint(factor * a + alpha_bias)), 0, 255);
            else out[A] = source_alpha;
         }
      }
   }

   ERR processResampled(objBitmap *InputBitmap, uint8_t *Output, int pLeft, int pTop, int pRight, int pBottom,
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

      std::vector<uint8_t> grid_in(grid_pixels * 4);
      std::vector<uint8_t> grid_out(grid_pixels * 4);

      const double source_scale_x = double(width) / double(grid_width);
      const double source_scale_y = double(height) / double(grid_height);
      for (int y=0; y < grid_height; y++) {
         for (int x=0; x < grid_width; x++) {
            const double sx = double(pLeft) + ((double(x) + 0.5) * source_scale_x) - 0.5;
            const double sy = double(pTop) + ((double(y) + 0.5) * source_scale_y) - 0.5;
            sampleBitmap(InputBitmap, sx, sy, grid_in.data() + ((y * grid_width) + x) * 4);
         }
      }

      convolveGrid(grid_in.data(), grid_out.data(), grid_width, grid_height, InputBitmap);

      const double grid_scale_x = double(grid_width) / double(width);
      const double grid_scale_y = double(grid_height) / double(height);
      const uint8_t A = InputBitmap->ColourFormat->AlphaPos>>3;
      uint8_t *outline = Output;
      uint8_t *alpha_input = InputBitmap->Data + (pTop * InputBitmap->LineWidth);
      for (int y=0; y < height; y++) {
         auto out = outline;
         for (int x=0; x < width; x++) {
            const double gx = ((double(x) + 0.5) * grid_scale_x) - 0.5;
            const double gy = ((double(y) + 0.5) * grid_scale_y) - 0.5;
            sampleGrid(grid_out.data(), grid_width, grid_height, gx, gy, out);
            if (PreserveAlpha) out[A] = (alpha_input + ((pLeft + x) * InputBitmap->BytesPerPixel))[A];
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

   void processClipped(objBitmap *InputBitmap, uint8_t *output, int pLeft, int pTop, int pRight, int pBottom,
      double UnitXPixel, double UnitYPixel) {
      if ((pRight <= pLeft) or (pBottom <= pTop)) return;

      const uint8_t A = InputBitmap->ColourFormat->AlphaPos>>3;
      const uint8_t R = InputBitmap->ColourFormat->RedPos>>3;
      const uint8_t G = InputBitmap->ColourFormat->GreenPos>>3;
      const uint8_t B = InputBitmap->ColourFormat->BluePos>>3;

      const double factor = 1.0 / Divisor;
      const double alpha_bias = Bias * 255.0;
      const bool linear_rgb = Filter->ColourSpace IS VCS::LINEAR_RGB;
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

      uint8_t *alpha_input = InputBitmap->Data + (pTop * stride);
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

            double rgb_bias = Bias * double(source_alpha);
            int lr = std::clamp(int(std::lrint((factor * r) + rgb_bias)), 0, 255);
            int lg = std::clamp(int(std::lrint((factor * g) + rgb_bias)), 0, 255);
            int lb = std::clamp(int(std::lrint((factor * b) + rgb_bias)), 0, 255);
            out[R] = linear_rgb ? glLinearRGB.invert(lr) : lr;
            out[G] = linear_rgb ? glLinearRGB.invert(lg) : lg;
            out[B] = linear_rgb ? glLinearRGB.invert(lb) : lb;
            if (!PreserveAlpha) out[A] = std::clamp(int(std::lrint(factor * a + alpha_bias)), 0, 255);
            else out[A] = source_alpha;
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

   std::vector<uint8_t> data;
   data.resize(canvas_width * canvas_height * Self->Target->BytesPerPixel);

   objBitmap *inBmp;
   if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;

   // Note: The inBmp->Data pointer is pre-adjusted to match the Clip Left and Top values (i.e. add
   // (Clip.Left * BPP) + (Clip.Top * LineWidth) to Data in order to get its true value)

   double unit_x_pixel = Self->unitPixelSpacingX();
   double unit_y_pixel = Self->unitPixelSpacingY();
   if ((unit_x_pixel <= 0.0) or (unit_y_pixel <= 0.0)) return ERR::InvalidValue;

   if (std::abs(unit_x_pixel - 1.0) < 0.001) unit_x_pixel = 1.0;
   if (std::abs(unit_y_pixel - 1.0) < 0.001) unit_y_pixel = 1.0;

   if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) inBmp->convertToLinear();
   inBmp->premultiply();

   if ((unit_x_pixel != 1.0) or (unit_y_pixel != 1.0)) {
      auto error = Self->processResampled(inBmp, data.data(), Self->Target->Clip.Left, Self->Target->Clip.Top,
         Self->Target->Clip.Right, Self->Target->Clip.Bottom, unit_x_pixel, unit_y_pixel);
      if (error != ERR::Okay) {
         if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) inBmp->convertToRGB();
         inBmp->demultiply();
         return error;
      }
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
         auto thread_output = (uint8_t *)data.data() + output_offset;

         pool.detach_task([Self, inBmp, thread_output, unit_x_pixel, unit_y_pixel, L = Self->Target->Clip.Left, T = top, R = Self->Target->Clip.Right, B = bottom]() {
            Self->processClipped(inBmp, thread_output, L, T, R, B, unit_x_pixel, unit_y_pixel);
         });
      }

      pool.wait();
   }

   // Copy the resulting output back to the bitmap.

   auto pixel = (uint32_t *)(Self->Target->Data + (Self->Target->Clip.Left * Self->Target->BytesPerPixel) + (Self->Target->Clip.Top * Self->Target->LineWidth));
   auto src   = (uint32_t *)data.data();
   for (int y=0; y < canvas_height; y++) {
      copymem(src, pixel, size_t(Self->Target->BytesPerPixel) * size_t(canvas_width));
      pixel += Self->Target->LineWidth>>2;
      src += canvas_width;
   }

   if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) inBmp->convertToRGB();

   inBmp->demultiply();
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
UnitX: The distance in filter units between rows in the Matrix.

Indicates the intended distance in current filter units (i.e. as determined by the value of PrimitiveUnits)
between successive columns and rows, respectively, in the #Matrix.  By specifying value(s) for #UnitX, the kernel
becomes defined in a scalable, abstract coordinate system.  If #UnitX is not specified, the default value is one pixel
in the offscreen bitmap, which is a pixel-based coordinate system, and thus potentially not scalable.  For some level
of consistency across display media and user agents, it is necessary that a value be provided for at least one of
ResX and #UnitX.

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
UnitY: The distance in filter units between columns in the Matrix.

Indicates the intended distance in current filter units (i.e. as determined by the value of PrimitiveUnits)
between successive columns and rows, respectively, in the #Matrix.  By specifying value(s) for #UnitY, the kernel
becomes defined in a scalable, abstract coordinate system.  If #UnitY is not specified, the default value is one pixel
in the offscreen bitmap, which is a pixel-based coordinate system, and thus potentially not scalable.  For some level
of consistency across display media and user agents, it is necessary that a value be provided for at least one of
ResY and #UnitY.

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

static const FieldArray clConvolveFXFields[] = {
   { "Bias",          FDF_DOUBLE|FDF_RI },
   { "Divisor",       FDF_DOUBLE|FDF_RI,         nullptr, CONVOLVEFX_SET_Divisor },
   { "EdgeMode",      FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clConvolveFXEM },
   { "MatrixRows",    FDF_INT|FDF_RI,            nullptr, CONVOLVEFX_SET_MatrixRows },
   { "MatrixColumns", FDF_INT|FDF_RI,            nullptr, CONVOLVEFX_SET_MatrixColumns },
   { "Matrix",        FDF_VIRTUAL|FDF_DOUBLE|FDF_ARRAY|FDF_RI|FDF_PURE, CONVOLVEFX_GET_Matrix, CONVOLVEFX_SET_Matrix },
   { "PreserveAlpha", FDF_INT|FDF_RW },
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
