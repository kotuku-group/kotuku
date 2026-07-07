/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
SourceFX: Renders a source vector in the effect pipeline.

The SourceFX class will render a named vector into a given rectangle within the current user coordinate system.

Technically the SourceFX object is represented by a new viewport, the bounds of which are defined by attributes `X`, `Y`,
`Width` and `Height`.  The placement and scaling of the referenced vector is controlled by the #AspectRatio field.

-END-

NOTE: This class exists to meet the needs of the SVG feImage element in a specific case where the href refers to a
registered vector rather than an image file.

*********************************************************************************************************************/

class extSourceFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SOURCEFX;
   static constexpr CSTRING CLASS_NAME = "SourceFX";
   using create = kt::Create<extSourceFX>;

   ARF  AspectRatio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // Aspect ratio flags.
   objBitmap *Bitmap = nullptr;     // Rendered image cache.
   objVector *Source = nullptr;     // The vector branch to render as source graphic.
   objVectorScene *Scene = nullptr; // Internal scene for rendering.
   uint8_t *BitmapData = nullptr;
   int  DataSize = 0;
   bool Render = true;              // Must be true if the bitmap cache needs to be rendered.

   extSourceFX(objMetaClass *ClassPtr, OBJECTID ObjectID) : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
      if ((Scene = objVectorScene::create::local(fl::Name("fx_src_scene"), fl::PageWidth(1), fl::PageHeight(1)))) {
         if (objVectorViewport::create::global(fl::Name("fx_src_viewport"), fl::Owner(Scene->UID))) {
            if ((Bitmap = objBitmap::create::local(fl::Name("fx_src_cache"),
                  fl::Width(1),
                  fl::Height(1),
                  fl::BitsPerPixel(32),
                  fl::Flags(BMF::ALPHA_CHANNEL|BMF::NO_DATA)))) {
            }
            else kt::Log().fatal(ERR::CreateObject);
         }
         else kt::Log().fatal(ERR::CreateObject);
      }
      else kt::Log().fatal(ERR::CreateObject);
   }

   ~extSourceFX() {
      if (Bitmap)     FreeResource(Bitmap);
      if (Source)     Source->unpinWeak();
      if (Scene)      FreeResource(Scene);
      if (BitmapData) FreeResource(BitmapData);
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the source vector to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR SOURCEFX_Draw(extSourceFX *Self, struct acDraw *Args)
{
   validate_object_link(Self->Source); // The source is weak-pinned; drop the link if it has been terminated.
   if (!Self->Source) return ERR::Okay;

   auto &filter = Self->Filter;

   // The configuration of the img values must be identical to the ImageFX code.

   double img_x = filter->TargetX;
   double img_y = filter->TargetY;
   double img_width = filter->TargetWidth;
   double img_height = filter->TargetHeight;

   if (filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      if (Self->X.defined()) img_x = trunc(filter->TargetX + (Self->X * filter->BoundWidth));
      if (Self->Y.defined()) img_y = trunc(filter->TargetY + (Self->Y * filter->BoundHeight));
      if (Self->Width.defined()) img_width = Self->Width * filter->BoundWidth;
      if (Self->Height.defined()) img_height = Self->Height * filter->BoundHeight;
   }
   else {
      if (Self->X.scaled()) img_x = filter->TargetX + (Self->X * filter->TargetWidth);
      else if (Self->X.defined()) img_x = Self->X;

      if (Self->Y.scaled()) img_y = filter->TargetY + (Self->Y * filter->TargetHeight);
      else if (Self->Y.defined()) img_y = Self->Y;

      if (Self->Width.scaled()) img_width = filter->TargetWidth * Self->Width;
      else if (Self->Width.defined()) img_width = Self->Width;

      if (Self->Height.scaled()) img_height = filter->TargetHeight * Self->Height;
      else if (Self->Height.defined()) img_height = Self->Height;
   }

   if ((filter->ClientViewport->Scene->PageWidth > Self->Scene->PageWidth) or
       (filter->ClientViewport->Scene->PageHeight > Self->Scene->PageHeight)) {
      acResize(Self->Scene, filter->ClientViewport->Scene->PageWidth, filter->ClientViewport->Scene->PageHeight, 0);
   }

   if ((filter->VectorClip.right > Self->Bitmap->Clip.Right) or
       (filter->VectorClip.bottom > Self->Bitmap->Clip.Bottom)) {
      acResize(Self->Bitmap, filter->ClientViewport->Scene->PageWidth, filter->ClientViewport->Scene->PageHeight, 0);
   }

   auto vp = (extVectorViewport *)Self->Scene->Viewport;
   if ((img_x != vp->vpViewX) or (img_y != vp->vpViewY) or (img_width != vp->vpViewWidth) or (img_height != vp->vpViewHeight)) {
      Self->Render = true;
   }

   if (Self->Render) {
      auto &cache = Self->Bitmap;
      cache->Clip = { filter->VectorClip.left, filter->VectorClip.top, filter->VectorClip.right, filter->VectorClip.bottom };

      // Manual data management - bitmap data is restricted to the clipping region.

      const int canvas_width  = cache->Clip.Right - cache->Clip.Left;
      const int canvas_height = cache->Clip.Bottom - cache->Clip.Top;
      cache->LineWidth = canvas_width * cache->BytesPerPixel;

      if ((Self->BitmapData) and (Self->DataSize < cache->LineWidth * canvas_height)) {
         FreeResource(Self->BitmapData);
         Self->BitmapData = nullptr;
         cache->Data = nullptr;
      }

      if (!cache->Data) {
         if (!AllocMemory(cache->LineWidth * canvas_height, MEM::DATA|MEM::NO_CLEAR, (APTR *)&Self->BitmapData)) {
            Self->DataSize = cache->LineWidth * canvas_height;
         }
         else return ERR::AllocMemory;
      }

      cache->Data = Self->BitmapData - (cache->Clip.Left * cache->BytesPerPixel) - (cache->Clip.Top * cache->LineWidth);

      Self->Scene->Viewport->setX(img_x);
      Self->Scene->Viewport->setY(img_y);
      Self->Scene->Viewport->setWidth(img_width);
      Self->Scene->Viewport->setHeight(img_height);
      Self->Scene->Viewport->setAspectRatio(Self->AspectRatio);

      agg::trans_affine &t = filter->ClientVector->Transform;
      auto viewport = (extVectorViewport *)Self->Scene->Viewport;

      // Temporarily inject the client's transform as the viewport's sole matrix for the duration of the render.

      viewport->Matrices.clear();
      auto &matrix = viewport->Matrices.emplace_back();
      matrix.Vector     = Self->Scene->Viewport;
      matrix.ScaleX     = t.sx;
      matrix.ShearY     = t.shy;
      matrix.ShearX     = t.shx;
      matrix.ScaleY     = t.sy;
      matrix.TranslateX = t.tx;
      matrix.TranslateY = t.ty;

      auto save_parent = Self->Source->Parent;
      auto const save_next = Self->Source->Next;
      Self->Scene->Viewport->Child = Self->Source;
      Self->Source->Parent = Self->Scene->Viewport;
      Self->Source->Next = nullptr;

      filter->Disabled = true; // Turning off the filter is required to prevent infinite recursion.

      mark_dirty(Self->Scene->Viewport, RC::TRANSFORM);

      Self->Scene->Bitmap = cache;
      gfx::DrawRectangle(cache, 0, 0, cache->Width, cache->Height, 0x00000000, BAF::FILL);
      acDraw(Self->Scene);

      filter->Disabled = false;
      Self->Scene->Viewport->Child = nullptr;
      Self->Source->Parent = save_parent;
      Self->Source->Next   = save_next;
      viewport->Matrices.clear();
      mark_dirty(Self->Source, RC::DIRTY);
   }

   gfx::CopyArea(Self->Bitmap, Self->Target, BAF::NIL, 0, 0, Self->Bitmap->Width, Self->Bitmap->Height, 0, 0);

   Self->Render = false;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SOURCEFX_Init(extSourceFX *Self)
{
   kt::Log log;

   validate_object_link(Self->Source);
   if (!Self->Source) return log.warning(ERR::UndefinedField);

   Self->Scene->Viewport->setColourSpace(Self->Filter->ColourSpace);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

*********************************************************************************************************************/

static ERR SOURCEFX_SET_AspectRatio(extSourceFX *Self, ARF Value)
{
   Self->AspectRatio = Value;
   Self->Render = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Source: The source @Vector that will be rendered.

The Source field must refer to a @Vector that will be rendered in the filter pipeline.  The vector must be under the
ownership of the same @VectorScene that the filter pipeline belongs.

*********************************************************************************************************************/

static ERR SOURCEFX_SET_Source(extSourceFX *Self, objVector *Value)
{
   kt::Log log;
   if (!Value) return log.warning(ERR::InvalidValue);
   if (Value->Class->BaseClassID != CLASSID::VECTOR) return log.warning(ERR::WrongClass);

   if (Self->Source) Self->Source->unpinWeak();
   Value->pinWeak();
   Self->Source = Value;
   Self->Render = true;
   return ERR::Okay;
}

static ERR SOURCEFX_GET_Source(extSourceFX *Self, objVector **Value)
{
   validate_object_link(Self->Source);
   *Value = Self->Source;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SourceName: Name of a source definition to be rendered.

Setting Def to the name of a pre-registered scene definition will reference that object in #Source.  If the name is
not registered then `ERR::Search` is returned.  The named object must be derived from the @Vector class.

Vectors are registered via the @VectorScene.AddDef() method.

*********************************************************************************************************************/

static ERR SOURCEFX_SET_SourceName(extSourceFX *Self, const std::string_view &Value)
{
   kt::Log log;

   if ((not Self->Filter) or (not Self->Filter->Scene)) return log.warning(ERR::UndefinedField);

   if (Self->Source) {
      Self->Source->unpinWeak();
      Self->Source = nullptr;
   }

   objVector *src;
   if (!Self->Filter->Scene->findDef(Value.data(), (OBJECTPTR *)&src)) {
      if (src->Class->BaseClassID != CLASSID::VECTOR) return log.warning(ERR::WrongClass);
      src->pinWeak();
      Self->Source = src;
      Self->Render = true;
      return ERR::Okay;
   }
   else return log.warning(ERR::Search);
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR SOURCEFX_GET_XMLDef(extSourceFX *Self, std::string &Value)
{
   Value = std::string("feImage");
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_source_def.c"

static const FieldArray clSourceFXFields[] = {
   { "AspectRatio", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SOURCEFX_SET_AspectRatio, &clAspectRatio },
   { "SourceName",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_I, nullptr, SOURCEFX_SET_SourceName },
   { "Source",      FDF_VIRTUAL|FDF_OBJECT|FDF_R|FDF_PURE, SOURCEFX_GET_Source, SOURCEFX_SET_Source, CLASSID::VECTOR },
   { "XMLDef",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R|FDF_PURE, SOURCEFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_sourcefx(void)
{
   clSourceFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::SOURCEFX),
      fl::Name("SourceFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clSourceFXActions),
      fl::Fields(clSourceFXFields),
      fl::Size(sizeof(extSourceFX)),
      fl::Path(MOD_PATH));

   return clSourceFX ? ERR::Okay : ERR::AddClass;
}
