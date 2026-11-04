
static constexpr auto HASH_URL   = kt::strhash("url");
static constexpr auto HASH_RGB   = kt::strhash("rgb");
static constexpr auto HASH_RGBA  = kt::strhash("rgba");
static constexpr auto HASH_OKLAB = kt::strhash("oklab");
static constexpr auto HASH_OKLCH = kt::strhash("oklch");
static constexpr auto HASH_LAB   = kt::strhash("lab");
static constexpr auto HASH_LCH   = kt::strhash("lch");
static constexpr auto HASH_HSL   = kt::strhash("hsl");
static constexpr auto HASH_HSLA  = kt::strhash("hsla");
static constexpr auto HASH_HSV   = kt::strhash("hsv");
static constexpr auto HASH_COLOR = kt::strhash("color");

inline char read_nibble(const char Alpha)
{
   if (std::isdigit(Alpha)) return (Alpha - '0');
   else if ((Alpha >= 'A') and (Alpha <= 'F')) return ((Alpha - 'A')+10);
   else if ((Alpha >= 'a') and (Alpha <= 'f')) return ((Alpha - 'a')+10);
   else return char(0xff);
}

// Remove the sRGB transfer function.  Negative values are mirrored, as in CSS Color 4.

inline double srgb_to_linear(double V)
{
   const double sign = (V < 0) ? -1.0 : 1.0;
   V = std::abs(V);
   if (V <= 0.04045) return sign * V / 12.92;
   return sign * pow((V + 0.055) / 1.055, 2.4);
}

inline double linear_to_srgb(double V)
{
   const double sign = (V < 0) ? -1.0 : 1.0;
   V = std::abs(V);
   if (V <= 0.0031308) return sign * 12.92 * V;
   return sign * (1.055 * pow(V, 1.0 / 2.4) - 0.055);
}

// Store linear sRGB values into a VectorPainter, computing both the sRGB colour and the CIE XYZ representation.

static void linear_rgb_to_painter(double LR, double LG, double LB, float Alpha, VectorPainter *Painter)
{
   auto &rgb = Painter->Colour;
   rgb.Red   = (float)linear_to_srgb(LR); // Can potentially overflow the 0 - 1.0 sRGB range (permitted)
   rgb.Green = (float)linear_to_srgb(LG);
   rgb.Blue  = (float)linear_to_srgb(LB);
   rgb.Alpha = Alpha;
}

inline void skip_whitespace(std::string_view &IRI)
{
   std::size_t i = 0;
   const auto size = IRI.size();

   while ((i < size) and (uint8_t(IRI[i]) <= 0x20)) i++;
   if (i) IRI.remove_prefix(i);
}

static double parse_number(std::string_view &IRI, std::errc *Error = nullptr)
{
   if (Error) *Error = std::errc::invalid_argument;
   skip_whitespace(IRI);
   if (IRI.empty()) return 0;
   if (IRI.starts_with('+')) IRI.remove_prefix(1);

   double value = 0;
   auto [ ptr, error ] = std::from_chars(IRI.data(), IRI.data() + IRI.size(), value);
   if (Error) *Error = error;
   if (error IS std::errc()) IRI.remove_prefix(ptr - IRI.data());
   return value;
}

// Expose the content that follows the current value, which will start with a separator or a fallback colour.

inline void advance_result(std::string_view IRI, std::string_view &Result)
{
   skip_whitespace(IRI);
   Result = IRI;
}

// Advance a function-style colour parser past its closing bracket and expose the content that follows it.

static void advance_function_result(std::string_view &IRI, std::string_view &Result)
{
   skip_whitespace(IRI);
   if (IRI.starts_with(')')) IRI.remove_prefix(1);
   advance_result(IRI, Result);
}

// Parse a CSS colour component: reads a double, applies percentage scaling, and skips trailing whitespace.

inline double parse_css_value(std::string_view &IRI, double PercentScale = 0.01)
{
   double value = parse_number(IRI);
   if (IRI.starts_with('%')) { value *= PercentScale; IRI.remove_prefix(1); }
   skip_whitespace(IRI);
   return value;
}

// Parse optional CSS alpha after '/' separator.  Returns the parsed alpha value, or 1.0 if absent.

static float parse_css_alpha(std::string_view &IRI)
{
   skip_whitespace(IRI);
   if (IRI.starts_with('/')) {
      IRI.remove_prefix(1);
      float alpha = (float)parse_number(IRI);
      if (IRI.starts_with('%')) { alpha *= 0.01f; IRI.remove_prefix(1); }
      return std::clamp(alpha, 0.0f, 1.0f);
   }
   return 1.0f;
}

