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

static ERR GRADIENT_SET_Stops(extGradient *Self, GradientStop *Value, int Elements);
static ERR rebuild_gradient_colours(extGradient *Self);

static ERR init_gradient_linear(void);
static ERR init_gradient_radial(void);
static ERR init_gradient_conic(void);
static ERR init_gradient_diamond(void);
static ERR init_gradient_contour(void);
static ERR init_gradient_gouraud(void);
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
      if ((Painter.GradientTable) and (Opacity IS Painter.GradientAlpha)) return Painter.GradientTable;

      delete Painter.GradientTable;
      Painter.GradientTable = new (std::nothrow) GRADIENT_TABLE();
      if (not Painter.GradientTable) {
         log.warning("Failed to allocate fill gradient table");
         return nullptr;
      }
      Painter.GradientAlpha = Opacity;

      for (unsigned i=0; i < Painter.GradientTable->size(); i++) {
         (*Painter.GradientTable)[i] = agg::rgba8(cols->table[i].r, cols->table[i].g, cols->table[i].b,
            cols->table[i].a * Opacity);
      }

      return Painter.GradientTable;
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
      if ((Vector.Stroke.GradientTable) and (opacity IS Vector.Stroke.GradientAlpha)) return Vector.Stroke.GradientTable;

      delete Vector.Stroke.GradientTable;
      Vector.Stroke.GradientTable = new (std::nothrow) GRADIENT_TABLE();
      if (not Vector.Stroke.GradientTable) {
         log.warning("Failed to allocate stroke gradient table");
         return nullptr;
      }
      Vector.Stroke.GradientAlpha = opacity;

      for (unsigned i=0; i < Vector.Stroke.GradientTable->size(); i++) {
         (*Vector.Stroke.GradientTable)[i] = agg::rgba8(cols->table[i].r, cols->table[i].g, cols->table[i].b,
            cols->table[i].a * opacity);
      }

      return Vector.Stroke.GradientTable;
   }
}

//********************************************************************************************************************
// Constructor for the GradientColours class.  This expects to be called whenever the Gradient class updates the
// Stops array.

GradientColours::GradientColours(const std::vector<GradientStop> &Stops, VCS ColourSpace, double Alpha, double Resolution,
   double Gamma, GEZ Easing)
{
   resolution = 1.0;
   gamma = 1.0;

   int stop, i1, i2, i;

   for (stop=0; stop < std::ssize(Stops)-1; stop++) {
      i1 = int(255.0 * Stops[stop].Offset);
      if (i1 < 0) i1 = 0;
      else if (i1 > 255) i1 = 255;

      i2 = int(255.0 * Stops[stop+1].Offset);
      if (i2 < 0) i2 = 0;
      else if (i2 > 255) i2 = 255;

      agg::rgba8 begin(Stops[stop].RGB.Red * 255, Stops[stop].RGB.Green * 255,
         Stops[stop].RGB.Blue * 255, Stops[stop].RGB.Alpha * Alpha * 255);
      agg::rgba8 end(Stops[stop+1].RGB.Red * 255, Stops[stop+1].RGB.Green * 255,
         Stops[stop+1].RGB.Blue * 255, Stops[stop+1].RGB.Alpha * Alpha * 255);

      if ((stop IS 0) and (i1 > 0)) {
         for (i=0; i < i1; i++) table[i] = begin;
      }

      if (i1 < i2) {
         for (i=i1; i <= i2; i++) {
            double j = double(i - i1) / double(i2 - i1);
            j = GradientColours::ease(Easing, j);
            if (ColourSpace IS VCS::LINEAR_RGB) table[i] = begin.linear_gradient(end, j);
            else table[i] = begin.gradient(end, j);
         }
      }
      else if (i1 IS i2) table[i1] = end;

      if ((stop IS std::ssize(Stops)-2) and (i2 < 255)) {
         for (i=i2; i <= 255; i++) table[i] = end;
      }
   }

   apply_gamma(Gamma);
   if (Resolution < 1) apply_resolution(Resolution);
}

GradientColours::GradientColours(const std::array<FRGB, 256> &Map, double Resolution, double Gamma)
{
   resolution = 1.0;
   gamma = 1.0;

   for (int i=0; i < std::ssize(Map); i++) {
      table[i] = agg::rgba8(Map[i]);
   }

   apply_gamma(Gamma);
   if (Resolution < 1) apply_resolution(Resolution);
}

