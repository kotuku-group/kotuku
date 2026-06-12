
// NB: The SSE2 span kernels use unaligned loads/stores because span destinations land on arbitrary
// 4-byte pixel boundaries.  If these kernels are ever widened to AVX2 (32-byte accesses), cache-line
// splits double in frequency and alignment starts to matter: pad bitmap strides to a multiple of 32,
// allocate bitmap data 32-byte aligned, and add a scalar peel loop so the SIMD body runs aligned.

#include <cstring>
#include "../link/simd.h"

//#define SKIP_ALPHA_ZERO 1 // Enables skipping translucent pixels when compositing.

extern agg::gamma_lut<uint8_t, uint16_t, 8, 12> glGamma;

static PIXEL_ORDER pxBGRA(2, 1, 0, 3);
static PIXEL_ORDER pxRGBA(0, 1, 2, 3);
static PIXEL_ORDER pxARGB(1, 2, 3, 0);
static PIXEL_ORDER pxAGBR(3, 1, 2, 0);
static PIXEL_ORDER pxBGR(2, 1, 0, 0);
static PIXEL_ORDER pxRGB(0, 1, 2, 0);

//********************************************************************************************************************

// For use when the bitmap data is already in linear RGB space.

struct linear_blend32 {
   const uint8_t oR, oG, oB, oA;

   linear_blend32(uint8_t R, uint8_t G, uint8_t B, uint8_t A) : oR(R), oG(G), oB(B), oA(A) { }

   inline void operator()(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) const noexcept {
      p[oR] = glLinearRGB.invert(((glLinearRGB.convert(p[oR]) * (0xff-ca)) + (glLinearRGB.convert(cr) * ca) + 0xff)>>8);
      p[oG] = glLinearRGB.invert(((glLinearRGB.convert(p[oG]) * (0xff-ca)) + (glLinearRGB.convert(cg) * ca) + 0xff)>>8);
      p[oB] = glLinearRGB.invert(((glLinearRGB.convert(p[oB]) * (0xff-ca)) + (glLinearRGB.convert(cb) * ca) + 0xff)>>8);
      p[oA] = 0xff - (((0xff - ca) * (0xff - p[oA]))>>8);
   }
};

// The commonly used, if technically incorrect, sRGB blending algorithm.

struct srgb_blend32 {
   const uint8_t oR, oG, oB, oA;

   srgb_blend32(uint8_t R, uint8_t G, uint8_t B, uint8_t A) : oR(R), oG(G), oB(B), oA(A) { }

   inline void operator()(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) const noexcept {
      p[oR] = ((p[oR] * (0xff-ca)) + (cr * ca) + 0xff)>>8;
      p[oG] = ((p[oG] * (0xff-ca)) + (cg * ca) + 0xff)>>8;
      p[oB] = ((p[oB] * (0xff-ca)) + (cb * ca) + 0xff)>>8;
      p[oA] = 0xff - (((0xff - ca) * (0xff - p[oA]))>>8); // The W3C's SVG sanctioned method for the alpha channel :)
   }
};

// Gamma correct blending.  To be used only when alpha < 255

struct srgb_blend32_gamma {
   const uint8_t oR, oG, oB, oA;

   srgb_blend32_gamma(uint8_t R, uint8_t G, uint8_t B, uint8_t A) : oR(R), oG(G), oB(B), oA(A) { }

   inline void operator()(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) const noexcept {
      const uint8_t dest_alpha = p[oA];
      const uint8_t alpha_inv = 0xff - ca;
      const uint32_t a5 = alpha_inv * dest_alpha;
      const uint32_t final_alpha = 0xff - ((alpha_inv * (0xff - dest_alpha))>>8);

      if (final_alpha > 0) {
          const uint32_t a4 = 0xff * ca;
          const uint32_t a6 = 0xff * final_alpha;

          const uint32_t r3 = (glGamma.dir(cr) * a4 + glGamma.dir(p[oR]) * a5) / a6;
          const uint32_t g3 = (glGamma.dir(cg) * a4 + glGamma.dir(p[oG]) * a5) / a6;
          const uint32_t b3 = (glGamma.dir(cb) * a4 + glGamma.dir(p[oB]) * a5) / a6;

          p[oR] = glGamma.inv(r3 < glGamma.hi_res_mask ? r3 : glGamma.hi_res_mask);
          p[oG] = glGamma.inv(g3 < glGamma.hi_res_mask ? g3 : glGamma.hi_res_mask);
          p[oB] = glGamma.inv(b3 < glGamma.hi_res_mask ? b3 : glGamma.hi_res_mask);
          p[oA] = final_alpha;
      }
      else ((uint32_t *)p)[0] = 0;
   }
};

