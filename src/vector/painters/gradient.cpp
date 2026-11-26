/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of gradient definitions only.

-CLASS-
Gradient: Base class for colour gradient paint servers.

Gradient stores the common state shared by all gradient paint servers.  Concrete gradient classes such as
@GradientLinear, @GradientRadial and @GradientGouraud provide the fields that describe a specific gradient shape.

Gradient objects are definition objects and should normally be registered with a @VectorScene via
@VectorScene.AddDef() before being referenced by vector fill or stroke attributes.

-END-

*********************************************************************************************************************/

#include "../oklab.h"
#include <numeric>

static ERR GRADIENT_SET_Stops(extGradient *, std::span<const GradientStop> *);

static ERR rebuild_gradient_colours(extGradient *);
static void invalidate_gradient_tables(extGradient *);

static ERR init_gradient_linear(void);
static ERR init_gradient_radial(void);
static ERR init_gradient_conic(void);
static ERR init_gradient_diamond(void);
static ERR init_gradient_contour(void);
static ERR init_gradient_gouraud(void);
static ERR init_gradient_mesh(void);
static ERR init_gradient_diffusion(void);
static ERR init_gradient_distal(void);
static ERR init_gradient_voronoi(void);

// Return a gradient table for a vector with its opacity multiplier applied.  The table is cached with the vector so
// that it does not need to be recalculated when required again.

GRADIENT_TABLE * get_fill_gradient_table(extPainter &Painter, double Opacity)
{
   kt::Log log(__FUNCTION__);

   GradientColours *cols = ((extGradient *)Painter.Gradient)->Colours;
   if (not cols) {
      log.warning("No colour table in gradient %p.", Painter.Gradient);
      return nullptr;
   }

   if (Opacity >= 1.0) { // Return the original gradient table if no translucency is applicable.
      Painter.GradientAlpha = 1.0;
      return &cols->table;
   }
   else {
      if ((Painter.GradientTable) and (Opacity IS Painter.GradientAlpha)) return Painter.GradientTable.get();

      Painter.GradientTable.reset(new (std::nothrow) GRADIENT_TABLE());
      if (not Painter.GradientTable) {
         log.warning("Failed to allocate fill gradient table");
         return nullptr;
      }
      Painter.GradientAlpha = Opacity;

      for (unsigned i=0; i < Painter.GradientTable->size(); i++) {
         (*Painter.GradientTable)[i] = agg::rgba8(cols->table[i].r, cols->table[i].g, cols->table[i].b,
            cols->table[i].a * Opacity);
      }

      return Painter.GradientTable.get();
   }
}

//********************************************************************************************************************

GRADIENT_TABLE * get_stroke_gradient_table(extVector &Vector)
{
   kt::Log log(__FUNCTION__);

   GradientColours *cols = ((extGradient *)Vector.Stroke.Gradient)->Colours;
   if (not cols) {
      log.warning("No colour table referenced in stroke gradient %p for vector #%d.", Vector.Stroke.Gradient, Vector.UID);
      return nullptr;
   }

   if ((Vector.StrokeOpacity IS 1.0) and (Vector.Opacity IS 1.0)) {
      Vector.Stroke.GradientAlpha = 1.0;
      return &cols->table;
   }
   else {
      double opacity = Vector.StrokeOpacity * Vector.Opacity;
      if ((Vector.Stroke.GradientTable) and (opacity IS Vector.Stroke.GradientAlpha)) {
         return Vector.Stroke.GradientTable.get();
      }

      Vector.Stroke.GradientTable.reset(new (std::nothrow) GRADIENT_TABLE());
      if (not Vector.Stroke.GradientTable) {
         log.warning("Failed to allocate stroke gradient table");
         return nullptr;
      }
      Vector.Stroke.GradientAlpha = opacity;

      for (unsigned i=0; i < Vector.Stroke.GradientTable->size(); i++) {
         (*Vector.Stroke.GradientTable)[i] = agg::rgba8(cols->table[i].r, cols->table[i].g, cols->table[i].b,
            cols->table[i].a * opacity);
      }

      return Vector.Stroke.GradientTable.get();
   }
}

//********************************************************************************************************************
// Clear cached opacity-adjusted gradient tables on vectors that reference a modified shared gradient.

static void invalidate_gradient_painter_table(extPainter &Painter, extGradient *Target)
{
   if (Painter.Gradient IS Target) {
      Painter.GradientTable.reset();
   }
}

