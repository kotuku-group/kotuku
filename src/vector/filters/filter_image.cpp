/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ImageFX: Renders a bitmap image in the effect pipeline.

The ImageFX class will render a source image into a given rectangle within the current user coordinate system.  The
client has the option of providing a pre-allocated @Bitmap or the path to a @Image file as the source.

If a pre-allocated @Bitmap is to be used, it must be owned by the ImageFX object before the ImageFX is initialised.
The bitmap can be created as a child of the uninitialised ImageFX, or an existing bitmap can be assigned to it with
~Core.SetOwner().  The bitmap must be initialised before the ImageFX, and it is required to use 32 bits per pixel with
the alpha channel enabled.  The bitmap is retained for the lifetime of the ImageFX; an attempt to free it earlier is
deferred until the ImageFX is freed.  Changes to the content of the bitmap are rendered on the next draw of the filter.
Call @Vector.Refresh() on the filtered vector to ensure that it is redrawn.

If a source image file is referenced, it will be upscaled to meet the requirements automatically as needed.

Technically the ImageFX object is represented by a new viewport, the bounds of which are defined by attributes `X`,
`Y`, `Width` and `Height`.  The placement and scaling of the referenced image is controlled by the #AspectRatio field.

-END-

*********************************************************************************************************************/

class extImageFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::IMAGEFX;
   static constexpr CSTRING CLASS_NAME = "ImageFX";
   using create = kt::Create<extImageFX>;

   ARF AspectRatio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // Aspect ratio flags.
   VSM ResampleMethod = VSM::BILINEAR; // Resample method.
   objBitmap *Bitmap;    // Bitmap containing source image data.
   objImage *Image;      // Origin image if loading a source file.
   bool BitmapPinned = false;  // True if the client's Bitmap holds a strong pin taken by Init()

   extImageFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::PREVIOUS;
   }

   ~extImageFX() {
      if (BitmapPinned) { Bitmap->unpin(true); BitmapPinned = false; }
      if (Image) FreeResource(Image);
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR IMAGEFX_Draw(extImageFX *Self, struct acDraw *Args)
{
   if (not active_filter_step(Self)) return ERR::InvalidState;
   return render_to_filter(Self, Self->Bitmap, Self->AspectRatio, Self->ResampleMethod);
}


//********************************************************************************************************************

static ERR IMAGEFX_Init(extImageFX *Self)
{
   kt::Log log;

   if (not Self->Bitmap) return log.warning(ERR::FieldNotSet);

   if (not Self->Bitmap->initialised()) {
      if (Self->BitmapPinned) { Self->Bitmap->unpin(); Self->BitmapPinned = false; }
      return log.warning(ERR::NotInitialised);
   }

   if (Self->Bitmap->BytesPerPixel != 4) {
      log.warning("The source bitmap #%d has %d bytes per pixel; 4 are required.", Self->Bitmap->UID,
         Self->Bitmap->BytesPerPixel);
      return ERR::InvalidValue;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// If the client attaches a bitmap as a child of our object, we use it as the primary image source.

static ERR IMAGEFX_NewChild(extImageFX *Self, struct acNewChild *Args)
{
   kt::Log log;
   if (Args->Object->classID() IS CLASSID::BITMAP) {
      if ((Self->initialised()) or (Self->Bitmap)) log.warning(ERR::AlreadyDefined);
      else if (Self->Bitmap) log.warning(ERR::AlreadyDefined);
      else {
         Self->Bitmap = (objBitmap *)Args->Object;
         Self->Bitmap->pin();
         Self->BitmapPinned = true;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

-FIELD-
Bitmap: The @Bitmap being used as the image source.

Reading the Bitmap field will return the @Bitmap that is being used as the image source.  A custom Bitmap is
assigned by making the ImageFX object its owner prior to initialisation, as described in the class overview.

If an image has been processed by setting the #Path, the Bitmap will refer to the content that has been
processed.

-FIELD-
Path: Path to an image file supported by the @Image class.

*********************************************************************************************************************/

static ERR IMAGEFX_GET_Path(extImageFX *Self, std::string_view &Value)
{
   if (Self->Image) return Self->Image->getPath(Value);
   else Value = std::string_view{};
   return ERR::Okay;
}

static ERR IMAGEFX_SET_Path(extImageFX *Self, const std::string_view &Value)
{
   if ((Self->Bitmap) or (Self->Image)) return ERR::Immutable;

   if ((Self->Image = objImage::create::local(fl::Path(Value), fl::BitsPerPixel(32), fl::Flags(PCF::FORCE_ALPHA_32)))) {
      Self->Bitmap = Self->Image->Bitmap;
      return ERR::Okay;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: The resample algorithm to use for transforming the source image.

!VSM

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR IMAGEFX_GET_XMLDef(extImageFX *Self, std::string &Value)
{
   std::stringstream stream;

   stream << "feImage";

   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_image_def.c"

static const FieldArray clImageFXFields[] = {
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clAspectRatio },
   { "ResampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clImageFXVSM },
   { "Bitmap",         FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "Path",           FDF_VIRTUAL|FDF_CPPSTRING|FDF_RI, IMAGEFX_GET_Path, IMAGEFX_SET_Path },
   { "XMLDef",         FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, IMAGEFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_imagefx(void)
{
   clImageFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::IMAGEFX),
      fl::Name("ImageFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clImageFXActions),
      fl::Fields(clImageFXFields),
      fl::Size(sizeof(extImageFX)),
      fl::Path(MOD_PATH));

   return clImageFX ? ERR::Okay : ERR::AddClass;
}
