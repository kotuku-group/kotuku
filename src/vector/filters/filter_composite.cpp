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

   template <class CompositeOp>
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
            CompositeOp::blend(dp, sp, mp, A, R, G, B);
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
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (!M[A]) ((uint32_t *)D)[0] = ((uint32_t *)S)[0];
      else if (!S[A]) ((uint32_t *)D)[0] = ((uint32_t *)M)[0];
      else {
         const uint32_t dA = S[A] + M[A] - ((S[A] * M[A] + 0xff)>>8);
         const uint32_t sA = S[A] + (S[A] >> 7); // 0..255 -> 0..256
         const uint32_t cA = 256 - sA;
         const uint32_t mA = M[A] + (M[A] >> 7); // 0..255 -> 0..256

         D[R] = glLinearRGB.invert16(linear_clamp(((glLinearRGB.convert16(S[R]) * sA +
            ((glLinearRGB.convert16(M[R]) * mA * cA)>>8))>>8) * 255 / dA));
         D[G] = glLinearRGB.invert16(linear_clamp(((glLinearRGB.convert16(S[G]) * sA +
            ((glLinearRGB.convert16(M[G]) * mA * cA)>>8))>>8) * 255 / dA));
         D[B] = glLinearRGB.invert16(linear_clamp(((glLinearRGB.convert16(S[B]) * sA +
            ((glLinearRGB.convert16(M[B]) * mA * cA)>>8))>>8) * 255 / dA));
         D[A] = dA;
      }
   }
};

struct composite_in {
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
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (auto m_alpha = M[A]) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         const uint8_t sA  = S[A];
         const uint8_t scA = 0xff - sA;

         D[R] = glLinearRGB.invert16(((sR * sA) + (mR * scA) + 0xff)>>8);
         D[G] = glLinearRGB.invert16(((sG * sA) + (mG * scA) + 0xff)>>8);
         D[B] = glLinearRGB.invert16(((sB * sA) + (mB * scA) + 0xff)>>8);
         D[A] = m_alpha;
      }
   }
};

struct composite_xor {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      auto sR = glLinearRGB.convert16(S[R]);
      auto sG = glLinearRGB.convert16(S[G]);
      auto sB = glLinearRGB.convert16(S[B]);

      auto mR = glLinearRGB.convert16(M[R]);
      auto mG = glLinearRGB.convert16(M[G]);
      auto mB = glLinearRGB.convert16(M[B]);

      const uint8_t s1a = 0xff - S[A];
      const uint8_t d1a = 0xff - M[A];
      D[R] = glLinearRGB.invert16(linear_clamp(((mR * s1a) + (sR * d1a) + 0xff) >> 8));
      D[G] = glLinearRGB.invert16(linear_clamp(((mG * s1a) + (sG * d1a) + 0xff) >> 8));
      D[B] = glLinearRGB.invert16(linear_clamp(((mB * s1a) + (sB * d1a) + 0xff) >> 8));
      D[A] = (S[A] + M[A] - ((S[A] * M[A] + (0xff>>1)) >> (8 - 1)));
   }
};

//********************************************************************************************************************
// Blending algorithms, refer to https://en.wikipedia.org/wiki/Blend_modes

struct blend_screen {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      auto sR = glLinearRGB.convert16(S[R]);
      auto sG = glLinearRGB.convert16(S[G]);
      auto sB = glLinearRGB.convert16(S[B]);

      auto mR = glLinearRGB.convert16(M[R]);
      auto mG = glLinearRGB.convert16(M[G]);
      auto mB = glLinearRGB.convert16(M[B]);

      D[R] = glLinearRGB.invert16(linear_clamp(sR + mR - linear_multiply(sR, mR)));
      D[G] = glLinearRGB.invert16(linear_clamp(sG + mG - linear_multiply(sG, mG)));
      D[B] = glLinearRGB.invert16(linear_clamp(sB + mB - linear_multiply(sB, mB)));
      D[A] = uint8_t(S[A] + M[A] - ((S[A] * M[A] + 0Xff) >> 8));
   }
};

