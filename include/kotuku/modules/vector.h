#pragma once

// Name:      vector.h
// Copyright: Paul Manias © 2010-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_VECTOR (1)

#include <kotuku/modules/display.h>
#include <kotuku/modules/image.h>

class objVectorColour;
class objVectorTransition;
class objVectorScene;
class objVectorImage;
class objVectorPattern;
class objGradient;
class objGradientLinear;
class objGradientRadial;
class objGradientConic;
class objGradientDiamond;
class objGradientContour;
class objGradientGouraud;
class objGradientDistal;
class objGradientVoronoi;
class objFilterEffect;
class objImageFX;
class objSourceFX;
class objBlurFX;
class objColourFX;
class objCompositeFX;
class objConvolveFX;
class objDisplacementFX;
class objFloodFX;
class objLightingFX;
class objMergeFX;
class objMorphologyFX;
class objOffsetFX;
class objRemapFX;
class objTurbulenceFX;
class objWaveFunctionFX;
class objVectorClip;
class objVectorFilter;
class objVector;
class objVectorPath;
class objVectorText;
class objVectorGroup;
class objVectorWave;
class objVectorRectangle;
class objVectorPolygon;
class objVectorShape;
class objVectorSpiral;
class objVectorEllipse;
class objVectorViewport;

// Options for drawing arcs.

enum class ARC : uint32_t {
   NIL = 0,
   LARGE = 0x00000001,
   SWEEP = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(ARC)

// Options for VectorClip.

enum class VCLF : uint32_t {
   NIL = 0,
   APPLY_FILLS = 0x00000001,
   APPLY_STROKES = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(VCLF)

// Optional flags and indicators for the Vector class.

enum class VF : uint32_t {
   NIL = 0,
   DISABLED = 0x00000001,
   HAS_FOCUS = 0x00000002,
   JOIN_PATHS = 0x00000004,
   ISOLATED = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(VF)

// Define the aspect ratio for VectorFilter unit scaling.

enum class VFA : int {
   NIL = 0,
   MEET = 0,
   NONE = 1,
};

// Light source identifiers.

enum class LS : int {
   NIL = 0,
   DISTANT = 0,
   SPOT = 1,
   POINT = 2,
};

// Lighting algorithm for the LightingFX class.

enum class LT : int {
   NIL = 0,
   DIFFUSE = 0,
   SPECULAR = 1,
};

enum class VUNIT : int {
   NIL = 0,
   UNDEFINED = 0,
   BOUNDING_BOX = 1,
   USERSPACE = 2,
   END = 3,
};

// Spread method options define the method to use for tiling filled graphics.

enum class VSPREAD : int {
   NIL = 0,
   UNDEFINED = 0,
   PAD = 1,
   REFLECT = 2,
   REPEAT = 3,
   REFLECT_X = 4,
   REFLECT_Y = 5,
   CLIP = 6,
   END = 7,
};

enum class EM : int {
   NIL = 0,
   DUPLICATE = 1,
   WRAP = 2,
   NONE = 3,
};

enum class PE : int {
   NIL = 0,
   Move = 1,
   MoveRel = 2,
   Line = 3,
   LineRel = 4,
   HLine = 5,
   HLineRel = 6,
   VLine = 7,
   VLineRel = 8,
   Curve = 9,
   CurveRel = 10,
   Smooth = 11,
   SmoothRel = 12,
   QuadCurve = 13,
   QuadCurveRel = 14,
   QuadSmooth = 15,
   QuadSmoothRel = 16,
   Arc = 17,
   ArcRel = 18,
   ClosePath = 19,
};

// Vector fill rules for the FillRule field in the Vector class.

enum class VFR : int {
   NIL = 0,
   NON_ZERO = 1,
   EVEN_ODD = 2,
   INHERIT = 3,
   END = 4,
};

// Options for the Vector class' Visibility field.

enum class VIS : int {
   NIL = 0,
   HIDDEN = 0,
   VISIBLE = 1,
   COLLAPSE = 2,
   INHERIT = 3,
};

// Viewport overflow options.

enum class VOF : int {
   NIL = 0,
   VISIBLE = 0,
   HIDDEN = 1,
   SCROLL = 2,
   INHERIT = 3,
};

// Component selection for RemapFX methods.

enum class CMP : int {
   NIL = 0,
   ALL = -1,
   RED = 0,
   GREEN = 1,
   BLUE = 2,
   ALPHA = 3,
};

// Options for the look of line joins.

enum class VLJ : int {
   NIL = 0,
   MITER = 0,
   MITER_SMART = 1,
   ROUND = 2,
   BEVEL = 3,
   MITER_ROUND = 4,
   INHERIT = 5,
};

// Line-cap options.

enum class VLC : int {
   NIL = 0,
   BUTT = 1,
   SQUARE = 2,
   ROUND = 3,
   INHERIT = 4,
};

// Inner join options for angled lines.

enum class VIJ : int {
   NIL = 0,
   BEVEL = 1,
   MITER = 2,
   JAG = 3,
   ROUND = 4,
   INHERIT = 5,
};

// Options for stretching text in VectorText.

enum class VTS : int {
   NIL = 0,
   INHERIT = 0,
   NORMAL = 1,
   WIDER = 2,
   NARROWER = 3,
   ULTRA_CONDENSED = 4,
   EXTRA_CONDENSED = 5,
   CONDENSED = 6,
   SEMI_CONDENSED = 7,
   EXPANDED = 8,
   SEMI_EXPANDED = 9,
   ULTRA_EXPANDED = 10,
   EXTRA_EXPANDED = 11,
};

// MorphologyFX options.

enum class MOP : int {
   NIL = 0,
   ERODE = 0,
   DILATE = 1,
};

// Operators for CompositionFX.

enum class OP : int {
   NIL = 0,
   OVER = 0,
   IN = 1,
   OUT = 2,
   ATOP = 3,
   XOR = 4,
   ARITHMETIC = 5,
   SCREEN = 6,
   MULTIPLY = 7,
   LIGHTEN = 8,
   DARKEN = 9,
   INVERT_RGB = 10,
   INVERT = 11,
   CONTRAST = 12,
   DODGE = 13,
   BURN = 14,
   HARD_LIGHT = 15,
   SOFT_LIGHT = 16,
   DIFFERENCE = 17,
   EXCLUSION = 18,
   PLUS = 19,
   MINUS = 20,
   SUBTRACT = 20,
   OVERLAY = 21,
};

// VectorText flags.

enum class VTXF : uint32_t {
   NIL = 0,
   UNDERLINE = 0x00000001,
   OVERLINE = 0x00000002,
   LINE_THROUGH = 0x00000004,
   BLINK = 0x00000008,
   EDITABLE = 0x00000010,
   EDIT = 0x00000010,
   AREA_SELECTED = 0x00000020,
   NO_SYS_KEYS = 0x00000040,
   OVERWRITE = 0x00000080,
   SECRET = 0x00000100,
   RASTER = 0x00000200,
};

DEFINE_ENUM_FLAG_OPERATORS(VTXF)

// Morph flags

enum class VMF : uint32_t {
   NIL = 0,
   STRETCH = 0x00000001,
   AUTO_SPACING = 0x00000002,
   X_MIN = 0x00000004,
   X_MID = 0x00000008,
   X_MAX = 0x00000010,
   Y_MIN = 0x00000020,
   Y_MID = 0x00000040,
   Y_MAX = 0x00000080,
};

DEFINE_ENUM_FLAG_OPERATORS(VMF)

// Colour space options.

enum class VCS : int {
   NIL = 0,
   INHERIT = 0,
   SRGB = 1,
   LINEAR_RGB = 2,
};

// Filter source types - these are used internally

enum class VSF : int {
   NIL = 0,
   IGNORE = 0,
   NONE = 0,
   GRAPHIC = 1,
   ALPHA = 2,
   BKGD = 3,
   BKGD_ALPHA = 4,
   FILL = 5,
   STROKE = 6,
   REFERENCE = 7,
   PREVIOUS = 8,
};

// Wave options.

enum class WVC : int {
   NIL = 0,
   NONE = 1,
   TOP = 2,
   BOTTOM = 3,
};

// Wave style options.

enum class WVS : int {
   NIL = 0,
   CURVED = 1,
   ANGLED = 2,
   SAWTOOTH = 3,
};

// Gradient fall-off options.

enum class GFALL : int {
   NIL = 0,
   OPAQUE = 1,
   LINEAR = 2,
   QUADRATIC = 3,
   CUBIC = 4,
   SMOOTHSTEP = 5,
   CLEAR = 6,
};

// Gradient easing functions.

enum class GEZ : int {
   NIL = 0,
   LINEAR = 0,
   IN = 1,
   OUT = 2,
   IN_OUT = 3,
   CUBIC_IN = 4,
   CUBIC_OUT = 5,
   CUBIC_IN_OUT = 6,
};

// Worley field modes.

enum class WLF : int {
   NIL = 0,
   F1 = 0,
   F2_F1 = 1,
   F2 = 2,
   WEIGHTED = 3,
   PEAKS = 4,
};

// Worley distance metrics.

enum class WLM : int {
   NIL = 0,
   EUCLIDEAN = 0,
   MANHATTAN = 1,
   CHEBYSHEV = 2,
};

// Colour modes for ColourFX.

enum class CM : int {
   NIL = 0,
   NONE = 0,
   MATRIX = 1,
   SATURATE = 2,
   HUE_ROTATE = 3,
   LUMINANCE_ALPHA = 4,
   CONTRAST = 5,
   BRIGHTNESS = 6,
   HUE = 7,
   DESATURATE = 8,
   COLOURISE = 9,
};

// Gradient flags

enum class VGF : uint32_t {
   NIL = 0,
   SCALED_X1 = 0x00000001,
   SCALED_Y1 = 0x00000002,
   SCALED_X2 = 0x00000004,
   SCALED_Y2 = 0x00000008,
   SCALED_CX = 0x00000010,
   SCALED_CY = 0x00000020,
   SCALED_FX = 0x00000040,
   SCALED_FY = 0x00000080,
   SCALED_RADIUS = 0x00000100,
   SCALED_FOCAL_RADIUS = 0x00000200,
   FIXED_X1 = 0x00000400,
   FIXED_Y1 = 0x00000800,
   FIXED_X2 = 0x00001000,
   FIXED_Y2 = 0x00002000,
   FIXED_CX = 0x00004000,
   FIXED_CY = 0x00008000,
   FIXED_FX = 0x00010000,
   FIXED_FY = 0x00020000,
   FIXED_RADIUS = 0x00040000,
   FIXED_FOCAL_RADIUS = 0x00080000,
};

DEFINE_ENUM_FLAG_OPERATORS(VGF)

// Optional flags for the VectorScene object.

enum class VPF : uint32_t {
   NIL = 0,
   BITMAP_SIZED = 0x00000001,
   RENDER_TIME = 0x00000002,
   RESIZE = 0x00000004,
   OUTLINE_VIEWPORTS = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(VPF)

enum class TB : int {
   NIL = 0,
   TURBULENCE = 0,
   NOISE = 1,
};

enum class VSM : int {
   NIL = 0,
   AUTO = 0,
   NEIGHBOUR = 1,
   BILINEAR = 2,
   BICUBIC = 3,
   SPLINE16 = 4,
   KAISER = 5,
   QUADRIC = 6,
   GAUSSIAN = 7,
   BESSEL = 8,
   MITCHELL = 9,
   SINC = 10,
   LANCZOS = 11,
   BLACKMAN = 12,
};

enum class RQ : int {
   NIL = 0,
   AUTO = 0,
   FAST = 1,
   CRISP = 2,
   PRECISE = 3,
   BEST = 4,
};

enum class RC : uint8_t {
   NIL = 0,
   FINAL_PATH = 0x00000001,
   BASE_PATH = 0x00000002,
   TRANSFORM = 0x00000004,
   ALL = 0x00000007,
   DIRTY = 0x00000007,
};

DEFINE_ENUM_FLAG_OPERATORS(RC)

// Aspect ratios control alignment, scaling and clipping.

enum class ARF : uint32_t {
   NIL = 0,
   X_MIN = 0x00000001,
   X_MID = 0x00000002,
   X_MAX = 0x00000004,
   Y_MIN = 0x00000008,
   Y_MID = 0x00000010,
   Y_MAX = 0x00000020,
   MEET = 0x00000040,
   SLICE = 0x00000080,
   NONE = 0x00000100,
};

DEFINE_ENUM_FLAG_OPERATORS(ARF)

// Options for vecGetBoundary().

enum class VBF : uint32_t {
   NIL = 0,
   INCLUSIVE = 0x00000001,
   NO_TRANSFORM = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(VBF)

// Mask for controlling feedback events that are received.

enum class FM : uint32_t {
   NIL = 0,
   PATH_CHANGED = 0x00000001,
   HAS_FOCUS = 0x00000002,
   CHILD_HAS_FOCUS = 0x00000004,
   LOST_FOCUS = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(FM)

struct GradientStop {
   double Offset;    // An offset in the range of 0 - 1.0
   struct FRGB RGB;  // A floating point RGB value.
};

struct GouraudVertex {
   double X;              // The X coordinate of the vertex.
   double Y;              // The Y coordinate of the vertex.
   struct FRGB Colour;    // The floating point RGB colour assigned to this vertex.
};

struct Transition {
   double  Offset;       // An offset from 0.0 to 1.0 at which to apply the transform.
   CSTRING Transform;    // A transform string, as per SVG guidelines.
};

struct VectorPoint {
   double  X;            // The X coordinate of this point.
   double  Y;            // The Y coordinate of this point.
   uint8_t XScaled:1;    // TRUE if the X value is scaled to its viewport (between 0 and 1.0).
   uint8_t YScaled:1;    // TRUE if the Y value is scaled to its viewport (between 0 and 1.0).
};

struct VectorPainter {
   objVectorPattern * Pattern;    // A VectorPattern object, suitable for pattern based fills.
   objVectorImage * Image;        // A VectorImage object, suitable for image fills.
   objGradient * Gradient;        // A Gradient object, suitable for gradient fills.
   struct FRGB Colour;            // A single RGB colour definition, suitable for block colour fills.  Colour values are unclamped to support all possible colour spaces.
   void reset() {
      Colour.Alpha = 0;
      Gradient = nullptr;
      Image    = nullptr;
      Pattern  = nullptr;
   }
};

struct PathCommand {
   PE      Type;        // The command type
   uint8_t LargeArc;    // Equivalent to the large-arc-flag in SVG, it ensures that the arc follows the longest drawing path when TRUE.
   uint8_t Sweep;       // Equivalent to the sweep-flag in SVG, it inverts the default behaviour in generating arc paths.
   uint8_t Pad1;        // Private
   double  X;           // The targeted X coordinate (absolute or scaled) for the command
   double  Y;           // The targeted Y coordinate (absolute or scaled) for the command
   double  AbsX;        // Private
   double  AbsY;        // Private
   double  X2;          // The X2 coordinate for curve commands, or RX for arcs
   double  Y2;          // The Y2 coordinate for curve commands, or RY for arcs
   double  X3;          // The X3 coordinate for curve-to or smooth-curve-to
   double  Y3;          // The Y3 coordinate for curve-to or smooth-curve-to
   double  Angle;       // Arc angle
};

struct VectorMatrix {
   struct VectorMatrix * Next;    // The next transform in the list.
   objVector * Vector;            // The vector associated with the transform.
   double ScaleX;                 // Matrix value A
   double ShearY;                 // Matrix value B
   double ShearX;                 // Matrix value C
   double ScaleY;                 // Matrix value D
   double TranslateX;             // Matrix value E
   double TranslateY;             // Matrix value F
   int    Tag;                    // An optional tag value defined by the client for matrix identification.
};

#define MTAG_ANIMATE_MOTION 0x1da6b394
#define MTAG_ANIMATE_TRANSFORM 0x3e521882
#define MTAG_SCENE_GRAPH 0x4445102d
#define MTAG_USE_TRANSFORM 0xa04f6c85
#define MTAG_SVG_TRANSFORM 0xdd1ae058

struct FontMetrics {
   int Height;         // Full font height equivalent to Ascent (cap-height) + Descent (gutter).  Does NOT include accents.
   int LineSpacing;    // Vertical advance from one line to the next.  Includes coverage for accents and additional whitespace.
   int Ascent;         // Height from the baseline to the top (cap-height) of the font.  Does NOT include accents.
   int Descent;        // Height from the baseline to the bottom of the font (gutter)
};

// VectorColour class definition

#define VER_VECTORCOLOUR (1.000000)

class objVectorColour : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORCOLOUR;
   static constexpr CSTRING CLASS_NAME = "VectorColour";

