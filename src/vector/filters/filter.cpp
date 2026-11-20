/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of filter definitions only.

-CLASS-
VectorFilter: Constructs filter pipelines that alter rendered vector graphics.

The VectorFilter class allows post-effect filters to be applied to vectors as they are being rendered.  Filter
support is closely modelled around the SVG standard, and effect results are intended to match that of the standard.
Once created, a filter can be utilised by vector objects through their @Vector.Filter field.  By way of example in
SVG:

<pre>
&lt;circle cx="160" cy="50" r="40" fill="#f00" filter="url(&#35;EffectPipeline)"/&gt;
</pre>

Filter pipelines are constructed from effects based on the @FilterEffect class, such as @CompositeFX and @FloodFX.
Construct a new pipeline by creating effect objects and placing them under the ownership of the VectorFilter that
will be supporting them.  The VectorFilter will detect the filter objects and they will be processed in the order
in which they are added.  It is most optimal to create each pipeline in advance, and a new VectorFilter object should
be created for each pipeline as necessary.

Each reference must name an earlier effect owned by the same filter.  Before drawing the pipeline, the renderer
validates the inputs and effect order.  If validation fails, it logs the affected effect and omits the filter's
output.  The last effect supplies the rendered result.  Use @MergeFX to combine multiple branches.

The filter's output is composited into the target with the blend mode resolved for the client vector, as described
for @Vector.ColourSpace, and the filter's #Opacity is applied at the same time.  The filter's own #ColourSpace
governs effect processing only and does not affect how the output is composited.

Effects can read content that lies beyond the edges of the page, within the filter region, so a filtered vector that
touches or crosses a page edge is processed as if the page were larger.  Samples outside of the filter region are
transparent black.  The output is clipped to the page and any clipping imposed by container viewports.

It is important to note that filter effects are CPU intensive tasks and real-time performance may be disappointing.
If this is an issue, consider pre-rendering the filter effects in advance and caching the results in memory or files.

It is a requirement that VectorFilter objects are owned by the @VectorScene they are targeting.

-END-

*********************************************************************************************************************/

#include <bs_thread_pool.h>
#include "filter_sampling.h"

//#define DEBUG_FILTER_BITMAP
//#define EXPORT_FILTER_BITMAP

constexpr int64_t MAX_FILTER_MARGIN_AREA = 4096 * 4096; // Processing regions with margins are limited to this area

enum class filter_input_role { INPUT, MIX, MERGE };
enum class filter_inputs { GENERATOR, SINGLE, DUAL, MERGE };
enum class filter_input_kind { IMAGE, PARAMETER_MAP };
enum class filter_access { POINTWISE, BOUNDED, FULL_INPUT };

struct filter_sampling {
   static constexpr double UNBOUNDED = std::numeric_limits<double>::infinity();
   filter_access Access = filter_access::POINTWISE;
   double X = 0, Y = 0; // Conservative device-pixel reach.  Full input may apply along only one axis.
};

struct filter_operation {
   filter_inputs Inputs;
   filter_sampling (*Sampling)(const extFilterEffect *) = [](const extFilterEffect *) -> filter_sampling {
      return {};
   };
   filter_input_kind MixKind = filter_input_kind::IMAGE;
   bool MapContributesRegion = false;
};

enum class filter_alpha { STRAIGHT, PREMULTIPLIED };

// RGBA8 in the bitmap's channel order.  Valid is a half-open rectangle in bitmap coordinates and Origin
// locates (0,0) in the scene.  Every pixel in Valid is initialised, including transparent padding.
struct filter_surface_view {
   const objBitmap *Bitmap = nullptr; // Adapter for bitmap-based readers.
   const uint8_t *Data = nullptr;
   int Width = 0, Height = 0, Stride = 0;
   int OriginX = 0, OriginY = 0;
   CS Encoding = CS::SRGB;
   filter_alpha Alpha = filter_alpha::STRAIGHT;
   TClipRectangle<int> Valid = { 0 };

   const uint8_t * row(int Y) const { return Data + std::ptrdiff_t(Y) * Stride; }
};

struct filter_step;

struct filter_input {
   VSF Source;
   filter_input_role Role;
   size_t Slot;
   const filter_step *Producer;
   filter_input_kind Kind;
   bool MapContributesRegion;
};

struct filter_step {
   extFilterEffect *Effect;
   filter_operation Operation;
   std::span<const filter_input> Inputs;
   TClipRectangle<double> Subregion = { 0.0 }; // Client user coordinates.
   size_t Consumers = 0;
   filter_surface_view Result = {};
};

struct filter_plan {
   std::vector<filter_step> Steps;
   std::vector<filter_input> Inputs;
   const filter_step *Final = nullptr;
   bool ReadsSourceGraphic = false;
   bool ReadsBackground = false;
};

struct filter_render_context {
   extVector *ClientVector;
   extVectorViewport *ClientViewport;
   objBitmap *BkgdBitmap;
   int BkgdOriginX, BkgdOriginY;
   VCS ClientColourSpace;
   uint64_t Revision;
   filter_plan Plan;
   const filter_step *ActiveStep = nullptr;
   TClipRectangle<int> VectorClip = { 0 }; // Processing rectangle in scene pixels.  Left/top are the bitmap origin.
   TClipRectangle<double> ClientBounds = { 0.0 }; // Unrounded bounds of the subtree in client user coordinates.
   TClipRectangle<double> ClientPathBounds = { 0.0 }; // Client path alone in user coordinates.
   TClipRectangle<double> SceneBounds = { 0.0 }; // Unrounded subtree bounds in scene coordinates for per-axis units.
   TClipRectangle<double> FilterRegion = { 0.0 }; // Default primitive region in client user coordinates.
   double BoundWidth = 0, BoundHeight = 0; // Rounded subtree dimensions used by legacy distance scaling.
   double ViewportWidth = 0, ViewportHeight = 0; // Viewport dimensions in its user coordinates (viewBox if set).
   double ContainerWidth = 0, ContainerHeight = 0; // Fixed viewport dimensions used by legacy filter region lengths.
   VUNIT FilterUnits, PrimitiveUnits;
   VFA AspectRatio;
   agg::trans_affine ToScene; // Client user coordinates to scene coordinates.
   agg::trans_affine Transform; // Client user coordinates to local bitmap coordinates.
   agg::trans_affine Inverse; // Local bitmap coordinates to client user coordinates.
   uint8_t BankIndex = 0;
   filter_surface_view SourceGraphic, SourceAlpha, Background, BackgroundAlpha;

   TClipRectangle<int> scratch_clip() const { return { 0, 0, VectorClip.width(), VectorClip.height() }; }

   std::pair<double, double> primitive_scales(bool AllowDiagonal) const {
      if (PrimitiveUnits != VUNIT::BOUNDING_BOX) return { 1, 1 };
      if (AllowDiagonal and (AspectRatio IS VFA::NONE)) {
         const auto diagonal = normalised_diagonal(BoundWidth, BoundHeight);
         return { diagonal, diagonal };
      }
      return { BoundWidth, BoundHeight };
   }

   static double normalised_diagonal(double Width, double Height) {
      return sqrt((Width * Width) + (Height * Height)) * INV_SQRT2;
   }

   std::pair<double, double> axis_scales() const {
      return { std::hypot(ToScene.sx, ToScene.shy), std::hypot(ToScene.shx, ToScene.sy) };
   }
};

static bool finite_transform(const agg::trans_affine &Transform)
{
   return std::isfinite(Transform.sx) and std::isfinite(Transform.sy) and
      std::isfinite(Transform.shx) and std::isfinite(Transform.shy) and
      std::isfinite(Transform.tx) and std::isfinite(Transform.ty);
}

static bool invert_filter_transform(agg::trans_affine &Transform)
{
   const double determinant = Transform.determinant();
   if ((not finite_transform(Transform)) or (not std::isfinite(determinant)) or (determinant IS 0)) return false;
   Transform.invert();
   return finite_transform(Transform);
}

static bool finite_region(const TClipRectangle<double> &Region)
{
   return std::isfinite(Region.left) and std::isfinite(Region.top) and
      std::isfinite(Region.right) and std::isfinite(Region.bottom) and
      std::isfinite(Region.width()) and std::isfinite(Region.height());
}

static bool transform_filter_region(TClipRectangle<double> &, const agg::trans_affine &);

static bool filter_raster_bounds(const TClipRectangle<double> &Bounds)
{
   // Reserve headroom for additions and differences in the rasteriser's fixed-point arithmetic.
   constexpr double limit = double(agg::poly_max_coord) / agg::poly_subpixel_scale;
   return (Bounds.left >= -limit) and (Bounds.top >= -limit) and
      (Bounds.right <= limit) and (Bounds.bottom <= limit);
}

static void map_filter_pixel_centre(const agg::trans_affine &Transform, int X, int Y, double &PX, double &PY)
{
   PX = filter_pixel_centre(X);
   PY = filter_pixel_centre(Y);
   Transform.transform(&PX, &PY);
}

static filter_rgba_sampler filter_sampler(const objBitmap *Bitmap)
{
   const auto &clip = Bitmap->Clip;
   return { Bitmap->Data, Bitmap->LineWidth, clip.Left, clip.Top, clip.Right, clip.Bottom };
}

