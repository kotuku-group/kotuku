
#define PRV_VECTOR_MODULE

template<class... Args> void DBG_TRANSFORM(Args...) {
   //log.trace(Args)
}

#include <array>
#include <memory>
#include <unordered_set>
#include <sstream>
#include <set>
#include <unordered_map>
#include <mutex>
#include <stack>
#include <algorithm>

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/picture.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/font.h>
#include <kotuku/strings.hpp>

using namespace pf;

#include "agg_alpha_mask_u8.h"
#include "agg_basics.h"
#include "agg_bounding_rect.h"
#include "agg_conv_dash.h"
#include "agg_conv_stroke.h"
#include "agg_conv_transform.h"
#include "agg_curves.h"
#include "agg_gamma_lut.h"
#include "agg_path_storage.h"
#include "agg_pattern_filters_rgba.h"
#include "agg_pixfmt_gray.h"
#include "agg_pixfmt_rgba.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_renderer_scanline.h"
#include "agg_rendering_buffer.h"
#include "agg_scanline_p.h"
#include "agg_scanline_u.h"
#include "agg_span_allocator.h"
#include "agg_span_converter.h"
#include "agg_span_image_filter_rgba.h"
#include "agg_trans_affine.h"
//#include "agg_conv_marker.h"
//#include "agg_vcgen_markers_term.h"

#include "../link/linear_rgb.h"
#include "../link/unicode.h"

#include <math.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <float.h>

static const double DISPLAY_DPI = 96.0;          // Freetype measurements are based on this DPI.
static const double DEG2RAD     = 0.01745329251994329576923690768489;  // Multiple any angle by this value to convert to radians
static const double RAD2DEG     = 57.295779513082320876798154814105;
static const double SQRT2       = 1.414213562373095; // sqrt(2)
static const double INV_SQRT2   = 1.0 / SQRT2;

extern OBJECTPTR clVectorScene, clVectorViewport, clVectorGroup, clVectorColour;
extern OBJECTPTR clVectorEllipse, clVectorRectangle, clVectorPath, clVectorWave;
extern OBJECTPTR clVectorFilter, clVectorPolygon, clVectorText, clVectorClip;
extern OBJECTPTR clVectorGradient, clVectorImage, clVectorPattern, clVector;
extern OBJECTPTR clVectorSpiral, clVectorShape, clVectorTransition, clImageFX, clSourceFX, clWaveFunctionFX;
extern OBJECTPTR clBlurFX, clColourFX, clCompositeFX, clConvolveFX, clFilterEffect, clDisplacementFX;
extern OBJECTPTR clFloodFX, clMergeFX, clMorphologyFX, clOffsetFX, clTurbulenceFX, clRemapFX, clLightingFX;
extern OBJECTPTR glVectorModule;

typedef agg::pod_auto_array<agg::rgba8, 256> GRADIENT_TABLE;
class objVectorTransition;
class extVectorText;
class extVector;
class extVectorScene;
class extFilterEffect;
class extVectorViewport;
class extVectorClip;

extern ankerl::unordered_dense::map<std::string, std::array<FRGB, 256>> glColourMaps;
extern objConfig *glFontConfig;

class PIXEL_ORDER {
public:
   PIXEL_ORDER() = default;
   PIXEL_ORDER(uint8_t r, uint8_t g, uint8_t b, uint8_t a) : Red(r), Green(g), Blue(b), Alpha(a) {}
   uint8_t Red, Green, Blue, Alpha;
};

class PIXEL_DATA : public PIXEL_ORDER {
public:
   const uint8_t *Data;
   PIXEL_DATA(uint8_t *pData) : Data(pData) { }
   PIXEL_DATA(uint8_t *pData, const PIXEL_ORDER pPX) : PIXEL_ORDER(pPX), Data(pData) {}

   agg::rgba8 getRGB() const {
      return agg::rgba8(Data[Red], Data[Green], Data[Blue], Data[Alpha]);
   }
};

//********************************************************************************************************************

template<class T = double> struct TClipRectangle {
   T left, top, right, bottom;

   TClipRectangle() { }
   TClipRectangle(T Value) : left(Value), top(Value), right(Value), bottom(Value) { }
   TClipRectangle(T pLeft, T pTop, T pRight, T pBottom) : left(pLeft), top(pTop), right(pRight), bottom(pBottom) { }
   TClipRectangle(const class extVector *pVector);
   TClipRectangle(const class extVectorViewport *pVector);

   inline void expanding(const TClipRectangle<T> &Other) {
      if (Other.left   < left)   left   = Other.left;
      if (Other.top    < top)    top    = Other.top;
      if (Other.right  > right)  right  = Other.right;
      if (Other.bottom > bottom) bottom = Other.bottom;
   }

   inline void shrinking(const TClipRectangle<T> &Other) {
      if (Other.left   > left)   left   = Other.left;
      if (Other.top    > top)    top    = Other.top;
      if (Other.right  < right)  right  = Other.right;
      if (Other.bottom < bottom) bottom = Other.bottom;
   }

   inline bool hit_test(const T X, const T Y) const {
      return (X >= left) and (Y >= top) and (X < right) and (Y < bottom);
   }

   inline std::array<T, 4> as_array() const {
      return std::array<T, 4> { left, top, right, bottom };
   }

   inline agg::path_storage as_path() const {
      agg::path_storage path;
      path.move_to(left, top);
      path.line_to(right, top);
      path.line_to(right, bottom);
      path.line_to(left, bottom);
      path.close_polygon();
      return std::move(path);
   }

   // Return the boundary as a path, with a transform already applied.

   inline agg::path_storage as_path(const agg::trans_affine &Transform) const {
      agg::path_storage path;
      path.move_to(Transform.transform({ left, top }));
      path.line_to(Transform.transform({ right, top }));
      path.line_to(Transform.transform({ right, bottom }));
      path.line_to(Transform.transform({ left, bottom }));
      path.close_polygon();
      return std::move(path);
   }

   inline bool valid() const { return (left < right) and (top < bottom); }
   inline T width() const { return right - left; }
   inline T height() const { return bottom - top; }

   TClipRectangle<T> & operator+=(const TClipRectangle<T> &Other) {
      expanding(Other);
      return *this;
   }

   TClipRectangle<T> & operator-=(const TClipRectangle<T> &Other) {
      shrinking(Other);
      return *this;
   }

   TClipRectangle<T> & operator+=(const agg::point_base<T> &Delta) {
      left   += Delta.x;
      top    += Delta.y;
      right  += Delta.x;
      bottom += Delta.y;
      return *this;
   }

   TClipRectangle<T> & operator-=(const agg::point_base<T> &Delta) {
      left   -= Delta.x;
      top    -= Delta.y;
      right  -= Delta.x;
      bottom -= Delta.y;
      return *this;
   }
};

