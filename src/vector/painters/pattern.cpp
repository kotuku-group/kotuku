/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of pattern definitions only.

-CLASS-
VectorPattern: Provides support for the filling and stroking of vectors with patterns.

The VectorPattern class is used by Vector painting algorithms to fill and stroke vectors with pre-rendered patterns.
It is the most efficient way of rendering a common set of graphics multiple times.

The VectorPattern must be registered with a @VectorScene via the @VectorScene.AddDef() method.
Any vector within the target scene will be able to utilise the pattern for filling or stroking by referencing its
name through the @Vector.Fill and @Vector.Stroke fields.  For instance `url(#dots)`.

A special use case is made for patterns that are applied as a fill operation in @VectorViewport objects.  If the
pattern is a single tile that covers the viewport, which is the default when #Units is `BOUNDING_BOX`, #X and #Y
are zero, #Width and #Height are `100%` and no #Transform is set, the renderer will dynamically render the pattern
as a background within the viewport.  This ensures that the pattern is rendered at maximum fidelity whenever it is
used, and not affected by bitmap clipping restrictions.  It should be noted that this means the image caching
feature will be disabled.

Any other configuration is tiled within the viewport in the same way as a vector fill, with the viewport serving as
both the bounding box and the user space of the pattern.  User space coordinates are therefore relative to the
viewport's top-left corner.

It is strongly recommended that the VectorPattern is owned by the @VectorScene that is handling the
definition.  This will ensure that the VectorPattern is deallocated when the scene is destroyed.
-END-

NOTE: The VectorPattern inherits attributes from the VectorScene, which is used to define the size of the pattern and
contains the pattern content.

*********************************************************************************************************************/

// Prepare the pattern's viewport for rendering against a consumer.  BoxWidth and BoxHeight are the dimensions of the
// consumer's bounding box, to which bounding-box content units are relative.  Scale is the resolution scaling that
// is applied to the pattern's target area, e.g. when the tile bitmap is rendered at display scale.
//
// A configured viewbox is fitted to the target area with the viewport's aspect ratio, so no preparation is needed.
// Without a viewbox, a content scale is derived so that user-space content retains its coordinate scale and
// bounding-box content is stretched independently on each axis.  The aspect ratio has no effect in that case.  The
// viewport is only marked dirty if the derived mapping has changed.

void prepare_pattern(extVectorPattern *Self, double BoxWidth, double BoxHeight, double Scale)
{
   auto view = (extVectorViewport *)Self->Viewport;
   std::optional<std::array<double, 2>> derived;

   if ((view->vpViewWidth <= 0) or (view->vpViewHeight <= 0)) {
      std::array<double, 2> scale = { Scale, Scale };
      if (Self->ContentUnits IS VUNIT::BOUNDING_BOX) {
         scale[0] *= BoxWidth;
         scale[1] *= BoxHeight;
      }

      if ((scale[0] > 0) and (scale[1] > 0)) derived = scale;
   }

   if (derived != view->vpDerivedScale) {
      view->vpDerivedScale = derived;
      mark_dirty(view, RC::DIRTY);
   }
}

//********************************************************************************************************************
// Render the pattern's scene to its tile bitmap.  The scene's page dimensions must have been defined and the
// viewport prepared with prepare_pattern().  BlendMode is the tile's blend preference, which a consumer sets to its own
// resolved mode so that overlapping content in the tile blends as it would if drawn directly.

ERR render_pattern(extVectorPattern *Self, BLM BlendMode)
{
   kt::Log log(__FUNCTION__);

   if (not Self->Scene->PageWidth) return log.warning(ERR::FieldNotSet);
   if (not Self->Scene->PageHeight) return log.warning(ERR::FieldNotSet);

   if (Self->Bitmap) {
      if ((Self->Scene->PageWidth != Self->Bitmap->Width) or (Self->Scene->PageHeight != Self->Bitmap->Height)) {
         acResize(Self->Bitmap, Self->Scene->PageWidth, Self->Scene->PageHeight, 32);
      }
   }
   else if (not (Self->Bitmap = objBitmap::create::local(
      fl::Owner(Self->UID),
      fl::Width(Self->Scene->PageWidth),
      fl::Height(Self->Scene->PageHeight),
      fl::Flags(BMF::ALPHA_CHANNEL),
      fl::BitsPerPixel(32)))) return ERR::CreateObject;

   Self->Bitmap->BlendMode = BlendMode;
   clearmem(Self->Bitmap->Data, Self->Bitmap->LineWidth * Self->Bitmap->Height);
   Self->Scene->Bitmap = Self->Bitmap;
   return acDraw(Self->Scene);
}

