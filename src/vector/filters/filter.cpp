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

//#define DEBUG_FILTER_BITMAP
//#define EXPORT_FILTER_BITMAP

constexpr int64_t MAX_FILTER_MARGIN_AREA = 4096 * 4096; // Processing regions with margins are limited to this area

struct target {
   double bound_x, bound_y, bound_width, bound_height;
   double x, y, width, height;
};

enum class filter_input_role { INPUT, MIX, MERGE };
enum class filter_operation { GENERATOR, SINGLE, DUAL, MERGE };

struct filter_step;

struct filter_input {
   VSF Source;
   filter_input_role Role;
   size_t Slot;
   const filter_step *Producer;
};

struct filter_step {
   extFilterEffect *Effect;
   std::span<const filter_input> Inputs;
   size_t Consumers = 0;
   objBitmap *Result = nullptr;
};

struct filter_plan {
   std::vector<filter_step> Steps;
   std::vector<filter_input> Inputs;
   const filter_step *Final = nullptr;
   bool ReadsSourceGraphic = false;
   bool ReadsBackground = false;
};

using filter_positions = std::unordered_map<objFilterEffect *, size_t>;

static ERR get_source_bitmap(extFilterEffect *, objBitmap **, filter_input_role, size_t = 0);
static objBitmap * get_source_graphic(extVectorFilter *);
static ERR get_banked_bitmap(extVectorFilter *, objBitmap **);
static void unlink_filter_effect(extFilterEffect *);
static void clear_filter_references(extFilterEffect *, objFilterEffect * = nullptr);
static void update_filter_links(extVectorFilter *);
static ERR check_filter_reference(extFilterEffect *, objFilterEffect *, filter_input_role, size_t = 0,
   const filter_positions * = nullptr);

static const filter_step * active_filter_step(const extFilterEffect *Effect)
{
   const auto filter = Effect->Filter;
   if ((not filter) or (not filter->ActiveStep) or (filter->ActiveStep->Effect != Effect) or
       (filter->GraphRevision != filter->ActiveRevision)) return nullptr;
   return filter->ActiveStep;
}

//********************************************************************************************************************
// Universal function for rendering a filter's Bitmap to the target region.
//
// No blending is performed because this is intended for use when there is no former input.  Copying is done
// with only the transforms applied (if any).  Linear RGB interpolation will wait until post processing.