   using create = kt::Create<objVectorColour>;

   double Red;    // The red component value.
   double Green;  // The green component value.
   double Blue;   // The blue component value.
   double Alpha;  // The alpha component value.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getRed(double &Value) noexcept {
      Value = this->Red;
      return ERR::Okay;
   }

   inline ERR getGreen(double &Value) noexcept {
      Value = this->Green;
      return ERR::Okay;
   }

   inline ERR getBlue(double &Value) noexcept {
      Value = this->Blue;
      return ERR::Okay;
   }

   inline ERR getAlpha(double &Value) noexcept {
      Value = this->Alpha;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setRed(const double Value) noexcept {
      this->Red = Value;
      return ERR::Okay;
   }

   inline ERR setGreen(const double Value) noexcept {
      this->Green = Value;
      return ERR::Okay;
   }

   inline ERR setBlue(const double Value) noexcept {
      this->Blue = Value;
      return ERR::Okay;
   }

   inline ERR setAlpha(const double Value) noexcept {
      this->Alpha = Value;
      return ERR::Okay;
   }

};

// VectorTransition class definition

#define VER_VECTORTRANSITION (1.000000)

class objVectorTransition : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORTRANSITION;
   static constexpr CSTRING CLASS_NAME = "VectorTransition";

   using create = kt::Create<objVectorTransition>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getTotalStops(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + 88));
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setStops(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00001218, Value, Elements);
   }

};

// VectorScene class definition

#define VER_VECTORSCENE (1.000000)

// VectorScene methods

namespace sc {
struct AddDef { std::string_view Name; OBJECTPTR Def; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SearchByID { int ID; OBJECTPTR Result; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct FindDef { std::string_view Name; OBJECTPTR Def; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Debug { static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVectorScene : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSCENE;
   static constexpr CSTRING CLASS_NAME = "VectorScene";

   using create = kt::Create<objVectorScene>;

