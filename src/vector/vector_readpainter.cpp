
static double linear_to_srgb(double V)
{
   if (V <= 0.0031308) return 12.92 * V;
   return 1.055 * pow(V, 1.0 / 2.4) - 0.055;
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

// Advance the IRI past the current value and set the Result pointer.

static void advance_result(CSTRING IRI, CSTRING *Result)
{
   if (Result) {
      while ((*IRI) and (*IRI != ';')) IRI++;
      *Result = IRI[0] ? IRI : nullptr;
   }
}

// Parse a CSS colour component: reads a double, applies percentage scaling, and skips trailing whitespace.

static double parse_css_value(CSTRING &IRI, double PercentScale = 0.01)
{
   double val = strtod(IRI, (STRING *)&IRI);
   if (*IRI IS '%') { val *= PercentScale; IRI++; }
   while ((*IRI) and (*IRI <= 0x20)) IRI++;
   return val;
}

// Parse optional CSS alpha after '/' separator.  Returns the parsed alpha value, or 1.0 if absent.

static float parse_css_alpha(CSTRING &IRI)
{
   while ((*IRI) and (*IRI <= 0x20)) IRI++;
   if (*IRI IS '/') {
      IRI++;
      float alpha = (float)strtod(IRI, (STRING *)&IRI);
      if (*IRI IS '%') { alpha *= 0.01f; IRI++; }
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

static ERR parse_url(pf::Log &Log, objVectorScene *Scene, CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   if (not Scene) return Log.warning(ERR::NullArgs);

   if (Scene->Class->BaseClassID IS CLASSID::VECTOR) Scene = ((objVector *)Scene)->Scene;
   else if (Scene->classID() != CLASSID::VECTORSCENE) return Log.warning(ERR::InvalidObject);

   if (Scene->HostScene) Scene = Scene->HostScene;

   if (IRI[4] != '#') {
      Log.warning("Invalid IRI: %s", IRI);
      return ERR::Syntax;
   }

   // Compute the hash identifier
   uint32_t i;
   for (i = 5; (IRI[i] != ')') and IRI[i]; i++);
   std::string lookup;
   lookup.assign(IRI, 5, i - 5);

   bool found = false;
   if (((extVectorScene *)Scene)->Defs.contains(lookup)) {
      auto def = ((extVectorScene *)Scene)->Defs[lookup];
      if (def->classID() IS CLASSID::VECTORGRADIENT) Painter->Gradient = (objVectorGradient *)def;
      else if (def->classID() IS CLASSID::VECTORIMAGE) Painter->Image = (objVectorImage *)def;
      else if (def->classID() IS CLASSID::VECTORPATTERN) Painter->Pattern = (objVectorPattern *)def;
      else Log.warning("Vector definition '%s' (class $%.8x) not supported.", lookup.c_str(), uint32_t(def->classID()));
      found = true;
   }
   else if (glColourMaps.contains(lookup)) {
      // Referencing a pre-defined colour map results in it being added to the Scene's definitions as a linear gradient.
      // It is then accessible permanently under that name.

      extVectorGradient *gradient;
      if (NewObject(CLASSID::VECTORGRADIENT, &gradient) IS ERR::Okay) {
         SetOwner(gradient, Scene);
         gradient->setFields(
            fl::Name(lookup),
            fl::Type(VGT::LINEAR),
            fl::Units(VUNIT::BOUNDING_BOX),
            fl::X1(0.0),
            fl::Y1(0.0),
            fl::X2(SCALE(1.0)),
            fl::Y2(0.0));

         if (gradient->Colours) delete gradient->Colours;
         gradient->Colours = new (std::nothrow) GradientColours(glColourMaps[lookup], 0);

         if (InitObject(gradient) IS ERR::Okay) {
            Scene->addDef(lookup.c_str(), gradient);
            Painter->Gradient = gradient;
         }
      }
      found = true;
   }

   if (found) {
      IRI += i;
      if (*IRI IS ')') {
         IRI++;
         while ((*IRI) and (*IRI <= 0x20)) IRI++; // Skip whitespace
      }

      if (Result) *Result = IRI[0] ? IRI : nullptr;
      return ERR::Okay;
   }

   Log.warning("Failed to lookup IRI '%s' in scene #%d", IRI, Scene->UID);
   return ERR::NotFound;
}

//********************************************************************************************************************

static ERR parse_rgb(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   auto &rgb = Painter->Colour;
   // Supports both legacy comma-separated format: rgb(R,G,B) / rgba(R,G,B,A)
   // and modern CSS space-separated format: rgb(R G B) / rgb(R G B / A)
   // Component values are permitted to exceed the standard ranges of 0-255 or 0%-100% if representing colours
   // outside the sRGB gamut.

   while (*IRI != '(') IRI++;
   IRI++;
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   rgb.Red = strtod(IRI, (STRING *)&IRI) * (1.0 / 255.0);
   if (*IRI IS '%') { rgb.Red *= (255.0 / 100.0); IRI++; }
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   bool legacy = (*IRI IS ',');
   if (legacy) IRI++;

   rgb.Green = strtod(IRI, (STRING *)&IRI) * (1.0 / 255.0);
   if (*IRI IS '%') { rgb.Green *= (255.0 / 100.0); IRI++; }
   while ((*IRI) and (*IRI <= 0x20)) IRI++;
   if (legacy and (*IRI IS ',')) IRI++;

   rgb.Blue = strtod(IRI, (STRING *)&IRI) * (1.0 / 255.0);
   if (*IRI IS '%') { rgb.Blue *= (255.0 / 100.0); IRI++; }

   if (legacy) {
      if (*IRI IS ',') {
         IRI++;
         rgb.Alpha = strtod(IRI, (STRING *)&IRI);
         if (*IRI IS '%') { rgb.Alpha *= 0.01f; IRI++; }
         rgb.Alpha = std::clamp(rgb.Alpha, 0.0f, 1.0f);
      }
      else rgb.Alpha = 1.0;
   }
   else rgb.Alpha = parse_css_alpha(IRI);

   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_oklab(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   // CSS oklab() colour function: oklab(L a b [/ alpha])
   // L: lightness 0-1 (or 0%-100%), a: ~-0.4 to 0.4 (or percentage), b: ~-0.4 to 0.4 (or percentage)
   // Values are space-separated; alpha is optional, preceded by '/'

   IRI += 6;
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   double l    = parse_css_value(IRI, 0.01);
   double ok_a = parse_css_value(IRI, 0.004); // 100% = 0.4
   double ok_b = parse_css_value(IRI, 0.004); // 100% = 0.4
   float alpha  = parse_css_alpha(IRI);

   oklab_to_painter(l, ok_a, ok_b, alpha, Painter);
   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_oklch(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   // CSS oklch() colour function: oklch(L C H [/ alpha])
   // L: lightness 0-1 (or 0%-100%), C: chroma ~0-0.4 (or percentage), H: hue in degrees
   // Values are space-separated; alpha is optional, preceded by '/'

   IRI += 6;
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   double l     = parse_css_value(IRI, 0.01);
   double c     = parse_css_value(IRI, 0.004); // 100% = 0.4
   double h_deg = strtod(IRI, (STRING *)&IRI);
   float alpha   = parse_css_alpha(IRI);

   c = std::max(c, 0.0);

   // OKLCh to OKLAB (polar to cartesian)
   const double h_rad = h_deg * (agg::pi / 180.0);
   const double ok_a = c * cos(h_rad);
   const double ok_b = c * sin(h_rad);

   oklab_to_painter(l, ok_a, ok_b, alpha, Painter);
   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// CSS lab() colour function: lab(L a b [/ alpha])
// L: lightness 0-100 (or 0%-100%), a: ~-125 to 125 (or percentage), b: ~-125 to 125 (or percentage)
// Values are space-separated; alpha is optional, preceded by '/'

static ERR parse_lab(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   IRI += 4;
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   double l    = parse_css_value(IRI, 1.0);  // Percentage maps to 0-100
   double a    = parse_css_value(IRI, 1.25); // 100% = 125
   double b    = parse_css_value(IRI, 1.25); // 100% = 125
   float alpha  = parse_css_alpha(IRI);

   cielab_to_painter(l, a, b, alpha, Painter);
   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// CSS lch() colour function: lch(L C H [/ alpha])
// L: lightness 0-100 (or 0%-100%), C: chroma ~0-150 (or percentage), H: hue in degrees
// Values are space-separated; alpha is optional, preceded by '/'

static ERR parse_lch(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   IRI += 4;
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   double l     = parse_css_value(IRI, 1.0); // Percentage maps to 0-100
   double c     = parse_css_value(IRI, 1.5); // 100% = 150
   double h_deg = strtod(IRI, (STRING *)&IRI);
   float alpha   = parse_css_alpha(IRI);

   c = std::max(c, 0.0);

   // LCH to Lab (polar to cartesian)
   const double h_rad = h_deg * (agg::pi / 180.0);
   const double a = c * cos(h_rad);
   const double b = c * sin(h_rad);

   cielab_to_painter(l, a, b, alpha, Painter);
   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse the common (H, S, X [, A]) format used by HSL and HSV.
// Returns hue (0-1), saturation (0-1), third component (0-1), and alpha (0-1).

static void parse_hsx(CSTRING &IRI, double &Hue, double &Sat, double &Third, float &Alpha)
{
   while (*IRI != '(') IRI++;
   IRI++;
   Hue   = std::clamp(strtod(IRI, nullptr) * (1.0 / 360.0), 0.0, 1.0);
   while ((*IRI) and (*IRI != ',')) IRI++;
   if (*IRI) IRI++;
   Sat   = std::clamp(strtod(IRI, nullptr) * 0.01, 0.0, 1.0);
   while ((*IRI) and (*IRI != ',')) IRI++;
   if (*IRI) IRI++;
   Third = std::clamp(strtod(IRI, nullptr) * 0.01, 0.0, 1.0);
   while ((*IRI) and (*IRI != ',')) IRI++;

   if (*IRI) {
      IRI++;
      Alpha = std::clamp((float)strtod(IRI, nullptr), 0.0f, 1.0f);
      while (*IRI) IRI++;
   }
   else Alpha = 1.0f;
}

//********************************************************************************************************************

static ERR parse_hsl(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   auto &rgb = Painter->Colour;
   double hue, sat, light;
   parse_hsx(IRI, hue, sat, light, rgb.Alpha);

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

   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_hsv(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   auto &rgb = Painter->Colour;
   double hue, sat, val;
   parse_hsx(IRI, hue, sat, val, rgb.Alpha);

   hue = hue / 60.0;
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

   advance_result(IRI, Result);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_hex(CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   auto &rgb = Painter->Colour;
   IRI++;
   char nibbles[8];
   uint8_t n = 0;
   while ((*IRI) and (n < std::ssize(nibbles))) nibbles[n++] = read_nibble(IRI++);
   while ((*IRI) and (*IRI != ';')) IRI++;

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

   if (Result) *Result = IRI[0] ? IRI : nullptr;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_named_colour(pf::Log &Log, CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   auto hash = strihash(IRI);

   const ankerl::unordered_dense::map<uint32_t, RGB8> *table = nullptr;

   if (auto it = glNamedColours.find(hash); it != glNamedColours.end()) {
      table = &glNamedColours;
   }
   else if (auto it = glAppColours.find(hash); it != glAppColours.end()) {
      table = &glAppColours;
   }

   if (table) {
      auto &src = table->at(hash);
      auto &rgb = Painter->Colour;
      rgb.Red   = (float)src.Red   * (1.0 / 255.0);
      rgb.Green = (float)src.Green * (1.0 / 255.0);
      rgb.Blue  = (float)src.Blue  * (1.0 / 255.0);
      rgb.Alpha = (float)src.Alpha * (1.0 / 255.0);
      advance_result(IRI, Result);
      return ERR::Okay;
   }

   // Note: Resolving 'currentColour' is handled in the SVG parser and not the Vector API.
   Log.warning("Failed to interpret colour \"%s\"", IRI);
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
<type name="hsl(H,S,L) / hsla(H,S,L,A)">Hue is expressed in degrees (`0`-`360`).  Saturation and lightness are
percentages.  Alpha ranges from `0.0` to `1.0`.</type>
<type name="hsv(H,S,V)">Hue in degrees (`0`-`360`), saturation and value as percentages.  An optional alpha
component ranging from `0.0` to `1.0` is supported.</type>
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
that are returned will remain valid as long as the provided Scene exists with its registered painter definitions.  An
optional `Result` string can store a reference to the character position up to which the IRI was parsed.

Note: To ensure that colour values are never clipped, values stored in the Colour field are unclamped from the sRGB
colour space (colours may be negative or exceed 1.0).  Clients must clamp colours as necessary when converting to their
target colour space.

-INPUT-
obj(VectorScene) Scene: Optional.  Required if `url()` references are to be resolved.
cstr IRI: The IRI string to be translated.
struct(*VectorPainter) Painter: This !VectorPainter structure will store the deserialised result.
&cstr Result: Optional pointer for storing the end of the parsed IRI string.  `NULL` is returned if there is no further content to parse or an error occurred.

-ERRORS-
Okay:
NullArgs:
Failed:

*********************************************************************************************************************/

ERR ReadPainter(objVectorScene *Scene, CSTRING IRI, VectorPainter *Painter, CSTRING *Result)
{
   pf::Log log(__FUNCTION__);

   if (Result) *Result = nullptr;
   if ((not IRI) or (not Painter)) return ERR::NullArgs;

   Painter->reset();

   log.trace("IRI: %s", IRI);

   if (*IRI IS ';') IRI++;
   while ((*IRI) and (*IRI <= 0x20)) IRI++;

   // TODO: Use a hash comparison

   if (startswith("url(", IRI))           return parse_url(log, Scene, IRI, Painter, Result);
   else if ((startswith("rgb(", IRI)) or
            (startswith("rgba(", IRI)))   return parse_rgb(IRI, Painter, Result);
   else if (startswith("oklab(", IRI))    return parse_oklab(IRI, Painter, Result);
   else if (startswith("oklch(", IRI))    return parse_oklch(IRI, Painter, Result);
   else if (startswith("lab(", IRI))      return parse_lab(IRI, Painter, Result);
   else if (startswith("lch(", IRI))      return parse_lch(IRI, Painter, Result);
   else if ((startswith("hsl(", IRI)) or
            (startswith("hsla(", IRI)))   return parse_hsl(IRI, Painter, Result);
   else if (startswith("hsv(", IRI))      return parse_hsv(IRI, Painter, Result);
   else if (*IRI IS '#')                  return parse_hex(IRI, Painter, Result);
   else                                   return parse_named_colour(log, IRI, Painter, Result);
}
