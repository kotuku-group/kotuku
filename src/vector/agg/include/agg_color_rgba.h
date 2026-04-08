// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.

#pragma once

#include <math.h>
#include <algorithm>
#include "agg_basics.h"
#include "../../../link/linear_rgb.h"

namespace agg {
   // Supported byte orders for RGB and RGBA pixel formats

    struct order_rgb  { enum rgb_e  { R=0, G=1, B=2, rgb_tag }; };       //----order_rgb
    struct order_bgr  { enum bgr_e  { B=0, G=1, R=2, rgb_tag }; };       //----order_bgr
    struct order_rgba { enum rgba_e { R=0, G=1, B=2, A=3, rgba_tag }; }; //----order_rgba
    struct order_argb { enum argb_e { A=0, R=1, G=2, B=3, rgba_tag }; }; //----order_argb
    struct order_abgr { enum abgr_e { A=0, B=1, G=2, R=3, rgba_tag }; }; //----order_abgr
    struct order_bgra { enum bgra_e { B=0, G=1, R=2, A=3, rgba_tag }; }; //----order_bgra

    struct rgba { // RGB values from 0.0 - 1.0
         typedef double value_type;

         double r, g, b, a;
         bool linear;

         rgba() {}

         rgba(double r_, double g_, double b_, double a_=1.0, bool linear_=false) :
            r(r_), g(g_), b(b_), a(a_), linear(linear_) {}

         rgba(const rgba& c, double a_) : r(c.r), g(c.g), b(c.b), a(a_), linear(c.linear) {}

         // Consolidated RGB8 constructors - reduced code duplication
         rgba(RGB8 &RGB, uint8_t Alpha = 0) :
            r(RGB.Red / 255.0), g(RGB.Green / 255.0), b(RGB.Blue / 255.0),
            a((Alpha ? Alpha : RGB.Alpha) / 255.0), linear(false) {}

         rgba(FRGB &RGB, double Alpha) :
            r(std::clamp<float>(RGB.Red, 0, 1)),
            g(std::clamp<float>(RGB.Green, 0, 1)),
            b(std::clamp<float>(RGB.Blue, 0, 1)),
            a(std::clamp<float>(Alpha, 0, 1)),
            linear(false) {}

         void clear() {
            r = g = b = a = 0;
            linear = false;
         }

         const rgba & transparent() {
            a = 0.0;
            linear = false;
            return *this;
         }

         const rgba & opacity(double a_) {
            if (a_ < 0.0) a_ = 0.0;
            else if (a_ > 1.0) a_ = 1.0;
            a = a_;
            return *this;
         }

         double opacity() const {
            return a;
         }

         const rgba & premultiply() {
            r *= a;
            g *= a;
            b *= a;
            return *this;
         }

         const rgba & premultiply(double a_) {
            if (a <= 0.0 or a_ <= 0.0) {
                r = g = b = a = 0.0;
                return *this;
            }
            a_ /= a;
            r *= a_;
            g *= a_;
            b *= a_;
            a  = a_;
            return *this;
         }

         const rgba & demultiply() {
            if (a == 0) {
               r = g = b = 0;
               return *this;
            }
            double a_ = 1.0 / a;
            r *= a_;
            g *= a_;
            b *= a_;
            return *this;
         }

         rgba gradient(rgba c, double k) const {
            rgba ret;
            ret.r = r + (c.r - r) * k;
            ret.g = g + (c.g - g) * k;
            ret.b = b + (c.b - b) * k;
            ret.a = a + (c.a - a) * k;
            return ret;
         }

         static rgba no_color() { return rgba(0,0,0,0, false); }

   };

    inline rgba rgba_pre(double r, double g, double b, double a=1.0) {
        return rgba(r, g, b, a).premultiply();
    }

    inline rgba rgba_pre(const rgba& c) {
        return rgba(c).premultiply();
    }

    inline rgba rgba_pre(const rgba& c, double a) {
        return rgba(c, a).premultiply();
    }

    //--------------------------------------------------------------

    struct rgba8 { // RGB values from 0 - 255
        typedef uint8_t  value_type;
        typedef uint32_t calc_type;
        typedef int  long_type;

        enum base_scale_e {
            base_shift = 8,
            base_scale = 1 << base_shift,
            base_mask  = base_scale - 1
        };

        typedef rgba8 self_type;