   int64_t  RenderTime;           // Returns the rendering time of the last scene.
   double   Gamma;                // Private. Not currently implemented.
   objVectorScene * HostScene;    // Refers to a top-level VectorScene object, if applicable.
   objVectorViewport * Viewport;  // References the first object in the scene, which must be a VectorViewport object.
   objBitmap * Bitmap;            // Target bitmap for drawing vectors.
   OBJECTID SurfaceID;            // May refer to a Surface object for enabling automatic rendering.
   VPF      Flags;                // Optional flags.
   int      PageWidth;            // The width of the page that contains the vector.
   int      PageHeight;           // The height of the page that contains the vector.
   VSM      SampleMethod;         // The sampling method to use when interpolating images and patterns.

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR redimension(double X, double Y, double Z, double Width, double Height, double Depth) noexcept {
      struct acRedimension args = { X, Y, Z, Width, Height, Depth };
      return Action(AC::Redimension, this, &args);
   }
   inline ERR redimension(double X, double Y, double Width, double Height) noexcept {
      struct acRedimension args = { X, Y, 0, Width, Height, 0 };
      return Action(AC::Redimension, this, &args);
   }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR resize(double Width, double Height, double Depth = 0) noexcept {
      struct acResize args = { Width, Height, Depth };
      return Action(AC::Resize, this, &args);
   }
   inline ERR addDef(const std::string_view &Name, OBJECTPTR Def) noexcept {
      struct sc::AddDef args = { Name, Def };
      return Action(AC(-1), this, &args);
   }
   inline ERR searchByID(int ID, OBJECTPTR * Result) noexcept {
      struct sc::SearchByID args = { ID, (OBJECTPTR)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR findDef(const std::string_view &Name, OBJECTPTR * Def) noexcept {
      struct sc::FindDef args = { Name, (OBJECTPTR)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Def) *Def = args.Def;
      return error;
   }
   inline ERR debug() noexcept {
      return Action(AC(-4), this, nullptr);
   }

   // Customised field getting

   inline ERR getRenderTime(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getHostScene(objVectorScene * &Value) noexcept {
      Value = this->HostScene;
      return ERR::Okay;
   }

   inline ERR getViewport(objVectorViewport * &Value) noexcept {
      Value = this->Viewport;
      return ERR::Okay;
   }

   inline ERR getBitmap(objBitmap * &Value) noexcept {
      Value = this->Bitmap;
      return ERR::Okay;
   }

   inline ERR getSurface(OBJECTID &Value) noexcept {
      Value = this->SurfaceID;
      return ERR::Okay;
   }

   inline ERR getFlags(VPF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getPageWidth(int &Value) noexcept {
      Value = this->PageWidth;
      return ERR::Okay;
   }

   inline ERR getPageHeight(int &Value) noexcept {
      Value = this->PageHeight;
      return ERR::Okay;
   }

   inline ERR getSampleMethod(VSM &Value) noexcept {
      Value = this->SampleMethod;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setGamma(const double Value) noexcept {
      this->Gamma = Value;
      return ERR::Okay;
   }

   inline ERR setHostScene(objVectorScene * Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->HostScene = Value;
      return ERR::Okay;
   }

   inline ERR setBitmap(objBitmap * Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setSurface(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VPF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setPageWidth(const int Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setPageHeight(const int Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setSampleMethod(const VSM Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorImage class definition

#define VER_VECTORIMAGE (1.000000)

class objVectorImage : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORIMAGE;
   static constexpr CSTRING CLASS_NAME = "VectorImage";

   using create = kt::Create<objVectorImage>;

   Unit    X;               // Apply a horizontal offset to the image, the origin of which is determined by the Units value.
   Unit    Y;               // Apply a vertical offset to the image, the origin of which is determined by the Units value.
   objImage * Image;        // Refers to a Image from which the source Bitmap is acquired.
   objBitmap * Bitmap;      // Reference to a source bitmap for the rendering algorithm.
   VUNIT   Units;           // Declares the coordinate system to use for the X and Y values.
   VSPREAD SpreadMethod;    // Defines image tiling behaviour, if desired.
   ARF     AspectRatio;     // Flags that affect the aspect ratio of the image within its target vector.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getX(Unit &Value) noexcept {
      Value = *((Unit *)(((int8_t *)this) + 88));
      return ERR::Okay;
   }

   inline ERR getY(Unit &Value) noexcept {
      Value = *((Unit *)(((int8_t *)this) + 104));
      return ERR::Okay;
   }

   inline ERR getImage(objImage * &Value) noexcept {
      Value = this->Image;
      return ERR::Okay;
   }

   inline ERR getBitmap(objBitmap * &Value) noexcept {
      Value = this->Bitmap;
      return ERR::Okay;
   }

   inline ERR getUnits(VUNIT &Value) noexcept {
      Value = this->Units;
      return ERR::Okay;
   }

   inline ERR getSpreadMethod(VSPREAD &Value) noexcept {
      Value = this->SpreadMethod;
      return ERR::Okay;
   }

   inline ERR getAspectRatio(ARF &Value) noexcept {
      Value = this->AspectRatio;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setX(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setY(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setImage(objImage * Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setBitmap(objBitmap * Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setUnits(const VUNIT Value) noexcept {
      this->Units = Value;
      return ERR::Okay;
   }

   inline ERR setSpreadMethod(const VSPREAD Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setAspectRatio(const ARF Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorPattern class definition

#define VER_VECTORPATTERN (1.000000)

class objVectorPattern : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPATTERN;
   static constexpr CSTRING CLASS_NAME = "VectorPattern";

   using create = kt::Create<objVectorPattern>;

   double  X;                       // X coordinate for the pattern.
   double  Y;                       // Y coordinate for the pattern.
   double  Width;                   // Width of the pattern tile.
   double  Height;                  // Height of the pattern tile.
   double  Opacity;                 // The opacity of the pattern.
   objVectorScene * Scene;          // Refers to the internal VectorScene that will contain the rendered pattern.
   objVectorViewport * Viewport;    // Refers to the viewport that contains the pattern.
   objVectorPattern * Inherit;      // Inherit attributes from a VectorPattern referenced here.
   VSPREAD SpreadMethod;            // The behaviour to use when the pattern bounds do not match the vector path.
   VUNIT   Units;                   // Defines the coordinate system for fields X, Y, Width and Height.
   VUNIT   ContentUnits;            // Private. Not yet implemented.
   DMF     Dimensions;              // Dimension flags are stored here.

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getOpacity(double &Value) noexcept {
      Value = this->Opacity;
      return ERR::Okay;
   }

   inline ERR getScene(objVectorScene * &Value) noexcept {
      Value = this->Scene;
      return ERR::Okay;
   }

   inline ERR getViewport(objVectorViewport * &Value) noexcept {
      Value = this->Viewport;
      return ERR::Okay;
   }

   inline ERR getInherit(objVectorPattern * &Value) noexcept {
      Value = this->Inherit;
      return ERR::Okay;
   }

   inline ERR getSpreadMethod(VSPREAD &Value) noexcept {
      Value = this->SpreadMethod;
      return ERR::Okay;
   }

   inline ERR getUnits(VUNIT &Value) noexcept {
      Value = this->Units;
      return ERR::Okay;
   }

   inline ERR getDimensions(DMF &Value) noexcept {
      Value = this->Dimensions;
      return ERR::Okay;
   }

   inline ERR getMatrices(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setInherit(objVectorPattern * Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setSpreadMethod(const VSPREAD Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setUnits(const VUNIT Value) noexcept {
      this->Units = Value;
      return ERR::Okay;
   }

   inline ERR setContentUnits(const VUNIT Value) noexcept {
      this->ContentUnits = Value;
      return ERR::Okay;
   }

   inline ERR setMatrices(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x08100318, Value, 1);
   }

   inline ERR setTransform(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, 0x00804208, &Value, 1);
   }

};

// Gradient class definition

#define VER_GRADIENT (1.000000)

class objGradient : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENT;
   static constexpr CSTRING CLASS_NAME = "Gradient";

   using create = kt::Create<objGradient>;

   double  Resolution;      // Affects the rate of change for colours in the gradient.
   double  Gamma;           // Applies a gamma curve to the gradient ramp.
   VSPREAD SpreadMethod;    // Determines the rendering behaviour to use when gradient colours are cycled.
   VUNIT   Units;           // Defines the coordinate system for gradient coordinates.
   VCS     ColourSpace;     // Defines the colour space to use when interpolating gradient colours.
   GEZ     Easing;          // Selects the easing function for interpolation between gradient stops.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getResolution(double &Value) noexcept {
      Value = this->Resolution;
      return ERR::Okay;
   }

   inline ERR getGamma(double &Value) noexcept {
      Value = this->Gamma;
      return ERR::Okay;
   }

   inline ERR getSpreadMethod(VSPREAD &Value) noexcept {
      Value = this->SpreadMethod;
      return ERR::Okay;
   }

   inline ERR getUnits(VUNIT &Value) noexcept {
      Value = this->Units;
      return ERR::Okay;
   }

   inline ERR getColourSpace(VCS &Value) noexcept {
      Value = this->ColourSpace;
      return ERR::Okay;
   }

   inline ERR getEasing(GEZ &Value) noexcept {
      Value = this->Easing;
      return ERR::Okay;
   }

   inline ERR getColour(float * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto get_field = (ERR (*)(APTR, float *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getColourMap(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getMatrices(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getNumeric(int &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getID(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getStops(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[17];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getTotalStops(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setResolution(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setGamma(const double Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSpreadMethod(const VSPREAD Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setUnits(const VUNIT Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Units = Value;
      return ERR::Okay;
   }

   inline ERR setColourSpace(const VCS Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ColourSpace = Value;
      return ERR::Okay;
   }

   inline ERR setEasing(const GEZ Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setColour(const float * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, 0x10101308, Value, Elements);
   }

   inline ERR setColourMap(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00904208, &Value, 1);
   }

   inline ERR setMatrices(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x08100318, Value, 1);
   }

   inline ERR setNumeric(const int Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setID(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setStops(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00101318, Value, Elements);
   }

   inline ERR setTransform(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, 0x00804208, &Value, 1);
   }

};

// GradientLinear class definition

#define VER_GRADIENTLINEAR (1.000000)

class objGradientLinear : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTLINEAR;
   static constexpr CSTRING CLASS_NAME = "GradientLinear";

   using create = kt::Create<objGradientLinear>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getX1(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY1(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getX2(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY2(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }


   // Customised field setting

   inline ERR setX1(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY1(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setX2(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY2(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

};

// GradientRadial class definition

#define VER_GRADIENTRADIAL (1.000000)

class objGradientRadial : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTRADIAL;
   static constexpr CSTRING CLASS_NAME = "GradientRadial";

   using create = kt::Create<objGradientRadial>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getCX(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCY(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFX(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFY(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getRadius(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFocalRadius(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getContainFocal(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setCX(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setCY(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setFX(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setFY(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setRadius(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setFocalRadius(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setContainFocal(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// GradientConic class definition

#define VER_GRADIENTCONIC (1.000000)

class objGradientConic : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTCONIC;
   static constexpr CSTRING CLASS_NAME = "GradientConic";

   using create = kt::Create<objGradientConic>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getRadius(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCX(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCY(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSpan(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setRadius(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setCX(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setCY(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setSpan(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// GradientDiamond class definition

#define VER_GRADIENTDIAMOND (1.000000)

class objGradientDiamond : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTDIAMOND;
   static constexpr CSTRING CLASS_NAME = "GradientDiamond";

   using create = kt::Create<objGradientDiamond>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getCX(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCY(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getRadius(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setCX(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setCY(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setRadius(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

};

// GradientContour class definition

#define VER_GRADIENTCONTOUR (1.000000)

class objGradientContour : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTCONTOUR;
   static constexpr CSTRING CLASS_NAME = "GradientContour";

   using create = kt::Create<objGradientContour>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getFloor(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMultiplier(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setFloor(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setMultiplier(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

};

// GradientGouraud class definition

#define VER_GRADIENTGOURAUD (1.000000)

class objGradientGouraud : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTGOURAUD;
   static constexpr CSTRING CLASS_NAME = "GradientGouraud";

   using create = kt::Create<objGradientGouraud>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getVertices(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getIndices(int * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto get_field = (ERR (*)(APTR, int *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }


   // Customised field setting

   inline ERR setColour(const float * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, 0x10111308, Value, Elements);
   }

   inline ERR setColourMap(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00914208, &Value, 1);
   }

   inline ERR setSpreadMethod(const int Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setStops(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00111318, Value, Elements);
   }

   inline ERR setVertices(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, 0x00101318, Value, Elements);
   }

   inline ERR setIndices(const int * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x40101308, Value, Elements);
   }

};

// GradientDistal class definition

#define VER_GRADIENTDISTAL (1.000000)

class objGradientDistal : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTDISTAL;
   static constexpr CSTRING CLASS_NAME = "GradientDistal";

   using create = kt::Create<objGradientDistal>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getFloor(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMultiplier(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getRadius(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getInnerRadius(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getInnerFall(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getOuterFall(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setFloor(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setMultiplier(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setRadius(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setInnerRadius(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setInnerFall(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setOuterFall(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// GradientVoronoi class definition

#define VER_GRADIENTVORONOI (1.000000)

class objGradientVoronoi : public objGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTVORONOI;
   static constexpr CSTRING CLASS_NAME = "GradientVoronoi";

   using create = kt::Create<objGradientVoronoi>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getFloor(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMultiplier(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSeed(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPointCount(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getWorleyMode(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getWorleyMetric(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getHeightMin(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getHeightMax(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getJitter(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setFloor(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setMultiplier(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_UNIT, &Value, 1);
   }

   inline ERR setSeed(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT64, &Value, 1);
   }

   inline ERR setPointCount(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setWorleyMode(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setWorleyMetric(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setHeightMin(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setHeightMax(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setJitter(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// FilterEffect class definition

#define VER_FILTEREFFECT (1.000000)

class objFilterEffect : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FILTEREFFECT;
   static constexpr CSTRING CLASS_NAME = "FilterEffect";

   using create = kt::Create<objFilterEffect>;

   objFilterEffect * Next;    // Next filter in the chain.
   objFilterEffect * Prev;    // Previous filter in the chain.
   objBitmap * Target;        // Target bitmap for rendering the effect.
   objFilterEffect * Input;   // Reference to another effect to be used as an input source.
   objFilterEffect * Mix;     // Reference to another effect to be used a mixer with Input.
   double X;                  // Primitive X coordinate for the effect.
   double Y;                  // Primitive Y coordinate for the effect.
   double Width;              // Primitive width of the effect area.
   double Height;             // Primitive height of the effect area.
   DMF    Dimensions;         // Dimension flags are stored here.
   VSF    SourceType;         // Specifies an input source for the effect algorithm, if required.
   VSF    MixType;            // If a secondary mix input is required for the effect, specify it here.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR moveToBack() noexcept { return Action(AC::MoveToBack, this, nullptr); }
   inline ERR moveToFront() noexcept { return Action(AC::MoveToFront, this, nullptr); }

   // Customised field getting

   inline ERR getNext(objFilterEffect * &Value) noexcept {
      Value = this->Next;
      return ERR::Okay;
   }

   inline ERR getPrev(objFilterEffect * &Value) noexcept {
      Value = this->Prev;
      return ERR::Okay;
   }

   inline ERR getTarget(objBitmap * &Value) noexcept {
      Value = this->Target;
      return ERR::Okay;
   }

   inline ERR getInput(objFilterEffect * &Value) noexcept {
      Value = this->Input;
      return ERR::Okay;
   }

   inline ERR getMix(objFilterEffect * &Value) noexcept {
      Value = this->Mix;
      return ERR::Okay;
   }

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getDimensions(DMF &Value) noexcept {
      Value = this->Dimensions;
      return ERR::Okay;
   }

   inline ERR getSourceType(VSF &Value) noexcept {
      Value = this->SourceType;
      return ERR::Okay;
   }

   inline ERR getMixType(VSF &Value) noexcept {
      Value = this->MixType;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setNext(objFilterEffect * Value) noexcept {
      this->Next = Value;
      return ERR::Okay;
   }

   inline ERR setPrev(objFilterEffect * Value) noexcept {
      this->Prev = Value;
      return ERR::Okay;
   }

   inline ERR setTarget(objBitmap * Value) noexcept {
      this->Target = Value;
      return ERR::Okay;
   }

   inline ERR setInput(objFilterEffect * Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setMix(objFilterEffect * Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setSourceType(const VSF Value) noexcept {
      this->SourceType = Value;
      return ERR::Okay;
   }

   inline ERR setMixType(const VSF Value) noexcept {
      this->MixType = Value;
      return ERR::Okay;
   }

};

struct MergeSource {
   VSF SourceType;              // The type of the required source.
   objFilterEffect * Effect;    // Effect pointer if the SourceType is REFERENCE.
  MergeSource(VSF pType, objFilterEffect *pEffect = nullptr) : SourceType(pType), Effect(pEffect) { };
};

// ImageFX class definition

#define VER_IMAGEFX (1.000000)

class objImageFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::IMAGEFX;
   static constexpr CSTRING CLASS_NAME = "ImageFX";

   using create = kt::Create<objImageFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getBitmap(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }

   inline ERR getAspectRatio(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getResampleMethod(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, 0x00804508, &Value, 1);
   }

   inline ERR setAspectRatio(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setResampleMethod(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// SourceFX class definition

#define VER_SOURCEFX (1.000000)

class objSourceFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SOURCEFX;
   static constexpr CSTRING CLASS_NAME = "SourceFX";

   using create = kt::Create<objSourceFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getAspectRatio(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSource(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setAspectRatio(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setSourceName(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, 0x00804408, &Value, 1);
   }

   inline ERR setSource(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, 0x08100109, Value, 1);
   }

};

// BlurFX class definition

#define VER_BLURFX (1.000000)

class objBlurFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::BLURFX;
   static constexpr CSTRING CLASS_NAME = "BlurFX";

   using create = kt::Create<objBlurFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getSX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setSX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// ColourFX class definition

#define VER_COLOURFX (1.000000)

class objColourFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COLOURFX;
   static constexpr CSTRING CLASS_NAME = "ColourFX";

   using create = kt::Create<objColourFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getMode(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getValues(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setMode(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setValues(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x80101508, Value, Elements);
   }

};

// CompositeFX class definition

#define VER_COMPOSITEFX (1.000000)

class objCompositeFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPOSITEFX;
   static constexpr CSTRING CLASS_NAME = "CompositeFX";

   using create = kt::Create<objCompositeFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getOperator(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getK1(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getK2(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getK3(double &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getK4(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setOperator(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setK1(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setK2(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setK3(const double Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setK4(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// ConvolveFX class definition

#define VER_CONVOLVEFX (1.000000)

class objConvolveFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONVOLVEFX;
   static constexpr CSTRING CLASS_NAME = "ConvolveFX";

   using create = kt::Create<objConvolveFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getBias(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDivisor(double &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getEdgeMode(int &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMatrixRows(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMatrixColumns(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMatrix(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getPreserveAlpha(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTargetX(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTargetY(int &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getUnitX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getUnitY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setBias(const double Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setDivisor(const double Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setEdgeMode(const int Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setMatrixRows(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setMatrixColumns(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setMatrix(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, 0x80101508, Value, Elements);
   }

   inline ERR setPreserveAlpha(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setTargetX(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setTargetY(const int Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setUnitX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setUnitY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// DisplacementFX class definition

#define VER_DISPLACEMENTFX (1.000000)

class objDisplacementFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DISPLACEMENTFX;
   static constexpr CSTRING CLASS_NAME = "DisplacementFX";

   using create = kt::Create<objDisplacementFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getScale(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXChannel(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getYChannel(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setScale(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setXChannel(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setYChannel(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// FloodFX class definition

#define VER_FLOODFX (1.000000)

class objFloodFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FLOODFX;
   static constexpr CSTRING CLASS_NAME = "FloodFX";

   using create = kt::Create<objFloodFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getColour(float * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto get_field = (ERR (*)(APTR, float *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getOpacity(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setColour(const float * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x10101308, Value, Elements);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// LightingFX class definition

#define VER_LIGHTINGFX (1.000000)

// LightingFX methods

namespace lt {
struct SetDistantLight { double Azimuth; double Elevation; static const AC id = AC(-20); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetPointLight { double X; double Y; double Z; static const AC id = AC(-22); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetSpotLight { double X; double Y; double Z; double PX; double PY; double PZ; double Exponent; double ConeAngle; static const AC id = AC(-21); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objLightingFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::LIGHTINGFX;
   static constexpr CSTRING CLASS_NAME = "LightingFX";

   using create = kt::Create<objLightingFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR setDistantLight(double Azimuth, double Elevation) noexcept {
      struct lt::SetDistantLight args = { Azimuth, Elevation };
      return Action(AC(-20), this, &args);
   }
   inline ERR setPointLight(double X, double Y, double Z) noexcept {
      struct lt::SetPointLight args = { X, Y, Z };
      return Action(AC(-22), this, &args);
   }
   inline ERR setSpotLight(double X, double Y, double Z, double PX, double PY, double PZ, double Exponent, double ConeAngle) noexcept {
      struct lt::SetSpotLight args = { X, Y, Z, PX, PY, PZ, Exponent, ConeAngle };
      return Action(AC(-21), this, &args);
   }

   // Customised field getting

   inline ERR getColour(float * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto get_field = (ERR (*)(APTR, float *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getConstant(double &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getExponent(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getScale(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getType(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getUnitX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getUnitY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setColour(const float * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, 0x10101308, Value, Elements);
   }

   inline ERR setConstant(const double Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setExponent(const double Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setScale(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setType(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setUnitX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setUnitY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// MergeFX class definition

#define VER_MERGEFX (1.000000)

class objMergeFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MERGEFX;
   static constexpr CSTRING CLASS_NAME = "MergeFX";

   using create = kt::Create<objMergeFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getSourceList(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setSourceList(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x00101318, Value, Elements);
   }

};

// MorphologyFX class definition

#define VER_MORPHOLOGYFX (1.000000)

class objMorphologyFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MORPHOLOGYFX;
   static constexpr CSTRING CLASS_NAME = "MorphologyFX";

   using create = kt::Create<objMorphologyFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getOperator(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getRadiusX(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getRadiusY(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setOperator(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setRadiusX(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setRadiusY(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// OffsetFX class definition

#define VER_OFFSETFX (1.000000)

class objOffsetFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::OFFSETFX;
   static constexpr CSTRING CLASS_NAME = "OffsetFX";

   using create = kt::Create<objOffsetFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getXOffset(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getYOffset(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setXOffset(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setYOffset(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// RemapFX class definition

#define VER_REMAPFX (1.000000)

// RemapFX methods

namespace rf {
struct SelectGamma { CMP Component; double Amplitude; double Offset; double Exponent; static const AC id = AC(-20); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectTable { CMP Component; kt::vector<double> *Values; static const AC id = AC(-21); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectLinear { CMP Component; double Slope; double Intercept; static const AC id = AC(-22); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectIdentity { CMP Component; static const AC id = AC(-23); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectDiscrete { CMP Component; kt::vector<double> *Values; static const AC id = AC(-24); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectInvert { CMP Component; static const AC id = AC(-25); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectMask { CMP Component; int Mask; static const AC id = AC(-26); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objRemapFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::REMAPFX;
   static constexpr CSTRING CLASS_NAME = "RemapFX";

   using create = kt::Create<objRemapFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR selectGamma(CMP Component, double Amplitude, double Offset, double Exponent) noexcept {
      struct rf::SelectGamma args = { Component, Amplitude, Offset, Exponent };
      return Action(AC(-20), this, &args);
   }
   inline ERR selectTable(CMP Component, kt::vector<double> &Values) noexcept {
      struct rf::SelectTable args = { Component, &Values };
      ERR error = Action(AC(-21), this, &args);
      return error;
   }
   inline ERR selectLinear(CMP Component, double Slope, double Intercept) noexcept {
      struct rf::SelectLinear args = { Component, Slope, Intercept };
      return Action(AC(-22), this, &args);
   }
   inline ERR selectIdentity(CMP Component) noexcept {
      struct rf::SelectIdentity args = { Component };
      return Action(AC(-23), this, &args);
   }
   inline ERR selectDiscrete(CMP Component, kt::vector<double> &Values) noexcept {
      struct rf::SelectDiscrete args = { Component, &Values };
      ERR error = Action(AC(-24), this, &args);
      return error;
   }
   inline ERR selectInvert(CMP Component) noexcept {
      struct rf::SelectInvert args = { Component };
      return Action(AC(-25), this, &args);
   }
   inline ERR selectMask(CMP Component, int Mask) noexcept {
      struct rf::SelectMask args = { Component, Mask };
      return Action(AC(-26), this, &args);
   }

   // Customised field getting

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

};

// TurbulenceFX class definition

#define VER_TURBULENCEFX (1.000000)

class objTurbulenceFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TURBULENCEFX;
   static constexpr CSTRING CLASS_NAME = "TurbulenceFX";

   using create = kt::Create<objTurbulenceFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getFX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getOctaves(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSeed(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getStitch(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getType(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setFX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setFY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setOctaves(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setSeed(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setStitch(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setType(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// WaveFunctionFX class definition

#define VER_WAVEFUNCTIONFX (1.000000)

class objWaveFunctionFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WAVEFUNCTIONFX;
   static constexpr CSTRING CLASS_NAME = "WaveFunctionFX";

   using create = kt::Create<objWaveFunctionFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getAspectRatio(int &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getColourMap(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getN(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getL(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getM(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getResolution(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getScale(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getStops(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getXMLDef(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setAspectRatio(const int Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setColourMap(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setN(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setL(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setM(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setResolution(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setScale(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setStops(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, 0x00101318, Value, Elements);
   }

};

// VectorClip class definition

#define VER_VECTORCLIP (1.000000)

class objVectorClip : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORCLIP;
   static constexpr CSTRING CLASS_NAME = "VectorClip";

   using create = kt::Create<objVectorClip>;

   objVectorViewport * Viewport;    // This viewport hosts the Vector objects that will contribute to the clip path.
   VUNIT Units;                     // Defines the coordinate system for fields X, Y, Width and Height.
   VCLF  Flags;                     // Optional flags.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getViewport(objVectorViewport * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getUnits(VUNIT &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFlags(VCLF &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setUnits(const VUNIT Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VCLF Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorFilter class definition

#define VER_VECTORFILTER (1.000000)

class objVectorFilter : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORFILTER;
   static constexpr CSTRING CLASS_NAME = "VectorFilter";

   using create = kt::Create<objVectorFilter>;

   double X;                     // X coordinate for the filter.
   double Y;                     // Y coordinate for the filter.
   double Width;                 // The width of the filter area.  Can be expressed as a fixed or scaled coordinate.
   double Height;                // The height of the filter area.  Can be expressed as a fixed or scaled coordinate.
   double Opacity;               // The opacity of the filter.
   objVectorFilter * Inherit;    // Inherit attributes from a VectorFilter referenced here.
   int    ResX;                  // Width of the intermediate images, measured in pixels.
   int    ResY;                  // Height of the intermediate images, measured in pixels.
   VUNIT  Units;                 // Defines the coordinate system for X, Y, Width and Height.
   VUNIT  PrimitiveUnits;        // Alters the behaviour of some effects that support alternative position calculations.
   DMF    Dimensions;            // Dimension flags define whether individual dimension fields contain fixed or scaled values.
   VCS    ColourSpace;           // The colour space of the filter graphics (SRGB or linear RGB).
   VFA    AspectRatio;           // Aspect ratio to use when scaling X/Y values

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getOpacity(double &Value) noexcept {
      Value = this->Opacity;
      return ERR::Okay;
   }

   inline ERR getInherit(objVectorFilter * &Value) noexcept {
      Value = this->Inherit;
      return ERR::Okay;
   }

   inline ERR getResX(int &Value) noexcept {
      Value = this->ResX;
      return ERR::Okay;
   }

   inline ERR getResY(int &Value) noexcept {
      Value = this->ResY;
      return ERR::Okay;
   }

   inline ERR getUnits(VUNIT &Value) noexcept {
      Value = this->Units;
      return ERR::Okay;
   }

   inline ERR getPrimitiveUnits(VUNIT &Value) noexcept {
      Value = this->PrimitiveUnits;
      return ERR::Okay;
   }

   inline ERR getDimensions(DMF &Value) noexcept {
      Value = this->Dimensions;
      return ERR::Okay;
   }

   inline ERR getColourSpace(VCS &Value) noexcept {
      Value = this->ColourSpace;
      return ERR::Okay;
   }

   inline ERR getAspectRatio(VFA &Value) noexcept {
      Value = this->AspectRatio;
      return ERR::Okay;
   }

   inline ERR getEffectXML(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setInherit(objVectorFilter * Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setResX(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ResX = Value;
      return ERR::Okay;
   }

   inline ERR setResY(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ResY = Value;
      return ERR::Okay;
   }

   inline ERR setUnits(const VUNIT Value) noexcept {
      this->Units = Value;
      return ERR::Okay;
   }

   inline ERR setPrimitiveUnits(const VUNIT Value) noexcept {
      this->PrimitiveUnits = Value;
      return ERR::Okay;
   }

   inline ERR setColourSpace(const VCS Value) noexcept {
      this->ColourSpace = Value;
      return ERR::Okay;
   }

   inline ERR setAspectRatio(const VFA Value) noexcept {
      this->AspectRatio = Value;
      return ERR::Okay;
   }

};

// Vector class definition

#define VER_VECTOR (1.000000)

// Vector methods

namespace vec {
struct Push { int Position; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Trace { FUNCTION *Callback; double Scale; int Transform; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetBoundary { VBF Flags; double X; double Y; double Width; double Height; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct PointInPath { double X; double Y; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SubscribeInput { JTYPE Mask; FUNCTION *Callback; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SubscribeKeyboard { FUNCTION *Callback; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SubscribeFeedback { FM Mask; FUNCTION *Callback; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Debug { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct NewMatrix { struct VectorMatrix *Transform; int End; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct FreeMatrix { struct VectorMatrix *Matrix; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVector : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTOR;
   static constexpr CSTRING CLASS_NAME = "Vector";

   using create = kt::Create<objVector>;

   objVector * Child;                 // The first child vector, or NULL.
   objVectorScene * Scene;            // Short-cut to the top-level VectorScene.
   objVector * Next;                  // The next vector in the branch, or NULL.
   objVector * Prev;                  // The previous vector in the branch, or NULL.
   OBJECTPTR Parent;                  // The parent of the vector, or NULL if this is the top-most vector.
   struct VectorMatrix * Matrices;    // A linked list of transform matrices that have been applied to the vector.
   double    StrokeOpacity;           // Defines the opacity of the path stroke.
   double    FillOpacity;             // The opacity to use when filling the vector.
   double    Opacity;                 // Defines an overall opacity for the vector's graphics.
   double    MiterLimit;              // Imposes a limit on the ratio of the miter length to the StrokeWidth.
   double    InnerMiterLimit;         // Private. No internal documentation exists for this feature.
   double    DashOffset;              // The distance into the dash pattern to start the dash.  Can be a negative number.
   VIS       Visibility;              // Controls the visibility of a vector and its children.
   VF        Flags;                   // Optional flags.
   PTC       Cursor;                  // The mouse cursor to display when the pointer is within the vector's boundary.
   RQ        PathQuality;             // Defines the quality of a path when it is rendered.
   VCS       ColourSpace;             // Defines the colour space to use when blending the vector with a target bitmap's content.
   int       PathTimestamp;           // This counter is modified each time the path is regenerated.

   // Action stubs

   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR hide() noexcept { return Action(AC::Hide, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR moveToBack() noexcept { return Action(AC::MoveToBack, this, nullptr); }
   inline ERR moveToFront() noexcept { return Action(AC::MoveToFront, this, nullptr); }
   inline ERR show() noexcept { return Action(AC::Show, this, nullptr); }
   inline ERR push(int Position) noexcept {
      struct vec::Push args = { Position };
      return Action(AC(-1), this, &args);
   }
   inline ERR trace(FUNCTION Callback, double Scale, int Transform) noexcept {
      struct vec::Trace args = { &Callback, Scale, Transform };
      return Action(AC(-2), this, &args);
   }
   inline ERR getBoundary(VBF Flags, double * X, double * Y, double * Width, double * Height) noexcept {
      struct vec::GetBoundary args = { Flags, (double)0, (double)0, (double)0, (double)0 };
      ERR error = Action(AC(-3), this, &args);
      if (X) *X = args.X;
      if (Y) *Y = args.Y;
      if (Width) *Width = args.Width;
      if (Height) *Height = args.Height;
      return error;
   }
   inline ERR pointInPath(double X, double Y) noexcept {
      struct vec::PointInPath args = { X, Y };
      return Action(AC(-4), this, &args);
   }
   inline ERR subscribeInput(JTYPE Mask, FUNCTION Callback) noexcept {
      struct vec::SubscribeInput args = { Mask, &Callback };
      return Action(AC(-5), this, &args);
   }
   inline ERR subscribeKeyboard(FUNCTION Callback) noexcept {
      struct vec::SubscribeKeyboard args = { &Callback };
      return Action(AC(-6), this, &args);
   }
   inline ERR subscribeFeedback(FM Mask, FUNCTION Callback) noexcept {
      struct vec::SubscribeFeedback args = { Mask, &Callback };
      return Action(AC(-7), this, &args);
   }
   inline ERR debug() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR newMatrix(struct VectorMatrix ** Transform, int End) noexcept {
      struct vec::NewMatrix args = { (struct VectorMatrix *)0, End };
      ERR error = Action(AC(-9), this, &args);
      if (Transform) *Transform = args.Transform;
      return error;
   }
   inline ERR freeMatrix(struct VectorMatrix * Matrix) noexcept {
      struct vec::FreeMatrix args = { Matrix };
      return Action(AC(-10), this, &args);
   }

   // Customised field getting

   inline ERR getChild(objVector * &Value) noexcept {
      Value = this->Child;
      return ERR::Okay;
   }

   inline ERR getScene(objVectorScene * &Value) noexcept {
      Value = this->Scene;
      return ERR::Okay;
   }

   inline ERR getNext(objVector * &Value) noexcept {
      Value = this->Next;
      return ERR::Okay;
   }

   inline ERR getPrev(objVector * &Value) noexcept {
      Value = this->Prev;
      return ERR::Okay;
   }

   inline ERR getParent(OBJECTPTR &Value) noexcept {
      Value = this->Parent;
      return ERR::Okay;
   }

   inline ERR getMatrices(struct VectorMatrix * &Value) noexcept {
      Value = this->Matrices;
      return ERR::Okay;
   }

   inline ERR getStrokeOpacity(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFillOpacity(double &Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getOpacity(double &Value) noexcept {
      Value = this->Opacity;
      return ERR::Okay;
   }

   inline ERR getMiterLimit(double &Value) noexcept {
      Value = this->MiterLimit;
      return ERR::Okay;
   }

   inline ERR getDashOffset(double &Value) noexcept {
      Value = this->DashOffset;
      return ERR::Okay;
   }

   inline ERR getVisibility(VIS &Value) noexcept {
      Value = this->Visibility;
      return ERR::Okay;
   }

   inline ERR getFlags(VF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getCursor(PTC &Value) noexcept {
      Value = this->Cursor;
      return ERR::Okay;
   }

   inline ERR getPathQuality(RQ &Value) noexcept {
      Value = this->PathQuality;
      return ERR::Okay;
   }

   inline ERR getColourSpace(VCS &Value) noexcept {
      Value = this->ColourSpace;
      return ERR::Okay;
   }

   inline ERR getPathTimestamp(int &Value) noexcept {
      Value = this->PathTimestamp;
      return ERR::Okay;
   }

   inline ERR getClipRule(int &Value) noexcept {
      auto field = &this->Class->Dictionary[42];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDashArray(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getDisplayScale(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getMask(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMorph(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getAppendPath(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[41];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMorphFlags(int &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getNumeric(int &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSID(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getSequence(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }

   inline ERR getStroke(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getStrokeColour(float * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto get_field = (ERR (*)(APTR, float *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getStrokeWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getTransition(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[39];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFill(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getFillColour(float * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto get_field = (ERR (*)(APTR, float *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getFillRule(int &Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFilter(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getLineJoin(int &Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getLineCap(int &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getInnerJoin(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTabOrder(int &Value) noexcept {
      auto field = &this->Class->Dictionary[40];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setNext(objVector * Value) noexcept {
      auto field = &this->Class->Dictionary[26];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setPrev(objVector * Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, 0x08000301, Value, 1);
   }

   inline ERR setStrokeOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setFillOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setMiterLimit(const double Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setInnerMiterLimit(const double Value) noexcept {
      this->InnerMiterLimit = Value;
      return ERR::Okay;
   }

   inline ERR setDashOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setVisibility(const VIS Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setCursor(const PTC Value) noexcept {
      auto field = &this->Class->Dictionary[44];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setPathQuality(const RQ Value) noexcept {
      this->PathQuality = Value;
      return ERR::Okay;
   }

   inline ERR setColourSpace(const VCS Value) noexcept {
      this->ColourSpace = Value;
      return ERR::Okay;
   }

   inline ERR setClipRule(const int Value) noexcept {
      auto field = &this->Class->Dictionary[42];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setDashArray(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, 0x80101308, Value, Elements);
   }

   inline ERR setMask(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      return field->WriteValue(this, field, 0x08100309, Value, 1);
   }

   inline ERR setMorph(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      return field->WriteValue(this, field, 0x08100309, Value, 1);
   }

   inline ERR setAppendPath(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[41];
      return field->WriteValue(this, field, 0x08100309, Value, 1);
   }

   inline ERR setMorphFlags(const int Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setNumeric(const int Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setSID(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setResizeEvent(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setStroke(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setStrokeColour(const float * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x10101308, Value, Elements);
   }

   inline ERR setStrokeWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setTransition(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[39];
      return field->WriteValue(this, field, 0x08100309, Value, 1);
   }

   inline ERR setFill(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setFillColour(const float * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, 0x10101308, Value, Elements);
   }

   inline ERR setFillRule(const int Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFilter(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setLineJoin(const int Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setLineCap(const int Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setInnerJoin(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setTabOrder(const int Value) noexcept {
      auto field = &this->Class->Dictionary[40];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorPath class definition

#define VER_VECTORPATH (1.000000)

// VectorPath methods

namespace vp {
struct AddCommand { struct PathCommand *Commands; int Size; static const AC id = AC(-30); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveCommand { int Index; int Total; static const AC id = AC(-31); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetCommand { int Index; struct PathCommand *Command; int Size; static const AC id = AC(-32); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetCommand { int Index; struct PathCommand *Command; static const AC id = AC(-33); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetCommandList { APTR Commands; int Size; static const AC id = AC(-34); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVectorPath : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPATH;
   static constexpr CSTRING CLASS_NAME = "VectorPath";

   using create = kt::Create<objVectorPath>;

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR move(double X, double Y, double Z) noexcept {
      struct acMove args = { X, Y, Z };
      return Action(AC::Move, this, &args);
   }
   inline ERR moveToPoint(double X, double Y, double Z, MTF Flags) noexcept {
      struct acMoveToPoint moveto = { X, Y, Z, Flags };
      return Action(AC::MoveToPoint, this, &moveto);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR addCommand(struct PathCommand * Commands, int Size) noexcept {
      struct vp::AddCommand args = { Commands, Size };
      return Action(AC(-30), this, &args);
   }
   inline ERR removeCommand(int Index, int Total) noexcept {
      struct vp::RemoveCommand args = { Index, Total };
      return Action(AC(-31), this, &args);
   }
   inline ERR setCommand(int Index, struct PathCommand * Command, int Size) noexcept {
      struct vp::SetCommand args = { Index, Command, Size };
      return Action(AC(-32), this, &args);
   }
   inline ERR getCommand(int Index, struct PathCommand ** Command) noexcept {
      struct vp::GetCommand args = { Index, (struct PathCommand *)0 };
      ERR error = Action(AC(-33), this, &args);
      if (Command) *Command = args.Command;
      return error;
   }
   inline ERR setCommandList(APTR Commands, int Size) noexcept {
      struct vp::SetCommandList args = { Commands, Size };
      return Action(AC(-34), this, &args);
   }

   // Customised field getting

   inline ERR getSequence(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getTotalCommands(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPathLength(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCommands(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }


   // Customised field setting

   inline ERR setSequence(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, 0x00804308, &Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setTotalCommands(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setPathLength(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setCommands(APTR Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x00101318, Value, Elements);
   }

};

// VectorText class definition

#define VER_VECTORTEXT (1.000000)

// VectorText methods

namespace vt {
struct DeleteLine { int Line; static const AC id = AC(-30); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVectorText : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORTEXT;
   static constexpr CSTRING CLASS_NAME = "VectorText";

   using create = kt::Create<objVectorText>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR deleteLine(int Line) noexcept {
      struct vt::DeleteLine args = { Line };
      return Action(AC(-30), this, &args);
   }

   // Customised field getting

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getWeight(int &Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getString(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getAlign(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFill(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getFace(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getFontSize(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getFontStyle(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getDescent(int &Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getDisplayHeight(int &Value) noexcept {
      auto field = &this->Class->Dictionary[29];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getDisplaySize(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getDX(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[28];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getDY(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getInlineSize(double &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPoint(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getLineSpacing(int &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getRotate(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[12];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getShapeInside(OBJECTID &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getShapeSubtract(OBJECTID &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTextLength(double &Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getTextWidth(int &Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOnChange(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getFocus(OBJECTID &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCursorColumn(int &Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getCursorRow(int &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getTotalLines(int &Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSelectRow(int &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSelectColumn(int &Value) noexcept {
      auto field = &this->Class->Dictionary[26];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getLineLimit(int &Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getCharLimit(int &Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWeight(const int Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setString(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setAlign(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFill(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setFace(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, 0x00904308, &Value, 1);
   }

   inline ERR setFontSize(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00804308, &Value, 1);
   }

   inline ERR setFontStyle(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x00904508, &Value, 1);
   }

   inline ERR setDX(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[28];
      return field->WriteValue(this, field, 0x80101308, Value, Elements);
   }

   inline ERR setDY(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x80101308, Value, Elements);
   }

   inline ERR setInlineSize(const double Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setLetterSpacing(const double Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setRotate(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, 0x80101308, Value, Elements);
   }

   inline ERR setShapeInside(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setShapeSubtract(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setTextLength(const double Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setTextFlags(const int Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setStartOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSpacing(const double Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setFont(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, 0x08000409, Value, 1);
   }

   inline ERR setOnChange(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setFocus(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setCursorColumn(const int Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setCursorRow(const int Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setLineLimit(const int Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setCharLimit(const int Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorGroup class definition

#define VER_VECTORGROUP (1.000000)

class objVectorGroup : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORGROUP;
   static constexpr CSTRING CLASS_NAME = "VectorGroup";

   using create = kt::Create<objVectorGroup>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting


   // Customised field setting

};

// VectorWave class definition

#define VER_VECTORWAVE (1.000000)

class objVectorWave : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORWAVE;
   static constexpr CSTRING CLASS_NAME = "VectorWave";

   using create = kt::Create<objVectorWave>;

   // Action stubs

   inline ERR move(double X, double Y, double Z) noexcept {
      struct acMove args = { X, Y, Z };
      return Action(AC::Move, this, &args);
   }
   inline ERR moveToPoint(double X, double Y, double Z, MTF Flags) noexcept {
      struct acMoveToPoint moveto = { X, Y, Z, Flags };
      return Action(AC::MoveToPoint, this, &moveto);
   }
   inline ERR resize(double Width, double Height, double Depth = 0) noexcept {
      struct acResize args = { Width, Height, Depth };
      return Action(AC::Resize, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getAmplitude(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getClose(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDecay(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDegree(double &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDimensions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getFrequency(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getStyle(int &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getThickness(double &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }


   // Customised field setting

   inline ERR setAmplitude(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setClose(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setDecay(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setDegree(const double Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setFrequency(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setStyle(const int Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setThickness(const double Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

};

// VectorRectangle class definition

#define VER_VECTORRECTANGLE (1.000000)

class objVectorRectangle : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORRECTANGLE;
   static constexpr CSTRING CLASS_NAME = "VectorRectangle";

   using create = kt::Create<objVectorRectangle>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getRounding(double * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto get_field = (ERR (*)(APTR, double *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getRoundX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getRoundY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getXOffset(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getYOffset(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getDimensions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setRounding(const double * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x80101308, Value, Elements);
   }

   inline ERR setRoundX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setRoundY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setXOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setYOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorPolygon class definition

#define VER_VECTORPOLYGON (1.000000)

class objVectorPolygon : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPOLYGON;
   static constexpr CSTRING CLASS_NAME = "VectorPolygon";

   using create = kt::Create<objVectorPolygon>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getClosed(int &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPathLength(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPointsArray(APTR * &Value, int &Elements) noexcept {
      auto field = &this->Class->Dictionary[4];
      auto get_field = (ERR (*)(APTR, APTR *&, int &))field->GetValue;
      auto error = get_field(this, Value, Elements);
      return error;
   }

   inline ERR getTotalPoints(int &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getX1(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY1(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getX2(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getY2(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }


   // Customised field setting

   inline ERR setClosed(const int Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setPathLength(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setPointsArray(APTR * Value, int Elements) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, 0x08101308, Value, Elements);
   }

   inline ERR setPoints(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00804208, &Value, 1);
   }

   inline ERR setX1(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY1(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setX2(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY2(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

};

// VectorShape class definition

#define VER_VECTORSHAPE (1.000000)

class objVectorShape : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSHAPE;
   static constexpr CSTRING CLASS_NAME = "VectorShape";

   using create = kt::Create<objVectorShape>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getCenterX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getCenterY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getRadius(double &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getClose(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDimensions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getPhi(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getA(double &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getB(double &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getM(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getN1(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getN2(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getN3(double &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getVertices(int &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getMod(int &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSpiral(int &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getRepeat(int &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setCenterX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setClose(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setPhi(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setA(const double Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setB(const double Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setM(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setN1(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setN2(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setN3(const double Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setVertices(const int Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setMod(const int Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setSpiral(const int Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setRepeat(const int Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorSpiral class definition

#define VER_VECTORSPIRAL (1.000000)

class objVectorSpiral : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSPIRAL;
   static constexpr CSTRING CLASS_NAME = "VectorSpiral";

   using create = kt::Create<objVectorSpiral>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getPathLength(int &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getCenterX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getCenterY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getRadius(double &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getOffset(double &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getStep(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getSpacing(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getLoopLimit(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setPathLength(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setStep(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSpacing(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setLoopLimit(const double Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

// VectorEllipse class definition

#define VER_VECTORELLIPSE (1.000000)

class objVectorEllipse : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORELLIPSE;
   static constexpr CSTRING CLASS_NAME = "VectorEllipse";

   using create = kt::Create<objVectorEllipse>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getCenterX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getCenterY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getRadius(double &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getRadiusX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getRadiusY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      return error;
   }

   inline ERR getDimensions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getVertices(int &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadiusX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadiusY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setVertices(const int Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

};

// VectorViewport class definition

#define VER_VECTORVIEWPORT (1.000000)

class objVectorViewport : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORVIEWPORT;
   static constexpr CSTRING CLASS_NAME = "VectorViewport";

   using create = kt::Create<objVectorViewport>;

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR move(double X, double Y, double Z) noexcept {
      struct acMove args = { X, Y, Z };
      return Action(AC::Move, this, &args);
   }
   inline ERR moveToPoint(double X, double Y, double Z, MTF Flags) noexcept {
      struct acMoveToPoint moveto = { X, Y, Z, Flags };
      return Action(AC::MoveToPoint, this, &moveto);
   }
   inline ERR redimension(double X, double Y, double Z, double Width, double Height, double Depth) noexcept {
      struct acRedimension args = { X, Y, Z, Width, Height, Depth };
      return Action(AC::Redimension, this, &args);
   }
   inline ERR redimension(double X, double Y, double Width, double Height) noexcept {
      struct acRedimension args = { X, Y, 0, Width, Height, 0 };
      return Action(AC::Redimension, this, &args);
   }
   inline ERR resize(double Width, double Height, double Depth = 0) noexcept {
      struct acResize args = { Width, Height, Depth };
      return Action(AC::Resize, this, &args);
   }

   // Customised field getting

   inline ERR getAbsX(int &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getAbsY(int &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getAspectRatio(int &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getBuffer(OBJECTPTR &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getBuffered(int &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDimensions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getDragCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      return error;
   }

   inline ERR getOverflow(int &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getOverflowX(int &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getOverflowY(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getXOffset(double &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getYOffset(double &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      SetObjectContext(this, field, AC::NIL);
      Unit var(0, FD_DOUBLE);
      auto error = field->GetValue(this, &var);
      if (error IS ERR::Okay) Value = var.Value;
      RestoreObjectContext();
      return error;
   }

   inline ERR getViewX(double &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getViewY(double &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getViewWidth(double &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto error = field->GetValue(this, &Value);
      return error;
   }

   inline ERR getViewHeight(double &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      auto error = field->GetValue(this, &Value);
      return error;
   }


   // Customised field setting

   inline ERR setAspectRatio(const int Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setBuffered(const int Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setDragCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setOverflow(const int Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setOverflowX(const int Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setOverflowY(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setXOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setYOffset(const double Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      Unit var(Value);
      return field->WriteValue(this, field, FD_UNIT, &var, 1);
   }

   inline ERR setViewX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setViewY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setViewWidth(const double Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setViewHeight(const double Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_DOUBLE, &Value, 1);
   }

};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_VECTOR [[maybe_unused]] static struct VectorBase *VectorBase = nullptr;
#else
#define JUMPTABLE_VECTOR struct VectorBase *VectorBase = nullptr;
#endif

struct VectorBase {
#ifndef KOTUKU_STATIC
   ERR (*_DrawPath)(objBitmap *Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle);
   ERR (*_GenerateEllipse)(double CX, double CY, double RX, double RY, int Vertices, APTR *Path);
   ERR (*_GeneratePath)(const std::string_view &Sequence, APTR *Path);
   ERR (*_GenerateRectangle)(double X, double Y, double Width, double Height, APTR *Path);
   ERR (*_ReadPainter)(objVectorScene *Scene, const std::string_view &IRI, struct VectorPainter *Painter, std::string_view *Result);
   void (*_TranslatePath)(APTR Path, double X, double Y);
   void (*_MoveTo)(APTR Path, double X, double Y);
   void (*_LineTo)(APTR Path, double X, double Y);
   void (*_ArcTo)(APTR Path, double RX, double RY, double Angle, double X, double Y, ARC Flags);
   void (*_Curve3)(APTR Path, double CtrlX, double CtrlY, double X, double Y);
   void (*_Smooth3)(APTR Path, double X, double Y);
   void (*_Curve4)(APTR Path, double CtrlX1, double CtrlY1, double CtrlX2, double CtrlY2, double X, double Y);
   void (*_Smooth4)(APTR Path, double CtrlX, double CtrlY, double X, double Y);
   void (*_ClosePath)(APTR Path);
   void (*_RewindPath)(APTR Path);
   int (*_GetVertex)(APTR Path, double *X, double *Y);
   ERR (*_ApplyPath)(APTR Path, objVectorPath *VectorPath);
   ERR (*_Rotate)(struct VectorMatrix *Matrix, double Angle, double CenterX, double CenterY);
   ERR (*_Translate)(struct VectorMatrix *Matrix, double X, double Y);
   ERR (*_Skew)(struct VectorMatrix *Matrix, double X, double Y);
   ERR (*_Multiply)(struct VectorMatrix *Matrix, double ScaleX, double ShearY, double ShearX, double ScaleY, double TranslateX, double TranslateY);
   ERR (*_MultiplyMatrix)(struct VectorMatrix *Target, struct VectorMatrix *Source);
   ERR (*_Scale)(struct VectorMatrix *Matrix, double X, double Y);
   ERR (*_ParseTransform)(struct VectorMatrix *Matrix, const std::string_view &Transform);
   ERR (*_ResetMatrix)(struct VectorMatrix *Matrix);
   ERR (*_GetFontHandle)(const std::string_view &Family, const std::string_view &Style, int Weight, int Size, APTR *Handle);
   ERR (*_GetFontMetrics)(APTR Handle, struct FontMetrics *Info);
   double (*_CharWidth)(APTR FontHandle, uint32_t Char, uint32_t KChar, double *Kerning);
   double (*_StringWidth)(APTR FontHandle, const std::string_view &String, int Chars);
   ERR (*_FlushMatrix)(struct VectorMatrix *Matrix);
   ERR (*_TracePath)(APTR Path, FUNCTION *Callback, double Scale);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_VECTOR_MODULE)
extern struct VectorBase *VectorBase;
namespace vec {
inline ERR DrawPath(objBitmap *Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle) { return VectorBase->_DrawPath(Bitmap,Path,StrokeWidth,StrokeStyle,FillStyle); }
inline ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path) { return VectorBase->_GenerateEllipse(CX,CY,RX,RY,Vertices,Path); }
inline ERR GeneratePath(const std::string_view &Sequence, APTR *Path) { return VectorBase->_GeneratePath(Sequence,Path); }
inline ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path) { return VectorBase->_GenerateRectangle(X,Y,Width,Height,Path); }
inline ERR ReadPainter(objVectorScene *Scene, const std::string_view &IRI, struct VectorPainter *Painter, std::string_view *Result) { return VectorBase->_ReadPainter(Scene,IRI,Painter,Result); }
inline void TranslatePath(APTR Path, double X, double Y) { return VectorBase->_TranslatePath(Path,X,Y); }
inline void MoveTo(APTR Path, double X, double Y) { return VectorBase->_MoveTo(Path,X,Y); }
inline void LineTo(APTR Path, double X, double Y) { return VectorBase->_LineTo(Path,X,Y); }
inline void ArcTo(APTR Path, double RX, double RY, double Angle, double X, double Y, ARC Flags) { return VectorBase->_ArcTo(Path,RX,RY,Angle,X,Y,Flags); }
inline void Curve3(APTR Path, double CtrlX, double CtrlY, double X, double Y) { return VectorBase->_Curve3(Path,CtrlX,CtrlY,X,Y); }
inline void Smooth3(APTR Path, double X, double Y) { return VectorBase->_Smooth3(Path,X,Y); }
inline void Curve4(APTR Path, double CtrlX1, double CtrlY1, double CtrlX2, double CtrlY2, double X, double Y) { return VectorBase->_Curve4(Path,CtrlX1,CtrlY1,CtrlX2,CtrlY2,X,Y); }
inline void Smooth4(APTR Path, double CtrlX, double CtrlY, double X, double Y) { return VectorBase->_Smooth4(Path,CtrlX,CtrlY,X,Y); }
inline void ClosePath(APTR Path) { return VectorBase->_ClosePath(Path); }
inline void RewindPath(APTR Path) { return VectorBase->_RewindPath(Path); }
inline int GetVertex(APTR Path, double *X, double *Y) { return VectorBase->_GetVertex(Path,X,Y); }
inline ERR ApplyPath(APTR Path, objVectorPath *VectorPath) { return VectorBase->_ApplyPath(Path,VectorPath); }
inline ERR Rotate(struct VectorMatrix *Matrix, double Angle, double CenterX, double CenterY) { return VectorBase->_Rotate(Matrix,Angle,CenterX,CenterY); }
inline ERR Translate(struct VectorMatrix *Matrix, double X, double Y) { return VectorBase->_Translate(Matrix,X,Y); }
inline ERR Skew(struct VectorMatrix *Matrix, double X, double Y) { return VectorBase->_Skew(Matrix,X,Y); }
inline ERR Multiply(struct VectorMatrix *Matrix, double ScaleX, double ShearY, double ShearX, double ScaleY, double TranslateX, double TranslateY) { return VectorBase->_Multiply(Matrix,ScaleX,ShearY,ShearX,ScaleY,TranslateX,TranslateY); }
inline ERR MultiplyMatrix(struct VectorMatrix *Target, struct VectorMatrix *Source) { return VectorBase->_MultiplyMatrix(Target,Source); }
inline ERR Scale(struct VectorMatrix *Matrix, double X, double Y) { return VectorBase->_Scale(Matrix,X,Y); }
inline ERR ParseTransform(struct VectorMatrix *Matrix, const std::string_view &Transform) { return VectorBase->_ParseTransform(Matrix,Transform); }
inline ERR ResetMatrix(struct VectorMatrix *Matrix) { return VectorBase->_ResetMatrix(Matrix); }
inline ERR GetFontHandle(const std::string_view &Family, const std::string_view &Style, int Weight, int Size, APTR *Handle) { return VectorBase->_GetFontHandle(Family,Style,Weight,Size,Handle); }
inline ERR GetFontMetrics(APTR Handle, struct FontMetrics *Info) { return VectorBase->_GetFontMetrics(Handle,Info); }
inline double CharWidth(APTR FontHandle, uint32_t Char, uint32_t KChar, double *Kerning) { return VectorBase->_CharWidth(FontHandle,Char,KChar,Kerning); }
inline double StringWidth(APTR FontHandle, const std::string_view &String, int Chars) { return VectorBase->_StringWidth(FontHandle,String,Chars); }
inline ERR FlushMatrix(struct VectorMatrix *Matrix) { return VectorBase->_FlushMatrix(Matrix); }
inline ERR TracePath(APTR Path, FUNCTION *Callback, double Scale) { return VectorBase->_TracePath(Path,Callback,Scale); }
} // namespace
#else
namespace vec {
extern ERR DrawPath(objBitmap *Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle);
extern ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path);
extern ERR GeneratePath(const std::string_view &Sequence, APTR *Path);
extern ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path);
extern ERR ReadPainter(objVectorScene *Scene, const std::string_view &IRI, struct VectorPainter *Painter, std::string_view *Result);
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
extern int GetVertex(APTR Path, double *X, double *Y);
extern ERR ApplyPath(APTR Path, objVectorPath *VectorPath);
extern ERR Rotate(struct VectorMatrix *Matrix, double Angle, double CenterX, double CenterY);
extern ERR Translate(struct VectorMatrix *Matrix, double X, double Y);
extern ERR Skew(struct VectorMatrix *Matrix, double X, double Y);
extern ERR Multiply(struct VectorMatrix *Matrix, double ScaleX, double ShearY, double ShearX, double ScaleY, double TranslateX, double TranslateY);
extern ERR MultiplyMatrix(struct VectorMatrix *Target, struct VectorMatrix *Source);
extern ERR Scale(struct VectorMatrix *Matrix, double X, double Y);
extern ERR ParseTransform(struct VectorMatrix *Matrix, const std::string_view &Transform);
extern ERR ResetMatrix(struct VectorMatrix *Matrix);
extern ERR GetFontHandle(const std::string_view &Family, const std::string_view &Style, int Weight, int Size, APTR *Handle);
extern ERR GetFontMetrics(APTR Handle, struct FontMetrics *Info);
extern double CharWidth(APTR FontHandle, uint32_t Char, uint32_t KChar, double *Kerning);
extern double StringWidth(APTR FontHandle, const std::string_view &String, int Chars);
extern ERR FlushMatrix(struct VectorMatrix *Matrix);
extern ERR TracePath(APTR Path, FUNCTION *Callback, double Scale);
} // namespace
#endif // KOTUKU_STATIC


//********************************************************************************************************************

inline void operator*=(VectorMatrix &This, const VectorMatrix &Other)
{
   double t0 = This.ScaleX * Other.ScaleX + This.ShearY * Other.ShearX;
   double t2 = This.ShearX * Other.ScaleX + This.ScaleY * Other.ShearX;
   double t4 = This.TranslateX * Other.ScaleX + This.TranslateY * Other.ShearX + Other.TranslateX;
   This.ShearY     = This.ScaleX * Other.ShearY + This.ShearY * Other.ScaleY;
   This.ScaleY     = This.ShearX * Other.ShearY + This.ScaleY * Other.ScaleY;
   This.TranslateY = This.TranslateX  * Other.ShearY + This.TranslateY * Other.ScaleY + Other.TranslateY;
   This.ScaleX     = t0;
   This.ShearX     = t2;
   This.TranslateX = t4;
}

//********************************************************************************************************************

inline void SET_VECTOR_COLOUR(objVectorColour *Colour, double Red, double Green, double Blue, double Alpha) {
   Colour->Class->ClassID = CLASSID::VECTORCOLOUR;
   Colour->Red   = Red;
   Colour->Green = Green;
   Colour->Blue  = Blue;
   Colour->Alpha = Alpha;
}
namespace vec {
inline ERR SubscribeInput(APTR Ob, JTYPE Mask, FUNCTION Callback) {
   struct SubscribeInput args = { Mask, &Callback };
   return(Action(vec::SubscribeInput::id, (OBJECTPTR)Ob, &args));
}

inline ERR SubscribeKeyboard(APTR Ob, FUNCTION Callback) {
   struct SubscribeKeyboard args = { &Callback };
   return(Action(vec::SubscribeKeyboard::id, (OBJECTPTR)Ob, &args));
}

inline ERR SubscribeFeedback(APTR Ob, FM Mask, FUNCTION Callback) {
   struct SubscribeFeedback args = { Mask, &Callback };
   return(Action(vec::SubscribeFeedback::id, (OBJECTPTR)Ob, &args));
}
} // namespace

namespace fl {
   using namespace kt;

constexpr FieldValue Flags(VCLF Value) { return FieldValue(FID_Flags, int(Value)); }

constexpr FieldValue AppendPath(OBJECTPTR Value) { return FieldValue(FID_AppendPath, Value); }

constexpr FieldValue DragCallback(const FUNCTION &Value) { return FieldValue(FID_DragCallback, &Value); }
constexpr FieldValue DragCallback(const FUNCTION *Value) { return FieldValue(FID_DragCallback, Value); }

constexpr FieldValue OnChange(const FUNCTION &Value) { return FieldValue(FID_OnChange, &Value); }
constexpr FieldValue OnChange(const FUNCTION *Value) { return FieldValue(FID_OnChange, Value); }

constexpr FieldValue TextFlags(VTXF Value) { return FieldValue(FID_TextFlags, int(Value)); }
constexpr FieldValue Overflow(VOF Value) { return FieldValue(FID_Overflow, int(Value)); }

[[nodiscard]] inline FieldValue Sequence(std::string_view Value) { return FieldValue(FID_Sequence, Value); }
[[nodiscard]] constexpr FieldValue Sequence(CSTRING Value) { return FieldValue(FID_Sequence, Value ? std::string_view(Value) : std::string_view{}); }

[[nodiscard]] inline FieldValue FontStyle(std::string_view Value) { return FieldValue(FID_FontStyle, Value); }
[[nodiscard]] constexpr FieldValue FontStyle(CSTRING Value) { return FieldValue(FID_FontStyle, Value ? std::string_view(Value) : std::string_view{}); }

template <kt::NumericOrScale T> FieldValue RoundX(T Value) { return FieldValue(FID_RoundX, Value); }
template <kt::NumericOrScale T> FieldValue RoundY(T Value) { return FieldValue(FID_RoundY, Value); }

}

