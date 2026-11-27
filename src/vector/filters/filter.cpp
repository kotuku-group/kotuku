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

If the processing region or the pipeline exceeds an internal resource limit, the renderer logs the failure and
omits the filter output.  Processing margins are preserved for successful renders.

It is important to note that filter effects are CPU intensive tasks and real-time performance may be disappointing.
If this is an issue, consider pre-rendering the filter effects in advance and caching the results in memory or files.

It is a requirement that VectorFilter objects are owned by the @VectorScene they are targeting.

-END-

*********************************************************************************************************************/

#include <bs_thread_pool.h>
#include "filter_sampling.h"
#include "../../link/filter_resource.h"

//#define EXPORT_FILTER_BITMAP

//********************************************************************************************************************
// Resource limits for a single filter pipeline.

constexpr size_t MAX_FILTER_EFFECTS = 4096;                     // Maximum effects in one pipeline
constexpr size_t MAX_FILTER_INPUTS = 65536;                     // Maximum input bindings across all effects
constexpr size_t MAX_FILTER_SURFACE_BYTES = 256 * 1024 * 1024;  // Maximum live surface storage during a draw
constexpr size_t MAX_RETAINED_FILTER_BYTES = 64 * 1024 * 1024;  // Maximum idle storage retained between draws

enum class filter_surface_format { RGBA8, SCALAR_F32, VECTOR_F32 };

//********************************************************************************************************************
// A storage slot for an intermediate filter result.  Slots own their backing memory and are recycled through the
// filter's free list.  A bitmap is attached on demand when a slot holds an RGBA8 image.

struct filter_surface {
   filter_surface *Next = nullptr; // Link in the free list or a working list
   std::unique_ptr<uint8_t[]> Data; // Backing memory, shared with the attached bitmap if any
   size_t Bytes; // Size of Data
   bool Numerical; // True if the slot holds float samples; such slots are only recycled for numerical results
   OBJECTID BitmapID = 0; // Bitmap bound to Data on demand, otherwise zero

   ~filter_surface() {
      if (auto object = BitmapID ? GetObjectPtr(BitmapID) : nullptr; object) {
         auto bitmap = (objBitmap *)object;
         bitmap->Data = nullptr;
         bitmap->CreatorMeta = nullptr;
         FreeResource(bitmap);
      }
   }
};

//********************************************************************************************************************
// Enumerations that describe how effects consume their inputs.

enum class filter_input_role { INPUT, MIX, MERGE, NAMED };
enum class filter_inputs { GENERATOR, SINGLE, DUAL, MERGE };
enum class filter_input_kind { IMAGE, SCALAR_MAP, VECTOR_MAP };
enum class filter_access { POINTWISE, BOUNDED, FULL_INPUT };
enum class filter_map_channel { RED, GREEN, BLUE, ALPHA, LUMINANCE };
enum class filter_map_transfer { STORED, LINEAR };
enum class filter_bitmap_conversion { NONE, UNIT_CHANNELS };
using filter_map_value = std::array<float, 2>;

static_assert((sizeof(float) IS 4) and std::numeric_limits<float>::is_iec559);
constexpr float INVALID_FILTER_SAMPLE = std::numeric_limits<float>::quiet_NaN();

//********************************************************************************************************************
// Describes how far an effect reads beyond each output pixel.  The renderer sums the reach of every effect to size
// the processing margin around the filter region.

struct filter_sampling {
   static constexpr double UNBOUNDED = std::numeric_limits<double>::infinity();
   filter_access Access = filter_access::POINTWISE; // How the effect reads its input relative to each output pixel
   double X = 0, Y = 0; // Conservative device-pixel reach.  Full input may apply along only one axis.
   bool EntireRegion = false; // Process the full filter region, beyond the page margin, if it can be allocated
};

//********************************************************************************************************************
// The input arrangement and sampling reach of an effect class.  See describe_filter_operation().

struct filter_step;

struct filter_operation {
   filter_inputs Inputs; // The standard input fields that the effect consumes
   // Returns the effect's sampling reach for its current settings and planned inputs.  The default is pointwise.
   filter_sampling (*Sampling)(const filter_step &) = [](const filter_step &) -> filter_sampling { return {}; };
   filter_input_kind Primary = filter_input_kind::IMAGE; // Contract kind of the ordinary Input binding
   filter_surface_format Format = filter_surface_format::RGBA8; // Result format for the effect's current settings
   bool InputFormat = false; // The result takes the format of the Input producer, overriding Format and Primary
   bool InputRegion = true;  // The Input binding contributes to the default primitive subregion
   filter_input_kind Secondary = filter_input_kind::IMAGE; // Contract kind of the Mix binding of DUAL effects
};

//********************************************************************************************************************
// The requirements that an effect places on one of its inputs, including how numerical maps are derived from RGBA8
// producers and how samples outside of the valid region are resolved.

struct filter_input_contract {
   filter_input_kind Kind = filter_input_kind::IMAGE; // An RGBA8 image, or a scalar or vector map
   bool ContributesRegion = true; // If true, the input is considered when defaulting the primitive subregion
   bool Optional = false; // An image binding can be unset if the consumer supplies its own fallback
   // RGBA8 channels that supply the map's X and Y components.  YChannel applies to vector maps only.
   filter_map_channel XChannel = filter_map_channel::ALPHA, YChannel = filter_map_channel::ALPHA;
   filter_map_transfer Transfer = filter_map_transfer::STORED; // LINEAR decodes sRGB colour channels before use
   filter_map_value Fallback = {}; // Map value for an unset input and for invalid or out-of-bounds samples
   filter_interpolation Interpolation = filter_interpolation::NEAREST; // Map sampling between pixel centres
   FEM Edge = FEM::TRANSPARENT; // Edge mode for map samples outside of the producer's valid region
   filter_sampling Sampling = {}; // Reach of the input, added to the pipeline's processing margin
   filter_bitmap_conversion BitmapConversion = filter_bitmap_conversion::NONE; // Numerical-to-RGBA8 adapter for images
   bool TransparentFallback = false; // Colour channels of RGBA8 map pixels with zero alpha read as the Fallback
   bool NativeOnly = false; // A numerical input requires a native producer rather than RGBA8 map channels
   // Any output pixel can read any pixel of the producer's subregion, which is then always processed in full.
   bool WholeProducer = false;
};

//********************************************************************************************************************
// A named input slot declared by an effect extension.  The client assigns Source, and Reference when Source is
// VSF::REFERENCE, through set_named_filter_input().

struct filter_named_input {
   CSTRING Name; // Slot name, matched case-sensitively
   filter_input_contract Contract; // Requirements fixed by the effect
   VSF Source = VSF::NONE; // Assigned source; NONE leaves the slot unset
   objFilterEffect *Reference = nullptr; // Producer when Source is VSF::REFERENCE, otherwise null
};

//********************************************************************************************************************
// Optional extension for effects that declare named inputs or produce numerical output.  Slot names, contracts and
// the output format are fixed by the effect.  Its Draw action writes the result.

struct filter_effect_extension {
   std::span<filter_named_input> Inputs = {}; // Named input slots, owned by the effect
   filter_surface_format Format = filter_surface_format::RGBA8; // Format of the effect's result
   // Optional.  Returns false for a slot that the effect's current settings do not read, which the planner then
   // treats as unset so that its binding is neither validated nor drawn for.  Null if every slot is read.
   bool (*Active)(const extFilterEffect *, size_t Slot) = nullptr;
};

enum class filter_alpha { STRAIGHT, PREMULTIPLIED };

constexpr int NORMAL_MAP_CENTRE = 128;
constexpr int NORMAL_MAP_RANGE = 127;

//********************************************************************************************************************
// A read-only view of an intermediate result.  Valid is half-open in local coordinates, Origin locates (0,0) in scene
// device pixels, and Stride is in bytes.  Bitmap, Encoding and Alpha describe RGBA8 only.  Numerical rows hold one
// float or two interleaved floats per pixel.

struct filter_surface_view {
   const objBitmap *Bitmap = nullptr;     // Adapter for bitmap-based readers.  Null for numerical surfaces.
   const uint8_t *Data = nullptr;         // First byte of row zero
   int Width = 0, Height = 0, Stride = 0; // Surface dimensions in pixels; Stride is in bytes
   int OriginX = 0, OriginY = 0;          // Scene device-pixel position of local (0,0)
   CS Encoding = CS::SRGB;                // Colour encoding of RGBA8 data
   filter_alpha Alpha = filter_alpha::STRAIGHT; // Alpha representation of RGBA8 data
   TClipRectangle<int> Valid = { 0 };           // Half-open region of initialised pixels in local coordinates
   filter_surface_format Format = filter_surface_format::RGBA8; // Pixel format of Data
   bool NormalMap = false; // RGBA8 stores signed normal components as data rather than colour

   const uint8_t * row(int Y) const { return Data + std::ptrdiff_t(Y) * Stride; }
};

//********************************************************************************************************************
// Reads a height along a row, using the bitmap's stored alpha or a native float sample.  Missing samples read as zero.

template<bool Scalar>
static float filter_height_sample(const uint8_t *Row, int X, int Alpha)
{
   if constexpr (Scalar) {
      const float height = ((const float *)Row)[X];
      return std::isfinite(height) ? height : 0.0f;
   }
   else return Row[std::ptrdiff_t(X) * 4 + Alpha] / 255.0f;
}

//********************************************************************************************************************
// A resolved input binding in the draw plan.  Producer is set for references to earlier effects, otherwise Source
// names a standard source.

struct filter_step;

struct filter_input {
   VSF Source;               // Source kind, or VSF::REFERENCE for a producer
   filter_input_role Role;   // The effect field that declared the binding
   size_t Slot;              // SourceList index for MERGE, named slot index for NAMED, otherwise zero
   filter_step *Producer;    // Producing step for references, otherwise null
   filter_input_contract Contract; // Requirements copied from the consumer
};

//********************************************************************************************************************
// One effect in the draw plan.  Consumers counts the remaining bindings that read Result, which is released when the
// count reaches zero.

struct filter_step {
   extFilterEffect *Effect;              // The effect drawn by this step
   filter_operation Operation;           // Input arrangement and sampling reach of the effect's class
   std::span<const filter_input> Inputs; // This step's bindings within filter_plan::Inputs
   filter_surface_format Format = filter_surface_format::RGBA8; // Format of Result
   TClipRectangle<double> Subregion = { 0.0 }; // Client user coordinates.
   size_t Consumers = 0;               // Remaining bindings that read Result
   filter_surface *Surface = nullptr;  // Storage for Result while it is live
   filter_surface_view Result = {};    // Output of the step, valid after it has drawn
};

//********************************************************************************************************************
// A lazily populated standard source (SourceGraphic, SourceAlpha, BackgroundImage or BackgroundAlpha), shared by its
// readers until the last planned use.

struct filter_standard_source {
   size_t Consumers = 0;              // Remaining planned reads
   filter_surface *Surface = nullptr; // Storage for View once populated
   filter_surface_view View = {};     // Populated on first read; empty until then
};

//********************************************************************************************************************
// The validated pipeline for a single draw, built by prepare_filter_plan().  Steps follow the effect order and their
// Inputs spans index into the shared Inputs array.

struct filter_plan {
   std::vector<filter_step> Steps;   // One step per effect, in authored order
   std::vector<filter_input> Inputs; // Bindings for all steps; reserved in advance so that spans remain valid
   filter_step *Final = nullptr;     // The last step, which supplies the filter's output
   // Standard sources from VSF::GRAPHIC to VSF::BKGD_ALPHA.  Index with standard_source().
   std::array<filter_standard_source, size_t(VSF::BKGD_ALPHA) - size_t(VSF::GRAPHIC) + 1> StandardSources;

   filter_standard_source & standard_source(VSF Source) {
      return StandardSources[size_t(Source) - size_t(VSF::GRAPHIC)];
   }
};

//********************************************************************************************************************
// State for a filter draw in progress.  It lives on the stack of render_filter() and is reachable from effects
// through extVectorFilter::Render for the duration of the draw.

struct filter_render_context {
   extVector *ClientVector;           // The vector that the filter is applied to
   extVectorViewport *ClientViewport; // The client's nearest viewport
   objBitmap *BkgdBitmap;             // Destination bitmap; null unless SourceGraphic or a background source is read
   int BkgdOriginX, BkgdOriginY;      // Scene device-pixel position of BkgdBitmap (0,0)
   VCS ClientColourSpace;             // The client's resolved colour space, used when capturing SourceGraphic
   uint64_t Revision;                 // Filter GraphRevision when the draw started.  A mismatch aborts the draw.
   bool Bounded = false;  // Limit processing to the page and margin, and tiles, even where effects need more
   bool Expanded = false; // True if processing extends beyond the page and margin for the entire region or a whole
                          // named input, which a bounded retry can avoid
   filter_plan Plan;
   const filter_step *ActiveStep = nullptr; // Step currently drawing, otherwise null
   TClipRectangle<int> VectorClip = { 0 }; // Processing rectangle in scene pixels.  Left/top are the bitmap origin.
   TClipRectangle<double> ClientBounds = { 0.0 }; // Unrounded bounds of the subtree in client user coordinates.
   TClipRectangle<double> ClientPathBounds = { 0.0 }; // Client path alone in user coordinates.
   TClipRectangle<double> SceneBounds = { 0.0 };   // Unrounded subtree bounds in scene coordinates for per-axis units.
   TClipRectangle<double> FilterRegion = { 0.0 };  // Default primitive region in client user coordinates.
   double BoundWidth = 0, BoundHeight = 0;         // Rounded subtree dimensions used by legacy distance scaling.
   double ViewportWidth = 0, ViewportHeight = 0;   // Viewport dimensions in its user coordinates (viewBox if set).
   double ContainerWidth = 0, ContainerHeight = 0; // Fixed viewport dimensions used by legacy filter region lengths.
   VUNIT FilterUnits, PrimitiveUnits; // Filter settings captured at the start of the draw
   VFA AspectRatio;                // Filter setting captured at the start of the draw
   agg::trans_affine ToScene;      // Client user coordinates to scene coordinates.
   agg::trans_affine Transform;    // Client user coordinates to local bitmap coordinates.
   agg::trans_affine Inverse;      // Local bitmap coordinates to client user coordinates.
   filter_surface *WorkingSurfaces = nullptr; // Scratch and converted inputs, released after each step
   int BitmapStride = 0;   // Row stride of RGBA8 surfaces covering VectorClip
   size_t BitmapBytes = 0; // Size of RGBA8 surfaces covering VectorClip
   size_t LiveBytes = 0, PeakLiveBytes = 0; // Checked-out surface storage, enforced by MAX_FILTER_SURFACE_BYTES
   size_t LiveSurfaces = 0, PeakLiveSurfaces = 0; // Checked-out surface count
   size_t Allocations = 0, AllocatedBytes = 0, Reuses = 0; // Storage statistics for diagnostic logging

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

using filter_positions = std::unordered_map<objFilterEffect *, size_t>;

//********************************************************************************************************************
// Returns the number of floats per pixel for a numerical surface format.

static int filter_components(filter_surface_format Format)
{
   return (Format IS filter_surface_format::VECTOR_F32) ? 2 : 1;
}

//********************************************************************************************************************
// Reads one pixel from a numerical surface.  Returns false if any component is not finite.

static bool read_numeric_sample(const filter_surface_view &Source, int X, int Y, filter_map_value &Value)
{
   const auto components = filter_components(Source.Format);
   const auto pixel = (const float *)Source.row(Y) + std::ptrdiff_t(X) * components;
   Value = { pixel[0], (components IS 2) ? pixel[1] : 0.0f };
   return std::isfinite(Value[0]) and std::isfinite(Value[1]);
}

//********************************************************************************************************************
// Samples a scalar or vector map from a named input, applying the input contract's channel selection, transfer
// curve, interpolation, edge mode and fallback.

struct filter_map_view {
   filter_surface_view Source;
   filter_input_contract Contract;