static const TClipRectangle<double> TCR_EXPANDING(DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX);
static const TClipRectangle<double> TCR_SHRINKING(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX);

class InputBoundary {
public:
   OBJECTID vector_id;
   PTC cursor; // This value buffers the Vector.Cursor field for optimisation purposes.
   TClipRectangle<double> bounds; // Collision boundary
   double x, y; // Absolute X,Y without collision
   bool pass_through; // True if input events should be passed through (the cursor will still apply)

   InputBoundary(OBJECTID pV, PTC pC, TClipRectangle<double> &pBounds, double p5, double p6, bool pPass = false) :
      vector_id(pV), cursor(pC), bounds(pBounds), x(p5), y(p6), pass_through(pPass) {};
};

//********************************************************************************************************************

class InputSubscription {
public:
   FUNCTION Callback;
   JTYPE Mask;
   InputSubscription(FUNCTION pCallback, JTYPE pMask) : Callback(pCallback), Mask(pMask) { }
};

class KeyboardSubscription {
public:
   FUNCTION Callback;
   KeyboardSubscription(FUNCTION pCallback) : Callback(pCallback) { }
};

class DashedStroke {
public:
   agg::conv_dash<agg::path_storage> path;
   agg::conv_stroke<agg::conv_dash<agg::path_storage>> stroke;
   std::vector<double> values;

   DashedStroke(agg::path_storage &pPath, int Elements=2) : path(pPath), stroke(path), values(Elements) { }
};

class filter_state {
public:
};

class filter_bitmap {
public:
   objBitmap *Bitmap;
   std::vector<uint8_t> Data;

   filter_bitmap() : Bitmap(nullptr) { };

   ~filter_bitmap() {
      if (Bitmap) { FreeResource(Bitmap); Bitmap = nullptr; }
   };

   objBitmap * get_bitmap(int Width, int Height, TClipRectangle<int> &Clip, bool Debug) {
      pf::Log log;

      if (Width < Clip.right) Width = Clip.right;
      if (Height < Clip.bottom) Height = Clip.bottom;

      if ((Clip.bottom <= Clip.top) or (Clip.right <= Clip.left)) {
         log.warning("Invalid clip region %d %d %d %d", Clip.left, Clip.top, Clip.right, Clip.bottom);
         return nullptr;
      }

      if ((Width < 1) or (Height < 1) or (Width > 0xffff) or (Height > 0xffff)) {
         log.warning("Invalid bitmap size of %dx%d", Width, Height);
         return nullptr;
      }

      if (Bitmap) {
         Bitmap->Width = Width;
         Bitmap->Height = Height;
      }
      else {
         // NB: The clip region defines the true size and no data is allocated by the bitmap itself unless in debug mode.
         Bitmap = objBitmap::create::local(
            fl::Name("dummy_fx_bitmap"),
            fl::Width(Width), fl::Height(Height), fl::BitsPerPixel(32),
            fl::Flags(Debug ? BMF::ALPHA_CHANNEL : (BMF::ALPHA_CHANNEL|BMF::NO_DATA)));
         if (!Bitmap) return nullptr;
      }

      Bitmap->Clip = { Clip.left, Clip.top, Clip.right, Clip.bottom };
      if (Bitmap->Clip.Left < 0) Bitmap->Clip.Left = 0;
      if (Bitmap->Clip.Top < 0) Bitmap->Clip.Top = 0;

      if (!Debug) {
         Bitmap->LineWidth = Clip.width() * Bitmap->BytesPerPixel;

         if (Data.size() < Bitmap->LineWidth * Clip.height()) {
            Data.resize(Bitmap->LineWidth * Clip.height());
         }

         Bitmap->Data = Data.data() - (Clip.left * Bitmap->BytesPerPixel) - (Clip.top * Bitmap->LineWidth);
         Bitmap->CreatorMeta = Data.data();
      }

      return Bitmap;
   }

};

constexpr int TB_NOISE = 1;

#include <kotuku/modules/vector.h>

class FeedbackSubscription {
public:
   FUNCTION Callback;
   FM Mask;
   FeedbackSubscription(FUNCTION pCallback, FM pMask) : Callback(pCallback), Mask(pMask) { }
};

//********************************************************************************************************************

class SceneDef {
   public:
   extVectorScene *HostScene;

   inline void modified();
};

//********************************************************************************************************************

constexpr int MAX_TRANSITION_STOPS = 10;

struct TransitionStop { // Passed to the Stops field.
   double Offset;
   struct VectorMatrix Matrix;
   agg::trans_affine *AGGTransform;
};

class extVectorTransition : public objVectorTransition, public SceneDef {
   public:
   int TotalStops; // Total number of stops registered.

   TransitionStop Stops[MAX_TRANSITION_STOPS];
   bool Dirty:1;
};

class extVectorGradient : public objVectorGradient, public SceneDef {
   public:
   using create = pf::Create<extVectorGradient>;

   std::vector<GradientStop> Stops;  // An array of gradient stop colours.
   struct VectorMatrix *Matrices;
   class GradientColours *Colours;
   std::string ColourMap;
   FRGB   Colour;
   RGB8   ColourRGB; // A cached conversion of the FRGB value
   STRING ID;
   int   NumericID;
   double Angle;
   double Length;
   bool   CalcAngle; // True if the Angle/Length values require recalculation.
};