template <class T> void render_to_filter(T *Self, objBitmap *Bitmap, ARF AspectRatio = ARF::NONE, VSM SampleMethod = VSM::AUTO)
{
   auto filter = Self->Filter;

   // The image is fitted to the primitive subregion, which render_filter() computes before drawing the effect.  In
   // objectBoundingBox mode the subregion is measured against the bounding box of the client vector, as per SVG.

   double p_x = std::round(Self->Subregion.left), p_y = std::round(Self->Subregion.top);
   double p_width = std::round(Self->Subregion.width()), p_height = std::round(Self->Subregion.height());

   double x_scale = 1, y_scale = 1, align_x = 0, align_y = 0;
   calc_aspectratio("align_filter", AspectRatio, p_width, p_height, Bitmap->Width, Bitmap->Height, align_x, align_y, x_scale, y_scale);

   p_x += std::round(align_x);
   p_y += std::round(align_y);

   agg::trans_affine img_transform;
   img_transform.scale(x_scale, y_scale);
   img_transform.translate(p_x, p_y);
   img_transform *= filter->Transform;
   img_transform.invert();

   if (img_transform.is_complex()) {
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
   else gfx::CopyArea(Bitmap, Self->Target, BAF::NIL, 0, 0, Bitmap->Width, Bitmap->Height, -img_transform.tx, -img_transform.ty);
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

static filter_operation describe_filter_operation(CLASSID Class)
{
   switch (Class) {
      case CLASSID::MERGEFX:
         return filter_operation::MERGE;
      case CLASSID::COMPOSITEFX:
      case CLASSID::DISPLACEMENTFX:
         return filter_operation::DUAL;
      case CLASSID::FLOODFX:
      case CLASSID::IMAGEFX:
      case CLASSID::SOURCEFX:
      case CLASSID::TURBULENCEFX:
      case CLASSID::WAVEFUNCTIONFX:
         return filter_operation::GENERATOR;
      default:
         return filter_operation::SINGLE;
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
      Plan.Steps.push_back({ .Effect = effect, .Inputs = {} });
      switch (describe_filter_operation(effect->classID())) {
         case filter_operation::GENERATOR: break;
         case filter_operation::SINGLE: input_count++; break;
         case filter_operation::DUAL: input_count += 2; break;
         case filter_operation::MERGE: input_count += ((extMergeFX *)effect)->List.size(); break;
      }
   }

   Plan.Inputs.reserve(input_count);
   Plan.Final = &Plan.Steps.back();
   const extFilterEffect *forward_consumer = nullptr;
   const filter_input *forward_input = nullptr;

   for (size_t i = 0; i < Plan.Steps.size(); i++) {
      auto &step = Plan.Steps[i];
      auto effect = step.Effect;
      const auto operation = describe_filter_operation(effect->classID());
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
         const auto &input = Plan.Inputs.emplace_back(Type, Role, Slot, producer);
         if (producer and (producer > &step) and (not forward_input)) {
            forward_consumer = effect;
            forward_input = &input;
         }
         return ERR::Okay;
      };

      switch (operation) {
         case filter_operation::GENERATOR:
            break;
         case filter_operation::DUAL:
         case filter_operation::SINGLE:
            if (auto error = add_input(effect->SourceType, effect->Input, filter_input_role::INPUT);
                error != ERR::Okay) return error;
            if (operation IS filter_operation::DUAL) {
               if (auto error = add_input(effect->MixType, effect->Mix, filter_input_role::MIX); error != ERR::Okay) {
                  return error;
               }
            }
            break;
         case filter_operation::MERGE: {
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

static TClipRectangle<double> diagonal_region(extVectorFilter *Self, double Left, double Top, double Width,
   double Height)
{
   double diag = dist(0, 0, Width, Height) * INV_SQRT2;
   double x = Self->X.defined() ? double(Self->X) : 0.0;
   double y = Self->Y.defined() ? double(Self->Y) : 0.0;
   double width  = std::max(0.0, Width + (((Self->Width.defined() ? double(Self->Width) : 1.0) - 1.0) * diag));
   double height = std::max(0.0, Height + (((Self->Height.defined() ? double(Self->Height) : 1.0) - 1.0) * diag));
   double left = Left + (x * diag);
   double top  = Top + (y * diag);
   return { left, top, left + width, top + height };
}

//********************************************************************************************************************
// Compute the Target* and Bound* values, which are used by filter effect algorithms to determine placement.  They
// reflect positions *without* transforms.  The caller is expected to apply ClientVector->Transform after making
// normalised coordinate calculations.
//
// The Target* values tell the effects exactly where to render to.
//
// BoundsWidth/Height reflect the bounds of the client vector and its children.  These values are to
// be used by effects to compute their area when PrimitiveUnits = BOUNDING_BOX.
//
// The bounds are measured in the client vector's coordinate system.  Children that are transformed relative to the
// client, e.g. by a nested viewBox, are mapped into that system so that the area matches the rendered content.

static void compute_target_area(extVectorFilter *Self)
{
   TClipRectangle<double> bounds = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0 };
   calc_local_boundary(Self->ClientVector, bounds);
   double boundX = std::round(bounds.left);
   double boundY = std::round(bounds.top);
   Self->BoundWidth  = std::round(bounds.width());
   Self->BoundHeight = std::round(bounds.height());

   if ((Self->Units IS VUNIT::BOUNDING_BOX) and (Self->AspectRatio IS VFA::NONE)) {
      auto region = diagonal_region(Self, boundX, boundY, Self->BoundWidth, Self->BoundHeight);
      Self->TargetX      = std::round(region.left);
      Self->TargetY      = std::round(region.top);
      Self->TargetWidth  = std::round(region.width());
      Self->TargetHeight = std::round(region.height());
   }
   else if (Self->Units IS VUNIT::BOUNDING_BOX) {
      if (Self->X.defined()) Self->TargetX = std::round(boundX + (Self->X * Self->BoundWidth));
      else Self->TargetX = boundX;

      if (Self->Y.defined()) Self->TargetY = std::round(boundY + (Self->Y * Self->BoundHeight));
      else Self->TargetY = boundY;

      if (Self->Width.defined()) Self->TargetWidth = std::round(Self->Width * Self->BoundWidth);
      else Self->TargetWidth = Self->BoundWidth;

      if (Self->Height.defined()) Self->TargetHeight = std::round(Self->Height * Self->BoundHeight);
      else Self->TargetHeight = Self->BoundHeight;
   }
   else { // USERSPACE: Scaled dimensions are measured against the client's viewport rather than the vector.
      if (Self->X.scaled()) Self->TargetX = std::round(Self->X * Self->ClientViewport->vpFixedWidth);
      else if (Self->X.defined()) Self->TargetX = std::round(Self->X);
      else Self->TargetX = boundX;

      if (Self->Y.scaled()) Self->TargetY = std::round(Self->Y * Self->ClientViewport->vpFixedHeight);
      else if (Self->Y.defined()) Self->TargetY = std::round(Self->Y);
      else Self->TargetY = boundY;

      if (Self->Width.scaled()) Self->TargetWidth = std::round(Self->Width * Self->ClientViewport->vpFixedWidth);
      else if (Self->Width.defined()) Self->TargetWidth = Self->Width;
      else Self->TargetWidth = Self->ClientViewport->vpFixedWidth;

      if (Self->Height.scaled()) Self->TargetHeight = std::round(Self->Height * Self->ClientViewport->vpFixedHeight);
      else if (Self->Height.defined()) Self->TargetHeight = Self->Height;
      else Self->TargetHeight = Self->ClientViewport->vpFixedHeight;
   }
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

   auto bi = Self->BankIndex;
   if (bi >= 255) return log.warning(ERR::ArrayFull);

   if (bi >= Self->Bank.size()) Self->Bank.emplace_back(std::make_unique<filter_bitmap>());

   auto clip = Self->scratch_clip();
   #ifdef DEBUG_FILTER_BITMAP
      auto bmp = Self->Bank[bi].get()->get_bitmap(clip.right, clip.bottom, clip, true, Self->UID);
   #else
      auto bmp = Self->Bank[bi].get()->get_bitmap(clip.right, clip.bottom, clip, false, Self->UID);
   #endif

   if (bmp) {
      bmp->ColourSpace = CS::SRGB;
      bmp->Flags &= ~BMF::PREMUL;
      *BitmapResult = bmp;
      Self->BankIndex++;
      return ERR::Okay;
   }
   else return log.warning(ERR::CreateObject);
}

//********************************************************************************************************************
// Returns a rendered bitmap that represents the source.  Where possible, if a bitmap is being referenced then that
// reference will be returned.  Otherwise a new bitmap is allocated and rendered with the effect.  The bitmap must
// not be freed as they are permanently maintained until the VectorFilter is destroyed.

static ERR get_source_bitmap(extFilterEffect *Effect, objBitmap **BitmapResult, filter_input_role Role,
   size_t Slot)
{
   kt::Log log(__FUNCTION__);

   if (not BitmapResult) return log.warning(ERR::NullArgs);
   *BitmapResult = nullptr;
   const auto step = active_filter_step(Effect);
   const size_t index = (Role IS filter_input_role::MIX) ? 1 : Slot;
   if ((not step) or (index >= step->Inputs.size()) or (step->Inputs[index].Role != Role)) {
      return log.warning(ERR::InvalidState);
   }

   auto filter = Effect->Filter;
   kt::SwitchContext ctx(filter);
   const auto &input = step->Inputs[index];
   const auto source_type = input.Source;
   log.msg(VLF::BRANCH|VLF::DETAIL, "%s #%d <- ID: #%u, Type: %d", Effect->className(), Effect->UID,
      input.Producer ? input.Producer->Effect->UID : 0, int(source_type));
   if (not active_filter_step(Effect)) return ERR::InvalidState;

   objBitmap *bmp = nullptr;
   if (source_type IS VSF::GRAPHIC) {
      if (auto error = get_banked_bitmap(filter, &bmp); error != ERR::Okay) return log.warning(error);
      auto sg = get_source_graphic(filter);
      if ((not sg) or (not active_filter_step(Effect))) return log.warning(ERR::NoData);
      gfx::CopyArea(sg, bmp, BAF::NIL, sg->Clip.Left, sg->Clip.Top,
         sg->Clip.Right - sg->Clip.Left, sg->Clip.Bottom - sg->Clip.Top, bmp->Clip.Left, bmp->Clip.Top);
   }
   else if (source_type IS VSF::ALPHA) { // SourceAlpha
      if (auto error = get_banked_bitmap(filter, &bmp); error != ERR::Okay) return log.warning(error);
      auto sg = get_source_graphic(filter);
      if ((not sg) or (not active_filter_step(Effect))) return log.warning(ERR::NoData);
      int dy = bmp->Clip.Top;
      for (int sy = sg->Clip.Top; sy < sg->Clip.Bottom; sy++) {
         const auto src = (const uint32_t *)(sg->Data + (sy * sg->LineWidth));
         auto dest = (uint32_t *)(bmp->Data + (dy * bmp->LineWidth));
         int dx = bmp->Clip.Left;
         for (int sx = sg->Clip.Left; sx < sg->Clip.Right; sx++) dest[dx++] = src[sx] & 0xff000000;
         dy++;
      }
   }
   else if (source_type IS VSF::BKGD) {
      // "Represents an image snapshot of the canvas under the filter region at the time that the filter element is invoked."
      // NOTE: The client needs to specify 'enable-background' in the nearest container element in order to indicate where the background is coming from;
      // additionally it serves as a marker for graphics to be rendered to a separate bitmap (essential for coping with any transformations in the scene graph).
      //
      // Refer to enable-background support in scene_draw.cpp

      if (auto error = get_banked_bitmap(filter, &bmp); error != ERR::Okay) return log.warning(error);

      gfx::DrawRectangle(bmp, 0, 0, bmp->Width, bmp->Height, 0x00000000, BAF::FILL);
      if ((filter->BkgdBitmap) and ((filter->BkgdBitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
         // The renderer shifts Data for scene-coordinate drawing.  CopyArea expects local bitmap coordinates.
         auto save_data = filter->BkgdBitmap->offset(filter->BkgdOriginX, filter->BkgdOriginY);
         gfx::CopyArea(filter->BkgdBitmap, bmp, BAF::NIL,
            filter->VectorClip.left - filter->BkgdOriginX, filter->VectorClip.top - filter->BkgdOriginY,
            filter->VectorClip.width(), filter->VectorClip.height(), bmp->Clip.Left, bmp->Clip.Top);
         filter->BkgdBitmap->Data = save_data;
      }
   }
   else if (source_type IS VSF::BKGD_ALPHA) {
      if (auto error = get_banked_bitmap(filter, &bmp); error != ERR::Okay) return log.warning(error);
      gfx::DrawRectangle(bmp, 0, 0, bmp->Width, bmp->Height, 0x00000000, BAF::FILL);
      if ((filter->BkgdBitmap) and ((filter->BkgdBitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
         int src_left = std::max(filter->VectorClip.left, filter->BkgdBitmap->Clip.Left + filter->BkgdOriginX);
         int src_top = std::max(filter->VectorClip.top, filter->BkgdBitmap->Clip.Top + filter->BkgdOriginY);
         int src_right = std::min(filter->VectorClip.right, filter->BkgdBitmap->Clip.Right + filter->BkgdOriginX);
         int src_bottom = std::min(filter->VectorClip.bottom, filter->BkgdBitmap->Clip.Bottom + filter->BkgdOriginY);

         int dest_x = bmp->Clip.Left + (src_left - filter->VectorClip.left);
         int dest_y = bmp->Clip.Top + (src_top - filter->VectorClip.top);

         if (dest_x < bmp->Clip.Left) {
            src_left += bmp->Clip.Left - dest_x;
            dest_x = bmp->Clip.Left;
         }

         if (dest_y < bmp->Clip.Top) {
            src_top += bmp->Clip.Top - dest_y;
            dest_y = bmp->Clip.Top;
         }

         int width = std::min(src_right - src_left, bmp->Clip.Right - dest_x);
         int height = std::min(src_bottom - src_top, bmp->Clip.Bottom - dest_y);

         if ((width > 0) and (height > 0)) {
            int dy = dest_y;
            for (int sy=src_top; sy < src_top + height; sy++) {
               auto src = (uint32_t *)(filter->BkgdBitmap->Data + (sy * filter->BkgdBitmap->LineWidth));
               auto dest = (uint32_t *)(bmp->Data + (dy * bmp->LineWidth));
               int dx = dest_x;
               for (int sx=src_left; sx < src_left + width; sx++) {
                  dest[dx++] = src[sx] & 0xff000000;
               }
               dy++;
            }
         }
      }
   }
   else if (source_type IS VSF::REFERENCE) {
      bmp = input.Producer->Result;
      if (not bmp) {
         return filter_input_error(Effect, Role, Slot, ERR::NoData, "producer has no result in this draw",
            input.Producer->Effect->UID);
      }
   }
   else if (source_type IS VSF::NONE) {
      *BitmapResult = nullptr;
      return ERR::Continue;
   }
   else {
      log.warning("Effect source %d is not supported.", int(source_type));
      return ERR::NoSupport;
   }

   #if defined(EXPORT_FILTER_BITMAP) && defined (DEBUG_FILTER_BITMAP)
      save_bitmap(bmp, std::to_string(filter->UID) + "_" + std::to_string(filter->ClientVector->UID) + "_source");
   #endif

   if (not active_filter_step(Effect)) return ERR::InvalidState;
   *BitmapResult = bmp;
   return ERR::Okay;
}

//********************************************************************************************************************
// Render the vector client(s) to an internal bitmap that can be used for SourceGraphic and SourceAlpha input.
// If the referenced vector has no content then the result is a bitmap cleared to 0x00000000, as per SVG specs.
// Rendering will occur only once to SourceGraphic, so multiple calls to this function in a filter pipeline are OK.
//
// TODO: It would be efficient to hook into the dirty markers of the client vector so that re-rendering
// occurs only in the event that the client has been modified.

static objBitmap * get_source_graphic(extVectorFilter *Self)
{
   kt::Log log(__FUNCTION__);

   if (!Self->ClientVector) {
      log.warning("No client vector defined.");
      return nullptr;
   }

   if (Self->Rendered) return Self->SourceGraphic.Bitmap; // Source bitmap already drawn at the correct size.

   kt::SwitchContext ctx(Self);

   // The bitmap covers the processing region only, using the same local coordinates as the banked bitmaps.  The
   // client is rendered with a matching origin, which allows content to the left of or above the page to be drawn.

   auto clip = Self->scratch_clip();
   #ifdef DEBUG_FILTER_BITMAP
      auto sg = Self->SourceGraphic.get_bitmap(clip.right, clip.bottom, clip, true, 0, "source_graphic");
   #else
      auto sg = Self->SourceGraphic.get_bitmap(clip.right, clip.bottom, clip, false, 0, "source_graphic");
   #endif

   if (!sg) return nullptr;

   // The SourceGraphic is an sRGB-encoded intermediate, so it inherits the target's preference but not its encoding
   // (LINEAR and NONE).  An explicit Vector.ColourSpace on the client or its ancestors still applies, which is how
   // SVG content resolves to SRGB.

   auto preference = Self->BkgdBitmap ? Self->BkgdBitmap->BlendMode : BLM::AUTO;
   if ((preference IS BLM::LINEAR) or (preference IS BLM::NONE)) preference = BLM::AUTO;

   sg->BlendMode   = preference;
   sg->ColourSpace = CS::SRGB;

   if (!Self->SourceScene) {
      if ((Self->SourceScene = extVectorScene::create::local(
            fl::PageWidth(Self->ClientViewport->Scene->PageWidth),
            fl::PageHeight(Self->ClientViewport->Scene->PageHeight)))) {

         if (!extVectorViewport::create::global(fl::Owner(Self->SourceScene->UID))) return nullptr;
      }
      else return nullptr;
   }
   else if ((Self->ClientViewport->Scene->PageWidth > Self->SourceScene->PageWidth) or
            (Self->ClientViewport->Scene->PageHeight > Self->SourceScene->PageHeight)) {
      acResize(Self->SourceScene, Self->ClientViewport->Scene->PageWidth, Self->ClientViewport->Scene->PageHeight, 0);
   }

   // The client is drawn outside of its scene graph, so the viewport supplies the colour space that the client
   // resolved to in the renderer.  The filter's own ColourSpace applies to effect processing only.

   ((extVector *)Self->SourceScene->Viewport)->ColourSpace = Self->ClientColourSpace;

   auto const save_child = Self->SourceScene->Viewport->Child;
   Self->SourceScene->Viewport->Child = Self->ClientVector;

   auto const save_vector = Self->ClientVector->Next; // Switch off the Next pointer to prevent processing of siblings.
   Self->ClientVector->Next = nullptr;
   Self->Disabled = true; // Turning off the filter is required to prevent infinite recursion.

   gfx::DrawRectangle(sg, 0, 0, sg->Width, sg->Height, 0x00000000, BAF::FILL);
   Self->SourceScene->Bitmap = sg;
   Self->SourceScene->RenderOriginX = Self->VectorClip.left;
   Self->SourceScene->RenderOriginY = Self->VectorClip.top;
   acDraw(Self->SourceScene);
   Self->SourceScene->RenderOriginX = 0;
   Self->SourceScene->RenderOriginY = 0;

   Self->Disabled = false;
   Self->ClientVector->Next = save_vector;
   Self->SourceScene->Viewport->Child = save_child;

   Self->Rendered = true;
   return sg;
}

//********************************************************************************************************************
// Estimates the distance, in bitmap pixels, over which the effect pipeline can move content or sample neighbouring
// pixels.  Pixels further than this from the page cannot affect the visible output.  The estimate is the sum over
// every effect, so it is safe for any arrangement of the pipeline.  Effects with no fixed limit, such as a wrapping
// convolution or a displacement map, return infinity.

static void filter_reach(extVectorFilter *Self, double &ReachX, double &ReachY)
{
   constexpr double UNBOUNDED = std::numeric_limits<double>::infinity();

   ReachX = ReachY = 0;
   for (auto e : Self->Effects) {
      double x = 0, y = 0;
      switch (e->classID()) {
         case CLASSID::BLURFX: {
            auto blur = (extBlurFX *)e;
            blur->radii(x, y);
            if (blur->EdgeMode != FEM::TRANSPARENT) {
               x = (x >= MIN_BLUR_RADIUS) ? UNBOUNDED : 0;
               y = (y >= MIN_BLUR_RADIUS) ? UNBOUNDED : 0;
            }
            else {
               x = std::ceil(std::min(std::max(0.0, x), MAX_BLUR_RADIUS)) + 1;
               y = std::ceil(std::min(std::max(0.0, y), MAX_BLUR_RADIUS)) + 1;
            }
            break;
         }

         case CLASSID::OFFSETFX: {
            int dx, dy;
            ((extOffsetFX *)e)->pixel_offset(dx, dy);
            x = std::abs(dx);
            y = std::abs(dy);
            break;
         }

         case CLASSID::LIGHTINGFX:
            x = y = ((extLightingFX *)e)->reach();
            break;

         case CLASSID::MORPHOLOGYFX:
            x = std::max(((extMorphologyFX *)e)->RadiusX, 0) + 1;
            y = std::max(((extMorphologyFX *)e)->RadiusY, 0) + 1;
            break;

         case CLASSID::CONVOLVEFX: {
            auto convolve = (extConvolveFX *)e;
            if (convolve->EdgeMode IS EM::WRAP) x = y = UNBOUNDED;
            else {
               x = std::ceil(convolve->MatrixColumns * convolve->unitPixelSpacingX()) + 1;
               y = std::ceil(convolve->MatrixRows * convolve->unitPixelSpacingY()) + 1;
            }
            break;
         }

         // Effects that process each pixel independently of its neighbours

         case CLASSID::COLOURFX:
         case CLASSID::COMPOSITEFX:
         case CLASSID::FLOODFX:
         case CLASSID::IMAGEFX:
         case CLASSID::MERGEFX:
         case CLASSID::REMAPFX:
         case CLASSID::SOURCEFX:
         case CLASSID::TURBULENCEFX:
         case CLASSID::WAVEFUNCTIONFX:
            break;

         default: // Including DISPLACEMENTFX, which can sample at any distance
            x = y = UNBOUNDED;
            break;
      }

      ReachX += x;
      ReachY += y;
   }
}

//********************************************************************************************************************
// Defines the VectorClip values utilised by the filter renderers.  The processing region is the filter region,
// limited to the scene page and a margin around it.  The margin allows effects such as blurs and offsets to read
// content that lies beyond the page edges, and it retains source pixels outside the drawing clip that effects may move
// into it.  The scene renderer clips the final composite to the drawable area, including any clipping imposed by
// container viewports.
//
// VectorClip is measured in scene coordinates and can extend beyond the page on every side.  Filter bitmaps are
// addressed in local coordinates relative to its top-left corner.

static ERR set_clip_region(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector)
{
   kt::Log log(__FUNCTION__);

   const double container_width  = Viewport->vpFixedWidth;
   const double container_height = Viewport->vpFixedHeight;

   if ((container_width < 1) or (container_height < 1)) {
      log.warning("Viewport #%d has no size.", Viewport->UID);
      return ERR::NothingDone;
   }

   if (Self->Units IS VUNIT::BOUNDING_BOX) {
      // Diagonal margins are measured in the client's coordinate system before applying its transform.

      TClipRectangle<double> bounds = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0 };
      bool transform_region = Self->AspectRatio IS VFA::NONE;
      if (transform_region) calc_local_boundary(Vector, bounds);
      else calc_full_boundary(Vector, bounds, false /* siblings */, true /* transforms */);

      if ((bounds.right <= bounds.left) or (bounds.bottom <= bounds.top)) {
         // No child vector defines a path for a SourceGraphic.  Default back to the viewport.
         bounds = Viewport->vpBounds;
         transform_region = false;
      }
      auto const bound_width  = bounds.width();
      auto const bound_height = bounds.height();

      if (Self->AspectRatio IS VFA::NONE) {
         // Match the rounded target region used by effects, then map all four corners to the scene.
         auto region = diagonal_region(Self, std::round(bounds.left), std::round(bounds.top),
            std::round(bound_width), std::round(bound_height));
         Self->VectorClip.left   = std::round(region.left);
         Self->VectorClip.top    = std::round(region.top);
         Self->VectorClip.right  = Self->VectorClip.left + std::round(region.width());
         Self->VectorClip.bottom = Self->VectorClip.top + std::round(region.height());
         if (transform_region) {
            agg::path_storage rect;
            rect.move_to(Self->VectorClip.left, Self->VectorClip.top);
            rect.line_to(Self->VectorClip.right, Self->VectorClip.top);
            rect.line_to(Self->VectorClip.right, Self->VectorClip.bottom);
            rect.line_to(Self->VectorClip.left, Self->VectorClip.bottom);
            rect.close_polygon();

            agg::conv_transform<agg::path_storage, agg::trans_affine> path(rect, Vector->Transform);
            auto clip = get_bounds(path);
            Self->VectorClip = { int(std::floor(clip.left)), int(std::floor(clip.top)),
               int(std::ceil(clip.right)), int(std::ceil(clip.bottom)) };
         }
      }
      else {
         if (Self->X.defined()) Self->VectorClip.left = std::round(bounds.left + (Self->X * bound_width));
         else Self->VectorClip.left = std::round(bounds.left);

         if (Self->Y.defined()) Self->VectorClip.top = std::round(bounds.top + (Self->Y * bound_height));
         else Self->VectorClip.top = std::round(bounds.top);

         if (Self->Width.defined()) {
            Self->VectorClip.right = Self->VectorClip.left + std::round(Self->Width * bound_width);
         }
         else Self->VectorClip.right = Self->VectorClip.left + std::round(bound_width);

         if (Self->Height.defined()) {
            Self->VectorClip.bottom = Self->VectorClip.top + std::round(Self->Height * bound_height);
         }
         else Self->VectorClip.bottom = Self->VectorClip.top + std::round(bound_height);
      }
   }
   else { // USERSPACE
      double x, y, w, h;
      if (Self->X.scaled()) x = std::round(Self->X * container_width);
      else if (Self->X.defined()) x = std::round(Self->X);
      else x = 0;

      if (Self->Y.scaled()) y = std::round(Self->Y * container_height);
      else if (Self->Y.defined()) y = std::round(Self->Y);
      else y = 0;

      if (Self->Width.scaled()) w = std::round(Self->Width * container_width);
      else if (Self->Width.defined()) w = std::round(Self->Width);
      else w = std::round(container_width);

      if (Self->Height.scaled()) h = std::round(Self->Height * container_height);
      else if (Self->Height.defined()) h = std::round(Self->Height);
      else h = std::round(container_height);

      agg::path_storage rect;
      rect.move_to(x, y);
      rect.line_to(x + w, y);
      rect.line_to(x + w, y + h);
      rect.line_to(x, y + h);
      rect.close_polygon();

      agg::conv_transform<agg::path_storage, agg::trans_affine> path(rect, Vector->Transform);
      Self->VectorClip = get_bounds<agg::conv_transform<agg::path_storage, agg::trans_affine>, int>(path);
   }

   // The margin is limited so that unbounded effects do not process an excessive area around the page, and it is
   // dropped if the processing region would become unreasonably large.

   const int page_width  = Viewport->Scene->PageWidth;
   const int page_height = Viewport->Scene->PageHeight;
   const double max_margin = std::max(page_width, page_height);

   double reach_x, reach_y;
   filter_reach(Self, reach_x, reach_y);

   const auto region = Self->VectorClip;
   auto limit_region = [&](int MarginX, int MarginY) {
      Self->VectorClip.left   = std::max(region.left, -MarginX);
      Self->VectorClip.top    = std::max(region.top, -MarginY);
      Self->VectorClip.right  = std::min(region.right, page_width + MarginX);
      Self->VectorClip.bottom = std::min(region.bottom, page_height + MarginY);
   };

   limit_region(int(std::ceil(std::min(reach_x, max_margin))), int(std::ceil(std::min(reach_y, max_margin))));

   const int64_t area = int64_t(Self->VectorClip.width()) * int64_t(Self->VectorClip.height());
   if ((area > MAX_FILTER_MARGIN_AREA) and (area > int64_t(page_width) * int64_t(page_height))) {
      limit_region(0, 0);
   }

   if ((Self->VectorClip.bottom <= Self->VectorClip.top) or (Self->VectorClip.right <= Self->VectorClip.left)) {
      // This is a common error, and typically triggers from resizing to small coverage areas.
      return ERR::InvalidDimension;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Compute the primitive subregion of an effect in the client's user space, which is the coordinate system of the
// Target* values.  Any of x, y, width and height that are undefined default to the union of the subregions of the
// referenced effects.  If there are no referenced effects, or any input is a standard input such as SourceGraphic,
// the filter region is the default.  Effects are rendered in sequence, so referenced effects are always computed
// before the effects that use them.  Bounds are only required if PrimitiveUnits is BOUNDING_BOX.

static void compute_subregion(extVectorFilter *Self, const filter_step &Step, const TClipRectangle<double> &Bounds)
{
   const TClipRectangle<double> filter_region = { Self->TargetX, Self->TargetY,
      Self->TargetX + Self->TargetWidth, Self->TargetY + Self->TargetHeight };

   TClipRectangle<double> inputs = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
   bool referenced = false, standard = false;

   for (const auto &input : Step.Inputs) {
      if (input.Producer) {
         inputs.expanding(input.Producer->Effect->Subregion);
         referenced = true;
      }
      else if (input.Source != VSF::NONE) standard = true;
   }
   const auto effect = Step.Effect;

   auto region = (referenced and (not standard)) ? inputs : filter_region;

   double x = region.left, y = region.top, width = region.width(), height = region.height();
   if (Self->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      if (effect->X.defined()) x = Bounds.left + (effect->X * Bounds.width());
      if (effect->Y.defined()) y = Bounds.top + (effect->Y * Bounds.height());
      if (effect->Width.defined()) width = effect->Width * Bounds.width();
      if (effect->Height.defined()) height = effect->Height * Bounds.height();
   }
   else { // Explicit percentages are relative to the client's viewport in user space.
      const double viewport_width = viewport_coordinate_width(Self->ClientViewport);
      const double viewport_height = viewport_coordinate_height(Self->ClientViewport);
      if (effect->X.scaled()) x = effect->X * viewport_width;
      else if (effect->X.defined()) x = effect->X;
      if (effect->Y.scaled()) y = effect->Y * viewport_height;
      else if (effect->Y.defined()) y = effect->Y;
      if (effect->Width.scaled()) width = effect->Width * viewport_width;
      else if (effect->Width.defined()) width = effect->Width;
      if (effect->Height.scaled()) height = effect->Height * viewport_height;
      else if (effect->Height.defined()) height = effect->Height;
   }

   effect->Subregion = { x, y, x + width, y + height };
}

//********************************************************************************************************************
// Clear the pixels of an effect's output that fall outside of its primitive subregion.  Pixels are tested at their
// centres in user space, which matches the sampling used by TurbulenceFX.  Nothing is done if the subregion covers
// the filter region, which is the most common case.

static void clip_to_subregion(extVectorFilter *Self, extFilterEffect *Effect)
{
   const auto &sub = Effect->Subregion;
   if ((sub.left <= Self->TargetX) and (sub.top <= Self->TargetY) and
       (sub.right >= Self->TargetX + Self->TargetWidth) and (sub.bottom >= Self->TargetY + Self->TargetHeight)) {
      return;
   }

   auto bmp = Effect->Target;
   if ((not bmp) or (bmp->BytesPerPixel != 4)) return;

   auto inverse = Self->Transform;
   const double determinant = inverse.determinant();
   if ((not std::isfinite(determinant)) or (determinant IS 0)) return;
   inverse.invert();

   for (int y=bmp->Clip.Top; y < bmp->Clip.Bottom; y++) {
      auto pixel = (uint32_t *)(bmp->Data + (y * bmp->LineWidth));
      for (int x=bmp->Clip.Left; x < bmp->Clip.Right; x++) {
         double px = x + 0.5, py = y + 0.5;
         inverse.transform(&px, &py);
         if (not sub.hit_test(px, py)) pixel[x] = 0;
      }
   }
}

//********************************************************************************************************************
// Main rendering routine for filter effects.  Called by the scene graph renderer whenever a vector uses a filter.
// ColourSpace is the client's resolved colour space in the renderer, which can be inherited from a vector that is not
// a physical ancestor, e.g. the viewport that consumes a live pattern.

ERR render_filter(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector,
   objBitmap *BkgdBitmap, VCS ColourSpace, objBitmap **Output, int BkgdOriginX, int BkgdOriginY)
{
   kt::Log log(__FUNCTION__);

   if ((not Vector) or (not Viewport) or (not Output)) return log.warning(ERR::NullArgs);
   *Output = nullptr;
   if (Self->Disabled) return ERR::NothingDone;
   if (Self->Drawing) return log.warning(ERR::Recursion);

   SharedObjectAccess access(Self);
   if (not access.granted()) return access.error;
   kt::SwitchContext context(Self);

   Self->Drawing = true;
   Self->ActiveRevision = Self->GraphRevision;
   auto graph_changed = [&]() {
      return (Self->GraphRevision != Self->ActiveRevision) or Self->collecting();
   };
   bool completed = false;
   auto cleanup = kt::Defer([&]() {
      Self->ActiveStep = nullptr;
      Self->Drawing = false;
      if (not completed) {
         for (auto effect : Self->Effects) effect->Target = nullptr;
      }
   });

   for (auto effect : Self->Effects) effect->Target = nullptr;
   if (Self->Effects.empty()) return log.warning(ERR::FieldNotSet);
   filter_plan plan;
   if (auto error = prepare_filter_plan(Self, plan); error != ERR::Okay) return error;

   CSTRING filter_name = Self->Name;
   if ((!filter_name) or (!filter_name[0])) filter_name = "Unnamed";
   CSTRING vector_name = Vector->Name;
   if ((!vector_name) or (!vector_name[0])) vector_name = "Unnamed";
   log.msg(VLF::BRANCH|VLF::DETAIL, "Rendering '%s' filter content for %s #%d '%s'.", filter_name,
      Vector->Class->ClassName.c_str(), Vector->UID, vector_name);

   Self->ClientViewport = Viewport;
   Self->ClientVector   = Vector;
   Self->BkgdBitmap     = (plan.ReadsBackground or plan.ReadsSourceGraphic) ? BkgdBitmap : nullptr;
   Self->BkgdOriginX = BkgdOriginX;
   Self->BkgdOriginY = BkgdOriginY;
   Self->ClientColourSpace = ColourSpace;
   Self->Rendered       = false; // Set to true when SourceGraphic is rendered
   Self->BankIndex      = 0;

   // Calculate Self->Target* and Self->Bound* values.  The reach of the effects depends on these, so they are
   // computed before the clip region.

   compute_target_area(Self);

   if (auto error = set_clip_region(Self, Viewport, Vector); error != ERR::Okay) return error;

   Self->Transform = Vector->Transform;
   Self->Transform.translate(-Self->VectorClip.left, -Self->VectorClip.top);

   // Effect subregions in objectBoundingBox units are relative to the unrounded bounds of the client.

   TClipRectangle<double> bounds = { 0, 0, 0, 0 };
   if (Self->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      bounds = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0 };
      calc_local_boundary(Self->ClientVector, bounds);
   }

   if (graph_changed()) {
      log.warning("Filter #%d changed while preparing its bounds.", Self->UID);
      return ERR::InvalidState;
   }

   objBitmap *unused = nullptr;
   for (auto &step : plan.Steps) {
      const auto effect = step.Effect;
      log.detail("Effect: %s #%u, Consumers: %zu", effect->className(), effect->UID, step.Consumers);
      Self->ActiveStep = &step;

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

      compute_subregion(Self, step, bounds);
      if (auto error = acDraw(effect); error != ERR::Okay) return log.warning(error);
      if (graph_changed() or effect->collecting()) {
         log.warning("Filter #%d changed while drawing.", Self->UID);
         return ERR::InvalidState;
      }

      effect->Target = target;
      clip_to_subregion(Self, effect);
      step.Result = target;
   }

   auto out = plan.Final->Result;

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
         const auto peak_live_surfaces = int(Self->BankIndex) + int(Self->Rendered);
         log.detail("Filter storage: retained_bytes=%zu peak_live_surfaces=%d", retained_bytes, peak_live_surfaces);
      }
   #endif

   if (graph_changed()) return log.warning(ERR::InvalidState);
   *Output = out;
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

   if (Self->Drawing) return log.warning(ERR::InUse);

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
   Self->BankIndex = 0;

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
ResX: Width of the intermediate images, measured in pixels.

The combination of ResX and #ResY define the available space for rendering of filter effects.  It is recommended that
the client does not set these values because the default `1:1` pixel ratio is appropriate in the majority of
circumstances.

-FIELD-
ResY: Height of the intermediate images, measured in pixels.

The combination of #ResX and ResY define the available space for rendering of filter effects.  It is recommended that
the client does not set these values because the default `1:1` pixel ratio is appropriate in the majority of
circumstances.

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