   // Scene coordinates use pixel centres at Origin + local index + 0.5.

   filter_map_value sample(double X, double Y) const {
      if (not Source.Data) return Contract.Fallback;
      const double x = X - Source.OriginX - 0.5, y = Y - Source.OriginY - 0.5;
      constexpr double limit = 0x1p52;
      if (not ((x > -limit) and (x < limit) and (y > -limit) and (y < limit))) return Contract.Fallback;

      if (Contract.Interpolation IS filter_interpolation::NEAREST) {
         return tap(int64_t(std::nearbyint(x)), int64_t(std::nearbyint(y)));
      }

      const int64_t x0 = int64_t(std::floor(x)), y0 = int64_t(std::floor(y));
      const double tx = x - double(x0), ty = y - double(y0);
      const auto p00 = tap(x0, y0), p10 = tap(x0 + 1, y0), p01 = tap(x0, y0 + 1), p11 = tap(x0 + 1, y0 + 1);
      filter_map_value value;
      for (size_t i = 0; i < value.size(); i++) {
         const auto top = std::lerp(double(p00[i]), double(p10[i]), tx);
         const auto bottom = std::lerp(double(p01[i]), double(p11[i]), tx);
         value[i] = float(std::lerp(top, bottom, ty));
      }
      return value;
   }