class extVectorImage : public objVectorImage, public SceneDef {
   public:
   using create = pf::Create<extVectorImage>;
};

class extVectorPattern : public objVectorPattern, public SceneDef {
   public:
   using create = pf::Create<extVectorPattern>;

   struct VectorMatrix *Matrices;
   objBitmap *Bitmap;
};

class extVectorFilter : public objVectorFilter {
   public:
   using create = pf::Create<extVectorFilter>;

   extVector *ClientVector;            // Client vector or viewport supplied by Scene.acDraw()
   extVectorViewport *ClientViewport;  // The nearest viewport containing the vector.
   extVectorScene *SourceScene;        // Internal scene for rendering SourceGraphic
   extVectorScene *Scene;              // Scene that the filter belongs to.
   objBitmap *SourceGraphic;           // An internal rendering of the vector client, used for SourceGraphic and SourceAlpha.
   objBitmap *BkgdBitmap;              // Target bitmap supplied by Scene.acDraw()
   extFilterEffect *ActiveEffect;      // Current effect being processed by the pipeline.
   extFilterEffect *Effects;           // Pointer to the first effect in the chain.
   extFilterEffect *LastEffect;
   std::vector<std::unique_ptr<filter_bitmap>> Bank;
   TClipRectangle<int> VectorClip;           // Clipping region of the vector client (reflects the vector bounds)
   uint8_t BankIndex;
   double BoundWidth, BoundHeight; // Filter boundary, computed on acDraw()
   double TargetX, TargetY, TargetWidth, TargetHeight; // Target boundary, computed on acDraw()
   bool Rendered;
   bool Disabled;
   bool ReqBkgd; // True if the filter requires a background bitmap for one or more effects.
};

class extFilterEffect : public objFilterEffect {
   public:
   using create = pf::Create<extFilterEffect>;

   extVectorFilter *Filter; // Direct reference to the parent filter
   uint16_t UsageCount;        // Total number of other effects utilising this effect to build a pipeline
};

class extPainter : public VectorPainter {
public:
   GRADIENT_TABLE *GradientTable;
   double GradientAlpha;
};

class extVector : public objVector {
   public:
   using create = pf::Create<extVector>;

   extPainter Fill[2], Stroke;
   double FinalX, FinalY;         // Used by Viewport to define the target X,Y; also VectorText to position the text' final position.
   TClipRectangle<double> Bounds; // Must be calculated by GeneratePath() and called from calc_full_boundary()
   double StrokeWidth;
   agg::path_storage BasePath;
   agg::trans_affine Transform;   // Final transform.  Accumulated from the Matrix list during path generation.
   CSTRING FilterString, StrokeString, FillString;
   STRING SID;
   void   (*GeneratePath)(extVector *, agg::path_storage &);
   agg::rasterizer_scanline_aa<>     *StrokeRaster;
   agg::rasterizer_scanline_aa<>     *FillRaster;
   std::vector<FeedbackSubscription> *FeedbackSubscriptions;
   std::vector<InputSubscription>    *InputSubscriptions;
   std::vector<KeyboardSubscription> *KeyboardSubscriptions;
   extVectorFilter     *Filter;
   extVectorViewport   *ParentView;
   extVectorClip       *ClipMask;
   extVectorTransition *Transition;
   extVector           *Morph;
   extVector           *AppendPath;
   DashedStroke        *DashArray;
   JTYPE InputMask;
   int   NumericID;
   int   PathLength;
   VMF   MorphFlags;
   VFR   FillRule;
   VFR   ClipRule;
   RC    Dirty;
   uint16_t  TabOrder;
   uint16_t  Isolated:1;
   uint16_t  DisableFillColour:1;  // Bitmap fonts set this to true in order to disable colour fills
   uint16_t  ButtonLock:1;
   uint16_t  ScaledStrokeWidth:1;
   uint16_t  DisableHitTesting:1;
   uint16_t  ResizeSubscription:1;
   uint16_t  FGFill:1;
   uint16_t  Stroked:1;
   uint16_t  ValidState:1;         // Can be set to false during path generation if the shape is invalid
   uint16_t  RequiresRedraw:1;
   agg::line_join_e  LineJoin;
   agg::line_cap_e   LineCap;
   agg::inner_join_e InnerJoin;

   // Methods

   double fixed_stroke_width();

   inline bool dirty() { return (Dirty & RC::DIRTY) != RC::NIL; }

   inline bool is_stroked() {
      return (StrokeWidth > 0) and
         ((Stroke.Pattern) or (Stroke.Gradient) or (Stroke.Image) or
          (Stroke.Colour.Alpha * StrokeOpacity * Opacity > 0.001));
   }
};

struct TabOrderedVector {
   bool operator()(const extVector *a, const extVector *b) const;
};

inline bool TabOrderedVector::operator()(const extVector *a, const extVector *b) const {
   if (a->TabOrder == b->TabOrder) return a->UID < b->UID;
   else return a->TabOrder < b->TabOrder;
}

class extVectorScene : public objVectorScene {
   public:
   using create = pf::Create<extVectorScene>;

   double ActiveVectorX, ActiveVectorY; // X,Y location of the active vector.
   agg::rendering_buffer *Buffer; // AGG representation of the target bitmap
   APTR KeyHandle; // Keyboard subscription
   ankerl::unordered_dense::map<std::string, OBJECTPTR> Defs;
   std::unordered_set<extVectorViewport *> PendingResizeMsgs;
   ankerl::unordered_dense::map<extVector *, JTYPE> InputSubscriptions;
   std::set<extVector *, TabOrderedVector> KeyboardSubscriptions;
   std::vector<class InputBoundary> InputBoundaries; // Defined on the fly each time that the scene is rendered.  Used to manage input events and cursor changes.
   ankerl::unordered_dense::map<extVectorViewport *, ankerl::unordered_dense::map<extVector *, FUNCTION>> ResizeSubscriptions;
   OBJECTID ButtonLock; // The vector currently holding a button lock
   OBJECTID ActiveVector; // The most recent vector to have received an input movement event.
   int InputHandle;
   PTC Cursor; // Current cursor image
   bool RefreshCursor;
   bool ShareModified; // True if a shareable object has been modified (e.g. VectorGradient), requiring a redraw of any vectors that use it.
   uint8_t BufferCount; // Active tally of viewports that are buffered.
};

