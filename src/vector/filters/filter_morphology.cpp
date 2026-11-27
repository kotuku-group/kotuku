/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
MorphologyFX: Applies the morphology filter effect.

The MorphologyFX class performs "fattening" or "thinning" of artwork.  It is particularly useful for fattening or
thinning an alpha channel.

The radii are measured in the filter's primitive units.  When @VectorFilter.PrimitiveUnits is `BOUNDING_BOX`, the
radii scale with the dimensions of the filtered vector's bounding box by default.  If @VectorFilter.AspectRatio
is `NONE`, both radii scale with the normalised diagonal of the bounding box.  The renderer applies the magnitude
of the vector's transform scale on each axis and converts the resulting radii to whole pixels.  The kernel stays
aligned to the bitmap axes.  Its width and height are twice the respective pixel radii plus one.

Dilation takes the maximum value in the kernel and erosion takes the minimum.  The default bitmap mode applies
the operator to each component independently.  Alpha-only mode retains the straight RGB of the centre pixel and
changes only its alpha.  Numerical inputs retain their scalar or vector float format and use component-wise
minima or maxima without colour conversion.  Invalid numerical samples are skipped.  An empty numerical
neighbourhood yields an invalid sample.

-END-

**********************************************************************************************************************

Radii below VHGW_MIN_RADIUS use a direct kernel that costs O(radius) per pixel, which is fastest for the small radii
common in SVG.  Larger radii use the van Herk/Gil-Werman algorithm, which costs O(1) per pixel.  Benchmarked crossovers
were radius 6-8 at one megapixel; the direct vertical pass degrades sharply once its 2r+1 strided rows exceed the
cache, taking around 90ms per megapixel at radius 128 against 1.5ms for van Herk/Gil-Werman.

*********************************************************************************************************************/

class extMorphologyFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MORPHOLOGYFX;
   static constexpr CSTRING CLASS_NAME = "MorphologyFX";
   using create = kt::Create<extMorphologyFX>;

   double RadiusX = 0, RadiusY = 0;
   MOP Operator = MOP::ERODE;
   MKS KernelShape = MKS::RECTANGLE;
   MCM ChannelMode = MCM::COMPONENTS;

   extMorphologyFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   // Computes the kernel radius of each axis in bitmap pixels.  Radii are scaled from primitive units to pixels and
   // then truncated, as browsers do.  The epsilon prevents an exact product such as 0.3 * 10 from truncating to 2.

   std::pair<int, int> pixel_radii() const {
      const auto &render = *Filter->Render;
      double unit_x = 1.0, unit_y = 1.0;
      if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
         unit_x = render.ClientBounds.width();
         unit_y = render.ClientBounds.height();
         if (render.AspectRatio IS VFA::NONE) {
            unit_x = unit_y = filter_render_context::normalised_diagonal(unit_x, unit_y);
         }
      }
      const auto [scale_x, scale_y] = render.axis_scales();
      const auto to_pixels = [](double Radius) {
         if (not std::isfinite(Radius)) return 0;
         return int(std::floor(std::clamp(Radius, 0.0, double(MAX_RADIUS)) + 1e-6));
      };
      return { to_pixels(RadiusX * unit_x * scale_x), to_pixels(RadiusY * unit_y * scale_y) };
   }

   filter_sampling sampling() const {
      const auto [x, y] = pixel_radii();
      return { filter_access::BOUNDED, double(x) + 1, double(y) + 1 };
   }

   // Caps the pixel radius so that the sampling margin stays finite.  The cap exceeds any practical bitmap dimension.

   static constexpr int MAX_RADIUS = 65536;
};

//********************************************************************************************************************
// Computes the component-wise min or max of a single pixel's kernel span.  Spans are expressed as a start/end pixel
// pair with a byte step between each pixel: the horizontal pass steps by 4 within a row, the vertical pass steps by
// the row stride within a column.  Min/max is applied per byte, so channel ordering is irrelevant.

