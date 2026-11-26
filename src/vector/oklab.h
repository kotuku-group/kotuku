#pragma once

#include <cmath>

namespace oklab {

struct Lab {
   double l, a, b;
};

struct LinearRGB {
   double r, g, b;
};

// Convert via the LMS cube-root space.

inline Lab from_linear_rgb(double R, double G, double B)
{
   const double l = std::cbrt(0.4122214708 * R + 0.5363325363 * G + 0.0514459929 * B);
   const double m = std::cbrt(0.2119034982 * R + 0.6806995451 * G + 0.1073969566 * B);
   const double s = std::cbrt(0.0883024619 * R + 0.2817188376 * G + 0.6299787005 * B);

   return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
}

inline LinearRGB to_linear_rgb(const Lab &Colour)
{
   const double l = Colour.l + 0.3963377774 * Colour.a + 0.2158037573 * Colour.b;
   const double m = Colour.l - 0.1055613458 * Colour.a - 0.0638541728 * Colour.b;
   const double s = Colour.l - 0.0894841775 * Colour.a - 1.2914855480 * Colour.b;
   const double ll = l * l * l;
   const double mm = m * m * m;
   const double ss = s * s * s;

   return { +4.0767416621 * ll - 3.3077115913 * mm + 0.2309699292 * ss,
            -1.2684380046 * ll + 2.6097574011 * mm - 0.3413193965 * ss,
            -0.0041960863 * ll - 0.7034186147 * mm + 1.7076147010 * ss };
}

} // namespace oklab