//********************************************************************************************************************
// Blend the pixel at (p) with the provided colour values and store the result back in (p)

template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA, typename DrawPixel>
void blend32(uint8_t *p, uint8_t Red, uint8_t Green, uint8_t Blue, uint8_t Alpha) noexcept
{
   if (p[oA]) DrawPixel{oR,oG,oB,oA}(p,Red,Green,Blue,Alpha);
   else {
      p[oR] = Red;
      p[oG] = Green;
      p[oB] = Blue;
      p[oA] = Alpha;
   }
}

template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA, typename DrawPixel>
void copy32(uint8_t *p, uint8_t Red, uint8_t Green, uint8_t Blue, uint8_t Alpha) noexcept
{
   if (Alpha) {
      if ((Alpha == 0xff) or (!p[oA])) {
         p[oR] = Red;
         p[oG] = Green;
         p[oB] = Blue;
         p[oA] = Alpha;
      }
      else DrawPixel{oR,oG,oB,oA}(p,Red,Green,Blue,Alpha);
   }
}

template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA, typename DrawPixel>
void cover32(uint8_t *p, uint8_t Red, uint8_t Green, uint8_t Blue, uint8_t Alpha, uint32_t cover) noexcept
{
   if (cover == 255) copy32<oR,oG,oB,oA,DrawPixel>(p, Red, Green, Blue, Alpha);
   else if (Alpha) {
      Alpha = (Alpha * (cover + 1)) >> 8;
      if ((Alpha == 0xff) or (!p[oA])) {
         p[oR] = Red;
         p[oG] = Green;
         p[oB] = Blue;
         p[oA] = Alpha;
      }
      else DrawPixel{oR,oG,oB,oA}(p,Red,Green,Blue,Alpha);
   }
}

//********************************************************************************************************************
// sRGB blend operations

static void srgb32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<2,1,0,3,srgb_blend32>(p, cr, cg, cb, alpha);
}

static void srgb32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<0,1,2,3,srgb_blend32>(p,cr,cg,cb,alpha);
}

static void srgb32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<3,1,2,0,srgb_blend32>(p,cr,cg,cb,alpha);
}

static void srgb32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<1,2,3,0,srgb_blend32>(p,cr,cg,cb,alpha);
}

// Gamma correct blend operations

static void gamma32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<2,1,0,3,srgb_blend32_gamma>(p, cr, cg, cb, alpha);
}

static void gamma32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<0,1,2,3,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

static void gamma32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<3,1,2,0,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

static void gamma32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<1,2,3,0,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

// Linear version of the blend operations

static void linear32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<2,1,0,3,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linear32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<0,1,2,3,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linear32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<3,1,2,0,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linear32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   blend32<1,2,3,0,linear_blend32>(p,cr,cg,cb,alpha);
}

//********************************************************************************************************************
// sRGB copy and cover operations

static void srgbCopy32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<2,1,0,3,srgb_blend32>(p,cr,cg,cb,alpha);
}

static void srgbCover32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<2,1,0,3,srgb_blend32>(p,cr,cg,cb,alpha,cover);
}

static void srgbCopy32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<0,1,2,3,srgb_blend32>(p,cr,cg,cb,alpha);
}

static void srgbCover32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<0,1,2,3,srgb_blend32>(p,cr,cg,cb,alpha,cover);
}

static void srgbCopy32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<3,1,2,0,srgb_blend32>(p,cr,cg,cb,alpha);
}

static void srgbCover32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<3,1,2,0,srgb_blend32>(p,cr,cg,cb,alpha,cover);
}

static void srgbCopy32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<1,2,3,0,srgb_blend32>(p,cr,cg,cb,alpha);
}

static void srgbCover32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<1,2,3,0,srgb_blend32>(p,cr,cg,cb,alpha,cover);
}

// Gamma correct copy and cover operations

static void gammaCopy32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<2,1,0,3,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

static void gammaCover32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<2,1,0,3,srgb_blend32_gamma>(p,cr,cg,cb,alpha,cover);
}

static void gammaCopy32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<0,1,2,3,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

static void gammaCover32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<0,1,2,3,srgb_blend32_gamma>(p,cr,cg,cb,alpha,cover);
}

static void gammaCopy32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<3,1,2,0,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

