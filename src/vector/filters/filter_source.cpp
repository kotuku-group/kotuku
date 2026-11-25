/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
SourceFX: Renders a source vector in the effect pipeline.

The SourceFX class will render a named vector into a given rectangle within the current user coordinate system.

Technically the SourceFX object is represented by a new viewport, the bounds of which are defined by the primitive
subregion: the `X`, `Y`, `Width` and `Height` fields of the effect.  The referenced vector is drawn at its own size,
with the origin of its user space at the top-left corner of the subregion, and it is clipped to the subregion.  The
viewport has no view box, so the #AspectRatio field does not scale the vector.  This is how web browsers place an
element referenced by the SVG `feImage` primitive.

If the subregion is not set, it defaults to the filter region.  The vector then moves with the filtered element: with
a filter region that starts at (40, 40), a vector point at (0, 0) is drawn at (40, 40).  Maps drawn by other routes,
such as a pattern fill, will not line up with it.  To draw the vector in the user space of the filtered element, set
`X` and `Y` to 0 and set `Width` and `Height` so that the subregion covers the filter region.  This applies when the
filter's primitive units are user space, which is the default.  In SVG:

<pre>
&lt;feImage href="#heightMap" x="0" y="0" width="1000" height="600"/&gt;
</pre>

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
   objVector *Source = nullptr; // The vector branch to render as source graphic.
   extVectorScene *Scene = nullptr;

   extSourceFX(objMetaClass *ClassPtr, OBJECTID ObjectID) : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
   }

   ~extSourceFX() {
      if (Scene) FreeResource(Scene);
      if (Source) Source->unpinWeak();
   }
};

// Descendants under a viewport already have the right nearest viewport.  Only the outer branch inherits the
// temporary viewport's dimensions while SourceFX is drawing it.

static void set_source_parent_view(extVector *Vector, extVectorViewport *View)
{
   Vector->ParentView = View;
   if (Vector->classID() IS CLASSID::VECTORVIEWPORT) return;

   for (auto child = (extVector *)Vector->Child; child; child = (extVector *)child->Next) {
      set_source_parent_view(child, View);
   }
}

//********************************************************************************************************************

/*********************************************************************************************************************
-ACTION-
Draw: Render the source vector to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR SOURCEFX_Draw(extSourceFX *Self, struct acDraw *Args)
{
   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   validate_object_link(Self->Source);
   if (not Self->Source) return ERR::Okay; // A missing vector produces transparent pixels.

   auto filter = Self->Filter;
   auto &render = *filter->Render;
   auto source = (extVector *)Self->Source;
   SharedObjectAccess access(source);
   if (not access.granted()) return access.error;

   auto scene = Self->Scene;
   const auto client_scene = render.ClientViewport->Scene;
   if ((client_scene->PageWidth != scene->PageWidth) or (client_scene->PageHeight != scene->PageHeight)) {
      if (auto error = acResize(scene, client_scene->PageWidth, client_scene->PageHeight, 0);
          error != ERR::Okay) return error;
   }

   // Use the current primitive subregion and filter transform to place the source.  A source subtree may contain
   // live paint servers or filters, so render it on every draw with the current parameters.
   auto viewport = (extVectorViewport *)scene->Viewport;
   const auto &sub = step->Subregion;
   if (auto error = scene->Viewport->setX(sub.left); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setY(sub.top); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setWidth(sub.width()); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setHeight(sub.height()); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setAspectRatio(Self->AspectRatio); error != ERR::Okay) return error;
   if (not active_filter_step(Self)) return ERR::InvalidState;

   auto saved_colour = std::exchange(viewport->ColourSpace, effect_colour_space(Self));
   auto saved_matrices = std::move(viewport->Matrices);
   auto saved_parent_view = source->ParentView;
   auto saved_parent = std::exchange(source->Parent, (objVector *)viewport);
   auto restore = kt::Defer([&]() {
      source->Parent = saved_parent;
      set_source_parent_view(source, saved_parent_view);
      viewport->Matrices = std::move(saved_matrices);
      viewport->ColourSpace = saved_colour;
      mark_dirty(source, RC::DIRTY);
   });

   const auto &t = render.Transform;
   auto &matrix = viewport->Matrices.emplace_back();
   matrix.Vector = viewport;
   matrix.ScaleX = t.sx;
   matrix.ShearY = t.shy;
   matrix.ShearX = t.shx;
   matrix.ScaleY = t.sy;
   matrix.TranslateX = t.tx;
   matrix.TranslateY = t.ty;
   mark_dirty(viewport, RC::TRANSFORM);
   set_source_parent_view(source, viewport);
   mark_children(source, RC::DIRTY);

   auto error = draw_filter_capture(filter, scene, source, Self->Target, 0, 0);
   if (error != ERR::Okay) return error;
   return source->collecting() ? ERR::InvalidState : ERR::Okay;
}

//********************************************************************************************************************

static ERR SOURCEFX_Init(extSourceFX *Self)
{
   kt::Log log;

   validate_object_link(Self->Source);
   if (!Self->Source) return log.warning(ERR::FieldNotSet);

   auto scene = extVectorScene::create::local(fl::Name("fx_src_scene"), fl::PageWidth(1), fl::PageHeight(1));
   if (not scene) return log.warning(ERR::CreateObject);
   if (not extVectorViewport::create::global(fl::Name("fx_src_viewport"), fl::Owner(scene->UID))) {
      FreeResource(scene);
      return log.warning(ERR::CreateObject);
   }
   Self->Scene = scene;

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
   effect_modified(Self);
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
   effect_modified(Self);
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

   if ((not Self->Filter) or (not Self->Filter->Scene)) return log.warning(ERR::FieldNotSet);

   if (Self->Source) {
      Self->Source->unpinWeak();
      Self->Source = nullptr;
   }

   OBJECTPTR def;
   if (!Self->Filter->Scene->findDef(Value.data(), &def)) {
      auto src = (objVector *)def;
      if (src->Class->BaseClassID != CLASSID::VECTOR) return log.warning(ERR::WrongClass);
      src->pinWeak();
      Self->Source = src;
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
   { "XMLDef",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, SOURCEFX_GET_XMLDef },
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
