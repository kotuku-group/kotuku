#pragma once

// Name:      vector.h
// Copyright: Paul Manias © 2010-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_VECTOR (1)

#include <kotuku/modules/display.h>
#include <kotuku/modules/picture.h>

class objVectorColour;
class objVectorTransition;
class objVectorScene;
class objVectorImage;
class objVectorPattern;
class objVectorGradient;
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

// VectorGradient options.

enum class VGT : int {
   NIL = 0,
   LINEAR = 0,
   RADIAL = 1,
   CONIC = 2,
   DIAMOND = 3,
   CONTOUR = 4,
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
   CONTAIN_FOCAL = 0x00100000,
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
   objVectorGradient * Gradient;  // A VectorGradient object, suitable for gradient fills.
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

#define MTAG_ANIMATE_MOTION 0xe7adf794
#define MTAG_ANIMATE_TRANSFORM 0xa8348253
#define MTAG_SCENE_GRAPH 0x0be1a484
#define MTAG_USE_TRANSFORM 0x3629f654
#define MTAG_SVG_TRANSFORM 0x4b7c7a89

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

   using create = pf::Create<objVectorColour>;

   double Red;    // The red component value.
   double Green;  // The green component value.
   double Blue;   // The blue component value.
   double Alpha;  // The alpha component value.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

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

   using create = pf::Create<objVectorTransition>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setStops(const APTR Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Stops, &target);
      return field->WriteValue(target, field, 0x00001218, Value, Elements);
   }

};

// VectorScene class definition

#define VER_VECTORSCENE (1.000000)

// VectorScene methods

namespace sc {
struct AddDef { CSTRING Name; OBJECTPTR Def; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SearchByID { int ID; OBJECTPTR Result; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct FindDef { CSTRING Name; OBJECTPTR Def; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Debug { static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVectorScene : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSCENE;
   static constexpr CSTRING CLASS_NAME = "VectorScene";