struct blend_multiply {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         const uint8_t s1a = 0xff - S[A];
         const uint8_t d1a = 0xff - M[A];
         D[R] = glLinearRGB.invert16(linear_clamp(linear_multiply(sR, mR) +
            linear_scale_alpha(sR, d1a) + linear_scale_alpha(mR, s1a)));
         D[G] = glLinearRGB.invert16(linear_clamp(linear_multiply(sG, mG) +
            linear_scale_alpha(sG, d1a) + linear_scale_alpha(mG, s1a)));
         D[B] = glLinearRGB.invert16(linear_clamp(linear_multiply(sB, mB) +
            linear_scale_alpha(sB, d1a) + linear_scale_alpha(mB, s1a)));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_darken {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         uint8_t d1a = 0xff - M[A];
         uint8_t s1a = 0xff - S[A];
         uint8_t da  = M[A];

         D[R] = glLinearRGB.invert16((agg::sd_min(sR * da, mR * S[A]) + sR * d1a + mR * s1a + 0xff) >> 8);
         D[G] = glLinearRGB.invert16((agg::sd_min(sG * da, mG * S[A]) + sG * d1a + mG * s1a + 0xff) >> 8);
         D[B] = glLinearRGB.invert16((agg::sd_min(sB * da, mB * S[A]) + sB * d1a + mB * s1a + 0xff) >> 8);
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_lighten {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         uint8_t d1a = 0xff - M[A];
         uint8_t s1a = 0xff - S[A];

         D[R] = glLinearRGB.invert16((agg::sd_max(sR * M[A], mR * S[A]) + sR * d1a + mR * s1a + 0xff) >> 8);
         D[G] = glLinearRGB.invert16((agg::sd_max(sG * M[A], mG * S[A]) + sG * d1a + mG * s1a + 0xff) >> 8);
         D[B] = glLinearRGB.invert16((agg::sd_max(sB * M[A], mB * S[A]) + sB * d1a + mB * s1a + 0xff) >> 8);
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_dodge {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         uint32_t d1a  = 0xff - M[A];
         uint32_t s1a  = 0xff - S[A];
         uint32_t drsa = mR * S[A];
         uint32_t dgsa = mG * S[A];
         uint32_t dbsa = mB * S[A];
         uint32_t srda = sR * M[A];
         uint32_t sgda = sG * M[A];
         uint32_t sbda = sB * M[A];
         uint32_t sada = S[A] * M[A] * 257u;

         D[R] = glLinearRGB.invert16(linear_clamp((srda + drsa >= sada) ?
             (sada + sR * d1a + mR * s1a + 0xff) >> 8 :
             uint64_t(drsa) * 257u / (0xffffu - (sR << 8) / S[A]) +
                ((sR * d1a + mR * s1a + 0xff) >> 8)));

         D[G] = glLinearRGB.invert16(linear_clamp((sgda + dgsa >= sada) ?
             (sada + sG * d1a + mG * s1a + 0xff) >> 8 :
             uint64_t(dgsa) * 257u / (0xffffu - (sG << 8) / S[A]) +
                ((sG * d1a + mG * s1a + 0xff) >> 8)));

         D[B] = glLinearRGB.invert16(linear_clamp((sbda + dbsa >= sada) ?
             (sada + sB * d1a + mB * s1a + 0xff) >> 8 :
             uint64_t(dbsa) * 257u / (0xffffu - (sB << 8) / S[A]) +
                ((sB * d1a + mB * s1a + 0xff) >> 8)));

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_contrast {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      auto sR = glLinearRGB.convert16(S[R]);
      auto sG = glLinearRGB.convert16(S[G]);
      auto sB = glLinearRGB.convert16(S[B]);

      auto mR = glLinearRGB.convert16(M[R]);
      auto mG = glLinearRGB.convert16(M[G]);
      auto mB = glLinearRGB.convert16(M[B]);

      int32_t d2a = int32_t(linear_alpha(M[A]) >> 1);
      int32_t s2a = int32_t(linear_alpha(S[A]) >> 1);

      auto r = int((int64_t(int32_t(mR) - d2a) * (int32_t(sR) - s2a) * 2 >> 16) + d2a);
      auto g = int((int64_t(int32_t(mG) - d2a) * (int32_t(sG) - s2a) * 2 >> 16) + d2a);
      auto b = int((int64_t(int32_t(mB) - d2a) * (int32_t(sB) - s2a) * 2 >> 16) + d2a);

      r = (r < 0) ? 0 : r;
      g = (g < 0) ? 0 : g;
      b = (b < 0) ? 0 : b;

      D[R] = glLinearRGB.invert16((r > int(linear_alpha(M[A]))) ? linear_alpha(M[A]) : r);
      D[G] = glLinearRGB.invert16((g > int(linear_alpha(M[A]))) ? linear_alpha(M[A]) : g);
      D[B] = glLinearRGB.invert16((b > int(linear_alpha(M[A]))) ? linear_alpha(M[A]) : b);
   }
};

struct blend_overlay {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         uint8_t d1a = 0xff - M[A];
         uint8_t s1a = 0xff - S[A];
         uint32_t sada = S[A] * M[A] * 257u;

         D[R] = glLinearRGB.invert16(((2*mR < linear_alpha(M[A])) ?
             uint64_t(2) * sR*mR / 257u + sR*d1a + mR*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mR)*(linear_alpha(S[A]) - sR) / 257u +
                sR*d1a + mR*s1a + 0xff) >> 8);

         D[G] = glLinearRGB.invert16(((2*mG < linear_alpha(M[A])) ?
             uint64_t(2) * sG*mG / 257u + sG*d1a + mG*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mG)*(linear_alpha(S[A]) - sG) / 257u +
                sG*d1a + mG*s1a + 0xff) >> 8);

         D[B] = glLinearRGB.invert16(((2*mB < linear_alpha(M[A])) ?
             uint64_t(2) * sB*mB / 257u + sB*d1a + mB*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mB)*(linear_alpha(S[A]) - sB) / 257u +
                sB*d1a + mB*s1a + 0xff) >> 8);

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_burn {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         const uint8_t d1a = 0xff - M[A];
         const uint8_t s1a = 0xff - S[A];
         const uint32_t drsa = mR * S[A];
         const uint32_t dgsa = mG * S[A];
         const uint32_t dbsa = mB * S[A];
         const uint32_t srda = sR * M[A];
         const uint32_t sgda = sG * M[A];
         const uint32_t sbda = sB * M[A];
         const uint32_t sada = S[A] * M[A] * 257u;

         D[R] = glLinearRGB.invert16(((srda + drsa <= sada) ?
             sR * d1a + mR * s1a :
             uint64_t(S[A]) * (srda + drsa - sada) / sR + sR * d1a + mR * s1a + 0xff) >> 8);

         D[G] = glLinearRGB.invert16(((sgda + dgsa <= sada) ?
             sG * d1a + mG * s1a :
             uint64_t(S[A]) * (sgda + dgsa - sada) / sG + sG * d1a + mG * s1a + 0xff) >> 8);

         D[B] = glLinearRGB.invert16(((sbda + dbsa <= sada) ?
             sB * d1a + mB * s1a :
             uint64_t(S[A]) * (sbda + dbsa - sada) / sB + sB * d1a + mB * s1a + 0xff) >> 8);

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_hard_light {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         uint8_t d1a  = 0xff - M[A];
         uint8_t s1a  = 0xff - S[A];
         uint32_t sada = S[A] * M[A] * 257u;

         D[R] = glLinearRGB.invert16(((2*sR < linear_alpha(S[A])) ?
             uint64_t(2) * sR*mR / 257u + sR*d1a + mR*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mR)*(linear_alpha(S[A]) - sR) / 257u +
                sR*d1a + mR*s1a + 0xff) >> 8);

         D[G] = glLinearRGB.invert16(((2*sG < linear_alpha(S[A])) ?
             uint64_t(2) * sG*mG / 257u + sG*d1a + mG*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mG)*(linear_alpha(S[A]) - sG) / 257u +
                sG*d1a + mG*s1a + 0xff) >> 8);

