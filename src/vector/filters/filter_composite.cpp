/*********************************************************************************************************************

-CLASS-
CompositeFX: Composite two sources together with a mixing algorithm.

This filter combines the @FilterEffect.Input and @FilterEffect.Mix sources using either one of the Porter-Duff
compositing operations, or a colour blending algorithm.  The Input has priority and will be placed in the foreground
for ordered operations such as `ATOP` and `OVER`.

-END-

*********************************************************************************************************************/

class extCompositeFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPOSITEFX;
   static constexpr CSTRING CLASS_NAME = "CompositeFX";
   using create = kt::Create<extCompositeFX>;

   double K1 = 0, K2 = 0, K3 = 0, K4 = 0; // For the arithmetic operator
   OP Operator = OP::OVER; // OP constant

   extCompositeFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   template <class CompositeOp, bool Linear>
   void doMix(const objBitmap *InBitmap, const objBitmap *MixBitmap, uint8_t *Dest, const uint8_t *In,
      const uint8_t *Mix) {
      const uint8_t A = Target->ColourFormat->AlphaPos>>3;
      const uint8_t R = Target->ColourFormat->RedPos>>3;
      const uint8_t G = Target->ColourFormat->GreenPos>>3;
      const uint8_t B = Target->ColourFormat->BluePos>>3;

      int height = Target->Clip.Bottom - Target->Clip.Top;
      int width  = Target->Clip.Right - Target->Clip.Left;
      if (InBitmap->Clip.Right - InBitmap->Clip.Left < width) width = InBitmap->Clip.Right - InBitmap->Clip.Left;
      if (InBitmap->Clip.Bottom - InBitmap->Clip.Top < height) height = InBitmap->Clip.Bottom - InBitmap->Clip.Top;

      for (int y=0; y < height; y++) {
         auto dp = Dest;
         auto sp = In;
         auto mp = Mix;
         for (int x=0; x < width; x++) {
            CompositeOp::template blend<Linear>(dp, sp, mp, A, R, G, B);
            dp += 4;
            sp += 4;
            mp += 4;
         }
         Dest += Target->LineWidth;
         In   += InBitmap->LineWidth;
         Mix  += MixBitmap->LineWidth;
      }
   }

};

// The equations use the same 16-bit range for sRGB and linear RGB.  Only linear RGB needs a transfer function.

template <bool Linear>
static inline uint16_t composite_decode(uint8_t Value)
{
   if constexpr (Linear) return glLinearRGB.convert16(Value);
   else return uint16_t(Value * 257u);
}

template <bool Linear>
static inline uint8_t composite_encode(uint16_t Value)
{
   if constexpr (Linear) return glLinearRGB.invert16(Value);
   else return uint8_t((Value + 128u) / 257u);
}

static inline uint32_t linear_alpha(const uint8_t Alpha)
{
   return uint32_t(Alpha) * 257u;
}

static inline uint16_t linear_clamp(const int64_t Value)
{
   return uint16_t((Value < 0) ? 0 : (Value > 0xffff) ? 0xffff : Value);
}

static inline uint16_t linear_scale_alpha(const uint32_t Colour, const uint32_t Alpha)
{
   return linear_clamp(int64_t((uint64_t(Colour) * Alpha + 0xffu) >> 8));
}

static inline uint16_t linear_multiply(const uint32_t A, const uint32_t B)
{
   return uint16_t((uint64_t(A) * B + 0xffffu) >> 16);
}

//********************************************************************************************************************
// Porter/Duff Compositing routines
// For reference, this Wikipedia page explains it best: https://en.wikipedia.org/wiki/Alpha_compositing
//
// D = Dest; S = Source; M = Mix (equates to Dest as a pixel source)

struct composite_over {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (!M[A]) ((uint32_t *)D)[0] = ((uint32_t *)S)[0];
      else if (!S[A]) ((uint32_t *)D)[0] = ((uint32_t *)M)[0];
      else {
         const uint32_t dA = S[A] + M[A] - ((S[A] * M[A] + 0xff)>>8);
         const uint32_t sA = S[A] + (S[A] >> 7); // 0..255 -> 0..256
         const uint32_t cA = 256 - sA;
         const uint32_t mA = M[A] + (M[A] >> 7); // 0..255 -> 0..256

         D[R] = composite_encode<Linear>(linear_clamp(((composite_decode<Linear>(S[R]) * sA +
            ((composite_decode<Linear>(M[R]) * mA * cA)>>8))>>8) * 255 / dA));
         D[G] = composite_encode<Linear>(linear_clamp(((composite_decode<Linear>(S[G]) * sA +
            ((composite_decode<Linear>(M[G]) * mA * cA)>>8))>>8) * 255 / dA));
         D[B] = composite_encode<Linear>(linear_clamp(((composite_decode<Linear>(S[B]) * sA +
            ((composite_decode<Linear>(M[B]) * mA * cA)>>8))>>8) * 255 / dA));
         D[A] = dA;
      }
   }
};

