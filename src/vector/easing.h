// Easing curves for GEZ values, shared by gradient colour tables and filter profiles.  Value is expected in [0, 1];
// every curve maps 0 to 0 and 1 to 1 and is non-decreasing over that range.

#pragma once

#include <algorithm>
#include <cmath>
#include <kotuku/modules/vector.h>

inline double gez_ease(GEZ Easing, double Value)
{
   switch (Easing) {
      case GEZ::IN:
         return Value * Value;

      case GEZ::OUT: {
         const double inverse = 1.0 - Value;
         return 1.0 - (inverse * inverse);
      }

      case GEZ::IN_OUT:
         return Value * Value * (3.0 - (2.0 * Value));

      case GEZ::CUBIC_IN:
         return Value * Value * Value;

      case GEZ::CUBIC_OUT: {
         const double inverse = 1.0 - Value;
         return 1.0 - (inverse * inverse * inverse);
      }

      case GEZ::CUBIC_IN_OUT:
         if (Value < 0.5) return 4.0 * Value * Value * Value;
         else {
            const double inverse = -2.0 * Value + 2.0;
            return 1.0 - ((inverse * inverse * inverse) * 0.5);
         }

      case GEZ::CIRCLE_IN:
         return 1.0 - std::sqrt(std::max(0.0, 1.0 - (Value * Value)));

      case GEZ::CIRCLE_OUT:
         return std::sqrt(std::max(0.0, Value * (2.0 - Value)));

      case GEZ::CIRCLE_IN_OUT:
         if (Value < 0.5) {
            const double scaled = 2.0 * Value;
            return (1.0 - std::sqrt(std::max(0.0, 1.0 - (scaled * scaled)))) * 0.5;
         }
         else {
            const double inverse = -2.0 * Value + 2.0;
            return (std::sqrt(std::max(0.0, 1.0 - (inverse * inverse))) + 1.0) * 0.5;
         }

      case GEZ::LINEAR:
      default:
         return Value;
   }
}
