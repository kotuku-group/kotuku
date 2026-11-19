/*********************************************************************************************************************

-CATEGORY-
Name: Bitmap
-END-

*********************************************************************************************************************/

#include "defs.h"
#include "../link/simd.h"
#include "blit_stats.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

//********************************************************************************************************************
// Exact reciprocals for the divisor 255 * a of the translucent destination blend, for a >= 1.  For any numerator
// n < 2^24, (n * glTranslucentRecip[a]) >> 40 IS n / (255 * a), because each entry is ceil(2^40 / d) with d < 2^16.
// This is the table used by glTranslucent in the Vector module.

static constexpr auto glTranslucentRecip = []() {
   std::array<uint64_t, 256> table{};
   for (uint64_t a=1; a < 256; a++) table[a] = ((uint64_t(1)<<40) + (0xff * a) - 1) / (0xff * a);
   return table;
}();

//********************************************************************************************************************
// round((Value * Alpha) / 255) for Value and Alpha in the range 0 - 255.  The result is exact for every input pair,
// and simd_mul_div255() computes the same result on 16-bit lanes.

inline uint32_t mul_div255(uint32_t Value, uint32_t Alpha)
{
   const uint32_t t = (Value * Alpha) + 128;
   return (t + (t >> 8)) >> 8;
}

// Mixes a source and destination channel with the weight Alpha.  The result never exceeds 255, because neither
// term exceeds its weight: round(S * A / 255) <= A and round(D * (255 - A) / 255) <= 255 - A.  An Alpha of 255
// returns Src and an Alpha of 0 returns Dest.

inline uint32_t mix255(uint32_t Src, uint32_t Dest, uint32_t Alpha)
{
   return mul_div255(Src, Alpha) + mul_div255(Dest, 0xff - Alpha);
}

// A local copy of a packed colour format, so that 15/16-bit loops hold the shifts and masks in registers.  Reading
// them through a ColourFormat pointer forces a reload after every pixel store, which prevents vectorisation.  The
// unpack and pack functions match objBitmap's unpackRed() and packPixel().

struct PackedFormat {
   uint32_t RedPos, GreenPos, BluePos, RedShift, GreenShift, BlueShift, RedMask, GreenMask, BlueMask, AlphaBits;

   PackedFormat(const ColourFormat &Format) :
      RedPos(Format.RedPos), GreenPos(Format.GreenPos), BluePos(Format.BluePos),
      RedShift(Format.RedShift), GreenShift(Format.GreenShift), BlueShift(Format.BlueShift),
      RedMask(Format.RedMask), GreenMask(Format.GreenMask), BlueMask(Format.BlueMask),
      AlphaBits(((255u >> Format.AlphaShift) & Format.AlphaMask) << Format.AlphaPos) { }

   inline uint32_t red(uint32_t Packed) const   { return uint8_t(((Packed >> RedPos) & RedMask) << RedShift); }
   inline uint32_t green(uint32_t Packed) const { return uint8_t(((Packed >> GreenPos) & GreenMask) << GreenShift); }
   inline uint32_t blue(uint32_t Packed) const  { return uint8_t(((Packed >> BluePos) & BlueMask) << BlueShift); }

   inline uint32_t pack(uint32_t Red, uint32_t Green, uint32_t Blue) const {
      return (((Red >> RedShift) & RedMask) << RedPos) | (((Green >> GreenShift) & GreenMask) << GreenPos) |
         (((Blue >> BlueShift) & BlueMask) << BluePos) | AlphaBits;
   }
};

// The row functions below are kept out of line.  When GCC inlines them into CopyArea(), it does not vectorise the
// row loop.

#if defined(_MSC_VER)
#define ROW_NOINLINE __declspec(noinline)
#else
#define ROW_NOINLINE __attribute__((noinline))
#endif

// The row pointers are not declared __restrict.  CopyArea() permits copying within a bitmap, including overlapping
// regions, and the rows must then be processed in order: a copy one pixel to the right reads pixels that the same
// row has already written.  Without __restrict, GCC checks for overlap at run time and takes the scalar loop when
// the ranges overlap, so that such copies keep the results of the original sequential loop.

// Mixes a row of 15/16-bit pixels at a constant weight.  Shared by the constant-opacity paths of CopyArea() and
// CopyRawBitmap().

ROW_NOINLINE static void mix_row16(const uint16_t *Src, uint16_t *Dest, int Width,
   const PackedFormat &SrcFormat, const PackedFormat &DestFormat, uint32_t Alpha)
{
   const PackedFormat sf = SrcFormat, df = DestFormat;
   for (int i=0; i < Width; i++) {
      const uint32_t s = Src[i], d = Dest[i];
      Dest[i] = uint16_t(df.pack(mix255(sf.red(s), df.red(d), Alpha), mix255(sf.green(s), df.green(d), Alpha),
         mix255(sf.blue(s), df.blue(d), Alpha)));
   }
}

// Blends a row of 32-bit source pixels onto 15/16-bit pixels, weighting each by its alpha scaled by Opacity.  There
// are no special cases for alpha 0 and 255, because mix255() returns the destination and source values exactly for
// those weights.  An alpha of 0 therefore rewrites the destination pixel with its own colour bits.  Bits outside the
// colour masks, such as the unused top bit of a 15-bit pixel, are cleared, as the translucent case always did.

ROW_NOINLINE static void blend_row32to16(const uint32_t *Src, uint16_t *Dest, int Width,
   const ColourFormat &SrcFormat, const PackedFormat &DestFormat, uint32_t Opacity)
{
   const PackedFormat df = DestFormat;
   const uint32_t r_pos = SrcFormat.RedPos, g_pos = SrcFormat.GreenPos, b_pos = SrcFormat.BluePos;
   const uint32_t a_pos = SrcFormat.AlphaPos;
   for (int i=0; i < Width; i++) {
      const uint32_t s = Src[i], d = Dest[i];
      const uint32_t alpha = mul_div255((s >> a_pos) & 0xff, Opacity);
      Dest[i] = uint16_t(df.pack(mix255((s >> r_pos) & 0xff, df.red(d), alpha),
         mix255((s >> g_pos) & 0xff, df.green(d), alpha), mix255((s >> b_pos) & 0xff, df.blue(d), alpha)));
   }
}

//********************************************************************************************************************
// Blends a source colour onto a 32-bit destination pixel with the sRGB formula used by CopyArea().  A translucent
// destination (0 < alpha < 255) uses straight-alpha source-over, which weights each colour by its alpha.  The result
// is identical to srgb_blend32 in the Vector module, which relies on this for its CopyArea() fast path.  An empty
// destination retains the opaque formula; BAF::COPY replaces such pixels instead.
//
// The translucent numerators are below 2^24 (src_weight + dest_weight <= divisor), so the divisions are performed
// exactly with glTranslucentRecip.  simd_blend_translucent4() computes the same quotients.

inline void blend_srgb32(uint8_t *Dest, uint8_t dR, uint8_t dG, uint8_t dB, uint8_t dA,
   uint8_t Red, uint8_t Green, uint8_t Blue, uint8_t Alpha)
{
   const uint32_t alpha_inv  = 0xff - Alpha;
   const uint32_t dest_alpha = Dest[dA];

   if ((dest_alpha IS 0xff) or (!dest_alpha)) {
      Dest[dR] = ((Red * Alpha) + (Dest[dR] * alpha_inv) + 0xff)>>8;
      Dest[dG] = ((Green * Alpha) + (Dest[dG] * alpha_inv) + 0xff)>>8;
      Dest[dB] = ((Blue * Alpha) + (Dest[dB] * alpha_inv) + 0xff)>>8;
      Dest[dA] = 0xff - ((alpha_inv * (0xff - dest_alpha))>>8);
   }
   else {
      const uint32_t final_alpha = 0xff - ((alpha_inv * (0xff - dest_alpha))>>8); // Always >= 1
      const uint32_t src_weight  = 0xff * Alpha;
      const uint32_t dest_weight = alpha_inv * dest_alpha;
      const uint32_t round       = (0xff * final_alpha)>>1;
      const uint64_t recip       = glTranslucentRecip[final_alpha];
      auto channel = [&](uint32_t Src, uint32_t Dst) {
         return uint8_t(std::min<uint64_t>(0xff, ((Src * src_weight + Dst * dest_weight + round) * recip)>>40));
      };
      Dest[dR] = channel(Red, Dest[dR]);
      Dest[dG] = channel(Green, Dest[dG]);
      Dest[dB] = channel(Blue, Dest[dB]);
      Dest[dA] = uint8_t(final_alpha);
   }
}

//********************************************************************************************************************
// floor(Numerator / Divisor) for the translucent destination formula of blend_linear32(), given Recip = 1.0 / Divisor
// as a double.  The numerator is an integer below 2^32 and the divisor an integer from 1 to 65025, so the quotient is
// below 65536.  avx_linear_quotient4() performs the same operations.
//
// Exactness: Recip and the product each carry a relative error of at most 2^-53, so the computed product is within
// 2^-36 of the true quotient q, and adding the bias rounds by at most 2^-37 more.  The bias, 2^-20, exceeds that
// error, so an integer q is never truncated to q - 1.  A q that is not an integer lies at least 1/Divisor (> 2^-16)
// below the next integer, which exceeds the bias plus the error, so it is never truncated up.  A fused multiply-add
// only reduces the error.

constexpr double LINEAR_QUOTIENT_BIAS = 1.0 / double(1<<20);

inline uint32_t linear_quotient(double Numerator, double Recip)
{
   return uint32_t((Numerator * Recip) + LINEAR_QUOTIENT_BIAS);
}

// The linear light equivalent of blend_srgb32().  A translucent destination weights its colour by its own alpha,
// without which it would be treated as fully opaque and darken the result.  blend_linear_row_avx2() computes the same
// results for eight pixels at a time.

inline void blend_linear32(uint8_t *Dest, uint8_t dR, uint8_t dG, uint8_t dB, uint8_t dA,
   uint8_t Red, uint8_t Green, uint8_t Blue, uint8_t Alpha)
{
   const uint32_t alpha_inv  = 0xff - Alpha;
   const uint32_t dest_alpha = Dest[dA];

   if ((dest_alpha IS 0xff) or (!dest_alpha)) {
      auto mix = [&](uint8_t Source, uint8_t Target) -> uint8_t {
         return glLinearRGB.invert(((glLinearRGB.convert(Source) * Alpha) +
            (glLinearRGB.convert(Target) * alpha_inv) + 0xff)>>8);
      };

      Dest[dR] = mix(Red, Dest[dR]);
      Dest[dG] = mix(Green, Dest[dG]);
      Dest[dB] = mix(Blue, Dest[dB]);
      Dest[dA] = 0xff - ((alpha_inv * (0xff - dest_alpha))>>8);
   }
   else {
      // The divisor is at least 255, because a translucent destination gives dest_weight >= 255 when Alpha is 0.
      // Each numerator is at most 65535 * divisor + round, so the quotient never exceeds 0xffff.

      const uint32_t final_alpha = 0xff - ((alpha_inv * (0xff - dest_alpha))>>8); // Always >= 1
      const uint32_t src_weight  = 0xff * Alpha;
      const uint32_t dest_weight = alpha_inv * dest_alpha;
      const uint32_t divisor     = src_weight + dest_weight; // Normalise before rounding the stored alpha.
      const double round = double(divisor>>1);
      const double recip = 1.0 / double(divisor);

      auto mix = [&](uint8_t Source, uint8_t Target) -> uint8_t {
         const double numerator = (double(glLinearRGB.convert16(Source)) * double(src_weight)) +
            (double(glLinearRGB.convert16(Target)) * double(dest_weight)) + round;
         return glLinearRGB.invert16(uint16_t(std::min<uint32_t>(0xffff, linear_quotient(numerator, recip))));
      };

      Dest[dR] = mix(Red, Dest[dR]);
      Dest[dG] = mix(Green, Dest[dG]);
      Dest[dB] = mix(Blue, Dest[dB]);
      Dest[dA] = uint8_t(final_alpha);
   }
}

#ifdef KOTUKU_SSE2

// SIMD helpers for the 32-bit blitting routines.  All arithmetic is performed on 16-bit lanes so that the
// scalar formulae are reproduced exactly; callers handle trailing pixels with the original scalar loops.

// Returns true if a row cannot use the 16-byte SIMD loops because the copy overlaps itself.  CopyArea() permits
// copying within a bitmap, and CopyRawBitmap() can be given a surface over the destination's memory.  The scalar
// loops process each row from left to right, so a destination that starts after its source reads source bytes that
// the same row has already rewritten.  A SIMD iteration loads 16 source bytes before storing any, which gives the
// same result only when the destination starts at least 16 bytes after the source or anywhere before it.  The
// destination loads are not affected, because each iteration reads its own destination bytes before storing them.
// The AVX2 loops load 32 bytes per iteration and pass a Span of 32.

inline bool simd_overlap(const void *Src, const void *Dest, uintptr_t Span = 16)
{
   const auto s = uintptr_t(Src), d = uintptr_t(Dest);
   return (d > s) and (d - s < Span);
}

// The helpers below use SSE4.1 and SSSE3 instructions where KOTUKU_SSE42 is defined, which is the build baseline on
// x86.  The SSE2 forms are retained for toolchains without it and give identical results.

// Mask ? A : B.  Every Mask byte must be 0x00 or 0xff, because _mm_blendv_epi8() reads only the top bit of each
// byte.  The masks used here are comparison results and byte-aligned channel masks, which meet that condition.

inline __m128i simd_select(__m128i Mask, __m128i A, __m128i B)
{
#ifdef KOTUKU_SSE42
   return _mm_blendv_epi8(B, A, Mask);
#else
   return _mm_or_si128(_mm_and_si128(Mask, A), _mm_andnot_si128(Mask, B));
#endif
}

// Zero-extend the low eight bytes of V to 16-bit lanes, and the low four 16-bit lanes to 32-bit lanes.  The high
// halves still use _mm_unpackhi_epi8() and _mm_unpackhi_epi16() with a zero vector.

inline __m128i simd_widen8_lo(__m128i V)
{
#ifdef KOTUKU_SSE42
   return _mm_cvtepu8_epi16(V);
#else
   return _mm_unpacklo_epi8(V, _mm_setzero_si128());
#endif
}

inline __m128i simd_widen16_lo(__m128i V)
{
#ifdef KOTUKU_SSE42
   return _mm_cvtepu16_epi32(V);
#else
   return _mm_unpacklo_epi16(V, _mm_setzero_si128());
#endif
}

// Broadcast the alpha byte of each 32-bit pixel across all four of its byte lanes.  AlphaPos must be a multiple of
// 8.  The shuffle control depends only on AlphaPos, so the compiler hoists it out of the callers' loops.

inline __m128i simd_broadcast_alpha(__m128i Pixels, int AlphaPos)
{
#ifdef KOTUKU_SSE42
   const auto control = _mm_add_epi8(_mm_set1_epi8(char(AlphaPos>>3)),
      _mm_setr_epi8(0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 12));
   return _mm_shuffle_epi8(Pixels, control);
#else
   auto a = _mm_and_si128(_mm_srl_epi32(Pixels, _mm_cvtsi32_si128(AlphaPos)), _mm_set1_epi32(0xff));
   a = _mm_or_si128(a, _mm_slli_epi32(a, 8));
   return _mm_or_si128(a, _mm_slli_epi32(a, 16));
#endif
}

// round((Value * Alpha) / 255) on 16-bit lanes, matching the scalar mul_div255().

inline __m128i simd_mul_div255(__m128i Value, __m128i Alpha)
{
   auto t = _mm_add_epi16(_mm_mullo_epi16(Value, Alpha), _mm_set1_epi16(128));
   return _mm_srli_epi16(_mm_add_epi16(t, _mm_srli_epi16(t, 8)), 8);
}

// Blend four 32-bit source pixels onto four destination pixels with the (S*A + D*(255-A) + 255)>>8 formula
// used by CopyArea(), updating the destination alpha channel as 255 - (((255-A) * (255-D))>>8).  The source
// alpha is first scaled by Opacity with (A*Opacity + 255)>>8, which is an exact identity when Opacity is 255.
// AlphaPos is the bit position of the alpha channel and AlphaLane is the matching 16-bit lane mask.