static void gammaCover32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<3,1,2,0,srgb_blend32_gamma>(p,cr,cg,cb,alpha,cover);
}

static void gammaCopy32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<1,2,3,0,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
}

static void gammaCover32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<1,2,3,0,srgb_blend32_gamma>(p,cr,cg,cb,alpha,cover);
}

// Linear copy and cover operations

static void linearCopy32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<2,1,0,3,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linearCover32BGRA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<2,1,0,3,linear_blend32>(p,cr,cg,cb,alpha,cover);
}

static void linearCopy32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<0,1,2,3,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linearCover32RGBA(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<0,1,2,3,linear_blend32>(p,cr,cg,cb,alpha,cover);
}

static void linearCopy32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<3,1,2,0,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linearCover32AGBR(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<3,1,2,0,linear_blend32>(p,cr,cg,cb,alpha,cover);
}

static void linearCopy32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept {
   copy32<1,2,3,0,linear_blend32>(p,cr,cg,cb,alpha);
}

static void linearCover32ARGB(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept {
   cover32<1,2,3,0,linear_blend32>(p,cr,cg,cb,alpha,cover);
}

//********************************************************************************************************************

namespace agg {

class pixfmt_psl
{
public:
   typedef agg::rgba8 color_type;
   typedef typename agg::rendering_buffer::row_data row_data;

   pixfmt_psl() {}

   explicit pixfmt_psl(objBitmap &Bitmap, BLM BlendMode = BLM::AUTO) {
      setBitmap(Bitmap, BlendMode);
   }

   explicit pixfmt_psl(uint8_t *Data, int Width, int Height, int Stride, int BPP, ColourFormat &Format, BLM BlendMode = BLM::AUTO) {
      rawBitmap(Data, Width, Height, Stride, BPP, Format, BlendMode);
   }

   void setBitmap(objBitmap &, BLM BlendMode = BLM::AUTO) noexcept;
   void rawBitmap(uint8_t *Data, int Width, int Height, int Stride, int BitsPerPixel, ColourFormat &, BLM BlendMode = BLM::AUTO) noexcept;

   // The setBitmap() code in scene_draw.cpp defines the following functions.
   void (*fBlendPix)(uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t) noexcept;
   void (*fCopyPix)(uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t) noexcept;
   void (*fCoverPix)(uint8_t *, uint8_t, uint8_t, uint8_t, uint8_t, uint32_t) noexcept;
   void (*fBlendHLine)(agg::pixfmt_psl *, int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept;
   void (*fBlendSolidHSpan)(agg::pixfmt_psl *, int x, int y, uint32_t len, const agg::rgba8 &, const uint8_t *covers) noexcept;
   void (*fBlendColorHSpan)(agg::pixfmt_psl *, int x, int y, uint32_t len, const agg::rgba8 *, const uint8_t *covers, uint8_t cover) noexcept;
   void (*fCopyColorHSpan)(agg::pixfmt_psl *, int x, int y, uint32_t len, const agg::rgba8 *) noexcept; // copy_color_hspan

   inline unsigned width()  const { return mWidth;  }
   inline unsigned height() const { return mHeight; }
   inline int      stride() const { return mStride; }
   inline uint8_t *   row_ptr(int y) { return mData + (y * mStride); }
   inline const uint8_t * row_ptr(int y) const { return mData + (y * mStride); }

   uint8_t *mData;
   int mWidth, mHeight, mStride;
   uint8_t mBytesPerPixel;
   PIXEL_ORDER mPixelOrder;

private:
   void pixel_order(PIXEL_ORDER PixelOrder)
   {
      mPixelOrder = PixelOrder;
   }

   inline static void fill32(uint8_t *Buffer, unsigned Length, uint32_t Value) noexcept
   {
#ifdef KOTUKU_SSE2
      const __m128i vv = _mm_set1_epi32(int(Value));
      while (Length >= 4) {
         _mm_storeu_si128((__m128i *)Buffer, vv);
         Buffer += 16;
         Length -= 4;
      }
#endif
      while (Length) {
         *(uint32_t *)Buffer = Value;
         Buffer += sizeof(uint32_t);
         --Length;
      }
   }

   // 32-bit span routines.  The pixel order and per-pixel operations are template parameters so
   // that the per-pixel work inlines into the span loop; function pointer indirection is paid once
   // per span instead of once per pixel.

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA, auto BlendPix>
   static void blendHLine32T(agg::pixfmt_psl *Self, int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept
   {
      if (c.a) {
         uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
         uint32_t alpha = (uint32_t(c.a) * (cover + 1)) >> 8;
         if (alpha IS 0xff) {
            uint32_t v;
            ((uint8_t *)&v)[oR] = c.r;
            ((uint8_t *)&v)[oG] = c.g;
            ((uint8_t *)&v)[oB] = c.b;
            ((uint8_t *)&v)[oA] = c.a;
            fill32(p, len, v);
         }
         else {
            do {
               BlendPix(p, c.r, c.g, c.b, alpha);
               p += sizeof(uint32_t);
            } while(--len);
         }
      }
   }

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA, auto BlendPix>
   static void blendSolidHSpan32T(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 &c, const uint8_t *covers) noexcept
   {
      if (c.a) {
         uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
         do {
            uint32_t alpha = (uint32_t(c.a) * (uint32_t(*covers) + 1)) >> 8;
            if (alpha IS 0xff) {
               p[oR] = c.r;
               p[oG] = c.g;
               p[oB] = c.b;
               p[oA] = 0xff;
            }
            else BlendPix(p, c.r, c.g, c.b, alpha);
            p += sizeof(uint32_t);
            ++covers;
         } while(--len);
      }
   }

   template <auto CopyPix, auto CoverPix>
   static void blendColorHSpan32T(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors, const uint8_t *covers, uint8_t cover) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
      if (covers) {
         do {
            CoverPix(p, colors->r, colors->g, colors->b, colors->a, *covers++);
            p += 4;
            ++colors;
         } while(--len);
      }
      else if (cover IS 255) {
         do {
            CopyPix(p, colors->r, colors->g, colors->b, colors->a);
            p += 4;
            ++colors;
         } while(--len);
      }
      else {
         do {
            CoverPix(p, colors->r, colors->g, colors->b, colors->a, cover);
            p += 4;
            ++colors;
         } while(--len);
      }
   }

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA>
   static void copyColorHSpan32T(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
      do {
          p[oR] = colors->r;
          p[oG] = colors->g;
          p[oB] = colors->b;
          p[oA] = colors->a;
          ++colors;
          p += sizeof(uint32_t);
      } while(--len);
   }

#ifdef KOTUKU_SSE2

   // SSE2 paths for the sRGB blend mode, processing four pixels per iteration with 16-bit lane
   // arithmetic that reproduces the scalar srgb_blend32/blend32 formulae exactly.  Trailing pixels
   // are handled by the original scalar logic.  The linear and gamma blend modes depend on LUT
   // conversions per channel and remain scalar.

   // Builds the 16-bit lane pattern of the source colour for two pixels.  Alpha lanes are left at
   // zero; they are populated per-pixel by the kernel.

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA>
   inline static __m128i srcLanes16(const agg::rgba8 &c) noexcept
   {
      alignas(16) uint16_t lanes[8] = {};
      lanes[oR] = c.r; lanes[oG] = c.g; lanes[oB] = c.b;
      lanes[4+oR] = c.r; lanes[4+oG] = c.g; lanes[4+oB] = c.b;
      return _mm_load_si128((const __m128i *)lanes);
   }

   template <uint8_t oA>
   inline static __m128i alphaLaneMask16() noexcept
   {
      alignas(16) uint16_t lanes[8] = {};
      lanes[oA] = 0xffff;
      lanes[4+oA] = 0xffff;
      return _mm_load_si128((const __m128i *)lanes);
   }

   // Blends two pixels held in 16-bit lanes (d) against the solid source colour (src16) with the
   // per-pixel alpha replicated across each pixel's four lanes (a).  Reproduces blend32's
   // semantics: pixels with zero destination alpha (or a saturated source alpha) take the source
   // colour directly, all others use the sRGB blend formulae.

   template <uint8_t oA>
   inline static __m128i srgbBlendPair16(__m128i d, __m128i a, __m128i src16, __m128i amask) noexcept
   {
      const __m128i v255 = _mm_set1_epi16(255);
      const __m128i zero = _mm_setzero_si128();
      const __m128i inv  = _mm_sub_epi16(v255, a);

      // Colour lanes: (d*(255-a) + c*a + 255) >> 8.  The product sum cannot exceed 16 bits
      // because inv + a IS 255.
      __m128i blended = _mm_add_epi16(_mm_mullo_epi16(d, inv), _mm_mullo_epi16(src16, a));
      blended = _mm_srli_epi16(_mm_add_epi16(blended, v255), 8);

      // Alpha lanes: 255 - (((255-a) * (255-d)) >> 8)
      const __m128i q    = _mm_sub_epi16(v255, d);
      const __m128i ares = _mm_sub_epi16(v255, _mm_srli_epi16(_mm_mullo_epi16(inv, q), 8));
      __m128i res = _mm_or_si128(_mm_and_si128(amask, ares), _mm_andnot_si128(amask, blended));

      // Direct write applies when the source alpha is saturated or the destination alpha is zero.
      const __m128i direct = _mm_or_si128(src16, _mm_and_si128(a, amask));
      constexpr int sh = _MM_SHUFFLE(oA, oA, oA, oA);
      const __m128i pa   = _mm_shufflehi_epi16(_mm_shufflelo_epi16(d, sh), sh);
      const __m128i cond = _mm_or_si128(_mm_cmpeq_epi16(a, v255), _mm_cmpeq_epi16(pa, zero));
      return _mm_or_si128(_mm_and_si128(cond, direct), _mm_andnot_si128(cond, res));
   }

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA>
   static void srgbBlendSolidHSpan32S(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 &c, const uint8_t *covers) noexcept
   {
      if (!c.a) return;
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);

      const __m128i zero  = _mm_setzero_si128();
      const __m128i src16 = srcLanes16<oR,oG,oB,oA>(c);
      const __m128i amask = alphaLaneMask16<oA>();
      const __m128i ca16  = _mm_set1_epi16(short(c.a));
      const __m128i one16 = _mm_set1_epi16(1);

      while (len >= 4) {
         const __m128i d8 = _mm_loadu_si128((const __m128i *)p);

         // Per-pixel alpha: (c.a * (cover+1)) >> 8, then replicated across each pixel's lanes.
         uint32_t cv;
         std::memcpy(&cv, covers, 4);
         const __m128i cvv = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(cv)), zero);
         const __m128i aV  = _mm_srli_epi16(_mm_mullo_epi16(_mm_add_epi16(cvv, one16), ca16), 8);
         const __m128i tt  = _mm_unpacklo_epi16(aV, aV);

         const __m128i r_lo = srgbBlendPair16<oA>(_mm_unpacklo_epi8(d8, zero), _mm_unpacklo_epi32(tt, tt), src16, amask);
         const __m128i r_hi = srgbBlendPair16<oA>(_mm_unpackhi_epi8(d8, zero), _mm_unpackhi_epi32(tt, tt), src16, amask);
         _mm_storeu_si128((__m128i *)p, _mm_packus_epi16(r_lo, r_hi));

         p += 16;
         covers += 4;
         len -= 4;
      }

      while (len) {
         uint32_t alpha = (uint32_t(c.a) * (uint32_t(*covers) + 1)) >> 8;
         if (alpha IS 0xff) {
            p[oR] = c.r;
            p[oG] = c.g;
            p[oB] = c.b;
            p[oA] = 0xff;
         }
         else blend32<oR,oG,oB,oA,srgb_blend32>(p, c.r, c.g, c.b, alpha);
         p += 4;
         ++covers;
         --len;
      }
   }

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA>
   static void srgbBlendHLine32S(agg::pixfmt_psl *Self, int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept
   {
      if (!c.a) return;
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
      const uint32_t alpha = (uint32_t(c.a) * (cover + 1)) >> 8;

      if (alpha IS 0xff) {
         uint32_t v;
         ((uint8_t *)&v)[oR] = c.r;
         ((uint8_t *)&v)[oG] = c.g;
         ((uint8_t *)&v)[oB] = c.b;
         ((uint8_t *)&v)[oA] = c.a;
         fill32(p, len, v);
      }
      else {
         const __m128i zero  = _mm_setzero_si128();
         const __m128i src16 = srcLanes16<oR,oG,oB,oA>(c);
         const __m128i amask = alphaLaneMask16<oA>();
         const __m128i a16   = _mm_set1_epi16(short(alpha));

         while (len >= 4) {
            const __m128i d8 = _mm_loadu_si128((const __m128i *)p);
            const __m128i r_lo = srgbBlendPair16<oA>(_mm_unpacklo_epi8(d8, zero), a16, src16, amask);
            const __m128i r_hi = srgbBlendPair16<oA>(_mm_unpackhi_epi8(d8, zero), a16, src16, amask);
            _mm_storeu_si128((__m128i *)p, _mm_packus_epi16(r_lo, r_hi));
            p += 16;
            len -= 4;
         }

         while (len) {
            blend32<oR,oG,oB,oA,srgb_blend32>(p, c.r, c.g, c.b, alpha);
            p += 4;
            --len;
         }
      }
   }

   // Blends two pixels with per-pixel source colours, reproducing cover32's semantics: zero source
   // alpha leaves the destination untouched; otherwise srgbBlendPair16 applies the direct-write and
   // blend rules.  src16 must have its alpha lanes cleared.

   template <uint8_t oA>
   inline static __m128i srgbCoverPair16(__m128i d, __m128i a, __m128i src16, __m128i amask) noexcept
   {
      const __m128i blended = srgbBlendPair16<oA>(d, a, src16, amask);
      const __m128i skip = _mm_cmpeq_epi16(a, _mm_setzero_si128());
      return _mm_or_si128(_mm_and_si128(skip, d), _mm_andnot_si128(skip, blended));
   }

   // SIMD sink for per-pixel colour spans (gradients, images, patterns) in sRGB mode.  Source
   // colours arrive as rgba8 in memory order and are permuted to the destination pixel order with
   // 16-bit lane shuffles whose immediates are compile-time constants.  Scaled alpha is
   // (a * (cover+1)) >> 8, which reduces to the unscaled alpha when cover is 255, so a single
   // kernel serves the covers-array, solid-cover and full-cover dispatch cases.

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA>
   static void srgbBlendColorHSpan32S(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors, const uint8_t *covers, uint8_t cover) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);

      // Inverse channel permutation: destination lane j receives source channel lane_src(j).
      constexpr auto lane_src = [](int j) constexpr {
         return (j IS oR) ? 0 : (j IS oG) ? 1 : (j IS oB) ? 2 : 3;
      };
      constexpr int perm = _MM_SHUFFLE(lane_src(3), lane_src(2), lane_src(1), lane_src(0));
      constexpr int shA  = _MM_SHUFFLE(oA, oA, oA, oA);

      const __m128i zero  = _mm_setzero_si128();
      const __m128i amask = alphaLaneMask16<oA>();
      const __m128i one16 = _mm_set1_epi16(1);
      const __m128i cv16  = _mm_set1_epi16(short(cover));
#ifdef SKIP_ALPHA_ZERO
      const __m128i v255  = _mm_set1_epi16(255);
#endif

      while (len >= 4) {
#ifndef SKIP_ALPHA_ZERO
         const __m128i d8 = _mm_loadu_si128((const __m128i *)p);
#endif
         const __m128i s8 = _mm_loadu_si128((const __m128i *)colors);

         // Unpack four source pixels to 16-bit lanes and permute rgba into the destination order.
         __m128i s_lo = _mm_unpacklo_epi8(s8, zero);
         __m128i s_hi = _mm_unpackhi_epi8(s8, zero);
         s_lo = _mm_shufflehi_epi16(_mm_shufflelo_epi16(s_lo, perm), perm);
         s_hi = _mm_shufflehi_epi16(_mm_shufflelo_epi16(s_hi, perm), perm);

         // Source alpha replicated across each pixel's four lanes.
         const __m128i sa_lo = _mm_shufflehi_epi16(_mm_shufflelo_epi16(s_lo, shA), shA);
         const __m128i sa_hi = _mm_shufflehi_epi16(_mm_shufflelo_epi16(s_hi, shA), shA);

         // Per-pixel cover, replicated across each pixel's four lanes.
         __m128i cv_lo, cv_hi;
         if (covers) {
            uint32_t cv;
            std::memcpy(&cv, covers, 4);
            covers += 4;
            const __m128i cvv = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(cv)), zero);
            const __m128i tt  = _mm_unpacklo_epi16(cvv, cvv);
            cv_lo = _mm_unpacklo_epi32(tt, tt);
            cv_hi = _mm_unpackhi_epi32(tt, tt);
         }
         else {
            cv_lo = cv16;
            cv_hi = cv16;
         }

         // Scaled alpha: (a * (cover+1)) >> 8.  Maximum product is 255*256, within 16 bits.
         const __m128i a_lo = _mm_srli_epi16(_mm_mullo_epi16(sa_lo, _mm_add_epi16(cv_lo, one16)), 8);
         const __m128i a_hi = _mm_srli_epi16(_mm_mullo_epi16(sa_hi, _mm_add_epi16(cv_hi, one16)), 8);