//********************************************************************************************************************
// NB: Considered a shape (can be transformed).

class extVectorViewport : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORVIEWPORT;
   static constexpr CSTRING CLASS_NAME = "VectorViewport";
   using create = pf::Create<extVectorViewport>;

   FUNCTION vpDragCallback;
   double vpViewX, vpViewY, vpViewWidth, vpViewHeight;     // Viewbox values determine the area of the SVG content that is being sourced.  These values are always fixed pixel units.
   double vpTargetX, vpTargetY, vpTargetXO, vpTargetYO, vpTargetWidth, vpTargetHeight; // Target dimensions
   double vpXScale, vpYScale; // Internal scaling for ViewN -to-> TargetN; takes the AspectRatio into consideration.
   double vpFixedWidth, vpFixedHeight; // Fixed pixel position values, relative to parent viewport
   TClipRectangle<double> vpBounds; // Bounding box coordinates relative to (0,0), used for clipping
   double vpAlignX, vpAlignY;
   objBitmap *vpBuffer;
   uint8_t *vpBufferData;
   int vpBufferSize; // Size of the vpBufferData in bytes
   bool  vpClip; // Viewport requires non-rectangular clipping, e.g. because it is rotated or sheared.
   DMF   vpDimensions;
   ARF   vpAspectRatio;
   VOF   vpOverflowX, vpOverflowY;
   uint8_t vpDragging:1;
   uint8_t vpBuffered:1; // True if the client requested that the viewport is buffered.
   uint8_t vpRefreshBuffer:1;
};

//********************************************************************************************************************

class extVectorPoly : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPOLYGON;
   static constexpr CSTRING CLASS_NAME = "VectorPolygon";
   using create = pf::Create<extVectorPoly>;

   std::vector<VectorPoint> Points;
   bool Closed:1;      // Polygons are closed (TRUE) and Polylines are open (FALSE)
};

class extVectorPath : public extVector, public SceneDef {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPATH;
   static constexpr CSTRING CLASS_NAME = "VectorPath";
   using create = pf::Create<extVectorPath>;

   std::vector<PathCommand> Commands;
};

class extVectorRectangle : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORRECTANGLE;
   static constexpr CSTRING CLASS_NAME = "VectorRectangle";
   using create = pf::Create<extVectorRectangle>;

   struct coord { double x, y; };
   double rX, rY, rWidth, rHeight, rXOffset, rYOffset;
   std::array<coord, 4> rRound;
   DMF    rDimensions;
   bool   rFullControl;
};

//********************************************************************************************************************

class GradientColours {
   public:
      GradientColours(const std::vector<GradientStop> &, VCS, double, double);
      GradientColours(const std::array<FRGB, 256> &, double);
      GRADIENT_TABLE table;
      double resolution;

      void apply_resolution(double Resolution) {
         resolution = 1.0 - Resolution;

         // For a given block of colours, compute the average colour and apply it to the entire block.

         int block_size = int(resolution * table.size());
         for (int i = 0; i < table.size(); i += block_size) {

            int red = 0, green = 0, blue = 0, alpha = 0, total = 0;
            for (int b=i; (b < i + block_size) and (b < table.size()); b++, total++) {
               red   += table[b].r * table[b].r;
               green += table[b].g * table[b].g;
               blue  += table[b].b * table[b].b;
               alpha += table[b].a * table[b].a;
            }

            auto col = agg::rgba8{
               uint8_t(sqrt(red/total)), uint8_t(sqrt(green/total)), uint8_t(sqrt(blue/total)), uint8_t(sqrt(alpha/total))
            };

            for (int b = i; (b < i + block_size) and (b < table.size()); b++) table[b] = col;
         }
      }
};

//********************************************************************************************************************

class extVectorClip : public objVectorClip, public SceneDef {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORCLIP;
   static constexpr CSTRING CLASS_NAME = "VectorClip";
   using create = pf::Create<extVectorClip>;

   TClipRectangle<double> Bounds;
   OBJECTID ViewportID;
};

//********************************************************************************************************************

template <class T> void next_value(T &Value)
{
   while ((*Value) and ((*Value <= 0x20) or (*Value IS ',') or (*Value IS '(') or (*Value IS ')'))) Value++;
}

//********************************************************************************************************************

extern CSTRING get_name(OBJECTPTR);
extern ERR read_numseq(CSTRING &, std::initializer_list<double *>);
extern double read_unit(CSTRING &, bool &);
extern ERR init_blurfx(void);
extern ERR init_colour(void);
extern ERR init_colourfx(void);
extern ERR init_compositefx(void);
extern ERR init_convolvefx(void);
extern ERR init_displacementfx(void);
extern ERR init_filter(void);
extern ERR init_filtereffect(void);
extern ERR init_floodfx(void);
extern ERR init_gradient(void);
extern ERR init_image(void);
extern ERR init_imagefx(void);
extern ERR init_lightingfx(void);
extern ERR init_mergefx(void);
extern ERR init_morphfx(void);
extern ERR init_offsetfx(void);
extern ERR init_pattern(void);
extern ERR init_remapfx(void);
extern ERR init_sourcefx(void);
extern ERR init_transition(void);
extern ERR init_turbulencefx(void);
extern ERR init_vectorscene(void);
extern ERR init_wavefunctionfx(void);