// Convert OKLAB (L, a, b) to linear sRGB and store in the painter.

static void oklab_to_painter(double L, double OKA, double OKB, float Alpha, VectorPainter *Painter)
{
   L = std::clamp(L, 0.0, 1.0);

   // OKLAB to linear sRGB via the intermediate LMS cube-root space
   const double l_ = L + 0.3963377774 * OKA + 0.2158037573 * OKB;
   const double m_ = L - 0.1055613458 * OKA - 0.0638541728 * OKB;
   const double s_ = L - 0.0894841775 * OKA - 1.2914855480 * OKB;

   const double ll = l_ * l_ * l_;
   const double mm = m_ * m_ * m_;
   const double ss = s_ * s_ * s_;

   double lr = +4.0767416621 * ll - 3.3077115913 * mm + 0.2309699292 * ss;
   double lg = -1.2684380046 * ll + 2.6097574011 * mm - 0.3413193965 * ss;
   double lb = -0.0041960863 * ll - 0.7034186147 * mm + 1.7076147010 * ss;

   linear_rgb_to_painter(lr, lg, lb, Alpha, Painter);
}

// Convert CIE Lab (L, a, b) to CIE XYZ (D65) and linear sRGB, then store in the painter.

static void cielab_to_painter(double L, double A, double B, float Alpha, VectorPainter *Painter)
{
   L = std::clamp(L, 0.0, 100.0);

   // CIE Lab to CIE XYZ (D50 illuminant as per CSS Color Level 4)
   const double fy = (L + 16.0) / 116.0;
   const double fx = A / 500.0 + fy;
   const double fz = fy - B / 200.0;

   const double delta = 6.0 / 29.0;
   const double delta_sq = delta * delta;

   auto f_inv = [&](double T) -> double {
      return (T > delta) ? T * T * T : 3.0 * delta_sq * (T - 4.0 / 29.0);
   };

   // D50 white point
   const double xn = 0.96422;
   const double yn = 1.0;
   const double zn = 0.82521;

   double x50 = xn * f_inv(fx);
   double y50 = yn * f_inv(fy);
   double z50 = zn * f_inv(fz);

   // Chromatic adaptation from D50 to D65 (Bradford transform)
   double cx = x50 *  0.9555766 + y50 * -0.0230393 + z50 *  0.0631636;
   double cy = x50 * -0.0282895 + y50 *  1.0099416 + z50 *  0.0210077;
   double cz = x50 *  0.0122982 + y50 * -0.0204830 + z50 *  1.3299098;

   // CIE XYZ (D65) to linear sRGB
   double lr = +3.2404541 * cx - 1.5371385 * cy - 0.4985314 * cz;
   double lg = -0.9692660 * cx + 1.8760108 * cy + 0.0415560 * cz;
   double lb =  0.0556434 * cx - 0.2040259 * cy + 1.0572252 * cz;

   linear_rgb_to_painter(lr, lg, lb, Alpha, Painter);
}

//********************************************************************************************************************