         D[B] = glLinearRGB.invert16(((2*sB < linear_alpha(S[A])) ?
             uint64_t(2) * sB*mB / 257u + sB*d1a + mB*s1a :
             sada - uint64_t(2)*(linear_alpha(M[A]) - mB)*(linear_alpha(S[A]) - sB) / 257u +
                sB*d1a + mB*s1a + 0xff) >> 8);

         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_soft_light {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         double sR = double(glLinearRGB.convert16(S[R])) / 65535.0;
         double sG = double(glLinearRGB.convert16(S[G])) / 65535.0;
         double sB = double(glLinearRGB.convert16(S[B])) / 65535.0;

         double xr = double(glLinearRGB.convert16(M[R])) / 65535.0;
         double xg = double(glLinearRGB.convert16(M[G])) / 65535.0;
         double xb = double(glLinearRGB.convert16(M[B])) / 65535.0;
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

         D[R] = glLinearRGB.invert16(agg::uround(std::clamp(xr, 0.0, 1.0) * 65535.0));
         D[G] = glLinearRGB.invert16(agg::uround(std::clamp(xg, 0.0, 1.0) * 65535.0));
         D[B] = glLinearRGB.invert16(agg::uround(std::clamp(xb, 0.0, 1.0) * 65535.0));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_difference {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         D[R] = glLinearRGB.invert16(sR + mR - ((2 * agg::sd_min(sR*M[A], mR*S[A]) + 0xff) >> 8));
         D[G] = glLinearRGB.invert16(sG + mG - ((2 * agg::sd_min(sG*M[A], mG*S[A]) + 0xff) >> 8));
         D[B] = glLinearRGB.invert16(sB + mB - ((2 * agg::sd_min(sB*M[A], mB*S[A]) + 0xff) >> 8));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_exclusion {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto mR = glLinearRGB.convert16(M[R]);
         auto mG = glLinearRGB.convert16(M[G]);
         auto mB = glLinearRGB.convert16(M[B]);

         const uint8_t d1a = 0xff - M[A];
         const uint8_t s1a = 0xff - S[A];
         D[R] = glLinearRGB.invert16(linear_clamp(int64_t(linear_scale_alpha(sR, M[A])) +
            linear_scale_alpha(mR, S[A]) - 2 * linear_multiply(sR, mR) +
            linear_scale_alpha(sR, d1a) + linear_scale_alpha(mR, s1a)));
         D[G] = glLinearRGB.invert16(linear_clamp(int64_t(linear_scale_alpha(sG, M[A])) +
            linear_scale_alpha(mG, S[A]) - 2 * linear_multiply(sG, mG) +
            linear_scale_alpha(sG, d1a) + linear_scale_alpha(mG, s1a)));
         D[B] = glLinearRGB.invert16(linear_clamp(int64_t(linear_scale_alpha(sB, M[A])) +
            linear_scale_alpha(mB, S[A]) - 2 * linear_multiply(sB, mB) +
            linear_scale_alpha(sB, d1a) + linear_scale_alpha(mB, s1a)));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_plus {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         const int xr = glLinearRGB.convert16(M[R]) + sR;
         const int xg = glLinearRGB.convert16(M[G]) + sG;
         const int xb = glLinearRGB.convert16(M[B]) + sB;
         const int xa = M[A] + S[A];
         D[R] = glLinearRGB.invert16((xr > 0xffff) ? 0xffff : xr);
         D[G] = glLinearRGB.invert16((xg > 0xffff) ? 0xffff : xg);
         D[B] = glLinearRGB.invert16((xb > 0xffff) ? 0xffff : xb);
         D[A] = (xa > 0xff) ? 0xff : xa;
      }
   }
};

struct blend_minus {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         const int xr = glLinearRGB.convert16(M[R]) - sR;
         const int xg = glLinearRGB.convert16(M[G]) - sG;
         const int xb = glLinearRGB.convert16(M[B]) - sB;
         D[R] = glLinearRGB.invert16((xr < 0) ? 0 : xr);
         D[G] = glLinearRGB.invert16((xg < 0) ? 0 : xg);
         D[B] = glLinearRGB.invert16((xb < 0) ? 0 : xb);
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
         //D[A] = (UBYTE)(0xff - (((0xff - S[A]) * (0xff - D[A]) + 0xff) >> 8));
      }
   }
};