struct composite_in {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (M[A] IS 255) ((uint32_t *)D)[0] = ((uint32_t *)S)[0];
      else {
         D[R] = S[R];
         D[G] = S[G];
         D[B] = S[B];
         D[A] = (S[A] * M[A] + 0xff)>>8;
      }
   }
};

struct composite_out {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (!M[A]) ((uint32_t *)D)[0] = ((uint32_t *)S)[0];
      else {
         D[R] = S[R];
         D[G] = S[G];
         D[B] = S[B];
         D[A] = (S[A] * (0xff - M[A]) + 0xff)>>8;
      }
   }
};

// S is on top and blended with M as a background.  S is obscured if M is not present.  Mix alpha has priority in the
// output.  S alpha is ignored except for blending with M.

struct composite_atop {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (auto m_alpha = M[A]) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         const uint8_t sA  = S[A];
         const uint8_t scA = 0xff - sA;

         D[R] = composite_encode<Linear>(((sR * sA) + (mR * scA) + 0xff)>>8);
         D[G] = composite_encode<Linear>(((sG * sA) + (mG * scA) + 0xff)>>8);
         D[B] = composite_encode<Linear>(((sB * sA) + (mB * scA) + 0xff)>>8);
         D[A] = m_alpha;
      }
   }
};

struct composite_xor {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      auto sR = composite_decode<Linear>(S[R]);
      auto sG = composite_decode<Linear>(S[G]);
      auto sB = composite_decode<Linear>(S[B]);

      auto mR = composite_decode<Linear>(M[R]);
      auto mG = composite_decode<Linear>(M[G]);
      auto mB = composite_decode<Linear>(M[B]);

      const uint8_t s1a = 0xff - S[A];
      const uint8_t d1a = 0xff - M[A];
      D[R] = composite_encode<Linear>(linear_clamp(((mR * s1a) + (sR * d1a) + 0xff) >> 8));
      D[G] = composite_encode<Linear>(linear_clamp(((mG * s1a) + (sG * d1a) + 0xff) >> 8));
      D[B] = composite_encode<Linear>(linear_clamp(((mB * s1a) + (sB * d1a) + 0xff) >> 8));
      D[A] = (S[A] + M[A] - ((S[A] * M[A] + (0xff>>1)) >> (8 - 1)));
   }
};

//********************************************************************************************************************
// Blending algorithms, refer to https://en.wikipedia.org/wiki/Blend_modes

struct blend_screen {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      auto sR = composite_decode<Linear>(S[R]);
      auto sG = composite_decode<Linear>(S[G]);
      auto sB = composite_decode<Linear>(S[B]);

      auto mR = composite_decode<Linear>(M[R]);
      auto mG = composite_decode<Linear>(M[G]);
      auto mB = composite_decode<Linear>(M[B]);

      D[R] = composite_encode<Linear>(linear_clamp(sR + mR - linear_multiply(sR, mR)));
      D[G] = composite_encode<Linear>(linear_clamp(sG + mG - linear_multiply(sG, mG)));
      D[B] = composite_encode<Linear>(linear_clamp(sB + mB - linear_multiply(sB, mB)));
      D[A] = uint8_t(S[A] + M[A] - ((S[A] * M[A] + 0Xff) >> 8));
   }
};

