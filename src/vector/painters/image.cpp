/*********************************************************************************************************************

-CLASS-
VectorImage: Provides support for the filling and stroking of vectors with bitmap images.

The VectorImage class is used by Vector painting algorithms to fill and stroke vectors with bitmap images.  This is
achieved by initialising a VectorImage object with the desired settings and then registering it with
a @VectorScene via the @VectorScene.AddDef() method.

Any vector within the target scene will be able to utilise the image for filling or stroking by referencing its
name through the @Vector.Fill and @Vector.Stroke fields.  For instance 'url(#logo)'.

It is strongly recommended that the VectorImage is owned by the @VectorScene that is handling the
definition.  This will ensure that the VectorImage is de-allocated when the scene is destroyed.

NOTE: For the rendering of vectors as flattened images, use @VectorPattern.
-END-

*********************************************************************************************************************/

static ERR IMAGE_Init(extVectorImage *Self)
{
   kt::Log log;

   if (!Self->Bitmap) return log.warning(ERR::FieldNotSet);

   if ((Self->Bitmap->BitsPerPixel != 24) and (Self->Bitmap->BitsPerPixel != 32)) {
      return log.warning(ERR::NoSupport);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR IMAGE_NewObject(extVectorImage *Self)
{
   Self->Units        = VUNIT::BOUNDING_BOX;
   Self->SpreadMethod = VSPREAD::CLIP;
   Self->AspectRatio  = ARF::X_MID|ARF::Y_MID|ARF::MEET; // SVG defaults
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
AspectRatio: Flags that affect the aspect ratio of the image within its target vector.
Lookup: ARF

Defining an aspect ratio allows finer control over the position and scale of the image within its target
vector.

<types lookup="ARF"/>

*********************************************************************************************************************/

static ERR IMAGE_SET_AspectRatio(extVectorImage *Self, ARF Value)
{
   Self->AspectRatio = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Bitmap: Reference to a source bitmap for the rendering algorithm.

This field must be set prior to initialisation.  It will refer to a source bitmap that will be used by the rendering
algorithm.  The source bitmap must be in a 32-bit graphics format.

*********************************************************************************************************************/

static ERR IMAGE_SET_Bitmap(extVectorImage *Self, objBitmap *Value)
{
   if (Value->BitsPerPixel < 32) {
      kt::Log().warning("The source image must be 32 bit, not %d bit.", Value->BitsPerPixel);
      return ERR::InvalidData;
   }

   Self->Bitmap = Value;
   Self->Image = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Image: Refers to a @Image from which the source #Bitmap is acquired.

If an image bitmap is sourced from a @Image then this field may be used to refer to the @Image object.  The image
will not be used directly by the VectorImage, as only the bitmap is of interest.

The image bitmap must be in a 32-bit graphics format.

*********************************************************************************************************************/

static ERR IMAGE_SET_Image(extVectorImage *Self, objImage *Value)
{
   if (!Value) {
      Self->Image = nullptr;
      Self->Bitmap = nullptr;
      return ERR::Okay;
   }

   if (!Value->Bitmap) return ERR::InvalidData;

   if (Value->Bitmap->BitsPerPixel < 32) {
      kt::Log().warning("The source image must be 32 bit, not %d bit.", Value->Bitmap->BitsPerPixel);
      return ERR::InvalidData;
   }

   Self->Image = Value;
   if (Value) Self->Bitmap = Value->Bitmap;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SpreadMethod: Defines image tiling behaviour, if desired.

The SpreadMethod defines the way in which the image is tiled within the target area if it is smaller than the
available space.  It is secondary to the application of #AspectRatio.  The default setting is `CLIP`, which prevents
the image from being tiled.

*********************************************************************************************************************/

static ERR IMAGE_SET_SpreadMethod(extVectorImage *Self, VSPREAD Value)
{
   Self->SpreadMethod = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Units: Declares the coordinate system to use for the #X and #Y values.

This field declares the coordinate system that is used for values in the #X and #Y fields.  The default is `BOUNDING_BOX`.

-FIELD-
X: Apply a horizontal offset to the image, the origin of which is determined by the #Units value.

*********************************************************************************************************************/

static ERR IMAGE_SET_X(extVectorImage *Self, Unit &Value)
{
   Self->X = Value;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: Apply a vertical offset to the image, the origin of which is determined by the #Units value.
-END-

*********************************************************************************************************************/

static ERR IMAGE_SET_Y(extVectorImage *Self, Unit &Value)
{
   Self->Y = Value;
   Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

static const ActionArray clImageActions[] = {
   { AC::Init,      IMAGE_Init },
   { AC::NewObject, IMAGE_NewObject },
   { AC::NIL, nullptr }
};

static const FieldDef clImageSpread[] = {
   { "Pad",      VSPREAD::PAD },
   { "Repeat",   VSPREAD::REPEAT },
   { "ReflectX", VSPREAD::REFLECT_X },
   { "ReflectY", VSPREAD::REFLECT_Y },
   { "Clip",     VSPREAD::CLIP },
   { nullptr, 0 }
};

static const FieldDef clImageUnits[] = {
   { "BoundingBox", VUNIT::BOUNDING_BOX }, // Coordinates are relative to the object's bounding box
   { "UserSpace",   VUNIT::USERSPACE },    // Coordinates are relative to the current viewport
   { nullptr, 0 }
};

static const FieldArray clImageFields[] = {
   { "X",            FDF_UNIT|FDF_RW, nullptr, IMAGE_SET_X },
   { "Y",            FDF_UNIT|FDF_RW, nullptr, IMAGE_SET_Y },
   { "Image",        FDF_OBJECT|FDF_RW, nullptr, IMAGE_SET_Image, CLASSID::IMAGE },
   { "Bitmap",       FDF_OBJECT|FDF_RW, nullptr, IMAGE_SET_Bitmap, CLASSID::BITMAP },
   { "Units",        FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clImageUnits },
   { "SpreadMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGE_SET_SpreadMethod, &clImageSpread },
   { "AspectRatio",  FDF_INTFLAGS|FDF_RW, nullptr, IMAGE_SET_AspectRatio, &clAspectRatio },
   END_FIELD
};

//********************************************************************************************************************

ERR init_image(void) // The gradient is a definition type for creating gradients and not drawing.
{
   clVectorImage = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORIMAGE),
      fl::Name("VectorImage"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clImageActions),
      fl::Fields(clImageFields),
      fl::Size(sizeof(extVectorImage)),
      fl::Path(MOD_PATH));

   return clVectorImage ? ERR::Okay : ERR::AddClass;
}
