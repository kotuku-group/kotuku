/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of pattern definitions only.

-CLASS-
VectorPattern: Provides support for the filling and stroking of vectors with patterns.

The VectorPattern class is used by Vector painting algorithms to fill and stroke vectors with pre-rendered patterns.
It is the most efficient way of rendering a common set of graphics multiple times.

The VectorPattern must be registered with a @VectorScene via the <method class="VectorScene">AddDef</> method.
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

static ERR PATTERN_Draw(extVectorPattern *Self, struct acDraw *Args)
{
   kt::Log log;

   if (!Self->Scene->PageWidth) return log.warning(ERR::FieldNotSet);
   if (!Self->Scene->PageHeight) return log.warning(ERR::FieldNotSet);

   if (Self->Bitmap) {
      if ((Self->Scene->PageWidth != Self->Bitmap->Width) or (Self->Scene->PageHeight != Self->Bitmap->Height)) {
         acResize(Self->Bitmap, Self->Scene->PageWidth, Self->Scene->PageHeight, 32);
      }
   }
   else if (!(Self->Bitmap = objBitmap::create::local(
      fl::Width(Self->Scene->PageWidth),
      fl::Height(Self->Scene->PageHeight),
      fl::Flags(BMF::ALPHA_CHANNEL),
      fl::BitsPerPixel(32)))) return ERR::CreateObject;

   clearmem(Self->Bitmap->Data, Self->Bitmap->LineWidth * Self->Bitmap->Height);
   Self->Scene->Bitmap = Self->Bitmap;
   acDraw(Self->Scene);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PATTERN_Free(extVectorPattern *Self)
{
   VectorMatrix *next;
   for (auto node=Self->Matrices; node; node=next) {
      next = node->Next;
      FreeResource(node);
   }
   Self->Matrices = nullptr;

   if (Self->Bitmap) { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }
   if (Self->Scene)  { FreeResource(Self->Scene); Self->Scene = nullptr; }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PATTERN_Init(extVectorPattern *Self)
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

   if (!Self->Width) {
      Self->Width = 1.0;
      Self->Dimensions |= DMF::SCALED_WIDTH;
   }

   if (!Self->Height) {
      Self->Height = 1.0;
      Self->Dimensions |= DMF::SCALED_HEIGHT;
   }

   if (InitObject(Self->Scene) != ERR::Okay) return ERR::Init;
   if (InitObject(Self->Viewport) != ERR::Okay) return ERR::Init;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PATTERN_NewObject(extVectorPattern *Self)
{
   if (NewLocalObject(CLASSID::VECTORSCENE, &Self->Scene) IS ERR::Okay) {
      if (NewObject(CLASSID::VECTORVIEWPORT, &Self->Viewport) IS ERR::Okay) {
         SetOwner(Self->Viewport, Self->Scene);
         Self->SpreadMethod = VSPREAD::REPEAT;
         Self->Units        = VUNIT::BOUNDING_BOX;
         Self->ContentUnits = VUNIT::USERSPACE;
         Self->Opacity      = 1.0;
         return ERR::Okay;
      }
      else return ERR::NewObject;
   }
   else return ERR::NewObject;
}

/*********************************************************************************************************************

-FIELD-
ContentUnits: Private. Not yet implemented.

In compliance with SVG requirements, the application of ContentUnits is only effective if the Viewport's X, Y, Width
and Height fields have been defined.  The default setting is `USERSPACE`.

-FIELD-
Dimensions: Dimension flags are stored here.

-FIELD-
Height: Height of the pattern tile.

The (Width,Height) field values define the dimensions of the pattern tile.  If the provided value is scaled,
then the dimension is calculated relative to the bounding box or viewport applying the pattern, dependent on the
#Units setting.

*********************************************************************************************************************/

static ERR PATTERN_GET_Height(extVectorPattern *Self, Unit *Value)
{
   Value->set(Self->Height);
   return ERR::Okay;
}

static ERR PATTERN_SET_Height(extVectorPattern *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_HEIGHT) & (~DMF::FIXED_HEIGHT);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_HEIGHT) & (~DMF::SCALED_HEIGHT);
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

static ERR PATTERN_SET_Inherit(extVectorPattern *Self, extVectorPattern *Value)
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
Matrices: A linked list of transform matrices that have been applied to the pattern.

All transforms that have been applied to the pattern can be read from the Matrices field.  Each transform is
represented by a !VectorMatrix structure, and are linked in the order in which they were applied to the pattern.

Setting this field is always additive unless NULL is passed, in which case all existing matrices are removed.

!VectorMatrix

*********************************************************************************************************************/

static ERR VECTORPATTERN_GET_Matrices(extVectorPattern *Self, VectorMatrix **Value)
{
   *Value = Self->Matrices;
   return ERR::Okay;
}

static ERR VECTORPATTERN_SET_Matrices(extVectorPattern *Self, VectorMatrix *Value)
{
   if (Value) {
      auto hook = &Self->Matrices;
      while (Value) {
         VectorMatrix *matrix;
         if (AllocMemory(sizeof(VectorMatrix), MEM::DATA|MEM::NO_CLEAR, (APTR *)&matrix) IS ERR::Okay) {
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
Opacity: The opacity of the pattern.

The opacity of the pattern is defined as a value between 0.0 and 1.0, with 1.0 being fully opaque.  The default value
is 1.0.

*********************************************************************************************************************/

static ERR PATTERN_SET_Opacity(extVectorPattern *Self, double Value)
{
   if (Value < 0.0) Value = 0;
   else if (Value > 1.0) Value = 1.0;
   Self->Opacity = Value;
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

Indicates what happens if the pattern starts or ends inside the bounds of the target vector.  The default value is PAD.

*********************************************************************************************************************/

static ERR PATTERN_SET_SpreadMethod(extVectorPattern *Self, VSPREAD Value)
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

static ERR PATTERN_SET_Transform(extVectorPattern *Self, const std::string_view &Commands)
{
   kt::Log log;

   if (Commands.empty()) return log.warning(ERR::InvalidValue);

   Self->modified();

   if (not Self->Matrices) {
      VectorMatrix *matrix;
      if (AllocMemory(sizeof(VectorMatrix), MEM::DATA|MEM::NO_CLEAR, (APTR *)&matrix) IS ERR::Okay) {
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
Units:  Defines the coordinate system for fields X, Y, Width and Height.

This field declares the coordinate system that is used for values in the #X and #Y fields.  The default setting is
`BOUNDING_BOX`, which means the pattern will be drawn to scale in realtime.  The most efficient method is USERSPACE,
which allows the pattern image to be persistently cached.

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

static ERR PATTERN_GET_Width(extVectorPattern *Self, Unit *Value)
{
   Value->set(Self->Width);
   return ERR::Okay;
}

static ERR PATTERN_SET_Width(extVectorPattern *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_WIDTH) & (~DMF::FIXED_WIDTH);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_WIDTH) & (~DMF::SCALED_WIDTH);
   Self->Width = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
X: X coordinate for the pattern.

The (X,Y) field values define the starting coordinate for mapping patterns.

*********************************************************************************************************************/

static ERR PATTERN_GET_X(extVectorPattern *Self, Unit *Value)
{
   Value->set(Self->X);
   return ERR::Okay;
}

static ERR PATTERN_SET_X(extVectorPattern *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_X) & (~DMF::FIXED_X);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_X) & (~DMF::SCALED_X);
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

static ERR PATTERN_GET_Y(extVectorPattern *Self, Unit *Value)
{
   Value->set(Self->Y);
   return ERR::Okay;
}

static ERR PATTERN_SET_Y(extVectorPattern *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_Y) & (~DMF::FIXED_Y);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_Y) & (~DMF::SCALED_Y);
   Self->Y = Value;
   Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

static const ActionArray clPatternActions[] = {
   { AC::Draw,      PATTERN_Draw },
   { AC::Free,      PATTERN_Free },
   { AC::Init,      PATTERN_Init },
   { AC::NewObject, PATTERN_NewObject },
   { AC::NIL, nullptr }
};

static const FieldDef clPatternDimensions[] = {
   { "FixedX",       DMF::FIXED_X },
   { "FixedY",       DMF::FIXED_Y },
   { "ScaledX",      DMF::SCALED_X },
   { "ScaledY",      DMF::SCALED_Y },
   { "FixedWidth",   DMF::FIXED_WIDTH },
   { "FixedHeight",  DMF::FIXED_HEIGHT },
   { "ScaledWidth",  DMF::SCALED_WIDTH },
   { "ScaledHeight", DMF::SCALED_HEIGHT },
   { nullptr, 0 }
};

static const FieldDef clPatternUnits[] = {
   { "BoundingBox", VUNIT::BOUNDING_BOX },  // Coordinates are relative to the object's bounding box
   { "UserSpace",   VUNIT::USERSPACE },    // Coordinates are relative to the current viewport
   { nullptr, 0 }
};

static const FieldDef clPatternSpread[] = {
   { "Pad",      VSPREAD::PAD },
   { "Reflect",  VSPREAD::REFLECT },
   { "Repeat",   VSPREAD::REPEAT },
   { "ReflectX", VSPREAD::REFLECT_X },
   { "ReflectY", VSPREAD::REFLECT_Y },
   { nullptr, 0 }
};

static const FieldArray clPatternFields[] = {
   { "X",            FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, PATTERN_GET_X, PATTERN_SET_X },
   { "Y",            FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, PATTERN_GET_Y, PATTERN_SET_Y },
   { "Width",        FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, PATTERN_GET_Width, PATTERN_SET_Width },
   { "Height",       FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, PATTERN_GET_Height, PATTERN_SET_Height },
   { "Opacity",      FDF_DOUBLE|FDF_RW, nullptr, PATTERN_SET_Opacity },
   { "Scene",        FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::VECTORSCENE },
   { "Viewport",     FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Inherit",      FDF_OBJECT|FDF_RW, nullptr, PATTERN_SET_Inherit },
   { "SpreadMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PATTERN_SET_SpreadMethod, &clPatternSpread },
   { "Units",        FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clPatternUnits },
   { "ContentUnits", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clPatternUnits },
   { "Dimensions",   FDF_INTFLAGS|FDF_R, nullptr, nullptr, &clPatternDimensions },
   //{ "AspectRatio", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, PATTERN_GET_AspectRatio, PATTERN_SET_AspectRatio, &clAspectRatio },
   // Virtual fields
   { "Matrices",     FDF_VIRTUAL|FDF_POINTER|FDF_STRUCT|FDF_RW|FDF_PURE, VECTORPATTERN_GET_Matrices, VECTORPATTERN_SET_Matrices, "VectorMatrix" },
   { "Transform",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_W, nullptr, PATTERN_SET_Transform },
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
      fl::Actions(clPatternActions),
      fl::Fields(clPatternFields),
      fl::Size(sizeof(extVectorPattern)),
      fl::Path(MOD_PATH));

   return clVectorPattern ? ERR::Okay : ERR::AddClass;
}