static filter_surface_view filter_view(const filter_render_context &Render, const objBitmap *Bitmap)
{
   return { Bitmap, Bitmap->Data, Bitmap->Width, Bitmap->Height, Bitmap->LineWidth,
      Render.VectorClip.left, Render.VectorClip.top, Bitmap->ColourSpace,
      ((Bitmap->Flags & BMF::PREMUL) != BMF::NIL) ? filter_alpha::PREMULTIPLIED : filter_alpha::STRAIGHT,
      { Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right, Bitmap->Clip.Bottom } };
}

using filter_positions = std::unordered_map<objFilterEffect *, size_t>;

static ERR get_source_view(extFilterEffect *, filter_surface_view &, filter_input_role, size_t = 0);
static ERR get_source_bitmap(extFilterEffect *, const objBitmap **, filter_input_role, size_t = 0);
static ERR get_source_graphic(extVectorFilter *, filter_surface_view &);
static ERR premultiplied_input(extFilterEffect *, const filter_surface_view &, CS, filter_surface_view &);
static ERR draw_filter_capture(extVectorFilter *, extVectorScene *, extVector *, objBitmap *, int, int);
static ERR get_banked_bitmap(extVectorFilter *, objBitmap **);
static void unlink_filter_effect(extFilterEffect *);
static void clear_filter_references(extFilterEffect *, objFilterEffect * = nullptr);
static void update_filter_links(extVectorFilter *);
static ERR check_filter_reference(extFilterEffect *, objFilterEffect *, filter_input_role, size_t = 0,
   const filter_positions * = nullptr);

static const filter_step * active_filter_step(const extFilterEffect *Effect)
{
   const auto filter = Effect->Filter;
   const auto render = filter ? filter->Render : nullptr;
   if ((not render) or (not render->ActiveStep) or (render->ActiveStep->Effect != Effect) or
       (filter->GraphRevision != render->Revision) or filter->collecting()) return nullptr;
   return render->ActiveStep;
}

// The display blitter takes a mutable source object but only reads from it.

static ERR copy_filter_bitmap(const objBitmap *Source, objBitmap *Dest, BAF Flags,
   int X, int Y, int Width, int Height, int DestX, int DestY)
{
   return gfx::CopyArea(const_cast<objBitmap *>(Source), Dest, Flags, X, Y, Width, Height, DestX, DestY);
}

//********************************************************************************************************************

static void render_bilinear_upscale(objBitmap &Bitmap, objBitmap &Target, const agg::trans_affine &Transform,
   const agg::image_filter_lut &Filter)
{
   const int left = Target.Clip.Left, top = Target.Clip.Top, bottom = Target.Clip.Bottom;
   const int width = Target.Clip.Right - left, height = bottom - top;

   // The destination rectangle covers every pixel in the clip.  Generate one full span per row, as the
   // rasteriser does, so the sampler keeps the same interpolation endpoints and edge behaviour.
   auto render_rows = [&](int StartY, int EndY) {
      agg::pixfmt_psl pix_source(Bitmap), pix_dest(Target);
      agg::span_interpolator_linear<> interpolator(Transform);
      agg::span_once<agg::pixfmt_psl> source(pix_source, 0, 0);
      agg::span_image_filter_rgba<decltype(source), decltype(interpolator)> sampler(source, interpolator, Filter, false);
      agg::span_allocator<agg::rgba8> span_alloc;
      auto colours = span_alloc.allocate(width);

      for (int y=StartY; y < EndY; y++) {
         sampler.generate(colours, left, y, width);
         pix_dest.copy_color_hspan(left, y, width, colours);
      }
   };

   constexpr size_t MIN_PIXELS_PER_WORKER = 32 * 1024;
   constexpr int MIN_ROWS_PER_WORKER = 4;
   const size_t thread_count = std::min({ size_t(std::thread::hardware_concurrency()),
      size_t(height / MIN_ROWS_PER_WORKER), (size_t(width) * height) / MIN_PIXELS_PER_WORKER });
   if (thread_count < 2) render_rows(top, bottom);
   else {
      BS::thread_pool pool(thread_count);
      pool.detach_blocks(top, bottom, render_rows);
      pool.wait();
   }
}

//********************************************************************************************************************
// Universal function for rendering a filter's Bitmap to the target region.
//
// No blending is performed because this is intended for use when there is no former input.  Copying is done
// with only the transforms applied (if any).  Linear RGB interpolation will wait until post processing.

static ERR render_to_filter(extFilterEffect *Self, objBitmap *Bitmap, ARF AspectRatio, VSM SampleMethod)
{
   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const auto &subregion = step->Subregion;

   // The image is fitted to the primitive subregion, which render_filter() computes before drawing the effect.  In
   // objectBoundingBox mode the subregion is measured against the bounding box of the client vector, as per SVG.

   double p_x = std::round(subregion.left), p_y = std::round(subregion.top);
   double p_width = std::round(subregion.width()), p_height = std::round(subregion.height());

   if ((p_width <= 0) or (p_height <= 0)) return ERR::Okay;

   double x_scale = 1, y_scale = 1, align_x = 0, align_y = 0;
   calc_aspectratio("align_filter", AspectRatio, p_width, p_height, Bitmap->Width, Bitmap->Height, align_x, align_y, x_scale, y_scale);

   p_x += std::round(align_x);
   p_y += std::round(align_y);

   agg::trans_affine img_transform;
   img_transform.scale(x_scale, y_scale);
   img_transform.translate(p_x, p_y);
   img_transform *= Self->Filter->Render->Transform;
   if (not invert_filter_transform(img_transform)) return ERR::InvalidValue;
   const auto &clip = Self->Target->Clip;
   TClipRectangle<double> samples { filter_pixel_centre(clip.Left), filter_pixel_centre(clip.Top),
      filter_pixel_centre(clip.Right), filter_pixel_centre(clip.Bottom) };
   if ((not transform_filter_region(samples, img_transform)) or (not filter_raster_bounds(samples))) {
      return ERR::OutOfRange;
   }

   if (img_transform.is_complex()) {
      // The transform maps destination pixels back to the source, so upscale factors are at most one.
      if (((SampleMethod IS VSM::BILINEAR) or (SampleMethod IS VSM::AUTO)) and
          (img_transform.shx IS 0.0) and (img_transform.shy IS 0.0) and
          (img_transform.sx > 0.0) and (img_transform.sx <= 1.0) and
          (img_transform.sy > 0.0) and (img_transform.sy <= 1.0)) {
         render_bilinear_upscale(*Bitmap, *Self->Target, img_transform, get_filter(SampleMethod, img_transform));
         return ERR::Okay;
      }

      agg::rasterizer_scanline_aa<> raster;
      agg::renderer_base<agg::pixfmt_psl> renderBase;
      agg::pixfmt_psl pixDest(*Self->Target);
      agg::pixfmt_psl pixSource(*Bitmap);

      renderBase.attach(pixDest);
      renderBase.clip_box(Self->Target->Clip.Left, Self->Target->Clip.Top, Self->Target->Clip.Right-1, Self->Target->Clip.Bottom-1);

      agg::span_interpolator_linear<> interpolator(img_transform);
      agg::span_once<agg::pixfmt_psl> source(pixSource, 0, 0);

      set_raster_rect_path(raster, Self->Target->Clip.Left, Self->Target->Clip.Top,
         Self->Target->Clip.Right - Self->Target->Clip.Left,
         Self->Target->Clip.Bottom - Self->Target->Clip.Top);

      if (SampleMethod IS VSM::NEIGHBOUR) {
         agg::span_image_filter_rgba_nn<agg::span_once<agg::pixfmt_psl>, agg::span_interpolator_linear<>>
            spangen(source, interpolator);
         renderSolidBitmap(renderBase, raster, spangen); // Solid render without blending.
      }
      else {
         const agg::image_filter_lut &ifilter = get_filter(SampleMethod, img_transform);
         agg::span_image_filter_rgba<agg::span_once<agg::pixfmt_psl>, agg::span_interpolator_linear<>>
            spangen(source, interpolator, ifilter, false);
         renderSolidBitmap(renderBase, raster, spangen); // Solid render without blending.
      }
   }
   else return gfx::CopyArea(Bitmap, Self->Target, BAF::NIL, 0, 0, Bitmap->Width, Bitmap->Height,
      -img_transform.tx, -img_transform.ty);

   return ERR::Okay;
}

//********************************************************************************************************************
// SSE2 detection is shared via link/simd.h.  Filter effects use FILTER_SSE2 to guard their SIMD rendering
// paths, falling back to scalar code on other architectures.

#include "../../link/simd.h"

#ifdef KOTUKU_SSE2
   #define FILTER_SSE2 1
#endif

//********************************************************************************************************************

#include "filter_effect.cpp"
#include "filter_blur.cpp"
#include "filter_colourmatrix.cpp"
#include "filter_composite.cpp"
#include "filter_convolve.cpp"
#include "filter_displacement.cpp"
#include "filter_flood.cpp"
#include "filter_image.cpp"
#include "filter_lighting.cpp"
#include "filter_merge.cpp"
#include "filter_morphology.cpp"
#include "filter_offset.cpp"
#include "filter_remap.cpp"
#include "filter_source.cpp"
#include "filter_turbulence.cpp"
#include "filter_wavefunction.cpp"

