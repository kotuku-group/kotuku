/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ImageFX: Renders a bitmap image in the effect pipeline.

The ImageFX class will render a source image into a given rectangle within the current user coordinate system.  The
client has the option of providing a pre-allocated @Bitmap or the path to a @Picture file as the source.

If a pre-allocated @Bitmap is to be used, it must be created under the ownership of the ImageFX object, and this must
be configured prior to initialisation.  It is required that the bitmap uses 32 bits per pixel and that the alpha
channel is enabled.

If a source picture file is referenced, it will be upscaled to meet the requirements automatically as needed.

Technically the ImageFX object is represented by a new viewport, the bounds of which are defined by attributes `X`,
`Y`, `Width` and `Height`.  The placement and scaling of the referenced image is controlled by the #AspectRatio field.

-END-

*********************************************************************************************************************/

class extImageFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::IMAGEFX;
   static constexpr CSTRING CLASS_NAME = "ImageFX";
   using create = pf::Create<extImageFX>;

   objBitmap *Bitmap;    // Bitmap containing source image data.
   objPicture *Picture;  // Origin picture if loading a source file.
   ARF  AspectRatio;     // Aspect ratio flags.
   VSM ResampleMethod;   // Resample method.
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR IMAGEFX_Draw(extImageFX *Self, struct acDraw *Args)
{
   render_to_filter(Self, Self->Bitmap, Self->AspectRatio, Self->ResampleMethod);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR IMAGEFX_Free(extImageFX *Self)
{
   if (Self->Picture) { FreeResource(Self->Picture); Self->Picture = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR IMAGEFX_Init(extImageFX *Self)
{
   pf::Log log;

   if (!Self->Bitmap) return log.warning(ERR::UndefinedField);

   return ERR::Okay;
}

//********************************************************************************************************************
// If the client attaches a bitmap as a child of our object, we use it as the primary image source.

static ERR IMAGEFX_NewChild(extImageFX *Self, struct acNewChild *Args)
{
   pf::Log log;

   if (Args->Object->classID() IS CLASSID::BITMAP) {
      if (!Self->Bitmap) {
         if (((objBitmap *)Args->Object)->BytesPerPixel IS 4) Self->Bitmap = (objBitmap *)Args->Object;
         else log.warning("Attached bitmap ignored; BPP of %d != 4", ((objBitmap *)Args->Object)->BytesPerPixel);
      }
      else log.warning("Attached bitmap ignored; Bitmap field already defined.");
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR IMAGEFX_NewObject(extImageFX *Self)
{
   Self->AspectRatio    = ARF::X_MID|ARF::Y_MID|ARF::MEET;
   Self->ResampleMethod = VSM::BILINEAR;
   Self->SourceType     = VSF::PREVIOUS;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

*********************************************************************************************************************/

static ERR IMAGEFX_GET_AspectRatio(extImageFX *Self, ARF *Value)
{
   *Value = Self->AspectRatio;
   return ERR::Okay;
}

static ERR IMAGEFX_SET_AspectRatio(extImageFX *Self, ARF Value)
{
   Self->AspectRatio = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Bitmap: The @Bitmap being used as the image source.

Reading the Bitmap field will return the @Bitmap that is being used as the image source.  Note that if a custom
Bitmap is to be used, the correct way to do this as to assign it to the ImageFX object via ownership rules.

If a picture image has been processed by setting the #Path, the Bitmap will refer to the content that has been
processed.

*********************************************************************************************************************/

static ERR IMAGEFX_GET_Bitmap(extImageFX *Self, objBitmap **Value)
{
   *Value = Self->Bitmap;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Path to an image file supported by the @Picture class.

*********************************************************************************************************************/

static ERR IMAGEFX_GET_Path(extImageFX *Self, CSTRING *Value)
{
   if (Self->Picture) return Self->Picture->get(FID_Path, *Value);
   else *Value = nullptr;
   return ERR::Okay;
}

static ERR IMAGEFX_SET_Path(extImageFX *Self, CSTRING Value)
{
   if ((Self->Bitmap) or (Self->Picture)) return ERR::Immutable;

   if ((Self->Picture = objPicture::create::local(fl::Path(Value), fl::BitsPerPixel(32), fl::Flags(PCF::FORCE_ALPHA_32)))) {
      Self->Bitmap = Self->Picture->Bitmap;
      return ERR::Okay;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: The resample algorithm to use for transforming the source image.

!VSM

*********************************************************************************************************************/

static ERR IMAGEFX_GET_ResampleMethod(extImageFX *Self, VSM *Value)
{
   *Value = Self->ResampleMethod;
   return ERR::Okay;
}

static ERR IMAGEFX_SET_ResampleMethod(extImageFX *Self, VSM Value)
{
   Self->ResampleMethod = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR IMAGEFX_GET_XMLDef(extImageFX *Self, STRING *Value)
{
   *Value = strclone("feImage");
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clResampleMethod[] = {
   { "Auto",      VSM::AUTO },
   { "Neighbour", VSM::NEIGHBOUR },
   { "Bilinear",  VSM::BILINEAR },
   { "Bicubic",   VSM::BICUBIC },
   { "Spline16",  VSM::SPLINE16 },
   { "Kaiser",    VSM::KAISER },
   { "Quadric",   VSM::QUADRIC },
   { "Gaussian",  VSM::GAUSSIAN },
   { "Bessel",    VSM::BESSEL },
   { "Mitchell",  VSM::MITCHELL },
   { "Sinc",      VSM::SINC },
   { "Lanczos",   VSM::LANCZOS },
   { "Blackman",  VSM::BLACKMAN },
   { nullptr, 0 }
};

#include "filter_image_def.c"

static const FieldArray clImageFXFields[] = {
   { "Bitmap",         FDF_VIRTUAL|FDF_OBJECT|FDF_R, IMAGEFX_GET_Bitmap, nullptr, CLASSID::BITMAP },
   { "Path",           FDF_VIRTUAL|FDF_STRING|FDF_RI, IMAGEFX_GET_Path, IMAGEFX_SET_Path },
   { "XMLDef",         FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, IMAGEFX_GET_XMLDef },
   { "AspectRatio",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, IMAGEFX_GET_AspectRatio, IMAGEFX_SET_AspectRatio, &clAspectRatio },
   { "ResampleMethod", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, IMAGEFX_GET_ResampleMethod, IMAGEFX_SET_ResampleMethod, &clResampleMethod },
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