static void invalidate_gradient_tables(extVector *Vector, extGradient *Target)
{
   for (auto node=Vector; node; node=(extVector *)node->Next) {
      invalidate_gradient_painter_table(node->Fill, Target);
      if (node->ExtFill) {
         for (auto &fill : *node->ExtFill) invalidate_gradient_painter_table(fill, Target);
      }
      invalidate_gradient_painter_table(node->Stroke, Target);

      if (node->Child) invalidate_gradient_tables((extVector *)node->Child, Target);
   }
}

static void invalidate_gradient_tables(extGradient *Self)
{
   if ((Self->HostScene) and (Self->HostScene->Viewport)) {
      invalidate_gradient_tables((extVector *)Self->HostScene->Viewport, Self);
   }
}

//********************************************************************************************************************
// IEC 61966-2-1 transfer functions, mirrored for negative components in extended sRGB.

static agg::rgba gradient_stop_colour(const FRGB &Colour, bool Linear)
{
   if (not Linear) return { Colour.Red, Colour.Green, Colour.Blue, Colour.Alpha };

   auto decode = [](double Value) {
      const double magnitude = std::abs(Value);
      if (magnitude <= 0.04045) return Value / 12.92;
      return std::copysign(std::pow((magnitude + 0.055) / 1.055, 2.4), Value);
   };

   return { decode(Colour.Red), decode(Colour.Green), decode(Colour.Blue), Colour.Alpha };
}

static agg::rgba interpolate_ramp_colour(const agg::rgba &Start, const agg::rgba &End, double Blend)
{
   return { std::lerp(Start.r, End.r, Blend), std::lerp(Start.g, End.g, Blend),
      std::lerp(Start.b, End.b, Blend), std::lerp(Start.a, End.a, Blend) };
}

// CSS Color 4, Binary Search Gamut Mapping with Local MINDE.
// https://www.w3.org/TR/css-color-4/#binsearch

static agg::rgba map_oklab_to_linear_rgb(const oklab::Lab &Colour, double Alpha)
{
   constexpr double jnd = 0.02;
   constexpr double epsilon = 0.0001;

   if (Colour.l >= 1.0) return { 1.0, 1.0, 1.0, Alpha };
   if (Colour.l <= 0.0) return { 0.0, 0.0, 0.0, Alpha };

   auto in_gamut = [](const oklab::LinearRGB &RGB) {
      constexpr double tolerance = 1e-7;
      return (RGB.r >= -tolerance) and (RGB.r <= 1.0 + tolerance) and
             (RGB.g >= -tolerance) and (RGB.g <= 1.0 + tolerance) and
             (RGB.b >= -tolerance) and (RGB.b <= 1.0 + tolerance);
   };
   auto clip = [Alpha](const oklab::LinearRGB &RGB) -> agg::rgba {
      return { std::clamp(RGB.r, 0.0, 1.0), std::clamp(RGB.g, 0.0, 1.0),
         std::clamp(RGB.b, 0.0, 1.0), Alpha };
   };
   auto delta = [](const oklab::Lab &Current, const agg::rgba &Clipped) {
      const auto lab = oklab::from_linear_rgb(Clipped.r, Clipped.g, Clipped.b);
      return std::hypot(Current.l - lab.l, Current.a - lab.a, Current.b - lab.b);
   };

   const auto rgb = oklab::to_linear_rgb(Colour);
   if (in_gamut(rgb)) return { rgb.r, rgb.g, rgb.b, Alpha };

   auto clipped = clip(rgb);
   if (delta(Colour, clipped) < jnd) return clipped;

   const double chroma = std::hypot(Colour.a, Colour.b);
   if (not std::isfinite(chroma)) return clipped;
   double min_chroma = 0.0;
   double max_chroma = chroma;
   bool min_in_gamut = true;
   while (max_chroma - min_chroma > epsilon) {
      const double mid = std::midpoint(min_chroma, max_chroma);
      const double scale = mid / chroma;
      const oklab::Lab current { Colour.l, Colour.a * scale, Colour.b * scale };
      const auto candidate = oklab::to_linear_rgb(current);
      if (min_in_gamut and in_gamut(candidate)) {
         min_chroma = mid;
         continue;
      }

      clipped = clip(candidate);
      const double difference = delta(current, clipped);
      if (difference < jnd) {
         if (jnd - difference < epsilon) return clipped;
         min_in_gamut = false;
         min_chroma = mid;
      }
      else max_chroma = mid;
   }
   return clipped;
}

