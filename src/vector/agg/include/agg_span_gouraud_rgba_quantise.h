// Kotuku extension to Anti-Grain Geometry.
// ---
// Resolution (posterisation) wrapper for the Gouraud span generators.  The field-based gradients implement the
// Gradient Resolution field by collapsing their 256-entry colour table into blocks, leaving 1/(1-Resolution)
// distinct colours across the ramp (see GradientColours::apply_resolution).  A Gouraud gradient has no colour table:
// its colour is interpolated per pixel across each triangle.  The equivalent reduction is to quantise each output
// pixel's channels to a small number of levels, which produces the same banded, reduced-resolution appearance.
//
// This is a thin wrapper template parameterised on the underlying Gouraud span generator (plain sRGB or linear), so
// quantisation composes with either colour space.  The wrapped generator runs first (performing interpolation and,
// for the linear variant, the linear->sRGB re-encode); the quantiser then snaps the resulting sRGB channels.

#pragma once

#include "agg_span_gouraud_rgba.h"

namespace agg {

template<class BaseSpan> class span_gouraud_rgba_quantise : public BaseSpan {
public:
    typedef typename BaseSpan::color_type color_type;

    span_gouraud_rgba_quantise() : m_levels(256) {}

    // Levels is the number of discrete steps each channel is snapped to (>= 2 to band, 256 == no visible change).

    span_gouraud_rgba_quantise(const color_type& c1, const color_type& c2, const color_type& c3,
                               double x1, double y1, double x2, double y2, double x3, double y3,
                               double d, int levels) :
        BaseSpan(c1, c2, c3, x1, y1, x2, y2, x3, y3, d), m_levels(levels < 2 ? 2 : (levels > 256 ? 256 : levels))
    {}

    void generate(color_type* span, int x, int y, unsigned len)
    {
        BaseSpan::generate(span, x, y, len);

        // Snap each channel to one of m_levels evenly-spaced steps across the 0-255 range.  Rounding to the nearest
        // step keeps the banding centred on the original values.  Alpha is left untouched so edge anti-aliasing and
        // per-vertex transparency are preserved.
        const int steps = m_levels - 1;
        for (unsigned i=0; i < len; i++) {
            span[i].r = uint8_t((int(span[i].r) * steps + 127) / 255 * 255 / steps);
            span[i].g = uint8_t((int(span[i].g) * steps + 127) / 255 * 255 / steps);
            span[i].b = uint8_t((int(span[i].b) * steps + 127) / 255 * 255 / steps);
        }
    }

private:
    int m_levels;
};

} // namespace
