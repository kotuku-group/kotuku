/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of gradient definitions only.

-CLASS-
VectorGradient: Provides support for the filling and stroking of vectors with colour gradients.

The VectorGradient class is used by Vector painting algorithms to fill and stroke vectors with gradients.  This is
achieved by initialising a VectorGradient object with the desired settings and then registering it with
a @VectorScene via the @VectorScene.AddDef() method.

Any vector within the target scene will be able to utilise the gradient for filling or stroking by referencing its
name through the @Vector.Fill and @Vector.Stroke fields.  For instance 'url(#redgradient)'.

It is strongly recommended that the VectorGradient is owned by the @VectorScene that is handling the
definition.  This will ensure that the VectorGradient is de-allocated when the scene is destroyed.

-END-

*********************************************************************************************************************/

static ERR VECTORGRADIENT_SET_Stops(extVectorGradient *Self, GradientStop *Value, int Elements);

// Return a gradient table for a vector with its opacity multiplier applied.  The table is cached with the vector so
// that it does not need to be recalculated when required again.

GRADIENT_TABLE * get_fill_gradient_table(extPainter &Painter, double Opacity)
{
   pf::Log log(__FUNCTION__);

   GradientColours *cols = ((extVectorGradient *)Painter.Gradient)->Colours;
   if (!cols) {
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
      if (!Painter.GradientTable) {
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
   pf::Log log(__FUNCTION__);

   GradientColours *cols = ((extVectorGradient *)Vector.Stroke.Gradient)->Colours;
   if (!cols) {
      if (!cols) {
         log.warning("No colour table referenced in stroke gradient %p for vector #%d.", Vector.Stroke.Gradient, Vector.UID);
         return nullptr;
      }
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
      if (!Vector.Stroke.GradientTable) {
         log.warning("Failed to allocate stroke gradient table");
         return nullptr;
      }
      Vector.Stroke.GradientAlpha = opacity;

      for (unsigned i=0; i < Vector.Stroke.GradientTable->size(); i++) {
         (*Vector.Stroke.GradientTable)[i] = agg::rgba8(cols->table[i].r, cols->table[i].g, cols->table[i].b, cols->table[i].a * opacity);
      }

      return Vector.Stroke.GradientTable;
   }
}

//********************************************************************************************************************
// Constructor for the GradientColours class.  This expects to be called whenever the Gradient class updates the
// Stops array.

GradientColours::GradientColours(const std::vector<GradientStop> &Stops, VCS ColourSpace, double Alpha, double Resolution)
{
   int stop, i1, i2, i;

   for (stop=0; stop < std::ssize(Stops)-1; stop++) {
      i1 = int(255.0 * Stops[stop].Offset);
      if (i1 < 0) i1 = 0;
      else if (i1 > 255) i1 = 255;

      i2 = int(255.0 * Stops[stop+1].Offset);
      if (i2 < 0) i2 = 0;
      else if (i2 > 255) i2 = 255;

      agg::rgba8 begin(Stops[stop].RGB.Red*255, Stops[stop].RGB.Green*255, Stops[stop].RGB.Blue*255, Stops[stop].RGB.Alpha * Alpha * 255);
      agg::rgba8 end(Stops[stop+1].RGB.Red*255, Stops[stop+1].RGB.Green*255, Stops[stop+1].RGB.Blue*255, Stops[stop+1].RGB.Alpha * Alpha * 255);

      if ((stop IS 0) and (i1 > 0)) {
         for (i=0; i < i1; i++) table[i] = begin;
      }

      if (i1 <= i2) {
         for (i=i1; i <= i2; i++) {
            double j = (double)(i - i1) / (double)(i2-i1);
            if (ColourSpace IS VCS::LINEAR_RGB) {
               table[i] = begin.linear_gradient(end, j);
            }
            else table[i] = begin.gradient(end, j);
         }
      }

      if ((stop IS std::ssize(Stops)-2) and (i2 < 255)) {
         for (i=i2; i <= 255; i++) table[i] = end;
      }
   }

   if (Resolution < 1) apply_resolution(Resolution);
}

GradientColours::GradientColours(const std::array<FRGB, 256> &Map, double Resolution)
{
   for (int i=0; i < std::ssize(Map); i++) {
      table[i] = agg::rgba8(Map[i]);
   }

   if (Resolution < 1) apply_resolution(Resolution);
}

//********************************************************************************************************************

static ERR VECTORGRADIENT_Free(extVectorGradient *Self)
{
   if (Self->ID) { FreeResource(Self->ID); Self->ID = nullptr; }
   if (Self->Colours) { delete Self->Colours; Self->Colours = nullptr; }
   Self->Stops.~vector<GradientStop>();
   Self->ColourMap.~basic_string();

   VectorMatrix *next;
   for (auto node=Self->Matrices; node; node=next) {
      next = node->Next;
      FreeResource(node);
   }
   Self->Matrices = nullptr;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORGRADIENT_Init(extVectorGradient *Self)
{
   pf::Log log;

   if ((int(Self->SpreadMethod) <= 0) or (int(Self->SpreadMethod) >= int(VSPREAD::END))) {
      log.traceWarning("Invalid SpreadMethod value of %d", Self->SpreadMethod);
      return ERR::OutOfRange;
   }

   if ((int(Self->Units) <= 0) or (int(Self->Units) >= int(VUNIT::END))) {
      log.traceWarning("Invalid Units value of %d", Self->Units);
      return ERR::OutOfRange;
   }

   if ((Self->Type IS VGT::CONTOUR) and (Self->Units IS VUNIT::USERSPACE)) {
      log.warning("Contour gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORGRADIENT_NewObject(extVectorGradient *Self)
{
   new (&Self->Stops) std::vector<GradientStop>;
   new (&Self->ColourMap) std::string;
   Self->SpreadMethod = VSPREAD::PAD;
   Self->Type    = VGT::LINEAR;
   Self->Units   = VUNIT::BOUNDING_BOX;
   // SVG requires that these are all set to 50%
   Self->CenterX = 0.5;
   Self->CenterY = 0.5;
   Self->Radius  = 0.5;
   Self->X1      = 0;
   Self->X2      = 1.0; // Set for contoured gradients.
   Self->Flags  |= VGF::SCALED_CX|VGF::SCALED_CY|VGF::SCALED_RADIUS;
   Self->Resolution = 1;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CenterX: The horizontal center point of the gradient.

The `(CenterX, CenterY)` coordinates define the center point of the gradient.  The center point will only be used if
the gradient type requires it (such as the radial type).  By default, the center point is set to `50%`.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_CenterX(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->CenterX);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_CenterX(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CX) & (~VGF::FIXED_CX);
   else Self->Flags = (Self->Flags | VGF::FIXED_CX) & (~VGF::SCALED_CX);
   Self->CenterX = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CenterY: The vertical center point of the gradient.

The `(CenterX, CenterY)` coordinates define the center point of the gradient.  The center point will only be used if
the gradient type requires it (such as the radial type).  By default, the center point is set to `50%`.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_CenterY(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->CenterY);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_CenterY(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CY) & (~VGF::FIXED_CY);
   else Self->Flags = (Self->Flags | VGF::FIXED_CY) & (~VGF::SCALED_CY);

   Self->CenterY = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Colour: The default background colour to use when clipping is enabled.

The colour value in this field is applicable only when a gradient is in clip-mode - by specifying the `VSPREAD::CLIP`
flag in #SpreadMethod.  By default, this field has an alpha value of 0 to ensure that nothing is drawn
outside the initial bounds of the gradient.  Setting any other colour value here will otherwise
fill-in those areas.

The Colour value is defined in floating-point RGBA format, using a range of 0 - 1.0 per component.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_Colour(extVectorGradient *Self, float **Value, int *Elements)
{
   *Value = (float *)&Self->Colour;
   *Elements = 4;
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_Colour(extVectorGradient *Self, float *Value, int Elements)
{
   pf::Log log;
   if (Value) {
      if (Elements >= 3) {
         Self->Colour.Red   = Value[0];
         Self->Colour.Green = Value[1];
         Self->Colour.Blue  = Value[2];
         Self->Colour.Alpha = (Elements >= 4) ? Value[3] : 1.0;

         Self->ColourRGB.Red   = int(Self->Colour.Red * 255.0);
         Self->ColourRGB.Green = int(Self->Colour.Green * 255.0);
         Self->ColourRGB.Blue  = int(Self->Colour.Blue * 255.0);
         Self->ColourRGB.Alpha = int(Self->Colour.Alpha * 255.0);
      }
      else return log.warning(ERR::InvalidValue);
   }
   else Self->Colour.Alpha = 0;

   Self->modified();
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

static ERR VECTORGRADIENT_GET_ColourMap(extVectorGradient *Self, CSTRING *Value)
{
   if (!Self->ColourMap.empty()) *Value = Self->ColourMap.c_str();
   else *Value = nullptr;
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_ColourMap(extVectorGradient *Self, CSTRING Value)
{
   if (!Value) return ERR::NoData;

   if (glColourMaps.contains(Value)) {
      if (Self->Colours) delete Self->Colours;
      Self->Colours = new (std::nothrow) GradientColours(glColourMaps[Value], Self->Resolution);
      if (!Self->Colours) return ERR::AllocMemory;
      Self->ColourMap = Value;
      Self->modified();
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
Flags: Dimension flags are stored here.
Lookup: VGF

Dimension flags that indicate whether field values are fixed or scaled are defined here.

-FIELD-
FocalRadius: The size of the focal radius for radial gradients.

If a radial gradient has a defined focal point (by setting #FocalX and #FocalY) then the FocalRadius can be used to
adjust the size of the focal area.  The default of zero ensures that the focal area matches that defined by #Radius,
which is the standard maintained by SVG.

The FocalRadius value has no effect if the gradient is linear.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_FocalRadius(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->FocalRadius);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_FocalRadius(extVectorGradient *Self, Unit &Value)
{
   if (Value >= 0) {
      if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FOCAL_RADIUS) & (~VGF::FIXED_FOCAL_RADIUS);
      else Self->Flags = (Self->Flags | VGF::FIXED_FOCAL_RADIUS) & (~VGF::SCALED_FOCAL_RADIUS);

      Self->FocalRadius = Value;
      Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
FocalX: The horizontal focal point for radial gradients.

The `(FocalX, FocalY)` coordinates define the focal point for radial gradients.  If left undefined, the focal point
will match the center of the gradient.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_FocalX(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->FocalX);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_FocalX(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FX) & (~VGF::FIXED_FX);
   else Self->Flags = (Self->Flags | VGF::FIXED_FX) & (~VGF::SCALED_FX);

   Self->FocalX = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FocalY: The vertical focal point for radial gradients.

The `(FocalX, FocalY)` coordinates define the focal point for radial gradients.  If left undefined, the focal point
will match the center of the gradient.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_FocalY(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->FocalY);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_FocalY(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FY) & (~VGF::FIXED_FY);
   else Self->Flags = (Self->Flags | VGF::FIXED_FY) & (~VGF::SCALED_FY);

   Self->FocalY = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
ID: String identifier for a vector.

The ID field is provided for the purpose of SVG support.  Where possible, we recommend that you use the
existing object name and automatically assigned ID's for identifiers.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_ID(extVectorGradient *Self, STRING *Value)
{
   *Value = Self->ID;
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_ID(extVectorGradient *Self, CSTRING Value)
{
   if (Self->ID) FreeResource(Self->ID);

   if (Value) {
      Self->ID = strclone(Value);
      Self->NumericID = strhash(Value);
   }
   else {
      Self->ID = nullptr;
      Self->NumericID = 0;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Matrices: A linked list of transform matrices that have been applied to the gradient.

All transforms that have been applied to the gradient can be read from the Matrices field.  Each transform is
represented by a !VectorMatrix structure, and are linked in the order in which they were applied to the gradient.

!VectorMatrix

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_Matrices(extVectorGradient *Self, VectorMatrix **Value)
{
   *Value = Self->Matrices;
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_Matrices(extVectorGradient *Self, VectorMatrix *Value)
{
   if (!Value) {
      auto hook = &Self->Matrices;
      while (Value) {
         VectorMatrix *matrix;
         if (AllocMemory(sizeof(VectorMatrix), MEM::DATA|MEM::NO_CLEAR, &matrix) IS ERR::Okay) {
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

   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NumericID: A unique identifier for the vector.

This field assigns a numeric ID to a vector.  Alternatively it can also reflect a case-sensitive hash of the
#ID field if that has been defined previously.

If NumericID is set by the client, then any value in #ID will be immediately cleared.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_NumericID(extVectorGradient *Self, int *Value)
{
   *Value = Self->NumericID;
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_NumericID(extVectorGradient *Self, int Value)
{
   Self->NumericID = Value;
   if (Self->ID) { FreeResource(Self->ID); Self->ID = nullptr; }
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the gradient.

The radius of the gradient can be defined as a fixed unit or scaled relative to its container.  A default radius of
50% (0.5) applies if this field is not set.

The Radius value has no effect if the gradient is linear.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_Radius(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->Radius);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_Radius(extVectorGradient *Self, Unit &Value)
{
   if (Value >= 0) {
      if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_RADIUS) & (~VGF::FIXED_RADIUS);
      else Self->Flags = (Self->Flags | VGF::FIXED_RADIUS) & (~VGF::SCALED_RADIUS);

      Self->Radius = Value;
      Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
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

static ERR VECTORGRADIENT_SET_Resolution(extVectorGradient *Self, double Value)
{
   if ((Value < 0) or (Value > 1.0)) return ERR::OutOfRange;

   Self->Resolution = Value;

   if ((Self->Colours) and (Self->Colours->resolution != Value)) {
      if (Self->initialised()) {
         Self->modified();
         if (!Self->Stops.empty()) {
            auto copy = Self->Stops;
            VECTORGRADIENT_SET_Stops(Self, copy.data(), copy.size());
         }
         else if (!Self->ColourMap.empty()) {
            VECTORGRADIENT_SET_ColourMap(Self, Self->ColourMap.c_str());
         }
      }
      else Self->Colours->apply_resolution(Value);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SpreadMethod: Determines the rendering behaviour to use when gradient colours are cycled.

SpreadMethod determines what happens when the first cycle of gradient colours is exhausted and needs to begin again.
The default setting is `VSPREAD::PAD`.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_SET_SpreadMethod(extVectorGradient *Self, VSPREAD Value)
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

static ERR VECTORGRADIENT_GET_Stops(extVectorGradient *Self, GradientStop **Value, int *Elements)
{
   *Value    = Self->Stops.data();
   *Elements = Self->Stops.size();
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_Stops(extVectorGradient *Self, GradientStop *Value, int Elements)
{
   Self->Stops.clear();

   if (Elements >= 2) {
      Self->modified();
      Self->Stops.insert(Self->Stops.end(), &Value[0], &Value[Elements]);
      if (Self->Colours) delete Self->Colours;
      Self->Colours = new (std::nothrow) GradientColours(Self->Stops, Self->ColourSpace, 1.0, Self->Resolution);
      if (!Self->Colours) return ERR::AllocMemory;
      return ERR::Okay;
   }
   else {
      pf::Log log;
      log.warning("Array size %d < 2", Elements);
      return ERR::InvalidValue;
   }
}

/*********************************************************************************************************************
-FIELD-
TotalStops: Total number of stops defined in the Stops array.

This read-only field indicates the total number of stops that have been defined in the #Stops array.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_TotalStops(extVectorGradient *Self, int *Value)
{
   *Value = Self->Stops.size();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Transform: Applies a transform to the gradient.

A transform can be applied to the gradient by setting this field with an SVG compliant transform string.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_SET_Transform(extVectorGradient *Self, CSTRING Commands)
{
   pf::Log log;

   if (!Commands) return log.warning(ERR::InvalidValue);

   Self->modified();

   if (!Self->Matrices) {
      VectorMatrix *matrix;
      if (AllocMemory(sizeof(VectorMatrix), MEM::DATA|MEM::NO_CLEAR, &matrix) IS ERR::Okay) {
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
Type: Specifies the type of gradient (e.g. `RADIAL`, `LINEAR`)
Lookup: VGT

The type of the gradient to be drawn is specified here.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_SET_Type(extVectorGradient *Self, VGT Value)
{
   Self->Type = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Units: Defines the coordinate system for #X1, #Y1, #X2 and #Y2.

The default coordinate system for gradients is `BOUNDING_BOX`, which positions the gradient around the vector that
references it.  The alternative is `USERSPACE`, which positions the gradient scaled to the current viewport.

-FIELD-
X1: Initial X coordinate for the gradient.

For linear gradients, the `(X1, Y1)` field values define the starting coordinate for mapping linear gradients.  The
gradient will be drawn from `(X1, Y1)` to `(X2, Y2)`.  Coordinate values can be expressed as units that are
scaled to the target space.

For contour gradients, `X1` is used as the floor for the gradient colour values and `X2` acts as a multiplier.
`X1` has a range of `0 < X1 < X2` and `X2` has a range of `.01 < X2 < 10`.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_X1(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->X1);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_X1(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X1) & (~VGF::FIXED_X1);
   else Self->Flags = (Self->Flags | VGF::FIXED_X1) & (~VGF::SCALED_X1);
   Self->X1 = Value;
   Self->CalcAngle = true;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X2: Final X coordinate for the gradient.

For linear gradients, the `(X1, Y1)` field values define the starting coordinate for mapping linear gradients.  The
gradient will be drawn from `(X1, Y1)` to `(X2, Y2)`.  Coordinate values can be expressed as units that are
scaled to the target space.

For contour gradients, `X1` is used as the floor for the gradient colour values and `X2` acts as a multiplier.
`X1` has a range of `0 < X1 < X2` and `X2` has a range of `.01 < X2 < 10`.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_X2(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->X2);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_X2(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X2) & (~VGF::FIXED_X2);
   else Self->Flags = (Self->Flags | VGF::FIXED_X2) & (~VGF::SCALED_X2);
   Self->X2 = Value;
   Self->CalcAngle = true;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y1: Initial Y coordinate for the gradient.

The `(X1, Y1)` field values define the starting coordinate for mapping linear gradients.  Other gradient types ignore
these values.  The gradient will be drawn from `(X1, Y1)` to `(X2, Y2)`.

Coordinate values can also be expressed as units that are scaled to the target space.

*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_Y1(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->Y1);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_Y1(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_Y1) & (~VGF::FIXED_Y1);
   else Self->Flags = (Self->Flags | VGF::FIXED_Y1) & (~VGF::SCALED_Y1);
   Self->Y1 = Value;
   Self->CalcAngle = true;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y2: Final Y coordinate for the gradient.

The `(X2, Y2)` field values define the end coordinate for mapping linear gradients.  Other gradient types ignore
these values.  The gradient will be drawn from `(X1, Y1)` to `(X2, Y2)`.

Coordinate values can also be expressed as units that are scaled to the target space.
-END-
*********************************************************************************************************************/

static ERR VECTORGRADIENT_GET_Y2(extVectorGradient *Self, Unit *Value)
{
   Value->set(Self->Y2);
   return ERR::Okay;
}

static ERR VECTORGRADIENT_SET_Y2(extVectorGradient *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_Y2) & (~VGF::FIXED_Y2);
   else Self->Flags = (Self->Flags | VGF::FIXED_Y2) & (~VGF::SCALED_Y2);
   Self->Y2 = Value;
   Self->CalcAngle = true;
   Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_def.c"

static const FieldArray clGradientFields[] = {
   { "X1",           FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_X1, VECTORGRADIENT_SET_X1 },
   { "Y1",           FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_Y1, VECTORGRADIENT_SET_Y1 },
   { "X2",           FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_X2, VECTORGRADIENT_SET_X2 },
   { "Y2",           FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_Y2, VECTORGRADIENT_SET_Y2 },
   { "CenterX",      FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_CenterX, VECTORGRADIENT_SET_CenterX },
   { "CenterY",      FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_CenterY, VECTORGRADIENT_SET_CenterY },
   { "FocalX",       FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_FocalX, VECTORGRADIENT_SET_FocalX },
   { "FocalY",       FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_FocalY, VECTORGRADIENT_SET_FocalY },
   { "Radius",       FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_Radius, VECTORGRADIENT_SET_Radius },
   { "FocalRadius",  FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_FocalRadius, VECTORGRADIENT_SET_FocalRadius },
   { "Resolution",   FDF_DOUBLE|FDF_RW, nullptr, VECTORGRADIENT_SET_Resolution },
   { "SpreadMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORGRADIENT_SET_SpreadMethod, &clVectorGradientSpreadMethod },
   { "Units",        FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clVectorGradientUnits },
   { "Type",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORGRADIENT_SET_Type, &clVectorGradientType },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clVectorGradientFlags },
   { "ColourSpace",  FDF_INT|FDF_RI, nullptr, nullptr, &clVectorGradientColourSpace },
   // Virtual fields
   { "Colour",       FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FD_RW, VECTORGRADIENT_GET_Colour, VECTORGRADIENT_SET_Colour },
   { "ColourMap",    FDF_VIRTUAL|FDF_STRING|FDF_W, VECTORGRADIENT_GET_ColourMap, VECTORGRADIENT_SET_ColourMap },
   { "CX",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_CenterX, VECTORGRADIENT_SET_CenterX },
   { "CY",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_CenterY, VECTORGRADIENT_SET_CenterY },
   { "FX",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_FocalX, VECTORGRADIENT_SET_FocalX },
   { "FY",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORGRADIENT_GET_FocalY, VECTORGRADIENT_SET_FocalY },
   { "Matrices",     FDF_VIRTUAL|FDF_POINTER|FDF_STRUCT|FDF_RW, VECTORGRADIENT_GET_Matrices, VECTORGRADIENT_SET_Matrices, "VectorMatrix" },
   { "NumericID",    FDF_VIRTUAL|FDF_INT|FDF_RW, VECTORGRADIENT_GET_NumericID, VECTORGRADIENT_SET_NumericID },
   { "ID",           FDF_VIRTUAL|FDF_STRING|FDF_RW, VECTORGRADIENT_GET_ID, VECTORGRADIENT_SET_ID },
   { "Stops",        FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW, VECTORGRADIENT_GET_Stops, VECTORGRADIENT_SET_Stops, "GradientStop" },
   { "TotalStops",   FDF_INT|FDF_R, VECTORGRADIENT_GET_TotalStops },
   { "Transform",    FDF_VIRTUAL|FDF_STRING|FDF_W, nullptr, VECTORGRADIENT_SET_Transform },
   END_FIELD
};

//********************************************************************************************************************

ERR init_gradient(void) // The gradient is a definition type for creating gradients and not drawing.
{
   clVectorGradient = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORGRADIENT),
      fl::Name("VectorGradient"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorGradientActions),
      fl::Fields(clGradientFields),
      fl::Size(sizeof(extVectorGradient)),
      fl::Path(MOD_PATH));

   return clVectorGradient ? ERR::Okay : ERR::AddClass;
}