static void morph_span_scalar(const uint8_t *Start, const uint8_t *End, int Step, bool Dilate, uint8_t *Out)
{
   if (Dilate) {
      uint8_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
      for (auto pix = Start; pix <= End; pix += Step) {
         if (pix[0] > m0) m0 = pix[0];
         if (pix[1] > m1) m1 = pix[1];
         if (pix[2] > m2) m2 = pix[2];
         if (pix[3] > m3) m3 = pix[3];
      }
      Out[0] = m0; Out[1] = m1; Out[2] = m2; Out[3] = m3;
   }
   else {
      uint8_t m0 = 255, m1 = 255, m2 = 255, m3 = 255;
      for (auto pix = Start; pix <= End; pix += Step) {
         if (pix[0] < m0) m0 = pix[0];
         if (pix[1] < m1) m1 = pix[1];
         if (pix[2] < m2) m2 = pix[2];
         if (pix[3] < m3) m3 = pix[3];
      }
      Out[0] = m0; Out[1] = m1; Out[2] = m2; Out[3] = m3;
   }
}

#ifdef FILTER_SSE2

// As for morph_span_scalar(), but computes four consecutive output pixels (16 bytes) at once.  The caller must
// guarantee that Start through End+15 are valid reads and that Out has 16 writable bytes.

static void morph_span_sse2(const uint8_t *Start, const uint8_t *End, int Step, bool Dilate, uint8_t *Out)
{
   auto acc = _mm_loadu_si128((const __m128i *)Start);
   if (Dilate) {
      for (auto pix = Start + Step; pix <= End; pix += Step) {
         acc = _mm_max_epu8(acc, _mm_loadu_si128((const __m128i *)pix));
      }
   }
   else {
      for (auto pix = Start + Step; pix <= End; pix += Step) {
         acc = _mm_min_epu8(acc, _mm_loadu_si128((const __m128i *)pix));
      }
   }
   _mm_storeu_si128((__m128i *)Out, acc);
}

#endif

//********************************************************************************************************************
// Large radii use the van Herk/Gil-Werman algorithm, which costs a constant three min/max operations per pixel
// whatever the radius.  Each row (or column) is divided into segments of K = 2 * radius + 1 pixels, and running
// prefix (P) and suffix (S) extremes are computed within each segment.  A full kernel window [A, B] always spans the
// tail of one segment and the head of the next (or exactly one whole segment), so its result is op(S[A], P[B]).
// Windows clamped by the left edge start a segment, so they are P[B] alone.  Windows clamped by the right edge are
// S[A] alone if they lie within the final segment.
//
// The prefix is written directly to the output.  Output position X reads P[B] with B >= X, so ascending order never
// reads a prefix that has already been replaced by a result.  Below the threshold the direct kernel is faster
// because it has no setup cost.

static constexpr int VHGW_MIN_RADIUS = 7;

template <bool Dilate, class T> static inline T morph_op(T A, T B)
{
   if constexpr (Dilate) return std::max(A, B);
   else return std::min(A, B);
}

//********************************************************************************************************************
// Out = op(A, B) for Bytes bytes.  Out may be A or B, or may trail B in the same row.

template <bool Dilate> static void morph_combine(const uint8_t *A, const uint8_t *B, uint8_t *Out, size_t Bytes)
{
   size_t i = 0;
#ifdef FILTER_SSE2
   for (; i + 16 <= Bytes; i += 16) {
      auto a = _mm_loadu_si128((const __m128i *)(A + i));
      auto b = _mm_loadu_si128((const __m128i *)(B + i));
      if constexpr (Dilate) _mm_storeu_si128((__m128i *)(Out + i), _mm_max_epu8(a, b));
      else _mm_storeu_si128((__m128i *)(Out + i), _mm_min_epu8(a, b));
   }
#endif
   for (; i < Bytes; i++) Out[i] = morph_op<Dilate>(A[i], B[i]);
}

//********************************************************************************************************************

template <bool Dilate> static inline void morph_pixel(const uint8_t *A, const uint8_t *B, uint8_t *Out)
{
#ifdef FILTER_SSE2
   int32_t a, b;
   std::memcpy(&a, A, 4);
   std::memcpy(&b, B, 4);
   const auto va = _mm_cvtsi32_si128(a), vb = _mm_cvtsi32_si128(b);
   const int32_t result = _mm_cvtsi128_si32(Dilate ? _mm_max_epu8(va, vb) : _mm_min_epu8(va, vb));
   std::memcpy(Out, &result, 4);
#else
   Out[0] = morph_op<Dilate>(A[0], B[0]);
   Out[1] = morph_op<Dilate>(A[1], B[1]);
   Out[2] = morph_op<Dilate>(A[2], B[2]);
   Out[3] = morph_op<Dilate>(A[3], B[3]);
#endif
}