//********************************************************************************************************************

static ERR filter_input_error(const extFilterEffect *Effect, filter_input_role Role, size_t Slot, ERR Error,
   CSTRING Reason, OBJECTID Producer = 0)
{
   kt::Log log("FilterPlan");
   const auto class_name = Effect->Class->ClassName.c_str();
   const auto field = (Role IS filter_input_role::MIX) ? "Mix" :
      ((Role IS filter_input_role::MERGE) ? "SourceList" : "Input");
   if (Role IS filter_input_role::MERGE) {
      log.warning("%s #%d %s[%zu]: %s (producer #%d).", class_name, Effect->UID, field, Slot, Reason,
         Producer);
   }
   else log.warning("%s #%d %s: %s (producer #%d).", class_name, Effect->UID, field, Reason, Producer);
   return Error;
}

static ERR check_filter_reference(extFilterEffect *Effect, objFilterEffect *Producer, filter_input_role Role,
   size_t Slot, const filter_positions *Positions)
{
   if (not Producer) {
      return filter_input_error(Effect, Role, Slot, ERR::FieldNotSet, "missing reference");
   }
   if (Producer IS Effect) {
      return filter_input_error(Effect, Role, Slot, ERR::Recursion, "self-reference", Effect->UID);
   }
   const auto filter = Effect->Filter;
   const bool member = filter and (Positions ? Positions->contains(Producer) :
      std::ranges::find(filter->Effects, Producer) != filter->Effects.end());
   if (not member) {
      return filter_input_error(Effect, Role, Slot, ERR::InvalidValue, "reference must name a live sibling");
   }
   if ((Producer->Owner != filter) or Producer->collecting()) {
      return filter_input_error(Effect, Role, Slot, ERR::InvalidValue, "producer is no longer a live sibling",
         Producer->UID);
   }
   return ERR::Okay;
}

static void clear_filter_references(extFilterEffect *Effect, objFilterEffect *Producer)
{
   // Also detach dormant references.  A later SourceType or MixType write can reactivate them.
   if ((not Producer) or (Effect->Input IS Producer)) Effect->Input = nullptr;
   if ((not Producer) or (Effect->Mix IS Producer)) Effect->Mix = nullptr;
   if (Effect->classID() IS CLASSID::MERGEFX) {
      for (auto &source : ((extMergeFX *)Effect)->List) {
         if ((not Producer) or (source.Effect IS Producer)) source.Effect = nullptr;
      }
   }
}

static void update_filter_links(extVectorFilter *Filter)
{
   auto &effects = Filter->Effects;
   for (size_t i = 0; i < effects.size(); i++) {
      effects[i]->Prev = i ? effects[i - 1] : nullptr;
      effects[i]->Next = (i + 1 < effects.size()) ? effects[i + 1] : nullptr;
   }
   Filter->GraphRevision++;
}

static void unlink_filter_effect(extFilterEffect *Effect)
{
   auto filter = Effect->Filter;
   if (not filter) return;

   std::erase(filter->Effects, Effect);
   for (auto consumer : filter->Effects) clear_filter_references(consumer, Effect);
   Effect->Filter = nullptr;
   Effect->Prev = Effect->Next = nullptr;
   Effect->Target = nullptr;
   update_filter_links(filter);
}

extFilterEffect::~extFilterEffect()
{
   unlink_filter_effect(this);
}

//********************************************************************************************************************

template<class Effect>
static filter_sampling effect_sampling(const extFilterEffect *Instance)
{
   return ((const Effect *)Instance)->sampling();
}

static filter_operation describe_filter_operation(CLASSID Class)
{
   using enum filter_inputs;
   switch (Class) {
      case CLASSID::BLURFX:         return { SINGLE, effect_sampling<extBlurFX> };
      case CLASSID::COLOURFX:       return { SINGLE };
      case CLASSID::COMPOSITEFX:    return { DUAL };
      case CLASSID::CONVOLVEFX:     return { SINGLE, effect_sampling<extConvolveFX> };
      case CLASSID::DISPLACEMENTFX:
         // The legacy SVG Mix slot contributes to the default region even though it contains a parameter map.
         return { DUAL, effect_sampling<extDisplacementFX>, filter_input_kind::PARAMETER_MAP, true };
      case CLASSID::FLOODFX:        return { GENERATOR };
      case CLASSID::IMAGEFX:        return { GENERATOR };
      case CLASSID::LIGHTINGFX:     return { SINGLE, effect_sampling<extLightingFX> };
      case CLASSID::MERGEFX:        return { MERGE };
      case CLASSID::MORPHOLOGYFX:   return { SINGLE, effect_sampling<extMorphologyFX> };
      case CLASSID::OFFSETFX:       return { SINGLE, effect_sampling<extOffsetFX> };
      case CLASSID::REMAPFX:        return { SINGLE };
      case CLASSID::SOURCEFX:       return { GENERATOR };
      case CLASSID::TURBULENCEFX:   return { GENERATOR };
      case CLASSID::WAVEFUNCTIONFX: return { GENERATOR };
      default:
         return { SINGLE, [](const extFilterEffect *) -> filter_sampling {
            return { filter_access::FULL_INPUT, filter_sampling::UNBOUNDED, filter_sampling::UNBOUNDED };
         } };
   }
}

// Any cycle includes a forward edge.  Walk the dependencies only when the authored order is already invalid,
// so successful draws do not need a second graph pass.

static ERR diagnose_filter_order(const filter_plan &Plan, const extFilterEffect *Consumer, const filter_input &Forward)
{
   enum class visit { UNSEEN, ACTIVE, DONE };
   struct frame {
      const filter_step *Step;
      size_t NextInput = 0;
   };

   const auto first = Plan.Steps.data();
   std::vector<visit> visited(Plan.Steps.size(), visit::UNSEEN);
   std::vector<frame> stack;
   for (const auto &step : Plan.Steps) {
      if (visited[&step - first] != visit::UNSEEN) continue;
      visited[&step - first] = visit::ACTIVE;
      stack.push_back({ &step });

      while (not stack.empty()) {
         auto &current = stack.back();
         if (current.NextInput IS current.Step->Inputs.size()) {
            visited[current.Step - first] = visit::DONE;
            stack.pop_back();
            continue;
         }
         const auto &input = current.Step->Inputs[current.NextInput++];
         if (not input.Producer) continue;
         auto &state = visited[input.Producer - first];
         if (state IS visit::ACTIVE) {
            return filter_input_error(current.Step->Effect, input.Role, input.Slot, ERR::Recursion,
               "dependency cycle", input.Producer->Effect->UID);
         }
         if (state IS visit::UNSEEN) {
            state = visit::ACTIVE;
            stack.push_back({ input.Producer });
         }
      }
   }
   return filter_input_error(Consumer, Forward.Role, Forward.Slot, ERR::InvalidValue,
      "producer must precede its consumer", Forward.Producer->Effect->UID);
}