static ERR rebuild_gradient_colours(extGradient *Self)
{
   std::unique_ptr<GradientColours> colours;

   if (not Self->Stops.empty()) {
      colours.reset(new (std::nothrow) GradientColours(Self->Stops, Self->ColourSpace, 1.0, Self->Resolution,
         Self->Gamma, Self->Easing));
   }
   else if (not Self->ColourMap.empty()) {
      if (auto it = glColourMaps.find(Self->ColourMap); it != glColourMaps.end()) {
         colours.reset(new (std::nothrow) GradientColours(it->second, Self->Resolution, Self->Gamma));
      }
      else return ERR::NotFound;
   }
   else return ERR::Okay;

   if (not colours) return ERR::AllocMemory;

   if (Self->Colours) delete Self->Colours;
   Self->Colours = colours.release();
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

static ERR GRADIENT_GET_Colour(extGradient *Self, FRGB **Value)
{
   *Value = &Self->Colour;
   return ERR::Okay;
}

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

static ERR GRADIENT_GET_ColourMap(extGradient *Self, std::string_view &Value)
{
   if (not Self->ColourMap.empty()) Value = Self->ColourMap;
   else Value = std::string_view{};
   return ERR::Okay;
}

static ERR GRADIENT_SET_ColourMap(extGradient *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::NoData;

   if (auto it = glColourMaps.find(Value); it != glColourMaps.end()) {
      auto colours = std::unique_ptr<GradientColours>(new (std::nothrow) GradientColours(it->second, Self->Resolution,
         Self->Gamma));
      if (not colours) return ERR::AllocMemory;

      if (Self->Colours) delete Self->Colours;
      Self->Colours = colours.release();
      Self->Stops.clear();
      Self->ColourMap = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::NotFound;
}

/*********************************************************************************************************************

-FIELD-
ColourSpace: Defines the colour space to use when interpolating gradient colours.
Lookup: VCS

By default, gradients are rendered using the standard RGB colour space and alpha blending rules.  Changing the colour
space to `LINEAR_RGB` will force the renderer to automatically convert sRGB values to linear RGB when blending.

-FIELD-
Easing: Selects the easing function for interpolation between gradient stops.
Lookup: GEZ

Easing modifies the interpolation position between each adjacent pair of #Stops.  The default `GEZ::LINEAR` mode leaves
the gradient unchanged.  This field has no effect on gradients sourced from #ColourMap because colourmaps already supply
a complete colour table rather than editable stop segments.

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
Gamma: Applies a gamma curve to the gradient ramp.

Gamma remaps the completed colour ramp before #Resolution is applied.  Values greater than `1.0` bias the ramp toward
the first colour, while values between `0.0` and `1.0` bias it toward the last colour.  A value of `1.0` leaves the
gradient unchanged.

-FIELD-
ID: String identifier for a vector.

The ID field is provided for the purpose of SVG support.  Where possible, we recommend that you use the existing
object name and automatically assigned ID's for identifiers.

*********************************************************************************************************************/

static ERR GRADIENT_GET_ID(extGradient *Self, std::string_view &Value)
{
   Value = Self->ID;
   return ERR::Okay;
}

static ERR GRADIENT_SET_ID(extGradient *Self, const std::string_view &Value)
{
   if (not Value.empty()) {
      Self->ID = Value;
      Self->NumericID = strhash(Value);
   }
   else {
      Self->ID.clear();
      Self->NumericID = 0;
   }
   return ERR::Okay;
}

static ERR GRADIENT_SET_Gamma(extGradient *Self, double Value)
{
   if (Value <= 0) return ERR::OutOfRange;

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
Matrices: Applies one or more transforms to a gradient.

A transform can be applied to a gradient via one or more matrices.  These will influence how gradient fills are
rendered within their vector space.  Each matrix is represented by a !VectorMatrix structure, and the matrices are
linked in the order in which they should be applied.

*********************************************************************************************************************/

static ERR GRADIENT_GET_Matrices(extGradient *Self, VectorMatrix **Value)
{
   *Value = Self->Matrices;
   return ERR::Okay;
}

static ERR GRADIENT_SET_Matrices(extGradient *Self, VectorMatrix *Value)
{
   if (Value) {
      auto hook = &Self->Matrices;
      while (Value) {
         VectorMatrix *matrix;
         if (!AllocMemory(sizeof(VectorMatrix), MEM::DATA|MEM::NO_CLEAR, (APTR *)&matrix)) {
            matrix->Vector = nullptr;
            matrix->Next   = nullptr;
            matrix->ScaleX = Value->ScaleX;
            matrix->ScaleY = Value->ScaleY;
            matrix->ShearX = Value->ShearX;
            matrix->ShearY = Value->ShearY;
            matrix->TranslateX = Value->TranslateX;
            matrix->TranslateY = Value->TranslateY;
            *hook = matrix;
            hook = &matrix->Next;
         }
         else return ERR::AllocMemory;

         Value = Value->Next;
      }
   }
   else {
      VectorMatrix *next;
      for (auto node=Self->Matrices; node; node=next) {
         next = node->Next;
         FreeResource(node);
      }
      Self->Matrices = nullptr;
   }

   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NumericID: Numeric identifier for a vector.

The NumericID field is provided for internal use by the SVG and vector modules.  If NumericID is set by the client,
then any value in #ID will be immediately cleared.

*********************************************************************************************************************/

static ERR GRADIENT_GET_NumericID(extGradient *Self, int *Value)
{
   *Value = Self->NumericID;
   return ERR::Okay;
}

static ERR GRADIENT_SET_NumericID(extGradient *Self, int Value)
{
   Self->NumericID = Value;
   Self->ID.clear();
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
   if ((Value < 0) or (Value > 1.0)) return ERR::OutOfRange;

   const bool changed = (Self->Resolution != Value);
   Self->Resolution = Value;

   if ((Self->Colours) and (Self->Colours->resolution != Value)) {
      if (Self->initialised()) {
         Self->modified();
         if (auto error = rebuild_gradient_colours(Self); error != ERR::Okay) return error;
      }
      else Self->Colours->apply_resolution(Value);
   }
   else if (changed and Self->initialised()) Self->modified();

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

static ERR GRADIENT_GET_Stops(extGradient *Self, std::span<GradientStop> *Value)
{
   *Value = std::span<GradientStop>(Self->Stops);
   return ERR::Okay;
}

static ERR GRADIENT_SET_Stops(extGradient *Self, std::span<const GradientStop> *Value)
{
   if ((Value) and (Value->size() >= 2)) {
      auto stops = std::vector<GradientStop>(Value->begin(), Value->end());
      auto colours = std::unique_ptr<GradientColours>(new (std::nothrow) GradientColours(
         stops, Self->ColourSpace, 1.0, Self->Resolution, Self->Gamma, Self->Easing));
      if (not colours) return ERR::AllocMemory;

      Self->Stops = std::move(stops);
      if (Self->Colours) delete Self->Colours;
      Self->Colours = colours.release();
      Self->ColourMap.clear();
      Self->modified();
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
Transform: Applies a transform to the gradient.

A transform can be applied to the gradient by setting this field with an SVG compliant transform string.

*********************************************************************************************************************/

static ERR GRADIENT_SET_Transform(extGradient *Self, const std::string_view &Commands)
{
   if (Commands.empty()) return kt::Log().warning(ERR::InvalidValue);

   if (Self->initialised()) Self->modified();

   if (not Self->Matrices) {
      VectorMatrix *matrix;
      if (!AllocMemory(sizeof(VectorMatrix), MEM::DATA|MEM::NO_CLEAR, (APTR *)&matrix)) {
         matrix->Vector = nullptr;
         matrix->Next   = Self->Matrices;
         matrix->ScaleX = 1.0;
         matrix->ScaleY = 1.0;
         matrix->ShearX = 0;
         matrix->ShearY = 0;
         matrix->TranslateX = 0;
         matrix->TranslateY = 0;

         Self->Matrices = matrix;
         return vec::ParseTransform(Self->Matrices, Commands);
      }
      else return ERR::AllocMemory;
   }
   else {
      vec::ResetMatrix(Self->Matrices);
      return vec::ParseTransform(Self->Matrices, Commands);
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

   VectorMatrix *next;
   for (auto node=Matrices; node; node=next) {
      next = node->Next;
      FreeResource(node);
   }
}

//********************************************************************************************************************

#include "gradient_def.c"

static const FieldArray clGradientFields[] = {
   { "Resolution",   FDF_DOUBLE|FDF_RW, nullptr, GRADIENT_SET_Resolution },
   { "Gamma",        FDF_DOUBLE|FDF_RW, nullptr, GRADIENT_SET_Gamma },
   { "SpreadMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENT_SET_SpreadMethod, &clGradientSpreadMethod },
   { "Units",        FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clGradientUnits },
   { "ColourSpace",  FDF_INT|FDF_RI, nullptr, nullptr, &clGradientColourSpace },
   { "Easing",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENT_SET_Easing, &clGradientEasing },
   // Virtual fields
   { "Colour",       FDF_VIRTUAL|FDF_STRUCT|FD_RW|FDF_PURE, GRADIENT_GET_Colour, GRADIENT_SET_Colour, "FRGB" },
   { "ColourMap",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, GRADIENT_GET_ColourMap, GRADIENT_SET_ColourMap },
   { "Matrices",     FDF_VIRTUAL|FDF_POINTER|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENT_GET_Matrices, GRADIENT_SET_Matrices, "VectorMatrix" },
   { "NumericID",    FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, GRADIENT_GET_NumericID, GRADIENT_SET_NumericID },
   { "ID",           FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW|FDF_PURE, GRADIENT_GET_ID, GRADIENT_SET_ID },
   { "Stops",        FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENT_GET_Stops, GRADIENT_SET_Stops, "GradientStop" },
   { "Transform",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_W, nullptr, GRADIENT_SET_Transform },
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
   if ((error = init_gradient_distal()) != ERR::Okay) return error;
   if ((error = init_gradient_voronoi()) != ERR::Okay) return error;

   return error;
}