   private:
   filter_map_value tap(int64_t X, int64_t Y) const {
      const int x = filter_edge_index(X, Source.Valid.left, Source.Valid.right, Contract.Edge);
      const int y = filter_edge_index(Y, Source.Valid.top, Source.Valid.bottom, Contract.Edge);
      if ((x < 0) or (y < 0)) return Contract.Fallback;

      if (Source.Format != filter_surface_format::RGBA8) {
         filter_map_value value;
         return read_numeric_sample(Source, x, y, value) ? value : Contract.Fallback;
      }

      const auto pixel = Source.row(y) + std::ptrdiff_t(x) * 4;
      const auto &format = *Source.Bitmap->ColourFormat;
      const double alpha = double(pixel[format.AlphaPos >> 3]) / 255.0;
      const bool empty = Contract.TransparentFallback and (not pixel[format.AlphaPos >> 3]);

      auto colour = [&](int Shift) {
         double value = double(pixel[Shift >> 3]) / 255.0;
         if (Source.Alpha IS filter_alpha::PREMULTIPLIED) {
            value = alpha > 0.0 ? std::min(1.0, value / alpha) : 0.0;
         }

         if ((Contract.Transfer IS filter_map_transfer::LINEAR) and (Source.Encoding IS CS::SRGB)) {
            value = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
         }
         return value;
      };

      auto channel = [&](filter_map_channel Channel) -> float {
         using enum filter_map_channel;
         switch (Channel) {
            case RED: return float(colour(format.RedPos));
            case GREEN: return float(colour(format.GreenPos));
            case BLUE: return float(colour(format.BluePos));
            case ALPHA: return float(alpha);
            case LUMINANCE:
               return float(0.2126 * colour(format.RedPos) + 0.7152 * colour(format.GreenPos) +
                  0.0722 * colour(format.BluePos));
         }
         return 0.0f;
      };

      auto component = [&](filter_map_channel Channel, size_t Index) -> float {
         if (empty and (Channel != filter_map_channel::ALPHA)) return Contract.Fallback[Index];
         return channel(Channel);
      };

      return { component(Contract.XChannel, 0),
         (Contract.Kind IS filter_input_kind::VECTOR_MAP) ? component(Contract.YChannel, 1) : 0.0f };
   }
};

//********************************************************************************************************************
// Forward declarations.

static bool transform_filter_region(TClipRectangle<double> &, const agg::trans_affine &);
static ERR get_source_view(extFilterEffect *, filter_surface_view &, filter_input_role, size_t = 0);
static ERR get_source_bitmap(extFilterEffect *, const objBitmap **, filter_input_role, size_t = 0);
static ERR get_source_graphic(extVectorFilter *, filter_surface_view &);
static ERR get_numeric_target(extFilterEffect *, filter_surface_view &, float *&);
static ERR get_named_filter_bitmap(extFilterEffect *, std::string_view, filter_surface_view &);
static const filter_input * planned_named_input(const extFilterEffect *, std::string_view);
static ERR working_input(extFilterEffect *, const filter_surface_view &, CS, filter_alpha, filter_surface_view &);
static ERR premultiplied_input(extFilterEffect *, const filter_surface_view &, CS, filter_surface_view &);
static ERR draw_filter_capture(const extVectorFilter *, extVectorScene *, extVector *, objBitmap *, int, int);
static ERR get_filter_bitmap(extVectorFilter *, filter_surface *&, objBitmap *&);
template<class T> static ERR get_filter_scratch(extFilterEffect *, size_t, T *&);
static void release_filter_surfaces(extVectorFilter *, filter_surface *&, const filter_surface * = nullptr);
static void unlink_filter_effect(extFilterEffect *);
static void clear_filter_references(extFilterEffect *, objFilterEffect * = nullptr);
static void update_filter_links(extVectorFilter *);
static ERR check_filter_reference(extFilterEffect *, objFilterEffect *, filter_input_role, size_t = 0,
   const filter_positions * = nullptr);
static ERR validate_native_filter_input(const extFilterEffect *, const filter_named_input &, objFilterEffect *);
static ERR set_named_filter_input(extFilterEffect *, std::string_view, VSF, objFilterEffect * = nullptr);
static ERR get_filter_map(extFilterEffect *, std::string_view, filter_map_view &);

//********************************************************************************************************************
// Returns true if every component of the transform is finite.

static bool finite_transform(const agg::trans_affine &Transform)
{
   return std::isfinite(Transform.sx) and std::isfinite(Transform.sy) and
      std::isfinite(Transform.shx) and std::isfinite(Transform.shy) and
      std::isfinite(Transform.tx) and std::isfinite(Transform.ty);
}

//********************************************************************************************************************
// Inverts the transform in place.  Returns false if the transform is singular or the result is not finite.

static bool invert_filter_transform(agg::trans_affine &Transform)
{
   const double determinant = Transform.determinant();
   if ((not finite_transform(Transform)) or (not std::isfinite(determinant)) or (determinant IS 0)) return false;
   Transform.invert();
   return finite_transform(Transform);
}

//********************************************************************************************************************
// Returns true if the edges and dimensions of the region are finite.

static bool finite_region(const TClipRectangle<double> &Region)
{
   return std::isfinite(Region.left) and std::isfinite(Region.top) and
      std::isfinite(Region.right) and std::isfinite(Region.bottom) and
      std::isfinite(Region.width()) and std::isfinite(Region.height());
}

//********************************************************************************************************************
// Returns true if the bounds can be rasterised.  Headroom is reserved for additions and differences in the
// rasteriser's fixed-point arithmetic.

static bool filter_raster_bounds(const TClipRectangle<double> &Bounds)
{
   constexpr double limit = double(agg::poly_max_coord) / agg::poly_subpixel_scale;
   return (Bounds.left >= -limit) and (Bounds.top >= -limit) and
      (Bounds.right <= limit) and (Bounds.bottom <= limit);
}

//********************************************************************************************************************
// Maps the centre of pixel (X, Y) through the transform.

static void map_filter_pixel_centre(const agg::trans_affine &Transform, int X, int Y, double &PX, double &PY)
{
   PX = filter_pixel_centre(X);
   PY = filter_pixel_centre(Y);
   Transform.transform(&PX, &PY);
}

//********************************************************************************************************************
// Returns an RGBA sampler that is limited to the bitmap's clipping region.

static filter_rgba_sampler filter_sampler(const objBitmap *Bitmap)
{
   const auto &clip = Bitmap->Clip;
   return { Bitmap->Data, Bitmap->LineWidth, clip.Left, clip.Top, clip.Right, clip.Bottom };
}

//********************************************************************************************************************
// Describes an RGBA8 filter bitmap as a surface view, positioned at the origin of the processing rectangle.

static filter_surface_view filter_view(const filter_render_context &Render, const objBitmap *Bitmap)
{
   return { Bitmap, Bitmap->Data, Bitmap->Width, Bitmap->Height, Bitmap->LineWidth,
      Render.VectorClip.left, Render.VectorClip.top, Bitmap->ColourSpace,
      ((Bitmap->Flags & BMF::PREMUL) != BMF::NIL) ? filter_alpha::PREMULTIPLIED : filter_alpha::STRAIGHT,
      { Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right, Bitmap->Clip.Bottom } };
}

//********************************************************************************************************************
// Returns the plan step that is currently drawing Effect, or null if the effect is not active or the filter graph has
// changed since the plan was built.  Helpers re-check it after any operation that can run client code.

static const filter_step * active_filter_step(const extFilterEffect *Effect)
{
   const auto filter = Effect->Filter;
   const auto render = filter ? filter->Render : nullptr;
   if ((not render) or (not render->ActiveStep) or (render->ActiveStep->Effect != Effect) or
       (filter->GraphRevision != render->Revision) or filter->collecting()) return nullptr;
   return render->ActiveStep;
}

//********************************************************************************************************************
// The display blitter takes a mutable source object but only reads from it.

static ERR copy_filter_bitmap(const objBitmap *Source, objBitmap *Dest, BAF Flags,
   int X, int Y, int Width, int Height, int DestX, int DestY)
{
   return gfx::CopyArea(const_cast<objBitmap *>(Source), Dest, Flags, X, Y, Width, Height, DestX, DestY);
}

//********************************************************************************************************************
// Scales the alpha of straight-alpha colours by 8-bit coverage values.  The colour components are left unchanged, so
// partially covered pixels at the edge of an image keep the image's true colour.

static void apply_filter_coverage(agg::rgba8 *Colours, const agg::int8u *Covers, int Length)
{
   for (int i=0; i < Length; i++) {
      if (Covers[i] < 255) Colours[i].a = agg::int8u((int(Colours[i].a) * Covers[i] + 127) / 255);
   }
}

//********************************************************************************************************************
// A crop is a view into the original bitmap.  The sampler addresses it from (0,0) and clamps to its edges.

static void filter_source_pixels(agg::pixfmt_psl &Pixels, objBitmap &Bitmap, const TClipRectangle<int> *Crop)
{
   Pixels.setBitmap(Bitmap);
   if (Crop) {
      Pixels.mData = Pixels.row_ptr(Crop->top) + std::ptrdiff_t(Crop->left) * Pixels.mBytesPerPixel;
      Pixels.mWidth = Crop->width();
      Pixels.mHeight = Crop->height();
   }
}

//********************************************************************************************************************
// Resamples Bitmap into Target with a pure scale transform, splitting rows across worker threads when the region is
// large enough to benefit.  Dest is the image's rectangle in Target pixels.  The source is read with clamped edges and
// pixels that Dest covers in part receive that coverage in their alpha, as an anti-aliased rectangle would.

static void render_bilinear_upscale(objBitmap &Bitmap, objBitmap &Target, const agg::trans_affine &Transform,
   const agg::image_filter_lut &Filter, const TClipRectangle<double> &Dest, const TClipRectangle<int> *Crop)
{
   // The image can extend beyond the integer range even when its visible region is small.
   const auto &clip = Target.Clip;
   auto visible = Dest;
   visible.shrinking({ double(clip.Left), double(clip.Top), double(clip.Right), double(clip.Bottom) });
   if (not visible.valid()) return;

   const int left   = int(std::floor(visible.left));
   const int top    = int(std::floor(visible.top));
   const int right  = int(std::ceil(visible.right));
   const int bottom = int(std::ceil(visible.bottom));
   const int width = right - left, height = bottom - top;

   // Coverage of pixel Index by the span [Start, End), as an 8-bit value.
   auto coverage = [](int Index, double Start, double End) {
      const double cover = std::min(double(Index + 1), End) - std::max(double(Index), Start);
      return agg::int8u(std::lround(std::clamp(cover, 0.0, 1.0) * 255.0));
   };

   std::vector<agg::int8u> column_covers(width, 255);
   column_covers.front() = coverage(left, Dest.left, Dest.right);
   column_covers.back()  = std::min(column_covers.back(), coverage(right - 1, Dest.left, Dest.right));
   const bool edge_columns = (column_covers.front() < 255) or (column_covers.back() < 255);

   // The destination rectangle covers every pixel in the span.  Generate one full span per row, as the rasteriser
   // does, so the sampler keeps the same interpolation endpoints and edge behaviour.
   auto render_rows = [&](int StartY, int EndY) {
      agg::pixfmt_psl pix_source, pix_dest(Target);
      filter_source_pixels(pix_source, Bitmap, Crop);
      agg::span_interpolator_linear<> interpolator(Transform);
      agg::span_clamp<agg::pixfmt_psl> source(pix_source);
      agg::span_image_filter_rgba<decltype(source), decltype(interpolator)>
         sampler(source, interpolator, Filter, false);
      agg::span_allocator<agg::rgba8> span_alloc;
      std::vector<agg::int8u> row_covers;
      auto colours = span_alloc.allocate(width);

      for (int y=StartY; y < EndY; y++) {
         sampler.generate(colours, left, y, width);

         const auto row_cover = coverage(y, Dest.top, Dest.bottom);
         if (row_cover < 255) {
            row_covers.resize(width);
            for (int x=0; x < width; x++) row_covers[x] = agg::int8u((int(column_covers[x]) * row_cover + 127) / 255);
            apply_filter_coverage(colours, row_covers.data(), width);
         }
         else if (edge_columns) apply_filter_coverage(colours, column_covers.data(), width);

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
// Fits a bitmap to the primitive subregion and maps its pixel coordinates to the filter's local surface.

static ERR filter_image_transform(extFilterEffect *Effect, int Width, int Height, ARF AspectRatio,
   agg::trans_affine &Forward, agg::trans_affine &Inverse)
{
   const auto step = active_filter_step(Effect);
   if (not step) return ERR::InvalidState;
   const auto &subregion = step->Subregion;

   // The image is fitted to the primitive subregion, which render_filter() computes before drawing the effect.  In
   // objectBoundingBox mode the subregion is measured against the bounding box of the client vector, as per SVG.

   double p_x = std::round(subregion.left), p_y = std::round(subregion.top);
   double p_width = std::round(subregion.width()), p_height = std::round(subregion.height());

   if ((p_width <= 0) or (p_height <= 0)) return ERR::NothingDone;

   double x_scale = 1, y_scale = 1, align_x = 0, align_y = 0;
   calc_aspectratio("align_filter", AspectRatio, p_width, p_height, Width, Height, align_x, align_y,
      x_scale, y_scale);

   p_x += std::round(align_x);
   p_y += std::round(align_y);

   Forward = agg::trans_affine_scaling(x_scale, y_scale);
   Forward.translate(p_x, p_y);
   Forward *= Effect->Filter->Render->Transform;
   Inverse = Forward;
   return invert_filter_transform(Inverse) ? ERR::Okay : ERR::InvalidValue;
}

//********************************************************************************************************************
// Render a filter's bitmap without blending.  Sampling clamps to the source edges, and coverage of the
// transformed image rectangle is applied to alpha only.  Colour conversion waits until post-processing.

static ERR render_to_filter(extFilterEffect *Self, objBitmap *Bitmap, ARF AspectRatio, VSM SampleMethod,
   const TClipRectangle<int> *Crop = nullptr)
{
   const int width = Crop ? Crop->width() : Bitmap->Width;
   const int height = Crop ? Crop->height() : Bitmap->Height;
   agg::trans_affine forward, img_transform;
   if (auto error = filter_image_transform(Self, width, height, AspectRatio, forward, img_transform);
       error != ERR::Okay) {
      return (error IS ERR::NothingDone) ? ERR::Okay : error;
   }
   const auto &clip = Self->Target->Clip;
   TClipRectangle<double> samples { filter_pixel_centre(clip.Left), filter_pixel_centre(clip.Top),
      filter_pixel_centre(clip.Right), filter_pixel_centre(clip.Bottom) };
   if ((not transform_filter_region(samples, img_transform)) or (not filter_raster_bounds(samples))) {
      return ERR::OutOfRange;
   }

   if (Crop or img_transform.is_complex()) {
      // The transform maps destination pixels back to the source, so upscale factors are at most one.
      if (((SampleMethod IS VSM::BILINEAR) or (SampleMethod IS VSM::AUTO)) and
          (img_transform.shx IS 0.0) and (img_transform.shy IS 0.0) and
          (img_transform.sx > 0.0) and (img_transform.sx <= 1.0) and
          (img_transform.sy > 0.0) and (img_transform.sy <= 1.0)) {
         TClipRectangle<double> dest { 0, 0, double(width), double(height) };
         forward.transform(&dest.left, &dest.top);
         forward.transform(&dest.right, &dest.bottom);
         render_bilinear_upscale(*Bitmap, *Self->Target, img_transform, get_filter(SampleMethod, img_transform),
            dest, Crop);
         return ERR::Okay;
      }

      // Coordinates are clipped in double precision, so an image that extends far beyond the target is safe.

      agg::rasterizer_scanline_aa<> raster;
      raster.clip_box(clip.Left, clip.Top, clip.Right, clip.Bottom);

      const double corners[4][2] = {
         { 0, 0 }, { double(width), 0 },
         { double(width), double(height) }, { 0, double(height) }
      };
      for (int i=0; i < 4; i++) {
         double x = corners[i][0], y = corners[i][1];
         forward.transform(&x, &y);
         if (i IS 0) raster.move_to(x, y);
         else raster.line_to(x, y);
      }
      raster.close_polygon();

      agg::renderer_base<agg::pixfmt_psl> render_base;
      agg::pixfmt_psl pix_dest(*Self->Target);
      agg::pixfmt_psl pix_source;
      filter_source_pixels(pix_source, *Bitmap, Crop);
      render_base.attach(pix_dest);
      render_base.clip_box(clip.Left, clip.Top, clip.Right - 1, clip.Bottom - 1);

      agg::span_interpolator_linear<> interpolator(img_transform);
      agg::span_clamp<agg::pixfmt_psl> source(pix_source);

      auto render = [&](auto &SpanGen) {
         agg::scanline_u8 scanline;
         agg::span_allocator<agg::rgba8> span_alloc;
         if (not raster.rewind_scanlines()) return;
         scanline.reset(raster.min_x(), raster.max_x());
         SpanGen.prepare();
         while (raster.sweep_scanline(scanline)) {
            const int y = scanline.y();
            auto span = scanline.begin();
            for (auto n = scanline.num_spans(); n > 0; n--, ++span) {
               const int length = std::abs(span->len);
               auto colours = span_alloc.allocate(length);
               SpanGen.generate(colours, span->x, y, length);
               apply_filter_coverage(colours, span->covers, length);
               render_base.copy_color_hspan(span->x, y, length, colours);
            }
         }
      };

      if (SampleMethod IS VSM::NEIGHBOUR) {
         agg::span_image_filter_rgba_nn<decltype(source), agg::span_interpolator_linear<>>
            spangen(source, interpolator);
         render(spangen);
      }
      else {
         const agg::image_filter_lut &ifilter = get_filter(SampleMethod, img_transform);
         agg::span_image_filter_rgba<decltype(source), agg::span_interpolator_linear<>>
            spangen(source, interpolator, ifilter, false);
         render(spangen);
      }
   }
   else return gfx::CopyArea(Bitmap, Self->Target, BAF::NIL, 0, 0, width, height,
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

struct filter_xml_element {
   std::string_view Name;
   std::string Result;
   std::vector<std::pair<std::string, std::string>> Attributes;
   std::vector<filter_xml_element> Children;
   bool Complete = false; // XMLDef historically includes brackets for lighting and transfer functions.
   bool Valid = true;

   void attribute(std::string_view Attribute, std::string_view Value) {
      Attributes.emplace_back(Attribute, Value);
   }

   template<class T> requires std::is_arithmetic_v<T>
   void attribute(std::string_view Attribute, T Value) {
      attribute(Attribute, number(Value));
   }

   template<class T> std::string number(T Value) {
      if constexpr (std::is_floating_point_v<T>) {
         if (not std::isfinite(Value)) {
            Valid = false;
            return {};
         }
      }
      char buffer[64];
      auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), Value);
      if (error != std::errc()) Valid = false;
      return std::string(buffer, end);
   }

   template<class T> void list(std::string_view Attribute, std::span<const T> Values) {
      std::string value;
      for (auto item : Values) {
         if (not value.empty()) value += ' ';
         value += number(item);
      }
      attribute(Attribute, value);
   }

   filter_xml_element & child(std::string_view TagName) {
      auto &child = Children.emplace_back();
      child.Name = TagName;
      return child;
   }

   ERR write(std::string &Output, bool Fragment = false) const {
      if (not Valid) return ERR::InvalidValue;
      if (not Fragment) Output += '<';
      Output += Name;
      for (const auto &[name, value] : Attributes) {
         Output += ' ';
         Output += name;
         Output += "=\"";
         for (char c : value) {
            switch (c) {
               case '&': Output += "&amp;"; break;
               case '<': Output += "&lt;"; break;
               case '"': Output += "&quot;"; break;
               case '\t': Output += "&#9;"; break;
               case '\n': Output += "&#10;"; break;
               case '\r': Output += "&#13;"; break;
               default:
                  if (uint8_t(c) < 0x20) return ERR::InvalidValue;
                  Output += c;
            }
         }
         Output += '"';
      }
      if (Fragment) return ERR::Okay;
      if (Children.empty()) Output += "/>";
      else {
         Output += '>';
         for (const auto &child : Children) {
            if (auto error = child.write(Output); error != ERR::Okay) return error;
         }
         Output += "</";
         Output += Name;
         Output += '>';
      }
      return ERR::Okay;
   }
};

//********************************************************************************************************************

static void write_filter_xml_colour(filter_xml_element &Element, std::string_view Name, const FRGB &Colour)
{
   auto channel = [&](float Value) {
      const double level = Value * 255.0;
      const double integer = std::round(level);
      return Element.number((float(integer * (1.0 / 255.0)) IS Value) ? integer : level);
   };

   auto value = "rgb(" + channel(Colour.Red) + " " + channel(Colour.Green) + " " + channel(Colour.Blue);
   if (Colour.Alpha != 1.0f) value += " / " + Element.number(double(Colour.Alpha));
   value += ')';
   Element.attribute(Name, value);
}

//********************************************************************************************************************

// View boxes for ImageFX and SourceFX are stored as { x, y, width, height }.  A view box is active when it
// has an area; a zero width and height selects the entire source.

using view_box = std::array<double, 4>;
enum { VB_X, VB_Y, VB_WIDTH, VB_HEIGHT };

static bool has_view_box(const view_box &Rect)
{
   return (Rect[VB_WIDTH] > 0) and (Rect[VB_HEIGHT] > 0);
}

// The ViewBox field reads as an empty array when no rectangle is set.

static std::span<double> view_box_field(view_box &Rect)
{
   if (has_view_box(Rect)) return std::span<double>(Rect);
   return std::span<double>{};
}

// Normalises a client-supplied ViewBox array.  An empty array or a zero width and height clears the rectangle;
// any other value must have four finite values with a positive width and height.

static ERR read_view_box(std::span<const double> Array, view_box &Result)
{
   if (Array.empty()) {
      Result = view_box { 0, 0, 0, 0 };
      return ERR::Okay;
   }

   if (Array.size() != 4) return ERR::InvalidValue;

   if ((Array[VB_WIDTH] IS 0) and (Array[VB_HEIGHT] IS 0)) {
      Result = view_box { 0, 0, 0, 0 };
      return ERR::Okay;
   }

   for (double value : Array) {
      if (not std::isfinite(value)) return ERR::OutOfRange;
   }
   if ((Array[VB_WIDTH] <= 0) or (Array[VB_HEIGHT] <= 0)) return ERR::OutOfRange;

   std::copy(Array.begin(), Array.end(), Result.begin());
   return ERR::Okay;
}

static void write_filter_xml_view_box(filter_xml_element &Element, const view_box &Rect)
{
   if (has_view_box(Rect)) {
      Element.attribute("xvg:viewBox", Element.number(Rect[VB_X]) + ' ' + Element.number(Rect[VB_Y]) + ' ' +
         Element.number(Rect[VB_WIDTH]) + ' ' + Element.number(Rect[VB_HEIGHT]));
   }
}

//********************************************************************************************************************

static void write_filter_xml_subregion(filter_xml_element &Element, const objFilterEffect &Effect)
{
   auto write_unit = [&](std::string_view Name, const Unit &Length) {
      if (not Length.defined()) return;
      // Invert the percentage conversion in Unit::read() to preserve pixel boundaries on reload.
      if (Length.scaled()) Element.attribute(Name, Element.number(Length.Value / 0.01) + '%');
      else Element.attribute(Name, Length.Value);
   };
   write_unit("x", Effect.X);
   write_unit("y", Effect.Y);
   write_unit("width", Effect.Width);
   write_unit("height", Effect.Height);
}

//********************************************************************************************************************

static void write_filter_xml_aspect(filter_xml_element &Element, ARF Ratio)
{
   if (Ratio IS (ARF::X_MID|ARF::Y_MID|ARF::MEET)) return;
   if (((Ratio & ARF::NONE) != ARF::NIL) or ((Ratio & (ARF::MEET|ARF::SLICE)) IS ARF::NIL)) {
      Element.attribute("preserveAspectRatio", "none");
      return;
   }

   std::string value((Ratio & ARF::X_MIN) != ARF::NIL ? "xMin" :
      (Ratio & ARF::X_MAX) != ARF::NIL ? "xMax" : "xMid");
   value += (Ratio & ARF::Y_MIN) != ARF::NIL ? "YMin" : (Ratio & ARF::Y_MAX) != ARF::NIL ? "YMax" : "YMid";
   value += (Ratio & ARF::SLICE) != ARF::NIL ? " slice" : " meet";
   Element.attribute("preserveAspectRatio", value);
}

//********************************************************************************************************************

static void write_filter_xml_channel(filter_xml_element &Element, std::string_view Name, MCH Channel)
{
   static constexpr CSTRING channels[] = { "R", "G", "B", "A", "luminance" };
   if ((Channel > MCH::RED) and (Channel <= MCH::LUMINANCE)) Element.attribute(Name, channels[int(Channel)]);
}

//********************************************************************************************************************

static void write_filter_xml_resampling(filter_xml_element &Element, VSM Method)
{
   // Keep the selected API method, including methods which share one implementation in DisplacementFX.
   if (Method != VSM::BILINEAR) Element.attribute("xvg:resampleMethod", int(Method));
}

//********************************************************************************************************************
// The colour space in which an effect processes its inputs: its own ColourSpace, or the filter's if it inherits.
// Results are stored in sRGB either way, so effects with different colour spaces can be chained freely.

static VCS effect_colour_space(const extFilterEffect *Effect)
{
   return (Effect->ColourSpace != VCS::INHERIT) ? Effect->ColourSpace : Effect->Filter->ColourSpace;
}

// Resolve the addressing domain of a referenced image from its primitive bounds and the available pixels.

static TClipRectangle<int> filter_input_region(const extFilterEffect *Effect, const filter_surface_view &Source,
   const filter_step *Producer, FEM Edge)
{
   const auto &clip = Source.Valid;
   if ((Edge IS FEM::TRANSPARENT) or (not Producer)) {
      return clip;
   }

   const auto &sub = Producer->Subregion;
   const TClipRectangle<int> empty = { clip.left, clip.top, clip.left, clip.top };
   if (not sub.valid()) return empty;

   const auto &transform = Effect->Filter->Render->Transform;
   auto path = sub.as_path(transform);
   const auto bounds = get_bounds(path);
   if (not bounds.valid()) return empty;

   // Primitive clipping includes the left and top edges in user space.  Reflection can put those edges on the
   // far side of the bitmap, so account for which bounds include a pixel centre that falls exactly on the edge.
   auto limit = [](double Coordinate, bool RoundPastCentre, int Low, int High) {
      const auto centre = Coordinate - 0.5;
      const auto index = RoundPastCentre ? std::floor(centre) + 1 : std::ceil(centre);
      return int(std::clamp(index, double(Low), double(High)));
   };
   const TClipRectangle<int> region = {
      limit(bounds.left, (transform.sx < 0) or (transform.shx < 0), clip.left, clip.right),
      limit(bounds.top, (transform.shy < 0) or (transform.sy < 0), clip.top, clip.bottom),
      limit(bounds.right, (transform.sx <= 0) and (transform.shx <= 0), clip.left, clip.right),
      limit(bounds.bottom, (transform.shy <= 0) and (transform.sy <= 0), clip.top, clip.bottom)
   };
   return region.valid() ? region : empty;
}

//********************************************************************************************************************
// Fit a scalar grid like a bitmap, retaining float precision and leaving uncovered pixels invalid.

template<class Sample>
static ERR render_scalar_to_filter(extFilterEffect *Effect, int Width, int Height, ARF AspectRatio,
   VSM Method, double Maximum, Sample Tap)
{
   agg::trans_affine forward, inverse;
   if (auto error = filter_image_transform(Effect, Width, Height, AspectRatio, forward, inverse);
       error != ERR::Okay) {
      return (error IS ERR::NothingDone) ? ERR::Okay : error;
   }

   filter_surface_view layout;
   float *data;
   if (auto error = get_numeric_target(Effect, layout, data); error != ERR::Okay) return error;

   TClipRectangle<double> bounds { 0, 0, double(Width), double(Height) };
   if (not transform_filter_region(bounds, forward)) return ERR::OutOfRange;
   const auto &clip = layout.Valid;
   const int left = int(std::clamp(std::floor(bounds.left), double(clip.left), double(clip.right)));
   const int right = int(std::clamp(std::ceil(bounds.right), double(clip.left), double(clip.right)));
   const int top = int(std::clamp(std::floor(bounds.top), double(clip.top), double(clip.bottom)));
   const int bottom = int(std::clamp(std::ceil(bounds.bottom), double(clip.top), double(clip.bottom)));

   const bool nearest = Method IS VSM::NEIGHBOUR;
   const bool bilinear = (Method IS VSM::BILINEAR) or (Method IS VSM::AUTO);
   const auto kernel = (nearest or bilinear) ? nullptr : &get_filter(Method, inverse);
   auto tap = [&](int X, int Y) {
      return Tap(std::clamp(X, 0, Width - 1), std::clamp(Y, 0, Height - 1));
   };

   for (int y = top; y < bottom; y++) {
      auto row = data + std::ptrdiff_t(y) * (layout.Stride / sizeof(float));
      for (int x = left; x < right; x++) {
         double sx, sy;
         map_filter_pixel_centre(inverse, x, y, sx, sy);
         // Pixels outside the image (including letterboxing) retain the surface's invalid sample.
         if (not ((sx >= 0) and (sx < Width) and (sy >= 0) and (sy < Height))) continue;
         if (nearest) {
            row[x] = float(tap(int(sx), int(sy)));
            continue;
         }

         sx -= 0.5;
         sy -= 0.5;
         const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
         const double fx = sx - ix, fy = sy - iy;
         if (bilinear) {
            if ((fx IS 0.0) and (fy IS 0.0)) {
               row[x] = float(tap(ix, iy));
               continue;
            }
            const double a = std::lerp(tap(ix, iy), tap(ix + 1, iy), fx);
            const double b = std::lerp(tap(ix, iy + 1), tap(ix + 1, iy + 1), fx);
            row[x] = float(std::lerp(a, b, fy));
         }
         else {
            // Use the image kernel weights, accumulating the height itself without integer rounding.
            const auto weights = kernel->weight_array();
            const int start = kernel->start(), diameter = kernel->diameter();
            constexpr int subpixel_scale = agg::image_subpixel_scale;
            const int phase_x = agg::image_subpixel_mask - int(fx * subpixel_scale);
            const int phase_y = agg::image_subpixel_mask - int(fy * subpixel_scale);
            double height = 0;
            for (int ky = 0; ky < diameter; ky++) {
               const double wy = weights[phase_y + ky * subpixel_scale];
               for (int kx = 0; kx < diameter; kx++) {
                  const double wx = weights[phase_x + kx * subpixel_scale];
                  height += wx * wy * tap(ix + start + kx, iy + start + ky);
               }
            }
            constexpr double scale = agg::image_filter_scale;
            row[x] = float(std::clamp(height / (scale * scale), 0.0, Maximum));
         }
      }
   }
   return ERR::Okay;
}

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
#include "filter_tile.cpp"
#include "filter_turbulence.cpp"
#include "filter_wavefunction.cpp"

//********************************************************************************************************************
// Logs a problem with one of an effect's inputs, identifying the field or named slot, and returns Error.

static ERR filter_input_error(const extFilterEffect *Effect, filter_input_role Role, size_t Slot, ERR Error,
   CSTRING Reason, OBJECTID Producer = 0)
{
   kt::Log log("FilterPlan");
   const auto class_name = Effect->Class->ClassName.c_str();
   if (Role IS filter_input_role::NAMED) {
      log.warning("%s #%d %s: %s (producer #%d).", class_name, Effect->UID,
         Effect->FilterExtension->Inputs[Slot].Name, Reason, Producer);
      return Error;
   }
   const auto field = (Role IS filter_input_role::MIX) ? "Mix" :
      ((Role IS filter_input_role::MERGE) ? "SourceList" : "Input");
   if (Role IS filter_input_role::MERGE) {
      log.warning("%s #%d %s[%zu]: %s (producer #%d).", class_name, Effect->UID, field, Slot, Reason,
         Producer);
   }
   else log.warning("%s #%d %s: %s (producer #%d).", class_name, Effect->UID, field, Reason, Producer);
   return Error;
}

//********************************************************************************************************************
// Confirms that Producer is a live sibling of Effect within the same filter.  If Positions is provided it is used in
// place of a linear search of the filter's effect list.

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

//********************************************************************************************************************
// Assigns the source of a named input declared by the effect's extension.  Map inputs and optional images
// may be cleared with VSF::NONE.

static ERR set_named_filter_input(extFilterEffect *Effect, std::string_view Name, VSF Source,
   objFilterEffect *Reference)
{
   if (not Effect->FilterExtension) return ERR::NoSupport;
   const auto inputs = Effect->FilterExtension->Inputs;
   const auto slot = std::ranges::find_if(inputs, [&](const auto &Input) { return Name IS Input.Name; });
   if (slot IS inputs.end()) return ERR::Search;
   if (Source IS VSF::REFERENCE) {
      if (auto error = check_filter_reference(Effect, Reference, filter_input_role::NAMED, slot - inputs.begin());
          error != ERR::Okay) return error;
      if (slot->Contract.NativeOnly) {
         if (auto error = validate_native_filter_input(Effect, *slot, Reference); error != ERR::Okay) return error;
      }
   }
   else if ((Source < VSF::GRAPHIC) or (Source > VSF::BKGD_ALPHA)) {
      if (Source != VSF::NONE) return ERR::NoSupport;
      if ((slot->Contract.Kind IS filter_input_kind::IMAGE) and (not slot->Contract.Optional)) return ERR::FieldNotSet;
   }

   slot->Source = Source;
   slot->Reference = (Source IS VSF::REFERENCE) ? Reference : nullptr;
   if (Effect->Filter) Effect->Filter->GraphRevision++;
   effect_modified(Effect);
   return ERR::Okay;
}

//********************************************************************************************************************
// Clears every reference from Effect to Producer, or all of its references if Producer is null.  Dormant references
// are also detached, because a later SourceType or MixType write can reactivate them.

static void clear_filter_references(extFilterEffect *Effect, objFilterEffect *Producer)
{
   if ((not Producer) or (Effect->Input IS Producer)) Effect->Input = nullptr;
   if ((not Producer) or (Effect->Mix IS Producer)) Effect->Mix = nullptr;
   if (Effect->classID() IS CLASSID::MERGEFX) {
      auto merge = (extMergeFX *)Effect;
      for (size_t i = 0; i < merge->Layers.size(); i++) {
         if ((not Producer) or (merge->SourceList[i].Effect IS Producer)) {
            merge->Layers[i].EffectID = 0;
            merge->SourceList[i].Effect = nullptr;
         }
      }
   }
   if (auto extension = Effect->FilterExtension; extension) {
      for (auto &input : extension->Inputs) {
         if ((not Producer) or (input.Reference IS Producer)) {
            input.Reference = nullptr;
            if (input.Contract.Optional and (input.Source IS VSF::REFERENCE)) input.Source = VSF::NONE;
         }
      }
   }
}

//********************************************************************************************************************
// Rebuilds the Prev and Next links of the filter's effects to match their order, and invalidates any active plan.

static void update_filter_links(extVectorFilter *Filter)
{
   auto &effects = Filter->Effects;
   for (size_t i = 0; i < effects.size(); i++) {
      effects[i]->Prev = i ? effects[i - 1] : nullptr;
      effects[i]->Next = (i + 1 < effects.size()) ? effects[i + 1] : nullptr;
   }
   Filter->GraphRevision++;
   filter_modified(Filter);
}

//********************************************************************************************************************
// Removes an effect from its filter and clears all references to it held by the remaining effects.

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

//********************************************************************************************************************

extFilterEffect::~extFilterEffect()
{
   unlink_filter_effect(this);
}

//********************************************************************************************************************
// Adapts an effect class's sampling() method to the filter_operation::Sampling function signature.

template<class Effect>
static filter_sampling effect_sampling(const filter_step &Step)
{
   return ((const Effect *)Step.Effect)->sampling();
}

#ifdef UNIT_TESTS
static constexpr CSTRING glFilterMaskTestClass = "FilterMaskTestFX";
static constexpr CSTRING glFilterSurfaceTestClass = "FilterSurfaceTestFX";
#endif

//********************************************************************************************************************
// Returns the input arrangement and sampling reach for an effect.  Unrecognised classes are treated as single-input
// effects that may read the whole of their input.  LightingFX reads a second input only for its combined type, and
// only if a mix input has been set, as the albedo is optional.

static filter_operation describe_filter_operation(const extFilterEffect *Effect)
{
   using enum filter_inputs;
   const auto class_id = ((extFilterEffect *)Effect)->classID();

#ifdef UNIT_TESTS
   if (class_id IS CLASSID(kt::strihash(glFilterMaskTestClass))) return { DUAL };
   if (class_id IS CLASSID(kt::strihash(glFilterSurfaceTestClass))) return { GENERATOR };
#endif

   switch (class_id) {
      case CLASSID::BLURFX:
         if (((const extBlurFX *)Effect)->OutputMode IS FOM::HEIGHT) {
            return { SINGLE, effect_sampling<extBlurFX>, filter_input_kind::SCALAR_MAP,
               filter_surface_format::SCALAR_F32 };
         }

         return {
            .Inputs = SINGLE,
            .Sampling = effect_sampling<extBlurFX>,
            .InputFormat = ((const extBlurFX *)Effect)->OutputMode IS FOM::NATIVE
         };

      case CLASSID::COLOURFX:
         return { SINGLE };

      case CLASSID::COMPOSITEFX:
         if (((const extCompositeFX *)Effect)->OutputMode IS FOM::HEIGHT) {
            return {
               .Inputs = DUAL,
               .Primary = filter_input_kind::SCALAR_MAP,
               .Format = filter_surface_format::SCALAR_F32,
               .Secondary = filter_input_kind::SCALAR_MAP
            };
         }
         return { DUAL };

      case CLASSID::CONVOLVEFX:
         return {
            .Inputs = SINGLE,
            .Sampling = effect_sampling<extConvolveFX>,
            .InputFormat = true
         };

      case CLASSID::DISPLACEMENTFX:
         return {
            ((const extDisplacementFX *)Effect)->VectorMap.Reference ? SINGLE : DUAL,
            effect_sampling<extDisplacementFX>
         };

      case CLASSID::FLOODFX:
         return { GENERATOR };

      case CLASSID::IMAGEFX:
         return {
            .Inputs = GENERATOR,
            .Format = ((const extImageFX *)Effect)->OutputMode IS FOM::HEIGHT ?
                      filter_surface_format::SCALAR_F32 : filter_surface_format::RGBA8
         };

      case CLASSID::LIGHTINGFX: {
         const auto lighting = (const extLightingFX *)Effect;
         const auto primary = lighting->enhanced(filter_surface_format::SCALAR_F32) ?
            filter_input_kind::SCALAR_MAP : filter_input_kind::IMAGE;
         return {
            lighting->reads_albedo() ? DUAL : SINGLE,
            [](const filter_step &Step) {
               const auto producer = Step.Inputs.front().Producer;
               const auto format = producer ? producer->Format : filter_surface_format::RGBA8;
               return ((const extLightingFX *)Step.Effect)->sampling(format);
            },
            primary
         };
      }

      case CLASSID::MERGEFX:
         return { MERGE };

      case CLASSID::MORPHOLOGYFX:
         return {
            .Inputs = SINGLE,
            .Sampling = effect_sampling<extMorphologyFX>,
            .InputFormat = true
         };

      case CLASSID::OFFSETFX:
         return { SINGLE, effect_sampling<extOffsetFX> };

      case CLASSID::REMAPFX:
         return { SINGLE };

      case CLASSID::SOURCEFX:
         return { GENERATOR };

      case CLASSID::TILEFX:
         return {
            .Inputs = SINGLE,
            .InputFormat = true,
            .InputRegion = false
         };

      case CLASSID::TURBULENCEFX:
         return {
            .Inputs = GENERATOR,
            .Format = ((const extTurbulenceFX *)Effect)->OutputMode IS FOM::HEIGHT ?
                      filter_surface_format::SCALAR_F32 : filter_surface_format::RGBA8
         };

      case CLASSID::WAVEFUNCTIONFX:
         return {
            .Inputs = GENERATOR,
            .Format = ((const extWaveFunctionFX *)Effect)->OutputMode IS FOM::HEIGHT ?
                      filter_surface_format::SCALAR_F32 : filter_surface_format::RGBA8
         };

      default:
         return {
            SINGLE,
            [](const filter_step &) -> filter_sampling {
               return {
                  filter_access::FULL_INPUT,
                  filter_sampling::UNBOUNDED,
                  filter_sampling::UNBOUNDED
               };
            }
         };
   }
}

//********************************************************************************************************************
// Returns the input contract kind that accepts a producer of the given Format unchanged.

static filter_input_kind filter_input_kind_of(filter_surface_format Format)
{
   switch (Format) {
      case filter_surface_format::SCALAR_F32: return filter_input_kind::SCALAR_MAP;
      case filter_surface_format::VECTOR_F32: return filter_input_kind::VECTOR_MAP;
      default: return filter_input_kind::IMAGE;
   }
}

//********************************************************************************************************************
// Returns true if a producer of the given Format can satisfy the input Contract.

static bool supports_filter_input(filter_surface_format Format, const filter_input_contract &Contract)
{
   using enum filter_input_kind;
   using enum filter_surface_format;
   switch (Contract.Kind) {
      case IMAGE: return (Format IS RGBA8) or (Contract.BitmapConversion IS filter_bitmap_conversion::UNIT_CHANNELS);
      case SCALAR_MAP: return (Format IS SCALAR_F32) or ((not Contract.NativeOnly) and (Format IS RGBA8));
      case VECTOR_MAP: return (Format IS VECTOR_F32) or ((not Contract.NativeOnly) and (Format IS RGBA8));
   }
   return false;
}

//********************************************************************************************************************
// Validate native-only assignments before replacing the existing binding.  Format-preserving effects take their
// format from an earlier producer, so a single backwards scan resolves their input chain.

static ERR validate_native_filter_input(const extFilterEffect *Effect, const filter_named_input &Slot,
   objFilterEffect *Reference)
{
   const auto &effects = Effect->Filter->Effects;
   for (auto pos = std::ranges::find(effects, Effect); pos != effects.begin();) {
      const auto producer = *--pos;
      if (producer != Reference) continue;
      const auto operation = describe_filter_operation(producer);
      if (not operation.InputFormat) {
         const auto format = producer->FilterExtension ? producer->FilterExtension->Format : operation.Format;
         return supports_filter_input(format, Slot.Contract) ? ERR::Okay : ERR::NoSupport;
      }
      if (producer->SourceType != VSF::REFERENCE) return ERR::NoSupport;
      Reference = producer->Input;
   }
   return ERR::InvalidValue;
}

//********************************************************************************************************************
// Reports why the authored effect order is invalid, distinguishing a dependency cycle from a producer that merely
// follows its consumer.  Any cycle includes a forward edge, so this walk is only needed when a forward reference has
// been found, and successful draws do not need a second graph pass.

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

//********************************************************************************************************************
// Validates the filter's effects and builds the draw plan.  The first pass checks each effect, sizes the input
// array and enforces the resource limits.  The second pass resolves every input binding, counts consumers for
// producers and standard sources, and detects references that do not precede their consumer.

static ERR prepare_filter_plan(extVectorFilter *Filter, filter_plan &Plan)
{
   kt::Log log("FilterPlan");
   const auto &effects = Filter->Effects;
   if (effects.size() > MAX_FILTER_EFFECTS) return log.warning(ERR::ArrayFull);
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
      auto operation = describe_filter_operation(effect);
      const auto extension = effect->FilterExtension;
      auto format = extension ? extension->Format : operation.Format;

      if (operation.InputFormat) {
         // A valid producer precedes its consumer, so its format is already known.  An invalid reference keeps the
         // RGBA8 default and is reported when the inputs are resolved.
         format = filter_surface_format::RGBA8;
         if (effect->SourceType IS VSF::REFERENCE) {
            if (auto producer = positions.find(effect->Input); (producer != positions.end()) and
                (producer->second < i)) {
               format = Plan.Steps[producer->second].Format;
            }
         }
         operation.Primary = filter_input_kind_of(format);
      }

      Plan.Steps.push_back({ .Effect = effect, .Operation = operation, .Inputs = {}, .Format = format });
      if ((format != filter_surface_format::RGBA8) and
          (CheckAction(effect, AC::Draw) != ERR::True)) {
         log.warning("Numerical filter effect #%d requires a Draw action.", effect->UID);
         return ERR::NoSupport;
      }

      size_t count = 0;
      switch (operation.Inputs) {
         case filter_inputs::GENERATOR: break;
         case filter_inputs::SINGLE: count = 1; break;
         case filter_inputs::DUAL: count = 2; break;
         case filter_inputs::MERGE: count = ((extMergeFX *)effect)->Layers.size(); break;
      }

      if (count > MAX_FILTER_INPUTS - input_count) return log.warning(ERR::ArrayFull);

      input_count += count;

      if (extension) {
         if (extension->Inputs.size() > MAX_FILTER_INPUTS - input_count) return log.warning(ERR::ArrayFull);
         input_count += extension->Inputs.size();
      }
   }

   Plan.Inputs.reserve(input_count);
   Plan.Final = &Plan.Steps.back();
   if (Plan.Final->Format != filter_surface_format::RGBA8) {
      log.warning("Final filter effect #%d requires an RGBA8 result for scene composition.", Plan.Final->Effect->UID);
      return ERR::NoSupport;
   }

   const extFilterEffect *forward_consumer = nullptr;
   const filter_input *forward_input = nullptr;

   for (size_t i = 0; i < Plan.Steps.size(); i++) {
      auto &step = Plan.Steps[i];
      auto effect = step.Effect;
      const auto &operation = step.Operation;
      const size_t first = Plan.Inputs.size();

      auto add_input = [&](VSF Type, objFilterEffect *Reference, filter_input_role Role, size_t Slot = 0,
                           const filter_input_contract &Contract = {}) -> ERR {
         if ((Contract.Kind != filter_input_kind::IMAGE) and
             ((not std::isfinite(Contract.Fallback[0])) or (not std::isfinite(Contract.Fallback[1])))) {
            return filter_input_error(effect, Role, Slot, ERR::InvalidValue, "non-finite fallback");
         }

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
            case VSF::ALPHA: {
               // SourceAlpha derives from the shared capture once, regardless of how many effects use its alpha.
               auto &alpha = Plan.standard_source(Type);
               if (not alpha.Consumers) Plan.standard_source(VSF::GRAPHIC).Consumers++;
               alpha.Consumers++;
               break;
            }
            case VSF::GRAPHIC:
            case VSF::BKGD:
            case VSF::BKGD_ALPHA:
               Plan.standard_source(Type).Consumers++;
               break;
            case VSF::NONE:
               if ((Role IS filter_input_role::MERGE) or
                   ((Role IS filter_input_role::NAMED) and
                    (Contract.Optional or (Contract.Kind != filter_input_kind::IMAGE)))) break;
               return filter_input_error(effect, Role, Slot, ERR::FieldNotSet, "input is required");
            default: {
               char reason[64];
               std::snprintf(reason, sizeof(reason), "unsupported source kind %d", int(Type));
               return filter_input_error(effect, Role, Slot, ERR::NoSupport, reason);
            }
         }

         if ((Type != VSF::NONE) and
             (not supports_filter_input(producer ? producer->Format : filter_surface_format::RGBA8, Contract))) {
            return filter_input_error(effect, Role, Slot, ERR::NoSupport, "incompatible producer format",
               Reference ? Reference->UID : 0);
         }

         const auto &input = Plan.Inputs.emplace_back(Type, Role, Slot, producer, Contract);
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
            if (auto error = add_input(effect->SourceType, effect->Input, filter_input_role::INPUT, 0,
                { .Kind = operation.Primary, .ContributesRegion = operation.InputRegion }); error != ERR::Okay) {
               return error;
            }
            if (operation.Inputs IS filter_inputs::DUAL) {
               if (auto error = add_input(effect->MixType, effect->Mix, filter_input_role::MIX, 0,
                   { .Kind = operation.Secondary }); error != ERR::Okay) {
                  return error;
               }
            }
            break;
         case filter_inputs::MERGE: {
            const auto merge = (extMergeFX *)effect;
            for (size_t j = 0; j < merge->Layers.size(); j++) {
               const auto &layer = merge->Layers[j];
               const bool active = layer.Enabled and (layer.Opacity > 0);
               const auto source = active ? layer.SourceType : VSF::NONE;
               const auto producer = active ? merge->SourceList[j].Effect : nullptr;
               if (auto error = add_input(source, producer, filter_input_role::MERGE, j); error != ERR::Okay) {
                  return error;
               }
            }
            break;
         }
      }

      if (auto extension = effect->FilterExtension; extension) {
         for (size_t slot = 0; slot < extension->Inputs.size(); slot++) {
            const auto &input = extension->Inputs[slot];
            const bool active = (not extension->Active) or extension->Active(effect, slot);
            if (auto error = add_input(active ? input.Source : VSF::NONE, active ? input.Reference : nullptr,
                filter_input_role::NAMED, slot, input.Contract); error != ERR::Okay) return error;
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

//********************************************************************************************************************
// Resolves a filter length.  Scaled values are relative to Reference, and undefined values return Default.

static double filter_length(const Unit &Length, double Reference, double Default)
{
   if (Length.scaled()) return Length * Reference;
   return Length.defined() ? double(Length) : Default;
}

//********************************************************************************************************************
// Computes the client transforms, the bounds of the client subtree and the filter region for the current draw.
// Returns ERR::NothingDone if the client or its viewport is degenerate and nothing needs to be drawn.

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
   if (not invert_filter_transform(inverse)) {
      // A finite transform can collapse a client to zero area during a zoom animation.

      if (finite_transform(render.ToScene) and (render.ToScene.determinant() IS 0.0)) return ERR::NothingDone;
      return ERR::InvalidValue;
   }

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
// Trim idle storage only.  The draw owns every checked-out slot, and the renderer owns the final slot until
// composition returns.  Keep the most recently released slots first.

static void trim_filter_surfaces(extVectorFilter *Self, size_t Limit)
{
   const auto excess = Self->SurfaceBytes > Limit ? Self->SurfaceBytes - Limit : 0;
   if (not excess) return;

   size_t idle_bytes = 0;
   for (auto slot = Self->FreeSurfaces; slot; slot = slot->Next) idle_bytes += slot->Bytes;
   size_t keep = idle_bytes > excess ? idle_bytes - excess : 0;

   for (auto link = &Self->FreeSurfaces; *link;) {
      auto slot = *link;
      if (slot->Bytes <= keep) {
         keep -= slot->Bytes;
         link = &slot->Next;
      }
      else {
         *link = slot->Next;
         Self->SurfaceBytes -= slot->Bytes;
         delete slot;
      }
   }
}

//********************************************************************************************************************
// Checks out a surface of exactly Bytes from the free list, or allocates a new one.  The content of the surface is
// undefined.  Allocation is refused if the live storage of the draw would exceed MAX_FILTER_SURFACE_BYTES.

static ERR acquire_filter_surface(extVectorFilter *Self, size_t Bytes, filter_surface *&Surface,
   bool Numerical = false)
{
   auto &render = *Self->Render;
   if ((not Bytes) or (Bytes > MAX_FILTER_SURFACE_BYTES - render.LiveBytes)) {
      kt::Log("FilterStorage").warning("Filter #%d needs %zu additional bytes with %zu live (limit %zu).",
         Self->UID, Bytes, render.LiveBytes, MAX_FILTER_SURFACE_BYTES);
      return ERR::AllocMemory;
   }

   auto link = &Self->FreeSurfaces;
   while (*link and (((*link)->Bytes != Bytes) or ((*link)->Numerical != Numerical))) link = &(*link)->Next;
   if (*link) {
      Surface = *link;
      *link = Surface->Next;
      render.Reuses++;
   }
   else {
      trim_filter_surfaces(Self, MAX_FILTER_SURFACE_BYTES - Bytes);
      auto surface = std::unique_ptr<filter_surface>(new (std::nothrow) filter_surface);
      if (not surface) return ERR::AllocMemory;
      surface->Data.reset(new (std::nothrow) uint8_t[Bytes]);
      if (not surface->Data) return ERR::AllocMemory;
      surface->Bytes = Bytes;
      surface->Numerical = Numerical;
      Self->SurfaceBytes += Bytes;
      render.Allocations++;
      render.AllocatedBytes += Bytes;
      Surface = surface.release();
   }

   Surface->Next = nullptr;
   render.LiveBytes += Bytes;
   render.PeakLiveBytes = std::max(render.PeakLiveBytes, render.LiveBytes);
   render.LiveSurfaces++;
   render.PeakLiveSurfaces = std::max(render.PeakLiveSurfaces, render.LiveSurfaces);
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns a surface to the front of the free list and clears the caller's pointer.

static void release_filter_surface(extVectorFilter *Self, filter_surface *&Surface)
{
   if (not Surface) return;
   auto slot = std::exchange(Surface, nullptr);
   if (auto render = Self->Render; render) {
      render->LiveBytes -= slot->Bytes;
      render->LiveSurfaces--;
   }
   slot->Next = Self->FreeSurfaces;
   Self->FreeSurfaces = slot;
}

//********************************************************************************************************************
// Releases each surface in a linked List, up to but not including Stop.

static void release_filter_surfaces(extVectorFilter *Self, filter_surface *&List, const filter_surface *Stop)
{
   while (List != Stop) {
      auto slot = List;
      List = slot->Next;
      release_filter_surface(Self, slot);
   }
}

//********************************************************************************************************************
// Every bitmap exposes a fully initialised, tightly packed RGBA8 rectangle.  Targets in particular may blend or
// write only a primitive subregion, so initialise the entire processing region before making it addressable.

static ERR bind_filter_bitmap(extVectorFilter *Self, filter_surface &Surface, objBitmap *&Bitmap,
   CSTRING Name = "dummy_fx_bitmap")
{
   const auto clip = Self->Render->scratch_clip();
   Bitmap = Surface.BitmapID ? (objBitmap *)GetObjectPtr(Surface.BitmapID) : nullptr;
   if (not Bitmap) {
      Bitmap = objBitmap::create::local(fl::Name(Name), fl::Owner(Self->UID), fl::Width(clip.right),
         fl::Height(clip.bottom), fl::BitsPerPixel(32), fl::Flags(BMF::ALPHA_CHANNEL|BMF::NO_DATA));
      if (not Bitmap) return ERR::CreateObject;
      Surface.BitmapID = Bitmap->UID;
   }

   Bitmap->Width = clip.right;
   Bitmap->Height = clip.bottom;
   Bitmap->Clip = { 0, 0, clip.right, clip.bottom };
   Bitmap->ByteWidth = Bitmap->LineWidth = Self->Render->BitmapStride;
   Bitmap->Size = Bitmap->PlaneMod = int(Surface.Bytes);
   Bitmap->ColourSpace = CS::SRGB;
   Bitmap->BlendMode = BLM::AUTO;
   Bitmap->Opacity = 255;
   Bitmap->Flags &= ~BMF::PREMUL;
   Bitmap->Data = Surface.Data.get();
   Bitmap->CreatorMeta = Bitmap->Data;
   std::memset(Bitmap->Data, 0, Surface.Bytes);
   return ERR::Okay;
}

//********************************************************************************************************************
// Acquires a surface sized for the processing region and binds a cleared RGBA8 bitmap to it.

static ERR acquire_filter_bitmap(extVectorFilter *Self, filter_surface *&Surface, objBitmap *&Bitmap,
   CSTRING Name = "dummy_fx_bitmap")
{
   if (auto error = acquire_filter_surface(Self, Self->Render->BitmapBytes, Surface); error != ERR::Okay) return error;
   return bind_filter_bitmap(Self, *Surface, Bitmap, Name);
}

//********************************************************************************************************************
// As for acquire_filter_bitmap(), but the surface is pushed onto List so that the caller can release it in bulk.

static ERR get_filter_bitmap(extVectorFilter *Self, filter_surface *&List, objBitmap *&Bitmap)
{
   filter_surface *surface = nullptr;
   auto error = acquire_filter_bitmap(Self, surface, Bitmap);
   if (surface) {
      surface->Next = List;
      List = surface;
   }
   return error;
}

//********************************************************************************************************************
// Computes the stride and total size of a Width x Height surface, rejecting sizes that exceed the surface limit.

static ERR filter_surface_size(int Width, int Height, size_t PixelBytes, size_t &Bytes, int &Stride)
{
   if ((Width <= 0) or (Height <= 0)) return ERR::NothingDone;

   if (size_t(Width) > size_t(std::numeric_limits<int>::max()) / PixelBytes) return ERR::OutOfRange;

   const size_t stride = size_t(Width) * PixelBytes;
   if (size_t(Height) > MAX_FILTER_SURFACE_BYTES / stride) return ERR::AllocMemory;

   Stride = int(stride);
   Bytes = stride * size_t(Height);
   return ERR::Okay;
}

//********************************************************************************************************************
// Acquires the output surface for a numerical effect.  Every sample is initialised to INVALID_FILTER_SAMPLE so that
// pixels the effect does not write are treated as invalid by readers.

static ERR acquire_numeric_target(extVectorFilter *Filter, filter_step &Step)
{
   const auto &render = *Filter->Render;
   const auto clip = render.scratch_clip();
   size_t bytes;
   int stride;

   const size_t pixel_bytes = filter_components(Step.Format) * sizeof(float);
   if (auto error = filter_surface_size(clip.width(), clip.height(), pixel_bytes, bytes, stride); error != ERR::Okay) {
      return error;
   }

   if (auto error = acquire_filter_surface(Filter, bytes, Step.Surface, true); error != ERR::Okay) return error;

   std::fill_n((float *)Step.Surface->Data.get(), bytes / sizeof(float), INVALID_FILTER_SAMPLE);

   Step.Result = {
      .Data = Step.Surface->Data.get(),
      .Width = clip.right, .Height = clip.bottom, .Stride = stride,
      .OriginX = render.VectorClip.left, .OriginY = render.VectorClip.top,
      .Valid = clip, .Format = Step.Format
   };

   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the layout and float storage of the active numerical effect's output.  Fails with ERR::NoSupport for
// effects that produce RGBA8 output.

static ERR get_numeric_target(extFilterEffect *Effect, filter_surface_view &Layout, float *&Data)
{
   Layout = {};
   Data = nullptr;
   const auto step = active_filter_step(Effect);
   if (not step) return ERR::InvalidState;
   if (step->Format IS filter_surface_format::RGBA8) return ERR::NoSupport;

   Layout = step->Result;
   Data = (float *)step->Surface->Data.get();
   return ERR::Okay;
}

//********************************************************************************************************************
// Consumes one planned use of a standard source, releasing its storage after the last use.

static void consume_filter_source(extVectorFilter *Self, filter_standard_source &Source)
{
   if (not --Source.Consumers) {
      Source.View = {};
      release_filter_surface(Self, Source.Surface);
   }
}

//********************************************************************************************************************
// Effect-local arrays must initialise every element they read.  They are released along with converted input views
// after the current effect returns, including when the effect fails.

template<class T>
static ERR get_filter_scratch(extFilterEffect *Effect, size_t Count, T *&Data)
{
   if (not active_filter_step(Effect)) return ERR::InvalidState;
   if (Count > MAX_FILTER_SURFACE_BYTES / sizeof(T)) return ERR::AllocMemory;

   auto filter = Effect->Filter;
   filter_surface *surface;
   if (auto error = acquire_filter_surface(filter, Count * sizeof(T), surface); error != ERR::Okay) return error;
   auto &working = filter->Render->WorkingSurfaces;
   surface->Next = working;
   working = surface;
   if (not active_filter_step(Effect)) return ERR::InvalidState;
   Data = (T *)surface->Data.get();
   return ERR::Okay;
}

//********************************************************************************************************************
// Resolves an input binding to a surface view.  References return the producer's result, and standard sources are
// populated lazily and shared by their readers until their last planned use.  Returns ERR::Continue for an unset
// optional input.

static ERR read_filter_input(extFilterEffect *Effect, const filter_input &Input, filter_surface_view &Output)
{
   kt::Log log(__FUNCTION__);
   Output = {};
   auto filter = Effect->Filter;
   auto &render = *filter->Render;
   kt::SwitchContext ctx(filter);
   const auto &input = Input;

   log.msg(VLF::BRANCH|VLF::DETAIL, "%s #%d <- ID: #%u, Type: %d", Effect->className(), Effect->UID,
      input.Producer ? input.Producer->Effect->UID : 0, int(input.Source));

   if (not active_filter_step(Effect)) return ERR::InvalidState;

   if (input.Source IS VSF::REFERENCE) {
      if (not input.Producer->Result.Data) {
         return filter_input_error(Effect, input.Role, input.Slot, ERR::NoData, "producer has no result in this draw",
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

   auto &source = render.Plan.standard_source(input.Source);
   if (not source.View.Bitmap) {
      objBitmap *bmp;
      if (auto error = acquire_filter_bitmap(filter, source.Surface, bmp); error != ERR::Okay) return error;
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

         consume_filter_source(filter, render.Plan.standard_source(VSF::GRAPHIC));
      }
      else {
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

      source.View = filter_view(render, bmp);
   }

   Output = source.View;
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the view for the active effect's Input, Mix or SourceList[Slot] binding using its validated format.

static ERR get_source_view(extFilterEffect *Effect, filter_surface_view &Output, filter_input_role Role, size_t Slot)
{
   Output = {};
   const auto step = active_filter_step(Effect);
   const size_t index = (Role IS filter_input_role::MIX) ? 1 : Slot;

   if ((not step) or (Role IS filter_input_role::NAMED) or (index >= step->Inputs.size()) or
       (step->Inputs[index].Role != Role)) return ERR::InvalidState;

   return read_filter_input(Effect, step->Inputs[index], Output);
}

//********************************************************************************************************************
// As for get_source_view(), returning the bitmap alone.

static ERR get_source_bitmap(extFilterEffect *Effect, const objBitmap **Bitmap, filter_input_role Role, size_t Slot)
{
   filter_surface_view source;
   auto error = get_source_view(Effect, source, Role, Slot);
   *Bitmap = (error IS ERR::Okay) ? source.Bitmap : nullptr;
   if ((error IS ERR::Okay) and (not *Bitmap)) return ERR::NoSupport;
   return error;
}

//********************************************************************************************************************
// Returns the planned binding for a named input of the active effect.  Named inputs occupy the end of the step's
// input span, in declaration order.

static const filter_input * planned_named_input(const extFilterEffect *Effect, std::string_view Name)
{
   const auto step = active_filter_step(Effect);
   if ((not step) or (not Effect->FilterExtension)) return nullptr;
   const auto named = Effect->FilterExtension->Inputs;
   const auto inputs = step->Inputs.last(named.size());
   for (size_t i = 0; i < named.size(); i++) {
      if (Name IS named[i].Name) return &inputs[i];
   }
   return nullptr;
}

//********************************************************************************************************************
// Prepares a map view for a named map input of the active effect.  An unset input yields a view that returns the
// contract's fallback value.

static ERR get_filter_map(extFilterEffect *Effect, std::string_view Name, filter_map_view &Output)
{
   Output = {};
   const auto input = planned_named_input(Effect, Name);
   if (not input) return ERR::InvalidState;
   if (input->Contract.Kind IS filter_input_kind::IMAGE) return ERR::NoSupport;
   Output.Contract = input->Contract;
   if (input->Source IS VSF::NONE) return ERR::Okay;
   return read_filter_input(Effect, *input, Output.Source);
}

//********************************************************************************************************************
// Reads a named image input.  For inputs that permit unit-channel conversion, finite scalar values are grey and
// vectors use R/G.  Values clamp to [0,1] and round to 8 bits.  Invalid pixels remain transparent.

static ERR get_named_filter_bitmap(extFilterEffect *Effect, std::string_view Name,
   filter_surface_view &Output)
{
   Output = {};
   const auto input = planned_named_input(Effect, Name);
   if (not input) return ERR::InvalidState;
   if (input->Contract.Kind != filter_input_kind::IMAGE) return ERR::NoSupport;

   filter_surface_view source;

   if (auto error = read_filter_input(Effect, *input, source); error != ERR::Okay) return error;

   if (source.Bitmap) {
      Output = source;
      return ERR::Okay;
   }

   if (input->Contract.BitmapConversion != filter_bitmap_conversion::UNIT_CHANNELS) return ERR::NoSupport;

   auto filter = Effect->Filter;
   objBitmap *bitmap;
   if (auto error = get_filter_bitmap(filter, filter->Render->WorkingSurfaces, bitmap); error != ERR::Okay) {
      return error;
   }

   if (not active_filter_step(Effect)) return ERR::InvalidState;

   const auto &format = *bitmap->ColourFormat;
   const bool scalar = source.Format IS filter_surface_format::SCALAR_F32;
   const auto to_byte = [](float Value) { return uint8_t(std::lround(255.0 * std::clamp(Value, 0.0f, 1.0f))); };
   for (int y = source.Valid.top; y < source.Valid.bottom; y++) {
      auto dest = bitmap->Data + std::ptrdiff_t(y) * bitmap->LineWidth;
      for (int x = source.Valid.left; x < source.Valid.right; x++) {
         filter_map_value value;
         if (not read_numeric_sample(source, x, y, value)) continue;
         auto pixel = dest + std::ptrdiff_t(x) * 4;
         const auto red = to_byte(value[0]);
         pixel[format.RedPos >> 3] = red;
         pixel[format.GreenPos >> 3] = scalar ? red : to_byte(value[1]);
         pixel[format.BluePos >> 3] = scalar ? red : 0;
         pixel[format.AlphaPos >> 3] = 255;
      }
   }

   Output = filter_view(*filter->Render, bitmap);
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the input in the requested encoding and alpha representation, copying it to a working surface only when
// a conversion is needed.  Decode before premultiplying.  The filter bank uses a common RGBA8 layout, so a working
// copy preserves the source bytes exactly before the bitmap conversion routines perform their usual rounding.

static ERR working_input(extFilterEffect *Effect, const filter_surface_view &Source, CS Encoding, filter_alpha Alpha,
   filter_surface_view &Output)
{
   if (not active_filter_step(Effect)) return ERR::InvalidState;
   if ((Source.Alpha IS Alpha) and (Source.Encoding IS Encoding)) {
      Output = Source;
      return ERR::Okay;
   }

   auto filter = Effect->Filter;
   objBitmap *work;
   if (auto error = get_filter_bitmap(filter, filter->Render->WorkingSurfaces, work); error != ERR::Okay) return error;
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

   if (Alpha IS filter_alpha::PREMULTIPLIED) {
      if (auto error = work->premultiply(); error != ERR::Okay) return error;
   }
   Output = filter_view(*filter->Render, work);
   return ERR::Okay;
}

static ERR premultiplied_input(extFilterEffect *Effect, const filter_surface_view &Source, CS Encoding,
   filter_surface_view &Output)
{
   return working_input(Effect, Source, Encoding, filter_alpha::PREMULTIPLIED, Output);
}

//********************************************************************************************************************
// Suppress only this pipeline in the private scene that is drawing its raw input.  A request from any other
// scene will still detect an active instance, including indirect recursion through another filter.

static ERR draw_filter_capture(const extVectorFilter *Suppressed, extVectorScene *Scene, extVector *Root,
   objBitmap *Bitmap, int X, int Y)
{
   filter_capture capture { Suppressed, Root };
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
   auto &source = render.Plan.standard_source(VSF::GRAPHIC);

   if (source.View.Bitmap) {
      Output = source.View;
      return ERR::Okay;
   }

   kt::SwitchContext ctx(Self);
   objBitmap *sg;
   if (auto error = acquire_filter_bitmap(Self, source.Surface, sg, "source_graphic"); error != ERR::Okay) {
      return error;
   }

   if ((Self->GraphRevision != render.Revision) or Self->collecting()) return ERR::InvalidState;

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

   Output = source.View = filter_view(render, sg);
   return ERR::Okay;
}

//********************************************************************************************************************
// Sum the sampling margins across the pipeline.  Reference tiles are included separately using their geometry.

static ERR filter_reach(const filter_plan &Plan, double &ReachX, double &ReachY, bool &EntireRegion)
{
   ReachX = ReachY = 0;
   EntireRegion = false;
   for (const auto &step : Plan.Steps) {
      const auto sampling = step.Operation.Sampling(step);
      const auto valid = [](const filter_sampling &Sampling) {
         const bool unbounded = Sampling.Access IS filter_access::FULL_INPUT;
         return (Sampling.X >= 0) and (Sampling.Y >= 0) and
            (unbounded or (std::isfinite(Sampling.X) and std::isfinite(Sampling.Y)));
      };

      if (not valid(sampling)) {
         kt::Log("FilterGeometry").warning("%s #%d has invalid sampling reach.", step.Effect->className(),
            step.Effect->UID);
         return ERR::InvalidValue;
      }

      double reach_x = sampling.X, reach_y = sampling.Y;
      EntireRegion = EntireRegion or sampling.EntireRegion;
      for (const auto &input : step.Inputs) {
         if (input.Role != filter_input_role::NAMED) continue;
         const auto &reach = input.Contract.Sampling;
         if (not valid(reach)) {
            return filter_input_error(step.Effect, input.Role, input.Slot, ERR::InvalidValue,
               "invalid sampling reach");
         }

         if (input.Source IS VSF::NONE) continue;

         reach_x = std::max(reach_x, reach.X);
         reach_y = std::max(reach_y, reach.Y);
         EntireRegion = EntireRegion or reach.EntireRegion;
      }

      ReachX += reach_x;
      ReachY += reach_y;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Replaces Region with the axis-aligned bounds of its transformed corners.  Returns false if any result is not
// finite.

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

static TClipRectangle<double> resolve_subregion(const filter_render_context &, const filter_step &);

//********************************************************************************************************************
// Reserve the producers whose entire subregion an effect can read, with the sampling margin they may need, even when
// they are off-page.  These are the tiles of TileFX and named inputs declared with WholeProducer.  The subregion of a
// named input is only included if Fits accepts the result, otherwise the effect reads the processed part of it.
// Returns true if a named input was included.

template<class FITS>
static bool include_whole_inputs(filter_render_context &Render, const TClipRectangle<double> &Region,
   double MarginX, double MarginY, TClipRectangle<double> &Clip, FITS Fits)
{
   auto tile_producer = [](const filter_step &Step) -> const filter_step * {
      if (Step.Effect->classID() IS CLASSID::TILEFX) return Step.Inputs.front().Producer;
      return nullptr;
   };
   auto whole_producer = [](const filter_input &Input) {
      return (Input.Role IS filter_input_role::NAMED) and Input.Contract.WholeProducer and Input.Producer;
   };
   auto &steps = Render.Plan.Steps;
   if (std::ranges::none_of(steps, [&](const filter_step &Step) {
      return tile_producer(Step) or std::ranges::any_of(Step.Inputs, whole_producer);
   })) return false;

   // Resolve in draw order so that a producer can inherit the subregion of an earlier effect.
   for (auto &step : steps) step.Subregion = resolve_subregion(Render, step);

   // Returns the producer's subregion in scene pixels with the margin, or an invalid rectangle.

   auto producer_bounds = [&](const filter_step &Producer) -> TClipRectangle<double> {
      auto bounds = Producer.Subregion;
      if ((not bounds.valid()) or (not finite_region(bounds))) return { 0.0 };
      if (not transform_filter_region(bounds, Render.ToScene)) return { 0.0 };
      bounds.shrinking(Region);
      if (not bounds.valid()) return { 0.0 };
      bounds = { std::floor(bounds.left) - MarginX, std::floor(bounds.top) - MarginY,
         std::ceil(bounds.right) + MarginX, std::ceil(bounds.bottom) + MarginY };
      bounds.shrinking(Region);
      return bounds;
   };

   // Reserve all required tiles before checking whether optional named inputs fit.
   for (const auto &step : steps) {
      const auto producer = tile_producer(step);
      if ((not producer) or (not step.Subregion.valid()) or (not producer->Subregion.valid())) continue;

      // The output lies inside the original tile, so this step only reads at the current pixel.
      const auto &output = step.Subregion;
      const auto &tile = producer->Subregion;
      if ((output.left >= tile.left) and (output.top >= tile.top) and
          (output.right <= tile.right) and (output.bottom <= tile.bottom)) continue;
      if (auto bounds = producer_bounds(*producer); bounds.valid()) Clip.expanding(bounds);
   }

   bool included = false;
   for (const auto &step : steps) {
      if (not step.Subregion.valid()) continue;
      for (const auto &input : step.Inputs) {
         if (not whole_producer(input)) continue;
         const auto bounds = producer_bounds(*input.Producer);
         if (not bounds.valid()) continue;
         auto expanded = Clip;
         expanded.expanding(bounds);
         if ((expanded.left IS Clip.left) and (expanded.top IS Clip.top) and (expanded.right IS Clip.right) and
             (expanded.bottom IS Clip.bottom)) continue;
         if (not Fits(expanded)) continue;
         Clip = expanded;
         included = true;
      }
   }
   return included;
}

//********************************************************************************************************************
// Computes the processing rectangle in scene device pixels.  The filter region is extended by the pipeline's
// sampling reach.  Effects that need the whole filter region use it directly when its processing surface fits.
// Otherwise the processing rectangle is limited to the visible page plus a bounded margin, the source tiles and the
// subregions of whole named inputs that fit.

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
   bool entire_region;
   if (auto error = filter_reach(render.Plan, reach_x, reach_y, entire_region); error != ERR::Okay) return error;

   // True if Clip can be allocated as a processing surface.

   const auto allocatable = [](const TClipRectangle<double> &Clip) {
      if ((Clip.left < std::numeric_limits<int>::min()) or (Clip.top < std::numeric_limits<int>::min()) or
          (Clip.right > std::numeric_limits<int>::max()) or (Clip.bottom > std::numeric_limits<int>::max()) or
          (Clip.width() > 0xffff) or (Clip.height() > 0xffff)) return false;
      size_t bytes;
      int stride;
      return filter_surface_size(int(Clip.width()), int(Clip.height()), 4, bytes, stride) IS ERR::Okay;
   };

   auto clip = region;
   render.Expanded = entire_region and (not render.Bounded) and allocatable(region);
   if (not render.Expanded) {
      const double max_margin = std::max(page_width, page_height);
      const double margin_x = std::ceil(std::min(reach_x, max_margin));
      const double margin_y = std::ceil(std::min(reach_y, max_margin));
      clip.shrinking({ -margin_x, -margin_y, page_width + margin_x, page_height + margin_y });
      if (not clip.valid()) return ERR::NothingDone;
      // Whole named inputs are left out of a bounded retry, as they are the likely cause of its failure.

      const auto fits_named = [&](const TClipRectangle<double> &Clip) {
         return (not render.Bounded) and allocatable(Clip);
      };
      if (include_whole_inputs(render, region, margin_x, margin_y, clip, fits_named)) render.Expanded = true;
   }

   if ((clip.left < std::numeric_limits<int>::min()) or (clip.top < std::numeric_limits<int>::min()) or
       (clip.right > std::numeric_limits<int>::max()) or (clip.bottom > std::numeric_limits<int>::max()) or
       (clip.width() > 0xffff) or (clip.height() > 0xffff)) return ERR::OutOfRange;

   render.VectorClip = { int(clip.left), int(clip.top), int(clip.right), int(clip.bottom) };
   if (auto error = filter_surface_size(render.VectorClip.width(), render.VectorClip.height(), 4,
       render.BitmapBytes, render.BitmapStride); error != ERR::Okay) return error;

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

static TClipRectangle<double> resolve_subregion(const filter_render_context &Render, const filter_step &Step)
{
   TClipRectangle<double> inputs = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
      std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
   bool referenced = false, standard = false;

   for (const auto &input : Step.Inputs) {
      if (not input.Contract.ContributesRegion) continue;
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

   return { x, y, x + width, y + height };
}

static ERR compute_subregion(const filter_render_context &Render, filter_step &Step)
{
   auto local = Step.Subregion = resolve_subregion(Render, Step);
   if ((not finite_region(local)) or (local.valid() and (not transform_filter_region(local, Render.Transform)))) {
      kt::Log("FilterGeometry").warning("%s #%d has an invalid subregion.", Step.Effect->className(), Step.Effect->UID);
      return ERR::InvalidValue;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Test the pixel centres in user space.  The common case covers the entire filter region.

static void clip_to_subregion(const filter_render_context &Render, const filter_step &Step)
{
   const auto &sub = Step.Subregion;
   auto region = Render.FilterRegion;
   if (Step.Format != filter_surface_format::RGBA8) {
      // The device-space processing rectangle can include padding around a transformed primitive.
      region = { 0, 0, double(Step.Result.Width), double(Step.Result.Height) };
      transform_filter_region(region, Render.Inverse);
   }

   if ((sub.left <= region.left) and (sub.top <= region.top) and
       (sub.right >= region.right) and (sub.bottom >= region.bottom)) return;

   auto clip_pixels = [&](const TClipRectangle<int> &Valid, auto ClearRow) {
      for (int y = Valid.top; y < Valid.bottom; y++) {
         const auto clear = ClearRow(y);
         for (int x = Valid.left; x < Valid.right; x++) {
            double px, py;
            map_filter_pixel_centre(Render.Inverse, x, y, px, py);
            if (not sub.hit_test(px, py)) clear(x);
         }
      }
   };

   if (auto bmp = Step.Effect->Target; bmp) {
      if (bmp->BytesPerPixel != 4) return;
      clip_pixels({ bmp->Clip.Left, bmp->Clip.Top, bmp->Clip.Right, bmp->Clip.Bottom }, [&](int Y) {
         auto row = (uint32_t *)(bmp->Data + std::ptrdiff_t(Y) * bmp->LineWidth);
         return [=](int X) { row[X] = 0; };
      });
   }
   else {
      const auto &view = Step.Result;
      auto data = Step.Surface->Data.get();
      const int components = filter_components(Step.Format);
      clip_pixels(view.Valid, [&](int Y) {
         auto row = (float *)(data + std::ptrdiff_t(Y) * view.Stride);
         return [=](int X) { std::fill_n(row + std::ptrdiff_t(X) * components, components, INVALID_FILTER_SAMPLE); };
      });
   }
}

//********************************************************************************************************************
// Returns true if an RGBA8 result stores normal components as data.  Tiling carries over the flag from its producer.

static bool normal_map_result(const filter_step &Step)
{
   const auto effect = Step.Effect;
   if (effect->classID() IS CLASSID::LIGHTINGFX) return ((const extLightingFX *)effect)->Type IS LT::NORMAL;
   if ((effect->classID() IS CLASSID::TILEFX) and (not Step.Inputs.empty())) {
      if (auto producer = Step.Inputs.front().Producer; producer) return producer->Result.NormalMap;
   }
   return false;
}

//********************************************************************************************************************
// A single attempt at the draw.  If Bounded is true, processing is limited to the page, margin and source tiles.

static ERR render_filter_pass(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector,
   objBitmap *BkgdBitmap, VCS ColourSpace, filter_render_result &Output, int BkgdOriginX, int BkgdOriginY,
   bool Bounded, bool &Expanded)
{
   kt::Log log(__FUNCTION__);
   Expanded = false;
   const bool diagnostics = GetResource(RES::LOG_LEVEL) >= 6;

   filter_render_context render = {};
   render.ClientVector      = Vector;
   render.ClientViewport    = Viewport;
   render.BkgdOriginX       = BkgdOriginX;
   render.BkgdOriginY       = BkgdOriginY;
   render.ClientColourSpace = ColourSpace;
   render.Revision          = Self->GraphRevision;
   render.Bounded           = Bounded;

   for (auto effect : Self->Effects) effect->Target = nullptr;

   release_filter_surface(Self, Self->LastOutput);

   Self->Render = &render;
   auto graph_changed = [&]() { return (Self->GraphRevision != render.Revision) or Self->collecting(); };

   bool completed = false;

   auto cleanup = kt::Defer([&]() {
      if (not completed) {
         for (auto effect : Self->Effects) effect->Target = nullptr;
         for (auto &step : render.Plan.Steps) release_filter_surface(Self, step.Surface);
         release_filter_surfaces(Self, render.WorkingSurfaces);
         for (auto &source : render.Plan.StandardSources) release_filter_surface(Self, source.Surface);
         trim_filter_surfaces(Self, std::min(render.PeakLiveBytes, MAX_RETAINED_FILTER_BYTES));
      }

      Self->Render = nullptr;
   });

   if (Self->Effects.empty()) return log.warning(ERR::FieldNotSet);
   auto &plan = render.Plan;
   if (auto error = prepare_filter_plan(Self, plan); error != ERR::Okay) return error;

   CSTRING filter_name = Self->Name;
   if ((not filter_name) or (not filter_name[0])) filter_name = "Unnamed";

   CSTRING vector_name = Vector->Name;
   if ((not vector_name) or (not vector_name[0])) vector_name = "Unnamed";

   log.msg(VLF::BRANCH|VLF::DETAIL, "Rendering '%s' filter content for %s #%d '%s'.", filter_name,
      Vector->Class->ClassName.c_str(), Vector->UID, vector_name);

   if (plan.standard_source(VSF::GRAPHIC).Consumers or plan.standard_source(VSF::BKGD).Consumers or
       plan.standard_source(VSF::BKGD_ALPHA).Consumers) render.BkgdBitmap = BkgdBitmap;

   if (auto error = prepare_filter_geometry(Self); error != ERR::Okay) return error;
   if (graph_changed()) return log.warning(ERR::InvalidState);
   if (auto error = set_clip_region(Self); error != ERR::Okay) return error;
   if (graph_changed()) return log.warning(ERR::InvalidState);

   Expanded = render.Expanded;

   auto release_result = [&](filter_step &Step) {
      Step.Effect->Target = nullptr;
      Step.Result = {};
      release_filter_surface(Self, Step.Surface);
   };

   for (auto &step : plan.Steps) {
      const auto effect = step.Effect;
      const auto started = diagnostics ? PreciseTime() : 0;

      objBitmap *target = nullptr;
      if (step.Format IS filter_surface_format::RGBA8) {
         if (auto error = acquire_filter_surface(Self, render.BitmapBytes, step.Surface); error != ERR::Okay) {
            return error;
         }
         if (graph_changed()) return log.warning(ERR::InvalidState);
         if (auto error = bind_filter_bitmap(Self, *step.Surface, target); error != ERR::Okay) return error;
      }
      else if (auto error = acquire_numeric_target(Self, step); error != ERR::Okay) return error;
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

      if (target and ((target->ColourSpace != CS::SRGB) or ((target->Flags & BMF::PREMUL) != BMF::NIL))) {
         return log.warning(ERR::InvalidState);
      }

      if ((&step IS plan.Final) or step.Consumers) {
         if (target) {
            step.Result = filter_view(render, target);
            step.Result.NormalMap = normal_map_result(step);
         }
      }
      else release_result(step);

      // A reference is counted even if the current effect skips reading it.  Repeated inputs to the same
      // effect each consume a binding, and the last binding releases the result.

      for (const auto &input : step.Inputs) {
         if (auto producer = input.Producer; producer) {
            if (not --producer->Consumers) release_result(*producer);
         }
         else if (input.Source != VSF::NONE) {
            auto &source = plan.standard_source(input.Source);
            // The last alpha binding can complete without sampling (for example, an empty primitive).
            if ((input.Source IS VSF::ALPHA) and (source.Consumers IS 1) and (not source.View.Bitmap)) {
               consume_filter_source(Self, plan.standard_source(VSF::GRAPHIC));
            }
            consume_filter_source(Self, source);
         }
      }

      release_filter_surfaces(Self, render.WorkingSurfaces);
      if (diagnostics) {
         log.detail("Filter effect: %s #%u elapsed_us=%" PRId64, effect->className(), effect->UID,
            PreciseTime() - started);
      }

      if (graph_changed()) return log.warning(ERR::InvalidState);
   }

   const auto final_bytes = plan.Final->Surface->Bytes;
   const auto retain = std::min(render.PeakLiveBytes, MAX_RETAINED_FILTER_BYTES);
   trim_filter_surfaces(Self, std::max(final_bytes, retain));
   if (graph_changed()) return log.warning(ERR::InvalidState);

   const auto &result = plan.Final->Result;
   auto out = plan.Final->Effect->Target;

   #ifdef EXPORT_FILTER_BITMAP
      save_bitmap(out, std::to_string(Self->UID) + "_" + std::to_string(Vector->UID) + "_output");
   #endif

   if (diagnostics) {
      const auto retained = Self->SurfaceBytes - (final_bytes > retain ? final_bytes : 0);
      log.detail("Filter storage: allocations=%zu allocated_bytes=%zu reuses=%zu retained_bytes=%zu "
         "peak_live_bytes=%zu peak_live_surfaces=%zu", render.Allocations, render.AllocatedBytes, render.Reuses,
         retained, render.PeakLiveBytes, render.PeakLiveSurfaces);
   }

   if (graph_changed()) return log.warning(ERR::InvalidState);

   Self->LastOutput = std::exchange(plan.Final->Surface, nullptr);
   Self->Composing = true;
   Output = { out, result.OriginX, result.OriginY, Self->Opacity };
   completed = true;
   return ERR::Okay;
}

//********************************************************************************************************************
// Main rendering routine for filter effects.  Called by the scene graph renderer whenever a vector uses a filter.
// The caller holds the filter lock through rendering and composition, and must call finish_filter_composition()
// after a successful render.  ColourSpace is the client's resolved colour space in the renderer, which can be
// inherited from a vector that is not a physical ancestor, e.g. the viewport that consumes a live pattern.
//
// An expanded draw that runs out of storage is repeated with processing limited to the page, margin and source
// tiles.  Scratch storage can depend on the effect's inputs as well as the clip, so use the draw's live allocations.

ERR render_filter(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector,
   objBitmap *BkgdBitmap, VCS ColourSpace, filter_render_result &Output, int BkgdOriginX, int BkgdOriginY)
{
   kt::Log log(__FUNCTION__);

   Output = {};
   if ((not Vector) or (not Viewport)) return log.warning(ERR::NullArgs);

   if (Self->collecting()) return ERR::MarkedForDeletion;
   if (Self->Render or Self->Composing) return ERR::Recursion;

   kt::SwitchContext context(Self);

   const auto revision = Self->GraphRevision;
   bool expanded;
   auto error = render_filter_pass(Self, Viewport, Vector, BkgdBitmap, ColourSpace, Output, BkgdOriginX,
      BkgdOriginY, false, expanded);
   if ((error != ERR::AllocMemory) or (not expanded) or (Self->GraphRevision != revision) or
       Self->collecting()) return error;

   log.detail("Filter #%d ran out of storage with an expanded clip; retrying with a bounded clip.", Self->UID);
   return render_filter_pass(Self, Viewport, Vector, BkgdBitmap, ColourSpace, Output, BkgdOriginX, BkgdOriginY,
      true, expanded);
}

//********************************************************************************************************************
// Ends the composition that follows a successful render_filter().  An oversized final surface is freed immediately
// rather than retained for the next draw.

void finish_filter_composition(extVectorFilter *Self)
{
   if (Self->LastOutput->Bytes > MAX_RETAINED_FILTER_BYTES) {
      for (auto effect : Self->Effects) effect->Target = nullptr;
      auto slot = std::exchange(Self->LastOutput, nullptr);
      Self->SurfaceBytes -= slot->Bytes;
      delete slot;
   }
   Self->Composing = false;
}

/*********************************************************************************************************************
-ACTION-
Clear: Removes all filter effects and releases their working storage.
-END-
*********************************************************************************************************************/

static ERR VECTORFILTER_Clear(extVectorFilter *Self)
{
   kt::Log log;

   if (Self->Render or Self->Composing) return log.warning(ERR::InUse);

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
   filter_modified(Self);
   release_filter_surface(Self, Self->LastOutput);
   auto surfaces = std::exchange(Self->FreeSurfaces, nullptr);
   Self->SurfaceBytes = 0;
   auto source_scene = std::exchange(Self->SourceScene, nullptr);
   while (surfaces) {
      auto slot = surfaces;
      surfaces = slot->Next;
      delete slot;
   }
   if (source_scene) FreeResource(source_scene);

   // Callbacks and deferred destruction can change ownership while the detached effects are being freed.
   for (auto id : ids) {
      if (auto effect = GetObjectPtr(id); effect and (effect->Owner IS Self)) FreeResource(id);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

extVectorFilter::~extVectorFilter()
{
   VECTORFILTER_Clear(this);
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

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_AspectRatio(extVectorFilter *Self, VFA Value)
{
   Self->AspectRatio = Value;
   filter_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

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

An effect can override this setting with its own @FilterEffect.ColourSpace, which is the SVG equivalent of
`color-interpolation-filters` on a filter primitive.

The SVG standard defaults to linear RGB for filter effects, so the SVG parser selects `LINEAR_RGB` unless the
`color-interpolation-filters` attribute states otherwise.  Filters created through the API retain the SRGB default.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_ColourSpace(extVectorFilter *Self, VCS Value)
{
   Self->ColourSpace = Value;
   filter_modified(Self);
   return ERR::Okay;
}

static ERR describe_filter_xml(extFilterEffect *Effect, filter_xml_element &Element)
{
   ERR error = ERR::Okay;
   switch (Effect->classID()) {
      case CLASSID::BLURFX:         error = blur_xml(*(extBlurFX *)Effect, Element); break;
      case CLASSID::COLOURFX:       error = colour_xml(*(extColourFX *)Effect, Element); break;
      case CLASSID::COMPOSITEFX:    error = composite_xml(*(extCompositeFX *)Effect, Element); break;
      case CLASSID::CONVOLVEFX:     error = convolve_xml(*(extConvolveFX *)Effect, Element); break;
      case CLASSID::DISPLACEMENTFX: error = displacement_xml(*(extDisplacementFX *)Effect, Element); break;
      case CLASSID::FLOODFX:        error = flood_xml(*(extFloodFX *)Effect, Element); break;
      case CLASSID::IMAGEFX:        error = image_xml(*(extImageFX *)Effect, Element); break;
      case CLASSID::LIGHTINGFX:     error = lighting_xml(*(extLightingFX *)Effect, Element); break;
      case CLASSID::MERGEFX:        Element.Name = "feMerge"; break;
      case CLASSID::MORPHOLOGYFX:   error = morphology_xml(*(extMorphologyFX *)Effect, Element); break;
      case CLASSID::OFFSETFX:       error = offset_xml(*(extOffsetFX *)Effect, Element); break;
      case CLASSID::REMAPFX:        error = remap_xml(*(extRemapFX *)Effect, Element); break;
      case CLASSID::SOURCEFX:       error = source_xml(*(extSourceFX *)Effect, Element); break;
      case CLASSID::TILEFX:         Element.Name = "feTile"; break;
      case CLASSID::TURBULENCEFX:   error = turbulence_xml(*(extTurbulenceFX *)Effect, Element); break;
      case CLASSID::WAVEFUNCTIONFX: error = wavefunction_xml(*(extWaveFunctionFX *)Effect, Element); break;
      default: return ERR::NoSupport;
   }
   if (error != ERR::Okay) return error;

   write_filter_xml_subregion(Element, *Effect);
   return ERR::Okay;
}

//********************************************************************************************************************

static filter_positions filter_xml_positions(std::span<extFilterEffect * const> Effects)
{
   filter_positions positions;
   for (size_t i = 0; i < Effects.size(); i++) positions.emplace(Effects[i], i);
   return positions;
}

//********************************************************************************************************************

static ERR bind_filter_xml_input(filter_xml_element &Element, CSTRING Attribute, VSF Source, objFilterEffect *Producer,
   const filter_positions &Positions, size_t Consumer, std::span<filter_xml_element> Graph,
   std::string_view ReferencePrefix = {})
{
   switch (Source) {
      case VSF::GRAPHIC: Element.attribute(Attribute, "SourceGraphic"); break;
      case VSF::ALPHA: Element.attribute(Attribute, "SourceAlpha"); break;
      case VSF::BKGD: Element.attribute(Attribute, "BackgroundImage"); break;
      case VSF::BKGD_ALPHA: Element.attribute(Attribute, "BackgroundAlpha"); break;
      case VSF::FILL: Element.attribute(Attribute, "FillPaint"); break;
      case VSF::STROKE: Element.attribute(Attribute, "StrokePaint"); break;
      case VSF::REFERENCE: {
         if (not Producer) return ERR::FieldNotSet;
         const auto position = Positions.find(Producer);
         if ((position IS Positions.end()) or (position->second >= Consumer)) return ERR::InvalidValue;
         const auto name = "effect" + std::to_string(position->second);
         if (not Graph.empty()) Graph[position->second].Result = name;
         Element.attribute(Attribute, std::string(ReferencePrefix) + name);
         break;
      }
      case VSF::NONE: return ERR::FieldNotSet;
      default: return ERR::NoSupport;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR bind_merge_xml(const extMergeFX &Effect, filter_xml_element &Element, const filter_positions &Positions,
   size_t Consumer, std::span<filter_xml_element> Graph = {})
{
   for (size_t i = 0; i < Effect.Layers.size(); i++) {
      const auto &layer = Effect.Layers[i];
      const auto &source = Effect.SourceList[i];
      auto &node = Element.child("feMergeNode");
      if (layer.Opacity != 1.0) node.attribute("xvg:opacity", layer.Opacity);
      if (not layer.Enabled) node.attribute("xvg:enabled", "false");
      const bool unresolved = (source.SourceType IS VSF::REFERENCE) and (not source.Effect);
      const bool inactive = (not layer.Enabled) or (layer.Opacity IS 0);
      if ((source.SourceType IS VSF::NONE) or (unresolved and inactive)) node.attribute("xvg:source", "none");
      else if (auto error = bind_filter_xml_input(node, "in", source.SourceType, source.Effect, Positions,
          Consumer, Graph); error != ERR::Okay) return error;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR bind_filter_xml(std::span<extFilterEffect * const> Effects, std::span<filter_xml_element> Elements)
{
   const auto positions = filter_xml_positions(Effects);
   for (size_t i = 0; i < Effects.size(); i++) {
      auto effect = Effects[i];
      auto &element = Elements[i];

      auto bind = [&](CSTRING Attribute, VSF Source, objFilterEffect *Producer, std::string_view Prefix = {}) {
         return bind_filter_xml_input(element, Attribute, Source, Producer, positions, i, Elements, Prefix);
      };

      const auto operation = describe_filter_operation(effect);
      if ((operation.Inputs IS filter_inputs::SINGLE) or (operation.Inputs IS filter_inputs::DUAL)) {
         if (auto error = bind("in", effect->SourceType, effect->Input); error != ERR::Okay) return error;
      }
      if (operation.Inputs IS filter_inputs::DUAL) {
         if (auto error = bind("in2", effect->MixType, effect->Mix); error != ERR::Okay) return error;
      }
      else if ((effect->classID() IS CLASSID::LIGHTINGFX) and (effect->MixType != VSF::NONE)) {
         if (auto error = bind("xvg:albedo", effect->MixType, effect->Mix); error != ERR::Okay) return error;
      }
      if (operation.Inputs IS filter_inputs::MERGE) {
         if (auto error = bind_merge_xml(*(extMergeFX *)effect, element, positions, i, Elements);
             error != ERR::Okay) return error;
      }
      if (effect->classID() IS CLASSID::DISPLACEMENTFX) {
         if (((const extDisplacementFX *)effect)->VectorMap.Reference) return ERR::NoSupport;
      }
      else if (effect->classID() IS CLASSID::COMPOSITEFX) {
         const auto &mask = ((extCompositeFX *)effect)->AmountMask;
         if (mask.Source != VSF::NONE) {
            if (auto error = bind("xvg:amountMask", mask.Source, mask.Reference); error != ERR::Okay) return error;
         }
      }
      else if (effect->FilterExtension) {
         if (effect->classID() != CLASSID::LIGHTINGFX) return ERR::NoSupport;
         const bool combined = ((const extLightingFX *)effect)->Type IS LT::COMBINED;
         const CSTRING attributes[] = { "xvg:normalMap", "xvg:occlusionMap", "xvg:roughnessMap",
            combined ? "xvg:matcap" : "xvg:combinedMatcap" };
         const auto inputs = effect->FilterExtension->Inputs;
         if (inputs.size() != std::size(attributes)) return ERR::NoSupport;
         for (size_t slot = 0; slot < inputs.size(); slot++) {
            const auto &input = inputs[slot];
            if (input.Source IS VSF::NONE) continue;
            if (input.Source != VSF::REFERENCE) return ERR::NoSupport;
            // The matcap attribute also accepts a path, so its result reference takes a '#' prefix.
            const std::string_view prefix = (&input IS &((const extLightingFX *)effect)->MatcapEffect) ? "#" : "";
            if (auto error = bind(attributes[slot], input.Source, input.Reference, prefix); error != ERR::Okay) {
               return error;
            }
         }
      }
      if (effect->ColourSpace IS VCS::SRGB) element.attribute("color-interpolation-filters", "sRGB");
      else if (effect->ColourSpace IS VCS::LINEAR_RGB) element.attribute("color-interpolation-filters", "linearRGB");
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR FILTEREFFECT_GET_XMLDef(extFilterEffect *Self, std::string &Value)
{
   filter_xml_element element;
   if (Self->classID() IS CLASSID::MERGEFX) {
      if (not Self->Filter) return ERR::InvalidState;
      const auto positions = filter_xml_positions(Self->Filter->Effects);
      const auto position = positions.find(Self);
      if (position IS positions.end()) return ERR::InvalidState;
      if (auto error = bind_merge_xml(*(extMergeFX *)Self, element, positions, position->second);
          error != ERR::Okay) return error;
   }
   if (auto error = describe_filter_xml(Self, element); error != ERR::Okay) return error;
   std::string xml;
   const bool fragment = (not element.Complete) and element.Children.empty();
   if (auto error = element.write(xml, fragment); error != ERR::Okay) return error;
   Value = std::move(xml);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EffectXML: Serialises the filter's primitives as an XML fragment.

The fragment contains complete elements in pipeline order, including primitive bounds, input references and any
required Kōtuku extensions.  Embed it in an SVG filter element with the required Kōtuku and XLink namespace
declarations.  Result names are derived from the authored pipeline order and are local to this fragment.

An unsupported effect or resource returns `ERR::NoSupport`.  File-backed resources must have a document-relative
reference.  A vector source must be a named definition in the filter's scene.  A broken input binding returns an
error.  An empty filter produces an empty fragment.
-END-

*********************************************************************************************************************/

static ERR VECTORFILTER_GET_EffectXML(extVectorFilter *Self, std::string &Value)
{
   std::vector<filter_xml_element> elements(Self->Effects.size());
   if (auto error = bind_filter_xml(Self->Effects, elements); error != ERR::Okay) return error;

   std::string xml;
   for (size_t i = 0; i < elements.size(); i++) {
      auto &element = elements[i];
      if (not element.Result.empty()) element.attribute("result", element.Result);
      if (auto error = describe_filter_xml(Self->Effects[i], element); error != ERR::Okay) return error;
      if (auto error = element.write(xml); error != ERR::Okay) return error;
   }
   Value = std::move(xml);
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
      filter_modified(Self);
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
   filter_modified(Self);
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
   filter_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PrimitiveUnits: Alters the behaviour of some effects that support alternative position calculations.

PrimitiveUnits alters the behaviour of some effects when their dimensions are calculated.  The default value is
`USERSPACE`.  When set to `BOUNDING_BOX`, the effect may calculate its dimensions strictly based on the client vector
using a scaled coordinate space of `(0, 0, 100%, 100%)`.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_PrimitiveUnits(extVectorFilter *Self, VUNIT Value)
{
   Self->PrimitiveUnits = Value;
   filter_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

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

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Units(extVectorFilter *Self, VUNIT Value)
{
   Self->Units = Value;
   filter_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

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
      filter_modified(Self);
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

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_X(extVectorFilter *Self, Unit &Value)
{
   Self->X = Value;
   filter_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

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

static ERR VECTORFILTER_SET_Y(extVectorFilter *Self, Unit &Value)
{
   Self->Y = Value;
   filter_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_def.c"

//********************************************************************************************************************

static const FieldArray clFilterFields[] = {
   { "X",              FDF_UNIT|FDF_RW, nullptr, VECTORFILTER_SET_X },
   { "Y",              FDF_UNIT|FDF_RW, nullptr, VECTORFILTER_SET_Y },
   { "Width",          FDF_UNIT|FDF_RW, nullptr, VECTORFILTER_SET_Width },
   { "Height",         FDF_UNIT|FDF_RW, nullptr, VECTORFILTER_SET_Height },
   { "Opacity",        FDF_DOUBLE|FDF_RW, nullptr, VECTORFILTER_SET_Opacity },
   { "Inherit",        FDF_OBJECT|FDF_RW, nullptr, VECTORFILTER_SET_Inherit },
   { "ResX",           FDF_INT|FDF_RI },
   { "ResY",           FDF_INT|FDF_RI },
   { "Units",          FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORFILTER_SET_Units, &clVectorFilterUnits },
   { "PrimitiveUnits", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORFILTER_SET_PrimitiveUnits, &clVectorFilterPrimitiveUnits },
   { "ColourSpace",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORFILTER_SET_ColourSpace, &clVectorFilterColourSpace },
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORFILTER_SET_AspectRatio, &clVectorFilterAspectRatio },
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

#ifdef UNIT_TESTS
#include "../tests/unit_filter_surfaces.cpp"
#endif