//********************************************************************************************************************
// Horizontal van Herk/Gil-Werman pass over one row of Width pixels.  Suffix is a row buffer of Width pixels.
// Radius must be less than Width.

template <bool Dilate>
static void morph_row_vhgw(const uint8_t *In, uint8_t *Out, uint8_t *Suffix, int Width, int Radius)
{
   const int k = (2 * Radius) + 1;

   for (int start=0; start < Width; start += k) {
      const int end = std::min(start + k, Width) - 1;

      std::copy_n(In + (start<<2), 4, Out + (start<<2));
      for (int x=start + 1; x <= end; x++) morph_pixel<Dilate>(Out + ((x - 1)<<2), In + (x<<2), Out + (x<<2));

      std::copy_n(In + (end<<2), 4, Suffix + (end<<2));
      for (int x=end - 1; x >= start; x--) morph_pixel<Dilate>(Suffix + ((x + 1)<<2), In + (x<<2), Suffix + (x<<2));
   }

   const int last = Width - 1;
   int x = 0;
   for (; x < Radius; x++) { // Clamped by the left edge
      std::copy_n(Out + (std::min(last, x + Radius)<<2), 4, Out + (x<<2));
   }

   if (const int interior = Width - (2 * Radius); interior > 0) { // Full windows
      morph_combine<Dilate>(Suffix, Out + ((2 * Radius)<<2), Out + (Radius<<2), size_t(interior)<<2);
      x += interior;
   }

   for (; x < Width; x++) { // Clamped by the right edge
      const int a = x - Radius;
      if (a / k IS last / k) std::copy_n(Suffix + (a<<2), 4, Out + (x<<2));
      else morph_pixel<Dilate>(Suffix + (a<<2), Out + (last<<2), Out + (x<<2));
   }
}

//********************************************************************************************************************
// Vertical van Herk/Gil-Werman pass.  Rows are combined whole, so the work vectorises across the row.  Suffix may be
// Src, in which case the source is overwritten.  Radius must be less than Height.

template <bool Dilate>
static void morph_columns_vhgw(const uint8_t *Src, int SrcStride, uint8_t *Dest, int DestStride, uint8_t *Suffix,
   int SuffixStride, int Width, int Height, int Radius)
{
   const int k = (2 * Radius) + 1;
   const size_t row_bytes = size_t(Width)<<2;
   const auto src  = [&](int Y) { return Src + (size_t(Y) * SrcStride); };
   const auto dest = [&](int Y) { return Dest + (size_t(Y) * DestStride); };
   const auto sfx  = [&](int Y) { return Suffix + (size_t(Y) * SuffixStride); };

   // The prefix of each segment is completed before its suffix, because the suffix may overwrite the source.

   for (int start=0; start < Height; start += k) {
      const int end = std::min(start + k, Height) - 1;

      std::copy_n(src(start), row_bytes, dest(start));
      for (int y=start + 1; y <= end; y++) morph_combine<Dilate>(dest(y - 1), src(y), dest(y), row_bytes);

      if (sfx(end) != src(end)) std::copy_n(src(end), row_bytes, sfx(end));
      for (int y=end - 1; y >= start; y--) morph_combine<Dilate>(sfx(y + 1), src(y), sfx(y), row_bytes);
   }

   const int last = Height - 1;
   int y = 0;
   for (; y < Radius; y++) { // Clamped by the top edge
      std::copy_n(dest(std::min(last, y + Radius)), row_bytes, dest(y));
   }

   for (; y + Radius <= last; y++) { // Full windows
      morph_combine<Dilate>(sfx(y - Radius), dest(y + Radius), dest(y), row_bytes);
   }

   for (; y < Height; y++) { // Clamped by the bottom edge
      const int a = y - Radius;
      if (a / k IS last / k) std::copy_n(sfx(a), row_bytes, dest(y));
      else morph_combine<Dilate>(sfx(a), dest(last), dest(y), row_bytes);
   }
}