extern void apply_parent_transforms(extVector *, agg::trans_affine &);
extern void apply_transition(extVectorTransition *, double, agg::trans_affine &);
extern void apply_transition_xy(extVectorTransition *, double, double *, double *);
extern void calc_aspectratio(CSTRING, ARF, double, double, double, double, double &X, double &Y, double &, double &);
extern void calc_full_boundary(extVector *, TClipRectangle<double> &, bool IncludeSiblings = true, bool IncludeTransforms = true, bool IncludeStrokes = false);
extern void convert_to_aggpath(extVectorPath *, std::vector<PathCommand> &, agg::path_storage &);
extern void debug_tree(extVector *, int &);
extern void gen_vector_path(extVector *);
extern void gen_vector_tree(extVector *);
extern GRADIENT_TABLE * get_fill_gradient_table(extPainter &, double);
extern GRADIENT_TABLE * get_stroke_gradient_table(extVector &);
extern objBitmap * get_source_graphic(extVectorFilter *);
extern ERR read_path(std::vector<PathCommand> &, CSTRING);
extern ERR render_filter(extVectorFilter *, extVectorViewport *, extVector *, objBitmap *, objBitmap **);
extern ERR scene_input_events(const InputEvent *, int);
extern void send_feedback(extVector *, FM, OBJECTPTR = nullptr);
extern void set_filter(agg::image_filter_lut &, VSM, agg::trans_affine &, double Kernel = 0);

extern void render_scene_from_viewport(extVectorScene *, objBitmap *, objVectorViewport *);

extern const FieldDef clAspectRatio[];
extern std::recursive_mutex glVectorFocusLock;
extern std::vector<extVector *> glVectorFocusList; // The first reference is the most foreground object with the focus

//********************************************************************************************************************
// Generic function for setting the clip region of an AGG rasterizer

template<class T = int> void set_raster_rect_path(agg::rasterizer_scanline_aa<> &Raster, T X, T Y, T Width, T Height)
{
   if (Width < 0) Width = 0;
   if (Height < 0) Height = 0;

   agg::path_storage clip;
   clip.move_to(X, Y);
   clip.line_to(X+Width, Y);
   clip.line_to(X+Width, Y+Height);
   clip.line_to(X, Y+Height);
   clip.close_polygon();

   Raster.reset();
   Raster.add_path(clip);
}

//********************************************************************************************************************

template<class VertexSource, class T = double>
TClipRectangle<T> get_bounds(VertexSource &vs, const unsigned path_id = 0)
{
   TClipRectangle<T> rect;
   bounding_rect_single(vs, path_id, &rect.left, &rect.top, &rect.right, &rect.bottom);
   return rect;
}

//********************************************************************************************************************
// If a scene contains buffered viewports, they must be marked for refresh when the state of one or more of their
// children is changed.

static void mark_buffers_for_refresh(extVector *Vector)
{
   if ((Vector->Scene) and (!((extVectorScene *)Vector->Scene)->BufferCount)) return;

   extVectorViewport *parent_view;

   if (Vector->classID() IS CLASSID::VECTORVIEWPORT) parent_view = (extVectorViewport *)Vector;
   else parent_view = ((extVector *)Vector)->ParentView;

   while (parent_view) {
      if (parent_view->vpBuffered) {
         parent_view->vpRefreshBuffer = true;
         break;
      }

      parent_view = parent_view->ParentView;
   }
}

//********************************************************************************************************************
// Mark a vector and all its children as needing some form of recomputation.

inline static void mark_children(extVector *Vector, const RC Flags)
{
   ((extVector *)Vector)->Dirty |= Flags;
   for (auto node=(extVector *)Vector->Child; node; node=(extVector *)node->Next) {
      if ((node->Dirty & Flags) != Flags) mark_children(node, Flags);
   }
}

inline static void mark_dirty(objVector *Vector, RC Flags)
{
   mark_buffers_for_refresh((extVector *)Vector);

   mark_children((extVector *)Vector, Flags);
}

//********************************************************************************************************************

// Accepts agg::path_storage or agg::rasterizer_scanline_aa as the first argument.

template <class T> void basic_path(T &Target, double X1, double Y1, double X2, double Y2)
{
   Target.move_to(X1, Y1);
   Target.line_to(X2, Y1);
   Target.line_to(X2, Y2);
   Target.line_to(X1, Y2);
   Target.close_polygon();
}

//********************************************************************************************************************
// Call reset_path() when the shape of the vector requires recalculation.

inline static void reset_path(objVector *Vector)
{
   ((extVector *)Vector)->Dirty |= RC::BASE_PATH;
   mark_dirty(Vector, RC::FINAL_PATH);
}

//********************************************************************************************************************
// Call reset_final_path() when the base path is still valid and the vector is affected by a transform or coordinate
// translation.

inline static void reset_final_path(objVector *Vector)
{
   mark_dirty(Vector, RC::FINAL_PATH);
}

//********************************************************************************************************************

template <class T>
inline static void apply_transforms(const T &Vector, agg::trans_affine &AGGTransform)
{
   for (auto t=Vector.Matrices; t; t=t->Next) {
      AGGTransform.multiply(t->ScaleX, t->ShearY, t->ShearX, t->ScaleY, t->TranslateX, t->TranslateY);
   }
}

//********************************************************************************************************************

#include "pixfmt.h"