        value_type r, g, b, a;

        rgba8() {}

        rgba8(const RGB8 &RGB) :
            r(value_type(RGB.Red)),
            g(value_type(RGB.Green)),
            b(value_type(RGB.Blue)),
            a(value_type(RGB.Alpha)) {}

        rgba8(const RGB8 &RGB, uint8_t Alpha) :
            r(value_type(RGB.Red)),
            g(value_type(RGB.Green)),
            b(value_type(RGB.Blue)),
            a(value_type(Alpha)) {}

        rgba8(const FRGB &RGB) :
            r((value_type)uround(std::clamp<float>(RGB.Red, 0, 1) * double(base_mask))),
            g((value_type)uround(std::clamp<float>(RGB.Green, 0, 1) * double(base_mask))),
            b((value_type)uround(std::clamp<float>(RGB.Blue, 0, 1) * double(base_mask))),
            a((value_type)uround(std::clamp<float>(RGB.Alpha, 0, 1) * double(base_mask))) {}

        rgba8(const FRGB &RGB, float Alpha) :
            r((value_type)uround(std::clamp<float>(RGB.Red, 0, 1) * double(base_mask))),
            g((value_type)uround(std::clamp<float>(RGB.Green, 0, 1) * double(base_mask))),
            b((value_type)uround(std::clamp<float>(RGB.Blue, 0, 1) * double(base_mask))),
            a((value_type)uround(std::clamp<float>(Alpha, 0, 1) * double(base_mask))) {}

        rgba8(unsigned r_, unsigned g_, unsigned b_, unsigned a_=base_mask) :
            r(value_type(r_)),
            g(value_type(g_)),
            b(value_type(b_)),
            a(value_type(a_)) {}

        rgba8(const rgba& c, double a_) :
            r((value_type)uround(c.r * double(base_mask))),
            g((value_type)uround(c.g * double(base_mask))),
            b((value_type)uround(c.b * double(base_mask))),
            a((value_type)uround(a_  * double(base_mask))) {}

        rgba8(const self_type& c, unsigned a_) :
            r(c.r), g(c.g), b(c.b), a(value_type(a_)) {}

        rgba8(const rgba& c) :
            r((value_type)uround(c.r * double(base_mask))),
            g((value_type)uround(c.g * double(base_mask))),
            b((value_type)uround(c.b * double(base_mask))),
            a((value_type)uround(c.a * double(base_mask))) {}

        void clear() {
            r = g = b = a = 0;
        }

        const self_type& transparent() {
            a = 0;
            return *this;
        }

        const self_type& opacity(double a_) {
            if (a_ < 0.0) a_ = 0.0;
            if (a_ > 1.0) a_ = 1.0;
            a = (value_type)uround(a_ * double(base_mask));
            return *this;
        }

        double opacity() const {
            return double(a) / double(base_mask);
        }

        inline const self_type& premultiply() {
            if (a == base_mask) return *this;
            if (a == 0) {
                r = g = b = 0;
                return *this;
            }
            r = value_type((calc_type(r) * a) >> base_shift);
            g = value_type((calc_type(g) * a) >> base_shift);
            b = value_type((calc_type(b) * a) >> base_shift);
            return *this;
        }

        inline const self_type& premultiply(unsigned a_) {
            if (a == base_mask and a_ >= base_mask) return *this;
            if (a == 0 or a_ == 0) {
                r = g = b = a = 0;
                return *this;
            }
            calc_type r_ = (calc_type(r) * a_) / a;
            calc_type g_ = (calc_type(g) * a_) / a;
            calc_type b_ = (calc_type(b) * a_) / a;
            r = value_type((r_ > a_) ? a_ : r_);
            g = value_type((g_ > a_) ? a_ : g_);
            b = value_type((b_ > a_) ? a_ : b_);
            a = value_type(a_);
            return *this;
        }


        inline const self_type& demultiply() {
            if (a == base_mask) return *this;
            if (a == 0) {
                r = g = b = 0;
                return *this;
            }
            calc_type r_ = (calc_type(r) * base_mask) / a;
            calc_type g_ = (calc_type(g) * base_mask) / a;
            calc_type b_ = (calc_type(b) * base_mask) / a;
            r = value_type((r_ > calc_type(base_mask)) ? calc_type(base_mask) : r_);
            g = value_type((g_ > calc_type(base_mask)) ? calc_type(base_mask) : g_);
            b = value_type((b_ > calc_type(base_mask)) ? calc_type(base_mask) : b_);
            return *this;
        }