//********************************************************************************************************************
// A monotonic queue visits each input sample at most once. Destination coordinates are expressed in the source
// coordinate system, so a window can also have no overlap with the input.

template<class T> struct morph_queue_entry {
   int Index;
   T Value;
};

template<bool Dilate, class T, class Read, class Write>
static void morph_line(int SourceStart, int SourceEnd, int DestStart, int DestEnd, int Radius, T Neutral,
   morph_queue_entry<T> *Queue, Read ReadSample, Write WriteSample)
{
   int head = 0, tail = 0;
   int next = std::max(SourceStart, DestStart - Radius);

   for (int i=DestStart; i < DestEnd; ++i) {
      const int first = i - Radius, last = std::min(i + Radius, SourceEnd - 1);
      while ((head < tail) and (Queue[head].Index < first)) ++head;
      for (; next <= last; ++next) {
         const T value = ReadSample(next);
         while ((head < tail) and (morph_op<Dilate>(value, Queue[tail - 1].Value) IS value)) --tail;
         Queue[tail++] = { next, value };
      }
      WriteSample(i, (head < tail) ? Queue[head].Value : Neutral);
   }
}

//********************************************************************************************************************

static int ellipse_span(int RadiusX, int RadiusY, int Offset)
{
   const double fraction = double(Offset) / RadiusY;
   return int(std::floor(RadiusX * std::sqrt(1.0 - fraction * fraction) + 1e-9));
}

//********************************************************************************************************************