namespace agg {

// This template function is a customised caller into AGG's drawing process.
//
// U: Must be a scanline class or derivative; e.g. agg::scanline_u8
// RenderBase: The target bitmap.  Use the clip_box() method to limit the drawing region.
// Raster: Chooses the algorithm used to rasterise the vector path (affects AA, outlining etc).  Also configures the
//   filling rule, gamma and related drawing options.

template <class T, class U> static void drawBitmapRender(U &Input,
   agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::rasterizer_scanline_aa<> &Raster,
   T &spangen, double Opacity = 1.0)
{
   class spanconv_image {
      public:
         spanconv_image(double Alpha) : alpha(Alpha) { }

         void prepare() { }

         void generate(agg::rgba8 *span, int x, int y, unsigned len) const {
            do {
               span->a = span->a * alpha;
               ++span;
            } while(--len);
         }

      private:
         double alpha;
   };

   agg::span_allocator<agg::rgba8> spanalloc;

   // Refer to pixfmt_psl::blend_color_hspan() if you're looking for the code that does the actual drawing.

   if (Opacity < 1.0) {
      spanconv_image sci(Opacity);
      agg::span_converter<T, spanconv_image> sc(spangen, sci);
      agg::render_scanlines_aa(Raster, Input, RenderBase, spanalloc, sc);
   }
   else agg::render_scanlines_aa(Raster, Input, RenderBase, spanalloc, spangen);
};

//********************************************************************************************************************

template <class T> static void renderSolidBitmap(agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::rasterizer_scanline_aa<> &Raster, T &spangen, double Opacity = 1.0)
{
   class spanconv_image { // Only used if Opacity < 1.0
      public:
         spanconv_image(double Alpha) : alpha(Alpha) { }

         void prepare() { }

         void generate(agg::rgba8 *span, int x, int y, unsigned len) const {
            do {
               span->a = span->a * alpha;
               ++span;
            } while(--len);
         }

      private:
         double alpha;
   };

   agg::span_allocator<agg::rgba8> spanalloc;
   agg::scanline_u8 scanline;

   if (Opacity < 1.0) {
      spanconv_image sci(Opacity);
      agg::span_converter<T, spanconv_image> sc(spangen, sci);
      agg::render_scanlines_aa_noblend(Raster, scanline, RenderBase, spanalloc, sc);
   }
   else agg::render_scanlines_aa_noblend(Raster, scanline, RenderBase, spanalloc, spangen);
};

//********************************************************************************************************************
// This class is used for clipped images (no tiling)

template<class Source> class span_once // Based on span_pattern_rgba
{
private:
   span_once() {}
public:
   typedef typename agg::rgba8::value_type value_type;
   typedef agg::rgba8 color_type;

   span_once(Source & src, unsigned offset_x, unsigned offset_y) :
       m_src(&src), m_offset_x(offset_x), m_offset_y(offset_y)
   {
      m_bk_buf[0] = m_bk_buf[1] = m_bk_buf[2] = m_bk_buf[3] = 0;
   }

   void prepare() {}

   void generate(agg::rgba8 *s, int x, int y, unsigned len) {
      x += m_offset_x;
      y += m_offset_y;
      const value_type* p = (const value_type*)span(x, y, len);
      do {
         s->r = p[m_src->mPixelOrder.Red];
         s->g = p[m_src->mPixelOrder.Green];
         s->b = p[m_src->mPixelOrder.Blue];
         s->a = p[m_src->mPixelOrder.Alpha];
         p = (const value_type*)next_x();
         ++s;
      } while(--len);
   }

   int8u * span(int x, int y, unsigned len) {
      m_x = m_x0 = x;
      m_y = y;
      if ((y >= 0) and (y < m_src->mHeight) and (x >= 0) and (x+(int)len <= m_src->mWidth)) {
         return m_pix_ptr = m_src->row_ptr(y) + (x * m_src->mBytesPerPixel);
      }
      m_pix_ptr = 0;
      if ((m_y >= 0) and (m_y < m_src->mHeight) and (m_x >= 0) and (m_x < m_src->mWidth)) {
         return m_src->row_ptr(m_y) + (m_x * m_src->mBytesPerPixel);
      }
      return m_bk_buf;
   }

   int8u * next_x() {
      if (m_pix_ptr) return m_pix_ptr += m_src->mBytesPerPixel;
      ++m_x;
      if ((m_y >= 0) and (m_y < m_src->mHeight) and (m_x >= 0) and (m_x < m_src->mWidth)) {
         return m_src->row_ptr(m_y) + (m_x * m_src->mBytesPerPixel);
      }
      return m_bk_buf;
  }

   int8u * next_y() {
      ++m_y;
      m_x = m_x0;
      if (m_pix_ptr and m_y >= 0 and m_y < int(m_src->height())) {
         return m_pix_ptr = m_src->row_ptr(m_y) + (m_x * m_src->mBytesPerPixel);
      }
      m_pix_ptr = 0;
      if ((m_y >= 0) and (m_y < m_src->mHeight) and (m_x >= 0) and (m_x < m_src->mWidth)) {
         return m_src->row_ptr(m_y) + (m_x * m_src->mBytesPerPixel);
      }
      return m_bk_buf;
   }

   Source *m_src;

private:
   unsigned m_offset_x;
   unsigned m_offset_y;
   uint8_t m_bk_buf[4];
   int m_x, m_x0, m_y;
   uint8_t *m_pix_ptr;
};

} // namespace

class SimpleVector {
public:
   agg::path_storage mPath;
   agg::renderer_base<agg::pixfmt_psl> mRenderer;
   agg::rasterizer_scanline_aa<> mRaster; // For rendering the scene.  Stores a copy of the path, and other values.

   SimpleVector() { }

   // Refer to scene_draw.cpp for DrawPath()
   void DrawPath(objBitmap *, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle);
};

//********************************************************************************************************************
// Retrieve the width/height of a vector's nearest viewport or scene object, taking account of scaled dimensions
// and offsets.
//
// These functions expect to be called during path generation via gen_vector_path().  If this is not the case, ensure
// that Dirty field markers are cleared beforehand.

inline static double get_parent_width(const objVector *Vector)
{
   auto eVector = (const extVector *)Vector;
   if (auto view = (extVectorViewport *)eVector->ParentView) {
      if (view->vpViewWidth > 0) return view->vpViewWidth;
      else if ((dmf::hasAnyWidth(view->vpDimensions)) or
          ((dmf::hasAnyX(view->vpDimensions)) and (dmf::hasAnyXOffset(view->vpDimensions)))) {
         return view->vpFixedWidth;
      }
      else return eVector->Scene->PageWidth;
   }
   else if (eVector->Scene) return eVector->Scene->PageWidth;
   else return 0;
}

inline static double get_parent_height(const objVector *Vector)
{
   auto eVector = (const extVector *)Vector;
   if (auto view = (extVectorViewport *)eVector->ParentView) {
      if (view->vpViewHeight > 0) return view->vpViewHeight;
      else if ((dmf::hasAnyHeight(view->vpDimensions)) or
          ((dmf::hasAnyY(view->vpDimensions)) and (dmf::hasAnyYOffset(view->vpDimensions)))) {
         return view->vpFixedHeight;
      }
      else return eVector->Scene->PageHeight;
   }
   else if (eVector->Scene) return eVector->Scene->PageHeight;
   else return 0;
}