        inline self_type gradient(const self_type& c, double k) const {
            self_type ret;
            calc_type ik = uround(k * int(base_scale));
            ret.r = value_type(calc_type(r) + (((calc_type(c.r) - r) * ik) >> base_shift));
            ret.g = value_type(calc_type(g) + (((calc_type(c.g) - g) * ik) >> base_shift));
            ret.b = value_type(calc_type(b) + (((calc_type(c.b) - b) * ik) >> base_shift));
            ret.a = value_type(calc_type(a) + (((calc_type(c.a) - a) * ik) >> base_shift));
            return ret;
        }

        inline self_type linear_gradient(const self_type& c, double k) const {
            self_type ret;
            calc_type ik = uround(k * int(base_scale));
            calc_type r_linear = glLinearRGB.convert(r);
            calc_type g_linear = glLinearRGB.convert(g);
            calc_type b_linear = glLinearRGB.convert(b);
            calc_type c_r_linear = glLinearRGB.convert(c.r);
            calc_type c_g_linear = glLinearRGB.convert(c.g);
            calc_type c_b_linear = glLinearRGB.convert(c.b);

            ret.r = glLinearRGB.invert(value_type(r_linear + (((c_r_linear - r_linear) * ik) >> base_shift)));
            ret.g = glLinearRGB.invert(value_type(g_linear + (((c_g_linear - g_linear) * ik) >> base_shift)));
            ret.b = glLinearRGB.invert(value_type(b_linear + (((c_b_linear - b_linear) * ik) >> base_shift)));
            ret.a = value_type(calc_type(a) + (((calc_type(c.a) - a) * ik) >> base_shift));
            return ret;
        }

        inline void add(const self_type& c, unsigned cover) {
            calc_type cr, cg, cb, ca;
            if (cover == cover_mask) {
                if (c.a == base_mask) {
                    *this = c;
                }
                else {
                    cr = r + c.r; r = (cr > calc_type(base_mask)) ? calc_type(base_mask) : cr;
                    cg = g + c.g; g = (cg > calc_type(base_mask)) ? calc_type(base_mask) : cg;
                    cb = b + c.b; b = (cb > calc_type(base_mask)) ? calc_type(base_mask) : cb;
                    ca = a + c.a; a = (ca > calc_type(base_mask)) ? calc_type(base_mask) : ca;
                }
            }
            else {
                cr = r + ((c.r * cover + cover_mask/2) >> cover_shift);
                cg = g + ((c.g * cover + cover_mask/2) >> cover_shift);
                cb = b + ((c.b * cover + cover_mask/2) >> cover_shift);
                ca = a + ((c.a * cover + cover_mask/2) >> cover_shift);
                r = (cr > calc_type(base_mask)) ? calc_type(base_mask) : cr;
                g = (cg > calc_type(base_mask)) ? calc_type(base_mask) : cg;
                b = (cb > calc_type(base_mask)) ? calc_type(base_mask) : cb;
                a = (ca > calc_type(base_mask)) ? calc_type(base_mask) : ca;
            }
        }

        template<class GammaLUT>
        inline void apply_gamma_dir(const GammaLUT& gamma) {
            r = gamma.dir(r);
            g = gamma.dir(g);
            b = gamma.dir(b);
        }

        template<class GammaLUT>
        inline void apply_gamma_inv(const GammaLUT& gamma) {
            r = gamma.inv(r);
            g = gamma.inv(g);
            b = gamma.inv(b);
        }

      static self_type no_color() { return self_type(0,0,0,0); }
   };

    //-------------------------------------------------------------

    inline rgba8 rgba8_pre(unsigned r, unsigned g, unsigned b, unsigned a = rgba8::base_mask) {
        return rgba8(r,g,b,a).premultiply();
    }

    inline rgba8 rgba8_pre(const rgba8& c) {
        return rgba8(c).premultiply();
    }

    inline rgba8 rgba8_pre(const rgba8& c, unsigned a) {
        return rgba8(c,a).premultiply();
    }

    inline rgba8 rgba8_pre(const rgba& c) {
        return rgba8(c).premultiply();
    }

