/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
MorphologyFX: Applies the morphology filter effect.

The MorphologyFX class performs "fattening" or "thinning" of artwork.  It is particularly useful for fattening or
thinning an alpha channel.

The dilation (or erosion) kernel is a rectangle with a width of `2 * RadiusX` and a height of `2 * RadiusY`. In
dilation, the output pixel is the individual component-wise maximum of the corresponding R,G,B,A values in the input
image's kernel rectangle.  In erosion, the output pixel is the individual component-wise minimum of the
corresponding R,G,B,A values in the input image's kernel rectangle.

Frequently this operation will take place on alpha-only images, such as that produced by the built-in input,
SourceAlpha.  In that case, the implementation might want to optimize the single channel case.

Because the algorithm operates on premultiplied colour values, it will always result in colour values less than or
equal to the alpha channel.

-END-

**********************************************************************************************************************

TODO: The current implementation computes each output pixel's min/max over the full kernel span, costing O(radius)
per pixel even with the SSE2 fast path.  The van Herk/Gil-Werman algorithm would reduce this to O(1) per pixel
regardless of radius: divide each row (or column) into segments of length `2 * radius + 1`, precompute running
min/max prefixes and suffixes within each segment, then form each output as `op(suffix[x - radius],
prefix[x + radius])` - three operations per pixel in total.  The two extra prefix/suffix buffers need only cover a
single row/column at a time, and the technique combines well with the existing SIMD span helpers since prefix and
suffix accumulation are themselves 16-byte vectorisable.  Worthwhile if profiling shows large-radius morphology as
a bottleneck; for small radii (1-3, the common SVG case) the present approach is likely faster due to its lower
setup cost.

*********************************************************************************************************************/

class extMorphologyFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MORPHOLOGYFX;
   static constexpr CSTRING CLASS_NAME = "MorphologyFX";
   using create = kt::Create<extMorphologyFX>;

   int RadiusX = 0, RadiusY = 0;
   MOP Operator = MOP::ERODE;
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

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR MORPHOLOGYFX_Draw(extMorphologyFX *Self, struct acDraw *Args)
{
   const int canvasWidth = Self->Target->Clip.Right - Self->Target->Clip.Left;
   const int canvasHeight = Self->Target->Clip.Bottom - Self->Target->Clip.Top;

   if (canvasWidth * canvasHeight > 4096 * 4096) return ERR::OutOfRange; // Bail on really large bitmaps.

   objBitmap *inBmp;
   if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;

   if ((Self->RadiusX <= 0) and (Self->RadiusY <= 0)) {
      BAF copy_flags = (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) ? BAF::LINEAR : BAF::NIL;
      gfx::CopyArea(inBmp, Self->Target, copy_flags, 0, 0, inBmp->Width, inBmp->Height, 0, 0);
      return ERR::Okay;
   }

   uint8_t *out_line;
   uint8_t *buffer = nullptr;
   int out_linewidth;
   bool buffer_as_input;

   // A temporary buffer is required if we are applying the effect on both axis.  Otherwise we can
   // directly write to the target bitmap.

   if ((Self->RadiusX > 0) and (Self->RadiusY > 0)) {
      buffer = new (std::nothrow) uint8_t[canvasWidth * canvasHeight * 4];
      if (!buffer) return ERR::Memory;
      out_line = buffer;
      out_linewidth = canvasWidth * 4;
      buffer_as_input = true;
   }
   else {
      out_line = (uint8_t *)(Self->Target->Data + (Self->Target->Clip.Left<<2) + (Self->Target->Clip.Top * Self->Target->LineWidth));
      out_linewidth = Self->Target->LineWidth;
      buffer_as_input = false;
   }

   uint8_t *input = inBmp->Data + (inBmp->Clip.Top * inBmp->LineWidth) + (inBmp->Clip.Left * inBmp->BytesPerPixel);

   const bool dilate = Self->Operator IS MOP::DILATE;

   if (Self->RadiusX > 0) { // Horizontal pass
      auto radius = Self->RadiusX;
      if (canvasWidth - 1 < radius) radius = canvasWidth - 1;

      for (int y=0; y < canvasHeight; y++) {
         const uint8_t *in_row = input + (y * inBmp->LineWidth);
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

   if (Self->RadiusY > 0) { // Vertical pass
      auto radius = Self->RadiusY;
      if (canvasHeight - 1 < radius) radius = canvasHeight - 1;

      const uint8_t *src;
      int src_stride;
      if (buffer_as_input) {
         src = buffer;
         src_stride = canvasWidth * 4;
      }
      else {
         src = input;
         src_stride = inBmp->LineWidth;
      }

      auto dest = (uint8_t *)(Self->Target->Data + (Self->Target->Clip.Left<<2) + (Self->Target->Clip.Top * Self->Target->LineWidth));

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

   if (buffer) delete [] buffer;

   return ERR::Okay;
}

//********************************************************************************************************************

/*********************************************************************************************************************

-FIELD-
Operator: Set to either `ERODE` or `DILATE`.
Lookup: MOP

-FIELD-
RadiusX: X radius value.

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_RadiusX(extMorphologyFX *Self, int Value)
{
   if (Value >= 0) {
      Self->RadiusX = Value;
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
RadiusY: Y radius value.

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_SET_RadiusY(extMorphologyFX *Self, int Value)
{
   if (Value >= 0) {
      Self->RadiusY = Value;
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR MORPHOLOGYFX_GET_XMLDef(extMorphologyFX *Self, std::string_view &Value)
{
   std::stringstream stream;

   stream << "feMorphology operator=\"";

   if (Self->Operator IS MOP::ERODE) stream << "erode\"";
   else stream << "dilate\"";

   stream << "radius=\"" << Self->RadiusX << " " << Self->RadiusY << "\"";

   auto cppstr = stream.str();
   if (auto str = strclone(stream.str())) {
      Value = std::string_view{str, cppstr.size()};
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

//********************************************************************************************************************

#include "filter_morphology_def.c"

static const FieldArray clMorphologyFXFields[] = {
   { "RadiusX",  FDF_INT|FDF_RW, nullptr, MORPHOLOGYFX_SET_RadiusX },
   { "RadiusY",  FDF_INT|FDF_RW, nullptr, MORPHOLOGYFX_SET_RadiusY },
   { "Operator", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clMorphologyFXMOP },
   { "XMLDef",   FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, MORPHOLOGYFX_GET_XMLDef },
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
