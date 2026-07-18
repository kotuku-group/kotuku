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
   void doMix(objBitmap *InBitmap, objBitmap *MixBitmap, uint8_t *Dest, uint8_t *In, uint8_t *Mix) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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
   static inline void blend(uint8_t *D, uint8_t *S, uint8_t *M, uint8_t A, uint8_t R, uint8_t G, uint8_t B) {
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

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR COMPOSITEFX_Draw(extCompositeFX *Self, struct acDraw *Args)
{
   kt::Log log;

   if (Self->Target->BytesPerPixel != 4) return ERR::InvalidState;

   objBitmap *inBmp;

   uint8_t *dest = Self->Target->Data + (Self->Target->Clip.Left * 4) +
      (Self->Target->Clip.Top * Self->Target->LineWidth);

   switch (Self->Operator) {
      case OP::OVER: {
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
            objBitmap *mixBmp;
            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               Self->doMix<composite_over>(inBmp, mixBmp, dest, in, mix);
            }
         }
         break;
      }

      case OP::IN: {
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
            objBitmap *mixBmp;
            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               Self->doMix<composite_in>(inBmp, mixBmp, dest, in, mix);
            }
         }
         break;
      }

      case OP::OUT: {
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
            objBitmap *mixBmp;
            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               Self->doMix<composite_out>(inBmp, mixBmp, dest, in, mix);
            }
         }
         break;
      }

      case OP::ATOP: {
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
            objBitmap *mixBmp;
            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               Self->doMix<composite_atop>(inBmp, mixBmp, dest, in, mix);
            }
         }
         break;
      }

      case OP::XOR: {
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
            objBitmap *mixBmp;
            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               Self->doMix<composite_xor>(inBmp, mixBmp, dest, in, mix);
            }
         }
         break;
      }

      case OP::ARITHMETIC: {
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
            objBitmap *mixBmp;
            int height = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
            int width  = Self->Target->Clip.Right - Self->Target->Clip.Left;
            if (inBmp->Clip.Right - inBmp->Clip.Left < width) width = inBmp->Clip.Right - inBmp->Clip.Left;
            if (inBmp->Clip.Bottom - inBmp->Clip.Top < height) height = inBmp->Clip.Bottom - inBmp->Clip.Top;

            const uint8_t A = Self->Target->ColourFormat->AlphaPos>>3;
            const uint8_t R = Self->Target->ColourFormat->RedPos>>3;
            const uint8_t G = Self->Target->ColourFormat->GreenPos>>3;
            const uint8_t B = Self->Target->ColourFormat->BluePos>>3;

            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               constexpr double alpha_scale = 1.0 / 255.0;
               constexpr double linear_scale = 1.0 / 65535.0;
               for (int y=0; y < height; y++) {
                  auto dp = dest;
                  auto sp = in;
                  auto mp = mix;
                  for (int x=0; x < width; x++) {
                     if ((mp[A]) or (sp[A])) {
                        // Scale RGB to 0 - 1.0 and premultiply the values.
                        const double sA = double(sp[A]) * alpha_scale;
                        const double sR = double(glLinearRGB.convert16(sp[R])) * linear_scale * sA;
                        const double sG = double(glLinearRGB.convert16(sp[G])) * linear_scale * sA;
                        const double sB = double(glLinearRGB.convert16(sp[B])) * linear_scale * sA;

                        const double mA = double(mp[A]) * alpha_scale;
                        const double mR = double(glLinearRGB.convert16(mp[R])) * linear_scale * mA;
                        const double mG = double(glLinearRGB.convert16(mp[G])) * linear_scale * mA;
                        const double mB = double(glLinearRGB.convert16(mp[B])) * linear_scale * mA;

                        double dA = (Self->K1 * sA * mA) + (Self->K2 * sA) + (Self->K3 * mA) + Self->K4;

                        if (dA > 0.0) {
                           if (dA > 1.0) dA = 1.0;

                           double demul = 1.0 / dA;
                           int dr = int(((Self->K1 * sR * mR) + (Self->K2 * sR) + (Self->K3 * mR) + Self->K4) *
                              demul * 65535.0);
                           int dg = int(((Self->K1 * sG * mG) + (Self->K2 * sG) + (Self->K3 * mG) + Self->K4) *
                              demul * 65535.0);
                           int db = int(((Self->K1 * sB * mB) + (Self->K2 * sB) + (Self->K3 * mB) + Self->K4) *
                              demul * 65535.0);

                           if (dr > 0xffff) dp[R] = 0xff;
                           else if (dr < 0) dp[R] = 0;
                           else dp[R] = glLinearRGB.invert16(dr);

                           if (dg > 0xffff) dp[G] = 0xff;
                           else if (dg < 0) dp[G] = 0;
                           else dp[G] = glLinearRGB.invert16(dg);

                           if (db > 0xffff) dp[B] = 0xff;
                           else if (db < 0) dp[B] = 0;
                           else dp[B] = glLinearRGB.invert16(db);

                           dp[A] = int(dA * 255.0);
                        }
                     }

                     dp += 4;
                     sp += 4;
                     mp += 4;
                  }
                  dest += Self->Target->LineWidth;
                  in   += inBmp->LineWidth;
                  mix  += mixBmp->LineWidth;
               }
            }
         }
         break;
      }

      default: { // These mix routines use pre-multiplied content.
         if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, true)) {
            objBitmap *mixBmp;
            if (!get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, true)) {
               uint8_t *in  = inBmp->Data + (inBmp->Clip.Left * 4) + (inBmp->Clip.Top * inBmp->LineWidth);
               uint8_t *mix = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
               #pragma GCC diagnostic ignored "-Wswitch"
               switch(Self->Operator) {
                  case OP::MULTIPLY:    Self->doMix<blend_multiply>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::SCREEN:      Self->doMix<blend_screen>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::DARKEN:      Self->doMix<blend_darken>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::LIGHTEN:     Self->doMix<blend_lighten>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::OVERLAY:     Self->doMix<blend_overlay>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::BURN:        Self->doMix<blend_burn>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::DODGE:       Self->doMix<blend_dodge>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::HARD_LIGHT:  Self->doMix<blend_hard_light>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::SOFT_LIGHT:  Self->doMix<blend_soft_light>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::DIFFERENCE:  Self->doMix<blend_difference>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::EXCLUSION:   Self->doMix<blend_exclusion>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::PLUS:        Self->doMix<blend_plus>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::MINUS:       Self->doMix<blend_minus>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::CONTRAST:    Self->doMix<blend_contrast>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::INVERT:      Self->doMix<blend_invert>(inBmp, mixBmp, dest, in, mix); break;
                  case OP::INVERT_RGB:  Self->doMix<blend_invert_rgb>(inBmp, mixBmp, dest, in, mix); break;
               }

                mixBmp->demultiply();
            }
            inBmp->demultiply();
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

-FIELD-
K2: Input value for the arithmetic operation.

-FIELD-
K3: Input value for the arithmetic operation.

-FIELD-
K4: Input value for the arithmetic operation.

-FIELD-
Operator: The compositing algorithm to use for rendering.
Lookup: OP

Setting the Operator will determine the algorithm that is used for compositing.  The default is `OVER`.

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