struct blend_multiply {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         const uint8_t s1a = 0xff - S[A];
         const uint8_t d1a = 0xff - M[A];
         D[R] = composite_encode<Linear>(linear_clamp(linear_multiply(sR, mR) +
            linear_scale_alpha(sR, d1a) + linear_scale_alpha(mR, s1a)));
         D[G] = composite_encode<Linear>(linear_clamp(linear_multiply(sG, mG) +
            linear_scale_alpha(sG, d1a) + linear_scale_alpha(mG, s1a)));
         D[B] = composite_encode<Linear>(linear_clamp(linear_multiply(sB, mB) +
            linear_scale_alpha(sB, d1a) + linear_scale_alpha(mB, s1a)));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_darken {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         uint8_t d1a = 0xff - M[A];
         uint8_t s1a = 0xff - S[A];
         uint8_t da  = M[A];

         D[R] = composite_encode<Linear>((agg::sd_min(sR * da, mR * S[A]) + sR * d1a + mR * s1a + 0xff) >> 8);
         D[G] = composite_encode<Linear>((agg::sd_min(sG * da, mG * S[A]) + sG * d1a + mG * s1a + 0xff) >> 8);
         D[B] = composite_encode<Linear>((agg::sd_min(sB * da, mB * S[A]) + sB * d1a + mB * s1a + 0xff) >> 8);
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_lighten {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         uint8_t d1a = 0xff - M[A];
         uint8_t s1a = 0xff - S[A];

         D[R] = composite_encode<Linear>((agg::sd_max(sR * M[A], mR * S[A]) + sR * d1a + mR * s1a + 0xff) >> 8);
         D[G] = composite_encode<Linear>((agg::sd_max(sG * M[A], mG * S[A]) + sG * d1a + mG * s1a + 0xff) >> 8);
         D[B] = composite_encode<Linear>((agg::sd_max(sB * M[A], mB * S[A]) + sB * d1a + mB * s1a + 0xff) >> 8);
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_dodge {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         uint32_t d1a  = 0xff - M[A];
         uint32_t s1a  = 0xff - S[A];
         uint32_t drsa = mR * S[A];
         uint32_t dgsa = mG * S[A];
         uint32_t dbsa = mB * S[A];
         uint32_t srda = sR * M[A];
         uint32_t sgda = sG * M[A];
         uint32_t sbda = sB * M[A];
         uint32_t sada = S[A] * M[A] * 257u;

         D[R] = composite_encode<Linear>(linear_clamp((srda + drsa >= sada) ?
             (sada + sR * d1a + mR * s1a + 0xff) >> 8 :
             uint64_t(drsa) * 257u / (0xffffu - (sR << 8) / S[A]) +
                ((sR * d1a + mR * s1a + 0xff) >> 8)));

         D[G] = composite_encode<Linear>(linear_clamp((sgda + dgsa >= sada) ?
             (sada + sG * d1a + mG * s1a + 0xff) >> 8 :
             uint64_t(dgsa) * 257u / (0xffffu - (sG << 8) / S[A]) +
                ((sG * d1a + mG * s1a + 0xff) >> 8)));

         D[B] = composite_encode<Linear>(linear_clamp((sbda + dbsa >= sada) ?
             (sada + sB * d1a + mB * s1a + 0xff) >> 8 :
             uint64_t(dbsa) * 257u / (0xffffu - (sB << 8) / S[A]) +
                ((sB * d1a + mB * s1a + 0xff) >> 8)));

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_contrast {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      auto sR = composite_decode<Linear>(S[R]);
      auto sG = composite_decode<Linear>(S[G]);
      auto sB = composite_decode<Linear>(S[B]);

      auto mR = composite_decode<Linear>(M[R]);
      auto mG = composite_decode<Linear>(M[G]);
      auto mB = composite_decode<Linear>(M[B]);

      int32_t d2a = int32_t(linear_alpha(M[A]) >> 1);
      int32_t s2a = int32_t(linear_alpha(S[A]) >> 1);

      auto r = int((int64_t(int32_t(mR) - d2a) * (int32_t(sR) - s2a) * 2 >> 16) + d2a);
      auto g = int((int64_t(int32_t(mG) - d2a) * (int32_t(sG) - s2a) * 2 >> 16) + d2a);
      auto b = int((int64_t(int32_t(mB) - d2a) * (int32_t(sB) - s2a) * 2 >> 16) + d2a);

      r = (r < 0) ? 0 : r;
      g = (g < 0) ? 0 : g;
      b = (b < 0) ? 0 : b;

      D[R] = composite_encode<Linear>((r > int(linear_alpha(M[A]))) ? linear_alpha(M[A]) : r);
      D[G] = composite_encode<Linear>((g > int(linear_alpha(M[A]))) ? linear_alpha(M[A]) : g);
      D[B] = composite_encode<Linear>((b > int(linear_alpha(M[A]))) ? linear_alpha(M[A]) : b);
   }
};

struct blend_overlay {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         uint8_t d1a = 0xff - M[A];
         uint8_t s1a = 0xff - S[A];
         uint32_t sada = S[A] * M[A] * 257u;

         D[R] = composite_encode<Linear>(((2*mR < linear_alpha(M[A])) ?
             uint64_t(2) * sR*mR / 257u + sR*d1a + mR*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mR)*(linear_alpha(S[A]) - sR) / 257u +
                sR*d1a + mR*s1a + 0xff) >> 8);

         D[G] = composite_encode<Linear>(((2*mG < linear_alpha(M[A])) ?
             uint64_t(2) * sG*mG / 257u + sG*d1a + mG*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mG)*(linear_alpha(S[A]) - sG) / 257u +
                sG*d1a + mG*s1a + 0xff) >> 8);

         D[B] = composite_encode<Linear>(((2*mB < linear_alpha(M[A])) ?
             uint64_t(2) * sB*mB / 257u + sB*d1a + mB*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mB)*(linear_alpha(S[A]) - sB) / 257u +
                sB*d1a + mB*s1a + 0xff) >> 8);

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_burn {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         const uint8_t d1a = 0xff - M[A];
         const uint8_t s1a = 0xff - S[A];
         const uint32_t drsa = mR * S[A];
         const uint32_t dgsa = mG * S[A];
         const uint32_t dbsa = mB * S[A];
         const uint32_t srda = sR * M[A];
         const uint32_t sgda = sG * M[A];
         const uint32_t sbda = sB * M[A];
         const uint32_t sada = S[A] * M[A] * 257u;

         D[R] = composite_encode<Linear>(((srda + drsa <= sada) ?
             sR * d1a + mR * s1a :
             uint64_t(S[A]) * (srda + drsa - sada) / sR + sR * d1a + mR * s1a + 0xff) >> 8);

         D[G] = composite_encode<Linear>(((sgda + dgsa <= sada) ?
             sG * d1a + mG * s1a :
             uint64_t(S[A]) * (sgda + dgsa - sada) / sG + sG * d1a + mG * s1a + 0xff) >> 8);

         D[B] = composite_encode<Linear>(((sbda + dbsa <= sada) ?
             sB * d1a + mB * s1a :
             uint64_t(S[A]) * (sbda + dbsa - sada) / sB + sB * d1a + mB * s1a + 0xff) >> 8);

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_hard_light {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         uint8_t d1a  = 0xff - M[A];
         uint8_t s1a  = 0xff - S[A];
         uint32_t sada = S[A] * M[A] * 257u;

         D[R] = composite_encode<Linear>(((2*sR < linear_alpha(S[A])) ?
             uint64_t(2) * sR*mR / 257u + sR*d1a + mR*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mR)*(linear_alpha(S[A]) - sR) / 257u +
                sR*d1a + mR*s1a + 0xff) >> 8);

         D[G] = composite_encode<Linear>(((2*sG < linear_alpha(S[A])) ?
             uint64_t(2) * sG*mG / 257u + sG*d1a + mG*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mG)*(linear_alpha(S[A]) - sG) / 257u +
                sG*d1a + mG*s1a + 0xff) >> 8);

