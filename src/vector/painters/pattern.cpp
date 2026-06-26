/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of pattern definitions only.

-CLASS-
VectorPattern: Provides support for the filling and stroking of vectors with patterns.

The VectorPattern class is used by Vector painting algorithms to fill and stroke vectors with pre-rendered patterns.
It is the most efficient way of rendering a common set of graphics multiple times.

The VectorPattern must be registered with a @VectorScene via the @VectorScene.AddDef() method.
Any vector within the target scene will be able to utilise the pattern for filling or stroking by referencing its
name through the @Vector.Fill and @Vector.Stroke fields.  For instance `url(#dots)`.

A special use case is made for patterns that are applied as a fill operation in @VectorViewport objects.  In this
case the renderer will dynamically render the pattern as a background within the viewport.  This ensures that the
pattern is rendered at maximum fidelity whenever it is used, and not affected by bitmap clipping restrictions.  It
should be noted that this means the image caching feature will be disabled.

It is strongly recommended that the VectorPattern is owned by the @VectorScene that is handling the
definition.  This will ensure that the VectorPattern is deallocated when the scene is destroyed.
-END-

NOTE: The VectorPattern inherits attributes from the VectorScene, which is used to define the size of the pattern and
contains the pattern content.

*********************************************************************************************************************/

static ERR VECTORPATTERN_Draw(extVectorPattern *Self, struct acDraw *Args)
{
   kt::Log log;

   if (not Self->Scene->PageWidth) return log.warning(ERR::FieldNotSet);
   if (not Self->Scene->PageHeight) return log.warning(ERR::FieldNotSet);

   if (Self->Bitmap) {
      if ((Self->Scene->PageWidth != Self->Bitmap->Width) or (Self->Scene->PageHeight != Self->Bitmap->Height)) {
         acResize(Self->Bitmap, Self->Scene->PageWidth, Self->Scene->PageHeight, 32);
      }
   }
   else if (not (Self->Bitmap = objBitmap::create::local(
      fl::Width(Self->Scene->PageWidth),
      fl::Height(Self->Scene->PageHeight),
      fl::Flags(BMF::ALPHA_CHANNEL),
      fl::BitsPerPixel(32)))) return ERR::CreateObject;

   clearmem(Self->Bitmap->Data, Self->Bitmap->LineWidth * Self->Bitmap->Height);
   Self->Scene->Bitmap = Self->Bitmap;
   auto error = acDraw(Self->Scene);
   if (error != ERR::Okay) return error;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORPATTERN_Init(extVectorPattern *Self)
{
   kt::Log log;

   if ((int(Self->SpreadMethod) <= 0) or (int(Self->SpreadMethod) >= int(VSPREAD::END))) {
      log.traceWarning("Invalid SpreadMethod value of %d", Self->SpreadMethod);
      return log.warning(ERR::OutOfRange);
   }

   if ((int(Self->Units) <= 0) or (int(Self->Units) >= int(VUNIT::END))) {
      log.traceWarning("Invalid Units value of %d", Self->Units);
      return log.warning(ERR::OutOfRange);
   }

   if ((not Self->Width.defined()) or (double(Self->Width) IS 0)) Self->Width = Unit(1.0, FD_SCALED);
   if ((not Self->Height.defined()) or (double(Self->Height) IS 0)) Self->Height = Unit(1.0, FD_SCALED);

   if (InitObject(Self->Scene) != ERR::Okay) return ERR::Init;
   if (InitObject(Self->Viewport) != ERR::Okay) return ERR::Init;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ContentUnits: Not yet implemented.

In compliance with SVG requirements, the application of ContentUnits is only effective if the Viewport's X, Y, Width
and Height fields have been defined.  The default setting is `USERSPACE`.

-FIELD-
Height: Height of the pattern tile.

The (Width,Height) field values define the dimensions of the pattern tile.  If the provided value is scaled,
then the dimension is calculated relative to the bounding box or viewport applying the pattern, dependent on the
#Units setting.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Height(extVectorPattern *Self, Unit &Value)
{
   Self->Height = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Inherit: Inherit attributes from a VectorPattern referenced here.

Attributes can be inherited from another pattern by referencing it in this field.  This feature is provided
primarily for the purpose of simplifying SVG compatibility and its use may result in an unnecessary performance
penalty.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Inherit(extVectorPattern *Self, extVectorPattern *Value)
{
   if (Value) {
      if (Value->classID() IS CLASSID::VECTORPATTERN) {
         Self->Inherit = Value;
      }
      else return ERR::InvalidValue;
   }
   else Self->Inherit = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Matrices: Applies one or more transforms to a pattern.

A transform can be applied to a pattern via one or more matrices.  These will influence how pattern fills are
rendered within their vector space.  Each matrix is represented by a !VectorMatrix structure, and the matrices are
linked in the order in which they should be applied.

!VectorMatrix

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Matrices(extVectorPattern *Self, const std::span<const VectorMatrix> *Value)
{
   if (Value) {
      Self->Matrices.assign(Value->begin(), Value->end());

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
Opacity: The opacity of the pattern.

The opacity of the pattern is defined as a value between 0.0 and 1.0, with 1.0 being fully opaque.  The default value
is 1.0.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Opacity(extVectorPattern *Self, double Value)
{
   Self->Opacity = std::clamp(Value, 0.0, 1.0);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Scene: Refers to the internal @VectorScene that will contain the rendered pattern.

The VectorPattern class allocates a @VectorScene in this field and inherits its functionality.  In addition,
a @VectorViewport class will be assigned to the scene and is referenced in the #Viewport field for
managing the vectors that will be rendered.

-FIELD-
SpreadMethod: The behaviour to use when the pattern bounds do not match the vector path.

Indicates what happens if the pattern starts or ends inside the bounds of the target vector.  The default value is
`VSPREAD::PAD`.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_SpreadMethod(extVectorPattern *Self, VSPREAD Value)
{
   Self->SpreadMethod = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Transform: Applies a transform to the pattern during the render process.

A transform can be applied to the pattern by setting this field with an SVG compliant transform string.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Transform(extVectorPattern *Self, const std::string_view &Commands)
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
Units:  Defines the coordinate system for fields X, Y, Width and Height.

This field declares the coordinate system that is used for values in the #X, #Y, #Width and #Height fields.  The
default setting is `BOUNDING_BOX`, which means the pattern will be drawn to scale in realtime.  The most efficient
method is `USERSPACE`, which allows the pattern image to be persistently cached.

-FIELD-
Viewport: Refers to the viewport that contains the pattern.

The Viewport refers to a @VectorViewport object that is created to host the vectors for the rendered pattern.  If the
Viewport does not contain at least one vector that renders an image, the pattern will be ineffective.

-FIELD-
Width: Width of the pattern tile.

The (Width,Height) field values define the dimensions of the pattern tile.  If the provided value is scaled,
the dimension is calculated relative to the bounding box or viewport applying the pattern, dependent on the
#Units setting.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Width(extVectorPattern *Self, Unit &Value)
{
   Self->Width = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
X: X coordinate for the pattern.

The (X,Y) field values define the starting coordinate for mapping patterns.

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_X(extVectorPattern *Self, Unit &Value)
{
   Self->X = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: Y coordinate for the pattern.

The (X,Y) field values define the starting coordinate for mapping patterns.
-END-

*********************************************************************************************************************/

static ERR VECTORPATTERN_SET_Y(extVectorPattern *Self, Unit &Value)
{
   Self->Y = Value;
   Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

extVectorPattern::~extVectorPattern() {
   if (Bitmap) FreeResource(Bitmap);
   if (Scene)  FreeResource(Scene);
}

//********************************************************************************************************************

#include "pattern_def.cpp"

static const FieldArray clVectorPatternFields[] = {
   { "X",            FDF_UNIT|FDF_RW, nullptr, VECTORPATTERN_SET_X },
   { "Y",            FDF_UNIT|FDF_RW, nullptr, VECTORPATTERN_SET_Y },
   { "Width",        FDF_UNIT|FDF_RW, nullptr, VECTORPATTERN_SET_Width },
   { "Height",       FDF_UNIT|FDF_RW, nullptr, VECTORPATTERN_SET_Height },
   { "Matrices",     FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, VECTORPATTERN_SET_Matrices, "VectorMatrix" },
   { "Opacity",      FDF_DOUBLE|FDF_RW, nullptr, VECTORPATTERN_SET_Opacity },
   { "Scene",        FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::VECTORSCENE },
   { "Viewport",     FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Inherit",      FDF_OBJECT|FDF_RW, nullptr, VECTORPATTERN_SET_Inherit },
   { "SpreadMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORPATTERN_SET_SpreadMethod, &clVectorPatternSpreadMethod },
   { "Units",        FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorPatternUnits },
   { "ContentUnits", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorPatternContentUnits },
   //{ "AspectRatio", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, VECTORPATTERN_GET_AspectRatio, VECTORPATTERN_SET_AspectRatio, &clAspectRatio },
   // Virtual fields
   { "Transform",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_W, nullptr, VECTORPATTERN_SET_Transform },
   END_FIELD
};

//********************************************************************************************************************

ERR init_pattern(void) // The pattern is a definition type for creating patterns and not drawing.
{
   clVectorPattern = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORPATTERN),
      fl::Name("VectorPattern"),
      fl::Category(CCF::GRAPHICS),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::Actions(clVectorPatternActions),
      fl::Fields(clVectorPatternFields),
      fl::Size(sizeof(extVectorPattern)),
      fl::Path(MOD_PATH));

   return clVectorPattern ? ERR::Okay : ERR::AddClass;
}
