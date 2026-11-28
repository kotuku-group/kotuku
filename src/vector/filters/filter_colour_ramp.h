// Colour ramps for source effects that draw a scalar value through a named ColourMap or custom Stops, as used by
// CellularFX, TurbulenceFX and WaveFunctionFX.
//
// An effect that supports a ramp declares the exported fields `std::string ColourMap` and
// `kt::vector<GradientStop> Stops`, and the private `std::unique_ptr<GradientColours> Colours`, which holds the
// sampled table of whichever of the two is active.  Setting either one replaces the other, and an empty value clears
// both, restoring the effect's default output.
//
// Ramp colours are written as authored, without conversion to the effect's colour space, and keep the opacity of
// their stops.  This header relies on definitions from vector.h and filter.cpp, so it is only included by the filter
// sources compiled within filter.cpp.

#pragma once

// Removes any ramp.

template <class T> void clear_colour_ramp(T &Effect)
{
   Effect.ColourMap.clear();
   Effect.Stops.clear();
   Effect.Colours.reset();
}

// Selects a named colourmap from glColourMaps.  An empty name clears the ramp.  Returns ERR::NotFound for an unknown
// name, leaving the current ramp unchanged.  The caller is responsible for notifying the scene of the change.

template <class T> ERR set_colour_ramp_map(T &Effect, const std::string_view &Value)
{
   if (Value.empty()) {
      clear_colour_ramp(Effect);
      return ERR::Okay;
   }

   auto it = glColourMaps.find(Value);
   if (it IS glColourMaps.end()) return ERR::NotFound;

   auto colours = std::unique_ptr<GradientColours>(new (std::nothrow) GradientColours(it->second, 1));
   if (not colours) return ERR::AllocMemory;
   Effect.Colours = std::move(colours);
   Effect.ColourMap = Value;
   Effect.Stops.clear();
   return ERR::Okay;
}

// Selects a ramp built from custom stops.  An empty array clears the ramp.  Returns ERR::InvalidValue if fewer than
// two stops are given, leaving the current ramp unchanged.  The caller is responsible for notifying the scene of the
// change.

template <class T> ERR set_colour_ramp_stops(T &Effect, const std::span<const GradientStop> &Value)
{
   if (Value.empty()) {
      clear_colour_ramp(Effect);
      return ERR::Okay;
   }

   if (Value.size() < 2) return ERR::InvalidValue;

   kt::vector<GradientStop> stops;
   stops.assign(Value.begin(), Value.end());
   auto colours = std::unique_ptr<GradientColours>(new (std::nothrow)
      GradientColours(stops, VCS::SRGB, GCI::DEFAULT, GHM::SHORTER, 1.0, 1));
   if (not colours) return ERR::AllocMemory;

   Effect.Stops = std::move(stops);
   Effect.Colours = std::move(colours);
   Effect.ColourMap.clear();
   return ERR::Okay;
}

// Writes the ramp colour for a scalar value to a 32-bit pixel.  The value is clamped to [0, 1], so 0 selects the first
// colour of the ramp and 1 the last.  R, G, B and A are the byte positions of the channels within the pixel.

inline void write_colour_ramp_pixel(const GradientColours &Colours, double Value, uint8_t *Pixel,
   uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
   const auto &rgb = Colours.table[uint8_t(std::lround(std::clamp(Value, 0.0, 1.0) * 255.0))];
   Pixel[R] = rgb.r;
   Pixel[G] = rgb.g;
   Pixel[B] = rgb.b;
   Pixel[A] = rgb.a;
}

// Describes the ramp in an XMLDef element, as a `colormap` attribute or as child `stop` elements.  Returns
// ERR::NoSupport if the stops cannot be expressed in SVG, because their offsets decrease or exceed 1.

template <class T> ERR colour_ramp_xml(const T &Effect, filter_xml_element &Element)
{
   if (not Effect.ColourMap.empty()) {
      Element.attribute("colormap", Effect.ColourMap);
      return ERR::Okay;
   }

   double last_offset = 0;
   for (const auto &stop : Effect.Stops) {
      if ((stop.Offset < last_offset) or (stop.Offset > 1.0)) return ERR::NoSupport;
      auto &child = Element.child("stop");
      child.attribute("offset", stop.Offset);
      write_filter_xml_colour(child, "stop-color", stop.RGB);
      last_offset = stop.Offset;
   }
   return ERR::Okay;
}