         D[B] = composite_encode<Linear>(((2*sB < linear_alpha(S[A])) ?
             uint64_t(2) * sB*mB / 257u + sB*d1a + mB*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mB)*(linear_alpha(S[A]) - sB) / 257u +
                sB*d1a + mB*s1a + 0xff) >> 8);

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_soft_light {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         double sR = double(composite_decode<Linear>(S[R])) / 65535.0;
         double sG = double(composite_decode<Linear>(S[G])) / 65535.0;
         double sB = double(composite_decode<Linear>(S[B])) / 65535.0;

         double xr = double(composite_decode<Linear>(M[R])) / 65535.0;
         double xg = double(composite_decode<Linear>(M[G])) / 65535.0;
         double xb = double(composite_decode<Linear>(M[B])) / 65535.0;
         double da = double(M[A] ? M[A] : 1) / 255.0;
         double sa = double(S[A]) / 255.0;

         if(2*sR < sa)       xr = xr*(sa + (1 - xr/da)*(2*sR - sa)) + sR*(1 - da) + xr*(1 - sa);
         else if(8*xr <= da) xr = xr*(sa + (1 - xr/da)*(2*sR - sa)*(3 - 8*xr/da)) + sR*(1 - da) + xr*(1 - sa);
         else                xr = (xr*sa + (sqrt(xr/da)*da - xr)*(2*sR - sa)) + sR*(1 - da) + xr*(1 - sa);

         if(2*sG < sa)       xg = xg*(sa + (1 - xg/da)*(2*sG - sa)) + sG*(1 - da) + xg*(1 - sa);
         else if(8*xg <= da) xg = xg*(sa + (1 - xg/da)*(2*sG - sa)*(3 - 8*xg/da)) + sG*(1 - da) + xg*(1 - sa);
         else                xg = (xg*sa + (sqrt(xg/da)*da - xg)*(2*sG - sa)) + sG*(1 - da) + xg*(1 - sa);

         if(2*sB < sa)       xb = xb*(sa + (1 - xb/da)*(2*sB - sa)) + sB*(1 - da) + xb*(1 - sa);
         else if(8*xb <= da) xb = xb*(sa + (1 - xb/da)*(2*sB - sa)*(3 - 8*xb/da)) + sB*(1 - da) + xb*(1 - sa);
         else                xb = (xb*sa + (sqrt(xb/da)*da - xb)*(2*sB - sa)) + sB*(1 - da) + xb*(1 - sa);

         D[R] = composite_encode<Linear>(agg::uround(std::clamp(xr, 0.0, 1.0) * 65535.0));
         D[G] = composite_encode<Linear>(agg::uround(std::clamp(xg, 0.0, 1.0) * 65535.0));
         D[B] = composite_encode<Linear>(agg::uround(std::clamp(xb, 0.0, 1.0) * 65535.0));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_difference {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         D[R] = composite_encode<Linear>(sR + mR - ((2 * agg::sd_min(sR*M[A], mR*S[A]) + 0xff) >> 8));
         D[G] = composite_encode<Linear>(sG + mG - ((2 * agg::sd_min(sG*M[A], mG*S[A]) + 0xff) >> 8));
         D[B] = composite_encode<Linear>(sB + mB - ((2 * agg::sd_min(sB*M[A], mB*S[A]) + 0xff) >> 8));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_exclusion {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto mR = composite_decode<Linear>(M[R]);
         auto mG = composite_decode<Linear>(M[G]);
         auto mB = composite_decode<Linear>(M[B]);

         const uint8_t d1a = 0xff - M[A];
         const uint8_t s1a = 0xff - S[A];
         D[R] = composite_encode<Linear>(linear_clamp(int64_t(linear_scale_alpha(sR, M[A])) +
            linear_scale_alpha(mR, S[A]) - 2 * linear_multiply(sR, mR) +
            linear_scale_alpha(sR, d1a) + linear_scale_alpha(mR, s1a)));
         D[G] = composite_encode<Linear>(linear_clamp(int64_t(linear_scale_alpha(sG, M[A])) +
            linear_scale_alpha(mG, S[A]) - 2 * linear_multiply(sG, mG) +
            linear_scale_alpha(sG, d1a) + linear_scale_alpha(mG, s1a)));
         D[B] = composite_encode<Linear>(linear_clamp(int64_t(linear_scale_alpha(sB, M[A])) +
            linear_scale_alpha(mB, S[A]) - 2 * linear_multiply(sB, mB) +
            linear_scale_alpha(sB, d1a) + linear_scale_alpha(mB, s1a)));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_plus {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         const int xr = composite_decode<Linear>(M[R]) + sR;
         const int xg = composite_decode<Linear>(M[G]) + sG;
         const int xb = composite_decode<Linear>(M[B]) + sB;
         const int xa = M[A] + S[A];
         D[R] = composite_encode<Linear>((xr > 0xffff) ? 0xffff : xr);
         D[G] = composite_encode<Linear>((xg > 0xffff) ? 0xffff : xg);
         D[B] = composite_encode<Linear>((xb > 0xffff) ? 0xffff : xb);
         D[A] = (xa > 0xff) ? 0xff : xa;
      }
   }
};

struct blend_minus {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         const int xr = composite_decode<Linear>(M[R]) - sR;
         const int xg = composite_decode<Linear>(M[G]) - sG;
         const int xb = composite_decode<Linear>(M[B]) - sB;
         D[R] = composite_encode<Linear>((xr < 0) ? 0 : xr);
         D[G] = composite_encode<Linear>((xg < 0) ? 0 : xg);
         D[B] = composite_encode<Linear>((xb < 0) ? 0 : xb);
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
         //D[A] = (UBYTE)(0xff - (((0xff - S[A]) * (0xff - D[A]) + 0xff) >> 8));
      }
   }
};