inline __m128i simd_blend4(__m128i S, __m128i D, int AlphaPos, __m128i AlphaLane, __m128i Opacity)
{
   const auto zero = _mm_setzero_si128();
   const auto max  = _mm_set1_epi16(255);

   auto a8  = simd_broadcast_alpha(S, AlphaPos);
   auto slo = simd_widen8_lo(S),  shi = _mm_unpackhi_epi8(S, zero);
   auto dlo = simd_widen8_lo(D),  dhi = _mm_unpackhi_epi8(D, zero);
   auto alo = simd_widen8_lo(a8), ahi = _mm_unpackhi_epi8(a8, zero);

   alo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(alo, Opacity), max), 8);
   ahi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(ahi, Opacity), max), 8);

   auto calo = _mm_sub_epi16(max, alo), cahi = _mm_sub_epi16(max, ahi);

   auto blo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(slo, alo), _mm_mullo_epi16(dlo, calo)), max), 8);
   auto bhi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(shi, ahi), _mm_mullo_epi16(dhi, cahi)), max), 8);

   auto xlo = _mm_sub_epi16(max, _mm_srli_epi16(_mm_mullo_epi16(calo, _mm_sub_epi16(max, dlo)), 8));
   auto xhi = _mm_sub_epi16(max, _mm_srli_epi16(_mm_mullo_epi16(cahi, _mm_sub_epi16(max, dhi)), 8));

   blo = simd_select(AlphaLane, xlo, blo);
   bhi = simd_select(AlphaLane, xhi, bhi);
   return _mm_packus_epi16(blo, bhi);
}

// Per-pixel mask of the destination pixels that are translucent (0 < alpha < 255).  simd_blend4() implements the
// opaque formula only, so these pixels take their result from simd_blend_translucent4().

inline __m128i simd_translucent_mask(__m128i D, __m128i AlphaMask)
{
   const auto a = _mm_and_si128(D, AlphaMask);
   const auto edge = _mm_or_si128(_mm_cmpeq_epi32(a, _mm_setzero_si128()), _mm_cmpeq_epi32(a, AlphaMask));
   return _mm_xor_si128(edge, _mm_set1_epi32(-1));
}

// Blend one pixel's channels, held in 32-bit lanes, with the translucent destination formula.  The weights are
// broadcast from lane N of the per-pixel weight vectors.

template <int N>
inline __m128i simd_translucent_pixel(__m128i S, __m128i D, __m128 SrcWeight, __m128 DestWeight, __m128 Round,
   __m128 Divisor)
{
   constexpr int lane = _MM_SHUFFLE(N, N, N, N);
   auto num = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(S), _mm_shuffle_ps(SrcWeight, SrcWeight, lane)),
      _mm_mul_ps(_mm_cvtepi32_ps(D), _mm_shuffle_ps(DestWeight, DestWeight, lane))),
      _mm_shuffle_ps(Round, Round, lane));
   return _mm_cvttps_epi32(_mm_div_ps(num, _mm_shuffle_ps(Divisor, Divisor, lane)));
}

// Blend four 32-bit source pixels onto four destination pixels with the translucent destination formula of
// blend_srgb32(), with the source alpha scaled by Opacity as in simd_blend4().  The result is exact for every pixel,
// including opaque and empty destinations, but only translucent destinations use this formula.
//
// Exactness: every numerator, weight and divisor is an integer below 2^24, so each is held exactly in a float and
// the products and sums are exact.  The divisor d is at most 65025, so a quotient that is not an integer lies at least
// 1/d (> 2^-16) below the next integer.  For quotients below 256 that exceeds half an ulp (at most 2^-17), so the
// correctly rounded division never rounds up to the next integer and truncation yields floor(n / d).  The final
// alpha is at least 1, so no lane divides by zero.

inline __m128i simd_blend_translucent4(__m128i S, __m128i D, int AlphaPos, __m128i AlphaMask, __m128i Opacity)
{
   const auto zero  = _mm_setzero_si128();
   const auto max   = _mm_set1_epi32(255);
   const auto shift = _mm_cvtsi32_si128(AlphaPos);

   // Per-pixel alphas and weights in 32-bit lanes.  Each product is at most 65025, so the 16-bit multiplies of
   // values with a zero upper half produce the full 32-bit result.

   auto sa = _mm_and_si128(_mm_srl_epi32(S, shift), max);
   auto da = _mm_and_si128(_mm_srl_epi32(D, shift), max);
   sa = _mm_srli_epi32(_mm_add_epi32(_mm_mullo_epi16(sa, Opacity), max), 8);
   const auto inv = _mm_sub_epi32(max, sa);
   const auto final_alpha = _mm_sub_epi32(max, _mm_srli_epi32(_mm_mullo_epi16(inv, _mm_sub_epi32(max, da)), 8));
   const auto divisor = _mm_mullo_epi16(final_alpha, max);

   const auto src_weight  = _mm_cvtepi32_ps(_mm_mullo_epi16(sa, max));
   const auto dest_weight = _mm_cvtepi32_ps(_mm_mullo_epi16(inv, da));
   const auto round       = _mm_cvtepi32_ps(_mm_srli_epi32(divisor, 1));
   const auto div         = _mm_cvtepi32_ps(divisor);

   const auto slo = simd_widen8_lo(S), shi = _mm_unpackhi_epi8(S, zero);
   const auto dlo = simd_widen8_lo(D), dhi = _mm_unpackhi_epi8(D, zero);

   auto p0 = simd_translucent_pixel<0>(simd_widen16_lo(slo), simd_widen16_lo(dlo),
      src_weight, dest_weight, round, div);
   auto p1 = simd_translucent_pixel<1>(_mm_unpackhi_epi16(slo, zero), _mm_unpackhi_epi16(dlo, zero),
      src_weight, dest_weight, round, div);
   auto p2 = simd_translucent_pixel<2>(simd_widen16_lo(shi), simd_widen16_lo(dhi),
      src_weight, dest_weight, round, div);
   auto p3 = simd_translucent_pixel<3>(_mm_unpackhi_epi16(shi, zero), _mm_unpackhi_epi16(dhi, zero),
      src_weight, dest_weight, round, div);

   // Saturating packs clamp channels that exceed 255 by rounding, as std::min() does in blend_srgb32().

   auto colour = _mm_packus_epi16(_mm_packs_epi32(p0, p1), _mm_packs_epi32(p2, p3));
   return simd_select(AlphaMask, _mm_sll_epi32(final_alpha, shift), colour);
}

// Blend four pixels as blend_srgb32() does for any destination alpha: simd_blend4() for opaque and empty
// destinations, and simd_blend_translucent4() for translucent ones.  The translucent kernel only runs when at least
// one of the four destination pixels needs it.  Callers still select the source or destination for zero alphas.

inline __m128i simd_blend4_any(__m128i S, __m128i D, int AlphaPos, __m128i AlphaLane, __m128i AlphaMask,
   __m128i Opacity)
{
   auto blended = simd_blend4(S, D, AlphaPos, AlphaLane, Opacity);
   const auto translucent = simd_translucent_mask(D, AlphaMask);
   if (_mm_movemask_epi8(translucent)) {
      BLIT_STAT_PIXELS(BLEND32_TRANSLUCENT, 4);
      blended = simd_select(translucent, simd_blend_translucent4(S, D, AlphaPos, AlphaMask, Opacity), blended);
   }
   return blended;
}

#endif // KOTUKU_SSE2

#ifdef KOTUKU_AVX2

//********************************************************************************************************************
// AVX2 forms of the 32-bit blend kernels, for eight pixels per iteration.  Callers select them once per CopyArea() or
// CopyRawBitmap() call with simd_has_avx2().  Each row function processes whole groups of eight pixels and returns
// the number processed; the caller's SSE2 and scalar loops finish the remaining 0 - 7 pixels.  The rows must not
// overlap within 32 bytes; see simd_overlap().
//
// The 16-bit lane arithmetic is that of the SSE2 kernels.  The 256-bit unpack and pack instructions operate on each
// 128-bit half independently, so each half reproduces the SSE2 result for its four pixels.

// Shift counts for the alpha channel and the three colour channels of a 32-bit pixel with byte-aligned channels.

struct AvxShifts {
   __m128i Alpha;
   __m128i Colour[3];

   AvxShifts(int AlphaPos) {
      // The colour channels occupy the three byte positions that the alpha channel does not.
      Alpha     = _mm_cvtsi32_si128(AlphaPos);
      Colour[0] = _mm_cvtsi32_si128((AlphaPos IS 0) ? 8 : 0);
      Colour[1] = _mm_cvtsi32_si128((AlphaPos <= 8) ? 16 : 8);
      Colour[2] = _mm_cvtsi32_si128((AlphaPos IS 24) ? 16 : 24);
   }
};

KOTUKU_TARGET_AVX2 inline __m256i avx_select(__m256i Mask, __m256i A, __m256i B) // Mask ? A : B, per bit
{
   return _mm256_or_si256(_mm256_and_si256(Mask, A), _mm256_andnot_si256(Mask, B));
}

KOTUKU_TARGET_AVX2 inline __m256i avx_broadcast_alpha(__m256i Pixels, __m128i Shift)
{
   auto a = _mm256_and_si256(_mm256_srl_epi32(Pixels, Shift), _mm256_set1_epi32(0xff));
   a = _mm256_or_si256(a, _mm256_slli_epi32(a, 8));
   return _mm256_or_si256(a, _mm256_slli_epi32(a, 16));
}

KOTUKU_TARGET_AVX2 inline __m256i avx_mul_div255(__m256i Value, __m256i Alpha)
{
   auto t = _mm256_add_epi16(_mm256_mullo_epi16(Value, Alpha), _mm256_set1_epi16(128));
   return _mm256_srli_epi16(_mm256_add_epi16(t, _mm256_srli_epi16(t, 8)), 8);
}

// The eight-pixel form of simd_blend4(): the opaque and empty destination formula.

KOTUKU_TARGET_AVX2 inline __m256i avx_blend8(__m256i S, __m256i D, __m128i AlphaShift, __m256i AlphaLane,
   __m256i Opacity)
{
   const auto zero = _mm256_setzero_si256();
   const auto max  = _mm256_set1_epi16(255);

   auto a8  = avx_broadcast_alpha(S, AlphaShift);
   auto slo = _mm256_unpacklo_epi8(S, zero),  shi = _mm256_unpackhi_epi8(S, zero);
   auto dlo = _mm256_unpacklo_epi8(D, zero),  dhi = _mm256_unpackhi_epi8(D, zero);
   auto alo = _mm256_unpacklo_epi8(a8, zero), ahi = _mm256_unpackhi_epi8(a8, zero);

   alo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(alo, Opacity), max), 8);
   ahi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(ahi, Opacity), max), 8);

   auto calo = _mm256_sub_epi16(max, alo), cahi = _mm256_sub_epi16(max, ahi);

   auto blo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(slo, alo),
      _mm256_mullo_epi16(dlo, calo)), max), 8);
   auto bhi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(shi, ahi),
      _mm256_mullo_epi16(dhi, cahi)), max), 8);

   auto xlo = _mm256_sub_epi16(max, _mm256_srli_epi16(_mm256_mullo_epi16(calo, _mm256_sub_epi16(max, dlo)), 8));
   auto xhi = _mm256_sub_epi16(max, _mm256_srli_epi16(_mm256_mullo_epi16(cahi, _mm256_sub_epi16(max, dhi)), 8));

   blo = avx_select(AlphaLane, xlo, blo);
   bhi = avx_select(AlphaLane, xhi, bhi);
   return _mm256_packus_epi16(blo, bhi);
}

KOTUKU_TARGET_AVX2 inline __m256i avx_translucent_mask(__m256i D, __m256i AlphaMask)
{
   const auto a = _mm256_and_si256(D, AlphaMask);
   const auto edge = _mm256_or_si256(_mm256_cmpeq_epi32(a, _mm256_setzero_si256()), _mm256_cmpeq_epi32(a, AlphaMask));
   return _mm256_xor_si256(edge, _mm256_set1_epi32(-1));
}

// The eight-pixel form of simd_blend_translucent4().  It works on one channel of all eight pixels at a time, which
// holds each pixel's weights in its own 32-bit lane and needs one division per colour channel: three per eight
// pixels, against four per four pixels in the SSE2 kernel.  Each lane performs the same float operations in the same
// order as the SSE2 kernel, so the exactness argument given there applies unchanged.  The quotients are clamped to
// 255 as the saturating packs do there.

KOTUKU_TARGET_AVX2 inline __m256i avx_blend_translucent8(__m256i S, __m256i D, const AvxShifts &Shifts,
   __m256i Opacity)
{
   const auto max = _mm256_set1_epi32(255);

   auto sa = _mm256_and_si256(_mm256_srl_epi32(S, Shifts.Alpha), max);
   auto da = _mm256_and_si256(_mm256_srl_epi32(D, Shifts.Alpha), max);
   sa = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi16(sa, Opacity), max), 8);
   const auto inv = _mm256_sub_epi32(max, sa);
   const auto final_alpha = _mm256_sub_epi32(max,
      _mm256_srli_epi32(_mm256_mullo_epi16(inv, _mm256_sub_epi32(max, da)), 8));
   const auto divisor = _mm256_mullo_epi16(final_alpha, max);

   const auto src_weight  = _mm256_cvtepi32_ps(_mm256_mullo_epi16(sa, max));
   const auto dest_weight = _mm256_cvtepi32_ps(_mm256_mullo_epi16(inv, da));
   const auto round       = _mm256_cvtepi32_ps(_mm256_srli_epi32(divisor, 1));
   const auto div         = _mm256_cvtepi32_ps(divisor);

   auto result = _mm256_sll_epi32(final_alpha, Shifts.Alpha);
   for (const auto &shift : Shifts.Colour) {
      const auto sc = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srl_epi32(S, shift), max));
      const auto dc = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srl_epi32(D, shift), max));
      const auto num = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(sc, src_weight), _mm256_mul_ps(dc, dest_weight)),
         round);
      const auto q = _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_div_ps(num, div)), max);
      result = _mm256_or_si256(result, _mm256_sll_epi32(q, shift));
   }
   return result;
}

// Blends a row of 32-bit pixels for the three BAF::BLEND loops of CopyArea().  Copy selects the BAF::COPY form, in
// which an empty destination pixel is replaced by the source pixel.  Source and destination share one channel
// layout, with the alpha channel at AlphaPos.

template <bool Copy>
ROW_NOINLINE KOTUKU_TARGET_AVX2 static int blend_row32_avx2(const uint8_t *Src, uint8_t *Dest, int Width,
   int AlphaPos, int Opacity)
{
   const AvxShifts shifts(AlphaPos);
   const auto zero = _mm256_setzero_si256();
   const auto amask = _mm256_set1_epi32(int(uint32_t(0xff) << AlphaPos));
   const auto alpha_lane = _mm256_cmpeq_epi16(_mm256_unpacklo_epi8(amask, zero), _mm256_set1_epi16(0xff));
   const auto op = _mm256_set1_epi16(short(Opacity));

   int x = 0;
   for (; x + 8 <= Width; x += 8) {
      const auto s = _mm256_loadu_si256((const __m256i *)(Src + (x<<2)));
      const auto d = _mm256_loadu_si256((const __m256i *)(Dest + (x<<2)));
      const auto src_zero = _mm256_cmpeq_epi32(_mm256_and_si256(s, amask), zero);

      auto blended = avx_blend8(s, d, shifts.Alpha, alpha_lane, op);
      const auto translucent = avx_translucent_mask(d, amask);
      if (not _mm256_testz_si256(translucent, translucent)) {
         BLIT_STAT_PIXELS(BLEND32_TRANSLUCENT, 8);
         blended = avx_select(translucent, avx_blend_translucent8(s, d, shifts, op), blended);
      }

      auto result = avx_select(src_zero, d, blended);
      if constexpr (Copy) {
         result = avx_select(_mm256_cmpeq_epi32(_mm256_and_si256(d, amask), zero), s, result);
      }
      _mm256_storeu_si256((__m256i *)(Dest + (x<<2)), result);
   }
   return x;
}

