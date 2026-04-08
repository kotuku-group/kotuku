
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
   blend32<2,1,0,3,srgb_blend32>(p,cr,cg,cb,alpha);
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
   blend32<2,1,0,3,srgb_blend32_gamma>(p,cr,cg,cb,alpha);
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

   // Generic 32-bit routines.

   static void blendHLine32(agg::pixfmt_psl *Self, int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept
   {
      if (c.a) {
         uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
         uint32_t alpha = (uint32_t(c.a) * (cover + 1)) >> 8;
         if (alpha == 0xff) {
            uint32_t v;
            ((uint8_t *)&v)[Self->mPixelOrder.Red] = c.r;
            ((uint8_t *)&v)[Self->mPixelOrder.Green] = c.g;
            ((uint8_t *)&v)[Self->mPixelOrder.Blue] = c.b;
            ((uint8_t *)&v)[Self->mPixelOrder.Alpha] = c.a;
            do {
               *(uint32_t *)p = v;
               p += sizeof(uint32_t);
            } while(--len);
         }
         else {
            do {
               Self->fBlendPix(p, c.r, c.g, c.b, alpha);
               p += sizeof(uint32_t);
            } while(--len);
         }
      }
   }

   static void blendSolidHSpan32(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 &c, const uint8_t *covers) noexcept
   {
      if (c.a) {
         uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
         do {
            uint32_t alpha = (uint32_t(c.a) * (uint32_t(*covers) + 1)) >> 8;
            if (alpha == 0xff) {
               p[Self->mPixelOrder.Red] = c.r;
               p[Self->mPixelOrder.Green] = c.g;
               p[Self->mPixelOrder.Blue] = c.b;
               p[Self->mPixelOrder.Alpha] = 0xff;
            }
            else Self->fBlendPix(p, c.r, c.g, c.b, alpha);
            p += sizeof(uint32_t);
            ++covers;
         } while(--len);
      }
   }

   static void blendColorHSpan32(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors, const uint8_t *covers, uint8_t cover) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
      if (covers) {
         do {
            Self->fCoverPix(p, colors->r, colors->g, colors->b, colors->a, *covers++);
            p += 4;
            ++colors;
         } while(--len);
      }
      else if (cover == 255) {
         do {
            Self->fCopyPix(p, colors->r, colors->g, colors->b, colors->a);
            p += 4;
            ++colors;
         } while(--len);
      }
      else {
         do {
            Self->fCoverPix(p, colors->r, colors->g, colors->b, colors->a, cover);
            p += 4;
            ++colors;
         } while(--len);
      }
   }

   static void copyColorHSpan32(agg::pixfmt_psl *Self, int x, int y, uint32_t len, const agg::rgba8 *colors) noexcept
   {
      uint8_t *p = Self->mData + (y * Self->mStride) + (x<<2);
      do {
          p[Self->mPixelOrder.Red] = colors->r;
          p[Self->mPixelOrder.Green] = colors->g;
          p[Self->mPixelOrder.Blue] = colors->b;
          p[Self->mPixelOrder.Alpha] = colors->a;
          ++colors;
          p += sizeof(uint32_t);
      } while(--len);
   }

   // Generic 8-bit grey-scale routines

   static void blendHLine8(agg::pixfmt_psl *Self, int x, int y, unsigned len, const agg::rgba8 &c, int8u cover) noexcept
   {
      if (c.a) {
         uint8_t grey = int((c.r * 0.2126) + (c.g * 0.7152) + (c.b * 0.0722));
         uint8_t *p = Self->mData + (y * Self->mStride) + x;
         uint32_t alpha = (uint32_t(c.a) * (cover + 1)) >> 8;
         if (alpha == 0xff) {
            do {
               *p = grey;
               p++;
            } while(--len);
         }
         else {
            do {
               Self->fBlendPix(p, c.r, c.g, c.b, alpha);
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
            if (alpha == 0xff) p[0] = grey;
            else Self->fBlendPix(p, c.r, c.g, c.b, alpha);
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
            Self->fCoverPix(p, colors->r, colors->g, colors->b, colors->a, *covers++);
            p++;
            ++colors;
         } while(--len);
      }
      else if (cover == 255) {
         do {
            Self->fCopyPix(p, colors->r, colors->g, colors->b, colors->a);
            p++;
            ++colors;
         } while(--len);
      }
      else {
         do {
            Self->fCoverPix(p, colors->r, colors->g, colors->b, colors->a, cover);
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