#ifdef SKIP_ALPHA_ZERO
         // Group early-outs.  All four scaled alphas zero: the destination is untouched, so skip
         // the load and store entirely (avoids dirtying unchanged cache lines).  All four
         // saturated: a direct write of the permuted source; a saturated scaled alpha implies a
         // saturated source alpha, so the alpha lanes of s_lo/s_hi are already correct.

         if (_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_or_si128(a_lo, a_hi), zero)) IS 0xffff) {
            p += 16;
            colors += 4;
            len -= 4;
            continue;
         }

         if (_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_and_si128(a_lo, a_hi), v255)) IS 0xffff) {
            _mm_storeu_si128((__m128i *)p, _mm_packus_epi16(s_lo, s_hi));
         }
         else {
            const __m128i d8 = _mm_loadu_si128((const __m128i *)p);
            const __m128i r_lo = srgbCoverPair16<oA>(_mm_unpacklo_epi8(d8, zero), a_lo, _mm_andnot_si128(amask, s_lo), amask);
            const __m128i r_hi = srgbCoverPair16<oA>(_mm_unpackhi_epi8(d8, zero), a_hi, _mm_andnot_si128(amask, s_hi), amask);
            _mm_storeu_si128((__m128i *)p, _mm_packus_epi16(r_lo, r_hi));
         }