// Mixes a row of 32-bit pixels at a constant Opacity, for the constant-opacity loops of CopyArea() and
// CopyRawBitmap().  The result keeps the RGBMask channels and has Alpha added in.

ROW_NOINLINE KOTUKU_TARGET_AVX2 static int mix_row32_avx2(const uint32_t *Src, uint32_t *Dest, int Width,
   uint32_t RGBMask, uint32_t Alpha, int Opacity)
{
   const auto zero = _mm256_setzero_si256();
   const auto op   = _mm256_set1_epi16(short(Opacity));
   const auto cop  = _mm256_set1_epi16(short(255 - Opacity));
   const auto rgb_mask = _mm256_set1_epi32(int(RGBMask));
   const auto alpha = _mm256_set1_epi32(int(Alpha));

   int i = 0;
   for (; i + 8 <= Width; i += 8) {
      const auto s = _mm256_loadu_si256((const __m256i *)(Src + i));
      const auto d = _mm256_loadu_si256((const __m256i *)(Dest + i));
      const auto blo = _mm256_add_epi16(avx_mul_div255(_mm256_unpacklo_epi8(s, zero), op),
         avx_mul_div255(_mm256_unpacklo_epi8(d, zero), cop));
      const auto bhi = _mm256_add_epi16(avx_mul_div255(_mm256_unpackhi_epi8(s, zero), op),
         avx_mul_div255(_mm256_unpackhi_epi8(d, zero), cop));
      const auto packed = _mm256_packus_epi16(blo, bhi);
      _mm256_storeu_si256((__m256i *)(Dest + i), _mm256_or_si256(_mm256_and_si256(packed, rgb_mask), alpha));
   }
   return i;
}

//********************************************************************************************************************
// BAF::LINEAR blending.  The sRGB conversions are table lookups, which the AVX2 kernel performs with gathers.  These
// copies of the glLinearRGB tables suit 32-bit gathers: Forward holds the 8-bit linear value in bits 0-7 and the
// 16-bit linear value in bits 8-23, so one gather serves both blend formulas.  The inverse tables are byte tables
// that are read with a scale of 1 and masked, so they are padded by three bytes to keep every 32-bit read in bounds.

struct LinearTables {
   int32_t Forward[256];
   uint8_t Invert[256 + 3];
   uint8_t Invert16[4096 + 3];
};

// Built on first use, because glLinearRGB is initialised by another translation unit.

static const LinearTables & linear_tables()
{
   static const LinearTables tables = []() {
      LinearTables t{};
      for (int i=0; i < 256; i++) {
         t.Forward[i] = int32_t(glLinearRGB.convert(uint8_t(i)) | (uint32_t(glLinearRGB.convert16(uint8_t(i))) << 8));
         t.Invert[i]  = glLinearRGB.invert(uint8_t(i));
      }
      for (int i=0; i < 4096; i++) t.Invert16[i] = glLinearRGB.invert16(uint16_t(i << 4));
      return t;
   }();
   return tables;
}

KOTUKU_TARGET_AVX2 inline __m128i avx_half(__m256i V, int Half) // Half 0 is the low 128 bits, 1 the high
{
   return Half ? _mm256_extracti128_si256(V, 1) : _mm256_castsi256_si128(V);
}

KOTUKU_TARGET_AVX2 inline __m256i avx_gather_byte(const uint8_t *Table, __m256i Index)
{
   return _mm256_and_si256(_mm256_i32gather_epi32((const int *)Table, Index, 1), _mm256_set1_epi32(0xff));
}

// linear_quotient() for four pixels, with the numerator computed from 16-bit linear values in 32-bit lanes.  Every
// term of the numerator is an integer below 2^32, so it is exact in a double whatever the order of operations.

KOTUKU_TARGET_AVX2 inline __m128i avx_linear_quotient4(__m128i Src, __m128i Dest, __m256d SrcWeight,
   __m256d DestWeight, __m256d Round, __m256d Recip)
{
   const auto numerator = _mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(_mm256_cvtepi32_pd(Src), SrcWeight),
      _mm256_mul_pd(_mm256_cvtepi32_pd(Dest), DestWeight)), Round);
   return _mm256_cvttpd_epi32(_mm256_add_pd(_mm256_mul_pd(numerator, Recip),
      _mm256_set1_pd(LINEAR_QUOTIENT_BIAS)));
}

// Blends eight pixels with the formulas of blend_linear32(), for any source and destination alpha.  Each 32-bit lane
// holds one pixel, and each colour channel is processed for all eight pixels at once.  Opaque and empty destinations
// take nine gathers (two forward lookups and one inverse per channel).  A group with a translucent destination also
// runs the 16-bit formula, which takes three more gathers.

KOTUKU_TARGET_AVX2 inline __m256i avx_blend_linear8(__m256i S, __m256i D, const AvxShifts &Shifts,
   __m256i AlphaMask, __m256i Opacity, const LinearTables &Tables)
{
   const auto max = _mm256_set1_epi32(255);

   // Products of values below 256 fit the low 16-bit half of each 32-bit lane, as in avx_blend_translucent8().

   auto sa = _mm256_and_si256(_mm256_srl_epi32(S, Shifts.Alpha), max);
   const auto da = _mm256_and_si256(_mm256_srl_epi32(D, Shifts.Alpha), max);
   sa = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi16(sa, Opacity), max), 8);
   const auto inv = _mm256_sub_epi32(max, sa);
   const auto final_alpha = _mm256_sub_epi32(max,
      _mm256_srli_epi32(_mm256_mullo_epi16(inv, _mm256_sub_epi32(max, da)), 8));

   const auto translucent = avx_translucent_mask(D, AlphaMask);
   const bool any_translucent = not _mm256_testz_si256(translucent, translucent);

   __m256d src_weight[2], dest_weight[2], round[2], recip[2];
   if (any_translucent) {
      const auto sw = _mm256_mullo_epi16(sa, max);
      const auto dw = _mm256_mullo_epi16(inv, da);
      const auto divisor = _mm256_add_epi32(sw, dw);
      const auto half = _mm256_srli_epi32(divisor, 1);

      // Lanes that are not translucent can have a divisor of 0 (an empty source over an empty destination).  Their
      // results are discarded, but they still index the gather, so they divide by 1 instead.  Every lane's
      // numerator is then at most 65535 * divisor + round, which keeps its index within Invert16.

      const auto safe_divisor = _mm256_max_epi32(divisor, _mm256_set1_epi32(1));
      for (int h=0; h < 2; h++) {
         src_weight[h]  = _mm256_cvtepi32_pd(avx_half(sw, h));
         dest_weight[h] = _mm256_cvtepi32_pd(avx_half(dw, h));
         round[h]       = _mm256_cvtepi32_pd(avx_half(half, h));
         recip[h]       = _mm256_div_pd(_mm256_set1_pd(1.0), _mm256_cvtepi32_pd(avx_half(safe_divisor, h)));
      }
   }

   auto result = _mm256_sll_epi32(final_alpha, Shifts.Alpha);
   for (const auto &shift : Shifts.Colour) {
      const auto fs = _mm256_i32gather_epi32(Tables.Forward, _mm256_and_si256(_mm256_srl_epi32(S, shift), max), 4);
      const auto fd = _mm256_i32gather_epi32(Tables.Forward, _mm256_and_si256(_mm256_srl_epi32(D, shift), max), 4);

      const auto mixed = _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(
         _mm256_mullo_epi16(_mm256_and_si256(fs, max), sa), _mm256_mullo_epi16(_mm256_and_si256(fd, max), inv)),
         max), 8);
      auto channel = avx_gather_byte(Tables.Invert, mixed);

      if (any_translucent) {
         const auto ls = _mm256_srli_epi32(fs, 8), ld = _mm256_srli_epi32(fd, 8);
         const auto q_lo = avx_linear_quotient4(avx_half(ls, 0), avx_half(ld, 0),
            src_weight[0], dest_weight[0], round[0], recip[0]);
         const auto q_hi = avx_linear_quotient4(avx_half(ls, 1), avx_half(ld, 1),
            src_weight[1], dest_weight[1], round[1], recip[1]);
         const auto q = _mm256_min_epu32(_mm256_set_m128i(q_hi, q_lo), _mm256_set1_epi32(0xffff));
         channel = avx_select(translucent, avx_gather_byte(Tables.Invert16, _mm256_srli_epi32(q, 4)), channel);
      }

      result = _mm256_or_si256(result, _mm256_sll_epi32(channel, shift));
   }
   return result;
}

// The most pixels in a group of eight that blend_linear_row_avx2() blends one at a time.  On an i7-6700K, a group with
// one such pixel took a third of the vector blend's time onto a translucent destination and half onto an opaque one.
// Limits of 2 and 3 also gained on groups with that many pixels, but cost 4-9% on dense content, because the test
// and its branch are no longer a single comparison with zero.

constexpr int LINEAR_SPARSE_LIMIT = 1;

// Blends a row of 32-bit pixels for the BAF::LINEAR loops of CopyArea(), with the results of blend_linear32().  The
// pixel selection matches the scalar loops: a source alpha of 0 leaves the destination unchanged, a source alpha of
// 255 is copied when Opacity is 255, and Copy replaces empty destination pixels with the source.  The BAF::COPY loop
// ignores Opacity, so the caller passes 255 for it.  Groups in which few or no pixels blend, which are common in glyph
// and icon images, skip the vector blend.

template <bool Copy>
ROW_NOINLINE KOTUKU_TARGET_AVX2 static int blend_linear_row_avx2(const uint8_t *Src, uint8_t *Dest, int Width,
   int AlphaPos, int Opacity, const LinearTables &Tables)
{
   const AvxShifts shifts(AlphaPos);
   const auto zero  = _mm256_setzero_si256();
   const auto amask = _mm256_set1_epi32(int(uint32_t(0xff) << AlphaPos));
   const auto op    = _mm256_set1_epi32(Opacity);
   const auto copy_opaque = _mm256_set1_epi32((Opacity IS 255) ? -1 : 0);

   // Byte positions for blend_linear32(), which blends each colour channel alike, so their order does not matter.

   const uint8_t a_byte = AlphaPos>>3;
   const uint8_t c0 = (a_byte IS 0) ? 1 : 0, c1 = (a_byte <= 1) ? 2 : 1, c2 = (a_byte IS 3) ? 2 : 3;

   int x = 0;
   for (; x + 8 <= Width; x += 8) {
      const auto s = _mm256_loadu_si256((const __m256i *)(Src + (x<<2)));
      const auto d = _mm256_loadu_si256((const __m256i *)(Dest + (x<<2)));

      // A pixel cannot be both empty and opaque, so take_src can be applied after keep_dest.  For Copy, an empty
      // destination takes the source whatever the source alpha.

      const auto src_alpha = _mm256_and_si256(s, amask);
      const auto keep_dest = _mm256_cmpeq_epi32(src_alpha, zero);
      auto take_src = _mm256_and_si256(_mm256_cmpeq_epi32(src_alpha, amask), copy_opaque);
      if constexpr (Copy) {
         take_src = _mm256_or_si256(take_src, _mm256_cmpeq_epi32(_mm256_and_si256(d, amask), zero));
      }

      // The vector blend costs the same however few of its pixels need it, so a group in which at most
      // LINEAR_SPARSE_LIMIT pixels blend uses blend_linear32() for those pixels.  The group is staged in a local
      // copy, so that every pixel is still loaded before the single store, as simd_overlap() requires.

      const auto unchanged = _mm256_or_si256(keep_dest, take_src);
      const uint32_t blend_lanes = ~uint32_t(_mm256_movemask_ps(_mm256_castsi256_ps(unchanged))) & 0xff;
      __m256i result;
      if (std::popcount(blend_lanes) <= LINEAR_SPARSE_LIMIT) {
         result = avx_select(take_src, s, d);
         if (blend_lanes) {
            alignas(32) uint32_t src_px[8], dest_px[8];
            _mm256_store_si256((__m256i *)src_px, s);
            _mm256_store_si256((__m256i *)dest_px, result);
            for (auto lanes = blend_lanes; lanes; lanes &= lanes - 1) {
               const int lane = std::countr_zero(lanes);
               const auto sp = (const uint8_t *)(src_px + lane);
               const uint8_t a = (sp[a_byte] * Opacity + 0xff)>>8;
               blend_linear32((uint8_t *)(dest_px + lane), c0, c1, c2, a_byte, sp[c0], sp[c1], sp[c2], a);
            }
            result = _mm256_load_si256((const __m256i *)dest_px);
         }
      }
      else {
         result = avx_blend_linear8(s, d, shifts, amask, op, Tables);
         result = avx_select(take_src, s, avx_select(keep_dest, d, result));
      }
      _mm256_storeu_si256((__m256i *)(Dest + (x<<2)), result);
   }
   return x;
}

#endif // KOTUKU_AVX2

//********************************************************************************************************************
// NOTE: Please ensure that the Width and Height are already clipped to meet the restrictions of BOTH the source and
// destination bitmaps.