    inline rgba8 rgba8_pre(const rgba& c, double a) {
        return rgba8(c,a).premultiply();
    }

    inline rgba8 rgb8_packed(unsigned v) {
        return rgba8((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }

    inline rgba8 bgr8_packed(unsigned v) {
        return rgba8(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
    }

    inline rgba8 argb8_packed(unsigned v) {
        return rgba8((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, v >> 24);
    }

    template<class GammaLUT> rgba8 rgba8_gamma_dir(rgba8 c, const GammaLUT& gamma)
    {
        return rgba8(gamma.dir(c.r), gamma.dir(c.g), gamma.dir(c.b), c.a);
    }

    template<class GammaLUT> rgba8 rgba8_gamma_inv(rgba8 c, const GammaLUT& gamma)
    {
        return rgba8(gamma.inv(c.r), gamma.inv(c.g), gamma.inv(c.b), c.a);
    }

    //--------------------------------------------------------------

    struct rgba16
    {
        typedef int16_t value_type;
        typedef int32_t calc_type;
        typedef int64_t long_type;
        enum base_scale_e
        {
            base_shift = 16,
            base_scale = 1 << base_shift,
            base_mask  = base_scale - 1
        };
        typedef rgba16 self_type;

        value_type r;
        value_type g;
        value_type b;
        value_type a;

        rgba16() {}

        rgba16(unsigned r_, unsigned g_, unsigned b_, unsigned a_=base_mask) :
            r(value_type(r_)),
            g(value_type(g_)),
            b(value_type(b_)),
            a(value_type(a_)) {}


        rgba16(const self_type& c, unsigned a_) :
            r(c.r), g(c.g), b(c.b), a(value_type(a_)) {}


        rgba16(const rgba& c) :
            r((value_type)uround(c.r * double(base_mask))),
            g((value_type)uround(c.g * double(base_mask))),
            b((value_type)uround(c.b * double(base_mask))),
            a((value_type)uround(c.a * double(base_mask))) {}


        rgba16(const rgba& c, double a_) :
            r((value_type)uround(c.r * double(base_mask))),
            g((value_type)uround(c.g * double(base_mask))),
            b((value_type)uround(c.b * double(base_mask))),
            a((value_type)uround(a_  * double(base_mask))) {}


        rgba16(const rgba8& c) :
            r(value_type((value_type(c.r) << 8) | c.r)),
            g(value_type((value_type(c.g) << 8) | c.g)),
            b(value_type((value_type(c.b) << 8) | c.b)),
            a(value_type((value_type(c.a) << 8) | c.a)) {}


        rgba16(const rgba8& c, unsigned a_) :
            r(value_type((value_type(c.r) << 8) | c.r)),
            g(value_type((value_type(c.g) << 8) | c.g)),
            b(value_type((value_type(c.b) << 8) | c.b)),
            a(value_type((             a_ << 8) | c.a)) {}


        void clear() {
            r = g = b = a = 0;
        }

        const self_type& transparent() {
            a = 0;
            return *this;
        }

        inline const self_type& opacity(double a_) {
            if (a_ < 0.0) a_ = 0.0;
            if (a_ > 1.0) a_ = 1.0;
            a = (value_type)uround(a_ * double(base_mask));
            return *this;
        }

        double opacity() const {
            return double(a) / double(base_mask);
        }

        inline const self_type& premultiply() {
            if (a == base_mask) return *this;
            if (a == 0) {
                r = g = b = 0;
                return *this;
            }
            r = value_type((calc_type(r) * a) >> base_shift);
            g = value_type((calc_type(g) * a) >> base_shift);
            b = value_type((calc_type(b) * a) >> base_shift);
            return *this;
        }

        inline const self_type& premultiply(unsigned a_) {
            if (a == base_mask and a_ >= base_mask) return *this;
            if (a == 0 or a_ == 0) {
                r = g = b = a = 0;
                return *this;
            }
            calc_type r_ = (calc_type(r) * a_) / a;
            calc_type g_ = (calc_type(g) * a_) / a;
            calc_type b_ = (calc_type(b) * a_) / a;
            r = value_type((r_ > a_) ? a_ : r_);
            g = value_type((g_ > a_) ? a_ : g_);
            b = value_type((b_ > a_) ? a_ : b_);
            a = value_type(a_);
            return *this;
        }

        inline const self_type& demultiply() {
            if (a == base_mask) return *this;
            if (a == 0) {
                r = g = b = 0;
                return *this;
            }
            calc_type r_ = (calc_type(r) * base_mask) / a;
            calc_type g_ = (calc_type(g) * base_mask) / a;
            calc_type b_ = (calc_type(b) * base_mask) / a;
            r = value_type((r_ > calc_type(base_mask)) ? calc_type(base_mask) : r_);
            g = value_type((g_ > calc_type(base_mask)) ? calc_type(base_mask) : g_);
            b = value_type((b_ > calc_type(base_mask)) ? calc_type(base_mask) : b_);
            return *this;
        }

        inline self_type gradient(const self_type& c, double k) const {
            self_type ret;
            calc_type ik = uround(k * int(base_scale));
            ret.r = value_type(calc_type(r) + (((calc_type(c.r) - r) * ik) >> base_shift));
            ret.g = value_type(calc_type(g) + (((calc_type(c.g) - g) * ik) >> base_shift));
            ret.b = value_type(calc_type(b) + (((calc_type(c.b) - b) * ik) >> base_shift));
            ret.a = value_type(calc_type(a) + (((calc_type(c.a) - a) * ik) >> base_shift));
            return ret;
        }

        inline void add(const self_type& c, unsigned cover) {
            calc_type cr, cg, cb, ca;
            if (cover == cover_mask) {
                if (c.a == base_mask) *this = c;
                else {
                    cr = r + c.r; r = (cr > calc_type(base_mask)) ? calc_type(base_mask) : cr;
                    cg = g + c.g; g = (cg > calc_type(base_mask)) ? calc_type(base_mask) : cg;
                    cb = b + c.b; b = (cb > calc_type(base_mask)) ? calc_type(base_mask) : cb;
                    ca = a + c.a; a = (ca > calc_type(base_mask)) ? calc_type(base_mask) : ca;
                }
            }
            else {
                cr = r + ((c.r * cover + cover_mask) >> cover_shift);
                cg = g + ((c.g * cover + cover_mask) >> cover_shift);
                cb = b + ((c.b * cover + cover_mask) >> cover_shift);
                ca = a + ((c.a * cover + cover_mask) >> cover_shift);
                r = (cr > calc_type(base_mask)) ? calc_type(base_mask) : cr;
                g = (cg > calc_type(base_mask)) ? calc_type(base_mask) : cg;
                b = (cb > calc_type(base_mask)) ? calc_type(base_mask) : cb;
                a = (ca > calc_type(base_mask)) ? calc_type(base_mask) : ca;
            }
        }

        template<class GammaLUT>
        inline void apply_gamma_dir(const GammaLUT& gamma) {
            r = gamma.dir(r);
            g = gamma.dir(g);
            b = gamma.dir(b);
        }

        template<class GammaLUT>
        inline void apply_gamma_inv(const GammaLUT& gamma) {
            r = gamma.inv(r);
            g = gamma.inv(g);
            b = gamma.inv(b);
        }

        static self_type no_color() { return self_type(0,0,0,0); }
    };

    inline rgba16 rgba16_pre(unsigned r, unsigned g, unsigned b, unsigned a = rgba16::base_mask) {
        return rgba16(r,g,b,a).premultiply();
    }

    inline rgba16 rgba16_pre(const rgba16& c, unsigned a) {
        return rgba16(c,a).premultiply();
    }

    inline rgba16 rgba16_pre(const rgba& c) {
        return rgba16(c).premultiply();
    }

    inline rgba16 rgba16_pre(const rgba& c, double a) {
        return rgba16(c,a).premultiply();
    }

    inline rgba16 rgba16_pre(const rgba8& c) {
        return rgba16(c).premultiply();
    }

    inline rgba16 rgba16_pre(const rgba8& c, unsigned a) {
        return rgba16(c,a).premultiply();
    }

    template<class GammaLUT>
    rgba16 rgba16_gamma_dir(rgba16 c, const GammaLUT& gamma)
    {
        return rgba16(gamma.dir(c.r), gamma.dir(c.g), gamma.dir(c.b), c.a);
    }

    template<class GammaLUT>
    rgba16 rgba16_gamma_inv(rgba16 c, const GammaLUT& gamma)
    {
        return rgba16(gamma.inv(c.r), gamma.inv(c.g), gamma.inv(c.b), c.a);
    }
}