static ERR prepare_filter_plan(extVectorFilter *Filter, filter_plan &Plan)
{
   kt::Log log("FilterPlan");
   const auto &effects = Filter->Effects;
   filter_positions positions;
   positions.reserve(effects.size());
   Plan.Steps.reserve(effects.size());
   size_t input_count = 0;

   for (size_t i = 0; i < effects.size(); i++) {
      auto effect = effects[i];
      if ((effect->Filter != Filter) or (effect->Owner != Filter) or effect->collecting() or
          (not effect->initialised())) {
         log.warning("%s #%d is not an initialised, live child of filter #%d.", effect->className(), effect->UID,
            Filter->UID);
         return ERR::InvalidState;
      }
      const auto prev = i ? effects[i - 1] : nullptr;
      const auto next = (i + 1 < effects.size()) ? effects[i + 1] : nullptr;
      if ((effect->Prev != prev) or (effect->Next != next)) {
         log.warning("%s #%d has an invalid %s link.", effect->className(), effect->UID,
            (effect->Prev != prev) ? "Prev" : "Next");
         return ERR::InvalidState;
      }

      positions.emplace(effect, i);
      const auto operation = describe_filter_operation(effect->classID());
      Plan.Steps.push_back({ .Effect = effect, .Operation = operation, .Inputs = {} });
      switch (operation.Inputs) {
         case filter_inputs::GENERATOR: break;
         case filter_inputs::SINGLE: input_count++; break;
         case filter_inputs::DUAL: input_count += 2; break;
         case filter_inputs::MERGE: input_count += ((extMergeFX *)effect)->List.size(); break;
      }
   }

   Plan.Inputs.reserve(input_count);
   Plan.Final = &Plan.Steps.back();
   const extFilterEffect *forward_consumer = nullptr;
   const filter_input *forward_input = nullptr;

   for (size_t i = 0; i < Plan.Steps.size(); i++) {
      auto &step = Plan.Steps[i];
      auto effect = step.Effect;
      const auto &operation = step.Operation;
      const size_t first = Plan.Inputs.size();

      auto add_input = [&](VSF Type, objFilterEffect *Reference, filter_input_role Role, size_t Slot = 0) -> ERR {
         filter_step *producer = nullptr;
         switch (Type) {
            case VSF::REFERENCE: {
               if (auto error = check_filter_reference(effect, Reference, Role, Slot, &positions);
                   error != ERR::Okay) {
                  return error;
               }
               producer = &Plan.Steps[positions.find(Reference)->second];
               producer->Consumers++;
               break;
            }
            case VSF::GRAPHIC:
            case VSF::ALPHA:
               Plan.ReadsSourceGraphic = true;
               break;
            case VSF::BKGD:
            case VSF::BKGD_ALPHA:
               Plan.ReadsBackground = true;
               break;
            case VSF::NONE:
               if (Role IS filter_input_role::MERGE) break;
               return filter_input_error(effect, Role, Slot, ERR::FieldNotSet, "input is required");
            default: {
               char reason[64];
               std::snprintf(reason, sizeof(reason), "unsupported source kind %d", int(Type));
               return filter_input_error(effect, Role, Slot, ERR::NoSupport, reason);
            }
         }
         const bool mix = Role IS filter_input_role::MIX;
         const auto kind = mix ? operation.MixKind : filter_input_kind::IMAGE;
         const auto &input = Plan.Inputs.emplace_back(Type, Role, Slot, producer, kind,
            mix and operation.MapContributesRegion);
         if (producer and (producer > &step) and (not forward_input)) {
            forward_consumer = effect;
            forward_input = &input;
         }
         return ERR::Okay;
      };

      switch (operation.Inputs) {
         case filter_inputs::GENERATOR:
            break;
         case filter_inputs::DUAL:
         case filter_inputs::SINGLE:
            if (auto error = add_input(effect->SourceType, effect->Input, filter_input_role::INPUT);
                error != ERR::Okay) return error;
            if (operation.Inputs IS filter_inputs::DUAL) {
               if (auto error = add_input(effect->MixType, effect->Mix, filter_input_role::MIX); error != ERR::Okay) {
                  return error;
               }
            }
            break;
         case filter_inputs::MERGE: {
            const auto &sources = ((extMergeFX *)effect)->List;
            for (size_t j = 0; j < sources.size(); j++) {
               if (auto error = add_input(sources[j].SourceType, sources[j].Effect, filter_input_role::MERGE, j);
                   error != ERR::Okay) return error;
            }
            break;
         }
      }
      step.Inputs = std::span(Plan.Inputs).subspan(first);
   }

   if (forward_input) return diagnose_filter_order(Plan, forward_consumer, *forward_input);
   return ERR::Okay;
}

//********************************************************************************************************************
// Compute a bounding box region when the AspectRatio is NONE.  The region extends from the bounds by distances
// measured against the normalised diagonal, so that the margin around the content is the same on every side for any
// shape.  The default region of (-10%, -10%, 120%, 120%) adds 10% of the diagonal to each side.

static TClipRectangle<double> diagonal_region(const extVectorFilter *Self, double Left, double Top, double Width,
   double Height)
{
   double diag = filter_render_context::normalised_diagonal(Width, Height);
   double x = Self->X.defined() ? double(Self->X) : 0.0;
   double y = Self->Y.defined() ? double(Self->Y) : 0.0;
   double width  = std::max(0.0, Width + (((Self->Width.defined() ? double(Self->Width) : 1.0) - 1.0) * diag));
   double height = std::max(0.0, Height + (((Self->Height.defined() ? double(Self->Height) : 1.0) - 1.0) * diag));
   double left = Left + (x * diag);
   double top  = Top + (y * diag);
   return { left, top, left + width, top + height };
}

//********************************************************************************************************************
// Bounding box filter lengths use the dimensions of Bounds, with optional diagonal margins.

static TClipRectangle<double> box_filter_region(const extVectorFilter *Self, const TClipRectangle<double> &Bounds)
{
   const double bound_width = Bounds.width(), bound_height = Bounds.height();
   if (Self->AspectRatio IS VFA::NONE) {
      const auto region = diagonal_region(Self, Bounds.left, Bounds.top, bound_width, bound_height);
      const double left = std::round(region.left), top = std::round(region.top);
      return { left, top, left + std::round(region.width()), top + std::round(region.height()) };
   }

   const double left = std::round(Bounds.left + (Self->X.defined() ? Self->X * bound_width : 0.0));
   const double top = std::round(Bounds.top + (Self->Y.defined() ? Self->Y * bound_height : 0.0));
   const double width = std::round(Self->Width.defined() ? Self->Width * bound_width : bound_width);
   const double height = std::round(Self->Height.defined() ? Self->Height * bound_height : bound_height);
   return { left, top, left + width, top + height };
}

static double filter_length(const Unit &Length, double Reference, double Default)
{
   if (Length.scaled()) return Length * Reference;
   return Length.defined() ? double(Length) : Default;
}

static ERR prepare_filter_geometry(extVectorFilter *Self)
{
   auto &render = *Self->Render;
   const auto viewport = render.ClientViewport;
   render.ContainerWidth = viewport->vpFixedWidth;
   render.ContainerHeight = viewport->vpFixedHeight;
   render.ViewportWidth = viewport_coordinate_width(viewport);
   render.ViewportHeight = viewport_coordinate_height(viewport);
   render.FilterUnits = Self->Units;
   render.PrimitiveUnits = Self->PrimitiveUnits;
   render.AspectRatio = Self->AspectRatio;
   if ((not std::isfinite(render.ContainerWidth)) or (not std::isfinite(render.ContainerHeight)) or
       (not std::isfinite(render.ViewportWidth)) or (not std::isfinite(render.ViewportHeight))) {
      return ERR::InvalidValue;
   }
   if ((render.ContainerWidth < 1) or (render.ContainerHeight < 1)) return ERR::NothingDone;

   for (const auto &length : { Self->X, Self->Y, Self->Width, Self->Height }) {
      if (length.defined() and (not std::isfinite(double(length)))) return ERR::InvalidValue;
   }

   const auto client = render.ClientVector;
   render.ToScene = client->Transform;
   auto inverse = render.ToScene;
   if (not invert_filter_transform(inverse)) return ERR::InvalidValue;

   const TClipRectangle<double> initial_bounds { std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(), 0, 0 };
   auto bounds = initial_bounds;
   calc_local_boundary(client, bounds);
   if ((Self->GraphRevision != render.Revision) or Self->collecting() or client->collecting()) {
      return ERR::InvalidState;
   }
   if (not finite_region(bounds)) return ERR::InvalidValue;

   // A pathless client has no content dimensions.  Its processing rectangle can still fall back to the viewport.
   if ((bounds.right < bounds.left) or (bounds.bottom < bounds.top)) bounds = { 0.0 };
   render.ClientBounds = bounds;
   render.ClientPathBounds = client->Bounds;
   if ((render.FilterUnits IS VUNIT::BOUNDING_BOX) and (render.AspectRatio != VFA::NONE)) {
      render.SceneBounds = initial_bounds;
      calc_full_boundary(client, render.SceneBounds, false, true);
   }

   const double bound_x = std::round(bounds.left), bound_y = std::round(bounds.top);
   render.BoundWidth = std::round(bounds.width());
   render.BoundHeight = std::round(bounds.height());

   if (render.FilterUnits IS VUNIT::BOUNDING_BOX) {
      render.FilterRegion = box_filter_region(Self, { bound_x, bound_y,
         bound_x + render.BoundWidth, bound_y + render.BoundHeight });
   }
   else {
      const double x = std::round(filter_length(Self->X, render.ContainerWidth, bound_x));
      const double y = std::round(filter_length(Self->Y, render.ContainerHeight, bound_y));
      const double width = Self->Width.scaled() ? std::round(Self->Width * render.ContainerWidth) :
         filter_length(Self->Width, render.ContainerWidth, render.ContainerWidth);
      const double height = Self->Height.scaled() ? std::round(Self->Height * render.ContainerHeight) :
         filter_length(Self->Height, render.ContainerHeight, render.ContainerHeight);
      render.FilterRegion = { x, y, x + width, y + height };
   }

   return finite_region(render.FilterRegion) ? ERR::Okay : ERR::InvalidValue;
}

//********************************************************************************************************************
// Return a bitmap from the bank.  In order to save memory, bitmap data is managed internally so that it always
// reflects the size of the clipping region.  The bitmap uses local coordinates, so (0,0) is the top-left of
// VectorClip and the bitmap size matches the processing region.
//
// Effects can call this function while they are the current context, so the filter is set as the owner explicitly.
// The bank frees its bitmaps, which must not be freed earlier with an effect.

static ERR get_banked_bitmap(extVectorFilter *Self, objBitmap **BitmapResult)
{
   kt::Log log(__FUNCTION__);

   auto &render = *Self->Render;
   auto bi = render.BankIndex;
   if (bi >= 255) return log.warning(ERR::ArrayFull);

   if (bi >= Self->Bank.size()) Self->Bank.emplace_back(std::make_unique<filter_bitmap>());

   auto clip = render.scratch_clip();
   #ifdef DEBUG_FILTER_BITMAP
      auto bmp = Self->Bank[bi].get()->get_bitmap(clip.right, clip.bottom, clip, true, Self->UID);
   #else
      auto bmp = Self->Bank[bi].get()->get_bitmap(clip.right, clip.bottom, clip, false, Self->UID);
   #endif

   if (bmp) {
      bmp->ColourSpace = CS::SRGB;
      bmp->BlendMode = BLM::AUTO;
      bmp->Opacity = 255;
      bmp->Flags &= ~BMF::PREMUL;
      *BitmapResult = bmp;
      render.BankIndex++;
      return ERR::Okay;
   }
   else return log.warning(ERR::CreateObject);
}