template <class T> inline static double get_parent_diagonal(T *Vector)
{
   double a = std::abs(get_parent_width(Vector));
   double b = std::abs(get_parent_height(Vector));
   if (a > b) std::swap(a, b);
   return b + 0.428 * a * a / b; // Error level of ~1.04%
   //return std::sqrt((a * a) + (b * b)); // Full accuracy
}

inline static double dist(double X1, double Y1, double X2, double Y2)
{
   double a = std::abs(X2 - X1);
   double b = std::abs(Y2 - Y1);
   if (a > b) std::swap(a, b);
   return b + 0.428 * a * a / b; // Error level of ~1.04%
   //return std::sqrt((a * a) + (b * b)); // Full accuracy
}

// This SVG formula returns the multiplier that is used for computing relative length values within a viewport.
// Typically needed when computing things like radius values.

inline static double svg_diag(double Width, double Height)
{
   return sqrt((Width * Width) + (Height * Height)) / SQRT2;
}

//********************************************************************************************************************

enum { WS_NO_WORD=0, WS_NEW_WORD, WS_IN_WORD };

//********************************************************************************************************************

static constexpr double int26p6_to_dbl(int p) { return double(p) * (1.0 / 64.0); }
static constexpr int dbl_to_int26p6(double p) { return int(p * 64.0); }

//********************************************************************************************************************

inline double fast_inv_sqrt(double x) {
    union {
        double d;
        uint64_t i;
    } conv = {.d = x};

    conv.i = 0x5fe6eb50c7b537a9ULL - (conv.i >> 1);  // Magic number for double
    conv.d *= 1.5 - (x * 0.5 * conv.d * conv.d);    // One Newton-Raphson iteration
    conv.d *= 1.5 - (x * 0.5 * conv.d * conv.d);    // Second iteration for better accuracy
    return conv.d;
}

//********************************************************************************************************************

inline static void save_bitmap(objBitmap *Bitmap, const std::string Name)
{
   std::string path = "temp:bmp_" + Name + ".png";

   auto pic = objPicture::create {
      fl::Width(Bitmap->Clip.Right - Bitmap->Clip.Left),
      fl::Height(Bitmap->Clip.Bottom - Bitmap->Clip.Top),
      fl::BitsPerPixel(32),
      fl::Flags(PCF::FORCE_ALPHA_32|PCF::NEW),
      fl::Path(path),
      fl::ColourSpace(Bitmap->ColourSpace) };

   if (pic.ok()) {
      gfx::CopyArea(Bitmap, pic->Bitmap, BAF::NIL, Bitmap->Clip.Left, Bitmap->Clip.Top, pic->Bitmap->Width, pic->Bitmap->Height, 0, 0);
      pic->saveImage(nullptr);
   }
}

// Raw-copy version of save_bitmap()

inline static void save_bitmap(std::string Name, uint8_t *Data, int Width, int Height, int BPP = 32)
{
   std::string path = "temp:raw_" + Name + ".png";

   auto pic = objPicture::create {
      fl::Width(Width),
      fl::Height(Height),
      fl::BitsPerPixel(BPP),
      fl::Flags((BPP IS 32 ? PCF::FORCE_ALPHA_32 : PCF::NIL)|PCF::NEW),
      fl::Path(path)
   };

   if (pic.ok()) {
      auto &bmp = pic->Bitmap;
      if (BPP IS 8) {
         for (uint32_t i=0; i < bmp->Palette->AmtColours; i++) {
            bmp->Palette->Col[i] = { uint8_t(i), uint8_t(i), uint8_t(i), 255 };
         }
      }

      const int byte_width = Width * bmp->BytesPerPixel;
      uint8_t *out = bmp->Data;
      for (int y=0; y < Height; y++) {
         copymem(Data, out, byte_width);
         out  += bmp->LineWidth;
         Data += Width * bmp->BytesPerPixel;
      }
      pic->saveImage(nullptr);
   }
}

//********************************************************************************************************************
// Find the first parent of the targeted vector.  Returns nullptr if no valid parent is found.

inline static extVector * get_parent(const extVector *Vector)
{
   if (Vector->Class->BaseClassID != CLASSID::VECTOR) return nullptr;
   while (Vector) {
      if (!Vector->Parent) Vector = (extVector *)Vector->Prev; // Scan back to the first sibling to find the parent
      else if (Vector->Parent->Class->BaseClassID IS CLASSID::VECTOR) return (extVector *)(Vector->Parent);
      else return nullptr;
   }

   return nullptr;
}

//********************************************************************************************************************
// Test if a point is within a rectangle (four points, must be convex)
// This routine assumes clockwise points; for counter-clockwise you'd use < 0.

inline bool point_in_rectangle(const agg::vertex_d &X, const agg::vertex_d &Y, const agg::vertex_d &Z, const agg::vertex_d &W, const agg::vertex_d &P)
{
   auto is_left = [](agg::vertex_d A, agg::vertex_d B, agg::vertex_d C) -> double {
      return ((B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y));
   };

   return (is_left(X, Y, P) > 0) and (is_left(Y, Z, P) > 0) and (is_left(Z, W, P) > 0) and (is_left(W, X, P) > 0);
}

//********************************************************************************************************************

inline double fastPow(double a, double b) {
   union {
     double d;
     int x[2];
   } u = { a };
   u.x[1] = (int)(b * (u.x[1] - 1072632447) + 1072632447);
   u.x[0] = 0;
   return u.d;
}

//********************************************************************************************************************

inline int isPow2(uint32_t x)
{
   return ((x != 0) and !(x & (x - 1)));
}

//********************************************************************************************************************

inline void reset_matrix(VectorMatrix &Matrix)
{
   Matrix.ScaleX = 1.0;
   Matrix.ScaleY = 1.0;
   Matrix.ShearX = 0;
   Matrix.ShearY = 0;
   Matrix.TranslateX = 0;
   Matrix.TranslateY = 0;
}

//********************************************************************************************************************