// Prepare an OKLCH segment once, borrowing powerless hues before applying the CSS hue-arc rules.

struct GradientOKSegment {
   oklab::Lab Start, End;
   double StartAlpha, EndAlpha;
   double StartChroma = 0.0, EndChroma = 0.0;
   double StartHue = 0.0, EndHue = 0.0;
   bool Polar;

   GradientOKSegment(const oklab::Lab &First, const oklab::Lab &Last, double FirstAlpha, double LastAlpha,
      GCI Interpolation, GHM HueMethod) :
      Start(First), End(Last), StartAlpha(FirstAlpha), EndAlpha(LastAlpha), Polar(Interpolation IS GCI::OKLCH)
   {
      if (not Polar) return;

      constexpr double half_turn = std::numbers::pi;
      constexpr double turn = half_turn * 2.0;
      constexpr double epsilon = 0.000004; // CSS Color 4 powerless hue threshold for OKLCH.
      auto hue = [turn](const oklab::Lab &Colour) {
         const double angle = std::atan2(Colour.b, Colour.a);
         return (angle < 0.0) ? angle + turn : angle;
      };
      StartChroma = std::hypot(First.a, First.b);
      EndChroma = std::hypot(Last.a, Last.b);
      if ((StartChroma <= epsilon) and (EndChroma <= epsilon)) {
         StartChroma = EndChroma = 0.0;
         return;
      }

      StartHue = hue(First);
      EndHue = hue(Last);
      if ((StartChroma <= epsilon) or (StartAlpha IS 0.0)) StartHue = EndHue;
      else if ((EndChroma <= epsilon) or (EndAlpha IS 0.0)) EndHue = StartHue;
      else {
         const double arc = EndHue - StartHue;
         switch (HueMethod) {
            case GHM::SHORTER:
               if (arc > half_turn) StartHue += turn;
               else if (arc < -half_turn) EndHue += turn;
               break;
            case GHM::LONGER:
               if ((arc > 0.0) and (arc < half_turn)) StartHue += turn;
               else if ((arc <= 0.0) and (arc > -half_turn)) EndHue += turn;
               break;
            case GHM::INCREASING:
               if (arc < 0.0) EndHue += turn;
               break;
            case GHM::DECREASING:
               if (arc > 0.0) StartHue += turn;
               break;
         }
      }
   }

   agg::rgba sample(double Blend) const
   {
      const double alpha = std::lerp(StartAlpha, EndAlpha, Blend);
      // The alpha-weighted fraction interpolates premultiplied components and divides by the resulting alpha.
      const double weight = (alpha IS 0.0) ? Blend : Blend * EndAlpha / alpha;
      const double lightness = std::lerp(Start.l, End.l, weight);
      if (Polar) {
         const double hue = std::lerp(StartHue, EndHue, Blend);
         const double chroma = std::lerp(StartChroma, EndChroma, weight);
         return map_oklab_to_linear_rgb({ lightness, chroma * std::cos(hue), chroma * std::sin(hue) }, alpha);
      }
      return map_oklab_to_linear_rgb(
         { lightness, std::lerp(Start.a, End.a, weight), std::lerp(Start.b, End.b, weight) }, alpha);
   }
};

static void store_colour_ramp(GRADIENT_TABLE &Table, std::array<agg::rgba, 256> &Ramp, bool Linear, double Alpha,
   double Resolution)
{
   auto encode = [](double Value) {
      if (Value <= 0.0031308) return 12.92 * Value;
      return 1.055 * std::pow(Value, 1.0 / 2.4) - 0.055;
   };

   auto unit = [](double Value) { return (Value > 0.0) ? std::min(Value, 1.0) : 0.0; };

   for (auto &colour : Ramp) {
      colour.r = unit(Linear ? encode(colour.r) : colour.r);
      colour.g = unit(Linear ? encode(colour.g) : colour.g);
      colour.b = unit(Linear ? encode(colour.b) : colour.b);
      colour.a = unit(colour.a * Alpha);
   }

   // Preserve the existing RMS banding in sRGB and straight alpha, but average before rounding to bytes.
   const int block_size = std::max(1, int((1.0 - Resolution) * Ramp.size()));
   for (int i=0; i < std::ssize(Ramp); i += block_size) {
      const int end = std::min(i + block_size, int(Ramp.size()));
      auto colour = Ramp[i];
      if (end - i > 1) {
         agg::rgba sum(0.0, 0.0, 0.0, 0.0);
         for (int j=i; j < end; j++) {
            const auto &sample = Ramp[j];
            sum.r += sample.r * sample.r;
            sum.g += sample.g * sample.g;
            sum.b += sample.b * sample.b;
            sum.a += sample.a * sample.a;
         }
         const double count = end - i;
         colour = { std::sqrt(sum.r / count), std::sqrt(sum.g / count), std::sqrt(sum.b / count),
            std::sqrt(sum.a / count) };
      }

      const agg::rgba8 pixel(agg::uround(colour.r * 255.0), agg::uround(colour.g * 255.0),
         agg::uround(colour.b * 255.0), agg::uround(colour.a * 255.0));
      for (int j=i; j < end; j++) Table[j] = pixel;
   }
}

