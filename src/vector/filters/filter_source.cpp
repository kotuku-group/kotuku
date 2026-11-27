/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
SourceFX: Renders a source vector in the effect pipeline.

SourceFX renders a vector branch into the filter primitive subregion.  The branch is selected with #Source, or
with #SourceName for a definition registered in the scene.  The vector must belong to the same @VectorScene as the
filter.  Either field can be set at any time, including after initialisation, to replace the branch.

Technically the SourceFX object is represented by a new viewport, the bounds of which are defined by the primitive
subregion set by the `X`, `Y`, `Width` and `Height` fields.  By default the branch is drawn at its original size
with its origin at the top-left corner of the subregion, and it is clipped to the subregion.  The viewport has no
view box in this case, so the #AspectRatio field does not scale the vector.  This is how web browsers place an
element referenced by the SVG `feImage` primitive.

Set #ViewBox to render a section of the branch, such as one cell of a vector atlas.  The section, given in the
user coordinates of the source vector, becomes the view box of the viewport and is fitted to the subregion according
to #AspectRatio.  Artwork outside the section is clipped.

Changes to the source branch, including its destruction, are detected automatically and cause buffered viewports
that use the filter to be redrawn.

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
   view_box ViewBox { 0, 0, 0, 0 }; // Optional crop in source user space; zero size disables it.

   extSourceFX(objMetaClass *ClassPtr, OBJECTID ObjectID) : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
   }

   ~extSourceFX() {
      if (Scene) FreeResource(Scene);
      release_source();
   }

   void release_source() {
      validate_object_link(Source);
      if (auto source = (extVector *)std::exchange(Source, nullptr)) {
         source->FilterSourceRefs--;
         source->unpinWeak();
      }
   }
};

//********************************************************************************************************************
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
   if (source->Scene != filter->Scene) return ERR::Mismatch;

   auto scene = Self->Scene;
   const auto client_scene = render.ClientViewport->Scene;
   if ((client_scene->PageWidth != scene->PageWidth) or (client_scene->PageHeight != scene->PageHeight)) {
      if (auto error = acResize(scene, client_scene->PageWidth, client_scene->PageHeight, 0);
          error != ERR::Okay) return error;
   }

   // Use the current primitive subregion and filter transform to place the source.  A source subtree may contain
   // live paint servers or filters, so render it on every draw with the current parameters.

   auto viewport      = (extVectorViewport *)scene->Viewport;
   const auto &sub    = step->Subregion;
   const auto rect    = Self->ViewBox;
   const bool cropped = has_view_box(rect);
   const auto aspect  = Self->AspectRatio;
   auto transform     = render.Transform;
   double x = sub.left, y = sub.top, width = sub.width(), height = sub.height();

   if (cropped) {
      // For meet, clip the viewport to the fitted rectangle so artwork outside the crop cannot fill the margins.
      // For slice, the viewport retains the primitive's bounds and clips the scaled content to them.

      if ((aspect & ARF::MEET) != ARF::NIL) {
         double align_x, align_y, scale_x, scale_y;
         calc_aspectratio(__FUNCTION__, aspect, width, height, rect[VB_WIDTH], rect[VB_HEIGHT], align_x, align_y,
            scale_x, scale_y);
         x     += align_x;
         y     += align_y;
         width  = rect[VB_WIDTH] * scale_x;
         height = rect[VB_HEIGHT] * scale_y;
      }

      if ((not std::isfinite(width)) or (not std::isfinite(height)) or (width <= 0) or (height <= 0)) {
         return ERR::InvalidDimension;
      }

      transform.premultiply(agg::trans_affine_translation(x, y));
      x = y = 0;
   }

   if (auto error = scene->Viewport->setX(x); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setY(y); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setWidth(width); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setHeight(height); error != ERR::Okay) return error;
   if (auto error = scene->Viewport->setAspectRatio(aspect); error != ERR::Okay) return error;

   viewport->vpViewX      = rect[VB_X];
   viewport->vpViewY      = rect[VB_Y];
   viewport->vpViewWidth  = rect[VB_WIDTH];
   viewport->vpViewHeight = rect[VB_HEIGHT];
   viewport->vpOverflowX  = viewport->vpOverflowY = cropped ? VOF::HIDDEN : VOF::VISIBLE;
   viewport->vpClip       = false;

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
      mark_buffers_for_refresh(source, false);
      mark_children(source, RC::DIRTY);
   });

   auto &matrix = viewport->Matrices.emplace_back();
   matrix.Vector = viewport;
   matrix.ScaleX = transform.sx;
   matrix.ShearY = transform.shy;
   matrix.ShearX = transform.shx;
   matrix.ScaleY = transform.sy;
   matrix.TranslateX = transform.tx;
   matrix.TranslateY = transform.ty;
   mark_dirty(viewport, RC::TRANSFORM);
   set_source_parent_view(source, viewport);
   mark_children(source, RC::DIRTY);

   auto error = draw_filter_capture(nullptr, scene, source, Self->Target, 0, 0);
   if (error != ERR::Okay) return error;
   return source->collecting() ? ERR::InvalidState : ERR::Okay;
}

//********************************************************************************************************************