//********************************************************************************************************************
// Inputs are published only for the duration of this draw.  Standard sources are populated lazily and shared by
// readers, just like effect results.

static ERR get_source_view(extFilterEffect *Effect, filter_surface_view &Output, filter_input_role Role, size_t Slot)
{
   kt::Log log(__FUNCTION__);
   Output = {};
   const auto step = active_filter_step(Effect);
   const size_t index = (Role IS filter_input_role::MIX) ? 1 : Slot;
   if ((not step) or (index >= step->Inputs.size()) or (step->Inputs[index].Role != Role)) {
      return log.warning(ERR::InvalidState);
   }

   auto filter = Effect->Filter;
   auto &render = *filter->Render;
   kt::SwitchContext ctx(filter);
   const auto &input = step->Inputs[index];
   log.msg(VLF::BRANCH|VLF::DETAIL, "%s #%d <- ID: #%u, Type: %d", Effect->className(), Effect->UID,
      input.Producer ? input.Producer->Effect->UID : 0, int(input.Source));
   if (not active_filter_step(Effect)) return ERR::InvalidState;

   if (input.Source IS VSF::REFERENCE) {
      if (not input.Producer->Result.Bitmap) {
         return filter_input_error(Effect, Role, Slot, ERR::NoData, "producer has no result in this draw",
            input.Producer->Effect->UID);
      }
      Output = input.Producer->Result;
      return ERR::Okay;
   }
   if (input.Source IS VSF::NONE) return ERR::Continue;
   if (input.Source IS VSF::GRAPHIC) {
      auto error = get_source_graphic(filter, Output);
      if (error != ERR::Okay) return error;
      if (not active_filter_step(Effect)) return ERR::InvalidState;
      return ERR::Okay;
   }

   filter_surface_view *source;
   switch (input.Source) {
      case VSF::ALPHA:      source = &render.SourceAlpha; break;
      case VSF::BKGD:       source = &render.Background; break;
      case VSF::BKGD_ALPHA: source = &render.BackgroundAlpha; break;
      default: return log.warning(ERR::NoSupport);
   }

   if (not source->Bitmap) {
      objBitmap *bmp;
      if (auto error = get_banked_bitmap(filter, &bmp); error != ERR::Okay) return error;
      if (not active_filter_step(Effect)) return ERR::InvalidState;

      if (input.Source IS VSF::ALPHA) {
         filter_surface_view graphic;
         if (auto error = get_source_graphic(filter, graphic); error != ERR::Okay) return error;
         if (not active_filter_step(Effect)) return ERR::InvalidState;
         for (int y = graphic.Valid.top; y < graphic.Valid.bottom; y++) {
            const auto src = (const uint32_t *)graphic.row(y);
            const auto dest = (uint32_t *)(bmp->Data + std::ptrdiff_t(y) * bmp->LineWidth);
            for (int x = graphic.Valid.left; x < graphic.Valid.right; x++) dest[x] = src[x] & 0xff000000;
         }
      }
      else {
         // The destination is initially transparent, including pixels outside the captured background.
         gfx::DrawRectangle(bmp, 0, 0, bmp->Width, bmp->Height, 0, BAF::FILL);
         auto background = render.BkgdBitmap;
         if (background and ((background->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
            const auto &clip = render.VectorClip;
            auto save_data = background->offset(render.BkgdOriginX, render.BkgdOriginY);
            auto restore_data = kt::Defer([&]() { background->Data = save_data; });
            const int left = clip.left - render.BkgdOriginX;
            const int top = clip.top - render.BkgdOriginY;
            if (input.Source IS VSF::BKGD) {
               if (auto error = copy_filter_bitmap(background, bmp, BAF::NIL, left, top,
                   clip.width(), clip.height(), 0, 0); error != ERR::Okay) return error;
               if (background->ColourSpace IS CS::LINEAR_RGB) {
                  bmp->ColourSpace = CS::LINEAR_RGB;
                  if (auto error = bmp->convertToRGB(); error != ERR::Okay) return error;
               }
            }
            else {
               const int x0 = std::max(left, background->Clip.Left);
               const int x1 = std::min(left + clip.width(), background->Clip.Right);
               const int y0 = std::max(top, background->Clip.Top);
               const int y1 = std::min(top + clip.height(), background->Clip.Bottom);
               for (int y = y0; y < y1; y++) {
                  const auto src = (const uint32_t *)(background->Data + std::ptrdiff_t(y) * background->LineWidth);
                  const auto dest = (uint32_t *)(bmp->Data + std::ptrdiff_t(y - top) * bmp->LineWidth);
                  for (int x = x0; x < x1; x++) dest[x - left] = src[x] & 0xff000000;
               }
            }
         }
      }

      *source = filter_view(render, bmp);
   }

   Output = *source;
   return ERR::Okay;
}

static ERR get_source_bitmap(extFilterEffect *Effect, const objBitmap **Bitmap, filter_input_role Role, size_t Slot)
{
   filter_surface_view source;
   auto error = get_source_view(Effect, source, Role, Slot);
   *Bitmap = (error IS ERR::Okay) ? source.Bitmap : nullptr;
   return error;
}

// Decode before premultiplying.  The filter bank uses a common RGBA8 layout, so a working copy preserves the
// source bytes exactly before the bitmap conversion routines perform their usual rounding.

static ERR premultiplied_input(extFilterEffect *Effect, const filter_surface_view &Source, CS Encoding,
   filter_surface_view &Output)
{
   if (not active_filter_step(Effect)) return ERR::InvalidState;
   if ((Source.Alpha IS filter_alpha::PREMULTIPLIED) and (Source.Encoding IS Encoding)) {
      Output = Source;
      return ERR::Okay;
   }

   auto filter = Effect->Filter;
   objBitmap *work;
   if (auto error = get_banked_bitmap(filter, &work); error != ERR::Okay) return error;
   if (not active_filter_step(Effect)) return ERR::InvalidState;

   const auto &valid = Source.Valid;
   for (int y = valid.top; y < valid.bottom; y++) {
      constexpr int pixel_size = 4;
      const auto start = valid.left * pixel_size;
      copymem(Source.row(y) + start, work->Data + std::ptrdiff_t(y) * work->LineWidth + start,
         size_t(valid.width()) * pixel_size);
   }
   work->ColourSpace = Source.Encoding;
   if (Source.Alpha IS filter_alpha::PREMULTIPLIED) {
      work->Flags |= BMF::PREMUL;
      if (auto error = work->demultiply(); error != ERR::Okay) return error;
   }
   if (Source.Encoding != Encoding) {
      auto error = (Encoding IS CS::LINEAR_RGB) ? work->convertToLinear() : work->convertToRGB();
      if (error != ERR::Okay) return error;
   }
   if (auto error = work->premultiply(); error != ERR::Okay) return error;
   Output = filter_view(*filter->Render, work);
   return ERR::Okay;
}

// Suppress only this pipeline in the private scene that is drawing its raw input.  A request from any other
// scene will still detect an active instance, including indirect recursion through another filter.

static ERR draw_filter_capture(extVectorFilter *Filter, extVectorScene *Scene, extVector *Root,
   objBitmap *Bitmap, int X, int Y)
{
   filter_capture capture { Filter, Root };
   auto saved_capture = std::exchange(Scene->FilterCapture, &capture);
   auto saved_bitmap = std::exchange(Scene->Bitmap, Bitmap);
   auto saved_x = std::exchange(Scene->RenderOriginX, X);
   auto saved_y = std::exchange(Scene->RenderOriginY, Y);
   auto restore = kt::Defer([&]() {
      Scene->RenderOriginX = saved_x;
      Scene->RenderOriginY = saved_y;
      Scene->Bitmap = saved_bitmap;
      Scene->FilterCapture = saved_capture;
   });

   if (auto error = acDraw(Scene); error != ERR::Okay) return error;
   return capture.Error;
}

//********************************************************************************************************************
// Render the client once per draw for SourceGraphic and SourceAlpha.  A failed capture is never published.

static ERR get_source_graphic(extVectorFilter *Self, filter_surface_view &Output)
{
   auto &render = *Self->Render;
   if (render.SourceGraphic.Bitmap) {
      Output = render.SourceGraphic;
      return ERR::Okay;
   }

   kt::SwitchContext ctx(Self);
   auto clip = render.scratch_clip();
   #ifdef DEBUG_FILTER_BITMAP
      auto sg = Self->SourceGraphic.get_bitmap(clip.right, clip.bottom, clip, true, 0, "source_graphic");
   #else
      auto sg = Self->SourceGraphic.get_bitmap(clip.right, clip.bottom, clip, false, 0, "source_graphic");
   #endif
   if (not sg) return ERR::CreateObject;

   const auto scene = render.ClientViewport->Scene;
   if (not Self->SourceScene) {
      auto source_scene = extVectorScene::create::local(fl::PageWidth(scene->PageWidth),
         fl::PageHeight(scene->PageHeight));
      if (not source_scene) return ERR::CreateObject;
      if (not extVectorViewport::create::global(fl::Owner(source_scene->UID))) {
         FreeResource(source_scene);
         return ERR::CreateObject;
      }
      Self->SourceScene = source_scene;
   }
   else if ((scene->PageWidth != Self->SourceScene->PageWidth) or
            (scene->PageHeight != Self->SourceScene->PageHeight)) {
      if (auto error = acResize(Self->SourceScene, scene->PageWidth, scene->PageHeight, 0); error != ERR::Okay) {
         return error;
      }
      mark_dirty(Self->SourceScene->Viewport, RC::DIRTY);
   }

   // The source is sRGB encoded.  It inherits the destination's blending preference and the client's resolved
   // colour space, independently of the colour space requested for filter arithmetic.
   auto preference = render.BkgdBitmap ? render.BkgdBitmap->BlendMode : BLM::AUTO;
   if ((preference IS BLM::LINEAR) or (preference IS BLM::NONE)) preference = BLM::AUTO;
   sg->BlendMode = preference;
   sg->ColourSpace = CS::SRGB;
   sg->Flags &= ~BMF::PREMUL;
   gfx::DrawRectangle(sg, 0, 0, sg->Width, sg->Height, 0, BAF::FILL);

   auto viewport = (extVectorViewport *)Self->SourceScene->Viewport;
   auto client = render.ClientVector;
   SharedObjectAccess access(client);
   if (not access.granted()) return access.error;
   auto saved_colour = std::exchange(viewport->ColourSpace, render.ClientColourSpace);
   auto restore_colour = kt::Defer([&]() { viewport->ColourSpace = saved_colour; });

   auto error = draw_filter_capture(Self, Self->SourceScene, client, sg, render.VectorClip.left, render.VectorClip.top);
   if (error != ERR::Okay) return error;
   if ((Self->GraphRevision != render.Revision) or Self->collecting() or client->collecting()) {
      return ERR::InvalidState;
   }

   Output = render.SourceGraphic = filter_view(render, sg);
   return ERR::Okay;
}

//********************************************************************************************************************
// Sum the per-effect bounds across the whole pipeline.  Only the page and this conservative margin constrain
// processing.  The scene composites the final output using the actual drawing clip.

static ERR filter_reach(const filter_plan &Plan, double &ReachX, double &ReachY)
{
   ReachX = ReachY = 0;
   for (const auto &step : Plan.Steps) {
      const auto sampling = step.Operation.Sampling(step.Effect);
      const auto valid = [&](double Reach) {
         return (Reach >= 0) and (std::isfinite(Reach) or (sampling.Access IS filter_access::FULL_INPUT));
      };
      if ((not valid(sampling.X)) or (not valid(sampling.Y))) {
         kt::Log("FilterGeometry").warning("%s #%d has invalid sampling reach.", step.Effect->className(),
            step.Effect->UID);
         return ERR::InvalidValue;
      }
      ReachX += sampling.X;
      ReachY += sampling.Y;
   }
   return ERR::Okay;
}

static bool transform_filter_region(TClipRectangle<double> &Region, const agg::trans_affine &Transform)
{
   const auto rect = Region;
   Region = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
   for (double x : { rect.left, rect.right }) {
      for (double y : { rect.top, rect.bottom }) {
         double px = x, py = y;
         Transform.transform(&px, &py);
         if ((not std::isfinite(px)) or (not std::isfinite(py))) return false;
         Region.left = std::min(Region.left, px);
         Region.top = std::min(Region.top, py);
         Region.right = std::max(Region.right, px);
         Region.bottom = std::max(Region.bottom, py);
      }
   }
   return finite_region(Region);
}

static ERR set_clip_region(extVectorFilter *Self)
{
   auto &render = *Self->Render;
   const auto viewport = render.ClientViewport;
   TClipRectangle<double> region;

   if (render.FilterUnits IS VUNIT::BOUNDING_BOX) {
      bool transform = render.AspectRatio IS VFA::NONE;
      auto bounds = transform ? render.ClientBounds : render.SceneBounds;
      if (not finite_region(bounds)) return ERR::InvalidValue;
      if (not bounds.valid()) {
         bounds = viewport->vpBounds;
         transform = false;
      }
      if (render.AspectRatio IS VFA::NONE) {
         const double left = std::round(bounds.left), top = std::round(bounds.top);
         bounds = { left, top, left + std::round(bounds.width()), top + std::round(bounds.height()) };
      }
      region = box_filter_region(Self, bounds);
      if (transform and region.valid()) {
         if (not transform_filter_region(region, render.ToScene)) return ERR::InvalidValue;
         region = { std::floor(region.left), std::floor(region.top),
            std::ceil(region.right), std::ceil(region.bottom) };
      }
   }
   else {
      const double x = std::round(filter_length(Self->X, render.ContainerWidth, 0));
      const double y = std::round(filter_length(Self->Y, render.ContainerHeight, 0));
      const double width = std::round(filter_length(Self->Width, render.ContainerWidth, render.ContainerWidth));
      const double height = std::round(filter_length(Self->Height, render.ContainerHeight, render.ContainerHeight));
      if ((width <= 0) or (height <= 0)) return ERR::NothingDone;
      region = { x, y, x + width, y + height };
      if ((not finite_region(region)) or (not transform_filter_region(region, render.ToScene))) {
         return ERR::InvalidValue;
      }
      region = { std::trunc(region.left), std::trunc(region.top),
         std::trunc(region.right), std::trunc(region.bottom) };
   }
   if (not finite_region(region)) return ERR::InvalidValue;
   if (not region.valid()) return ERR::NothingDone;

   const double page_width = viewport->Scene->PageWidth, page_height = viewport->Scene->PageHeight;
   if ((page_width <= 0) or (page_height <= 0)) return ERR::NothingDone;

   double reach_x, reach_y;
   if (auto error = filter_reach(render.Plan, reach_x, reach_y); error != ERR::Okay) return error;
   const double max_margin = std::max(page_width, page_height);

   auto limit_region = [&](double MarginX, double MarginY) -> TClipRectangle<double> {
      return { std::max(region.left, -MarginX), std::max(region.top, -MarginY),
         std::min(region.right, page_width + MarginX), std::min(region.bottom, page_height + MarginY) };
   };
   auto clip = limit_region(std::ceil(std::min(reach_x, max_margin)), std::ceil(std::min(reach_y, max_margin)));
   const double area = clip.width() * clip.height();
   if ((area > MAX_FILTER_MARGIN_AREA) and (area > page_width * page_height)) clip = limit_region(0, 0);
   if (not clip.valid()) return ERR::NothingDone;
   if ((clip.right > std::numeric_limits<int>::max()) or (clip.bottom > std::numeric_limits<int>::max()) or
       (clip.width() > 0xffff) or (clip.height() > 0xffff) or
       (clip.width() * clip.height() * 4 > std::numeric_limits<int>::max())) return ERR::OutOfRange;

   render.VectorClip = { int(clip.left), int(clip.top), int(clip.right), int(clip.bottom) };
   render.Transform = render.ToScene;
   render.Transform.translate(-clip.left, -clip.top);
   render.Inverse = render.Transform;
   TClipRectangle<double> local = { 0, 0, clip.width(), clip.height() };
   if ((not invert_filter_transform(render.Inverse)) or (not transform_filter_region(local, render.Inverse))) {
      return ERR::InvalidValue;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Resolve the primitive subregion in client user coordinates.  With only producer images it defaults to their
// union.  A qualifying standard source or the absence of qualifying inputs uses the filter region.

static ERR compute_subregion(const filter_render_context &Render, filter_step &Step)
{
   TClipRectangle<double> inputs = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
   bool referenced = false, standard = false;
   for (const auto &input : Step.Inputs) {
      if ((input.Kind IS filter_input_kind::PARAMETER_MAP) and (not input.MapContributesRegion)) continue;
      if (input.Producer) {
         inputs.expanding(input.Producer->Subregion);
         referenced = true;
      }
      else if (input.Source != VSF::NONE) standard = true;
   }

   const auto &region = (referenced and (not standard)) ? inputs : Render.FilterRegion;
   const auto effect = Step.Effect;

   double x = region.left, y = region.top, width = region.width(), height = region.height();
   if (Render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      const auto &bounds = Render.ClientBounds;
      if (effect->X.defined()) x = bounds.left + (effect->X * bounds.width());
      if (effect->Y.defined()) y = bounds.top + (effect->Y * bounds.height());
      if (effect->Width.defined()) width = effect->Width * bounds.width();
      if (effect->Height.defined()) height = effect->Height * bounds.height();
   }
   else {
      x = filter_length(effect->X, Render.ViewportWidth, x);
      y = filter_length(effect->Y, Render.ViewportHeight, y);
      width = filter_length(effect->Width, Render.ViewportWidth, width);
      height = filter_length(effect->Height, Render.ViewportHeight, height);
   }

   Step.Subregion = { x, y, x + width, y + height };
   auto local = Step.Subregion;
   if ((not finite_region(local)) or (local.valid() and (not transform_filter_region(local, Render.Transform)))) {
      kt::Log("FilterGeometry").warning("%s #%d has an invalid subregion.", effect->className(), effect->UID);
      return ERR::InvalidValue;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Test the pixel centres in user space.  The common case covers the entire filter region.

static void clip_to_subregion(const filter_render_context &Render, const filter_step &Step)
{
   const auto &sub = Step.Subregion;
   const auto &region = Render.FilterRegion;
   if ((sub.left <= region.left) and (sub.top <= region.top) and
       (sub.right >= region.right) and (sub.bottom >= region.bottom)) return;

   auto bmp = Step.Effect->Target;
   if ((not bmp) or (bmp->BytesPerPixel != 4)) return;

   for (int y=bmp->Clip.Top; y < bmp->Clip.Bottom; y++) {
      auto pixel = (uint32_t *)(bmp->Data + std::ptrdiff_t(y) * bmp->LineWidth);
      for (int x=bmp->Clip.Left; x < bmp->Clip.Right; x++) {
         double px, py;
         map_filter_pixel_centre(Render.Inverse, x, y, px, py);
         if (not sub.hit_test(px, py)) pixel[x] = 0;
      }
   }
}

//********************************************************************************************************************
// Main rendering routine for filter effects.  Called by the scene graph renderer whenever a vector uses a filter.
// The caller holds the filter lock through rendering and composition.  ColourSpace is the client's resolved
// colour space in the renderer, which can be inherited from a vector that is not
// a physical ancestor, e.g. the viewport that consumes a live pattern.

ERR render_filter(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector,
   objBitmap *BkgdBitmap, VCS ColourSpace, filter_render_result &Output, int BkgdOriginX, int BkgdOriginY)
{
   kt::Log log(__FUNCTION__);

   Output = {};
   if ((not Vector) or (not Viewport)) return log.warning(ERR::NullArgs);

   if (Self->collecting()) return ERR::MarkedForDeletion;
   if (Self->Render) return ERR::Recursion;
   kt::SwitchContext context(Self);

   filter_render_context render = {};
   render.ClientVector = Vector;
   render.ClientViewport = Viewport;
   render.BkgdOriginX = BkgdOriginX;
   render.BkgdOriginY = BkgdOriginY;
   render.ClientColourSpace = ColourSpace;
   render.Revision = Self->GraphRevision;

   Self->Render = &render;
   auto graph_changed = [&]() { return (Self->GraphRevision != render.Revision) or Self->collecting(); };
   bool completed = false;
   auto cleanup = kt::Defer([&]() {
      Self->Render = nullptr;
      if (not completed) {
         for (auto effect : Self->Effects) effect->Target = nullptr;
      }
   });

   for (auto effect : Self->Effects) effect->Target = nullptr;
   if (Self->Effects.empty()) return log.warning(ERR::FieldNotSet);
   auto &plan = render.Plan;
   if (auto error = prepare_filter_plan(Self, plan); error != ERR::Okay) return error;

   CSTRING filter_name = Self->Name;
   if ((!filter_name) or (!filter_name[0])) filter_name = "Unnamed";
   CSTRING vector_name = Vector->Name;
   if ((!vector_name) or (!vector_name[0])) vector_name = "Unnamed";
   log.msg(VLF::BRANCH|VLF::DETAIL, "Rendering '%s' filter content for %s #%d '%s'.", filter_name,
      Vector->Class->ClassName.c_str(), Vector->UID, vector_name);

   render.BkgdBitmap = (plan.ReadsBackground or plan.ReadsSourceGraphic) ? BkgdBitmap : nullptr;

   if (auto error = prepare_filter_geometry(Self); error != ERR::Okay) return error;
   if (graph_changed()) return log.warning(ERR::InvalidState);
   if (auto error = set_clip_region(Self); error != ERR::Okay) return error;
   if (graph_changed()) return log.warning(ERR::InvalidState);

   objBitmap *unused = nullptr;
   for (auto &step : plan.Steps) {
      const auto effect = step.Effect;
      log.detail("Effect: %s #%u, Consumers: %zu", effect->className(), effect->UID, step.Consumers);

      // Unconsumed intermediates can reuse one cleared scratch surface.  The authored final effect owns its
      // output independently of the number of consumers on any other effect.
      objBitmap *target = nullptr;
      if ((&step IS plan.Final) or step.Consumers) {
         if (auto error = get_banked_bitmap(Self, &target); error != ERR::Okay) return error;
      }
      else {
         if (not unused) {
            if (auto error = get_banked_bitmap(Self, &unused); error != ERR::Okay) return error;
         }
         target = unused;
      }
      gfx::DrawRectangle(target, 0, 0, target->Width, target->Height, 0x00000000, BAF::FILL);
      if (graph_changed()) return log.warning(ERR::InvalidState);
      effect->Target = target;

      // Earlier draw and bitmap allocation callbacks can edit this primitive's dimensions.
      if (auto error = compute_subregion(render, step); error != ERR::Okay) return error;
      render.ActiveStep = &step;
      auto finish_step = kt::Defer([&]() { render.ActiveStep = nullptr; });
      if (step.Subregion.valid()) {
         if (auto error = acDraw(effect); error != ERR::Okay) return log.warning(error);
      }
      if (graph_changed() or effect->collecting()) {
         log.warning("Filter #%d changed while drawing.", Self->UID);
         return ERR::InvalidState;
      }

      effect->Target = target;
      if (step.Subregion.valid()) clip_to_subregion(render, step);
      if ((target->ColourSpace != CS::SRGB) or ((target->Flags & BMF::PREMUL) != BMF::NIL)) {
         return log.warning(ERR::InvalidState);
      }
      if ((&step IS plan.Final) or step.Consumers) step.Result = filter_view(render, target);
   }

   const auto &result = plan.Final->Result;
   auto out = plan.Final->Effect->Target;

   #if defined(EXPORT_FILTER_BITMAP) && defined (DEBUG_FILTER_BITMAP)
      save_bitmap(out, std::to_string(Self->UID) + "_" + std::to_string(Vector->UID) + "_output");
   #endif

   #ifdef DEBUG_FILTER_BITMAP
      gfx::DrawRectangle(out, out->Clip.Left, out->Clip.Top, out->Clip.Right-out->Clip.Left, out->Clip.Bottom-out->Clip.Top, 0xff0000ff, BAF::NIL);
   #endif

   #if !defined(NDEBUG) and !defined(DEBUG_FILTER_BITMAP)
      if (GetResource(RES::LOG_LEVEL) >= 6) {
         auto retained_bytes = Self->SourceGraphic.Data.capacity();
         for (const auto &slot : Self->Bank) retained_bytes += slot->Data.capacity();

         // Every acquired bank slot stays in use until the draw ends.  SourceGraphic is acquired at most once.
         const auto peak_live_surfaces = int(render.BankIndex) + int(render.SourceGraphic.Bitmap != nullptr);
         log.detail("Filter storage: retained_bytes=%zu peak_live_surfaces=%d", retained_bytes, peak_live_surfaces);
      }
   #endif

   if (graph_changed()) return log.warning(ERR::InvalidState);
   Output = { out, result.OriginX, result.OriginY, Self->Opacity };
   completed = true;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clear: Removes all filter effects.
-END-
*********************************************************************************************************************/

static ERR VECTORFILTER_Clear(extVectorFilter *Self)
{
   kt::Log log;

   if (Self->Render) return log.warning(ERR::InUse);

   auto detached = std::exchange(Self->Effects, {});
   std::vector<OBJECTID> ids;
   ids.reserve(detached.size());
   for (auto effect : detached) {
      ids.push_back(effect->UID);
      clear_filter_references(effect);
      effect->Filter = nullptr;
      effect->Prev = effect->Next = nullptr;
      effect->Target = nullptr;
   }

   Self->GraphRevision++;
   Self->Bank.clear();

   // Callbacks and deferred destruction can change ownership while the detached effects are being freed.
   for (auto id : ids) {
      if (auto effect = GetObjectPtr(id); effect and (effect->Owner IS Self)) FreeResource(id);
   }

   return ERR::Okay;
}

extVectorFilter::~extVectorFilter()
{
   VECTORFILTER_Clear(this);
   if (SourceScene) FreeResource(SourceScene);
}

//********************************************************************************************************************

static ERR VECTORFILTER_Init(extVectorFilter *Self)
{
   kt::Log log;

   if ((int(Self->Units) <= 0) or (int(Self->Units) >= int(VUNIT::END))) {
      return log.warning(ERR::OutOfRange);
   }

   if (not Self->Scene) {
      if (Self->Owner->classID() IS CLASSID::VECTORSCENE) Self->Scene = (extVectorScene *)Self->Owner;
      else return log.warning(ERR::UnsupportedOwner);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_NewChild(extVectorFilter *Self, struct acNewChild *Args)
{
   if ((not Args) or (not Args->Object)) return ERR::NullArgs;
   if (Self->collecting()) return ERR::MarkedForDeletion;

   if (Args->Object->Class->BaseClassID IS CLASSID::FILTEREFFECT) {
      auto effect = (extFilterEffect *)Args->Object;
      if (effect->Filter IS Self) return ERR::Okay;
      if (effect->Filter) {
         clear_filter_references(effect);
         unlink_filter_effect(effect);
      }

      auto prev = Self->Effects.empty() ? nullptr : Self->Effects.back();
      Self->Effects.push_back(effect);
      effect->Filter = Self;
      effect->Prev = prev;
      effect->Next = nullptr;
      if (prev) prev->Next = effect;
      Self->GraphRevision++;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_NewOwner(extVectorFilter *Self, struct acNewOwner *Args)
{
   if (Args->NewOwner->classID() IS CLASSID::VECTORSCENE) {
      Self->Scene = (extVectorScene *)Args->NewOwner;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: Selects per-axis or diagonal scaling for bounding box units.

AspectRatio affects the filter region when #Units is `BOUNDING_BOX`, and the effects that support it when
#PrimitiveUnits is `BOUNDING_BOX`.  The default is `MEET`, which scales horizontal values by the width of the bounding
box and vertical values by its height, as per the SVG standard.

The `NONE` option is a Kōtuku extension that scales all values by the normalised diagonal of the bounding box, i.e.
`sqrt(width^2 + height^2) / sqrt(2)`.  This is the same formula used by a scaled stroke width.  A blur radius or
offset then covers the same distance on both axes for any shape, which is preferable for effects such as glows and
drop shadows.  For the filter region, the #X, #Y, #Width and #Height values are measured from the edges of the
bounding box, so the default region adds a margin of 10% of the normalised diagonal to every side.

The following effects support `NONE`: @BlurFX, @LightingFX and @OffsetFX.

-FIELD-
ColourSpace: The colour space of the filter graphics (SRGB or linear RGB).

By default, colour filters are processed in SRGB format.  This is the same colour space as used by the rest of the
graphics system, which means that no special conversion is necessary prior to and post filter processing.  However,
linear RGB is better suited for producing high quality results at a cost of speed.

Intermediate images are always stored in sRGB encoding.  When `LINEAR_RGB` is selected, effects that honour this
setting decode their inputs, process them in linear RGB and encode the result for storage.  These are @BlurFX,
@ColourFX, @ConvolveFX, @DisplacementFX (for the displacement map), @LightingFX, @MergeFX, @MorphologyFX, @RemapFX
and the `ARITHMETIC` operator of @CompositeFX.  The other @CompositeFX operators that mix colour components always do so
in linear RGB.
Colours that are input values, such as the colour of a @FloodFX, are stored as given and are not converted.

This setting applies only to the processing of effects within the pipeline.  The composite of the filter's output into
the target is governed by the client vector's @Vector.ColourSpace.

The SVG standard defaults to linear RGB for filter effects, so the SVG parser selects `LINEAR_RGB` unless the
`color-interpolation-filters` attribute states otherwise.  Filters created through the API retain the SRGB default.

-FIELD-
EffectXML: Returns a SVG XML string that defines the filter's effects.

This field value will return a purpose-built string that defines the filter's effects in SVG compliant XML.

*********************************************************************************************************************/

static ERR VECTORFILTER_GET_EffectXML(extVectorFilter *Self, std::string &Value)
{
   std::stringstream ss;

   for (auto e : Self->Effects) {
      ss << "<";
      std::string def;
      if (!e->getXMLDef(def)) {
         ss << def;
      }
      ss << "/>";
   }

   Value = ss.str();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Height: The height of the filter area.  Can be expressed as a fixed or scaled coordinate.

The height of the filter area is expressed here as a fixed or scaled coordinate.  The #Width and Height effectively
restrain the working space for the effect processing, making them an important consideration for efficiency.

The coordinate system for the width and height depends on the value for #Units.  In bounding-box mode, the
#AspectRatio determines whether the value is relative to the bounding box or its normalised diagonal.

The default values for #Width and Height is `120%`, as per the SVG standard.  This provides a buffer space for the
filter algorithms to work with, and is usually a sufficient default.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Height(extVectorFilter *Self, Unit &Value)
{
   if (Value > 0) {
      Self->Height = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Inherit: Inherit attributes from a VectorFilter referenced here.

Attributes can be inherited from another filter by referencing that gradient in this field.  This feature is provided
primarily for the purpose of simplifying SVG compatibility and its use may result in an unnecessary performance penalty.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Inherit(extVectorFilter *Self, extVectorFilter *Value)
{
   if (Value) {
      if (Value->Class->BaseClassID IS CLASSID::VECTORFILTER) Self->Inherit = Value;
      else return ERR::InvalidValue;
   }
   else Self->Inherit = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Opacity: The opacity of the filter.

The opacity of the filter is defined as a value between 0.0 and 1.0, with 1.0 being fully opaque.  The default value
is 1.0.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Opacity(extVectorFilter *Self, double Value)
{
   Self->Opacity = std::clamp(Value, 0.0, 1.0);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
PrimitiveUnits: Alters the behaviour of some effects that support alternative position calculations.

PrimitiveUnits alters the behaviour of some effects when their dimensions are calculated.  The default value is
`USERSPACE`.  When set to `BOUNDING_BOX`, the effect may calculate its dimensions strictly based on the client vector
using a scaled coordinate space of `(0, 0, 100%, 100%)`.

-FIELD-
ResX: Stores the requested horizontal filter resolution for compatibility.

Filter surfaces are sized in scene device pixels.  This field and #ResY have no effect on rendering.

-FIELD-
ResY: Stores the requested vertical filter resolution for compatibility.

Filter surfaces are sized in scene device pixels.  This field and #ResX have no effect on rendering.

-FIELD-
Units: Defines the coordinate system for #X, #Y, #Width and #Height.

The default coordinate system is `BOUNDING_BOX`, which positions the filter within the client vector.
The alternative is `USERSPACE`, which positions the filter relative to the client vector's nearest viewport.

-FIELD-
Width: The width of the filter area.  Can be expressed as a fixed or scaled coordinate.

The width of the filter area is expressed here as a fixed or scaled coordinate.  The Width and #Height effectively
restrain the working space for the effect processing, making them an important consideration for efficiency.

The coordinate system for the width and height depends on the value for #Units.  In bounding-box mode, the
#AspectRatio determines whether the value is relative to the bounding box or its normalised diagonal.

The default values for #Width and Height is `120%`, as per the SVG standard.  This provides a buffer space for the
filter algorithms to work with, and is usually a sufficient default.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Width(extVectorFilter *Self, Unit &Value)
{
   if (Value > 0) {
      Self->Width = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
X: X coordinate for the filter.

The meaning of the (X, #Y) field values depend on the value for #Units.  In userspace mode, the filter position will be
relative to the client vector's parent viewport.  In bounding-box mode, the filter position is relative to the
vector's position, and the #AspectRatio determines whether the value is relative to the bounding box or its
normalised diagonal.  It is important to note that coordinates are measured before any transforms are applied.

The default values for X and #Y is `10%`, as per the SVG standard.  This provides a buffer space for the filter
algorithms to work with, and is usually a sufficient default.

-FIELD-
Y: Y coordinate for the filter.

The meaning of the (#X, Y) field values depend on the value for #Units.  In userspace mode, the filter position will be
relative to the client vector's parent viewport.  In bounding-box mode, the filter position is relative to the
vector's position, and the #AspectRatio determines whether the value is relative to the bounding box or its
normalised diagonal.  It is important to note that coordinates are measured before any transforms are applied.

The default values for #X and Y is `10%`, as per the SVG standard.  This provides a buffer space for the filter
algorithms to work with, and is usually a sufficient default.

-END-
*********************************************************************************************************************/

#include "filter_def.c"

static const FieldArray clFilterFields[] = {
   { "X",              FDF_UNIT|FDF_RW },
   { "Y",              FDF_UNIT|FDF_RW },
   { "Width",          FDF_UNIT|FDF_RW, nullptr, VECTORFILTER_SET_Width },
   { "Height",         FDF_UNIT|FDF_RW, nullptr, VECTORFILTER_SET_Height },
   { "Opacity",        FDF_DOUBLE|FDF_RW, nullptr, VECTORFILTER_SET_Opacity },
   { "Inherit",        FDF_OBJECT|FDF_RW, nullptr, VECTORFILTER_SET_Inherit },
   { "ResX",           FDF_INT|FDF_RI },
   { "ResY",           FDF_INT|FDF_RI },
   { "Units",          FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterUnits },
   { "PrimitiveUnits", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterPrimitiveUnits },
   { "ColourSpace",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterColourSpace },
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterAspectRatio },
   // Virtual fields
   { "EffectXML",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, VECTORFILTER_GET_EffectXML },
   END_FIELD
};

//********************************************************************************************************************

ERR init_filter(void)
{
   clVectorFilter = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORFILTER),
      fl::Name("VectorFilter"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorFilterActions),
      fl::Fields(clFilterFields),
      fl::Size(sizeof(extVectorFilter)),
      fl::Path(MOD_PATH));

   return clVectorFilter ? ERR::Okay : ERR::AddClass;
}