static ERR dither(extBitmap *Bitmap, extBitmap *Dest, ColourFormat *Format, int Width, int Height,
   int SrcX, int SrcY, int DestX, int DestY)
{
   RGB16 *buf1, *buf2, *buffer;
   RGB8 brgb;
   uint8_t *data;
   int x, y;
   int16_t index;

   if ((Width < 1) or (Height < 1)) return ERR::Okay;

   // Punch the developer for stupid mistakes

   if ((Dest->BitsPerPixel >= 24) and (!Format)) return ERR::InvalidData;

   // Do a straight copy if the bitmap is too small for dithering

   if ((Height < 2) or (Width < 2)) {
      for (y=0; y < Height; y++) {
         for (x=0; x < Width; x++) {
            Bitmap->ReadUCRPixel(Bitmap, SrcX + x, SrcY + y, &brgb);
            Dest->DrawUCRPixel(Dest, DestX + x, DestY + y, &brgb);
         }
      }
      return ERR::Okay;
   }

   auto DITHER_ERROR = [&]<typename T>(uint8_t Src, T RGB16::*Comp, int x, int y) {
      // Dither one colour component
      if (int dif = (buf1[x].*Comp>>3) - (Src<<3)) { // An eighth of the error
         int val3 = buf2[x+1].*Comp + (dif<<1);     // 1/4 down & right
         dif = dif + dif + dif;
         int val1 = buf1[x+1].*Comp + dif;          // 3/8 to the right
         int val2 = buf2[x].*Comp + dif;            // 3/8 to the next row
         if (dif > 0) {                              // Check for overflow
            buf1[x+1].*Comp = std::min(16383, val1);
            buf2[x].*Comp   = std::min(16383, val2);
            buf2[x+1].*Comp = std::min(16383, val3);
         }
         else if (dif < 0) {
            buf1[x+1].*Comp = std::max(0, val1);
            buf2[x].*Comp   = std::max(0, val2);
            buf2[x+1].*Comp = std::max(0, val3);
         }
      }
   };

   std::vector<RGB16> calc_buffer(Width * 2);

   buf1 = calc_buffer.data();
   buf2 = buf1 + Width;

   // Prime buf2, which will be copied to buf1 at the start of the loop.
   // We work with six binary "decimal places" to reduce roundoff errors.

   auto srcdata = Bitmap->Data + (SrcY * Bitmap->LineWidth);
   for (x=0,index=0; x < Width; x++,index+=Bitmap->BytesPerPixel) {
      Bitmap->ReadUCRIndex(Bitmap, srcdata + (SrcX * Bitmap->BytesPerPixel) + index, &brgb);
      buf2[x].Red   = brgb.Red<<6;
      buf2[x].Green = brgb.Green<<6;
      buf2[x].Blue  = brgb.Blue<<6;
      buf2[x].Alpha = brgb.Alpha;
   }

   if (!Format) Format = &Dest->prvColourFormat;

   srcdata = Bitmap->Data + ((SrcY+1) * Bitmap->LineWidth);
   auto destdata = Dest->Data + (DestY * Dest->LineWidth);
   uint8_t rmask = Format->RedMask   << Format->RedShift;
   uint8_t gmask = Format->GreenMask << Format->GreenShift;
   uint8_t bmask = Format->BlueMask  << Format->BlueShift;

   for (y=0; y < Height - 1; y++) {
      std::swap(buf1, buf2); // Move line 2 to line 1, line 2 then is empty for reading the next row

      // Read the next source line

      if (Bitmap->BytesPerPixel IS 4) {
         buffer = buf2;
         data = srcdata+(SrcX<<2);
         for (x=0; x < Width; x++, data+=4, buffer++) {
            auto colour = ((uint32_t *)data)[0];
            buffer->Red   = ((uint8_t)(colour >> Bitmap->prvColourFormat.RedPos))<<6;
            buffer->Green = ((uint8_t)(colour >> Bitmap->prvColourFormat.GreenPos))<<6;
            buffer->Blue  = ((uint8_t)(colour >> Bitmap->prvColourFormat.BluePos))<<6;
            buffer->Alpha = ((uint8_t)(colour >> Bitmap->prvColourFormat.AlphaPos));
         }
      }
      else if (Bitmap->BytesPerPixel IS 2) {
         buffer = buf2;
         data = srcdata+(SrcX<<1);
         for (x=0; x < Width; x++, data+=2, buffer++) {
            auto colour = ((uint16_t *)data)[0];
            buffer->Red   = Bitmap->unpackRed(colour)<<6;
            buffer->Green = Bitmap->unpackGreen(colour)<<6;
            buffer->Blue  = Bitmap->unpackBlue(colour)<<6;
         }
      }
      else {
         buffer = buf2;
         data = srcdata + (SrcX * Bitmap->BytesPerPixel);
         for (x=0; x < Width; x++, data+=Bitmap->BytesPerPixel, buffer++) {
            Bitmap->ReadUCRIndex(Bitmap, data, &brgb);
            buffer->Red   = brgb.Red<<6;
            buffer->Green = brgb.Green<<6;
            buffer->Blue  = brgb.Blue<<6;
         }
      }

      // Dither

      buffer = buf1;
      data = destdata + (DestX * Dest->BytesPerPixel);
      if (Dest->BytesPerPixel IS 2) {
         for (x=0; x < Width - 1; x++,data+=2,buffer++) {
            brgb.Red   = (buffer->Red>>6) & rmask;
            brgb.Green = (buffer->Green>>6) & gmask;
            brgb.Blue  = (buffer->Blue>>6) & bmask;
            ((uint16_t *)data)[0] = ((brgb.Red>>Dest->prvColourFormat.RedShift) << Dest->prvColourFormat.RedPos) |
                                 ((brgb.Green>>Dest->prvColourFormat.GreenShift) << Dest->prvColourFormat.GreenPos) |
                                 ((brgb.Blue>>Dest->prvColourFormat.BlueShift) << Dest->prvColourFormat.BluePos);
            DITHER_ERROR(brgb.Red, &RGB16::Red, x, y);
            DITHER_ERROR(brgb.Green, &RGB16::Green, x, y);
            DITHER_ERROR(brgb.Blue, &RGB16::Blue, x, y);
         }
      }
      else if (Dest->BytesPerPixel IS 4) {
         for (x=0; x < Width-1; x++,data+=4,buffer++) {
            brgb.Red   = (buffer->Red>>6) & rmask;
            brgb.Green = (buffer->Green>>6) & gmask;
            brgb.Blue  = (buffer->Blue>>6) & bmask;
            ((uint32_t *)data)[0] = Dest->packPixelWB(brgb.Red, brgb.Green, brgb.Blue, buffer->Alpha);
            DITHER_ERROR(brgb.Red, &RGB16::Red, x, y);
            DITHER_ERROR(brgb.Green, &RGB16::Green, x, y);
            DITHER_ERROR(brgb.Blue, &RGB16::Blue, x, y);
         }
      }
      else {
         for (x=0; x < Width - 1; x++,data+=Dest->BytesPerPixel,buffer++) {
            brgb.Red   = (buffer->Red>>6) & rmask;
            brgb.Green = (buffer->Green>>6) & gmask;
            brgb.Blue  = (buffer->Blue>>6) & bmask;
            Dest->DrawUCRIndex(Dest, data, &brgb);
            DITHER_ERROR(brgb.Red, &RGB16::Red, x, y);
            DITHER_ERROR(brgb.Green, &RGB16::Green, x, y);
            DITHER_ERROR(brgb.Blue, &RGB16::Blue, x, y);
         }
      }

      // Draw the last pixel in the row - no downward propagation

      brgb = { uint8_t(buf1[Width-1].Red>>6), uint8_t(buf1[Width-1].Green>>6), uint8_t(buf1[Width-1].Blue>>6), uint8_t(buf1[Width-1].Alpha) };
      Dest->DrawUCRIndex(Dest, destdata + ((Width - 1) * Dest->BytesPerPixel), &brgb);

      srcdata += Bitmap->LineWidth;
      destdata += Dest->LineWidth;
   }

   // Draw the last row of pixels - no leftward propagation

   if (Bitmap != Dest) {
      for (x=0,index=0; x < Width; x++,index+=Dest->BytesPerPixel) {
         brgb = { uint8_t(buf2[x].Red>>6), uint8_t(buf2[x].Green>>6), uint8_t(buf2[x].Blue>>6), uint8_t(buf2[x].Alpha) };
         Dest->DrawUCRIndex(Dest, destdata + (DestX * Dest->BytesPerPixel) + index, &brgb);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Fills a clipped rectangle without checking that the bitmap is initialised, so that BITMAP_Init() can clear a new
// bitmap once its data, colour format and pixel routines are prepared.  Clients must use DrawRectangle().

void fill_rectangle(extBitmap *Bitmap, int X, int Y, const int Width, const int Height, uint32_t Colour)
{
   uint8_t *data;
   uint16_t *word;
   uint32_t *longdata;
   int xend, x, i;

   if (X >= Bitmap->Clip.Right) return;
   if (Y >= Bitmap->Clip.Bottom) return;
   if (X + Width <= Bitmap->Clip.Left) return;
   if (Y + Height <= Bitmap->Clip.Top) return;

   auto w = Width;
   auto h = Height;
   if (X < Bitmap->Clip.Left) {
      w -= Bitmap->Clip.Left - X;
      X = Bitmap->Clip.Left;
   }

   if (Y < Bitmap->Clip.Top) {
      h -= Bitmap->Clip.Top - Y;
      Y = Bitmap->Clip.Top;
   }

   if ((X + w) >= Bitmap->Clip.Right)   w = Bitmap->Clip.Right - X;
   if ((Y + h) >= Bitmap->Clip.Bottom) h = Bitmap->Clip.Bottom - Y;

   uint16_t red   = Bitmap->unpackRed(Colour);
   uint16_t green = Bitmap->unpackGreen(Colour);
   uint16_t blue  = Bitmap->unpackBlue(Colour);

   // Translucent rectangle support

   // Standard rectangle (no translucency) video support

   if ((glDriver) and (glDriver->fillBitmap(Bitmap, X, Y, w, h, Colour) IS ERR::Okay)) return;


   // Standard rectangle data support

   if (!lock_surface(Bitmap, SURFACE_WRITE)) {
      if (!Bitmap->Data) {
         unlock_surface(Bitmap);
         return;
      }

      if (Bitmap->Type IS BMP::CHUNKY) {
         if (Bitmap->BitsPerPixel IS 32) {
            longdata = (uint32_t *)(Bitmap->Data + (Bitmap->LineWidth * Y));
            while (h > 0) {
               std::fill_n(longdata + X, w, Colour);
               longdata = (uint32_t *)(((uint8_t *)longdata) + Bitmap->LineWidth);
               h--;
            }
         }
         else if (Bitmap->BitsPerPixel IS 24) {
            data = Bitmap->Data + (Bitmap->LineWidth * Y);
            X = X + X + X;
            xend = X + w + w + w;
            while (h > 0) {
               for (x=X; x < xend;) {
                  data[x++] = blue; data[x++] = green; data[x++] = red;
               }
               data += Bitmap->LineWidth;
               h--;
            }
         }
         else if ((Bitmap->BitsPerPixel IS 16) or (Bitmap->BitsPerPixel IS 15)) {
            word = (uint16_t *)(Bitmap->Data + (Bitmap->LineWidth * Y));
            while (h > 0) {
               std::fill_n(word + X, w, uint16_t(Colour));
               word = (uint16_t *)(((int8_t *)word) + Bitmap->LineWidth);
               h--;
            }
         }
         else if (Bitmap->BitsPerPixel IS 8) {
            data = Bitmap->Data + (Bitmap->LineWidth * Y);
            while (h > 0) {
               memset(data + X, int(Colour), w);
               data += Bitmap->LineWidth;
               h--;
            }
         }
         else while (h > 0) {
            for (i=X; i < X + w; i++) Bitmap->DrawUCPixel(Bitmap, i, Y, Colour);
            Y++;
            h--;
         }
      }
      else while (h > 0) {
         for (i=X; i < X + w; i++) Bitmap->DrawUCPixel(Bitmap, i, Y, Colour);
         Y++;
         h--;
      }

      unlock_surface(Bitmap);
   }

   return;
}

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
CopyArea: Copies a rectangular area from one bitmap to another.

This function copies rectangular areas from one bitmap to another.  It performs a straight region-copy only, using the
fastest method available.  Bitmaps may be of a different type (e.g. bit depth), however this will result in performance
penalties.  The copy process will respect the clipping region defined in both the source and destination bitmap
objects.

If the `TRANSPARENT` flag is set in the source object, all colours that match the @Bitmap.TransIndex field will be
ignored in the copy operation.

To enable dithering, pass `BAF::DITHER` in the Flags parameter.  The drawing algorithm will use dithering if the source
needs to be down-sampled to the target bitmap's bit depth.  To enable alpha blending, set `BAF::BLEND` (the source bitmap
will also need to have the `BMF::ALPHA_CHANNEL` flag set to indicate that an alpha channel is available).

The quality of 32-bit alpha blending can be improved by selecting the `BAF::LINEAR` flag.  This enables an additional
computation whereby each RGB value is converted to linear sRGB colour space before performing the blend.  The
discernible value of using this option largely depends on the level of opaqueness of either bitmap.  Note that this
option is not usable if either bitmap is already in a linear colourspace (`ERR::InvalidState` will be returned if that
is the case).

-INPUT-
obj(Bitmap) Bitmap: The source bitmap.
obj(Bitmap) Dest: Pointer to the destination bitmap.
int(BAF) Flags: Optional flags.
int X:      The horizontal position of the area to be copied.
int Y:      The vertical position of the area to be copied.
int Width:  The width of the area.
int Height: The height of the area.
int XDest:  The horizontal position to copy the area to.
int YDest:  The vertical position to copy the area to.

-ERRORS-
Okay:
NullArgs: The `Dest` parameter was not specified.
InvalidObject: The `Dest` parameter does not refer to a Bitmap object.
NotInitialised: The source Bitmap has not been initialised.
InvalidState: The `LINEAR` flag was used when at least one bitmap is using a linear colourspace.

-TAGS-
mutates-object, blocking
-END-

*********************************************************************************************************************/

uint8_t validate_clip(CSTRING Header, CSTRING Name, extBitmap *Bitmap)
{
   kt::Log log(Header);

#ifndef NDEBUG // Force break if clipping is wrong (use gdb)
   if (((Bitmap->Clip.Right) > Bitmap->Width) or
       ((Bitmap->Clip.Bottom) > Bitmap->Height) or
       ((Bitmap->Clip.Left) < 0) or
       (Bitmap->Clip.Left >= Bitmap->Clip.Right) or
       (Bitmap->Clip.Top >= Bitmap->Clip.Bottom)) {
      DEBUG_BREAK
   }
#else
   if ((Bitmap->Clip.Right) > Bitmap->Width) {
      log.warning("#%d %s: Invalid right-clip of %d, limited to width of %d.", Bitmap->UID, Name, Bitmap->Clip.Right, Bitmap->Width);
      Bitmap->Clip.Right = Bitmap->Width;
   }

   if ((Bitmap->Clip.Bottom) > Bitmap->Height) {
      log.warning("#%d %s: Invalid bottom-clip of %d, limited to height of %d.", Bitmap->UID, Name, Bitmap->Clip.Bottom, Bitmap->Height);
      Bitmap->Clip.Bottom = Bitmap->Height;
   }

   if ((Bitmap->Clip.Left) < 0) {
      log.warning("#%d %s: Invalid left-clip of %d.", Bitmap->UID, Name, Bitmap->Clip.Left);
      Bitmap->Clip.Left = 0;
   }

   if ((Bitmap->Clip.Top) < 0) {
      log.warning("#%d %s: Invalid top-clip of %d.", Bitmap->UID, Name, Bitmap->Clip.Top);
      Bitmap->Clip.Top = 0;
   }

   if (Bitmap->Clip.Left >= Bitmap->Clip.Right) {
      log.warning("#%d %s: Left clip >= Right clip (%d >= %d)", Bitmap->UID, Name, Bitmap->Clip.Left, Bitmap->Clip.Right);
      return 1;
   }

   if (Bitmap->Clip.Top >= Bitmap->Clip.Bottom) {
      log.warning("#%d %s: Top clip >= Bottom clip (%d >= %d)", Bitmap->UID, Name, Bitmap->Clip.Top, Bitmap->Clip.Bottom);
      return 1;
   }
#endif

   return 0;
}

ERR CopyArea(objBitmap *Source, objBitmap *Dest, BAF Flags, int X, int Y, int Width, int Height, int DestX, int DestY)
{
   kt::Log log(__FUNCTION__);
   RGB8 pixel, srgb;
   int i;
   uint32_t colour;
   uint8_t *data, *srcdata;

   if (!Dest) return ERR::NullArgs;
   if (Dest->classID() != CLASSID::BITMAP) {
      log.warning("Destination #%d is not a Bitmap.", Dest->UID);
      return ERR::InvalidObject;
   }

   auto src = (extBitmap *)Source;
   auto dest = (extBitmap *)Dest;
   if (!src->initialised()) return log.warning(ERR::NotInitialised);

   //log.trace("%dx%d,%dx%d to %dx%d", X, Y, Width, Height, DestX, DestY);

   if (validate_clip(__FUNCTION__, "Source", src)) return ERR::Okay;

   if (Source != Dest) { // Validate the clipping region of the destination
      if (validate_clip(__FUNCTION__, "Dest", (extBitmap *)Dest)) return ERR::Okay;
   }

   if ((Flags & BAF::LINEAR) != BAF::NIL) {
      if ((src->ColourSpace IS CS::LINEAR_RGB) or (Dest->ColourSpace IS CS::LINEAR_RGB)) return log.warning(ERR::InvalidState);
      if ((src->BitsPerPixel != 32) or ((src->Flags & BMF::ALPHA_CHANNEL) IS BMF::NIL)) return log.warning(ERR::InvalidState);
   }

   if (Source IS Dest) { // Use this clipping routine only if we are copying within the same bitmap
      if (X < src->Clip.Left) {
         Width = Width - (src->Clip.Left - X);
         DestX = DestX + (src->Clip.Left - X);
         X = src->Clip.Left;
      }
      else if (X >= src->Clip.Right) {
         log.trace("Clipped: X >= Bitmap->ClipRight (%d >= %d)", X, src->Clip.Right);
         return ERR::Okay;
      }

      if (Y < src->Clip.Top) {
         Height = Height - (src->Clip.Top - Y);
         DestY  = DestY + (src->Clip.Top - Y);
         Y = src->Clip.Top;
      }
      else if (Y >= src->Clip.Bottom) {
         log.trace("Clipped: Y >= Bitmap->ClipBottom (%d >= %d)", Y, src->Clip.Bottom);
         return ERR::Okay;
      }

      // Clip the destination coordinates

      if ((DestX < Dest->Clip.Left)) {
         Width = Width - (Dest->Clip.Left - DestX);
         if (Width < 1) return ERR::Okay;
         X = X + (Dest->Clip.Left - DestX);
         DestX = Dest->Clip.Left;
      }
      else if (DestX >= Dest->Clip.Right) {
         log.trace("Clipped: DestX >= RightClip (%d >= %d)", DestX, Dest->Clip.Right);
         return ERR::Okay;
      }

      if ((DestY < Dest->Clip.Top)) {
         Height = Height - (Dest->Clip.Top - DestY);
         if (Height < 1) return ERR::Okay;
         Y = Y + (Dest->Clip.Top - DestY);
         DestY = Dest->Clip.Top;
      }
      else if (DestY >= Dest->Clip.Bottom) {
         log.trace("Clipped: DestY >= BottomClip (%d >= %d)", DestY, Dest->Clip.Bottom);
         return ERR::Okay;
      }

      // Clip the Width and Height

      if ((DestX + Width)   >= src->Clip.Right)  Width  = src->Clip.Right - DestX;
      if ((DestY + Height)  >= src->Clip.Bottom) Height = src->Clip.Bottom - DestY;

      if ((X + Width)  >= src->Clip.Right)  Width  = src->Clip.Right - X;
      if ((Y + Height) >= src->Clip.Bottom) Height = src->Clip.Bottom - Y;
   }
   else {
      // Check if the destination that we are copying to is within the drawable area.

      if (DestX < Dest->Clip.Left) {
         Width = Width - (Dest->Clip.Left - DestX);
         if (Width < 1) return ERR::Okay;
         X = X + (Dest->Clip.Left - DestX);
         DestX = Dest->Clip.Left;
      }
      else if (DestX >= Dest->Clip.Right) return ERR::Okay;

      if (DestY < Dest->Clip.Top) {
         Height = Height - (Dest->Clip.Top - DestY);
         if (Height < 1) return ERR::Okay;
         Y = Y + (Dest->Clip.Top - DestY);
         DestY = Dest->Clip.Top;
      }
      else if (DestY >= Dest->Clip.Bottom) return ERR::Okay;

      // Check if the source that we are copying from is within its own drawable area.

      if (X < src->Clip.Left) {
         DestX += (src->Clip.Left - X);
         Width = Width - (src->Clip.Left - X);
         if (Width < 1) return ERR::Okay;
         X = src->Clip.Left;
      }
      else if (X >= src->Clip.Right) return ERR::Okay;

      if (Y < src->Clip.Top) {
         DestY += (src->Clip.Top - Y);
         Height = Height - (src->Clip.Top - Y);
         if (Height < 1) return ERR::Okay;
         Y = src->Clip.Top;
      }
      else if (Y >= src->Clip.Bottom) return ERR::Okay;

      // Clip the Width and Height of the source area, based on the imposed clip region.

      if ((DestX + Width)  >= Dest->Clip.Right) Width   = Dest->Clip.Right - DestX;
      if ((DestY + Height) >= Dest->Clip.Bottom) Height = Dest->Clip.Bottom - DestY;
      if ((X + Width)  >= src->Clip.Right)  Width  = src->Clip.Right - X;
      if ((Y + Height) >= src->Clip.Bottom) Height = src->Clip.Bottom - Y;
   }

   if (Width < 1) return ERR::Okay;
   if (Height < 1) return ERR::Okay;

   if (glDriver) {
      if (auto error = glDriver->blitBitmap(dest, src, Flags, X, Y, Width, Height, DestX, DestY);
            error != ERR::NoSupport) {
         if (error IS ERR::Okay) BLIT_STAT(DRIVER, int64_t(Width) * Height);
         return error;
      }
   }

   // GENERIC SOFTWARE BLITTING ROUTINES

   if (((Flags & BAF::BLEND) != BAF::NIL) and (src->BitsPerPixel IS 32) and ((src->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
      // 32-bit alpha blending support

      if (!lock_surface(src, SURFACE_READ)) {
         if (!lock_surface(dest, SURFACE_WRITE)) {
            uint8_t red, green, blue;
            uint16_t alpha;

            if (dest->BitsPerPixel IS 32) { // Both bitmaps are 32 bit
               const uint8_t sA = src->ColourFormat->AlphaPos>>3;
               const uint8_t sR = src->ColourFormat->RedPos>>3;
               const uint8_t sG = src->ColourFormat->GreenPos>>3;
               const uint8_t sB = src->ColourFormat->BluePos>>3;
               const uint8_t dA = dest->ColourFormat->AlphaPos>>3;
               const uint8_t dR = dest->ColourFormat->RedPos>>3;
               const uint8_t dG = dest->ColourFormat->GreenPos>>3;
               const uint8_t dB = dest->ColourFormat->BluePos>>3;

               uint8_t *sdata = src->Data + (Y * src->LineWidth) + (X<<2);
               uint8_t *ddata = dest->Data + (DestY * dest->LineWidth) + (DestX<<2);

#ifdef KOTUKU_SSE2
               // The SIMD blends operate per byte lane, so the channel layout of both bitmaps must match.
               const bool simd_ok = (sR IS dR) and (sG IS dG) and (sB IS dB) and (sA IS dA);
               const auto simd_amask = _mm_set1_epi32(int(uint32_t(0xff) << (sA<<3)));
               const auto simd_alpha_lane = _mm_cmpeq_epi16(_mm_unpacklo_epi8(simd_amask, _mm_setzero_si128()), _mm_set1_epi16(0xff));
#endif
#ifdef KOTUKU_AVX2
               // The AVX2 kernels process groups of eight, so narrower rows skip the call
               const bool use_avx2 = (Width >= 8) and simd_has_avx2();
               const bool linear_avx2 = use_avx2 and simd_ok and ((Flags & BAF::LINEAR) != BAF::NIL);
               const LinearTables *linear = linear_avx2 ? &linear_tables() : nullptr;
#endif

#ifdef DISPLAY_BLIT_STATS
               {
                  const int64_t pixels = int64_t(Width) * Height;
                  if ((Flags & BAF::LINEAR) != BAF::NIL) BLIT_STAT(BLEND32_LINEAR, pixels);
                  else {
                     if ((Flags & BAF::COPY) != BAF::NIL) BLIT_STAT(BLEND32_COPY, pixels);
                     else if (src->Opacity IS 0xff) BLIT_STAT(BLEND32, pixels);
                     else BLIT_STAT(BLEND32_OPACITY, pixels);
                     if ((sR != dR) or (sG != dG) or (sB != dB) or (sA != dA)) BLIT_STAT(BLEND32_NO_SIMD, pixels);
                  }
               }
#endif

               if ((Flags & BAF::COPY) != BAF::NIL) { // Avoids blending in cases where the destination pixel is zero alpha.
                  for (int y=0; y < Height; y++) {
                     uint8_t *sp = sdata, *dp = ddata;
                     if ((Flags & BAF::LINEAR) != BAF::NIL) {
                        int x = 0;
#ifdef KOTUKU_AVX2
                        if (linear_avx2 and (not simd_overlap(sp, dp, 32))) {
                           x = blend_linear_row_avx2<true>(sp, dp, Width, sA<<3, 255, *linear);
                           sp += x<<2;
                           dp += x<<2;
                        }
#endif
                        for (; x < Width; x++) {
                           if (dp[dA]) {
                              if (sp[sA] IS 0xff) ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];
                              else if (auto a = sp[sA]) {
                                 blend_linear32(dp, dR, dG, dB, dA, sp[sR], sp[sG], sp[sB], a);
                              }
                           }
                           else ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];

                           sp += 4;
                           dp += 4;
                        }
                     }
                     else {
                        auto copy_pixel = [&](uint8_t *Src, uint8_t *Dest) {
                           if (Dest[dA]) {
                              if (Src[sA] IS 0xff) ((uint32_t *)Dest)[0] = ((uint32_t *)Src)[0];
                              else if (auto a = Src[sA]) blend_srgb32(Dest, dR, dG, dB, dA, Src[sR], Src[sG], Src[sB], a);
                           }
                           else ((uint32_t *)Dest)[0] = ((uint32_t *)Src)[0];
                        };

                        int x = 0;
#ifdef KOTUKU_SSE2
                        if (simd_ok and (not simd_overlap(sp, dp))) {
#ifdef KOTUKU_AVX2
                           if (use_avx2 and (not simd_overlap(sp, dp, 32))) {
                              x = blend_row32_avx2<true>(sp, dp, Width, sA<<3, 255);
                              sp += x<<2;
                              dp += x<<2;
                           }
#endif
                           const auto zero = _mm_setzero_si128();
                           const auto op = _mm_set1_epi16(255);
                           for (; x + 4 <= Width; x += 4, sp += 16, dp += 16) {
                              auto s = _mm_loadu_si128((const __m128i *)sp);
                              auto d = _mm_loadu_si128((const __m128i *)dp);
                              auto src_zero  = _mm_cmpeq_epi32(_mm_and_si128(s, simd_amask), zero);
                              auto dest_zero = _mm_cmpeq_epi32(_mm_and_si128(d, simd_amask), zero);
                              auto blended = simd_blend4_any(s, d, sA<<3, simd_alpha_lane, simd_amask, op);
                              _mm_storeu_si128((__m128i *)dp, simd_select(dest_zero, s, simd_select(src_zero, d, blended)));
                           }
                        }
#endif
                        for (; x < Width; x++) {
                           copy_pixel(sp, dp);
                           sp += 4;
                           dp += 4;
                        }
                     }
                     sdata += src->LineWidth;
                     ddata += dest->LineWidth;
                  }
               }
               else {
                  while (Height > 0) {
                     uint8_t *sp = sdata, *dp = ddata;
                     if (src->Opacity IS 0xff) {
                        if ((Flags & BAF::LINEAR) != BAF::NIL) {
                           i = 0;
#ifdef KOTUKU_AVX2
                           if (linear_avx2 and (not simd_overlap(sp, dp, 32))) {
                              i = blend_linear_row_avx2<false>(sp, dp, Width, sA<<3, 255, *linear);
                              sp += i<<2;
                              dp += i<<2;
                           }
#endif
                           for (; i < Width; i++) {
                              if (sp[sA] IS 0xff) ((uint32_t *)dp)[0] = ((uint32_t *)sp)[0];
                              else if (auto a = sp[sA]) {
                                 blend_linear32(dp, dR, dG, dB, dA, sp[sR], sp[sG], sp[sB], a);
                              }

                              sp += 4;
                              dp += 4;
                           }
                        }
                        else {
                           auto blend_pixel = [&](uint8_t *Src, uint8_t *Dest) {
                              if (Src[sA] IS 0xff) ((uint32_t *)Dest)[0] = ((uint32_t *)Src)[0];
                              else if (auto a = Src[sA]) blend_srgb32(Dest, dR, dG, dB, dA, Src[sR], Src[sG], Src[sB], a);
                           };

                           i = 0;
#ifdef KOTUKU_SSE2
                           if (simd_ok and (not simd_overlap(sp, dp))) {
#ifdef KOTUKU_AVX2
                              if (use_avx2 and (not simd_overlap(sp, dp, 32))) {
                                 i = blend_row32_avx2<false>(sp, dp, Width, sA<<3, 255);
                                 sp += i<<2;
                                 dp += i<<2;
                              }
#endif
                              const auto zero = _mm_setzero_si128();
                              const auto op = _mm_set1_epi16(255);
                              for (; i + 4 <= Width; i += 4, sp += 16, dp += 16) {
                                 auto s = _mm_loadu_si128((const __m128i *)sp);
                                 auto d = _mm_loadu_si128((const __m128i *)dp);
                                 auto src_zero = _mm_cmpeq_epi32(_mm_and_si128(s, simd_amask), zero);
                                 auto blended = simd_blend4_any(s, d, sA<<3, simd_alpha_lane, simd_amask, op);
                                 _mm_storeu_si128((__m128i *)dp, simd_select(src_zero, d, blended));
                              }
                           }
#endif
                           for (; i < Width; i++) {
                              blend_pixel(sp, dp);
                              sp += 4;
                              dp += 4;
                           }
                        }
                     }
                     else if ((Flags & BAF::LINEAR) != BAF::NIL) {
                        i = 0;
#ifdef KOTUKU_AVX2
                        // Opacity is not clamped by the Bitmap class.  The scalar loop truncates out-of-range results
                        // to 8 bits, which the kernel does not reproduce, and its gathers need indices below 256.

                        if (linear_avx2 and (uint32_t(src->Opacity) <= 255) and (not simd_overlap(sp, dp, 32))) {
                           i = blend_linear_row_avx2<false>(sp, dp, Width, sA<<3, src->Opacity, *linear);
                           sp += i<<2;
                           dp += i<<2;
                        }
#endif
                        for (; i < Width; i++) {
                           if (auto a = sp[sA]) {
                              a = (a * src->Opacity + 0xff)>>8;
                              blend_linear32(dp, dR, dG, dB, dA, sp[sR], sp[sG], sp[sB], a);
                           }

                           sp += 4;
                           dp += 4;
                        }
                     }
                     else {
                        auto blend_pixel = [&](uint8_t *Src, uint8_t *Dest) {
                           if (auto oa = Src[sA]) {
                              const uint8_t a = (oa * src->Opacity + 0xff)>>8;
                              blend_srgb32(Dest, dR, dG, dB, dA, Src[sR], Src[sG], Src[sB], a);
                           }
                        };

                        i = 0;
#ifdef KOTUKU_SSE2
                        if (simd_ok and (not simd_overlap(sp, dp))) {
#ifdef KOTUKU_AVX2
                           if (use_avx2 and (not simd_overlap(sp, dp, 32))) {
                              i = blend_row32_avx2<false>(sp, dp, Width, sA<<3, src->Opacity);
                              sp += i<<2;
                              dp += i<<2;
                           }
#endif
                           const auto zero = _mm_setzero_si128();
                           const auto op = _mm_set1_epi16(src->Opacity);
                           for (; i + 4 <= Width; i += 4, sp += 16, dp += 16) {
                              auto s = _mm_loadu_si128((const __m128i *)sp);
                              auto d = _mm_loadu_si128((const __m128i *)dp);
                              auto src_zero = _mm_cmpeq_epi32(_mm_and_si128(s, simd_amask), zero);
                              auto blended = simd_blend4_any(s, d, sA<<3, simd_alpha_lane, simd_amask, op);
                              _mm_storeu_si128((__m128i *)dp, simd_select(src_zero, d, blended));
                           }
                        }
#endif
                        for (; i < Width; i++) {
                           blend_pixel(sp, dp);
                           sp += 4;
                           dp += 4;
                        }
                     }
                     sdata += src->LineWidth;
                     ddata += dest->LineWidth;
                     Height--;
                  }
               }
            }
            else if (dest->BytesPerPixel IS 2) {
               BLIT_STAT(BLEND_TO16, int64_t(Width) * Height);
               uint16_t *ddata;
               uint32_t *sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
               ddata = (uint16_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<1));
               const PackedFormat dest_format(dest->prvColourFormat);
               while (Height > 0) {
                  blend_row32to16(sdata, ddata, Width, src->prvColourFormat, dest_format, src->Opacity);
                  sdata = (uint32_t *)(((uint8_t *)sdata) + src->LineWidth);
                  ddata = (uint16_t *)(((uint8_t *)ddata) + dest->LineWidth);
                  Height--;
               }
            }
            else {
               BLIT_STAT(BLEND_TO_OTHER, int64_t(Width) * Height);
               uint32_t *sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = sdata[i];
                     alpha = ((uint8_t)(colour >> src->prvColourFormat.AlphaPos));
                     alpha = mul_div255(alpha, src->Opacity); // Scale the source alpha by the overall opacity

                     if (alpha >= BLEND_MAX_THRESHOLD) {
                        pixel.Red   = colour >> src->prvColourFormat.RedPos;
                        pixel.Green = colour >> src->prvColourFormat.GreenPos;
                        pixel.Blue  = colour >> src->prvColourFormat.BluePos;
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                     else if (alpha >= BLEND_MIN_THRESHOLD) { // Skipping alpha 0 avoids a pixel read and write
                        red   = colour >> src->prvColourFormat.RedPos;
                        green = colour >> src->prvColourFormat.GreenPos;
                        blue  = colour >> src->prvColourFormat.BluePos;

                        dest->ReadUCRPixel(dest, DestX + i, DestY, &pixel);
                        pixel.Red   = mix255(red, pixel.Red, alpha);
                        pixel.Green = mix255(green, pixel.Green, alpha);
                        pixel.Blue  = mix255(blue, pixel.Blue, alpha);
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                  }
                  sdata = (uint32_t *)(((uint8_t *)sdata) + src->LineWidth);
                  DestY++;
                  Height--;
               }
            }

            unlock_surface(dest);
         }
         unlock_surface(src);
      }

      return ERR::Okay;
   }
   else if ((src->Flags & BMF::TRANSPARENT) != BMF::NIL) {
      // Transparent colour copying.  In this mode, the alpha component of individual source pixels is ignored

      if (!lock_surface(src, SURFACE_READ)) {
         if (!lock_surface(dest, SURFACE_WRITE)) {
            if (src->Opacity < 255) { // Transparent mask with translucent pixels (consistent blend level)
               BLIT_STAT(KEY_OPACITY, int64_t(Width) * Height);
               const uint32_t opacity = src->Opacity;
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = src->ReadUCPixel(src, X + i, Y);
                     if (colour != (uint32_t)src->TransIndex) {
                        dest->ReadUCRPixel(dest, DestX + i, DestY, &pixel);

                        pixel.Red   = mix255(src->unpackRed(colour), pixel.Red, opacity);
                        pixel.Green = mix255(src->unpackGreen(colour), pixel.Green, opacity);
                        pixel.Blue  = mix255(src->unpackBlue(colour), pixel.Blue, opacity);

                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                  }
                  Y++; DestY++;
                  Height--;
               }
            }
            else if (src->BitsPerPixel IS dest->BitsPerPixel) {
               if (src->BytesPerPixel IS 4) {
                  BLIT_STAT(KEY_32, int64_t(Width) * Height);
                  uint32_t *ddata, *sdata;

                  sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
                  ddata = (uint32_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<2));
                  colour = src->TransIndex;
                  while (Height > 0) {
                     i = 0;
#ifdef KOTUKU_SSE2
                     const auto key = _mm_set1_epi32(int(colour));
                     const int simd_width = simd_overlap(sdata, ddata) ? 0 : Width;
                     for (; i + 4 <= simd_width; i += 4) {
                        auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                        auto d = _mm_loadu_si128((const __m128i *)(ddata + i));
                        _mm_storeu_si128((__m128i *)(ddata + i), simd_select(_mm_cmpeq_epi32(s, key), d, s));
                     }
#endif
                     // The scalar colour-key loops keep their conditional stores.  A select form would vectorise,
                     // but it reads the destination at key pixels, which may be uninitialised in a new bitmap.
                     for (; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                     ddata = (uint32_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint32_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else if (src->BytesPerPixel IS 2) {
                  BLIT_STAT(KEY_16, int64_t(Width) * Height);
                  uint16_t *ddata, *sdata;

                  sdata = (uint16_t *)(src->Data + (Y * src->LineWidth) + (X<<1));
                  ddata = (uint16_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<1));
                  colour = src->TransIndex;
                  while (Height > 0) {
                     i = 0;
#ifdef KOTUKU_SSE2
                     // The 16-bit lane comparison cannot represent larger keys
                     if ((colour <= 0xffff) and (not simd_overlap(sdata, ddata))) {
                        const auto key = _mm_set1_epi16(int16_t(colour));
                        for (; i + 8 <= Width; i += 8) {
                           auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                           auto d = _mm_loadu_si128((const __m128i *)(ddata + i));
                           _mm_storeu_si128((__m128i *)(ddata + i), simd_select(_mm_cmpeq_epi16(s, key), d, s));
                        }
                     }
#endif
                     for (; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                     ddata = (uint16_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint16_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else {
                  BLIT_STAT(KEY_SAME_OTHER, int64_t(Width) * Height);
                  while (Height > 0) {
                     for (int i=0; i < Width; i++) {
                        colour = src->ReadUCPixel(src, X + i, Y);
                        if (colour != (uint32_t)src->TransIndex) dest->DrawUCPixel(dest, DestX + i, DestY, colour);
                     }
                     Y++; DestY++;
                     Height--;
                  }
               }
            }
            else if (src->BitsPerPixel IS 8) {
               BLIT_STAT(KEY_8, int64_t(Width) * Height);
               while (Height > 0) {
                  for (int i=0; i < Width; i++) {
                     colour = src->ReadUCPixel(src, X + i, Y);
                     if (colour != (uint32_t)src->TransIndex) {
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &src->Palette->Col[colour]);
                     }
                  }
                  Y++; DestY++;
                  Height--;
               }
            }
            else {
               BLIT_STAT(KEY_GENERIC, int64_t(Width) * Height);
               while (Height > 0) {
                  for (int i=0; i < Width; i++) {
                     src->ReadUCRPixel(src, X + i, Y, &pixel);
                     if ((pixel.Red != src->TransColour.Red) or (pixel.Green != src->TransColour.Green) or (pixel.Blue != src->TransColour.Blue)) {
                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                  }
                  Y++; DestY++;
                  Height--;
               }
            }

            unlock_surface(dest);
         }
         unlock_surface(src);
      }

      return ERR::Okay;
   }
   else { // Straight copy operation
      if (!lock_surface(src, SURFACE_READ)) {
         if (!lock_surface(dest, SURFACE_WRITE)) {
            if (src->Opacity < 255) { // Translucent draw
               const uint32_t opacity = src->Opacity;

               if ((src->BytesPerPixel IS 4) and (dest->BytesPerPixel IS 4)) {
                  BLIT_STAT(OPACITY32, int64_t(Width) * Height);
                  uint32_t *ddata, *sdata;
                  uint32_t cmp_alpha;

                  sdata = (uint32_t *)(src->Data + (Y * src->LineWidth) + (X<<2));
                  ddata = (uint32_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<2));
                  cmp_alpha = 255 << src->prvColourFormat.AlphaPos;

#ifdef KOTUKU_SSE2
                  // The SIMD path blends per byte lane, so the RGB channels must be byte-aligned and share
                  // the same positions in both bitmaps.
                  const bool simd_ok = (src->prvColourFormat.RedPos IS dest->prvColourFormat.RedPos) and
                     (src->prvColourFormat.GreenPos IS dest->prvColourFormat.GreenPos) and
                     (src->prvColourFormat.BluePos IS dest->prvColourFormat.BluePos) and
                     (((src->prvColourFormat.RedPos | src->prvColourFormat.GreenPos | src->prvColourFormat.BluePos) & 7) IS 0);
                  if (not simd_ok) BLIT_STAT(OPACITY32_NO_SIMD, int64_t(Width) * Height);
                  const uint32_t rgb_bits = (uint32_t(0xff) << dest->prvColourFormat.RedPos) |
                     (uint32_t(0xff) << dest->prvColourFormat.GreenPos) |
                     (uint32_t(0xff) << dest->prvColourFormat.BluePos);
#endif
#ifdef KOTUKU_AVX2
                  // The AVX2 kernels process groups of eight, so narrower rows skip the call
                  const bool use_avx2 = (Width >= 8) and simd_has_avx2();
#endif
                  while (Height > 0) {
                     i = 0;
#ifdef KOTUKU_SSE2
                     if (simd_ok and (not simd_overlap(sdata, ddata))) {
#ifdef KOTUKU_AVX2
                        if (use_avx2 and (not simd_overlap(sdata, ddata, 32))) {
                           i = mix_row32_avx2(sdata, ddata, Width, rgb_bits, cmp_alpha, src->Opacity);
                        }
#endif
                        const auto zero = _mm_setzero_si128();
                        const auto op   = _mm_set1_epi16(src->Opacity);
                        const auto cop  = _mm_set1_epi16(255 - src->Opacity);
                        const auto rgb_mask = _mm_set1_epi32(int(rgb_bits));
                        const auto alpha = _mm_set1_epi32(int(cmp_alpha));
                        for (; i + 4 <= Width; i += 4) {
                           auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                           auto d = _mm_loadu_si128((const __m128i *)(ddata + i));
                           auto blo = _mm_add_epi16(simd_mul_div255(simd_widen8_lo(s), op), simd_mul_div255(simd_widen8_lo(d), cop));
                           auto bhi = _mm_add_epi16(simd_mul_div255(_mm_unpackhi_epi8(s, zero), op), simd_mul_div255(_mm_unpackhi_epi8(d, zero), cop));
                           auto packed = _mm_packus_epi16(blo, bhi);
                           _mm_storeu_si128((__m128i *)(ddata + i), _mm_or_si128(_mm_and_si128(packed, rgb_mask), alpha));
                        }
                     }
#endif
                     for (; i < Width; i++) {
                        const auto &sf = src->prvColourFormat, &df = dest->prvColourFormat;
                        const uint32_t s = sdata[i], d = ddata[i];
                        ddata[i] = (mix255((uint8_t)(s>>sf.RedPos), (uint8_t)(d>>df.RedPos), opacity) << df.RedPos) |
                           (mix255((uint8_t)(s>>sf.GreenPos), (uint8_t)(d>>df.GreenPos), opacity) << df.GreenPos) |
                           (mix255((uint8_t)(s>>sf.BluePos), (uint8_t)(d>>df.BluePos), opacity) << df.BluePos) |
                           cmp_alpha;
                     }
                     ddata = (uint32_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint32_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else if ((src->BytesPerPixel IS 2) and (dest->BytesPerPixel IS 2)) {
                  BLIT_STAT(OPACITY16, int64_t(Width) * Height);
                  uint16_t *ddata, *sdata;

                  sdata = (uint16_t *)(src->Data + (Y * src->LineWidth) + (X<<1));
                  ddata = (uint16_t *)(dest->Data + (DestY * dest->LineWidth) + (DestX<<1));
                  const PackedFormat src_format(src->prvColourFormat), dest_format(dest->prvColourFormat);
                  while (Height > 0) {
                     mix_row16(sdata, ddata, Width, src_format, dest_format, opacity);
                     ddata = (uint16_t *)(((int8_t *)ddata) + dest->LineWidth);
                     sdata = (uint16_t *)(((int8_t *)sdata) + src->LineWidth);
                     Height--;
                  }
               }
               else {
                  BLIT_STAT(OPACITY_GENERIC, int64_t(Width) * Height);
                  while (Height > 0) {
                     for (i=0; i < Width; i++) {
                        src->ReadUCRPixel(src, X + i, Y, &srgb);
                        dest->ReadUCRPixel(dest, DestX + i, DestY, &pixel);

                        pixel.Red   = mix255(srgb.Red, pixel.Red, opacity);
                        pixel.Green = mix255(srgb.Green, pixel.Green, opacity);
                        pixel.Blue  = mix255(srgb.Blue, pixel.Blue, opacity);

                        dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                     }
                     Y++; DestY++;
                     Height--;
                  }
               }
            }
            else if (src->BitsPerPixel IS dest->BitsPerPixel) {
               // Use this fast routine for identical bitmaps

               BLIT_STAT(COPY_SAME, int64_t(Width) * Height);
               srcdata = src->Data + (X * src->BytesPerPixel) + (Y * src->LineWidth);
               data    = dest->Data + (DestX  * dest->BytesPerPixel) + (DestY * dest->LineWidth);
               Width   = Width * src->BytesPerPixel;

               if ((src IS dest) and (DestY >= Y) and (DestY < Y+Height)) {
                  // Copy backwards when we are copying within the same bitmap and there is an overlap.

                  srcdata += src->LineWidth * (Height-1);
                  data    += dest->LineWidth * (Height-1);

                  while (Height > 0) {
                     memmove(data, srcdata, Width);
                     srcdata -= src->LineWidth;
                     data    -= dest->LineWidth;
                     Height--;
                  }
               }
               else {
                  while (Height > 0) {
                     memcpy(data, srcdata, Width);
                     srcdata += src->LineWidth;
                     data    += dest->LineWidth;
                     Height--;
                  }
               }
            }
            else {
               // If the bitmaps do not match then we need to use this slower RGB translation subroutine.

               bool dithered = false;
               if ((Flags & BAF::DITHER) != BAF::NIL) {
                  if ((dest->BitsPerPixel < 24) and
                      ((src->BitsPerPixel > dest->BitsPerPixel) or
                       ((src->BitsPerPixel <= 8) and (dest->BitsPerPixel > 8)))) {
                     if ((src->Flags & BMF::TRANSPARENT) != BMF::NIL);
                     else {
                        BLIT_STAT(COPY_DITHER, int64_t(Width) * Height);
                        dither(src, dest, nullptr, Width, Height, X, Y, DestX, DestY);
                        dithered = TRUE;
                     }
                  }
               }

               if (dithered IS false) {
                  BLIT_STAT(COPY_CONVERT, int64_t(Width) * Height);
                  BLIT_STAT_CONVERT(src->BitsPerPixel, dest->BitsPerPixel, int64_t(Width) * Height);
                  if ((src IS dest) and (DestY >= Y) and (DestY < Y+Height)) {
                     while (Height > 0) {
                        Y += Height - 1;
                        DestY  += Height - 1;
                        for (i=0; i < Width; i++) {
                           src->ReadUCRPixel(src, X + i, Y, &pixel);
                           dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                        }
                        Y--; DestY--;
                        Height--;
                     }
                  }
                  else {
                     while (Height > 0) {
                        for (i=0; i < Width; i++) {
                           src->ReadUCRPixel(src, X + i, Y, &pixel);
                           dest->DrawUCRPixel(dest, DestX + i, DestY, &pixel);
                        }
                        Y++; DestY++;
                        Height--;
                     }
                  }
               }
            }

            unlock_surface(dest);
         }
         unlock_surface(src);
      }

      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FUNCTION-
CopyRawBitmap: Copies graphics data from an arbitrary surface to a bitmap.

This function will copy data from a described surface to a destination bitmap object.  You are required to provide the
function with a full description of the source in a !BitmapSurface structure.

The `X`, `Y`, `Width` and `Height` parameters define the area from the source that you wish to copy.  The `XDest` and
`YDest` parameters define the top left corner that you will blit the graphics to in the destination.

-INPUT-
struct(*BitmapSurface) Surface: Description of the surface source.
obj(Bitmap) Dest: Destination bitmap.
int(CSRF) Flags:  Optional flags.
int X:      Horizontal source coordinate.
int Y:      Vertical source coordinate.
int Width:  Source width.
int Height: Source height.
int XDest:  Horizontal destination coordinate.
int YDest:  Vertical destination coordinate.

-ERRORS-
Okay:
Args:
NullArgs:

-TAGS-
mutates-object, blocking

*********************************************************************************************************************/

template <class INT> uint8_t UnpackSRed(BITMAPSURFACE *S, INT C)  { return (((C >> S->Format.RedPos)   & S->Format.RedMask) << S->Format.RedShift); }
template <class INT> uint8_t UnpackSGreen(BITMAPSURFACE *S,INT C) { return (((C >> S->Format.GreenPos) & S->Format.GreenMask) << S->Format.GreenShift); }
template <class INT> uint8_t UnpackSBlue(BITMAPSURFACE *S, INT C) { return (((C >> S->Format.BluePos)  & S->Format.BlueMask) << S->Format.BlueShift); }
template <class INT> uint8_t UnpackSAlpha(BITMAPSURFACE *S,INT C) { return (((C >> S->Format.AlphaPos) & S->Format.AlphaMask)); }

static uint32_t read_surface8(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   return ((uint8_t *)Surface->Data)[(Surface->LineWidth * Y) + X];
}

static uint32_t read_surface16(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   return ((uint16_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + X + X))[0];
}

static uint32_t read_surface_lsb24(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   uint8_t *data;
   data = (uint8_t *)Surface->Data + (Surface->LineWidth * Y) + (X + X + X);
   return (data[2]<<16) | (data[1]<<8) | data[0];
}

static uint32_t read_surface_msb24(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   uint8_t *data;
   data = (uint8_t *)Surface->Data + (Surface->LineWidth * Y) + (X + X + X);
   return (data[0]<<16) | (data[1]<<8) | data[2];
}

static uint32_t read_surface32(BITMAPSURFACE *Surface, int16_t X, int16_t Y)
{
   return ((uint32_t *)((uint8_t *)Surface->Data + (Surface->LineWidth * Y) + (X<<2)))[0];
}

ERR CopyRawBitmap(BITMAPSURFACE *Surface, objBitmap *Bitmap, CSRF Flags, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   kt::Log log(__FUNCTION__);
   RGB8 pixel, src;
   int i;
   int16_t srcwidth;
   uint32_t colour;
   uint8_t *data, *srcdata;
   uint32_t (*read_surface)(BITMAPSURFACE *, int16_t, int16_t);

   if ((!Surface) or (!Bitmap)) return log.warning(ERR::NullArgs);

   if ((!Surface->Data) or (Surface->LineWidth < 1) or (!Surface->BitsPerPixel)) {
      return log.warning(ERR::Args);
   }

   auto dest = (extBitmap *)Bitmap;
   srcwidth = Surface->LineWidth / Surface->BytesPerPixel;

   // Check if the destination that we are copying to is within the drawable area.

   if ((XDest < Bitmap->Clip.Left)) {
      Width = Width - (Bitmap->Clip.Left - XDest);
      if (Width < 1) return ERR::Okay;
      X = X + (Bitmap->Clip.Left - XDest);
      XDest = Bitmap->Clip.Left;
   }
   else if (XDest >= Bitmap->Clip.Right) return ERR::Okay;

   if ((YDest < Bitmap->Clip.Top)) {
      Height = Height - (Bitmap->Clip.Top - YDest);
      if (Height < 1) return ERR::Okay;
      Y = Y + (Bitmap->Clip.Top - YDest);
      YDest = Bitmap->Clip.Top;
   }
   else if (YDest >= Bitmap->Clip.Bottom) return ERR::Okay;

   // Check if the source that we are blitting from is within its own drawable area.

   if ((Flags & CSRF::CLIP) != CSRF::NIL) {
      if (X < 0) {
         if ((Width += X) < 1) return ERR::Okay;
         X = 0;
      }
      else if (X >= srcwidth) return ERR::Okay;

      if (Y < 0) {
         if ((Height += Y) < 1) return ERR::Okay;
         Y = 0;
      }
      else if (Y >= Surface->Height) return ERR::Okay;
   }

   // Clip the width and height

   if ((XDest + Width)  >= Bitmap->Clip.Right)  Width  = Bitmap->Clip.Right - XDest;
   if ((YDest + Height) >= Bitmap->Clip.Bottom) Height = Bitmap->Clip.Bottom - YDest;

   if ((Flags & CSRF::CLIP) != CSRF::NIL) {
      if ((X + Width)  >= Surface->Clip.Right)  Width  = Surface->Clip.Right - X;
      if ((Y + Height) >= Surface->Clip.Bottom) Height = Surface->Clip.Bottom - Y;
   }

   if (Width < 1) return ERR::Okay;
   if (Height < 1) return ERR::Okay;

   // Adjust coordinates by offset values

   if ((Flags & CSRF::OFFSET) != CSRF::NIL) {
      X += Surface->XOffset;
      Y += Surface->YOffset;
   }

   if ((Flags & CSRF::DEFAULT_FORMAT) != CSRF::NIL) gfx::GetColourFormat(&Surface->Format, Surface->BitsPerPixel, 0, 0, 0, 0);;

   switch(Surface->BytesPerPixel) {
      case 1: read_surface = read_surface8; break;
      case 2: read_surface = read_surface16; break;
      case 3: if (Surface->Format.RedPos IS 16) read_surface = read_surface_lsb24;
              else read_surface = read_surface_msb24;
              break;
      case 4: read_surface = read_surface32; break;
      default: return log.warning(ERR::Args);
   }


   if (!lock_surface((extBitmap *)Bitmap, SURFACE_WRITE)) {
      if (((Flags & CSRF::ALPHA) != CSRF::NIL) and (Surface->BitsPerPixel IS 32)) { // 32-bit alpha blending support
         uint32_t *sdata = (uint32_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<2));

         if (Bitmap->BitsPerPixel IS 32) {
            BLIT_STAT(RAW_BLEND32, int64_t(Width) * Height);
            uint32_t *ddata = (uint32_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<2));

#ifdef KOTUKU_SSE2
            // The SIMD path blends per byte lane, so the RGB channels must be byte-aligned and share the
            // same positions in both the surface and the destination bitmap.
            const bool simd_ok = (Surface->Format.RedPos IS dest->prvColourFormat.RedPos) and
               (Surface->Format.GreenPos IS dest->prvColourFormat.GreenPos) and
               (Surface->Format.BluePos IS dest->prvColourFormat.BluePos) and
               (((Surface->Format.RedPos | Surface->Format.GreenPos | Surface->Format.BluePos | Surface->Format.AlphaPos) & 7) IS 0);
            if (not simd_ok) BLIT_STAT(RAW_BLEND32_NO_SIMD, int64_t(Width) * Height);
#endif
            while (Height > 0) {
               int i = 0;
#ifdef KOTUKU_SSE2
               if (simd_ok and (not simd_overlap(sdata, ddata))) {
                  const auto zero  = _mm_setzero_si128();
                  const auto max16 = _mm_set1_epi16(255);
                  const auto op    = _mm_set1_epi16(Surface->Opacity);
                  const auto rgb_mask = _mm_set1_epi32(int((uint32_t(0xff) << dest->prvColourFormat.RedPos) |
                     (uint32_t(0xff) << dest->prvColourFormat.GreenPos) | (uint32_t(0xff) << dest->prvColourFormat.BluePos)));
                  const auto alpha_or = _mm_set1_epi32(int(uint32_t(0xff) << dest->prvColourFormat.AlphaPos));
                  for (; i + 4 <= Width; i += 4) {
                     auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                     auto d = _mm_loadu_si128((const __m128i *)(ddata + i));

                     // Scale the source alpha by the overall opacity, then build per-pixel masks for the
                     // fully opaque (straight copy) and fully transparent (no-op) cases.

                     auto a8  = simd_broadcast_alpha(s, Surface->Format.AlphaPos);
                     auto alo = simd_mul_div255(simd_widen8_lo(a8), op);
                     auto ahi = simd_mul_div255(_mm_unpackhi_epi8(a8, zero), op);
                     auto a8s = _mm_packus_epi16(alo, ahi);
                     auto opaque = _mm_cmpeq_epi8(a8s, _mm_set1_epi8(char(0xff)));
                     auto transparent = _mm_cmpeq_epi8(a8s, zero);

                     auto blo = _mm_add_epi16(simd_mul_div255(simd_widen8_lo(s), alo),
                                              simd_mul_div255(simd_widen8_lo(d), _mm_sub_epi16(max16, alo)));
                     auto bhi = _mm_add_epi16(simd_mul_div255(_mm_unpackhi_epi8(s, zero), ahi),
                                              simd_mul_div255(_mm_unpackhi_epi8(d, zero), _mm_sub_epi16(max16, ahi)));
                     auto blended = _mm_or_si128(_mm_and_si128(_mm_packus_epi16(blo, bhi), rgb_mask), alpha_or);
                     _mm_storeu_si128((__m128i *)(ddata + i), simd_select(opaque, s, simd_select(transparent, d, blended)));
                  }
               }
#endif
               for (; i < Width; i++) {
                  colour = sdata[i];

                  uint8_t alpha = ((uint8_t)(colour >> Surface->Format.AlphaPos));
                  alpha = mul_div255(alpha, Surface->Opacity); // Scale the source alpha by the overall opacity

                  // The thresholds are kept: an opaque pixel is copied with its own alpha, and an empty one leaves
                  // the destination alpha intact, whereas a blended pixel is written with alpha 255.

                  if (alpha >= BLEND_MAX_THRESHOLD) ddata[i] = colour;
                  else if (alpha >= BLEND_MIN_THRESHOLD) {

                     uint8_t red   = colour >> Surface->Format.RedPos;
                     uint8_t green = colour >> Surface->Format.GreenPos;
                     uint8_t blue  = colour >> Surface->Format.BluePos;

                     colour = ddata[i];
                     uint8_t destred   = colour >> ((extBitmap *)Bitmap)->prvColourFormat.RedPos;
                     uint8_t destgreen = colour >> ((extBitmap *)Bitmap)->prvColourFormat.GreenPos;
                     uint8_t destblue  = colour >> ((extBitmap *)Bitmap)->prvColourFormat.BluePos;

                     ddata[i] = ((extBitmap *)Bitmap)->packPixelWB(mix255(red, destred, alpha),
                        mix255(green, destgreen, alpha), mix255(blue, destblue, alpha));
                  }
               }
               sdata = (uint32_t *)(((uint8_t *)sdata) + Surface->LineWidth);
               ddata = (uint32_t *)(((uint8_t *)ddata) + Bitmap->LineWidth);
               Height--;
            }
         }
         else {
            BLIT_STAT(RAW_BLEND_OTHER, int64_t(Width) * Height);
            while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = sdata[i];
                  uint8_t alpha = ((uint8_t)(colour >> Surface->Format.AlphaPos));
                  alpha = mul_div255(alpha, Surface->Opacity); // Scale the source alpha by the overall opacity

                  if (alpha >= BLEND_MAX_THRESHOLD) {
                     pixel.Red   = colour >> Surface->Format.RedPos;
                     pixel.Green = colour >> Surface->Format.GreenPos;
                     pixel.Blue  = colour >> Surface->Format.BluePos;
                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
                  else if (alpha >= BLEND_MIN_THRESHOLD) { // Skipping alpha 0 avoids a pixel read and write
                     uint8_t red   = colour >> Surface->Format.RedPos;
                     uint8_t green = colour >> Surface->Format.GreenPos;
                     uint8_t blue  = colour >> Surface->Format.BluePos;

                     Bitmap->ReadUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                     pixel.Red   = mix255(red, pixel.Red, alpha);
                     pixel.Green = mix255(green, pixel.Green, alpha);
                     pixel.Blue  = mix255(blue, pixel.Blue, alpha);
                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
               }
               sdata = (uint32_t *)(((uint8_t *)sdata) + Surface->LineWidth);
               YDest++;
               Height--;
            }
         }
      }
      else if ((Flags & CSRF::TRANSPARENT) != CSRF::NIL) {
         // Transparent colour blitting

         BLIT_STAT(RAW_KEY, int64_t(Width) * Height);

         if (((Flags & CSRF::TRANSLUCENT) != CSRF::NIL) and (Surface->Opacity < 255)) {
            // Transparent mask with translucent pixels

            const uint32_t opacity = Surface->Opacity;

            while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  if (colour != (uint32_t)Surface->Colour) {
                     Bitmap->ReadUCRPixel(Bitmap, XDest + i, YDest, &pixel);

                     pixel.Red   = mix255(UnpackSRed(Surface, colour), pixel.Red, opacity);
                     pixel.Green = mix255(UnpackSGreen(Surface, colour), pixel.Green, opacity);
                     pixel.Blue  = mix255(UnpackSBlue(Surface, colour), pixel.Blue, opacity);

                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
               }
               Y++; YDest++;
               Height--;
            }
         }
         else if (Surface->BitsPerPixel IS Bitmap->BitsPerPixel) {
            if (Surface->BytesPerPixel IS 4) {
               uint32_t *sdata = (uint32_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<2));
               uint32_t *ddata = (uint32_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<2));
               colour = Surface->Colour;
               while (Height > 0) {
                  i = 0;
#ifdef KOTUKU_SSE2
                  const auto key = _mm_set1_epi32(int(colour));
                  const int simd_width = simd_overlap(sdata, ddata) ? 0 : Width;
                  for (; i + 4 <= simd_width; i += 4) {
                     auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                     auto d = _mm_loadu_si128((const __m128i *)(ddata + i));
                     _mm_storeu_si128((__m128i *)(ddata + i), simd_select(_mm_cmpeq_epi32(s, key), d, s));
                  }
#endif
                  for (; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                  ddata = (uint32_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint32_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else if (Surface->BytesPerPixel IS 2) {
               uint16_t *ddata, *sdata;

               sdata = (uint16_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<1));
               ddata = (uint16_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<1));
               colour = Surface->Colour;
               while (Height > 0) {
                  i = 0;
#ifdef KOTUKU_SSE2
                  // The 16-bit lane comparison cannot represent larger keys
                  if ((colour <= 0xffff) and (not simd_overlap(sdata, ddata))) {
                     const auto key = _mm_set1_epi16(int16_t(colour));
                     for (; i + 8 <= Width; i += 8) {
                        auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                        auto d = _mm_loadu_si128((const __m128i *)(ddata + i));
                        _mm_storeu_si128((__m128i *)(ddata + i), simd_select(_mm_cmpeq_epi16(s, key), d, s));
                     }
                  }
#endif
                  for (; i < Width; i++) if (sdata[i] != colour) ddata[i] = sdata[i];
                  ddata = (uint16_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint16_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else {
               while (Height > 0) {
                  for (i=0; i < Width; i++) {
                     colour = read_surface(Surface, X + i, Y);
                     if (colour != (uint32_t)Surface->Colour) Bitmap->DrawUCPixel(Bitmap, XDest + i, YDest, colour);
                  }
                  Y++; YDest++;
                  Height--;
               }
            }
         }
         else {
            while (Height > 0) {
               for (i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  if (colour != (uint32_t)Surface->Colour) {
                     pixel.Red   = UnpackSRed(Surface, colour);
                     pixel.Green = UnpackSGreen(Surface, colour);
                     pixel.Blue  = UnpackSBlue(Surface, colour);
                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
               }
               Y++; YDest++;
               Height--;
            }
         }
      }
      else { // Straight copy operation
         if (((Flags & CSRF::TRANSLUCENT) != CSRF::NIL) and (Surface->Opacity < 255)) { // Straight translucent blit
            const uint32_t opacity = Surface->Opacity;

            if ((Surface->BytesPerPixel IS 4) and (Bitmap->BytesPerPixel IS 4)) {
               BLIT_STAT(RAW_OPACITY32, int64_t(Width) * Height);
               uint32_t *ddata, *sdata;

               sdata = (uint32_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<2));
               ddata = (uint32_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<2));

#ifdef KOTUKU_SSE2
               // The SIMD path blends per byte lane, so the RGB channels must be byte-aligned and share
               // the same positions in both the surface and the destination bitmap.
               const bool simd_ok = (Surface->Format.RedPos IS dest->prvColourFormat.RedPos) and
                  (Surface->Format.GreenPos IS dest->prvColourFormat.GreenPos) and
                  (Surface->Format.BluePos IS dest->prvColourFormat.BluePos) and
                  (((Surface->Format.RedPos | Surface->Format.GreenPos | Surface->Format.BluePos) & 7) IS 0);
               const uint32_t rgb_bits = (uint32_t(0xff) << dest->prvColourFormat.RedPos) |
                  (uint32_t(0xff) << dest->prvColourFormat.GreenPos) |
                  (uint32_t(0xff) << dest->prvColourFormat.BluePos);
#endif
#ifdef KOTUKU_AVX2
               // The AVX2 kernels process groups of eight, so narrower rows skip the call
               const bool use_avx2 = (Width >= 8) and simd_has_avx2();
#endif
               while (Height > 0) {
                  int i = 0;
#ifdef KOTUKU_SSE2
                  if (simd_ok and (not simd_overlap(sdata, ddata))) {
#ifdef KOTUKU_AVX2
                     if (use_avx2 and (not simd_overlap(sdata, ddata, 32))) {
                        i = mix_row32_avx2(sdata, ddata, Width, rgb_bits, 0, Surface->Opacity);
                     }
#endif
                     const auto zero = _mm_setzero_si128();
                     const auto op   = _mm_set1_epi16(Surface->Opacity);
                     const auto cop  = _mm_set1_epi16(255 - Surface->Opacity);
                     const auto rgb_mask = _mm_set1_epi32(int(rgb_bits));
                     for (; i + 4 <= Width; i += 4) {
                        auto s = _mm_loadu_si128((const __m128i *)(sdata + i));
                        auto d = _mm_loadu_si128((const __m128i *)(ddata + i));
                        auto blo = _mm_add_epi16(simd_mul_div255(simd_widen8_lo(s), op), simd_mul_div255(simd_widen8_lo(d), cop));
                        auto bhi = _mm_add_epi16(simd_mul_div255(_mm_unpackhi_epi8(s, zero), op), simd_mul_div255(_mm_unpackhi_epi8(d, zero), cop));
                        _mm_storeu_si128((__m128i *)(ddata + i), _mm_and_si128(_mm_packus_epi16(blo, bhi), rgb_mask));
                     }
                  }
#endif
                  for (; i < Width; i++) {
                     const auto &sf = Surface->Format, &df = dest->prvColourFormat;
                     const uint32_t s = sdata[i], d = ddata[i];
                     ddata[i] = (mix255((uint8_t)(s>>sf.RedPos), (uint8_t)(d>>df.RedPos), opacity) << df.RedPos) |
                        (mix255((uint8_t)(s>>sf.GreenPos), (uint8_t)(d>>df.GreenPos), opacity) << df.GreenPos) |
                        (mix255((uint8_t)(s>>sf.BluePos), (uint8_t)(d>>df.BluePos), opacity) << df.BluePos);
                  }
                  ddata = (uint32_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint32_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else if ((Surface->BytesPerPixel IS 2) and (Bitmap->BytesPerPixel IS 2)) {
               BLIT_STAT(RAW_OPACITY_OTHER, int64_t(Width) * Height);
               uint16_t *ddata, *sdata;

               sdata = (uint16_t *)((int8_t *)Surface->Data + (Y * Surface->LineWidth) + (X<<1));
               ddata = (uint16_t *)(Bitmap->Data + (YDest * Bitmap->LineWidth) + (XDest<<1));
               const PackedFormat src_format(Surface->Format), dest_format(*Bitmap->ColourFormat);
               while (Height > 0) {
                  mix_row16(sdata, ddata, Width, src_format, dest_format, opacity);
                  ddata = (uint16_t *)(((int8_t *)ddata) + Bitmap->LineWidth);
                  sdata = (uint16_t *)(((int8_t *)sdata) + Surface->LineWidth);
                  Height--;
               }
            }
            else {
               BLIT_STAT(RAW_OPACITY_OTHER, int64_t(Width) * Height);
               while (Height > 0) {
                  for (int i=0; i < Width; i++) {
                     colour = read_surface(Surface, X + i, Y);
                     src.Red   = UnpackSRed(Surface, colour);
                     src.Green = UnpackSGreen(Surface, colour);
                     src.Blue  = UnpackSBlue(Surface, colour);

                     Bitmap->ReadUCRPixel(Bitmap, XDest + i, YDest, &pixel);

                     pixel.Red   = mix255(src.Red, pixel.Red, opacity);
                     pixel.Green = mix255(src.Green, pixel.Green, opacity);
                     pixel.Blue  = mix255(src.Blue, pixel.Blue, opacity);

                     Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &pixel);
                  }
                  Y++; YDest++;
                  Height--;
               }
            }
         }
         else if (Surface->BitsPerPixel IS Bitmap->BitsPerPixel) {
            // Use this fast routine for identical bitmaps

            BLIT_STAT(RAW_COPY_SAME, int64_t(Width) * Height);
            srcdata = (uint8_t *)Surface->Data + (X * Surface->BytesPerPixel) + (Y * Surface->LineWidth);
            data    = Bitmap->Data + (XDest  * Bitmap->BytesPerPixel) + (YDest * Bitmap->LineWidth);
            Width   = Width * Surface->BytesPerPixel;

            while (Height > 0) {
               memcpy(data, srcdata, Width);
               srcdata += Surface->LineWidth;
               data    += Bitmap->LineWidth;
               Height--;
            }
         }
         else {
            // If the bitmaps do not match then we need to use this slower RGB translation subroutine.

            BLIT_STAT(RAW_COPY_CONVERT, int64_t(Width) * Height);
            BLIT_STAT_CONVERT(Surface->BitsPerPixel, Bitmap->BitsPerPixel, int64_t(Width) * Height);
            while (Height > 0) {
               for (int i=0; i < Width; i++) {
                  colour = read_surface(Surface, X + i, Y);
                  src.Red   = UnpackSRed(Surface, colour);
                  src.Green = UnpackSGreen(Surface, colour);
                  src.Blue  = UnpackSBlue(Surface, colour);
                  Bitmap->DrawUCRPixel(Bitmap, XDest + i, YDest, &src);
               }
               Y++; YDest++;
               Height--;
            }
         }
      }

      unlock_surface((extBitmap *)Bitmap);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
DrawRectangle: Draws rectangles, both filled and unfilled.

This function draws both filled and unfilled rectangles.  The rectangle is drawn to the target bitmap at position
(X, Y) with dimensions determined by the specified `Width` and `Height`.  If the `Flags` parameter defines `BAF::FILL` then
the rectangle will be filled, otherwise only the outline will be drawn.  The colour of the rectangle is determined by
the pixel value in the `Colour` parameter.  Alpha blending is not supported.

-INPUT-
obj(Bitmap) Bitmap: Pointer to the target @Bitmap.
int X:       The left-most coordinate of the rectangle.
int Y:       The top-most coordinate of the rectangle.
int Width:   The width of the rectangle.
int Height:  The height of the rectangle.
uint Colour: The colour value to use for the rectangle.
int(BAF) Flags: Use `FILL` to fill the rectangle.

-TAGS-
mutates-object, blocking

*********************************************************************************************************************/

void DrawRectangle(objBitmap *Target, int X, int Y, const int Width, const int Height, uint32_t Colour, BAF Flags)
{
   kt::Log log(__FUNCTION__);

   auto Bitmap = (extBitmap *)Target;
   if (!Bitmap) return;

   // If we are not going to fill the rectangle, use this routine to draw an outline.

   if (((Flags & BAF::FILL) IS BAF::NIL) and (Width > 1) and (Height > 1)) {
      auto EY = Y + Height - 1;
      if (X >= Bitmap->Clip.Left) gfx::DrawRectangle(Bitmap, X, Y, 1, Height, Colour, Flags|BAF::FILL); // Left
      if (Y >= Bitmap->Clip.Top)  gfx::DrawRectangle(Bitmap, X, Y, Width, 1, Colour, Flags|BAF::FILL); // Top
      if (Y + Height <= Bitmap->Clip.Bottom) gfx::DrawRectangle(Bitmap, X, EY, Width, 1, Colour, Flags|BAF::FILL); // Bottom
      if (X + Width <= Bitmap->Clip.Right)   gfx::DrawRectangle(Bitmap, X+Width-1, Y, 1, Height, Colour, Flags|BAF::FILL);
      return;
   }

   if (!Bitmap->initialised()) { log.warning(ERR::NotInitialised); return; }

   fill_rectangle(Bitmap, X, Y, Width, Height, Colour);
}

/*********************************************************************************************************************

-FUNCTION-
DrawRGBPixel: Draws a 24 bit pixel to a @Bitmap.

This function draws an !RGB8 colour to the `(X, Y)` position of a target @Bitmap.  The function will check the given
coordinates to ensure that the pixel is inside the bitmap's clipping area.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap object.
int X: Horizontal coordinate of the pixel.
int Y: Vertical coordinate of the pixel.
struct(*RGB8) RGB: The colour to be drawn, in RGB format.

-TAGS-
mutates-object

*********************************************************************************************************************/

void DrawRGBPixel(objBitmap *Bitmap, int X, int Y, RGB8 *Pixel)
{
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left)) return;
   if ((Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) return;
   Bitmap->DrawUCRPixel(Bitmap, X, Y, Pixel);
}

/*********************************************************************************************************************

-FUNCTION-
DrawPixel: Draws a single pixel to a bitmap.

This function draws a pixel to the coordinates `(X, Y)` on a bitmap with a colour determined by the `Colour` index.
This function will check the given coordinates to make sure that the pixel is inside the bitmap's clipping area.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap object.
int X: The horizontal coordinate of the pixel.
int Y: The vertical coordinate of the pixel.
uint Colour: The colour value to use for the pixel.

-TAGS-
mutates-object

*********************************************************************************************************************/

void DrawPixel(objBitmap *Bitmap, int X, int Y, uint32_t Colour)
{
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left)) return;
   if ((Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) return;
   Bitmap->DrawUCPixel(Bitmap, X, Y, Colour);
}

/*********************************************************************************************************************

-FUNCTION-
GetColourFormat: Generates the values for a !ColourFormat structure for a given bit depth.

This function will generate the values for a !ColourFormat structure, for either a given bit depth or
customised colour bit values.  The !ColourFormat structure is used by internal bitmap routines to pack and unpack bit
values to and from bitmap memory.

The !ColourFormat structure is supported by the following macros for packing and unpacking colour bit values:

<pre>
Colour = CFPackPixel(Format,Red,Green,Blue)
Colour = CFPackPixelA(Format,Red,Green,Blue,Alpha)
Colour = CFPackAlpha(Format,Alpha)
Red    = CFUnpackRed(Format,Colour)
Green  = CFUnpackGreen(Format,Colour)
Blue   = CFUnpackBlue(Format,Colour)
Alpha  = CFUnpackAlpha(Format,Colour)
</pre>

-INPUT-
struct(*ColourFormat) Format: Pointer to an empty !ColourFormat structure.
int BitsPerPixel: The depth that you would like to generate colour values for.  Ignored if mask values are set.
int RedMask:      Red component bit mask value.  Set this value to zero if the `BitsPerPixel` parameter is used.
int GreenMask:    Green component bit mask value.
int BlueMask:     Blue component bit mask value.
int AlphaMask:    Alpha component bit mask value.

-TAGS-
mutates-input, pure-query

*********************************************************************************************************************/

void GetColourFormat(ColourFormat *Format, int BPP, int RedMask, int GreenMask, int BlueMask, int AlphaMask)
{
   int mask;

   //log.function("R: $%.8x G: $%.8x, B: $%.8x, A: $%.8x", RedMask, GreenMask, BlueMask, AlphaMask);

   if (!RedMask) {
      if (BPP IS 15) {
         RedMask   = 0x7c00;
         GreenMask = 0x03e0;
         BlueMask  = 0x001f;
         AlphaMask = 0x0000;
      }
      else if (BPP IS 16) {
         RedMask   = 0xf800;
         GreenMask = 0x07e0;
         BlueMask  = 0x001f;
         AlphaMask = 0x0000;
      }
      else {
         BPP = 32;
         AlphaMask = 0xff000000;
         RedMask   = 0x00ff0000;
         GreenMask = 0x0000ff00;
         BlueMask  = 0x000000ff;
      }
   }

   // Calculate the lower byte mask and the position (left shift) of the colour

   mask = RedMask;
   Format->RedPos = 0;
   Format->RedShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->RedPos++; }
   Format->RedMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->RedMask)); mask=mask>>1) Format->RedShift++;

   mask = BlueMask;
   Format->BluePos = 0;
   Format->BlueShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->BluePos++; }
   Format->BlueMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->BlueMask)); mask=mask>>1) Format->BlueShift++;

   mask = GreenMask;
   Format->GreenPos = 0;
   Format->GreenShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->GreenPos++; }
   Format->GreenMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->GreenMask)); mask=mask>>1) Format->GreenShift++;

   mask = AlphaMask;
   Format->AlphaPos = 0;
   Format->AlphaShift = 0;
   while ((mask) and (!(mask & 1))) { mask = mask>>1; Format->AlphaPos++; }
   Format->AlphaMask = mask;
   for (mask=0x80; (mask) and (!(mask & Format->AlphaMask)); mask=mask>>1) Format->AlphaShift++;

   Format->BitsPerPixel = BPP;
}