static ERR parse_url(kt::Log &Log, objVectorScene *Scene, std::string_view IRI, VectorPainter *Painter,
   std::string_view &Result)
{
   if (not Scene) return Log.warning(ERR::NullArgs);

   if (Scene->Class->BaseClassID IS CLASSID::VECTOR) Scene = ((objVector *)Scene)->Scene;
   else if (Scene->classID() != CLASSID::VECTORSCENE) return Log.warning(ERR::InvalidObject);

   if (Scene->HostScene) Scene = Scene->HostScene;

   if (IRI.empty() or (IRI[0] != '#')) {
      Log.warning("Invalid IRI: %.*s", int(IRI.size()), IRI.data());
      return ERR::Syntax;
   }

   // Compute the hash identifier
   std::size_t i;
   for (i = 1; (i < IRI.size()) and (IRI[i] != ')'); i++);
   std::string lookup;
   lookup.assign(IRI.substr(1, i - 1));

   bool found = false;
   auto ext_scene = (extVectorScene *)Scene;
   if (auto def_lookup = ext_scene->Defs.find(lookup); def_lookup != ext_scene->Defs.end()) {
      auto def = def_lookup->second;
      if (def->baseClassID() IS CLASSID::GRADIENT) Painter->Gradient = (objGradient *)def;
      else if (def->classID() IS CLASSID::VECTORIMAGE) Painter->Image = (objVectorImage *)def;
      else if (def->classID() IS CLASSID::VECTORPATTERN) Painter->Pattern = (objVectorPattern *)def;
      else Log.warning("Vector definition '%s' (class $%.8x) not supported.", lookup.c_str(), uint32_t(def->classID()));
      found = true;
   }
   else if (auto map_lookup = glColourMaps.find(lookup); map_lookup != glColourMaps.end()) {
      // Referencing a pre-defined colour map results in it being added to the Scene's definitions as a linear gradient.
      // It is then accessible permanently under that name.

      extGradientLinear *gradient;
      if (!NewObject(CLASSID::GRADIENTLINEAR, &gradient)) {
         SetOwner(gradient, Scene);
         gradient->setName(lookup);
         gradient->setUnits(VUNIT::BOUNDING_BOX);
         gradient->X1 = Unit(0.0);
         gradient->Y1 = Unit(0.0);
         gradient->X2 = Unit(1.0, FD_SCALED);
         gradient->Y2 = Unit(0.0);

         if (gradient->Colours) delete gradient->Colours;
         gradient->Colours = new (std::nothrow) GradientColours(map_lookup->second, 0);

         if (!InitObject(gradient)) {
            Scene->addDef(lookup.c_str(), gradient);
            Painter->Gradient = gradient;
         }
      }
      found = true;
   }

   if (found) {
      IRI.remove_prefix(i);
      if (IRI.starts_with(')')) {
         IRI.remove_prefix(1);
         skip_whitespace(IRI);
      }

      Result = IRI;
      return ERR::Okay;
   }

   Log.warning("Failed to lookup IRI '%.*s' in scene #%d", int(IRI.size()), IRI.data(), Scene->UID);
   return ERR::NotFound;
}

//********************************************************************************************************************