struct blend_invert {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if ((S[A]) or (M[A])) {
         auto dR = glLinearRGB.convert16(D[R]);
         auto dG = glLinearRGB.convert16(D[G]);
         auto dB = glLinearRGB.convert16(D[B]);

         const uint16_t xr = linear_scale_alpha(linear_alpha(M[A]) - dR, S[A]);
         const uint16_t xg = linear_scale_alpha(linear_alpha(M[A]) - dG, S[A]);
         const uint16_t xb = linear_scale_alpha(linear_alpha(M[A]) - dB, S[A]);
         const uint8_t s1a = 0xff - S[A];
         D[R] = glLinearRGB.invert16(xr + ((dR * s1a + 0xff) >> 8));
         D[G] = glLinearRGB.invert16(xg + ((dG * s1a + 0xff) >> 8));
         D[B] = glLinearRGB.invert16(xb + ((dB * s1a + 0xff) >> 8));
         D[A] = (uint8_t)(S[A] + M[A] - ((S[A] * M[A] + 0xff) >> 8));
      }
   }
};

struct blend_invert_rgb {
   static inline void blend(uint8_t *D, const uint8_t *S, const uint8_t *M,
      uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
      if (S[A]) {
         auto sR = glLinearRGB.convert16(S[R]);
         auto sG = glLinearRGB.convert16(S[G]);
         auto sB = glLinearRGB.convert16(S[B]);

         auto dR = glLinearRGB.convert16(D[R]);
         auto dG = glLinearRGB.convert16(D[G]);
         auto dB = glLinearRGB.convert16(D[B]);

         uint16_t xr = linear_multiply(linear_alpha(M[A]) - dR, sR);
         uint16_t xg = linear_multiply(linear_alpha(M[A]) - dG, sG);
         uint16_t xb = linear_multiply(linear_alpha(M[A]) - dB, sB);
         uint8_t s1a = 0xff - S[A];
         D[R] = glLinearRGB.invert16(xr + ((dR * s1a + 0xff) >> 8));
         D[G] = glLinearRGB.invert16(xg + ((dG * s1a + 0xff) >> 8));
         D[B] = glLinearRGB.invert16(xb + ((dB * s1a + 0xff) >> 8));
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

   switch (Self->Operator) {
      case OP::OVER: Self->doMix<composite_over>(in_bmp, mix_bmp, dest, in, mix); break;
      case OP::IN:   Self->doMix<composite_in>(in_bmp, mix_bmp, dest, in, mix); break;
      case OP::OUT:  Self->doMix<composite_out>(in_bmp, mix_bmp, dest, in, mix); break;
      case OP::ATOP: Self->doMix<composite_atop>(in_bmp, mix_bmp, dest, in, mix); break;
      case OP::XOR:  Self->doMix<composite_xor>(in_bmp, mix_bmp, dest, in, mix); break;

      case OP::ARITHMETIC: {
         int height = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
         int width  = Self->Target->Clip.Right - Self->Target->Clip.Left;
         if (in_bmp->Clip.Right - in_bmp->Clip.Left < width) width = in_bmp->Clip.Right - in_bmp->Clip.Left;
         if (in_bmp->Clip.Bottom - in_bmp->Clip.Top < height) height = in_bmp->Clip.Bottom - in_bmp->Clip.Top;

         const auto arithmetic_rows = (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) ?
            composite_arithmetic_rows<true> : composite_arithmetic_rows<false>;

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
            case OP::MULTIPLY:    Self->doMix<blend_multiply>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::SCREEN:      Self->doMix<blend_screen>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::DARKEN:      Self->doMix<blend_darken>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::LIGHTEN:     Self->doMix<blend_lighten>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::OVERLAY:     Self->doMix<blend_overlay>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::BURN:        Self->doMix<blend_burn>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::DODGE:       Self->doMix<blend_dodge>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::HARD_LIGHT:  Self->doMix<blend_hard_light>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::SOFT_LIGHT:  Self->doMix<blend_soft_light>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::DIFFERENCE:  Self->doMix<blend_difference>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::EXCLUSION:   Self->doMix<blend_exclusion>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::PLUS:        Self->doMix<blend_plus>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::MINUS:       Self->doMix<blend_minus>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::CONTRAST:    Self->doMix<blend_contrast>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::INVERT:      Self->doMix<blend_invert>(in_bmp, mix_bmp, dest, in, mix); break;
            case OP::INVERT_RGB:  Self->doMix<blend_invert_rgb>(in_bmp, mix_bmp, dest, in, mix); break;
            default: return ERR::NoSupport;
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

The `ARITHMETIC` operator is computed in the colour space of the parent @VectorFilter, so the #K1 - #K4 values
operate on sRGB values by default, or on linear RGB values when `LINEAR_RGB` is selected.  The other operators that
mix colour components always do so in linear RGB, while `IN` and `OUT` only modify the alpha channel.

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR COMPOSITEFX_GET_XMLDef(extCompositeFX *Self, std::string &Value)
{
   Value = std::string("feComposite");
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_composite_def.c"

static const FieldArray clCompositeFXFields[] = {
   { "K1",       FDF_DOUBLE|FDF_RW },
   { "K2",       FDF_DOUBLE|FDF_RW },
   { "K3",       FDF_DOUBLE|FDF_RW },
   { "K4",       FDF_DOUBLE|FDF_RW },
   { "Operator", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clCompositeFXOP },
   { "XMLDef",   FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, COMPOSITEFX_GET_XMLDef },
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