template<bool Dilate, class T>
static ERR draw_morphology_channels(extMorphologyFX *Self, const filter_surface_view &Source,
   const filter_surface_view &Target, T *Dest, int RadiusX, int RadiusY, int Alpha)
{
   const auto &valid = Source.Valid;
   const auto &clip = Target.Valid;
   const int dx = Target.OriginX - Source.OriginX, dy = Target.OriginY - Source.OriginY;
   constexpr bool bitmap = std::is_same_v<T, uint8_t>;
   constexpr T neutral = bitmap ? (Dilate ? T(0) : std::numeric_limits<T>::max()) :
      (Dilate ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity());
   const int components = bitmap ? 4 : filter_components(Source.Format);
   const int first_channel = (Alpha >= 0) ? Alpha : 0;
   const int end_channel = (Alpha >= 0) ? Alpha + 1 : components;

   const int width = clip.width();
   const int left = clip.left + dx, right = clip.right + dx;
   const int top = clip.top + dy, bottom = clip.bottom + dy;
   const bool cross = (Self->KernelShape IS MKS::CROSS) and RadiusX and RadiusY;
   // When even the farthest corner fits inside the ellipse, the separable pass gives the same result.
   const int far_x = std::max(right - 1 - valid.left, valid.right - 1 - left);
   const int far_y = std::max(bottom - 1 - valid.top, valid.bottom - 1 - top);
   const bool ellipse = (Self->KernelShape IS MKS::ELLIPSE) and RadiusX and RadiusY and
      ((far_y > RadiusY) or (ellipse_span(RadiusX, RadiusY, far_y) < far_x));
   if (ellipse) {
      // Sliding extrema remove the horizontal radius from the cost. Bound the remaining work across row pairs.
      constexpr size_t max_ellipse_work = 128u * 1024u * 1024u;
      const size_t rows = std::min(valid.height(), 2 * RadiusY + 1);
      const size_t row_work = size_t(std::min(valid.width(), width + 2 * RadiusX)) + width;
      if (rows * clip.height() * row_work * (end_channel - first_channel) > max_ellipse_work) {
         return ERR::OutOfRange;
      }
   }

   morph_queue_entry<T> *queue;
   if (auto error = get_filter_scratch(Self, std::max(valid.width(), valid.height()), queue); error != ERR::Okay) {
      return error;
   }

   int *spans = nullptr;
   T *horizontal = nullptr;
   if (ellipse) {
      if (auto error = get_filter_scratch(Self, size_t(RadiusY) + 1, spans); error != ERR::Okay) return error;
      for (int offset=0; offset <= RadiusY; ++offset) {
         spans[offset] = ellipse_span(RadiusX, RadiusY, offset);
      }
   }
   else if (RadiusX and RadiusY and (not cross)) {
      if (auto error = get_filter_scratch(Self, size_t(width) * valid.height(), horizontal); error != ERR::Okay) {
         return error;
      }
   }

   const auto dest_pixel = [&](int X, int Y) {
      return (T *)((uint8_t *)Dest + std::ptrdiff_t(Y - dy) * Target.Stride) + std::ptrdiff_t(X - dx) * components;
   };

   if (Alpha >= 0) {
      for (int y=top; y < bottom; ++y) {
         for (int x=left; x < right; ++x) {
            auto out = dest_pixel(x, y);
            if (valid.hit_test(x, y)) {
               const auto in = (const T *)Source.row(y) + std::ptrdiff_t(x) * components;
               std::copy_n(in, components, out);
            }
            else std::fill_n(out, components, T(0));
         }
      }
   }

   for (int c=first_channel; c < end_channel; ++c) {
      const auto read = [&](int X, int Y) -> T {
         if constexpr (bitmap) return Source.row(Y)[std::ptrdiff_t(X) * 4 + c];
         else {
            filter_map_value value;
            return read_numeric_sample(Source, X, Y, value) ? value[c] : neutral;
         }
      };

      if (ellipse) {
         for (int y=top; y < bottom; ++y) {
            auto out = dest_pixel(left, y) + c;
            for (int x=left; x < right; ++x) out[std::ptrdiff_t(x - left) * components] = neutral;

            const int first = std::max(valid.top, y - RadiusY), last = std::min(valid.bottom, y + RadiusY + 1);
            for (int sy=first; sy < last; ++sy) {
               morph_line<Dilate>(valid.left, valid.right, left, right, spans[std::abs(sy - y)], neutral, queue,
                  [&](int X) { return read(X, sy); },
                  [&](int X, T Value) {
                     auto &pixel = out[std::ptrdiff_t(X - left) * components];
                     pixel = morph_op<Dilate>(pixel, Value);
                  });
            }
         }
      }
      else if (horizontal) {
         for (int y=valid.top; y < valid.bottom; ++y) {
            auto row = horizontal + std::ptrdiff_t(y - valid.top) * width;
            morph_line<Dilate>(valid.left, valid.right, left, right, RadiusX, neutral, queue,
               [&](int X) { return read(X, y); }, [&](int X, T Value) { row[X - left] = Value; });
         }
         for (int x=left; x < right; ++x) {
            morph_line<Dilate>(valid.top, valid.bottom, top, bottom, RadiusY, neutral, queue,
               [&](int Y) { return horizontal[std::ptrdiff_t(Y - valid.top) * width + x - left]; },
               [&](int Y, T Value) { dest_pixel(x, Y)[c] = Value; });
         }
      }
      else {
         if (RadiusX or (not RadiusY)) {
            for (int y=top; y < bottom; ++y) {
               morph_line<Dilate>(valid.left, valid.right, left, right, RadiusX, neutral, queue,
                  [&](int X) { return ((y >= valid.top) and (y < valid.bottom)) ? read(X, y) : neutral; },
                  [&](int X, T Value) { dest_pixel(X, y)[c] = Value; });
            }
         }
         if (RadiusY) {
            for (int x=left; x < right; ++x) {
               morph_line<Dilate>(valid.top, valid.bottom, top, bottom, RadiusY, neutral, queue,
                  [&](int Y) { return ((x >= valid.left) and (x < valid.right)) ? read(x, Y) : neutral; },
                  [&](int Y, T Value) {
                     auto &pixel = dest_pixel(x, Y)[c];
                     pixel = cross ? morph_op<Dilate>(pixel, Value) : Value;
                  });
            }
         }
      }

      if constexpr (not bitmap) {
         for (int y=top; y < bottom; ++y) {
            for (int x=left; x < right; ++x) {
               auto &pixel = dest_pixel(x, y)[c];
               if (pixel IS neutral) pixel = INVALID_FILTER_SAMPLE;
            }
         }
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR draw_shaped_morphology(extMorphologyFX *Self, const filter_surface_view &Source, int RadiusX, int RadiusY)
{
   const bool dilate = Self->Operator IS MOP::DILATE;
   if (Source.Format IS filter_surface_format::RGBA8) {
      const auto bmp = Self->Target;
      const filter_surface_view target { .Width = bmp->Width, .Height = bmp->Height, .Stride = bmp->LineWidth,
         .OriginX = Source.OriginX, .OriginY = Source.OriginY,
         .Valid = { bmp->Clip.Left, bmp->Clip.Top, bmp->Clip.Right, bmp->Clip.Bottom } };
      if (size_t(target.Valid.width()) * target.Valid.height() > 4096u * 4096u) return ERR::OutOfRange;

      const int alpha = (Self->ChannelMode IS MCM::ALPHA) ? Source.Bitmap->ColourFormat->AlphaPos >> 3 : -1;
      if (dilate) return draw_morphology_channels<true>(Self, Source, target, bmp->Data, RadiusX, RadiusY, alpha);
      return draw_morphology_channels<false>(Self, Source, target, bmp->Data, RadiusX, RadiusY, alpha);
   }

   filter_surface_view target;
   float *dest;
   if (auto error = get_numeric_target(Self, target, dest); error != ERR::Okay) return error;
   if (size_t(target.Valid.width()) * target.Valid.height() > 4096u * 4096u) return ERR::OutOfRange;
   if (dilate) return draw_morphology_channels<true>(Self, Source, target, dest, RadiusX, RadiusY, -1);
   return draw_morphology_channels<false>(Self, Source, target, dest, RadiusX, RadiusY, -1);
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR MORPHOLOGYFX_Draw(extMorphologyFX *Self, struct acDraw *Args)
{
   filter_surface_view source;
   if (auto error = get_source_view(Self, source, filter_input_role::INPUT); error != ERR::Okay) return error;

   const auto [radius_x, radius_y] = Self->pixel_radii();
   if ((source.Format != filter_surface_format::RGBA8) or (Self->KernelShape != MKS::RECTANGLE)) {
      return draw_shaped_morphology(Self, source, radius_x, radius_y);
   }

   const int canvasWidth = Self->Target->Clip.Right - Self->Target->Clip.Left;
   const int canvasHeight = Self->Target->Clip.Bottom - Self->Target->Clip.Top;

   if (size_t(canvasWidth) * canvasHeight > 4096u * 4096u) return ERR::OutOfRange;
   const auto in_bmp = source.Bitmap;

   if ((radius_x <= 0) and (radius_y <= 0)) {
      BAF copy_flags = (effect_colour_space(Self) IS VCS::LINEAR_RGB) ? BAF::LINEAR : BAF::NIL;
      return copy_filter_bitmap(in_bmp, Self->Target, copy_flags, 0, 0, in_bmp->Width, in_bmp->Height, 0, 0);
   }

   const int clamped_x = std::min(radius_x, canvasWidth - 1);
   const int clamped_y = std::min(radius_y, canvasHeight - 1);
   bool vhgw_x = clamped_x >= VHGW_MIN_RADIUS;
   bool vhgw_y = clamped_y >= VHGW_MIN_RADIUS;
   const bool both_axes = (radius_x > 0) and (radius_y > 0);

   // An image buffer is required if the effect applies on both axes, in which case it holds the horizontal result.
   // The vertical van Herk/Gil-Werman pass also needs one for its suffixes, which overwrite the horizontal result in
   // place.  The horizontal pass needs a row buffer for its suffixes.  Otherwise results are written directly to the
   // target bitmap.

   const size_t image_bytes = size_t(canvasWidth) * canvasHeight * 4;
   const bool need_image = both_axes or vhgw_y;
   const size_t scratch_bytes = (need_image ? image_bytes : 0) + (vhgw_x ? size_t(canvasWidth) * 4 : 0);

   uint8_t *scratch = nullptr;
   if (scratch_bytes) {
      if (auto error = get_filter_scratch(Self, scratch_bytes, scratch); error != ERR::Okay) {
         // Retry with only the intermediate image required by the two-axis pass. Its storage also serves
         // as the vertical suffix buffer, so the vertical fast path can still run when both axes apply.

         if ((error != ERR::AllocMemory) or (both_axes and not vhgw_x)) return error;

         vhgw_x = false;
         if (both_axes) {
            if (auto retry = get_filter_scratch(Self, image_bytes, scratch); retry != ERR::Okay) return retry;
         }
         else vhgw_y = false;
      }
   }

   uint8_t *buffer = (both_axes or vhgw_y) ? scratch : nullptr;
   uint8_t *row_suffix = vhgw_x ? scratch + (need_image ? image_bytes : 0) : nullptr;

   uint8_t *out_line;
   int out_linewidth;

   if (both_axes) {
      out_line = buffer;
      out_linewidth = canvasWidth * 4;
   }
   else {
      out_line = (uint8_t *)(Self->Target->Data + (Self->Target->Clip.Left<<2) + (Self->Target->Clip.Top * Self->Target->LineWidth));
      out_linewidth = Self->Target->LineWidth;
   }

   const uint8_t *input = in_bmp->Data + (in_bmp->Clip.Top * in_bmp->LineWidth) +
      (in_bmp->Clip.Left * in_bmp->BytesPerPixel);

   const bool dilate = Self->Operator IS MOP::DILATE;

   if (vhgw_x) { // Horizontal pass, large radius
      for (int y=0; y < canvasHeight; y++) {
         const uint8_t *in_row = input + (y * in_bmp->LineWidth);
         uint8_t *out = out_line + (y * out_linewidth);
         if (dilate) morph_row_vhgw<true>(in_row, out, row_suffix, canvasWidth, clamped_x);
         else morph_row_vhgw<false>(in_row, out, row_suffix, canvasWidth, clamped_x);
      }
   }
   else if (radius_x > 0) { // Horizontal pass
      auto radius = clamped_x;

      for (int y=0; y < canvasHeight; y++) {
         const uint8_t *in_row = input + (y * in_bmp->LineWidth);
         uint8_t *out = out_line + (y * out_linewidth);

         int x = 0;
#ifdef FILTER_SSE2
         // Left border pixels have a clamped kernel and are processed individually.
         for (; (x < radius) and (x < canvasWidth); x++) {
            morph_span_scalar(in_row + (std::max(0, x - radius)<<2), in_row + (std::min(canvasWidth - 1, x + radius)<<2),
               4, dilate, out + (x<<2));
         }

         // Interior pixels are processed four at a time; the kernels of all four must be within the row.
         for (; x + 3 + radius <= canvasWidth - 1; x += 4) {
            morph_span_sse2(in_row + ((x - radius)<<2), in_row + ((x + radius)<<2), 4, dilate, out + (x<<2));
         }
#endif
         for (; x < canvasWidth; x++) {
            morph_span_scalar(in_row + (std::max(0, x - radius)<<2), in_row + (std::min(canvasWidth - 1, x + radius)<<2),
               4, dilate, out + (x<<2));
         }
      }
   }

   if (radius_y > 0) { // Vertical pass
      auto radius = clamped_y;

      const uint8_t *src;
      int src_stride;
      if (both_axes) {
         src = buffer;
         src_stride = canvasWidth * 4;
      }
      else {
         src = input;
         src_stride = in_bmp->LineWidth;
      }

      auto dest = (uint8_t *)(Self->Target->Data + (Self->Target->Clip.Left<<2) + (Self->Target->Clip.Top * Self->Target->LineWidth));

      if (vhgw_y) {
         const int dest_stride = Self->Target->LineWidth;
         const int sfx_stride = canvasWidth * 4;
         if (dilate) {
            morph_columns_vhgw<true>(src, src_stride, dest, dest_stride, buffer, sfx_stride, canvasWidth,
               canvasHeight, radius);
         }
         else {
            morph_columns_vhgw<false>(src, src_stride, dest, dest_stride, buffer, sfx_stride, canvasWidth,
               canvasHeight, radius);
         }
      }
      else {
         for (int y=0; y < canvasHeight; y++) {
            const uint8_t *start_row = src + (std::max(0, y - radius) * src_stride);
            const uint8_t *end_row   = src + (std::min(canvasHeight - 1, y + radius) * src_stride);
            uint8_t *out = dest + (y * Self->Target->LineWidth);

            int x = 0;
#ifdef FILTER_SSE2
            for (; x + 4 <= canvasWidth; x += 4) {
               morph_span_sse2(start_row + (x<<2), end_row + (x<<2), src_stride, dilate, out + (x<<2));
            }
#endif
            for (; x < canvasWidth; x++) {
               morph_span_scalar(start_row + (x<<2), end_row + (x<<2), src_stride, dilate, out + (x<<2));
            }
         }
      }
   }

   if (Self->ChannelMode IS MCM::ALPHA) {
      // Reuse the separable rectangle pass for coverage and restore the centre RGB from the straight input.
      const int alpha = in_bmp->ColourFormat->AlphaPos >> 3;
      const auto target = Self->Target;
      auto dest = target->Data + std::ptrdiff_t(target->Clip.Top) * target->LineWidth + target->Clip.Left * 4;
      for (int y=0; y < canvasHeight; ++y) {
         const auto in = input + std::ptrdiff_t(y) * in_bmp->LineWidth;
         const auto out = dest + std::ptrdiff_t(y) * target->LineWidth;
         for (int x=0; x < canvasWidth; ++x) {
            const auto coverage = out[x * 4 + alpha];
            std::copy_n(in + x * 4, 4, out + x * 4);
            out[x * 4 + alpha] = coverage;
         }
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ChannelMode: Selects the bitmap channels to process.
Lookup: MCM

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_ChannelMode(extMorphologyFX *Self, MCM Value)
{
   if ((Value != MCM::COMPONENTS) and (Value != MCM::ALPHA)) return ERR::OutOfRange;
   Self->ChannelMode = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
KernelShape: Selects the morphology neighbourhood.
Lookup: MKS

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_KernelShape(extMorphologyFX *Self, MKS Value)
{
   if ((Value < MKS::RECTANGLE) or (Value > MKS::CROSS)) return ERR::OutOfRange;
   Self->KernelShape = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Operator: Set to either `ERODE` or `DILATE`.
Lookup: MOP

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_Operator(extMorphologyFX *Self, MOP Value)
{
   Self->Operator = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RadiusX: X radius value.

The horizontal radius of the kernel, in the filter's primitive units.  Fractional values are permitted.  A
value of zero disables the effect on this axis; the input is copied unchanged if both radii resolve to less than one
pixel.

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_RadiusX(extMorphologyFX *Self, double Value)
{
   if ((Value >= 0) and std::isfinite(Value)) {
      Self->RadiusX = Value;
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
RadiusY: Y radius value.

The vertical radius of the kernel, in the filter's primitive units.  Fractional values are permitted.  A
value of zero disables the effect on this axis; the input is copied unchanged if both radii resolve to less than one
pixel.

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_RadiusY(extMorphologyFX *Self, double Value)
{
   if ((Value >= 0) and std::isfinite(Value)) {
      Self->RadiusY = Value;
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR morphology_xml(const extMorphologyFX &Effect, filter_xml_element &Element)
{
   Element.Name = "feMorphology";
   Element.attribute("operator", Effect.Operator IS MOP::ERODE ? "erode" : "dilate");
   Element.attribute("radius", Element.number(Effect.RadiusX) + ' ' + Element.number(Effect.RadiusY));

   if (Effect.KernelShape IS MKS::ELLIPSE) Element.attribute("kotuku:kernelShape", "ellipse");
   else if (Effect.KernelShape IS MKS::CROSS) Element.attribute("kotuku:kernelShape", "cross");

   if (Effect.ChannelMode IS MCM::ALPHA) Element.attribute("kotuku:channelMode", "alpha");

   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_morphology_def.c"

static const FieldArray clMorphologyFXFields[] = {
   { "RadiusX",     FDF_DOUBLE|FDF_RW, nullptr, MORPHOLOGYFX_SET_RadiusX },
   { "RadiusY",     FDF_DOUBLE|FDF_RW, nullptr, MORPHOLOGYFX_SET_RadiusY },
   { "Operator",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, MORPHOLOGYFX_SET_Operator, &clMorphologyFXMOP },
   { "KernelShape", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, MORPHOLOGYFX_SET_KernelShape, &clMorphologyFXMKS },
   { "ChannelMode", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, MORPHOLOGYFX_SET_ChannelMode, &clMorphologyFXMCM },
   { "XMLDef",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_morphfx(void)
{
   clMorphologyFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::MORPHOLOGYFX),
      fl::Name("MorphologyFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clMorphologyFXActions),
      fl::Fields(clMorphologyFXFields),
      fl::Size(sizeof(extMorphologyFX)),
      fl::Path(MOD_PATH));

   return clMorphologyFX ? ERR::Okay : ERR::AddClass;
}