static ERR parse_rgb(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   auto &rgb = Painter->Colour;
   // Supports both legacy comma-separated format: rgb(R,G,B) / rgba(R,G,B,A)
   // and modern CSS space-separated format: rgb(R G B) / rgb(R G B / A)
   // Component values are permitted to exceed the standard ranges of 0-255 or 0%-100% if representing colours
   // outside the sRGB gamut.

   skip_whitespace(IRI);

   rgb.Red = parse_number(IRI) * (1.0 / 255.0);
   if (IRI.starts_with('%')) { rgb.Red *= (255.0 / 100.0); IRI.remove_prefix(1); }
   skip_whitespace(IRI);

   bool legacy = IRI.starts_with(',');
   if (legacy) IRI.remove_prefix(1);

   rgb.Green = parse_number(IRI) * (1.0 / 255.0);
   if (IRI.starts_with('%')) { rgb.Green *= (255.0 / 100.0); IRI.remove_prefix(1); }
   skip_whitespace(IRI);
   if (legacy and IRI.starts_with(',')) IRI.remove_prefix(1);

   rgb.Blue = parse_number(IRI) * (1.0 / 255.0);
   if (IRI.starts_with('%')) { rgb.Blue *= (255.0 / 100.0); IRI.remove_prefix(1); }
   skip_whitespace(IRI);

   if (legacy) {
      if (IRI.starts_with(',')) {
         IRI.remove_prefix(1);
         rgb.Alpha = (float)parse_number(IRI);
         if (IRI.starts_with('%')) { rgb.Alpha *= 0.01f; IRI.remove_prefix(1); }
         rgb.Alpha = std::clamp(rgb.Alpha, 0.0f, 1.0f);
      }
      else rgb.Alpha = 1.0;
   }
   else rgb.Alpha = parse_css_alpha(IRI);

   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_oklab(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   // CSS oklab() colour function: oklab(L a b [/ alpha])
   // L: lightness 0-1 (or 0%-100%), a: ~-0.4 to 0.4 (or percentage), b: ~-0.4 to 0.4 (or percentage)
   // Values are space-separated; alpha is optional, preceded by '/'

   skip_whitespace(IRI);

   double l    = parse_css_value(IRI, 0.01);
   double ok_a = parse_css_value(IRI, 0.004); // 100% = 0.4
   double ok_b = parse_css_value(IRI, 0.004); // 100% = 0.4
   float alpha = parse_css_alpha(IRI);

   oklab_to_painter(l, ok_a, ok_b, alpha, Painter);
   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_oklch(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   // CSS oklch() colour function: oklch(L C H [/ alpha])
   // L: lightness 0-1 (or 0%-100%), C: chroma ~0-0.4 (or percentage), H: hue in degrees
   // Values are space-separated; alpha is optional, preceded by '/'

   skip_whitespace(IRI);

   double l     = parse_css_value(IRI, 0.01);
   double c     = parse_css_value(IRI, 0.004); // 100% = 0.4
   double h_deg = parse_number(IRI);
   float alpha  = parse_css_alpha(IRI);

   c = std::max(c, 0.0);

   // OKLCh to OKLAB (polar to cartesian)
   const double h_rad = h_deg * (agg::pi / 180.0);
   const double ok_a = c * cos(h_rad);
   const double ok_b = c * sin(h_rad);

   oklab_to_painter(l, ok_a, ok_b, alpha, Painter);
   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse a channel of the CSS color() function.  Channels are numbers or percentages (100% = 1), or 'none' for zero.

static bool parse_color_channel(std::string_view &IRI, double &Value)
{
   skip_whitespace(IRI);
   if (IRI.starts_with("none")) {
      IRI.remove_prefix(4);
      Value = 0;
   }
   else {
      std::errc error;
      Value = parse_number(IRI, &error);
      if (error != std::errc()) return false;
      if (IRI.starts_with('%')) { Value *= 0.01; IRI.remove_prefix(1); }
   }
   skip_whitespace(IRI);
   return true;
}

//********************************************************************************************************************
// Transfer functions of the Rec. 2020 and Adobe RGB (1998) spaces, from encoded to linear values.  Negative values are
// mirrored, as in CSS Color 4.

static double rec2020_to_linear(double Value)
{
   // CSS Color 4 uses the BT.1886 display transfer function with zero black lift and unit gain.
   return std::copysign(std::pow(std::abs(Value), 2.4), Value);
}

static double a98_to_linear(double Value)
{
   return std::copysign(std::pow(std::abs(Value), 563.0 / 256.0), Value);
}

//********************************************************************************************************************
// CSS color() function: color(space R G B [/ alpha]), where space is srgb, srgb-linear, display-p3, a98-rgb or
// rec2020.  Display P3 uses the sRGB transfer function, and all of the spaces use the D65 white point.  Channels
// outside 0 - 1 are accepted, so a colour outside sRGB produces sRGB values outside 0 - 1.  Rec. 2020 is read as an
// SDR container gamut.

static ERR parse_color(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   // Linear RGB to linear sRGB, composed from the CSS Color 4 space to XYZ and XYZ to sRGB matrices

   static constexpr double P3_TO_SRGB[3][3] = {
      {  1.22494017628056,    -0.22494017628056,    0.0 },
      { -0.04205695470968816,  1.042056954709688,   0.0 },
      { -0.01963755459033443, -0.07863604555063189, 1.098273600140966 }
   };

   static constexpr double REC2020_TO_SRGB[3][3] = {
      {  1.6604910021084343,  -0.5876411387885496,  -0.0728498633198848 },
      { -0.12455047452159085,  1.1328998971259603,  -0.00834942260436947 },
      { -0.01815076335490531, -0.10057889800800737,  1.1187296613629127 }
   };

   static constexpr double A98_TO_SRGB[3][3] = {
      { 1.3983557439607786, -0.3983557439607784,  0.0 },
      { 0.0,                 1.0,                 0.0 },
      { 0.0,                -0.04292898929447327, 1.0429289892944733 }
   };

   auto convert = [Painter](const double (&M)[3][3], double R, double G, double B, float Alpha) {
      linear_rgb_to_painter(M[0][0] * R + M[0][1] * G + M[0][2] * B,
         M[1][0] * R + M[1][1] * G + M[1][2] * B,
         M[2][0] * R + M[2][1] * G + M[2][2] * B, Alpha, Painter);
   };

   skip_whitespace(IRI);

   std::size_t len = 0;
   while ((len < IRI.size()) and ((std::isalnum(uint8_t(IRI[len]))) or (IRI[len] IS '-'))) len++;
   auto space = IRI.substr(0, len);
   IRI.remove_prefix(len);

   double r, g, b;
   if ((not parse_color_channel(IRI, r)) or (not parse_color_channel(IRI, g)) or
       (not parse_color_channel(IRI, b))) return ERR::Syntax;
   float alpha = parse_css_alpha(IRI);

   if (kt::iequals(space, "srgb")) {
      auto &rgb = Painter->Colour;
      rgb.Red   = float(r);
      rgb.Green = float(g);
      rgb.Blue  = float(b);
      rgb.Alpha = alpha;
   }
   else if (kt::iequals(space, "srgb-linear")) linear_rgb_to_painter(r, g, b, alpha, Painter);
   else if (kt::iequals(space, "display-p3")) {
      convert(P3_TO_SRGB, srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), alpha);
   }
   else if (kt::iequals(space, "rec2020")) {
      convert(REC2020_TO_SRGB, rec2020_to_linear(r), rec2020_to_linear(g), rec2020_to_linear(b), alpha);
   }
   else if (kt::iequals(space, "a98-rgb")) {
      convert(A98_TO_SRGB, a98_to_linear(r), a98_to_linear(g), a98_to_linear(b), alpha);
   }
   else return ERR::Syntax;

   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// CSS lab() colour function: lab(L a b [/ alpha])
// L: lightness 0-100 (or 0%-100%), a: ~-125 to 125 (or percentage), b: ~-125 to 125 (or percentage)
// Values are space-separated; alpha is optional, preceded by '/'

static ERR parse_lab(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   skip_whitespace(IRI);

   double l    = parse_css_value(IRI, 1.0);  // Percentage maps to 0-100
   double a    = parse_css_value(IRI, 1.25); // 100% = 125
   double b    = parse_css_value(IRI, 1.25); // 100% = 125
   float alpha = parse_css_alpha(IRI);

   cielab_to_painter(l, a, b, alpha, Painter);
   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// CSS lch() colour function: lch(L C H [/ alpha])
// L: lightness 0-100 (or 0%-100%), C: chroma ~0-150 (or percentage), H: hue in degrees
// Values are space-separated; alpha is optional, preceded by '/'

static ERR parse_lch(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   skip_whitespace(IRI);

   double l     = parse_css_value(IRI, 1.0); // Percentage maps to 0-100
   double c     = parse_css_value(IRI, 1.5); // 100% = 150
   double h_deg = parse_number(IRI);
   float alpha  = parse_css_alpha(IRI);

   c = std::max(c, 0.0);

   // LCH to Lab (polar to cartesian)
   const double h_rad = h_deg * (agg::pi / 180.0);
   const double a = c * cos(h_rad);
   const double b = c * sin(h_rad);

   cielab_to_painter(l, a, b, alpha, Painter);
   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse the common (H, S, X [, A]) format used by HSL and HSV.
// Returns hue (0-1), saturation (0-1), third component (0-1), and alpha (0-1).

static double parse_hsx_percent(std::string_view &IRI)
{
   double value = parse_number(IRI);
   skip_whitespace(IRI);
   if (IRI.starts_with('%')) IRI.remove_prefix(1);
   return std::clamp(value * 0.01, 0.0, 1.0);
}

static float parse_hsx_alpha(std::string_view &IRI)
{
   float alpha = (float)parse_number(IRI);
   skip_whitespace(IRI);
   if (IRI.starts_with('%')) {
      alpha *= 0.01f;
      IRI.remove_prefix(1);
   }
   return std::clamp(alpha, 0.0f, 1.0f);
}

// Supports both the legacy comma-separated format: hsl(H,S,L) / hsla(H,S,L,A)
// and the modern CSS space-separated format: hsl(H S L) / hsl(H S L / A)
// The hue accepts the CSS angle units and wraps to the range 0 - 360 degrees.

static ERR parse_hsx(std::string_view &IRI, double &Hue, double &Sat, double &Third, float &Alpha)
{
   std::errc error;
   double hue = parse_number(IRI, &error);
   if ((error != std::errc()) or (not std::isfinite(hue))) return ERR::Syntax;

   double period = 360.0;
   if (kt::startswith("deg", IRI)) IRI.remove_prefix(3);
   else if (kt::startswith("grad", IRI)) { period = 400.0; IRI.remove_prefix(4); }
   else if (kt::startswith("rad", IRI)) { period = 2.0 * agg::pi; IRI.remove_prefix(3); }
   else if (kt::startswith("turn", IRI)) { period = 1.0; IRI.remove_prefix(4); }

   // Wrap in the original unit so large finite angles cannot overflow during conversion.
   hue = std::fmod(hue, period);
   if (hue < 0) hue += period;
   Hue = hue / period;
   skip_whitespace(IRI);

   const bool legacy = IRI.starts_with(',');
   if (legacy) IRI.remove_prefix(1);
   Sat = parse_hsx_percent(IRI);
   skip_whitespace(IRI);

   if (legacy and IRI.starts_with(',')) IRI.remove_prefix(1);
   Third = parse_hsx_percent(IRI);
   skip_whitespace(IRI);

   if (legacy) {
      if (IRI.starts_with(',')) {
         IRI.remove_prefix(1);
         Alpha = parse_hsx_alpha(IRI);
      }
      else Alpha = 1.0f;
   }
   else Alpha = parse_css_alpha(IRI);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_hsl(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   auto &rgb = Painter->Colour;
   double hue, sat, light;
   if (auto error = parse_hsx(IRI, hue, sat, light, rgb.Alpha); error != ERR::Okay) return error;

   // Convert HSL to RGB.  HSL values are from 0.0 - 1.0

   auto hueToRgb = [](double p, double q, double t) {
      if (t < 0) t += 1;
      if (t > 1) t -= 1;
      if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
      if (t < 1.0/2.0) return q;
      if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
      return p;
   };

   if (sat IS 0) {
      rgb.Red = rgb.Green = rgb.Blue = light;
   }
   else {
      const double q = (light < 0.5) ? light * (1.0 + sat) : light + sat - light * sat;
      const double p = 2.0 * light - q;
      rgb.Red   = hueToRgb(p, q, hue + 1.0/3.0);
      rgb.Green = hueToRgb(p, q, hue);
      rgb.Blue  = hueToRgb(p, q, hue - 1.0/3.0);
   }

   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_hsv(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   auto &rgb = Painter->Colour;
   double hue, sat, val;
   if (auto error = parse_hsx(IRI, hue, sat, val, rgb.Alpha); error != ERR::Okay) return error;

   hue = hue * 6.0;
   int i = floor(hue);
   double f = hue - i;

   if (!(i & 1)) f = 1.0 - f; // if i is even

   double m = val * (1.0 - sat);
   double n = val * (1.0 - sat * f);

   switch (i) {
      case 6:
      case 0:  rgb.Red = val; rgb.Green = n;   rgb.Blue = m; break;
      case 1:  rgb.Red = n;   rgb.Green = val; rgb.Blue = m; break;
      case 2:  rgb.Red = m;   rgb.Green = val; rgb.Blue = n; break;
      case 3:  rgb.Red = m;   rgb.Green = n;   rgb.Blue = val; break;
      case 4:  rgb.Red = n;   rgb.Green = m;   rgb.Blue = val; break;
      case 5:  rgb.Red = val; rgb.Green = m;   rgb.Blue = n; break;
      default: rgb.Red = 0;   rgb.Green = 0;   rgb.Blue = 0; break;
   }

   advance_function_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_hex(std::string_view IRI, VectorPainter *Painter, std::string_view &Result)
{
   auto &rgb = Painter->Colour;
   IRI.remove_prefix(1);
   char nibbles[8];
   uint8_t n = 0;
   while ((not IRI.empty()) and (n < std::ssize(nibbles))) {
      auto nibble = read_nibble(IRI[0]);
      if (nibble IS char(0xff)) break;
      nibbles[n++] = nibble;
      IRI.remove_prefix(1);
   }

   if (n IS 3) {
      // Expand shorthand #RGB by duplicating each nibble
      nibbles[5] = nibbles[4] = nibbles[2];
      nibbles[3] = nibbles[2] = nibbles[1];
      nibbles[1] = nibbles[0];
      n = 6;
   }

   if ((n IS 6) or (n IS 8)) {
      rgb.Red   = double((nibbles[0]<<4) | nibbles[1]) * (1.0 / 255.0);
      rgb.Green = double((nibbles[2]<<4) | nibbles[3]) * (1.0 / 255.0);
      rgb.Blue  = double((nibbles[4]<<4) | nibbles[5]) * (1.0 / 255.0);
      rgb.Alpha = (n IS 8) ? double((nibbles[6]<<4) | nibbles[7]) * (1.0 / 255.0) : 1.0;
   }
   else return ERR::Syntax;

   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_named_colour(kt::Log &Log, std::string_view IRI, std::size_t Separator, VectorPainter *Painter,
   std::string_view &Result)
{
   auto colour = IRI.substr(0, Separator);
   auto hash = strihash(colour);

   const RGB8 *src = nullptr;

   if (auto it = glNamedColours.find(hash); it != glNamedColours.end()) {
      src = &it->second;
   }
   else if (auto it = glAppColours.find(hash); it != glAppColours.end()) {
      src = &it->second;
   }

   if (src) {
      auto &rgb = Painter->Colour;
      rgb.Red   = (float)src->Red   * (1.0 / 255.0);
      rgb.Green = (float)src->Green * (1.0 / 255.0);
      rgb.Blue  = (float)src->Blue  * (1.0 / 255.0);
      rgb.Alpha = (float)src->Alpha * (1.0 / 255.0);
      advance_result(IRI.substr(Separator), Result);
      return ERR::Okay;
   }

   // Note: Resolving 'currentColour' is handled in the SVG parser and not the Vector API.
   Log.warning("Failed to interpret colour \"%.*s\"", int(colour.size()), colour.data());
   return ERR::Syntax;
}

/*********************************************************************************************************************

-FUNCTION-
ReadPainter: Parses a painter string to its colour, gradient, pattern or image value.

This function will parse an SVG style IRI into its equivalent logical values.  The results can then be processed for
rendering a stroke or fill operation in the chosen style.

Colours can be expressed in the following formats:

<types>
<type name="Named colour">Standard SVG colour names such as `orange` and `red` are accepted.  Application-defined
colour names are also supported.</type>
<type name="#RRGGBB / #RRGGBBAA">Hexadecimal formats.  Alpha defaults to fully opaque when omitted.</type>
<type name="rgb(R,G,B) / rgba(R,G,B,A)">Legacy comma-separated format.  Component values range from `0` to `255`,
or from `0%` to `100%`.  The alpha component ranges from `0.0` to `1.0` (or `0%` to `100%`).</type>
<type name="rgb(R G B) / rgb(R G B / A)">Modern CSS space-separated format.  Component values range from `0` to
`255`, or from `0%` to `100%`.  Alpha is optional, preceded by `/`, and ranges from `0.0` to `1.0` (or `0%` to
`100%`).</type>
<type name="hsl(H,S,L) / hsla(H,S,L,A)">Legacy comma-separated format.  Hue is expressed in degrees (`0`-`360`)
and may carry a `deg`, `grad`, `rad` or `turn` unit; values outside the range wrap.  Saturation and lightness are
percentages.  Alpha ranges from `0.0` to `1.0` (or `0%` to `100%`).</type>
<type name="hsl(H S L) / hsl(H S L / A)">Modern CSS space-separated format, with the same component ranges as the
legacy format.  Alpha is optional and preceded by `/`.</type>
<type name="hsv(H,S,V) / hsv(H S V / A)">Hue in degrees (`0`-`360`), saturation and value as percentages.  Both the
comma-separated and space-separated forms are accepted, with an optional alpha component ranging from `0.0` to
`1.0`.</type>
<type name="oklab(L a b [/ A])">CSS Color Level 4 OKLAB colour space.  Lightness (`L`) is `0`-`1` or `0%`-`100%`.
The `a` and `b` axes range from approximately `-0.4` to `0.4`, or as percentages where `100%` equals `0.4`.
Alpha (`A`) is optional, preceded by `/`, and ranges from `0.0` to `1.0` or `0%` to `100%`.</type>
<type name="oklch(L C H [/ A])">CSS Color Level 4 OKLCh colour space.  Lightness (`L`) is `0`-`1` or `0%`-`100%`.
Chroma (`C`) is an unbounded positive value, or a percentage where `100%` equals `0.4`.  Hue (`H`) is in degrees.
Alpha (`A`) is optional, preceded by `/`, and ranges from `0.0` to `1.0` or `0%` to `100%`.</type>
<type name="lab(L a b [/ A])">CSS Color Level 4 CIE Lab colour space.  Lightness (`L`) is `0`-`100` or `0%`-`100%`.
The `a` and `b` axes range from approximately `-125` to `125`, or as percentages where `100%` equals `125`.
Alpha (`A`) is optional, preceded by `/`, and ranges from `0.0` to `1.0` or `0%` to `100%`.</type>
<type name="lch(L C H [/ A])">CSS Color Level 4 CIE LCH colour space (cylindrical form of Lab).  Lightness (`L`) is
`0`-`100` or `0%`-`100%`.  Chroma (`C`) is an unbounded positive value, or a percentage where `100%` equals `150`.
Hue (`H`) is in degrees.  Alpha (`A`) is optional, preceded by `/`, and ranges from `0.0` to `1.0` or
`0%` to `100%`.</type>
<type name="color(space R G B [/ A])">CSS Color Level 4 `color()` function.  The supported spaces are `srgb`,
`srgb-linear`, `display-p3`, `a98-rgb` and `rec2020`.  Channels range from `0` to `1`, or `0%` to `100%`, and `none`
is read as zero.  Colours are converted to sRGB, so a colour outside the sRGB gamut has channels outside `0` to `1`.
Rec. 2020 is read with its SDR transfer function, as a container gamut rather than an HDR signal.  Alpha
(`A`) is optional, preceded by `/`, and ranges from `0.0` to `1.0` or `0%` to `100%`.  Any other space is a syntax
error.</type>
</types>

A Gradient, Image or Pattern can be referenced using the `url(#name)` format, where `name` is a definition
registered with the provided `Scene` object.  If `Scene` is `NULL` then it will not be possible to find the
reference.  Any failure to look up a reference will result in an error.  The `Scene` parameter accepts either a
@VectorScene or a @Vector object if its Scene field value is defined.

To access one of the pre-defined colourmaps, use the format `url(#cmap:name)`.  The colourmap will be accessible as
a linear gradient that belongs to the `Scene`.  Valid colourmap names are `cmap:crest`,
`cmap:flare`, `cmap:icefire`, `cmap:inferno`, `cmap:magma`, `cmap:mako`, `cmap:plasma`, `cmap:rocket`,
`cmap:viridis`.

A !VectorPainter structure must be provided by the client and will be used to store the final result.  All pointers
that are returned will remain valid as long as the provided Scene exists with its registered painter definitions.

Only the first painter in the IRI is parsed.  If the optional `Result` parameter is provided, it will refer to the
content that follows the painter, with leading whitespace removed.  This makes it possible for the client to process
painter lists and fallback colours:

<list type="bullet">
<li>A `,` or `;` prefix indicates that another painter follows, e.g. `url(#hatch), red` or `url(#hatch);red`.  The
remainder can be passed back to ReadPainter() as-is, as a leading separator is skipped.</li>
<li>Any other content is an SVG fallback colour, e.g. `red` in `url(#hatch) red`.  SVG only permits a fallback to
follow a `url()` reference.</li>
</list>

ReadPainter() does not apply fallback colours or interpret painter lists itself.  Refer to the @Vector.Fill field for
how they are interpreted when painting a vector.

Note: To ensure that colour values are never clipped, values stored in the Colour field are unclamped from the sRGB
colour space (colours may be negative or exceed 1.0).  Clients must clamp colours as necessary when converting to their
target colour space.

-INPUT-
obj(VectorScene) Scene: Optional.  Required if `url()` references are to be resolved.
strview IRI: The IRI string to be translated.
struct(*VectorPainter) Painter: This !VectorPainter structure will store the deserialised result.
&strview Result: Optional reference for storing the content that follows the parsed painter.  An empty string is returned if there is no further content to parse or an error occurred.

-ERRORS-
Okay:
NullArgs:
InvalidObject:
NotFound:
Syntax:

-TAGS-
mutates-input, object-owns-result

*********************************************************************************************************************/

ERR ReadPainter(objVectorScene *Scene, const std::string_view &IRI, VectorPainter *Painter, std::string_view *Result)
{
   kt::Log log(__FUNCTION__);

   if (Result) *Result = std::string_view{};
   if (IRI.empty() or (not Painter)) return ERR::NullArgs;

   std::string_view iri(IRI);

   Painter->reset();

   log.trace("IRI: %.*s", int(iri.size()), iri.data());

   if (iri.starts_with(';') or iri.starts_with(',')) iri.remove_prefix(1);
   skip_whitespace(iri);

   std::string_view result;
   ERR error;
   if (iri.starts_with('#')) error = parse_hex(iri, Painter, result);
   else {
      // The painter name terminates at an opening bracket, a painter separator or whitespace.

      std::size_t sep = 0;
      while ((sep < iri.size()) and (uint8_t(iri[sep]) > 0x20) and (iri[sep] != '(') and (iri[sep] != ';') and
         (iri[sep] != ',')) sep++;

      if ((sep < iri.size()) and (iri[sep] IS '(')) {
         auto body = iri.substr(sep + 1);
         switch (kt::strhash(iri.substr(0, sep))) {
            case HASH_URL:   error = parse_url(log, Scene, body, Painter, result); break;
            case HASH_RGB:
            case HASH_RGBA:  error = parse_rgb(body, Painter, result); break;
            case HASH_OKLAB: error = parse_oklab(body, Painter, result); break;
            case HASH_OKLCH: error = parse_oklch(body, Painter, result); break;
            case HASH_LAB:   error = parse_lab(body, Painter, result); break;
            case HASH_LCH:   error = parse_lch(body, Painter, result); break;
            case HASH_HSL:
            case HASH_HSLA:  error = parse_hsl(body, Painter, result); break;
            case HASH_HSV:   error = parse_hsv(body, Painter, result); break;
            case HASH_COLOR: error = parse_color(body, Painter, result); break;
            default:         error = ERR::Syntax;
         }
      }
      else error = parse_named_colour(log, iri, sep, Painter, result);
   }

   if (!error) {
      if (Result and (not result.empty())) *Result = result;
   }
   return error;
}