//********************************************************************************************************************

GradientColours::GradientColours(const kt::vector<GradientStop> &Stops, VCS ColourSpace, GCI Interpolation,
   GHM HueMethod, double Alpha, double Resolution, double Gamma, GEZ Easing)
{
   const bool use_oklab = Interpolation != GCI::DEFAULT;
   const bool linear = use_oklab or (ColourSpace IS VCS::LINEAR_RGB);
   std::array<agg::rgba, 256> ramp;
   ramp.fill(agg::rgba(0, 0, 0, 0));

   std::array<double, 256> positions;
   for (int i=0; i < std::ssize(positions); i++) {
      positions[i] = (Gamma IS 1.0) ? double(i) : std::pow(double(i) / 255.0, Gamma) * 255.0;
   }

   auto stop_index = [](double Offset) {
      return (Offset > 0.0) ? int(255.0 * std::min(Offset, 1.0)) : 0;
   };

   // Evaluate the original segments at the gamma-remapped positions. Resampling adjacent table entries
   // would blend across a hard stop and apply the sRGB transfer function before interpolation.
   auto first = positions.begin();
   auto begin = gradient_stop_colour(Stops.front().RGB, linear);
   oklab::Lab begin_lab {};
   if (use_oklab) {
      begin_lab = oklab::from_linear_rgb(begin.r, begin.g, begin.b);
      begin = map_oklab_to_linear_rgb(begin_lab, begin.a);
   }
   for (int stop=0; stop < std::ssize(Stops)-1; stop++) {
      const int i1 = stop_index(Stops[stop].Offset);
      const int i2 = stop_index(Stops[stop + 1].Offset);
      auto end = gradient_stop_colour(Stops[stop + 1].RGB, linear);
      std::optional<GradientOKSegment> segment;
      if (use_oklab) {
         const auto end_lab = oklab::from_linear_rgb(end.r, end.g, end.b);
         segment.emplace(begin_lab, end_lab, begin.a, end.a, Interpolation, HueMethod);
         end = map_oklab_to_linear_rgb(end_lab, end.a);
         begin_lab = end_lab;
      }
      auto start = std::lower_bound(first, positions.end(), double(i1));
      if (stop IS 0) std::fill(ramp.begin(), ramp.begin() + (start - first), begin);

      if (i1 < i2) {
         const auto finish = std::upper_bound(start, positions.end(), double(i2));
         for (auto pos=start; pos != finish; ++pos) {
            const double blend = ease(Easing, (*pos - i1) / (i2 - i1));
            ramp[pos - first] = segment ? segment->sample(blend) : interpolate_ramp_colour(begin, end, blend);
         }
      }
      else if (i1 IS i2) {
         for (auto pos=start; (pos != positions.end()) and (*pos IS i1); ++pos) ramp[pos - first] = end;
      }

      if (stop IS std::ssize(Stops)-2) {
         auto finish = std::lower_bound(first, positions.end(), double(i2));
         std::fill(ramp.begin() + (finish - first), ramp.end(), end);
      }
      begin = end;
   }

   store_colour_ramp(table, ramp, linear, Alpha, Resolution);
}

GradientColours::GradientColours(const std::array<FRGB, 256> &Map, double Resolution, double Gamma, GEZ Easing)
{
   std::array<agg::rgba, 256> ramp;
   for (int i=0; i < std::ssize(ramp); i++) {
      const double position = (Gamma IS 1.0) ? double(i) / 255.0 : std::pow(double(i) / 255.0, Gamma);
      const double source = ((Gamma IS 1.0) and (Easing IS GEZ::LINEAR)) ? double(i) : ease(Easing, position) * 255.0;
      const int index = int(source);
      const auto begin = gradient_stop_colour(Map[index], false);
      const auto end = gradient_stop_colour(Map[std::min(index + 1, 255)], false);
      ramp[i] = interpolate_ramp_colour(begin, end, source - index);
   }

   store_colour_ramp(table, ramp, false, 1.0, Resolution);
}