template <class T>
void configure_stroke(extVector &Vector, T &Stroke)
{
   Stroke.width(Vector.fixed_stroke_width());

   if (Vector.LineJoin)  Stroke.line_join(Vector.LineJoin); //miter, round, bevel
   if (Vector.LineCap)   Stroke.line_cap(Vector.LineCap); // butt, square, round
   if (Vector.InnerJoin) Stroke.inner_join(Vector.InnerJoin); // miter, round, bevel, jag

   // It has been noted that there may be issues between miter_join, miter_join_revert and line-caps that
   // need further investigation.  This section experiments with adjusting the line-cap according to the selected
   // line-join.

   /*
   if (Vector.LineJoin) {
      if (Vector.classID() IS CLASSID::VECTORPOLYGON) {
         if (((extVectorPoly &)Vector).Closed) {
            switch(Vector.LineJoin) {
               case agg::miter_join:        Stroke.line_cap(agg::square_cap); break;
               case agg::bevel_join:        Stroke.line_cap(agg::square_cap); break;
               case agg::miter_join_revert: Stroke.line_cap(agg::square_cap); break;
               case agg::round_join:        Stroke.line_cap(agg::round_cap); break;
               case agg::miter_join_round:  Stroke.line_cap(agg::round_cap); break;
               case agg::inherit_join:      break;
            }
         }
      }
   }
   */
   if (Vector.MiterLimit > 0) Stroke.miter_limit(Vector.MiterLimit);
   if (Vector.InnerMiterLimit > 0) Stroke.inner_miter_limit(Vector.InnerMiterLimit);
}

//********************************************************************************************************************

static int get_utf8(const std::string_view &Value, uint32_t &Unicode, std::size_t Index = 0)
{
   int len, code;

   if ((Value[Index] & 0x80) != 0x80) {
      Unicode = Value[Index];
      return 1;
   }
   else if ((Value[Index] & 0xe0) IS 0xc0) {
      len  = 2;
      code = Value[Index] & 0x1f;
   }
   else if ((Value[Index] & 0xf0) IS 0xe0) {
      len  = 3;
      code = Value[Index] & 0x0f;
   }
   else if ((Value[Index] & 0xf8) IS 0xf0) {
      len  = 4;
      code = Value[Index] & 0x07;
   }
   else if ((Value[Index] & 0xfc) IS 0xf8) {
      len  = 5;
      code = Value[Index] & 0x03;
   }
   else if ((Value[Index] & 0xfc) IS 0xfc) {
      len  = 6;
      code = Value[Index] & 0x01;
   }
   else { // Unprintable character
      Unicode = 0;
      return 1;
   }

   for (int i=1; i < len; ++i) {
      if ((Value[i] & 0xc0) != 0x80) code = -1;
      code <<= 6;
      code |= Value[i] & 0x3f;
   }

   if (code IS -1) {
      Unicode = 0;
      return 1;
   }
   else {
      Unicode = code;
      return len;
   }
}

//********************************************************************************************************************

extern agg::gamma_lut<uint8_t, uint16_t, 8, 12> glGamma;
extern double glDisplayVDPI, glDisplayHDPI, glDisplayDPI;

extern void set_text_final_xy(extVectorText *);

namespace vec {
extern ERR DrawPath(objBitmap * Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle);
extern ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path);
extern ERR GeneratePath(CSTRING Sequence, APTR *Path);
extern ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path);
extern ERR ReadPainter(objVectorScene * Scene, CSTRING IRI, struct VectorPainter * Painter, CSTRING * Result);
extern void TranslatePath(APTR Path, double X, double Y);
extern void MoveTo(APTR Path, double X, double Y);
extern void LineTo(APTR Path, double X, double Y);
extern void ArcTo(APTR Path, double RX, double RY, double Angle, double X, double Y, ARC Flags);
extern void Curve3(APTR Path, double CtrlX, double CtrlY, double X, double Y);
extern void Smooth3(APTR Path, double X, double Y);
extern void Curve4(APTR Path, double CtrlX1, double CtrlY1, double CtrlX2, double CtrlY2, double X, double Y);
extern void Smooth4(APTR Path, double CtrlX, double CtrlY, double X, double Y);
extern void ClosePath(APTR Path);
extern void RewindPath(APTR Path);
extern int GetVertex(APTR Path, double * X, double * Y);
extern ERR ApplyPath(APTR Path, objVectorPath * VectorPath);
extern ERR Rotate(struct VectorMatrix * Matrix, double Angle, double CenterX, double CenterY);
extern ERR Translate(struct VectorMatrix * Matrix, double X, double Y);
extern ERR Skew(struct VectorMatrix * Matrix, double X, double Y);
extern ERR Multiply(struct VectorMatrix * Matrix, double ScaleX, double ShearY, double ShearX, double ScaleY, double TranslateX, double TranslateY);
extern ERR MultiplyMatrix(struct VectorMatrix * Target, struct VectorMatrix * Source);
extern ERR Scale(struct VectorMatrix * Matrix, double X, double Y);
extern ERR ParseTransform(struct VectorMatrix * Matrix, CSTRING Transform);
extern ERR ResetMatrix(struct VectorMatrix * Matrix);
extern ERR GetFontHandle(CSTRING Family, CSTRING Style, int Weight, int Size, APTR *Handle);
extern ERR GetFontMetrics(APTR Handle, struct FontMetrics * Info);
extern double CharWidth(APTR FontHandle, uint32_t Char, uint32_t KChar, double * Kerning);
extern double StringWidth(APTR FontHandle, CSTRING String, int Chars);
extern ERR FlushMatrix(struct VectorMatrix * Matrix);
extern ERR TracePath(APTR Path, FUNCTION *Callback, double Scale);
}

template <class T> TClipRectangle<T>::TClipRectangle(const extVector *pVector) {
   *this = pVector->Bounds;
}

template <class T> TClipRectangle<T>::TClipRectangle(const class extVectorViewport *pVector) {
   *this = pVector->vpBounds;
}

inline void SceneDef::modified() {
   if (HostScene) HostScene->ShareModified = true;
}
