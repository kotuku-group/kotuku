// Kotuku extension to Anti-Grain Geometry.
// ---
// Linear-RGB variant of span_gouraud_rgba.  The base generator interpolates colours linearly in whatever byte
// encoding it is handed and emits those bytes verbatim.  For a perceptually-correct linearRGB gradient the
// interpolation must happen in linear space and the result re-encoded to sRGB for storage (the pixel format treats
// stored bytes as sRGB).  This is the same convert -> interpolate -> invert sequence that rgba8::linear_gradient()
// performs per gradient-table entry, applied here per output pixel.
//
// Usage: construct exactly like span_gouraud_rgba, but supply vertex colours already decoded to linear space (via
// glLinearRGB.convert).  generate() then interpolates in linear space and inverts each output pixel back to sRGB.

#pragma once

#include "agg_span_gouraud_rgba.h"
#include "../../../link/linear_rgb.h"

namespace agg {

template<class ColorT> class span_gouraud_rgba_linear : public span_gouraud_rgba<ColorT> {
public:
    typedef ColorT color_type;
    typedef span_gouraud_rgba<ColorT> base_type;

    span_gouraud_rgba_linear() {}
    span_gouraud_rgba_linear(const color_type& c1, const color_type& c2, const color_type& c3,
                             double x1, double y1, double x2, double y2, double x3, double y3, double d = 0) :
        base_type(c1, c2, c3, x1, y1, x2, y2, x3, y3, d)
    {}

    // Interpolate in linear space (the base generator's job, since it was handed linear-encoded vertex colours),
    // then re-encode each pixel's RGB channels back to sRGB.  Alpha is linear-light independent and left untouched.

    void generate(color_type* span, int x, int y, unsigned len)
    {
        base_type::generate(span, x, y, len);
        for (unsigned i=0; i < len; i++) {
            span[i].r = glLinearRGB.invert(uint8_t(span[i].r));
            span[i].g = glLinearRGB.invert(uint8_t(span[i].g));
            span[i].b = glLinearRGB.invert(uint8_t(span[i].b));
        }
    }
};

} // namespace