static ERR rebuild_gradient_colours(extGradient *Self)
{
   kt::Log log(__FUNCTION__);
   std::unique_ptr<GradientColours> colours;

   if (not Self->Stops.empty()) {
      colours.reset(new (std::nothrow) GradientColours(Self->Stops, Self->ColourSpace, Self->Interpolation,
         Self->HueMethod, 1.0, Self->Resolution, Self->Gamma, Self->Easing));
   }
   else if (not Self->ColourMap.empty()) {
      if (auto it = glColourMaps.find(Self->ColourMap); it != glColourMaps.end()) {
         colours.reset(new (std::nothrow) GradientColours(it->second, Self->Resolution, Self->Gamma, Self->Easing));
      }
      else return log.warning(ERR::NotFound);
   }
   else return ERR::Okay;

   if (not colours) return log.warning(ERR::AllocMemory);

   if (Self->Colours) delete Self->Colours;
   Self->Colours = colours.release();
   invalidate_gradient_tables(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR GRADIENT_Init(extGradient *Self)
{
   kt::Log log;

   if ((int(Self->SpreadMethod) <= 0) or (int(Self->SpreadMethod) >= int(VSPREAD::END))) {
      log.traceWarning("Invalid SpreadMethod value of %d", Self->SpreadMethod);
      return ERR::OutOfRange;
   }

   if ((int(Self->Units) <= 0) or (int(Self->Units) >= int(VUNIT::END))) {
      log.traceWarning("Invalid Units value of %d", Self->Units);
      return ERR::OutOfRange;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Colour: The default background colour to use when clipping is enabled.

The colour value in this field is applicable only when a gradient is in clip-mode by specifying the `VSPREAD::CLIP`
flag in #SpreadMethod.  By default, this field has an alpha value of zero to ensure that nothing is drawn outside
the initial bounds of the gradient.  Setting any other colour value will otherwise fill in those areas.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Colour(extGradient *Self, FRGB *Value)
{
   if (Value) {
      Self->Colour = *Value;

      Self->ColourRGB.Red   = std::clamp<uint8_t>(Self->Colour.Red * 255.0, 0, 255);
      Self->ColourRGB.Green = std::clamp<uint8_t>(Self->Colour.Green * 255.0, 0, 255);
      Self->ColourRGB.Blue  = std::clamp<uint8_t>(Self->Colour.Blue * 255.0, 0, 255);
      Self->ColourRGB.Alpha = std::clamp<uint8_t>(Self->Colour.Alpha * 255.0, 0, 255);
   }
   else Self->Colour.Alpha = 0;

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ColourMap: Assigns a pre-defined colourmap to the gradient.

An alternative to defining colour #Stops in a gradient is available in the form of named colourmaps.
Declaring a colourmap in this field will automatically populate the gradient with the colours defined in the map.

We currently support the following established colourmaps from the matplotlib and seaborn projects: `cmap:crest`,
`cmap:flare`, `cmap:icefire`, `cmap:inferno`, `cmap:magma`, `cmap:mako`, `cmap:plasma`, `cmap:rocket`,
`cmap:viridis`.

The use of colourmaps and custom stops are mutually exclusive.

*********************************************************************************************************************/

static ERR GRADIENT_SET_ColourMap(extGradient *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::NoData;

   if (auto it = glColourMaps.find(Value); it != glColourMaps.end()) {
      auto colours = std::unique_ptr<GradientColours>(new (std::nothrow) GradientColours(it->second, Self->Resolution,
         Self->Gamma, Self->Easing));
      if (not colours) return ERR::AllocMemory;

      if (Self->Colours) delete Self->Colours;
      Self->Colours = colours.release();
      Self->Stops.clear();
      Self->ColourMap = Value;
      invalidate_gradient_tables(Self);
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::NotFound;
}

/*********************************************************************************************************************

-FIELD-
ColourSpace: Defines the colour space to use when interpolating gradient colours.
Lookup: VCS

By default, gradient colours are interpolated in sRGB.  Setting `LINEAR_RGB` converts the sRGB stop colours to
linear RGB before interpolation and converts the result back to sRGB.  In both cases, alpha is interpolated
separately from the colour channels (straight alpha).

When #Interpolation is set to a value other than `GCI::DEFAULT`, it takes precedence over this field for gradients
defined by #Stops.  Gouraud, mesh and diffusion gradients always use this field.

-FIELD-
Easing: Selects the easing function for interpolation between gradient stops.
Lookup: GEZ

Easing modifies the interpolation position between each adjacent pair of #Stops.  For gradients sourced from #ColourMap,
it remaps the lookup position through the selected easing function before #Gamma and #Resolution are applied.  The
default `GEZ::LINEAR` mode leaves the gradient unchanged.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Easing(extGradient *Self, GEZ Value)
{
   const GEZ old_easing = Self->Easing;
   if (old_easing IS Value) return ERR::Okay;

   Self->Easing = Value;
   if (Self->Colours) {
      if (auto error = rebuild_gradient_colours(Self); error != ERR::Okay) {
         Self->Easing = old_easing;
         return error;
      }
   }

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
HueMethod: Selects the hue arc for OKLCH stop interpolation.
Lookup: GHM

Determines the direction taken around the hue circle when interpolating between two #Stops in OKLCH, following the
CSS Color 4 hue interpolation rules.  The default is `SHORTER`.  If one of the two stops is achromatic (grey, black
or white) or fully transparent, its hue is undefined and the hue of the other stop is used, so this field has no
effect on that segment.

This field is only used when #Interpolation is `GCI::OKLCH` and the gradient is defined by #Stops.  Gouraud, mesh and
diffusion gradients ignore it.

*********************************************************************************************************************/

static ERR GRADIENT_SET_HueMethod(extGradient *Self, GHM Value)
{
   if ((Value < GHM::SHORTER) or (Value > GHM::DECREASING)) return ERR::OutOfRange;
   const auto old_method = Self->HueMethod;
   if (old_method IS Value) return ERR::Okay;

   Self->HueMethod = Value;
   if (not Self->Stops.empty()) {
      if (auto error = rebuild_gradient_colours(Self); error != ERR::Okay) {
         Self->HueMethod = old_method;
         return error;
      }
   }

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Interpolation: Selects the colour space for interpolation between gradient stops.
Lookup: GCI

`DEFAULT` interpolates in the colour space set by #ColourSpace, with straight alpha.  `OKLAB` and `OKLCH` take
precedence over #ColourSpace and interpolate in a perceptually uniform colour space, which avoids the grey midpoint
that sRGB produces between colours of distant hue.  `OKLAB` is the recommended choice for general use.
`OKLCH` retains more chroma in the midpoint, and the hue path between stops is selected by #HueMethod.

The OKLab and OKLCH modes follow CSS Color 4.  Colour components are premultiplied by alpha during interpolation, so
a fade from a colour to transparent keeps its colour instead of darkening.  Interpolated colours that fall outside
the sRGB gamut are mapped back into it by reducing chroma while preserving lightness and hue.

#Easing and #Gamma continue to apply in every mode.  Gradients sourced from #ColourMap, and Gouraud, mesh and
diffusion gradients, ignore this field.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Interpolation(extGradient *Self, GCI Value)
{
   if ((Value < GCI::DEFAULT) or (Value > GCI::OKLCH)) return ERR::OutOfRange;
   const auto old_interpolation = Self->Interpolation;
   if (old_interpolation IS Value) return ERR::Okay;

   Self->Interpolation = Value;
   if (not Self->Stops.empty()) {
      if (auto error = rebuild_gradient_colours(Self); error != ERR::Okay) {
         Self->Interpolation = old_interpolation;
         return error;
      }
   }

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Gamma: Applies a gamma curve to the gradient ramp.

Gamma remaps the completed colour ramp before #Resolution is applied.  Values greater than `1.0` bias the ramp toward
the first colour, while values between `0.0` and `1.0` bias it toward the last colour.  A value of `1.0` leaves the
gradient unchanged.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Gamma(extGradient *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value <= 0)) return ERR::OutOfRange;

   const double old_gamma = Self->Gamma;
   if (old_gamma IS Value) return ERR::Okay;

   Self->Gamma = Value;
   if (Self->Colours) {
      if (auto error = rebuild_gradient_colours(Self); error != ERR::Okay) {
         Self->Gamma = old_gamma;
         return error;
      }
   }

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SID: String identifier for a gradient.

The SID field is provided for the purpose of SVG support.  Where possible, we recommend that you use the existing
object name and automatically assigned ID's for identifiers.

*********************************************************************************************************************/

static ERR GRADIENT_SET_SID(extGradient *Self, const std::string_view &Value)
{
   if (not Value.empty()) {
      Self->SID = Value;
      Self->NumericID = strhash(Value);
   }
   else {
      Self->SID.clear();
      Self->NumericID = 0;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Matrices: Applies one or more transforms to a gradient.

A transform can be applied to a gradient via one or more matrices.  These will influence how gradient fills are
rendered within their vector space.  Each matrix is represented by a !VectorMatrix structure, and the matrices are
linked in the order in which they should be applied.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Matrices(extGradient *Self, const std::span<const VectorMatrix> *Value)
{
   if (Value) {
      Self->Matrices.assign(Value->begin(), Value->end());

      // Chain the next and prev pointers
      VectorMatrix *prev = nullptr;
      for (auto &matrix : Self->Matrices) {
         matrix.Vector = nullptr;
         matrix.Next = nullptr;
         if (prev) prev->Next = &matrix;
         prev = &matrix;
      }
   }
   else Self->Matrices.clear();

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NumericID: Numeric identifier for a vector.

The NumericID field is provided for internal use by the SVG and vector modules.  If NumericID is set by the client,
then any value in #SID will be immediately cleared.

*********************************************************************************************************************/

static ERR GRADIENT_SET_NumericID(extGradient *Self, int Value)
{
   Self->NumericID = Value;
   Self->SID.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Resolution: Affects the rate of change for colours in the gradient.

By default, the colours generated for a gradient will be spaced for a smooth transition between stops that maximise
resolution.  The resolution can be reduced by setting the Resolution value to a fraction between 0 and 1.0.

This results in the colour values being averaged to a single value for every block of n colours, where n is the value
`1 / (1 - Resolution)`.

Resolution is at its maximum when this value is set to 1 (the default).

*********************************************************************************************************************/

static ERR GRADIENT_SET_Resolution(extGradient *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0) or (Value > 1.0)) return ERR::OutOfRange;

   const double old_resolution = Self->Resolution;
   if (old_resolution IS Value) return ERR::Okay;

   Self->Resolution = Value;

   if (Self->Colours) {
      if (auto error = rebuild_gradient_colours(Self); error != ERR::Okay) {
         Self->Resolution = old_resolution;
         return error;
      }
   }

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SpreadMethod: Determines the rendering behaviour to use when gradient colours are cycled.

SpreadMethod determines what happens when the first cycle of gradient colours is exhausted and needs to begin again.
The default setting is `VSPREAD::PAD`.

*********************************************************************************************************************/

static ERR GRADIENT_SET_SpreadMethod(extGradient *Self, VSPREAD Value)
{
   Self->SpreadMethod = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stops: Defines the colours to use for the gradient.

The colours that will be used for drawing a gradient are defined by the Stops array.  At least two stops are required
to define a start and end point for interpolating the gradient colours.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Stops(extGradient *Self, std::span<const GradientStop> *Value)
{
   if ((Value) and (Value->size() >= 2)) {
      auto stops = kt::vector<GradientStop>(Value->begin(), Value->end());
      auto colours = std::unique_ptr<GradientColours>(new (std::nothrow) GradientColours(
         stops, Self->ColourSpace, Self->Interpolation, Self->HueMethod, 1.0, Self->Resolution, Self->Gamma,
         Self->Easing));
      if (not colours) return ERR::AllocMemory;

      Self->Stops = std::move(stops);
      if (Self->Colours) delete Self->Colours;
      Self->Colours = colours.release();
      Self->ColourMap.clear();
      invalidate_gradient_tables(Self);
      Self->modified();
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
Transform: Applies a transform to the gradient.

A transform can be applied to the gradient by setting this field with an SVG compliant transform string.

The transform operates in the coordinate system defined by @Gradient.Units.  For `BOUNDING_BOX` that is the normalised
space of the target's bounding box, as for SVG's `gradientTransform` with `objectBoundingBox`: `translate(0.25 0)` moves
the gradient by a quarter of the target's width, and `rotate(45 0.5 0.5)` rotates it about the centre of the target.
For `USERSPACE`, distances are measured in the target's user coordinates and scale with the viewport.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Transform(extGradient *Self, const std::string_view &Commands)
{
   if (Commands.empty()) return kt::Log().warning(ERR::InvalidValue);

   if (Self->initialised()) Self->modified();

   if (Self->Matrices.empty()) {
      Self->Matrices.resize(1);
      return vec::ParseTransform(Self->Matrices.data(), Commands);
   }
   else {
      vec::ResetMatrix(Self->Matrices.data());
      return vec::ParseTransform(Self->Matrices.data(), Commands);
   }
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML element string that describes the gradient type.

Derived gradient classes override this field to return the SVG element name and any shape-specific attributes.
The returned string does not include angle brackets, common gradient attributes, stops or child elements.

-END-

*********************************************************************************************************************/

static ERR GRADIENT_GET_XMLDef(extGradient *, std::string &)
{
   return ERR::NoSupport;
}

static std::string gradient_xml_num(double Value)
{
   std::stringstream stream;
   stream << Value;
   return stream.str();
}

static void gradient_xml_attr(std::stringstream &Stream, const std::string_view Name, double Value)
{
   Stream << " " << Name << "=\"" << gradient_xml_num(Value) << "\"";
}

static void gradient_xml_attr(std::stringstream &Stream, const std::string_view Name, const Unit &Value)
{
   if (Value.defined()) {
      if (Value.scaled()) Stream << " " << Name << "=\"" << gradient_xml_num(double(Value) * 100.0) << "%\"";
      else Stream << " " << Name << "=\"" << gradient_xml_num(double(Value)) << "\"";
   }
}

/*********************************************************************************************************************

-FIELD-
Units: Defines the coordinate system for gradient coordinates.

The default coordinate system for gradients is `BOUNDING_BOX`, which positions the gradient around the vector that
references it.  The alternative is `USERSPACE`, which positions the gradient scaled to the current viewport.

*********************************************************************************************************************/

extGradient::~extGradient() {
   if (Colours) delete Colours;
}

//********************************************************************************************************************

#include "gradient_def.c"

static const FieldArray clGradientFields[] = {
   { "Matrices",     FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, GRADIENT_SET_Matrices, "VectorMatrix" },
   { "Stops",        FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, GRADIENT_SET_Stops, "GradientStop" },
   { "SID",          FDF_CPPSTRING|FDF_RW,      nullptr, GRADIENT_SET_SID },
   { "ColourMap",    FDF_CPPSTRING|FDF_RW,      nullptr, GRADIENT_SET_ColourMap },
   { "Resolution",   FDF_DOUBLE|FDF_RW,         nullptr, GRADIENT_SET_Resolution },
   { "Gamma",        FDF_DOUBLE|FDF_RW,         nullptr, GRADIENT_SET_Gamma },
   { "Colour",       FDF_STRUCT|FD_RW,          nullptr, GRADIENT_SET_Colour, "FRGB" },
   { "SpreadMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENT_SET_SpreadMethod, &clGradientSpreadMethod },
   { "Units",        FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clGradientUnits },
   { "ColourSpace",  FDF_INT|FDF_RI,            nullptr, nullptr, &clGradientColourSpace },
   { "Easing",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENT_SET_Easing, &clGradientEasing },
   { "Interpolation", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENT_SET_Interpolation, &clGradientInterpolation },
   { "HueMethod",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENT_SET_HueMethod, &clGradientHueMethod },
   { "NumericID",    FDF_INT|FDF_RW,            nullptr, GRADIENT_SET_NumericID },
   // Virtual fields
   { "Transform",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_W, nullptr, GRADIENT_SET_Transform },
   { "XMLDef",       FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, GRADIENT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_gradient(void) // The gradient is a definition type for creating gradients and not drawing.
{
   clGradient = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::Name("Gradient"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientActions),
      fl::Fields(clGradientFields),
      fl::Size(sizeof(extGradient)),
      fl::Path(MOD_PATH));

   if (!clGradient) return ERR::AddClass;

   ERR error;
   if ((error = init_gradient_linear()) != ERR::Okay) return error;
   if ((error = init_gradient_radial()) != ERR::Okay) return error;
   if ((error = init_gradient_conic()) != ERR::Okay) return error;
   if ((error = init_gradient_diamond()) != ERR::Okay) return error;
   if ((error = init_gradient_contour()) != ERR::Okay) return error;
   if ((error = init_gradient_gouraud()) != ERR::Okay) return error;
   if ((error = init_gradient_mesh()) != ERR::Okay) return error;
   if ((error = init_gradient_diffusion()) != ERR::Okay) return error;
   if ((error = init_gradient_distal()) != ERR::Okay) return error;
   if ((error = init_gradient_voronoi()) != ERR::Okay) return error;

   return error;
}