#else
         const __m128i r_lo = srgbCoverPair16<oA>(_mm_unpacklo_epi8(d8, zero), a_lo, _mm_andnot_si128(amask, s_lo), amask);
         const __m128i r_hi = srgbCoverPair16<oA>(_mm_unpackhi_epi8(d8, zero), a_hi, _mm_andnot_si128(amask, s_hi), amask);
         _mm_storeu_si128((__m128i *)p, _mm_packus_epi16(r_lo, r_hi));
#endif
         p += 16;
         colors += 4;
         len -= 4;
      }

      while (len) {
         cover32<oR,oG,oB,oA,srgb_blend32>(p, colors->r, colors->g, colors->b, colors->a, covers ? *covers++ : cover);
         p += 4;
         ++colors;
         --len;
      }
   }

#endif // KOTUKU_SSE2

   // Binds the full set of 32-bit operations for a pixel order and blend mode in one step.
   // BlendPix/CopyPix/CoverPix must match the order given by oR/oG/oB/oA.

   template <uint8_t oR, uint8_t oG, uint8_t oB, uint8_t oA, auto BlendPix, auto CopyPix, auto CoverPix, bool SRGB = false>
   void set_ops32(const PIXEL_ORDER &Order) noexcept
   {
      pixel_order(Order);
      fBlendPix        = BlendPix;
      fCopyPix         = CopyPix;
      fCoverPix        = CoverPix;
#ifdef KOTUKU_SSE2
      if constexpr (SRGB) {
         fBlendHLine      = &srgbBlendHLine32S<oR,oG,oB,oA>;
         fBlendSolidHSpan = &srgbBlendSolidHSpan32S<oR,oG,oB,oA>;
         fBlendColorHSpan = &srgbBlendColorHSpan32S<oR,oG,oB,oA>;
      }
      else {
         fBlendHLine      = &blendHLine32T<oR,oG,oB,oA,BlendPix>;
         fBlendSolidHSpan = &blendSolidHSpan32T<oR,oG,oB,oA,BlendPix>;
         fBlendColorHSpan = &blendColorHSpan32T<CopyPix,CoverPix>;
      }
#else
      fBlendHLine      = &blendHLine32T<oR,oG,oB,oA,BlendPix>;
      fBlendSolidHSpan = &blendSolidHSpan32T<oR,oG,oB,oA,BlendPix>;
      fBlendColorHSpan = &blendColorHSpan32T<CopyPix,CoverPix>;
#endif
      fCopyColorHSpan  = &copyColorHSpan32T<oR,oG,oB,oA>;
   }

   // Generic 8-bit grey-scale routines.  8-bit targets always use blend8/copy8/cover8, so these
   // call them directly rather than through the per-pixel function pointers.

   static void blendHLine8(agg::pixfmt_psl *Self, int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept
   {
      if (c.a) {
         uint8_t grey = int((c.r * 0.2126) + (c.g * 0.7152) + (c.b * 0.0722));
         uint8_t *p = Self->mData + (y * Self->mStride) + x;
         uint32_t alpha = (uint32_t(c.a) * (cover + 1)) >> 8;
         if (alpha IS 0xff) {
            do {
               *p = grey;
               p++;
            } while(--len);
         }
         else {
            do {
               blend8(p, c.r, c.g, c.b, alpha);
               p++;
            } while(--len);
         }
      }
   }

   static void blendSolidHSpan8(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 &c, const uint8_t *covers) noexcept
   {
      if (c.a) {
         uint8_t grey = int((c.r * 0.2126) + (c.g * 0.7152) + (c.b * 0.0722));
         uint8_t *p = Self->mData + (y * Self->mStride) + x;
         do {
            uint32_t alpha = (uint32_t(c.a) * (uint32_t(*covers) + 1)) >> 8;
            if (alpha IS 0xff) p[0] = grey;
            else blend8(p, c.r, c.g, c.b, alpha);
            p++;
            ++covers;
         } while(--len);
      }
   }

   static void blendColorHSpan8(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors, const uint8_t *covers, uint8_t cover) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + x;
      if (covers) {
         do {
            cover8(p, colors->r, colors->g, colors->b, colors->a, *covers++);
            p++;
            ++colors;
         } while(--len);
      }
      else if (cover IS 255) {
         do {
            copy8(p, colors->r, colors->g, colors->b, colors->a);
            p++;
            ++colors;
         } while(--len);
      }
      else {
         do {
            cover8(p, colors->r, colors->g, colors->b, colors->a, cover);
            p++;
            ++colors;
         } while(--len);
      }
   }

   static void copyColorHSpan8(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + x;
      do {
          p[0] = int((colors->r * 0.2126) + (colors->g * 0.7152) + (colors->b * 0.0722));
          ++colors;
          p++;
      } while(--len);
   }

   // Standard 8-bit routines

   static void blend8(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept
   {
      uint8_t grey = int((cr * 0.2126) + (cg * 0.7152) + (cb * 0.0722));
      p[0] = ((p[0] * (0xff-alpha)) + (grey * alpha) + 0xff)>>8;
   }

   inline static void copy8(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) noexcept
   {
      if (alpha) {
         uint8_t grey = int((cr * 0.2126) + (cg * 0.7152) + (cb * 0.0722));
         if (alpha == 0xff) p[0] = grey;
         else p[0] = ((p[0] * (0xff-alpha)) + (grey * alpha) + 0xff)>>8;
      }
   }

   static void cover8(uint8_t *p, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha, uint32_t cover) noexcept
   {
      if (cover == 255) {
         if (alpha) {
            uint8_t grey = int((cr * 0.2126) + (cg * 0.7152) + (cb * 0.0722));
            if (alpha == 0xff) p[0] = grey;
            else p[0] = ((p[0] * (0xff-alpha)) + (grey * alpha) + 0xff)>>8;
         }
      }
      else if (alpha) {
         uint8_t grey = int((cr * 0.2126) + (cg * 0.7152) + (cb * 0.0722));
         alpha = (alpha * (cover + 1)) >> 8;
         if (alpha == 0xff) p[0] = grey;
         else p[0] = ((p[0] * (0xff-alpha)) + (grey * alpha) + 0xff)>>8;
      }
   }

public:

   inline void blend_hline(int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept
   {
      fBlendHLine(this, x, y, len, c, cover);
   }

   inline void blend_solid_hspan(int x, int y, uint32_t len, const agg::rgba8 &c, const uint8_t *covers) noexcept
   {
      fBlendSolidHSpan(this, x, y, len, c, covers);
   }

   inline void copy_color_hspan(int x, int y, uint32_t len, const agg::rgba8 *colors) noexcept
   {
      fCopyColorHSpan(this, x, y, len, colors);
   }

   inline void blend_color_hspan(int x, int y, uint32_t len, const agg::rgba8 *colors, const uint8_t *covers, uint8_t cover) noexcept
   {
      fBlendColorHSpan(this, x, y, len, colors, covers, cover);
   }

   inline void blend_color_vspan(int x, int y, uint32_t len, const agg::rgba8 *colors, const uint8_t *covers, uint8_t cover) noexcept
   {
      uint8_t *p = (uint8_t *)mData + (y * mStride) + (x * mBytesPerPixel);
      if (covers) {
         do {
            fCoverPix(p, colors->r, colors->g, colors->b, colors->a, *covers++);
            p += mStride;
            ++colors;
         } while(--len);
      }
      else if (cover == 255) {
         do {
            fCopyPix(p, colors->r, colors->g, colors->b, colors->a);
            p += mStride;
            ++colors;
         } while(--len);
      }
      else {
         do {
            fCoverPix(p, colors->r, colors->g, colors->b, colors->a, cover);
            p += mStride;
            ++colors;
         } while(--len);
      }
   }
};

} // namespace