//********************************************************************************************************************
// Drawing the pattern directly renders a single tile to the internal bitmap at the dimensions of the pattern's
// Scene.  In the absence of a consumer, the tile serves as the bounding box for bounding-box content units.

static ERR VECTORPATTERN_Draw(extVectorPattern *Self, struct acDraw *Args)
{
   prepare_pattern(Self, Self->Scene->PageWidth, Self->Scene->PageHeight, 1.0);
   return render_pattern(Self);
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
AspectRatio: Controls how the viewport's viewbox is fitted to the pattern tile.
Lookup: ARF

The AspectRatio determines how the source area defined by the #Viewport's `ViewX`, `ViewY`, `ViewWidth` and
`ViewHeight` fields is scaled and aligned within each pattern tile.  It is equivalent to the `preserveAspectRatio`
attribute of SVG patterns.  The default value is `MEET|X_MID|Y_MID`, which scales the content uniformly so that it
fits within the tile and centres it.  Use `SLICE` to fill the tile while clipping the overflowing content, or `NONE`
to stretch the content independently on each axis.

The AspectRatio is only effective if a viewbox has been defined for the #Viewport.  It controls the fitting of
content within a tile and has no effect on the tile's dimensions or position, which are determined by #X, #Y, #Width,
#Height and #Units.

This field is a convenient alias for the `AspectRatio` of the pattern's #Viewport, and the two always report the
same value.

*********************************************************************************************************************/

static ERR VECTORPATTERN_GET_AspectRatio(extVectorPattern *Self, ARF &Value)
{
   Value = ((extVectorViewport *)Self->Viewport)->vpAspectRatio;
   return ERR::Okay;
}

static ERR VECTORPATTERN_SET_AspectRatio(extVectorPattern *Self, ARF Value)
{
   if (((extVectorViewport *)Self->Viewport)->vpAspectRatio IS Value) return ERR::Okay;

   if (auto error = Self->Viewport->setAspectRatio(Value); error != ERR::Okay) return error;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ContentUnits: Defines the coordinate system for the content of the pattern.

ContentUnits declares the coordinate system of the vectors in the pattern's #Viewport.  The default setting is
`USERSPACE`, in which content coordinates are expressed in the same units as the pattern tile.  If set to
`BOUNDING_BOX`, content coordinates are expressed as fractions of the bounding box of the vector that is consuming
the pattern, e.g. a coordinate of `(0.5,0.5)` refers to the centre of that bounding box.  In this case the
content is stretched independently on each axis and the #AspectRatio has no effect.  The mapping is always relative
to the consumer's bounding box, regardless of the #Units setting.

In compliance with SVG, ContentUnits has no effect if a viewbox has been defined in the #Viewport.  The viewbox
determines the content's coordinate system in that case.

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
default setting is `BOUNDING_BOX`, which means the pattern tile is sized relative to the bounding box of each vector
that uses it.  With `USERSPACE`, the tile size is independent of the consumer, which makes it the most efficient
choice when a pattern is shared by vectors of different sizes.

In both cases the tile is rendered at its scale on the display, including any scaling from #Transform, and it is
cached until the definition or that display size changes.

-FIELD-
Viewport: Refers to the viewport that contains the pattern.

The Viewport refers to a @VectorViewport object that is created to host the vectors for the rendered pattern.  If the
Viewport does not contain at least one vector that renders an image, the pattern will be ineffective.

The renderer manages the transform and size of the Viewport so that it matches each pattern tile.  Defining the
Viewport's `ViewX`, `ViewY`, `ViewWidth` and `ViewHeight` fields establishes a viewbox for the pattern content, which
is fitted to the tile according to the #AspectRatio.

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
   // Virtual fields
   { "AspectRatio",  FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, VECTORPATTERN_GET_AspectRatio, VECTORPATTERN_SET_AspectRatio, &clAspectRatio },
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