/*********************************************************************************************************************

-FUNCTION-
ReadRGBPixel: Reads a pixel's colour from the target bitmap.

This function reads a pixel from a bitmap surface and returns the value in an !RGB8 structure that remains good up until
the next call to this function.  Zero is returned in the alpha component if the pixel is out of bounds.

This function is thread-safe if the target @Bitmap is locked.

-INPUT-
obj(Bitmap) Bitmap: Pointer to a bitmap object.
int X: The horizontal coordinate of the pixel.
int Y: The vertical coordinate of the pixel.
&struct(RGB8) RGB: The colour values will be stored in this !RGB8 structure.

-TAGS-
api-owns-result, volatile-result, non-null-result

*********************************************************************************************************************/

void ReadRGBPixel(objBitmap *Bitmap, int X, int Y, RGB8 **Pixel)
{
   static thread_local RGB8 pixel;
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left) or
       (Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) {
      pixel.Red = 0; pixel.Green = 0; pixel.Blue = 0; pixel.Alpha = 0;
   }
   else {
      pixel.Alpha = 255;
      Bitmap->ReadUCRPixel(Bitmap, X, Y, &pixel);
   }
   *Pixel = &pixel;
}

/*********************************************************************************************************************

-FUNCTION-
ReadPixel: Reads a pixel's colour from the target bitmap.

This function reads a pixel from a bitmap area and returns its colour index (if the @Bitmap is indexed with a palette)
or its packed pixel value.  Zero is returned if the pixel is out of bounds.

-INPUT-
obj(Bitmap) Bitmap: Pointer to a bitmap object.
int X: The horizontal coordinate of the pixel.
int Y: The vertical coordinate of the pixel.

-RESULT-
uint: The colour value of the pixel will be returned.  Zero is returned if the pixel is out of bounds.

-TAGS-
pure-query

*********************************************************************************************************************/