   using create = pf::Create<objVectorScene>;

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
   inline ERR addDef(CSTRING Name, OBJECTPTR Def) noexcept {
      struct sc::AddDef args = { Name, Def };
      return(Action(AC(-1), this, &args));
   }
   inline ERR searchByID(int ID, OBJECTPTR * Result) noexcept {
      struct sc::SearchByID args = { ID, (OBJECTPTR)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR findDef(CSTRING Name, OBJECTPTR * Def) noexcept {
      struct sc::FindDef args = { Name, (OBJECTPTR)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Def) *Def = args.Def;
      return(error);
   }
   inline ERR debug() noexcept {
      return(Action(AC(-4), this, nullptr));
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
      OBJECTPTR target;
      auto field = FindField(this, FID_Bitmap, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setSurface(OBJECTID Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Surface, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VPF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setPageWidth(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PageWidth, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPageHeight(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PageHeight, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setSampleMethod(const VSM Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SampleMethod, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorImage class definition

#define VER_VECTORIMAGE (1.000000)

class objVectorImage : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORIMAGE;
   static constexpr CSTRING CLASS_NAME = "VectorImage";

   using create = pf::Create<objVectorImage>;

   double  X;               // Apply a horizontal offset to the image, the origin of which is determined by the Units value.
   double  Y;               // Apply a vertical offset to the image, the origin of which is determined by the Units value.
   objPicture * Picture;    // Refers to a Picture from which the source Bitmap is acquired.
   objBitmap * Bitmap;      // Reference to a source bitmap for the rendering algorithm.
   VUNIT   Units;           // Declares the coordinate system to use for the X and Y values.
   DMF     Dimensions;      // Dimension flags define whether individual dimension fields contain fixed or scaled values.
   VSPREAD SpreadMethod;    // Defines image tiling behaviour, if desired.
   ARF     AspectRatio;     // Flags that affect the aspect ratio of the image within its target vector.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setPicture(objPicture * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Picture, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setBitmap(objBitmap * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Bitmap, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setUnits(const VUNIT Value) noexcept {
      this->Units = Value;
      return ERR::Okay;
   }

   inline ERR setDimensions(const DMF Value) noexcept {
      this->Dimensions = Value;
      return ERR::Okay;
   }

   inline ERR setSpreadMethod(const VSPREAD Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SpreadMethod, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setAspectRatio(const ARF Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_AspectRatio, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorPattern class definition

#define VER_VECTORPATTERN (1.000000)

class objVectorPattern : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPATTERN;
   static constexpr CSTRING CLASS_NAME = "VectorPattern";

   using create = pf::Create<objVectorPattern>;

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

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Opacity, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setInherit(objVectorPattern * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Inherit, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setSpreadMethod(const VSPREAD Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SpreadMethod, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
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
      OBJECTPTR target;
      auto field = FindField(this, FID_Matrices, &target);
      return field->WriteValue(target, field, 0x08000318, Value, 1);
   }

   template <class T> inline ERR setTransform(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Transform, &target);
      return field->WriteValue(target, field, 0x08800208, to_cstring(Value), 1);
   }

};

// VectorGradient class definition

#define VER_VECTORGRADIENT (1.000000)

class objVectorGradient : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORGRADIENT;
   static constexpr CSTRING CLASS_NAME = "VectorGradient";

   using create = pf::Create<objVectorGradient>;

   double  X1;            // Initial X coordinate for the gradient.
   double  Y1;            // Initial Y coordinate for the gradient.
   double  X2;            // Final X coordinate for the gradient.
   double  Y2;            // Final Y coordinate for the gradient.
   double  CenterX;       // The horizontal center point of the gradient.
   double  CenterY;       // The vertical center point of the gradient.
   double  FocalX;        // The horizontal focal point for radial gradients.
   double  FocalY;        // The vertical focal point for radial gradients.
   double  Radius;        // The radius of the gradient.
   double  FocalRadius;   // The size of the focal radius for radial gradients.
   double  Resolution;    // Affects the rate of change for colours in the gradient.
   VSPREAD SpreadMethod;  // Determines the rendering behaviour to use when gradient colours are cycled.
   VUNIT   Units;         // Defines the coordinate system for X1, Y1, X2 and Y2.
   VGT     Type;          // Specifies the type of gradient (e.g. RADIAL, LINEAR)
   VGF     Flags;         // Dimension flags are stored here.
   VCS     ColourSpace;   // Defines the colour space to use when interpolating gradient colours.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setX1(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X1, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY1(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y1, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setX2(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X2, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY2(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y2, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setFocalX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FocalX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setFocalY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FocalY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Radius, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setFocalRadius(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FocalRadius, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setResolution(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Resolution, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSpreadMethod(const VSPREAD Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SpreadMethod, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setUnits(const VUNIT Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Units = Value;
      return ERR::Okay;
   }

   inline ERR setType(const VGT Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Type, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VGF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setColourSpace(const VCS Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->ColourSpace = Value;
      return ERR::Okay;
   }

   inline ERR setColour(const float * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Colour, &target);
      return field->WriteValue(target, field, 0x10001308, Value, Elements);
   }

   template <class T> inline ERR setColourMap(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ColourMap, &target);
      return field->WriteValue(target, field, 0x08800208, to_cstring(Value), 1);
   }

   inline ERR setMatrices(APTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Matrices, &target);
      return field->WriteValue(target, field, 0x08000318, Value, 1);
   }

   inline ERR setNumeric(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Numeric, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setID(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ID, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setStops(const APTR Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Stops, &target);
      return field->WriteValue(target, field, 0x00001318, Value, Elements);
   }

   template <class T> inline ERR setTransform(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Transform, &target);
      return field->WriteValue(target, field, 0x08800208, to_cstring(Value), 1);
   }

};

// FilterEffect class definition

#define VER_FILTEREFFECT (1.000000)

class objFilterEffect : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FILTEREFFECT;
   static constexpr CSTRING CLASS_NAME = "FilterEffect";

   using create = pf::Create<objFilterEffect>;

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
      OBJECTPTR target;
      auto field = FindField(this, FID_Input, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setMix(objFilterEffect * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Mix, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
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

   using create = pf::Create<objImageFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   template <class T> inline ERR setPath(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Path, &target);
      return field->WriteValue(target, field, 0x08800508, to_cstring(Value), 1);
   }

   inline ERR setAspectRatio(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_AspectRatio, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setResampleMethod(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ResampleMethod, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// SourceFX class definition

#define VER_SOURCEFX (1.000000)

class objSourceFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SOURCEFX;
   static constexpr CSTRING CLASS_NAME = "SourceFX";

   using create = pf::Create<objSourceFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setAspectRatio(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_AspectRatio, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setSourceName(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SourceName, &target);
      return field->WriteValue(target, field, 0x08800408, to_cstring(Value), 1);
   }

   inline ERR setSource(OBJECTPTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Source, &target);
      return field->WriteValue(target, field, 0x08000109, Value, 1);
   }

};

// BlurFX class definition

#define VER_BLURFX (1.000000)

class objBlurFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::BLURFX;
   static constexpr CSTRING CLASS_NAME = "BlurFX";

   using create = pf::Create<objBlurFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setSX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SX, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SY, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

};

// ColourFX class definition

#define VER_COLOURFX (1.000000)

class objColourFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COLOURFX;
   static constexpr CSTRING CLASS_NAME = "ColourFX";

   using create = pf::Create<objColourFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setMode(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Mode, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setValues(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Values, &target);
      return field->WriteValue(target, field, 0x80001508, Value, Elements);
   }

};

// CompositeFX class definition

#define VER_COMPOSITEFX (1.000000)

class objCompositeFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPOSITEFX;
   static constexpr CSTRING CLASS_NAME = "CompositeFX";

   using create = pf::Create<objCompositeFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setOperator(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Operator, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setK1(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_K1, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setK2(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_K2, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setK3(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_K3, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setK4(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_K4, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

};

// ConvolveFX class definition

#define VER_CONVOLVEFX (1.000000)

class objConvolveFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONVOLVEFX;
   static constexpr CSTRING CLASS_NAME = "ConvolveFX";

   using create = pf::Create<objConvolveFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setBias(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Bias, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setDivisor(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Divisor, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setEdgeMode(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_EdgeMode, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setMatrixRows(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_MatrixRows, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setMatrixColumns(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_MatrixColumns, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setMatrix(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Matrix, &target);
      return field->WriteValue(target, field, 0x80001508, Value, Elements);
   }

   inline ERR setPreserveAlpha(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PreserveAlpha, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setTargetX(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_TargetX, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setTargetY(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_TargetY, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setUnitX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_UnitX, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setUnitY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_UnitY, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

};

// DisplacementFX class definition

#define VER_DISPLACEMENTFX (1.000000)

class objDisplacementFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DISPLACEMENTFX;
   static constexpr CSTRING CLASS_NAME = "DisplacementFX";

   using create = pf::Create<objDisplacementFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setScale(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Scale, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setXChannel(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_XChannel, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setYChannel(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_YChannel, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// FloodFX class definition

#define VER_FLOODFX (1.000000)

class objFloodFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FLOODFX;
   static constexpr CSTRING CLASS_NAME = "FloodFX";

   using create = pf::Create<objFloodFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setColour(const float * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Colour, &target);
      return field->WriteValue(target, field, 0x10001308, Value, Elements);
   }

   inline ERR setOpacity(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Opacity, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
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

   using create = pf::Create<objLightingFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR setDistantLight(double Azimuth, double Elevation) noexcept {
      struct lt::SetDistantLight args = { Azimuth, Elevation };
      return(Action(AC(-20), this, &args));
   }
   inline ERR setPointLight(double X, double Y, double Z) noexcept {
      struct lt::SetPointLight args = { X, Y, Z };
      return(Action(AC(-22), this, &args));
   }
   inline ERR setSpotLight(double X, double Y, double Z, double PX, double PY, double PZ, double Exponent, double ConeAngle) noexcept {
      struct lt::SetSpotLight args = { X, Y, Z, PX, PY, PZ, Exponent, ConeAngle };
      return(Action(AC(-21), this, &args));
   }

   // Customised field setting

   inline ERR setColour(const float * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Colour, &target);
      return field->WriteValue(target, field, 0x10001308, Value, Elements);
   }

   inline ERR setConstant(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Constant, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setExponent(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Exponent, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setScale(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Scale, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setType(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Type, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setUnitX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_UnitX, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setUnitY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_UnitY, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

};

// MergeFX class definition

#define VER_MERGEFX (1.000000)

class objMergeFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MERGEFX;
   static constexpr CSTRING CLASS_NAME = "MergeFX";

   using create = pf::Create<objMergeFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setSourceList(const APTR Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_SourceList, &target);
      return field->WriteValue(target, field, 0x00001318, Value, Elements);
   }

};

// MorphologyFX class definition

#define VER_MORPHOLOGYFX (1.000000)

class objMorphologyFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MORPHOLOGYFX;
   static constexpr CSTRING CLASS_NAME = "MorphologyFX";

   using create = pf::Create<objMorphologyFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setOperator(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Operator, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setRadiusX(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_RadiusX, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setRadiusY(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_RadiusY, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// OffsetFX class definition

#define VER_OFFSETFX (1.000000)

class objOffsetFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::OFFSETFX;
   static constexpr CSTRING CLASS_NAME = "OffsetFX";

   using create = pf::Create<objOffsetFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setXOffset(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_XOffset, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setYOffset(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_YOffset, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// RemapFX class definition

#define VER_REMAPFX (1.000000)

// RemapFX methods

namespace rf {
struct SelectGamma { CMP Component; double Amplitude; double Offset; double Exponent; static const AC id = AC(-20); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectTable { CMP Component; double * Values; int Size; static const AC id = AC(-21); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectLinear { CMP Component; double Slope; double Intercept; static const AC id = AC(-22); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectIdentity { CMP Component; static const AC id = AC(-23); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectDiscrete { CMP Component; double * Values; int Size; static const AC id = AC(-24); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectInvert { CMP Component; static const AC id = AC(-25); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectMask { CMP Component; int Mask; static const AC id = AC(-26); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objRemapFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::REMAPFX;
   static constexpr CSTRING CLASS_NAME = "RemapFX";

   using create = pf::Create<objRemapFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR selectGamma(CMP Component, double Amplitude, double Offset, double Exponent) noexcept {
      struct rf::SelectGamma args = { Component, Amplitude, Offset, Exponent };
      return(Action(AC(-20), this, &args));
   }
   inline ERR selectTable(CMP Component, double * Values, int Size) noexcept {
      struct rf::SelectTable args = { Component, Values, Size };
      return(Action(AC(-21), this, &args));
   }
   inline ERR selectLinear(CMP Component, double Slope, double Intercept) noexcept {
      struct rf::SelectLinear args = { Component, Slope, Intercept };
      return(Action(AC(-22), this, &args));
   }
   inline ERR selectIdentity(CMP Component) noexcept {
      struct rf::SelectIdentity args = { Component };
      return(Action(AC(-23), this, &args));
   }
   inline ERR selectDiscrete(CMP Component, double * Values, int Size) noexcept {
      struct rf::SelectDiscrete args = { Component, Values, Size };
      return(Action(AC(-24), this, &args));
   }
   inline ERR selectInvert(CMP Component) noexcept {
      struct rf::SelectInvert args = { Component };
      return(Action(AC(-25), this, &args));
   }
   inline ERR selectMask(CMP Component, int Mask) noexcept {
      struct rf::SelectMask args = { Component, Mask };
      return(Action(AC(-26), this, &args));
   }

   // Customised field setting

};

// TurbulenceFX class definition

#define VER_TURBULENCEFX (1.000000)

class objTurbulenceFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TURBULENCEFX;
   static constexpr CSTRING CLASS_NAME = "TurbulenceFX";

   using create = pf::Create<objTurbulenceFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setFX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FX, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setFY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FY, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setOctaves(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Octaves, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setSeed(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Seed, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setStitch(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Stitch, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setType(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Type, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// WaveFunctionFX class definition

#define VER_WAVEFUNCTIONFX (1.000000)

class objWaveFunctionFX : public objFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WAVEFUNCTIONFX;
   static constexpr CSTRING CLASS_NAME = "WaveFunctionFX";

   using create = pf::Create<objWaveFunctionFX>;

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setAspectRatio(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_AspectRatio, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setColourMap(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ColourMap, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setN(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_N, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setL(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_L, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setM(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_M, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setResolution(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Resolution, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setScale(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Scale, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setStops(const APTR Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Stops, &target);
      return field->WriteValue(target, field, 0x00001318, Value, Elements);
   }

};

// VectorClip class definition

#define VER_VECTORCLIP (1.000000)

class objVectorClip : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORCLIP;
   static constexpr CSTRING CLASS_NAME = "VectorClip";

   using create = pf::Create<objVectorClip>;

   objVectorViewport * Viewport;    // This viewport hosts the Vector objects that will contribute to the clip path.
   VUNIT Units;                     // Defines the coordinate system for fields X, Y, Width and Height.
   VCLF  Flags;                     // Optional flags.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setUnits(const VUNIT Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Units, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VCLF Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Flags, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorFilter class definition

#define VER_VECTORFILTER (1.000000)

class objVectorFilter : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORFILTER;
   static constexpr CSTRING CLASS_NAME = "VectorFilter";

   using create = pf::Create<objVectorFilter>;

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

   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Opacity, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setInherit(objVectorFilter * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Inherit, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
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
struct Trace { FUNCTION * Callback; double Scale; int Transform; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetBoundary { VBF Flags; double X; double Y; double Width; double Height; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct PointInPath { double X; double Y; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SubscribeInput { JTYPE Mask; FUNCTION * Callback; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SubscribeKeyboard { FUNCTION * Callback; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SubscribeFeedback { FM Mask; FUNCTION * Callback; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Debug { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct NewMatrix { struct VectorMatrix * Transform; int End; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct FreeMatrix { struct VectorMatrix * Matrix; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVector : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTOR;
   static constexpr CSTRING CLASS_NAME = "Vector";

   using create = pf::Create<objVector>;

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
      return(Action(AC(-1), this, &args));
   }
   inline ERR trace(FUNCTION Callback, double Scale, int Transform) noexcept {
      struct vec::Trace args = { &Callback, Scale, Transform };
      return(Action(AC(-2), this, &args));
   }
   inline ERR getBoundary(VBF Flags, double * X, double * Y, double * Width, double * Height) noexcept {
      struct vec::GetBoundary args = { Flags, (double)0, (double)0, (double)0, (double)0 };
      ERR error = Action(AC(-3), this, &args);
      if (X) *X = args.X;
      if (Y) *Y = args.Y;
      if (Width) *Width = args.Width;
      if (Height) *Height = args.Height;
      return(error);
   }
   inline ERR pointInPath(double X, double Y) noexcept {
      struct vec::PointInPath args = { X, Y };
      return(Action(AC(-4), this, &args));
   }
   inline ERR subscribeInput(JTYPE Mask, FUNCTION Callback) noexcept {
      struct vec::SubscribeInput args = { Mask, &Callback };
      return(Action(AC(-5), this, &args));
   }
   inline ERR subscribeKeyboard(FUNCTION Callback) noexcept {
      struct vec::SubscribeKeyboard args = { &Callback };
      return(Action(AC(-6), this, &args));
   }
   inline ERR subscribeFeedback(FM Mask, FUNCTION Callback) noexcept {
      struct vec::SubscribeFeedback args = { Mask, &Callback };
      return(Action(AC(-7), this, &args));
   }
   inline ERR debug() noexcept {
      return(Action(AC(-8), this, nullptr));
   }
   inline ERR newMatrix(struct VectorMatrix ** Transform, int End) noexcept {
      struct vec::NewMatrix args = { (struct VectorMatrix *)0, End };
      ERR error = Action(AC(-9), this, &args);
      if (Transform) *Transform = args.Transform;
      return(error);
   }
   inline ERR freeMatrix(struct VectorMatrix * Matrix) noexcept {
      struct vec::FreeMatrix args = { Matrix };
      return(Action(AC(-10), this, &args));
   }

   // Customised field setting

   inline ERR setNext(objVector * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Next, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setPrev(objVector * Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Prev, &target);
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setStrokeOpacity(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_StrokeOpacity, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setFillOpacity(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FillOpacity, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Opacity, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setMiterLimit(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_MiterLimit, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setInnerMiterLimit(const double Value) noexcept {
      this->InnerMiterLimit = Value;
      return ERR::Okay;
   }

   inline ERR setDashOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_DashOffset, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setVisibility(const VIS Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Visibility, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFlags(const VF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setCursor(const PTC Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Cursor, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
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
      OBJECTPTR target;
      auto field = FindField(this, FID_ClipRule, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setDashArray(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_DashArray, &target);
      return field->WriteValue(target, field, 0x80001308, Value, Elements);
   }

   inline ERR setMask(OBJECTPTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Mask, &target);
      return field->WriteValue(target, field, 0x08000309, Value, 1);
   }

   inline ERR setMorph(OBJECTPTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Morph, &target);
      return field->WriteValue(target, field, 0x08000309, Value, 1);
   }

   inline ERR setAppendPath(OBJECTPTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_AppendPath, &target);
      return field->WriteValue(target, field, 0x08000309, Value, 1);
   }

   inline ERR setMorphFlags(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_MorphFlags, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setNumeric(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Numeric, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setResizeEvent(const FUNCTION Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ResizeEvent, &target);
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setStroke(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Stroke, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setStrokeColour(const float * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_StrokeColour, &target);
      return field->WriteValue(target, field, 0x10001308, Value, Elements);
   }

   inline ERR setStrokeWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_StrokeWidth, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setTransition(OBJECTPTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Transition, &target);
      return field->WriteValue(target, field, 0x08000309, Value, 1);
   }

   template <class T> inline ERR setFill(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Fill, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setFillColour(const float * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FillColour, &target);
      return field->WriteValue(target, field, 0x10001308, Value, Elements);
   }

   inline ERR setFillRule(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FillRule, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setFilter(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Filter, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setLineJoin(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_LineJoin, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setLineCap(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_LineCap, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setInnerJoin(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_InnerJoin, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setTabOrder(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_TabOrder, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorPath class definition

#define VER_VECTORPATH (1.000000)

// VectorPath methods

namespace vp {
struct AddCommand { struct PathCommand * Commands; int Size; static const AC id = AC(-30); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveCommand { int Index; int Total; static const AC id = AC(-31); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetCommand { int Index; struct PathCommand * Command; int Size; static const AC id = AC(-32); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetCommand { int Index; struct PathCommand * Command; static const AC id = AC(-33); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetCommandList { APTR Commands; int Size; static const AC id = AC(-34); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objVectorPath : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPATH;
   static constexpr CSTRING CLASS_NAME = "VectorPath";

   using create = pf::Create<objVectorPath>;

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR addCommand(struct PathCommand * Commands, int Size) noexcept {
      struct vp::AddCommand args = { Commands, Size };
      return(Action(AC(-30), this, &args));
   }
   inline ERR removeCommand(int Index, int Total) noexcept {
      struct vp::RemoveCommand args = { Index, Total };
      return(Action(AC(-31), this, &args));
   }
   inline ERR setCommand(int Index, struct PathCommand * Command, int Size) noexcept {
      struct vp::SetCommand args = { Index, Command, Size };
      return(Action(AC(-32), this, &args));
   }
   inline ERR getCommand(int Index, struct PathCommand ** Command) noexcept {
      struct vp::GetCommand args = { Index, (struct PathCommand *)0 };
      ERR error = Action(AC(-33), this, &args);
      if (Command) *Command = args.Command;
      return(error);
   }
   inline ERR setCommandList(APTR Commands, int Size) noexcept {
      struct vp::SetCommandList args = { Commands, Size };
      return(Action(AC(-34), this, &args));
   }

   // Customised field setting

   template <class T> inline ERR setSequence(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Sequence, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setTotalCommands(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_TotalCommands, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPathLength(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PathLength, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setCommands(const APTR Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Commands, &target);
      return field->WriteValue(target, field, 0x00001318, Value, Elements);
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

   using create = pf::Create<objVectorText>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR deleteLine(int Line) noexcept {
      struct vt::DeleteLine args = { Line };
      return(Action(AC(-30), this, &args));
   }

   // Customised field setting

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWeight(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Weight, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setString(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_String, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   inline ERR setAlign(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Align, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setFace(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Face, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   template <class T> inline ERR setFill(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Fill, &target);
      return field->WriteValue(target, field, 0x08800308, to_cstring(Value), 1);
   }

   template <class T> inline ERR setFontSize(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FontSize, &target);
      return field->WriteValue(target, field, 0x08800328, to_cstring(Value), 1);
   }

   template <class T> inline ERR setFontStyle(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_FontStyle, &target);
      return field->WriteValue(target, field, 0x08800508, to_cstring(Value), 1);
   }

   inline ERR setDX(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_DX, &target);
      return field->WriteValue(target, field, 0x80001308, Value, Elements);
   }

   inline ERR setDY(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_DY, &target);
      return field->WriteValue(target, field, 0x80001308, Value, Elements);
   }

   inline ERR setInlineSize(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_InlineSize, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setLetterSpacing(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_LetterSpacing, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setRotate(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Rotate, &target);
      return field->WriteValue(target, field, 0x80001308, Value, Elements);
   }

   inline ERR setShapeInside(OBJECTID Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ShapeInside, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setShapeSubtract(OBJECTID Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ShapeSubtract, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setTextLength(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_TextLength, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setTextFlags(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_TextFlags, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setStartOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_StartOffset, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSpacing(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Spacing, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setFont(OBJECTPTR Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Font, &target);
      return field->WriteValue(target, field, 0x08000409, Value, 1);
   }

   inline ERR setOnChange(FUNCTION Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_OnChange, &target);
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setFocus(OBJECTID Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Focus, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setCursorColumn(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CursorColumn, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setCursorRow(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CursorRow, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setLineLimit(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_LineLimit, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setCharLimit(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CharLimit, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorGroup class definition

#define VER_VECTORGROUP (1.000000)

class objVectorGroup : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORGROUP;
   static constexpr CSTRING CLASS_NAME = "VectorGroup";

   using create = pf::Create<objVectorGroup>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

};

// VectorWave class definition

#define VER_VECTORWAVE (1.000000)

class objVectorWave : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORWAVE;
   static constexpr CSTRING CLASS_NAME = "VectorWave";

   using create = pf::Create<objVectorWave>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setAmplitude(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Amplitude, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setClose(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Close, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setDecay(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Decay, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setDegree(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Degree, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Dimensions, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFrequency(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Frequency, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setStyle(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Style, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setThickness(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Thickness, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

};

// VectorRectangle class definition

#define VER_VECTORRECTANGLE (1.000000)

class objVectorRectangle : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORRECTANGLE;
   static constexpr CSTRING CLASS_NAME = "VectorRectangle";

   using create = pf::Create<objVectorRectangle>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setRounding(const double * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Rounding, &target);
      return field->WriteValue(target, field, 0x80001308, Value, Elements);
   }

   inline ERR setRoundX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_RoundX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRoundY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_RoundY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setXOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_XOffset, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setYOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_YOffset, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Dimensions, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorPolygon class definition

#define VER_VECTORPOLYGON (1.000000)

class objVectorPolygon : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORPOLYGON;
   static constexpr CSTRING CLASS_NAME = "VectorPolygon";

   using create = pf::Create<objVectorPolygon>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setClosed(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Closed, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPathLength(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PathLength, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPointsArray(APTR * Value, int Elements) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PointsArray, &target);
      return field->WriteValue(target, field, 0x08001308, Value, Elements);
   }

   template <class T> inline ERR setPoints(T && Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Points, &target);
      return field->WriteValue(target, field, 0x08800208, to_cstring(Value), 1);
   }

   inline ERR setX1(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X1, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY1(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y1, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setX2(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X2, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY2(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y2, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

};

// VectorShape class definition

#define VER_VECTORSHAPE (1.000000)

class objVectorShape : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSHAPE;
   static constexpr CSTRING CLASS_NAME = "VectorShape";

   using create = pf::Create<objVectorShape>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setCenterX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Radius, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setClose(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Close, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Dimensions, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPhi(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Phi, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setA(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_A, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setB(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_B, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setM(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_M, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setN1(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_N1, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setN2(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_N2, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setN3(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_N3, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setVertices(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Vertices, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setMod(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Mod, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setSpiral(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Spiral, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setRepeat(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Repeat, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorSpiral class definition

#define VER_VECTORSPIRAL (1.000000)

class objVectorSpiral : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSPIRAL;
   static constexpr CSTRING CLASS_NAME = "VectorSpiral";

   using create = pf::Create<objVectorSpiral>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setPathLength(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_PathLength, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Radius, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Offset, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setStep(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Step, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setSpacing(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Spacing, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setLoopLimit(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_LoopLimit, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

};

// VectorEllipse class definition

#define VER_VECTORELLIPSE (1.000000)

class objVectorEllipse : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORELLIPSE;
   static constexpr CSTRING CLASS_NAME = "VectorEllipse";

   using create = pf::Create<objVectorEllipse>;

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setCenterY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_CenterY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadius(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Radius, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadiusX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_RadiusX, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setRadiusY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_RadiusY, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Dimensions, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setVertices(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Vertices, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// VectorViewport class definition

#define VER_VECTORVIEWPORT (1.000000)

class objVectorViewport : public objVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORVIEWPORT;
   static constexpr CSTRING CLASS_NAME = "VectorViewport";

   using create = pf::Create<objVectorViewport>;

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

   // Customised field setting

   inline ERR setAspectRatio(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_AspectRatio, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setBuffered(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Buffered, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setDimensions(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Dimensions, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setDragCallback(FUNCTION Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_DragCallback, &target);
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setOverflow(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Overflow, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setOverflowX(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_OverflowX, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setOverflowY(const int Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_OverflowY, &target);
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_X, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Y, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setXOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_XOffset, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setYOffset(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_YOffset, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Width, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_Height, &target);
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   inline ERR setViewX(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ViewX, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setViewY(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ViewY, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setViewWidth(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ViewWidth, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setViewHeight(const double Value) noexcept {
      OBJECTPTR target;
      auto field = FindField(this, FID_ViewHeight, &target);
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
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
   ERR (*_GeneratePath)(CSTRING Sequence, APTR *Path);
   ERR (*_GenerateRectangle)(double X, double Y, double Width, double Height, APTR *Path);
   ERR (*_ReadPainter)(objVectorScene *Scene, CSTRING IRI, struct VectorPainter *Painter, CSTRING *Result);
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
   ERR (*_ParseTransform)(struct VectorMatrix *Matrix, CSTRING Transform);
   ERR (*_ResetMatrix)(struct VectorMatrix *Matrix);
   ERR (*_GetFontHandle)(CSTRING Family, CSTRING Style, int Weight, int Size, APTR *Handle);
   ERR (*_GetFontMetrics)(APTR Handle, struct FontMetrics *Info);
   double (*_CharWidth)(APTR FontHandle, uint32_t Char, uint32_t KChar, double *Kerning);
   double (*_StringWidth)(APTR FontHandle, CSTRING String, int Chars);
   ERR (*_FlushMatrix)(struct VectorMatrix *Matrix);
   ERR (*_TracePath)(APTR Path, FUNCTION *Callback, double Scale);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_VECTOR_MODULE)
extern struct VectorBase *VectorBase;
namespace vec {
inline ERR DrawPath(objBitmap *Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle) { return VectorBase->_DrawPath(Bitmap,Path,StrokeWidth,StrokeStyle,FillStyle); }
inline ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path) { return VectorBase->_GenerateEllipse(CX,CY,RX,RY,Vertices,Path); }
inline ERR GeneratePath(CSTRING Sequence, APTR *Path) { return VectorBase->_GeneratePath(Sequence,Path); }
inline ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path) { return VectorBase->_GenerateRectangle(X,Y,Width,Height,Path); }
inline ERR ReadPainter(objVectorScene *Scene, CSTRING IRI, struct VectorPainter *Painter, CSTRING *Result) { return VectorBase->_ReadPainter(Scene,IRI,Painter,Result); }
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
inline ERR ParseTransform(struct VectorMatrix *Matrix, CSTRING Transform) { return VectorBase->_ParseTransform(Matrix,Transform); }
inline ERR ResetMatrix(struct VectorMatrix *Matrix) { return VectorBase->_ResetMatrix(Matrix); }
inline ERR GetFontHandle(CSTRING Family, CSTRING Style, int Weight, int Size, APTR *Handle) { return VectorBase->_GetFontHandle(Family,Style,Weight,Size,Handle); }
inline ERR GetFontMetrics(APTR Handle, struct FontMetrics *Info) { return VectorBase->_GetFontMetrics(Handle,Info); }
inline double CharWidth(APTR FontHandle, uint32_t Char, uint32_t KChar, double *Kerning) { return VectorBase->_CharWidth(FontHandle,Char,KChar,Kerning); }
inline double StringWidth(APTR FontHandle, CSTRING String, int Chars) { return VectorBase->_StringWidth(FontHandle,String,Chars); }
inline ERR FlushMatrix(struct VectorMatrix *Matrix) { return VectorBase->_FlushMatrix(Matrix); }
inline ERR TracePath(APTR Path, FUNCTION *Callback, double Scale) { return VectorBase->_TracePath(Path,Callback,Scale); }
} // namespace
#else
namespace vec {
extern ERR DrawPath(objBitmap *Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle);
extern ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path);
extern ERR GeneratePath(CSTRING Sequence, APTR *Path);
extern ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path);
extern ERR ReadPainter(objVectorScene *Scene, CSTRING IRI, struct VectorPainter *Painter, CSTRING *Result);
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
extern ERR ParseTransform(struct VectorMatrix *Matrix, CSTRING Transform);
extern ERR ResetMatrix(struct VectorMatrix *Matrix);
extern ERR GetFontHandle(CSTRING Family, CSTRING Style, int Weight, int Size, APTR *Handle);
extern ERR GetFontMetrics(APTR Handle, struct FontMetrics *Info);
extern double CharWidth(APTR FontHandle, uint32_t Char, uint32_t KChar, double *Kerning);
extern double StringWidth(APTR FontHandle, CSTRING String, int Chars);
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
#define SVF_A 0xc1d04330
#define SVF_ACHROMATOMALY 0xc16bc7de
#define SVF_ACHROMATOPSIA 0x804ec7c2
#define SVF_ADDITIVE 0x0fc7ffda
#define SVF_ALIGN 0x2c01c869
#define SVF_ALT_FILL 0x117439c0
#define SVF_AMPLITUDE 0x03d76a6d
#define SVF_ANIMATE 0xcdf5d33c
#define SVF_ANIMATECOLOR 0x24aced9c
#define SVF_ANIMATEMOTION 0x781b80cb
#define SVF_ANIMATETRANSFORM 0xbca03b18
#define SVF_ARITHMETIC 0x7e25e3d6
#define SVF_ATOP 0x378173b8
#define SVF_B 0xd280b0c4
#define SVF_BACKGROUNDALPHA 0x777b65c4
#define SVF_BACKGROUNDIMAGE 0x9a59b6fb
#define SVF_BASEFREQUENCY 0xa667fb24
#define SVF_BASEPROFILE 0x8732e574
#define SVF_BEVEL 0xb54ed6fc
#define SVF_BIAS 0xc144eed4
#define SVF_BOTTOM 0xd92dd2b6
#define SVF_BRIGHTNESS 0xa476534b
#define SVF_BURN 0xb33ddb25
#define SVF_BUTT 0xa14e6540
#define SVF_CIRCLE 0xd8532380
#define SVF_CLIP 0xa9da4244
#define SVF_CLIP_PATH 0x21f4eca1
#define SVF_CLIP_RULE 0x6d16685a
#define SVF_CLIP_RULE 0x6d16685a
#define SVF_CLIPPATH 0x471bca53
#define SVF_CLIPPATHUNITS 0x3f970764
#define SVF_CLOSE 0x6554da40
#define SVF_COLOR 0xc597636b
#define SVF_COLOUR 0x403911d4
#define SVF_COLORMAP 0xa16aa9b7
#define SVF_COLOURMAP 0x6125ecab
#define SVF_COLOR_INTERPOLATION 0xa3648641
#define SVF_COLOUR_INTERPOLATION 0x51318fe4
#define SVF_COLOR_INTERPOLATION_FILTERS 0x8052894c
#define SVF_COLOUR_INTERPOLATION_FILTERS 0xd9cad7f5
#define SVF_COLOURISE 0xc2d45aad
#define SVF_CONTOURGRADIENT 0x42ee6886
#define SVF_CONTRAST 0x01051f6d
#define SVF_CROSSORIGIN 0xf49a177c
#define SVF_CX 0xbe5bf68f
#define SVF_CY 0x4c30758c
#define SVF_D 0xf421572c
#define SVF_DESATURATE 0x1b434fd9
#define SVF_DESC 0xab506079
#define SVF_DEUTERANOMALY 0x0748a74f
#define SVF_DEUTERANOPIA 0x56cfecad
#define SVF_DIFFERENCE 0xf26a07f6
#define SVF_DISPLAY 0x39413236
#define SVF_DIVISOR 0x722185de
#define SVF_DODGE 0x0c3033d6
#define SVF_DUR 0xaa49757b
#define SVF_DX 0xc4363fca
#define SVF_DY 0x365dbcc9
#define SVF_EDGEMODE 0x34104c57
#define SVF_ELLIPSE 0xbf617070
#define SVF_ENABLE_BACKGROUND 0xf1ae31f5
#define SVF_EXCLUSION 0x40469a29
#define SVF_EXTERNALRESOURCESREQUIRED 0x66c27f26
#define SVF_FEBLEND 0x3d9a8463
#define SVF_FEBLUR 0x70962894
#define SVF_FECOLORMATRIX 0x857c41e1
#define SVF_FECOLOURMATRIX 0x4252ddac
#define SVF_FECOMPONENTTRANSFER 0xc217c63c
#define SVF_FECOMPOSITE 0x7ec3993e
#define SVF_FECONVOLVEMATRIX 0x63a9eb4a
#define SVF_FEDIFFUSELIGHTING 0x60a478ee
#define SVF_FEDISPLACEMENTMAP 0x3d6f8c1a
#define SVF_FEDISTANTLIGHT 0xe1bb00d2
#define SVF_FEDROPSHADOW 0x497e7e86
#define SVF_FEFLOOD 0xb943ce6c
#define SVF_FEGAUSSIANBLUR 0x101beb00
#define SVF_FEIMAGE 0x8f88e968
#define SVF_FEMERGE 0x030589c7
#define SVF_FEMORPHOLOGY 0x02e7fe01
#define SVF_FEOFFSET 0x1cbe9e8c
#define SVF_FEPOINTLIGHT 0x07819080
#define SVF_FESPECULARLIGHTING 0xcf7bed6f
#define SVF_FESPOTLIGHT 0x5ad5f299
#define SVF_FETILE 0x13cfb475
#define SVF_FETURBULENCE 0x46415651
#define SVF_FEWAVEFUNCTION 0xb9fbd726
#define SVF_FILL 0x34171e7f
#define SVF_FILL_OPACITY 0x23221962
#define SVF_FILL_RULE 0xf0da7524
#define SVF_FILLPAINT 0x21229bc9
#define SVF_FILTER 0x10835eef
#define SVF_FILTERUNITS 0x591030a0
#define SVF_FLOOD_COLOR 0x27af2aee
#define SVF_FLOOD_COLOUR 0xd7618827
#define SVF_FLOOD_OPACITY 0x6d730b02
#define SVF_FONT 0x58644726
#define SVF_FONT_FAMILY 0x7cb2ebc6
#define SVF_FONT_SIZE 0x6ed191e8
#define SVF_FONT_SIZE_ADJUST 0x50050ff5
#define SVF_FONT_STRETCH 0x12cc6d81
#define SVF_FONT_STYLE 0x92afa999
#define SVF_FONT_VARIANT 0x199cc39f
#define SVF_FONT_WEIGHT 0xd0abda7f
#define SVF_FREQUENCY 0x4f58465c
#define SVF_FROM 0x5046b6d9
#define SVF_FX 0xe3730f24
#define SVF_FY 0x11188c27
#define SVF_G 0xe771a4d8
#define SVF_GRADIENTTRANSFORM 0xe9d62cec
#define SVF_GRADIENTUNITS 0xc682a33b
#define SVF_HARDLIGHT 0xf3c9d889
#define SVF_HEIGHT 0xe6a89f2c
#define SVF_HUE 0xc8524c20
#define SVF_HUEROTATE 0x50c2a60f
#define SVF_ID 0x59170d66
#define SVF_IMAGE 0x95fbfcbe
#define SVF_IMAGE 0x95fbfcbe
#define SVF_IMAGE_RENDERING 0xf3c8e352
#define SVF_IN 0x32f5255e
#define SVF_IN2 0x014119c4
#define SVF_INHERIT 0xcacd1022
#define SVF_INVERT 0x4817759b
#define SVF_INVERT_X_AXIS 0x990d08cf
#define SVF_INVERT_Y_AXIS 0x763d63d6
#define SVF_INVERTRGB 0x0c9b8279
#define SVF_JAG 0x1749fe87
#define SVF_K1 0x1a86f347
#define SVF_K2 0x09d600b3
#define SVF_K3 0xfbbd83b0
#define SVF_K4 0x2f77e75b
#define SVF_KERNELMATRIX 0x579987b9
#define SVF_KERNELUNITLENGTH 0x99b2c0d5
#define SVF_KERNING 0x3bcebb50
#define SVF_LENGTHADJUST 0x60cce192
#define SVF_LETTER_SPACING 0xe648347a
#define SVF_LIGHTEN 0xc5c643ca
#define SVF_LINE 0x22c3b53a
#define SVF_LINEARGRADIENT 0x239400f6
#define SVF_LUMINANCETOALPHA 0xc39c4ad8
#define SVF_M 0x8c938ce0
#define SVF_MARKER 0x66386134
#define SVF_MARKER_END 0xf920a261
#define SVF_MARKER_MID 0xbb1c55e1
#define SVF_MARKER_START 0xd49a7b70
#define SVF_MASK 0x945655f2
#define SVF_MATRIX 0xf7b3282b
#define SVF_METHOD 0x24b8de54
#define SVF_MINUS 0x6c42c8b9
#define SVF_MITER 0xa670ab79
#define SVF_MITER_CLIP 0xbabd2750
#define SVF_MITER_REVERT 0x2bbb449d
#define SVF_MITER_ROUND 0x4d21bd1e
#define SVF_MOD 0x8432fb90
#define SVF_MODE 0x94661ac3
#define SVF_MULTIPLY 0xe6d29e8b
#define SVF_N1 0x47ae0aec
#define SVF_N2 0x54fef918
#define SVF_N3 0xa6957a1b
#define SVF_NONE 0x4c1560ac
#define SVF_NUMERIC_ID 0x7829752c
#define SVF_NUMOCTAVES 0x1ee96945
#define SVF_OFFSET 0xe9d6ebda
#define SVF_OPACITY 0xad6741c4
#define SVF_OPERATOR 0x63ec5b6c
#define SVF_ORDER 0xdbf25249
#define SVF_OUT 0x5e976c25
#define SVF_OVER 0x1185d380
#define SVF_OVERFLOW 0x720828b4
#define SVF_OVERLAY 0x49084d0b
#define SVF_PATH 0x442c26d3
#define SVF_PATHLENGTH 0x9290eaf0
#define SVF_PATTERN 0xe10e7cce
#define SVF_PATTERNCONTENTUNITS 0x7675c6ac
#define SVF_PATTERNTRANSFORM 0xb7125a26
#define SVF_PATTERNUNITS 0xcd3a14ba
#define SVF_PHI 0x321b0b2d
#define SVF_PLUS 0xdd97da51
#define SVF_POINTS 0x5666f871
#define SVF_POLYGON 0x4281f815
#define SVF_POLYLINE 0x66709a6c
#define SVF_PRESERVEALPHA 0x322fd548
#define SVF_PRIMITIVEUNITS 0xaef503a4
#define SVF_PROTANOMALY 0xed7e2f19
#define SVF_PROTANOPIA 0x7a606134
#define SVF_R 0xc2de77ab
#define SVF_RADIALGRADIENT 0x4c5ad3ec
#define SVF_RADIUS 0xef9b3678
#define SVF_RECT 0x7d209133
#define SVF_REPEAT 0x9e92666f
#define SVF_RESULT 0x4c2ea8cf
#define SVF_ROTATE 0x4ef36b09
#define SVF_ROUND 0xba4b04d5
#define SVF_RX 0x923c9f79
#define SVF_RY 0x60571c7a
#define SVF_SATURATE 0x65771dac
#define SVF_SCALE 0x346e5311
#define SVF_SCREEN 0x6b94c194
#define SVF_SEED 0xd9f4add6
#define SVF_SET 0xb77f7af1
#define SVF_SHAPE_RENDERING 0xe49a0e51
#define SVF_SOFTLIGHT 0x2ff7c658
#define SVF_SOURCEALPHA 0xeccfea8d
#define SVF_SOURCEGRAPHIC 0xb7669641
#define SVF_SPACING 0xe343f97f
#define SVF_SPIRAL 0x7a644399
#define SVF_SPREADMETHOD 0x41cb876a
#define SVF_SQUARE 0xcf6bf030
#define SVF_STARTOFFSET 0x00aee9ae
#define SVF_STDDEVIATION 0x2f21533e
#define SVF_STEP 0xdbd31252
#define SVF_STITCHTILES 0xf4c2f3f2
#define SVF_STRING 0x3b0e8431
#define SVF_STYLE 0x8cf919a9
#define SVF_SVG 0xbf531772
#define SVF_SYMBOL 0x7849f957
#define SVF_TARGETX 0x00ff9146
#define SVF_TARGETY 0xf2941245
#define SVF_TEXT 0x2f5319e1
#define SVF_TEXTPATH 0xd063f2ef
#define SVF_THICKNESS 0xbba696c7
#define SVF_TITLE 0x9e0656dc
#define SVF_TO 0x3f676dcf
#define SVF_TOP 0xbe447ad1
#define SVF_TRANSITION 0xbeb94f53
#define SVF_TRITANOMALY 0x883f05ad
#define SVF_TRITANOPIA 0xea686c90
#define SVF_X 0xa93c5f93
#define SVF_X1 0x11a4aa5f
#define SVF_X2 0x02f459ab
#define SVF_XOFFSET 0x90587e0f
#define SVF_XLINK_HREF 0x2dad60f1
#define SVF_XML_SPACE 0x7086c3bd
#define SVF_XMLNS 0x994f8128
#define SVF_XOR 0xf9f090f9
#define SVF_Y 0x5b57dc90
#define SVF_Y1 0x02063228
#define SVF_Y2 0x1156c1dc
#define SVF_YOFFSET 0xf85b52c7
#define SVF_Z 0x48072f64

#define SVF_ACCUMULATE 0xe9b4ecfb
#define SVF_ADDITIVE 0x0fc7ffda
#define SVF_ALICEBLUE 0xdf07766c
#define SVF_ANTIQUEWHITE 0x2a18510a
#define SVF_AQUA 0x6bbf0367
#define SVF_AQUAMARINE 0x7641c87d
#define SVF_ATTRIBUTENAME 0xc2f932dd
#define SVF_ATTRIBUTETYPE 0xd5b44b5a
#define SVF_AZURE 0x2c692f62
#define SVF_BEGIN 0x5208eb9b
#define SVF_BEIGE 0xd6ba1af8
#define SVF_BISQUE 0x649867e8
#define SVF_BLACK 0x27183398
#define SVF_BLANCHEDALMOND 0x9f2dd0d1
#define SVF_BLINK 0xdf75374e
#define SVF_BLUE 0xbd6b686a
#define SVF_BLUEVIOLET 0xf4f13a52
#define SVF_BOLD 0x145d8154
#define SVF_BOLDER 0x42b375fa
#define SVF_BROWN 0x5123bfa2
#define SVF_BURLYWOOD 0xbd372330
#define SVF_CADETBLUE 0x4508da1c
#define SVF_CHARTREUSE 0x0ce94dce
#define SVF_CHOCOLATE 0xf14587aa
#define SVF_CLASS 0xba4b3215
#define SVF_CONDENSED 0x0eb560f1
#define SVF_CONICGRADIENT 0xfaae2313
#define SVF_CORAL 0x14283cf1
#define SVF_CORNFLOWERBLUE 0x1beeea21
#define SVF_CORNSILK 0x5a78216b
#define SVF_CRIMSON 0x3f7e8be6
#define SVF_CYAN 0xc3d5b25a
#define SVF_DARKBLUE 0x510bace9
#define SVF_DARKCYAN 0x2fb576d9
#define SVF_DARKGOLDENROD 0x35395eaf
#define SVF_DARKGRAY 0x427c1a18
#define SVF_DARKGREEN 0x77836b51
#define SVF_DARKGREY 0x0cf67bc4
#define SVF_DARKKHAKI 0x4c297d51
#define SVF_DARKMAGENTA 0xa299b2bf
#define SVF_DARKOLIVEGREEN 0x814de411
#define SVF_DARKORANGE 0xe29e8201
#define SVF_DARKORCHID 0x8af8461e
#define SVF_DARKRED 0x64ea415f
#define SVF_DARKSALMON 0x89e01c2d
#define SVF_DARKSEAGREEN 0x459f3d3b
#define SVF_DARKSLATEBLUE 0x65a0cac7
#define SVF_DARKSLATEGRAY 0x76d77c36
#define SVF_DARKSLATEGREY 0x385d1dea
#define SVF_DARKTURQUOISE 0xd97b592c
#define SVF_DARKVIOLET 0x17cc4c94
#define SVF_DEEPPINK 0xe70afe42
#define SVF_DEEPSKYBLUE 0x6af9dd97
#define SVF_DIAMONDGRADIENT 0xee00ef24
#define SVF_DIMGRAY 0x8a458533
#define SVF_DIMGREY 0xc4cfe4ef
#define SVF_DODGERBLUE 0x3b2a8356
#define SVF_END 0xafc15d22
#define SVF_EXTRA_CONDENSED 0xcac4e64d
#define SVF_FILTERRES 0x82fcb7d5
#define SVF_FIREBRICK 0xf3b334ac
#define SVF_FLORALWHITE 0xf5eadd61
#define SVF_FORESTGREEN 0xe4091d70
#define SVF_FUCHSIA 0x502abcc6
#define SVF_GAINSBORO 0x1f64f0ea
#define SVF_GHOSTWHITE 0x92ee6fd9
#define SVF_GOLD 0xb23a1a1f
#define SVF_GOLDENROD 0x1f6c9a7a
#define SVF_GRAY 0xae1cde9b
#define SVF_GREEN 0xe6c9c319
#define SVF_GREENYELLOW 0x24fb7266
#define SVF_GREY 0xe096bf47
#define SVF_HONEYDEW 0xc5ea1d5d
#define SVF_HOTPINK 0x3e001845
#define SVF_HREF 0x419f9f20
#define SVF_INDIANRED 0xf487f95c
#define SVF_INDIGO 0x53a11986
#define SVF_IVORY 0x3ee96f1b
#define SVF_KHAKI 0xdd63d519
#define SVF_LAVENDER 0xbaa3cbe8
#define SVF_LAVENDERBLUSH 0x8e3cc246
#define SVF_LAWNGREEN 0x5fd62cb1
#define SVF_LEMONCHIFFON 0x30bc6ce5
#define SVF_LIGHTBLUE 0xfde6d362
#define SVF_LIGHTCORAL 0x9eb1e985
#define SVF_LIGHTCYAN 0x83580952
#define SVF_LIGHTER 0x98db4b75
#define SVF_LIGHTGOLDENRODYELLOW 0xc8d13bc2
#define SVF_LIGHTGRAY 0xee916593
#define SVF_LIGHTGREEN 0x6c50166d
#define SVF_LIGHTGREY 0xa01b044f
#define SVF_LIGHTPINK 0xaa34523d
#define SVF_LIGHTSALMON 0xf45b4931
#define SVF_LIGHTSEAGREEN 0xcd10ddaa
#define SVF_LIGHTSKYBLUE 0xbb96cc38
#define SVF_LIGHTSLATEGRAY 0x169c8cc2
#define SVF_LIGHTSLATEGREY 0x5816ed1e
#define SVF_LIGHTSTEELBLUE 0xb40ccd96
#define SVF_LIGHTYELLOW 0x6239c180
#define SVF_LIME 0x16241da3
#define SVF_LIMEGREEN 0x4b27a5cb
#define SVF_LINEN 0xc4e0dd28
#define SVF_LINETHROUGH 0x50cf4a84
#define SVF_MAGENTA 0xe808cb20
#define SVF_MAROON 0x1faa3ec7
#define SVF_MAX 0x2df461a5
#define SVF_MEDIUMAQUAMARINE 0xa7f0d7c8
#define SVF_MEDIUMBLUE 0xfab00e21
#define SVF_MEDIUMORCHID 0x5048d7b1
#define SVF_MEDIUMPURPLE 0x0dec2f1b
#define SVF_MEDIUMSEAGREEN 0x466d8b99
#define SVF_MEDIUMSLATEBLUE 0x26d3fd20
#define SVF_MEDIUMSPRINGGREEN 0xc7d593c0
#define SVF_MEDIUMTURQUOISE 0x9a086ecb
#define SVF_MEDIUMVIOLETRED 0x860f3bb7
#define SVF_MIDDLE 0x88fd6b0d
#define SVF_MIDNIGHTBLUE 0x5ff72640
#define SVF_MIN 0x861f829a
#define SVF_MINTCREAM 0x95bf7164
#define SVF_MISTYROSE 0x552adffa
#define SVF_MOCCASIN 0xa06fb271
#define SVF_NARROWER 0xfab37e07
#define SVF_NAVAJOWHITE 0x53d5278d
#define SVF_NAVY 0x5a3992f8
#define SVF_NORMAL 0x24992f14
#define SVF_OLDLACE 0xa86c5a1c
#define SVF_OLIVE 0xccef60dd
#define SVF_OLIVEDRAB 0x3161a365
#define SVF_ORANGE 0x29ad8dda
#define SVF_ORANGERED 0x6f22f08e
#define SVF_ORCHID 0x41cb49c5
#define SVF_OVERLINE 0xe2737158
#define SVF_PALEGOLDENROD 0x8f2fce7a
#define SVF_PALEGREEN 0x72a82ea2
#define SVF_PALETURQUOISE 0x636dc9f9
#define SVF_PALEVIOLETRED 0x7f6a9c85
#define SVF_PAPAYAWHIP 0x03951d1b
#define SVF_PEACHPUFF 0xa5e884b9
#define SVF_PERU 0x1c4b5847
#define SVF_PINK 0xeab9e935
#define SVF_PLUM 0x61b1a219
#define SVF_POWDERBLUE 0x72c5db48
#define SVF_PRESERVEASPECTRATIO 0xe9d982a7
#define SVF_PURPLE 0x1c6fb16f
#define SVF_RED 0x02602fe0
#define SVF_REPEATCOUNT 0x603f8fc1
#define SVF_REPEATDUR 0x5a09ac50
#define SVF_RESTART 0xcee95720
#define SVF_ROSYBROWN 0x04e15cab
#define SVF_ROYALBLUE 0xd24650ab
#define SVF_SADDLEBROWN 0xd62c1c46
#define SVF_SALMON 0x42d313f6
#define SVF_SANDYBROWN 0x0ed42fc4
#define SVF_SEAGREEN 0x896e6c71
#define SVF_SEASHELL 0x29c36970
#define SVF_SEMI_CONDENSED 0x840c831d
#define SVF_SIENNA 0x0b2ab519
#define SVF_SILVER 0xc22560b2
#define SVF_SKYBLUE 0xac67064d
#define SVF_SLATEBLUE 0x4ff50e12
#define SVF_SLATEGRAY 0x5c82b8e3
#define SVF_SLATEGREY 0x1208d93f
#define SVF_SNOW 0xb2d494ad
#define SVF_SPRINGGREEN 0x40e0440a
#define SVF_START 0xed217d81
#define SVF_STEELBLUE 0xfe12f9b7
#define SVF_STOP_COLOR 0x8644def2
#define SVF_STOP_OPACITY 0xb5dc0166
#define SVF_STROKE 0x56acfb7a
#define SVF_STROKE_DASHARRAY 0xb0cfba70
#define SVF_STROKE_DASHOFFSET 0x9222294e
#define SVF_STROKE_INNER_MITERLIMIT 0x46f66141
#define SVF_STROKE_INNERJOIN 0x4aa4e134
#define SVF_STROKE_LINECAP 0x56f959ea
#define SVF_STROKE_LINEJOIN 0x44682373
#define SVF_STROKE_MITERLIMIT 0x335836aa
#define SVF_STROKE_MITERLIMIT_THETA 0xc2aebedb
#define SVF_STROKE_OPACITY 0x65f3c703
#define SVF_STROKE_WIDTH 0x806b98d0
#define SVF_STROKEPAINT 0x1e55494e
#define SVF_TAN 0xf6b99013
#define SVF_TEAL 0x04a72c0f
#define SVF_TEXT_ANCHOR 0x489437d4
#define SVF_TEXT_DECORATION 0x7f3be8a5
#define SVF_TEXTLENGTH 0x7b8219d7
#define SVF_THISTLE 0xc7bdee49
#define SVF_TOMATO 0x8e0dace6
#define SVF_TOTAL_POINTS 0x5ce55650
#define SVF_TRANSFORM 0xf8c65d96
#define SVF_TURQUOISE 0xf32e9df9
#define SVF_TYPE 0x865f7960
#define SVF_ULTRA_CONDENSED 0xa99bdac7
#define SVF_UNDERLINE 0xb4f0ecf7
#define SVF_UNITS 0x5855e62f
#define SVF_USE 0xd2f16839
#define SVF_VALUES 0x42c2743d
#define SVF_VERSION 0xf60c5f97
#define SVF_VERTEX_SCALING 0x5ebed0ef
#define SVF_VERTICES 0xd12b9fb1
#define SVF_VIEW_HEIGHT 0x61cd3e71
#define SVF_VIEW_WIDTH 0x3aa96ddd
#define SVF_VIEW_X 0xd9b83fb2
#define SVF_VIEW_Y 0x2bd3bcb1
#define SVF_VIEWBOX 0x9c09f0aa
#define SVF_VIOLET 0xdcff434f
#define SVF_VISIBILITY 0x907b5022
#define SVF_WHEAT 0xfc9add1b
#define SVF_WHITE 0xdacd0b82
#define SVF_WHITESMOKE 0xd1c802d8
#define SVF_WIDER 0xecb563e9
#define SVF_WIDTH 0xbb6ee548
#define SVF_WORD_SPACING 0xfb7f3dd6

#define SVF_APPEND_PATH 0xc9bcd430
#define SVF_JOIN_PATH 0x7f8ef082
#define SVF_AZIMUTH 0xee584136
#define SVF_DARKEN 0xb4131a8d
#define SVF_DECAY 0x05314e55
#define SVF_DECODING 0xb34542ee
#define SVF_DEFS 0xd9e3af3c
#define SVF_ELEVATION 0x9d2a62fa
#define SVF_FEFUNCR 0x3b06a692
#define SVF_FEFUNCG 0x1ea975e1
#define SVF_FEFUNCB 0x2b5861fd
#define SVF_FEFUNCA 0x38089209
#define SVF_FOCALPOINT 0xc77e27f3
#define SVF_LIGHTING_COLOR 0x4d427379
#define SVF_LIGHTING_COLOUR 0x9169fd82
#define SVF_LIMITINGCONEANGLE 0xf4efd3a5
#define SVF_LOOP_LIMIT 0xa1fc321b
#define SVF_MASKCONTENTUNITS 0x048bd0e0
#define SVF_MASKUNITS 0xed514670
#define SVF_KOTUKU_MORPH 0x019115ac
#define SVF_KOTUKU_PATHTRANSITION 0x092128eb
#define SVF_KOTUKU_SHAPE 0xe71757e6
#define SVF_KOTUKU_SPIRAL 0x26d7e3ed
#define SVF_KOTUKU_TRANSITION 0xc044df05
#define SVF_KOTUKU_WAVE 0xe0c648dd
#define SVF_POINTSATX 0x84bdfe73
#define SVF_POINTSATY 0x76d67d70
#define SVF_POINTSATZ 0x65868e84
#define SVF_SPECULARCONSTANT 0x6659eec6
#define SVF_SPECULAREXPONENT 0x0968e2ed
#define SVF_TABLEVALUES 0xbc8a5d29
#define SVF_EXPONENT 0xfdfec4e2
#define SVF_SLOPE 0xde91130a
#define SVF_INTERCEPT 0x5bfcd3be
#define SVF_INVERT 0x4817759b
#define SVF_IDENTITY 0x70e2a60c
#define SVF_L 0x7ef80fe3
#define SVF_M 0x8c938ce0
#define SVF_N 0x9fc37f14
#define SVF_LINEAR 0x8f074b9a
#define SVF_TABLE 0xc951bea7
#define SVF_GAMMA 0x96d93a44
#define SVF_DISCRETE 0x52a7b4c3
#define SVF_DIFFUSECONSTANT 0x740f43be
#define SVF_RESOLUTION 0x4eb139e6
#define SVF_SURFACESCALE 0x5054681f
#define SVF_SWITCH 0xe37c1c3b
#define SVF_XCHANNELSELECTOR 0x99b2d745
#define SVF_YCHANNELSELECTOR 0x6bbedabb
#define SVF_ZOOMANDPAN 0xa56740cc
#define SVF_EXPANDED 0xb41290ab
#define SVF_SEMI_EXPANDED 0xd4a9b383
#define SVF_EXTRA_EXPANDED 0x5137a89c
#define SVF_ULTRA_EXPANDED 0xc340ad1e
#define SVF_CALCMODE 0x8e4b6f40
#define SVF_KEYPOINTS 0xa218f2ce
#define SVF_ORIGIN 0x2fab7b36
#define SVF_KEYTIMES 0xc21d7682
#define SVF_KEYSPLINES 0x51660c4a
#define SVF_BY 0x5f92edfb
#define SVF_YELLOW 0xd4b19b47
#define SVF_YELLOWGREEN 0x9c57fa2d
#define SVF_REQUIREDFEATURES 0x93615737
#define SVF_REQUIREDEXTENSIONS 0x085e0792
#define SVF_SYSTEMLANGUAGE 0x75da6464

#define SVF_ActiveBorder 0x23c8bda8
#define SVF_ActiveCaption 0x163ec68b
#define SVF_AppWorkspace 0x72d4291c
#define SVF_Background 0x13fe30d5
#define SVF_ButtonFace 0xe38fab4d
#define SVF_ButtonHighlight 0x00fa16a9
#define SVF_ButtonShadow 0x4da6d058
#define SVF_ButtonText 0x5f514327
#define SVF_CaptionText 0x0fde9208
#define SVF_GrayText 0xe0bc8411
#define SVF_Highlight 0x32b9e084
#define SVF_HighlightText 0x981173db
#define SVF_InactiveBorder 0x43db2792
#define SVF_InactiveCaption 0x4d5fb498
#define SVF_InactiveCaptionText 0xe8112f5e
#define SVF_InfoBackground 0xdad7ddc5
#define SVF_InfoText 0xff78c65a
#define SVF_ISOLATION_MODE 0x80b4f69d
#define SVF_Menu 0x49574232
#define SVF_MenuText 0x722c6139
#define SVF_Scrollbar 0xd4338d6b
#define SVF_ThreeDDarkShadow 0x4efbf16c
#define SVF_ThreeDFace 0x6bc48b51
#define SVF_ThreeDHighlight 0x0e1291fe
#define SVF_ThreeDLightShadow 0xaffdbf40
#define SVF_ThreeDShadow 0x81408528
#define SVF_Window 0x7fdabc89
#define SVF_WindowFrame 0x3e9e6224
#define SVF_WindowText 0x9352300a

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
   using namespace pf;

constexpr FieldValue Flags(VCLF Value) { return FieldValue(FID_Flags, int(Value)); }

constexpr FieldValue AppendPath(OBJECTPTR Value) { return FieldValue(FID_AppendPath, Value); }

constexpr FieldValue DragCallback(const FUNCTION &Value) { return FieldValue(FID_DragCallback, &Value); }
constexpr FieldValue DragCallback(const FUNCTION *Value) { return FieldValue(FID_DragCallback, Value); }

constexpr FieldValue OnChange(const FUNCTION &Value) { return FieldValue(FID_OnChange, &Value); }
constexpr FieldValue OnChange(const FUNCTION *Value) { return FieldValue(FID_OnChange, Value); }

constexpr FieldValue TextFlags(VTXF Value) { return FieldValue(FID_TextFlags, int(Value)); }
constexpr FieldValue Overflow(VOF Value) { return FieldValue(FID_Overflow, int(Value)); }

constexpr FieldValue Sequence(CSTRING Value) { return FieldValue(FID_Sequence, Value); }
inline FieldValue Sequence(std::string &Value) { return FieldValue(FID_Sequence, Value.c_str()); }

constexpr FieldValue FontStyle(CSTRING Value) { return FieldValue(FID_FontStyle, Value); }
inline FieldValue FontStyle(std::string &Value) { return FieldValue(FID_FontStyle, Value.c_str()); }

template <pf::NumericOrScale T> FieldValue RoundX(T Value) {
   return FieldValue(FID_RoundX, Value);
}

template <pf::NumericOrScale T> FieldValue RoundY(T Value) {
   return FieldValue(FID_RoundY, Value);
}

}