struct blend_invert {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto dR = composite_decode<Linear>(D[R]);
         auto dG = composite_decode<Linear>(D[G]);
         auto dB = composite_decode<Linear>(D[B]);

         const uint16_t xr = linear_scale_alpha(linear_alpha(M[A]) - dR, S[A]);
         const uint16_t xg = linear_scale_alpha(linear_alpha(M[A]) - dG, S[A]);
         const uint16_t xb = linear_scale_alpha(linear_alpha(M[A]) - dB, S[A]);
         const uint8_t s1a = 0xff - S[A];
         D[R] = composite_encode<Linear>(xr + ((dR * s1a + 0xff) >> 8));
         D[G] = composite_encode<Linear>(xg + ((dG * s1a + 0xff) >> 8));
         D[B] = composite_encode<Linear>(xb + ((dB * s1a + 0xff) >> 8));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_invert_rgb {
   template <bool Linear>
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (S[A]) {
         auto sR = composite_decode<Linear>(S[R]);
         auto sG = composite_decode<Linear>(S[G]);
         auto sB = composite_decode<Linear>(S[B]);

         auto dR = composite_decode<Linear>(D[R]);
         auto dG = composite_decode<Linear>(D[G]);
         auto dB = composite_decode<Linear>(D[B]);

         uint16_t xr = linear_multiply(linear_alpha(M[A]) - dR, sR);
         uint16_t xg = linear_multiply(linear_alpha(M[A]) - dG, sG);
         uint16_t xb = linear_multiply(linear_alpha(M[A]) - dB, sB);
         uint8_t s1a = 0xff - S[A];
         D[R] = composite_encode<Linear>(xr + ((dR * s1a + 0xff) >> 8));
         D[G] = composite_encode<Linear>(xg + ((dG * s1a + 0xff) >> 8));
         D[B] = composite_encode<Linear>(xb + ((dB * s1a + 0xff) >> 8));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

// Arithmetic uses premultiplied channels in the selected colour space.  The bitmap storage is sRGB encoded.

template <bool Linear>
static void composite_arithmetic_rows(const extCompositeFX *Self, int Width, int Height,
   uint8_t *Dest, const uint8_t *In, int InStride, const uint8_t *Mix, int MixStride)
{
   const uint8_t A = Self->Target->ColourFormat->AlphaPos>>3;
   const uint8_t R = Self->Target->ColourFormat->RedPos>>3;
   const uint8_t G = Self->Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->Target->ColourFormat->BluePos>>3;

   constexpr double alpha_scale = 1.0 / 255.0;

   const double k1 = Self->K1, k2 = Self->K2, k3 = Self->K3, k4 = Self->K4;

   static const auto decode = [] {
      std::array<double, 256> table;
      for (size_t i=0; i < table.size(); ++i) {
         if constexpr (Linear) table[i] = double(glLinearRGB.convert16(uint8_t(i))) * (1.0 / 65535.0);
         else table[i] = double(i) * alpha_scale;
      }
      return table;
   }();

   auto encode = [](double Value) -> uint8_t {
      if constexpr (Linear) {
         const int v = int(Value * 65535.0);
         if (v > 0xffff) return 0xff;
         else if (v < 0) return 0;
         else return glLinearRGB.invert16(v);
      }
      else if (Value >= 1.0) return 0xff;
      else if (Value <= 0.0) return 0;
      else return uint8_t(std::lround(Value * 255.0));
   };

   for (int y=0; y < Height; y++) {
      auto dp = Dest + std::ptrdiff_t(y) * Self->Target->LineWidth;
      auto sp = In + std::ptrdiff_t(y) * InStride;
      auto mp = Mix + std::ptrdiff_t(y) * MixStride;
      for (int x=0; x < Width; x++) {
         if ((mp[A]) or (sp[A])) {
            // Scale RGB to 0 - 1.0 and premultiply the values.
            const double sA = double(sp[A]) * alpha_scale;
            const double sR = decode[sp[R]] * sA;
            const double sG = decode[sp[G]] * sA;
            const double sB = decode[sp[B]] * sA;

            const double mA = double(mp[A]) * alpha_scale;
            const double mR = decode[mp[R]] * mA;
            const double mG = decode[mp[G]] * mA;
            const double mB = decode[mp[B]] * mA;

            double dA = (k1 * sA * mA) + (k2 * sA) + (k3 * mA) + k4;

            if (dA > 0.0) {
               if (dA > 1.0) dA = 1.0;

               double demul = 1.0 / dA;
               dp[R] = encode(((k1 * sR * mR) + (k2 * sR) + (k3 * mR) + k4) * demul);
               dp[G] = encode(((k1 * sG * mG) + (k2 * sG) + (k3 * mG) + k4) * demul);
               dp[B] = encode(((k1 * sB * mB) + (k2 * sB) + (k3 * mB) + k4) * demul);
               dp[A] = int(dA * 255.0);
            }
         }

         dp += 4;
         sp += 4;
         mp += 4;
      }
   }
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR COMPOSITEFX_Draw(extCompositeFX *Self, struct acDraw *Args)
{
   if ((not Self->Target) or (Self->Target->BytesPerPixel != 4)) return ERR::InvalidState;

   filter_surface_view input, mixture;
   if (auto error = get_source_view(Self, input, filter_input_role::INPUT); error != ERR::Okay) return error;
   if (auto error = get_source_view(Self, mixture, filter_input_role::MIX); error != ERR::Okay) return error;

   auto first_pixel = [](const filter_surface_view &View) {
      return View.row(View.Valid.top) + std::ptrdiff_t(View.Valid.left) * 4;
   };

   auto dest    = Self->Target->Data + (Self->Target->Clip.Left * 4) + (Self->Target->Clip.Top * Self->Target->LineWidth);
   auto in_bmp  = input.Bitmap;
   auto mix_bmp = mixture.Bitmap;
   auto in      = first_pixel(input);
   auto mix     = first_pixel(mixture);

   const bool linear = effect_colour_space(Self) IS VCS::LINEAR_RGB;
   auto mix_pixels = [&]<class Operation>(Operation) {
      if (linear) Self->doMix<Operation, true>(in_bmp, mix_bmp, dest, in, mix);
      else Self->doMix<Operation, false>(in_bmp, mix_bmp, dest, in, mix);
   };

   switch (Self->Operator) {
      case OP::OVER: mix_pixels(composite_over{}); break;
      case OP::IN:   mix_pixels(composite_in{}); break;
      case OP::OUT:  mix_pixels(composite_out{}); break;
      case OP::ATOP: mix_pixels(composite_atop{}); break;
      case OP::XOR:  mix_pixels(composite_xor{}); break;

      case OP::ARITHMETIC: {
         int height = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
         int width  = Self->Target->Clip.Right - Self->Target->Clip.Left;
         if (in_bmp->Clip.Right - in_bmp->Clip.Left < width) width = in_bmp->Clip.Right - in_bmp->Clip.Left;
         if (in_bmp->Clip.Bottom - in_bmp->Clip.Top < height) height = in_bmp->Clip.Bottom - in_bmp->Clip.Top;

         const auto arithmetic_rows = linear ? composite_arithmetic_rows<true> : composite_arithmetic_rows<false>;

         auto render_rows = [&](int StartY, int EndY) {
            auto dp = dest + std::ptrdiff_t(StartY) * Self->Target->LineWidth;
            auto sp = in + std::ptrdiff_t(StartY) * in_bmp->LineWidth;
            auto mp = mix + std::ptrdiff_t(StartY) * mix_bmp->LineWidth;
            arithmetic_rows(Self, width, EndY - StartY, dp, sp, in_bmp->LineWidth, mp, mix_bmp->LineWidth);
         };

         constexpr size_t MIN_PIXELS_PER_WORKER = 32 * 1024;
         constexpr int MIN_ROWS_PER_WORKER = 4;
         if ((width <= 0) or (height <= 0)) break;

         const size_t blocks = std::min(size_t(height / MIN_ROWS_PER_WORKER),
            (size_t(width) * height) / MIN_PIXELS_PER_WORKER);
         const size_t workers = (blocks < 2) ? blocks : std::min(blocks, size_t(std::thread::hardware_concurrency()));

         if (workers < 2) render_rows(0, height);
         else {
            BS::thread_pool pool(workers);
            pool.detach_blocks(0, height, render_rows);
            pool.wait();
         }

         break;
      }

      default: { // These mix routines use pre-multiplied content.
         filter_surface_view in_work, mix_work;

         if (auto error = premultiplied_input(Self, input, CS::SRGB, in_work); error != ERR::Okay) return error;

         if (input.Bitmap IS mixture.Bitmap) mix_work = in_work;
         else if (auto error = premultiplied_input(Self, mixture, CS::SRGB, mix_work); error != ERR::Okay) {
            return error;
         }

         in_bmp  = in_work.Bitmap;
         mix_bmp = mix_work.Bitmap;
         in      = first_pixel(in_work);
         mix     = first_pixel(mix_work);
         switch (Self->Operator) {
            case OP::MULTIPLY:    mix_pixels(blend_multiply{}); break;
            case OP::SCREEN:      mix_pixels(blend_screen{}); break;
            case OP::DARKEN:      mix_pixels(blend_darken{}); break;
            case OP::LIGHTEN:     mix_pixels(blend_lighten{}); break;
            case OP::OVERLAY:     mix_pixels(blend_overlay{}); break;
            case OP::BURN:        mix_pixels(blend_burn{}); break;
            case OP::DODGE:       mix_pixels(blend_dodge{}); break;
            case OP::HARD_LIGHT:  mix_pixels(blend_hard_light{}); break;
            case OP::SOFT_LIGHT:  mix_pixels(blend_soft_light{}); break;
            case OP::DIFFERENCE:  mix_pixels(blend_difference{}); break;
            case OP::EXCLUSION:   mix_pixels(blend_exclusion{}); break;
            case OP::PLUS:        mix_pixels(blend_plus{}); break;
            case OP::MINUS:       mix_pixels(blend_minus{}); break;
            case OP::CONTRAST:    mix_pixels(blend_contrast{}); break;
            case OP::INVERT:      mix_pixels(blend_invert{}); break;
            case OP::INVERT_RGB:  mix_pixels(blend_invert_rgb{}); break;
            default: return ERR::NoSupport;
         }

         if (not linear) {
            // The sRGB blend equations produce premultiplied values.  Filter results use straight RGBA.
            Self->Target->Flags |= BMF::PREMUL;
            return Self->Target->demultiply();
         }
         break;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPOSITEFX_Init(extCompositeFX *Self)
{
   if (Self->MixType IS VSF::NIL) {
      kt::Log().warning("A MixType input is required.");
      return ERR::FieldNotSet;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
K1: Input value for the arithmetic operation.

The `ARITHMETIC` operator computes `result = K1 * i1 * i2 + K2 * i1 + K3 * i2 + K4` for each premultiplied colour
component, where `i1` is the @FilterEffect.Input and `i2` is the @FilterEffect.Mix.

-FIELD-
K2: Input value for the arithmetic operation.

Refer to #K1 for the arithmetic formula.

-FIELD-
K3: Input value for the arithmetic operation.

Refer to #K1 for the arithmetic formula.

-FIELD-
K4: Input value for the arithmetic operation.

Refer to #K1 for the arithmetic formula.

-FIELD-
Operator: The compositing algorithm to use for rendering.
Lookup: OP

Setting the Operator will determine the algorithm that is used for compositing.  The default is `OVER`.

Operators that mix colour components use @FilterEffect.ColourSpace.  They inherit the parent @VectorFilter's colour
space by default.  The `IN` and `OUT` operators only modify the alpha channel.

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR composite_xml(const extCompositeFX &Effect, filter_xml_element &Element)
{
   constexpr std::string_view operators[] = { "over", "in", "out", "atop", "xor", "arithmetic" };
   const auto op = size_t(Effect.Operator);
   const bool arithmetic = Effect.Operator IS OP::ARITHMETIC;
   const std::pair<CSTRING, double> coefficients[] = {
      { "k1", Effect.K1 }, { "k2", Effect.K2 }, { "k3", Effect.K3 }, { "k4", Effect.K4 }
   };
   for (const auto &[name, value] : coefficients) {
      if (arithmetic) Element.attribute(name, value);
      else if (value != 0) Element.attribute(std::string("kotuku:") + name, value);
   }
   if (op < std::size(operators)) {
      Element.Name = "feComposite";
      Element.attribute("operator", operators[op]);
      return ERR::Okay;
   }

   Element.Name = "feBlend";
   switch (Effect.Operator) {
      case OP::SCREEN: Element.attribute("mode", "screen"); break;
      case OP::MULTIPLY: Element.attribute("mode", "multiply"); break;
      case OP::LIGHTEN: Element.attribute("mode", "lighten"); break;
      case OP::DARKEN: Element.attribute("mode", "darken"); break;
      case OP::OVERLAY: Element.attribute("mode", "overlay"); break;
      case OP::HARD_LIGHT: Element.attribute("mode", "hard-light"); break;
      case OP::SOFT_LIGHT: Element.attribute("mode", "soft-light"); break;
      case OP::DODGE: Element.attribute("mode", "color-dodge"); break;
      case OP::BURN: Element.attribute("mode", "color-burn"); break;
      case OP::DIFFERENCE: Element.attribute("mode", "difference"); break;
      case OP::EXCLUSION: Element.attribute("mode", "exclusion"); break;
      case OP::INVERT_RGB: Element.attribute("kotuku:mode", "invertRGB"); break;
      case OP::INVERT: Element.attribute("kotuku:mode", "invert"); break;
      case OP::CONTRAST: Element.attribute("kotuku:mode", "contrast"); break;
      case OP::PLUS: Element.attribute("kotuku:mode", "plus"); break;
      case OP::MINUS: Element.attribute("kotuku:mode", "minus"); break;
      default: return ERR::InvalidValue;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_composite_def.c"

//********************************************************************************************************************
// Writable fields that would otherwise be written directly report their changes, so that buffered viewports redraw
// the filter's clients.

static ERR COMPOSITEFX_SET_K1(extCompositeFX *Self, double Value)
{
   Self->K1 = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPOSITEFX_SET_K2(extCompositeFX *Self, double Value)
{
   Self->K2 = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPOSITEFX_SET_K3(extCompositeFX *Self, double Value)
{
   Self->K3 = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPOSITEFX_SET_K4(extCompositeFX *Self, double Value)
{
   Self->K4 = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPOSITEFX_SET_Operator(extCompositeFX *Self, OP Value)
{
   Self->Operator = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldArray clCompositeFXFields[] = {
   { "K1",       FDF_DOUBLE|FDF_RW, nullptr, COMPOSITEFX_SET_K1 },
   { "K2",       FDF_DOUBLE|FDF_RW, nullptr, COMPOSITEFX_SET_K2 },
   { "K3",       FDF_DOUBLE|FDF_RW, nullptr, COMPOSITEFX_SET_K3 },
   { "K4",       FDF_DOUBLE|FDF_RW, nullptr, COMPOSITEFX_SET_K4 },
   { "Operator", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, COMPOSITEFX_SET_Operator, &clCompositeFXOP },
   { "XMLDef",   FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_compositefx(void)
{
   clCompositeFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::COMPOSITEFX),
      fl::Name("CompositeFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clCompositeFXActions),
      fl::Fields(clCompositeFXFields),
      fl::Size(sizeof(extCompositeFX)),
      fl::Path(MOD_PATH));

   return clCompositeFX ? ERR::Okay : ERR::AddClass;
}