uint32_t ReadPixel(objBitmap *Bitmap, int X, int Y)
{
   if ((X >= Bitmap->Clip.Right) or (X < Bitmap->Clip.Left) or
       (Y >= Bitmap->Clip.Bottom) or (Y < Bitmap->Clip.Top)) return 0;
   else return Bitmap->ReadUCPixel(Bitmap, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
Resample: Resamples a bitmap by dithering it to a new set of colour masks.

The Resample() function provides a means for resampling a bitmap to a new colour format without changing the actual
bit depth of the image. It uses dithering so as to retain the quality of the image when down-sampling.  This function
is generally used to 'pre-dither' true colour bitmaps in preparation for copying to bitmaps with lower colour quality.

You are required to supply a !ColourFormat structure that describes the colour format that you would like to apply to
the bitmap's image data.

-INPUT-
obj(Bitmap) Bitmap: The bitmap object to be resampled.
struct(*ColourFormat) ColourFormat: The new colour format to be applied to the bitmap.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object

*********************************************************************************************************************/

ERR Resample(objBitmap *Bitmap, ColourFormat *Format)
{
   if ((!Bitmap) or (!Format)) return ERR::NullArgs;

   dither((extBitmap *)Bitmap, (extBitmap *)Bitmap, Format, Bitmap->Width, Bitmap->Height, 0, 0, 0, 0);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetClipRegion: Sets a clipping region for a bitmap object.

The SetClipRegion() method is used to manage the clipping region assigned to a bitmap object.  The `ClipLeft`,
`ClipTop`, `ClipRight` and `ClipBottom` fields in the target `Bitmap` will be updated to reflect
the area covered by the provided parameter values.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap.
int Left:      The horizontal start of the clip region.
int Top:       The vertical start of the clip region.
int Right:     The right-most edge of the clip region.
int Bottom:    The bottom-most edge of the clip region.

-TAGS-
mutates-object

*********************************************************************************************************************/

void SetClipRegion(objBitmap *Bitmap, int Left, int Top, int Right, int Bottom)
{
   Bitmap->Clip.Left   = Left;
   Bitmap->Clip.Top    = Top;
   Bitmap->Clip.Right  = Right;
   Bitmap->Clip.Bottom = Bottom;

   if (Bitmap->Clip.Left < 0) Bitmap->Clip.Left = 0;
   if (Bitmap->Clip.Top  < 0) Bitmap->Clip.Top = 0;
   if (Bitmap->Clip.Right  > Bitmap->Width)  Bitmap->Clip.Right = Bitmap->Width;
   if (Bitmap->Clip.Bottom > Bitmap->Height) Bitmap->Clip.Bottom = Bitmap->Height;
}

/*********************************************************************************************************************

-FUNCTION-
Sync: Waits for the completion of all active bitmap operations.

The Sync() function will wait for all current video operations to complete before it returns.  This ensures that it is
safe to write to video memory with the CPU, preventing any possibility of clashes with the onboard graphics chip.

-INPUT-
obj(Bitmap) Bitmap: Pointer to the bitmap that you want to synchronise or `NULL` to sleep on the graphics accelerator.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

void Sync(objBitmap *Bitmap)
{

}

} // namespace