static ERR SOURCEFX_Init(extSourceFX *Self)
{
   kt::Log log;

   validate_object_link(Self->Source);
   if (Self->Source and (((extVector *)Self->Source)->Scene != Self->Filter->Scene)) {
      return log.warning(ERR::Mismatch);
   }

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

AspectRatio applies only when #ViewBox is set, and controls how the selected section is fitted to the primitive
subregion.  Without a #ViewBox the source vector is drawn at its original size.  The default is `X_MID|Y_MID|MEET`.

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

Set Source to select the @Vector branch that is rendered by the effect.  The field can be set before or after
initialisation.  The vector must belong to the same @VectorScene as the filter, or `ERR::Mismatch` is returned, and
a vector that is being destroyed is rejected with `ERR::MarkedForDeletion`.  The previous source remains active if
the new value is rejected.

The effect holds a weak reference to the vector, so it does not keep the vector alive.  Setting the field to null,
or destroying the vector, makes the effect produce transparent output.  Changes to the vector or its children cause
buffered viewports that use the filter to be redrawn.

*********************************************************************************************************************/

static ERR SOURCEFX_SET_Source(extSourceFX *Self, objVector *Value)
{
   if (Value) {
      if (Value->Class->BaseClassID != CLASSID::VECTOR) return ERR::WrongClass;
      if (Value->collecting()) return ERR::MarkedForDeletion;
      if (Self->Filter and (((extVector *)Value)->Scene != Self->Filter->Scene)) return ERR::Mismatch;
   }
   if (Value IS Self->Source) return ERR::Okay;

   Self->release_source();
   if (Value) {
      Value->pinWeak();
      ((extVector *)Value)->FilterSourceRefs++;
      Self->Source = Value;
   }
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

Setting SourceName looks up a definition registered with the filter's @VectorScene and assigns it to #Source.  The
field can be set before or after initialisation, but the effect must already belong to a @VectorFilter.  An unknown
name returns `ERR::Search`, and the definition must be derived from the @Vector class.  If the lookup or validation
fails, the previous source remains active.  Set an empty string to clear the source.

Vectors are registered via the @VectorScene.AddDef() method.

*********************************************************************************************************************/

static ERR SOURCEFX_SET_SourceName(extSourceFX *Self, const std::string_view &Value)
{
   kt::Log log;

   if ((not Self->Filter) or (not Self->Filter->Scene)) return log.warning(ERR::FieldNotSet);

   if (Value.empty()) return SOURCEFX_SET_Source(Self, nullptr);

   OBJECTPTR def;
   if (auto error = Self->Filter->Scene->findDef(Value, &def); error != ERR::Okay) return error;
   return SOURCEFX_SET_Source(Self, (objVector *)def);
}

/*********************************************************************************************************************

-FIELD-
ViewBox: Selects a section of the source vector, in its user coordinates.

By default the source vector is drawn at its original size.  Set ViewBox to four values, `{ x, y, width, height }`,
to render only a section of it, such as one cell of a vector atlas.  The section, given in the user coordinates of the
#Source vector, is fitted to the primitive subregion according to #AspectRatio, in the same way as the `viewBox` of an
SVG viewport.  Artwork outside the section is clipped, including in the margins that a `MEET` aspect ratio leaves.

The width and height must be greater than zero and all values must be finite, or `ERR::OutOfRange` is returned and
the current rectangle is retained.  An array that does not have four values is rejected with `ERR::InvalidValue`.

Set an empty array, or a zero width and height, to clear the rectangle.  Reading the field returns an empty array
when no rectangle is set.

In SVG, the equivalent attribute is `xvg:viewBox="x y width height"` on the `feImage` element.

*********************************************************************************************************************/

static ERR SOURCEFX_GET_ViewBox(extSourceFX *Self, std::span<double> &Array)
{
   Array = view_box_field(Self->ViewBox);
   return ERR::Okay;
}

static ERR SOURCEFX_SET_ViewBox(extSourceFX *Self, std::span<const double> &Array)
{
   view_box rect;
   if (auto error = read_view_box(Array, rect); error != ERR::Okay) return error;
   Self->ViewBox = rect;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Refresh: Redraws filter clients after an undetected change to the source.

Changes made through the fields and actions of the #Source vector are detected automatically.  Call Refresh after a
change that the vector cannot detect, such as new content in a bitmap that one of its fills refers to, so that
buffered viewports that use the filter discard their cached content.

Refresh does not schedule drawing.
-END-

*********************************************************************************************************************/

static ERR SOURCEFX_Refresh(extSourceFX *Self, APTR Void)
{
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR source_xml(const extSourceFX &Effect, filter_xml_element &Element)
{
   if ((not Effect.Source) or Effect.Source->collecting() or
       (not Effect.Filter) or (not Effect.Filter->Scene)) return ERR::NoSupport;

   // Select a stable ID when the source has multiple aliases in the scene.

   std::string_view reference;
   for (const auto &[name, def] : Effect.Filter->Scene->Defs) {
      if ((def IS Effect.Source) and (not name.empty()) and (reference.empty() or (name < reference))) {
         reference = name;
      }
   }

   if (reference.empty()) return ERR::NoSupport;
   Element.Name = "feImage";
   Element.attribute("xlink:href", "#" + std::string(reference));
   write_filter_xml_aspect(Element, Effect.AspectRatio);
   write_filter_xml_view_box(Element, Effect.ViewBox);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_source_def.c"

static const FieldArray clSourceFXFields[] = {
   { "AspectRatio", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SOURCEFX_SET_AspectRatio, &clAspectRatio },
   { "SourceName",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_W, nullptr, SOURCEFX_SET_SourceName },
   { "Source",      FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, SOURCEFX_GET_Source, SOURCEFX_SET_Source, CLASSID::VECTOR },
   { "ViewBox",     FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW|FDF_PURE, SOURCEFX_GET_ViewBox, SOURCEFX_SET_ViewBox },
   { "XMLDef",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
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
