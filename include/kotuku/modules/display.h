#pragma once

// Name:      display.h
// Copyright: Paul Manias © 2003-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_DISPLAY (1)

#ifdef __xwindows__

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#endif

class objBitmap;
class objDisplay;
class objClipboard;
class objController;
class objPointer;
class objSurface;

enum class DRAG : int {
   NIL = 0,
   NONE = 0,
   ANCHOR = 1,
   NORMAL = 2,
};

// Events for WindowHook()

enum class WH : int {
   NIL = 0,
   CLOSE = 1,
};

// Colour space options.

enum class CS : int {
   NIL = 0,
   SRGB = 1,
   LINEAR_RGB = 2,
   CIE_LAB = 3,
   CIE_LCH = 4,
};

// Optional flags for the ExposeSurface() function.

enum class EXF : uint32_t {
   NIL = 0,
   CHILDREN = 0x00000001,
   REDRAW_VOLATILE = 0x00000002,
   REDRAW_VOLATILE_OVERLAP = 0x00000004,
   ABSOLUTE_COORDS = 0x00000008,
   ABSOLUTE = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(EXF)

enum class RT : uint32_t {
   NIL = 0,
   ROOT = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(RT)

// drwLockBitmap() result flags

enum class LVF : uint32_t {
   NIL = 0,
   EXPOSE_CHANGES = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(LVF)

// Flags for RedrawSurface().

enum class IRF : uint32_t {
   NIL = 0,
   IGNORE_NV_CHILDREN = 0x00000001,
   IGNORE_CHILDREN = 0x00000002,
   SINGLE_BITMAP = 0x00000004,
   RELATIVE = 0x00000008,
   FORCE_DRAW = 0x00000010,
   REDRAWS_CHILDREN = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(IRF)

// CopySurface() flags

enum class BDF : uint32_t {
   NIL = 0,
   REDRAW = 0x00000001,
   DITHER = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(BDF)

enum class DSF : uint32_t {
   NIL = 0,
   NO_DRAW = 0x00000001,
   NO_EXPOSE = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(DSF)

// Options for the Surface WindowType field.

enum class SWIN : int {
   NIL = 0,
   HOST = 0,
   TASKBAR = 1,
   ICON_TRAY = 2,
   NONE = 3,
};

// Switches for the Surface class' Flags field.

enum class RNF : uint32_t {
   NIL = 0,
   TRANSPARENT = 0x00000001,
   STICK_TO_BACK = 0x00000002,
   STICK_TO_FRONT = 0x00000004,
   VISIBLE = 0x00000008,
   STICKY = 0x00000010,
   GRAB_FOCUS = 0x00000020,
   HAS_FOCUS = 0x00000040,
   DISABLED = 0x00000080,
   AUTO_QUIT = 0x00000100,
   HOST = 0x00000200,
   PRECOPY = 0x00000400,
   WRITE_ONLY = 0x00000800,
   VIDEO = 0x00000800,
   NO_HORIZONTAL = 0x00001000,
   NO_VERTICAL = 0x00002000,
   POINTER = 0x00004000,
   CURSOR = 0x00004000,
   AFTER_COPY = 0x00008000,
   READ_ONLY = 0x0000c040,
   VOLATILE = 0x0000c400,
   FIXED_BUFFER = 0x00010000,
   PERVASIVE_COPY = 0x00020000,
   NO_FOCUS = 0x00040000,
   FIXED_DEPTH = 0x00080000,
   TOTAL_REDRAW = 0x00100000,
   POST_COMPOSITE = 0x00200000,
   NO_PRECOMPOSITE = 0x00200000,
   COMPOSITE = 0x00200000,
   FULL_SCREEN = 0x00400000,
   IGNORE_FOCUS = 0x00800000,
   INIT_ONLY = 0x00cb0e81,
   ASPECT_RATIO = 0x01000000,
};

DEFINE_ENUM_FLAG_OPERATORS(RNF)

enum class HOST : int {
   NIL = 0,
   TRAY_ICON = 1,
   TASKBAR = 2,
   STICK_TO_FRONT = 3,
   TRANSLUCENCE = 4,
   TRANSPARENT = 5,
};

// Flags for the Pointer class.

enum class PF : uint32_t {
   NIL = 0,
   UNUSED = 0x00000001,
   VISIBLE = 0x00000002,
   ANCHOR = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(PF)

// Acceleration flags for GetDisplayInfo().

enum class ACF : uint32_t {
   NIL = 0,
   VIDEO_BLIT = 0x00000001,
   SOFTWARE_BLIT = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(ACF)

// Flags for the SetCursor() function.

enum class CRF : uint32_t {
   NIL = 0,
   LMB = 0x00000001,
   MMB = 0x00000002,
   RMB = 0x00000004,
   RESTRICT = 0x00000008,
   BUFFER = 0x00000010,
   NO_BUTTONS = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(CRF)

// Instructions for basic graphics operations.

enum class BAF : uint32_t {
   NIL = 0,
   DITHER = 0x00000001,
   FILL = 0x00000001,
   BLEND = 0x00000002,
   COPY = 0x00000004,
   LINEAR = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(BAF)

// Flags for CopySurface().

enum class CSRF : uint32_t {
   NIL = 0,
   TRANSPARENT = 0x00000001,
   ALPHA = 0x00000002,
   TRANSLUCENT = 0x00000004,
   DEFAULT_FORMAT = 0x00000008,
   CLIP = 0x00000010,
   OFFSET = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(CSRF)

// Bitmap types

enum class BMP : int {
   NIL = 0,
   PLANAR = 2,
   CHUNKY = 3,
};

// Defines the blending algorithm to use when transparent pixels are rendered to the bitmap.

enum class BLM : int {
   NIL = 0,
   AUTO = 0,
   NONE = 1,
   SRGB = 2,
   GAMMA = 3,
   LINEAR = 4,
};

// Bitmap flags

enum class BMF : uint32_t {
   NIL = 0,
   BLANK_PALETTE = 0x00000001,
   COMPRESSED = 0x00000002,
   NO_DATA = 0x00000004,
   TRANSPARENT = 0x00000008,
   MASK = 0x00000010,
   INVERSE_ALPHA = 0x00000020,
   QUERIED = 0x00000040,
   CLEAR = 0x00000080,
   USER = 0x00000100,
   ACCELERATED_2D = 0x00000200,
   ACCELERATED_3D = 0x00000400,
   ALPHA_CHANNEL = 0x00000800,
   NEVER_SHRINK = 0x00001000,
   X11_DGA = 0x00002000,
   FIXED_DEPTH = 0x00004000,
   PREMUL = 0x00008000,
};

DEFINE_ENUM_FLAG_OPERATORS(BMF)

// Gamepad controller buttons.

enum class CON : uint32_t {
   NIL = 0,
   GAMEPAD_S = 0x00000001,
   GAMEPAD_E = 0x00000002,
   GAMEPAD_W = 0x00000004,
   GAMEPAD_N = 0x00000008,
   DPAD_UP = 0x00000010,
   DPAD_DOWN = 0x00000020,
   DPAD_LEFT = 0x00000040,
   DPAD_RIGHT = 0x00000080,
   START = 0x00000100,
   SELECT = 0x00000200,
   LEFT_BUMPER_1 = 0x00000400,
   LEFT_BUMPER_2 = 0x00000800,
   RIGHT_BUMPER_1 = 0x00001000,
   RIGHT_BUMPER_2 = 0x00002000,
   LEFT_THUMB = 0x00004000,
   RIGHT_THUMB = 0x00008000,
};

DEFINE_ENUM_FLAG_OPERATORS(CON)

// Display flags.

enum class SCR : uint32_t {
   NIL = 0,
   READ_ONLY = 0xfe300019,
   VISIBLE = 0x00000001,
   AUTO_SAVE = 0x00000002,
   BUFFER = 0x00000004,
   NO_ACCELERATION = 0x00000008,
   BIT_6 = 0x00000010,
   BORDERLESS = 0x00000020,
   COMPOSITE = 0x00000040,
   ALPHA_BLEND = 0x00000040,
   GRAB_CONTROLLERS = 0x00000080,
   PRIMARY = 0x00000100,
   ACTIVE = 0x00000200,
   MIRRORED = 0x00000400,
   VIRTUAL = 0x00000800,
   ROTATED = 0x00001000,
   MAXSIZE = 0x00100000,
   REFRESH = 0x00200000,
   HOSTED = 0x02000000,
   POWERSAVE = 0x04000000,
   DPMS_ENABLED = 0x08000000,
   GTF_ENABLED = 0x10000000,
   FLIPPABLE = 0x20000000,
   CUSTOM_WINDOW = 0x40000000,
   MAXIMISE = 0x80000000,
};

DEFINE_ENUM_FLAG_OPERATORS(SCR)

// Flags for the Display class SetMonitor() method.

enum class MON : uint32_t {
   NIL = 0,
   AUTO_DETECT = 0x00000001,
   BIT_6 = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(MON)

// Flags for gamma operations.

enum class GMF : uint32_t {
   NIL = 0,
   SAVE = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(GMF)

// Flags for GetDisplayType().

enum class DT : int {
   NIL = 0,
   NATIVE = 1,
   X11 = 2,
   WINGDI = 3,
   GLES = 4,
};

// Possible modes for the Display class' PowerMode field.

enum class DPMS : int {
   NIL = 0,
   DEFAULT = 0,
   OFF = 1,
   SUSPEND = 2,
   STANDBY = 3,
};

enum class CT : int {
   NIL = 0,
   DATA = 0,
   AUDIO = 1,
   IMAGE = 2,
   FILE = 3,
   OBJECT = 4,
   TEXT = 5,
   END = 6,
};

// Clipboard types

enum class CLIPTYPE : uint32_t {
   NIL = 0,
   DATA = 0x00000001,
   AUDIO = 0x00000002,
   IMAGE = 0x00000004,
   FILE = 0x00000008,
   OBJECT = 0x00000010,
   TEXT = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(CLIPTYPE)

// Clipboard flags

enum class CPF : uint32_t {
   NIL = 0,
   DRAG_DROP = 0x00000001,
   HOST = 0x00000002,
   HISTORY_BUFFER = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(CPF)

enum class CEF : uint32_t {
   NIL = 0,
   DELETE = 0x00000001,
   EXTEND = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(CEF)

typedef struct SurfaceInfo {
   APTR     Data;           // Bitmap data memory ID
   OBJECTID ParentID;       // Object that contains the surface area
   OBJECTID BitmapID;       // Surface bitmap buffer
   OBJECTID DisplayID;      // Refers to the display if this object is at root level
   RNF      Flags;          // Surface flags
   int      X;              // Horizontal coordinate
   int      Y;              // Vertical coordinate
   int      Width;          // Width of the surface area
   int      Height;         // Height of the surface area
   int      AbsX;           // Absolute X coordinate
   int      AbsY;           // Absolute Y coordinate
   int16_t  Level;          // Branch level within the tree
   int8_t   BitsPerPixel;   // Bits per pixel of the bitmap
   int8_t   BytesPerPixel;  // Bytes per pixel of the bitmap
   int      LineWidth;      // Line width of the bitmap, in bytes
   inline bool visible() const { return (Flags & RNF::VISIBLE) != RNF::NIL; }
   inline bool invisible() const { return (Flags & RNF::VISIBLE) IS RNF::NIL; }
   inline bool hasFocus() const { return (Flags & RNF::HAS_FOCUS) != RNF::NIL; }
   inline bool transparent() const { return (Flags & RNF::TRANSPARENT) != RNF::NIL; }
} SURFACEINFO;

struct SurfaceCallback {
   OBJECTPTR Object;    // Context to use for the function.
   FUNCTION  Function;  // void (*Routine)(OBJECTPTR, struct Surface *, objBitmap *);
};

struct SurfaceCoords {
   int X;        // Horizontal coordinate
   int Y;        // Vertical coordinate
   int Width;    // Width
   int Height;   // Height
   int AbsX;     // Absolute X
   int AbsY;     // Absolute Y
};

struct xrMode {
   int Width;    // Horizontal
   int Height;   // Vertical
   int Depth;    // bit depth
};

typedef struct PixelFormat {
   uint8_t  RedShift;   // Right shift value
   uint8_t  GreenShift; // Green shift value
   uint8_t  BlueShift;  // Blue shift value
   uint8_t  AlphaShift; // Alpha shift value
   uint8_t  RedMask;    // The unshifted red mask value (ranges from 0x00 to 0xff)
   uint8_t  GreenMask;  // The unshifted green mask value (ranges from 0x00 to 0xff)
   uint8_t  BlueMask;   // The unshifted blue mask value (ranges from 0x00 to 0xff)
   uint8_t  AlphaMask;  // The unshifted alpha mask value (ranges from 0x00 to 0xff)
   uint8_t  RedPos;     // Left shift/positional value for red
   uint8_t  GreenPos;   // Left shift/positional value for green
   uint8_t  BluePos;    // Left shift/positional value for blue
   uint8_t  AlphaPos;   // Left shift/positional value for alpha
   uint32_t Pad;        // Padding
} PIXELFORMAT;

struct DisplayInfo {
   struct PixelFormat PixelFormat;    // The colour format to use for each pixel.
   float    MinRefresh;               // Minimum refresh rate
   float    MaxRefresh;               // Maximum refresh rate
   float    RefreshRate;              // Recommended refresh rate
   OBJECTID DisplayID;                // Object ID related to the display
   ACF      AccelFlags;               // Flags describing supported hardware features.
   SCR      Flags;                    // Display flags
   int      AmtColours;               // Total number of supported colours.
   int      Index;                    // Display mode ID (internal)
   int      HDensity;                 // Horizontal pixel density per inch.
   int      VDensity;                 // Vertical pixel density per inch.
   int      HostedX;                  // Horizontal display position in virtual desktop coordinates
   int      HostedY;                  // Vertical display position in virtual desktop coordinates
   int      MonitorX;                 // Horizontal position of the monitor, relative to the primary monitor
   int      MonitorY;                 // Vertical position of the monitor, relative to the primary monitor
   int      MonitorWidth;             // Pixel width of the monitor
   int      MonitorHeight;            // Pixel height of the monitor
   int      VirtualX;                 // Horizontal position of the complete desktop spanning all monitors
   int      VirtualY;                 // Vertical position of the complete desktop spanning all monitors
   int      VirtualWidth;             // Width of the complete desktop spanning all monitors
   int      VirtualHeight;            // Height of the complete desktop spanning all monitors
   int      PhysicalWidth;            // Width in millimeters, 0 if unknown
   int      PhysicalHeight;           // Height in millimeters, 0 if unknown
   int16_t  Width;                    // Pixel width of the display bitmap
   int16_t  Height;                   // Pixel height of the display bitmap
   int16_t  BitsPerPixel;             // Bits per pixel
   int16_t  BytesPerPixel;            // Bytes per pixel
};

struct CursorInfo {
   int     Width;           // Maximum cursor width for custom cursors
   int     Height;          // Maximum cursor height for custom cursors
   int     Flags;           // Currently unused
   int16_t BitsPerPixel;    // Preferred bits-per-pixel setting for custom cursors
};

typedef struct BitmapSurface {
   APTR    Data;                 // Pointer to the bitmap graphics data.
   int16_t Width;                // Pixel width of the bitmap.
   int16_t Height;               // Pixel height of the bitmap.
   int     LineWidth;            // The distance between bitmap lines, measured in bytes.
   uint8_t BitsPerPixel;         // The number of bits per pixel (8, 15, 16, 24, 32).
   uint8_t BytesPerPixel;        // The number of bytes per pixel (1, 2, 3, 4).
   uint8_t Opacity;              // Opacity level of the source if CSRF::TRANSLUCENT is used.
   uint8_t Version;              // Version of this structure.
   int     Colour;               // Colour index to use if CSRF::TRANSPARENT is used.
   struct ClipRectangle Clip;    // A clipping rectangle will restrict drawing operations to this region if CSRF::CLIP is used.
   int16_t XOffset;              // Offset all X coordinate references by the given value.
   int16_t YOffset;              // Offset all Y coordinate references by the given value.
   struct ColourFormat Format;   // The colour format of this bitmap's pixels, or alternatively use CSRF::DEFAULT_FORMAT.
} BITMAPSURFACE;

// Bitmap class definition

#define VER_BITMAP (1.000000)

// Bitmap methods

namespace bmp {
struct CopyArea { objBitmap *DestBitmap; BAF Flags; int X; int Y; int Width; int Height; int XDest; int YDest; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Compress { int Level; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Decompress { int RetainData; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DrawRectangle { int X; int Y; int Width; int Height; uint32_t Colour; BAF Flags; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetClipRegion { int Left; int Top; int Right; int Bottom; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetColour { int Red; int Green; int Blue; int Alpha; uint32_t Colour; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Premultiply { static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Demultiply { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ConvertToLinear { static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ConvertToRGB { static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objBitmap : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::BITMAP;
   static constexpr CSTRING CLASS_NAME = "Bitmap";

   using create = kt::Create<objBitmap>;

   struct RGBPalette * Palette;                                    // Points to a bitmap's colour palette.
   struct ColourFormat * ColourFormat;                             // Describes the colour format used to construct each bitmap pixel.
   void (*DrawUCPixel)(objBitmap *, int, int, uint32_t);           // Points to a C function that draws pixels to the bitmap using colour indexes.
   void (*DrawUCRPixel)(objBitmap *, int, int, struct RGB8 *);     // Points to a C function that draws pixels to the bitmap in RGB format.
   uint32_t (*ReadUCPixel)(objBitmap *, int, int);                 // Points to a C function that reads pixels from the bitmap in colour index format.
   void (*ReadUCRPixel)(objBitmap *, int, int, struct RGB8 *);     // Points to a C function that reads pixels from the bitmap in RGB format.
   void (*ReadUCRIndex)(objBitmap *, uint8_t *, struct RGB8 *);    // Points to a C function that reads pixels from the bitmap in RGB format.
   void (*DrawUCRIndex)(objBitmap *, uint8_t *, struct RGB8 *);    // Points to a C function that draws pixels to the bitmap in RGB format.
   uint8_t * Data;                                                 // Pointer to a bitmap's data area.
   int       Width;                                                // The width of the bitmap, in pixels.
   int       ByteWidth;                                            // The width of the bitmap, in bytes.
   int       Height;                                               // The height of the bitmap, in pixels.
   BMP       Type;                                                 // Defines the data type of the bitmap.
   int       LineWidth;                                            // The length of each bitmap line in bytes, including alignment.
   int       PlaneMod;                                             // The differential between each bitmap plane.
   struct ClipRectangle Clip;                                      // Defines the bitmap's clipping region.
   int       Size;                                                 // The total size of the bitmap, in bytes.
   MEM       DataFlags;                                            // Defines the memory flags to use when allocating a bitmap's data area.
   int       AmtColours;                                           // The maximum number of colours represented by the bitmap format.
   BMF       Flags;                                                // Optional flags.
   int       TransIndex;                                           // The transparent colour of the bitmap, represented as an index.
   int       BytesPerPixel;                                        // The number of bytes per pixel.
   int       BitsPerPixel;                                         // The number of bits used to represent each pixel.
   int       Position;                                             // The current read/write data position.
   int       Opacity;                                              // Determines the translucency setting to use in drawing operations.
   BLM       BlendMode;                                            // Defines the blending algorithm to use when rendering transparent pixels.
   struct RGB8 TransColour;                                        // The transparent colour of the bitmap, in RGB format.
   struct RGB8 Bkgd;                                               // Background colour in RGB format.
   int       BkgdIndex;                                            // Background colour as a packed pixel value or palette index.
   CS        ColourSpace;                                          // Defines the colour space for RGB values.
   public:
   inline uint32_t getColour(struct RGB8 &RGB) {
      if (BitsPerPixel > 8) return packPixel(RGB);
      else {
         uint32_t result;
         if (!getColour(RGB.Red, RGB.Green, RGB.Blue, RGB.Alpha, &result)) {
            return result;
         }
         else return 0;
      }
   }

   inline uint32_t packPixel(uint8_t R, uint8_t G, uint8_t B) {
      return
         (((R>>ColourFormat->RedShift) & ColourFormat->RedMask) << ColourFormat->RedPos) |
         (((G>>ColourFormat->GreenShift) & ColourFormat->GreenMask) << ColourFormat->GreenPos) |
         (((B>>ColourFormat->BlueShift) & ColourFormat->BlueMask) << ColourFormat->BluePos) |
         (((255>>ColourFormat->AlphaShift) & ColourFormat->AlphaMask) << ColourFormat->AlphaPos);
   }

   inline uint32_t packPixel(uint8_t R, uint8_t G, uint8_t B, uint8_t A) {
      return
         (((R>>ColourFormat->RedShift) & ColourFormat->RedMask) << ColourFormat->RedPos) |
         (((G>>ColourFormat->GreenShift) & ColourFormat->GreenMask) << ColourFormat->GreenPos) |
         (((B>>ColourFormat->BlueShift) & ColourFormat->BlueMask) << ColourFormat->BluePos) |
         (((A>>ColourFormat->AlphaShift) & ColourFormat->AlphaMask) << ColourFormat->AlphaPos);
   }

   inline uint32_t packPixel(struct RGB8 &RGB, uint8_t Alpha) {
      return
         (((RGB.Red>>ColourFormat->RedShift) & ColourFormat->RedMask) << ColourFormat->RedPos) |
         (((RGB.Green>>ColourFormat->GreenShift) & ColourFormat->GreenMask) << ColourFormat->GreenPos) |
         (((RGB.Blue>>ColourFormat->BlueShift) & ColourFormat->BlueMask) << ColourFormat->BluePos) |
         (((Alpha>>ColourFormat->AlphaShift) & ColourFormat->AlphaMask) << ColourFormat->AlphaPos);
   }

   inline uint32_t packPixel(struct RGB8 &RGB) {
      return
         (((RGB.Red>>ColourFormat->RedShift) & ColourFormat->RedMask) << ColourFormat->RedPos) |
         (((RGB.Green>>ColourFormat->GreenShift) & ColourFormat->GreenMask) << ColourFormat->GreenPos) |
         (((RGB.Blue>>ColourFormat->BlueShift) & ColourFormat->BlueMask) << ColourFormat->BluePos) |
         (((RGB.Alpha>>ColourFormat->AlphaShift) & ColourFormat->AlphaMask) << ColourFormat->AlphaPos);
   }

   // Pack pixel 'whole-byte' version, for faster 24/32 bit formats

   inline uint32_t packPixelWB(uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) {
      return (R << ColourFormat->RedPos) | (G << ColourFormat->GreenPos) | (B << ColourFormat->BluePos) | (A << ColourFormat->AlphaPos);
   }

   inline uint32_t packPixelWB(struct RGB8 &RGB) {
      return (RGB.Red << ColourFormat->RedPos) | (RGB.Green << ColourFormat->GreenPos) | (RGB.Blue << ColourFormat->BluePos) | (RGB.Alpha << ColourFormat->AlphaPos);
   }

   inline uint32_t packPixelWB(struct RGB8 &RGB, uint8_t Alpha) {
      return (RGB.Red << ColourFormat->RedPos) | (RGB.Green << ColourFormat->GreenPos) | (RGB.Blue << ColourFormat->BluePos) | (Alpha << ColourFormat->AlphaPos);
   }

   // Modify the Data pointer to point to a specific pixel offset, return the original Data value which must be restored later.

   inline uint8_t * offset(int X, int Y) {
      auto orig_data = Data;
      Data += (X * BytesPerPixel) + (Y * LineWidth);
      return orig_data;
   }

   // Colour unpacking routines

   template <class T> inline uint8_t unpackRed(T Packed)   { return (((Packed >> ColourFormat->RedPos) & ColourFormat->RedMask) << ColourFormat->RedShift); }
   template <class T> inline uint8_t unpackGreen(T Packed) { return (((Packed >> ColourFormat->GreenPos) & ColourFormat->GreenMask) << ColourFormat->GreenShift); }
   template <class T> inline uint8_t unpackBlue(T Packed)  { return (((Packed >> ColourFormat->BluePos) & ColourFormat->BlueMask) << ColourFormat->BlueShift); }
   template <class T> inline uint8_t unpackAlpha(T Packed) { return (((Packed >> ColourFormat->AlphaPos) & ColourFormat->AlphaMask)); }

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR copyData(OBJECTPTR Dest) noexcept {
      struct acCopyData args = { .Dest = Dest };
      return Action(AC::CopyData, this, &args);
   }
   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR lock() noexcept { return Action(AC::Lock, this, nullptr); }
   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = static_cast<U>(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR resize(double Width, double Height, double Depth = 0) noexcept {
      struct acResize args = { Width, Height, Depth };
      return Action(AC::Resize, this, &args);
   }
   inline ERR saveImage(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveImage args = { Dest, { ClassID } };
      return Action(AC::SaveImage, this, &args);
   }
   inline ERR seek(double Offset, SEEK Position = SEEK::CURRENT) noexcept {
      struct acSeek args = { Offset, Position };
      return Action(AC::Seek, this, &args);
   }
   inline ERR seekStart(double Offset) noexcept { return seek(Offset, SEEK::START); }
   inline ERR seekEnd(double Offset) noexcept { return seek(Offset, SEEK::END); }
   inline ERR seekCurrent(double Offset) noexcept { return seek(Offset, SEEK::CURRENT); }
   inline ERR unlock() noexcept { return Action(AC::Unlock, this, nullptr); }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR copyArea(objBitmap * DestBitmap, BAF Flags, int X, int Y, int Width, int Height, int XDest, int YDest) noexcept {
      struct bmp::CopyArea args = { DestBitmap, Flags, X, Y, Width, Height, XDest, YDest };
      return Action(AC(-1), this, &args);
   }
   inline ERR compress(int Level) noexcept {
      struct bmp::Compress args = { Level };
      return Action(AC(-2), this, &args);
   }
   inline ERR decompress(int RetainData) noexcept {
      struct bmp::Decompress args = { RetainData };
      return Action(AC(-3), this, &args);
   }
   inline ERR drawRectangle(int X, int Y, int Width, int Height, uint32_t Colour, BAF Flags) noexcept {
      struct bmp::DrawRectangle args = { X, Y, Width, Height, Colour, Flags };
      return Action(AC(-4), this, &args);
   }
   inline ERR setClipRegion(int Left, int Top, int Right, int Bottom) noexcept {
      struct bmp::SetClipRegion args = { Left, Top, Right, Bottom };
      return Action(AC(-5), this, &args);
   }
   inline ERR getColour(int Red, int Green, int Blue, int Alpha, uint32_t * Colour) noexcept {
      struct bmp::GetColour args = { Red, Green, Blue, Alpha, (uint32_t)0 };
      ERR error = Action(AC(-6), this, &args);
      if (Colour) *Colour = args.Colour;
      return error;
   }
   inline ERR premultiply() noexcept {
      return Action(AC(-7), this, nullptr);
   }
   inline ERR demultiply() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR convertToLinear() noexcept {
      return Action(AC(-9), this, nullptr);
   }
   inline ERR convertToRGB() noexcept {
      return Action(AC(-10), this, nullptr);
   }

   // Customised field getting

   inline ERR getPalette(struct RGBPalette * &Value) noexcept {
      Value = this->Palette;
      return ERR::Okay;
   }

   inline ERR getColourFormat(struct ColourFormat * &Value) noexcept {
      Value = this->ColourFormat;
      return ERR::Okay;
   }

   inline ERR getData(uint8_t * &Value) noexcept {
      Value = this->Data;
      return ERR::Okay;
   }

   inline ERR getWidth(int &Value) noexcept {
      Value = this->Width;
      return ERR::Okay;
   }

   inline ERR getByteWidth(int &Value) noexcept {
      Value = this->ByteWidth;
      return ERR::Okay;
   }

   inline ERR getHeight(int &Value) noexcept {
      Value = this->Height;
      return ERR::Okay;
   }

   inline ERR getType(BMP &Value) noexcept {
      Value = this->Type;
      return ERR::Okay;
   }

   inline ERR getLineWidth(int &Value) noexcept {
      Value = this->LineWidth;
      return ERR::Okay;
   }

   inline ERR getPlaneMod(int &Value) noexcept {
      Value = this->PlaneMod;
      return ERR::Okay;
   }

   inline ERR getClip(struct ClipRectangle * &Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->GetValue(this, &Value);
   }

   inline ERR getSize(int &Value) noexcept {
      Value = this->Size;
      return ERR::Okay;
   }

   inline ERR getDataFlags(MEM &Value) noexcept {
      Value = this->DataFlags;
      return ERR::Okay;
   }

   inline ERR getAmtColours(int &Value) noexcept {
      Value = this->AmtColours;
      return ERR::Okay;
   }

   inline ERR getFlags(BMF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getTransIndex(int &Value) noexcept {
      Value = this->TransIndex;
      return ERR::Okay;
   }

   inline ERR getBytesPerPixel(int &Value) noexcept {
      Value = this->BytesPerPixel;
      return ERR::Okay;
   }

   inline ERR getBitsPerPixel(int &Value) noexcept {
      Value = this->BitsPerPixel;
      return ERR::Okay;
   }

   inline ERR getPosition(int &Value) noexcept {
      Value = this->Position;
      return ERR::Okay;
   }

   inline ERR getOpacity(int &Value) noexcept {
      Value = this->Opacity;
      return ERR::Okay;
   }

   inline ERR getBlendMode(BLM &Value) noexcept {
      Value = this->BlendMode;
      return ERR::Okay;
   }

   inline ERR getTransColour(struct RGB8 * &Value) noexcept {
      Value = &this->TransColour;
      return ERR::Okay;
   }

   inline ERR getBkgd(struct RGB8 * &Value) noexcept {
      Value = &this->Bkgd;
      return ERR::Okay;
   }

   inline ERR getBkgdIndex(int &Value) noexcept {
      Value = this->BkgdIndex;
      return ERR::Okay;
   }

   inline ERR getColourSpace(CS &Value) noexcept {
      Value = this->ColourSpace;
      return ERR::Okay;
   }

   inline ERR getClipLeft(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + 184));
      return ERR::Okay;
   }

   inline ERR getClipRight(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + 188));
      return ERR::Okay;
   }

   inline ERR getClipBottom(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + 192));
      return ERR::Okay;
   }

   inline ERR getClipTop(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + 196));
      return ERR::Okay;
   }

   inline ERR getHandle(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setPalette(struct RGBPalette * Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(this, field, 0x08000300, Value);
   }

   inline ERR setData(uint8_t * Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, 0x08000500, Value);
   }

   inline ERR setWidth(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Width = Value;
      return ERR::Okay;
   }

   inline ERR setHeight(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Height = Value;
      return ERR::Okay;
   }

   inline ERR setType(const BMP Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Type = Value;
      return ERR::Okay;
   }

   inline ERR setClip(struct ClipRectangle * Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, 0x08100310, Value);
   }

   inline ERR setDataFlags(const MEM Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->DataFlags = Value;
      return ERR::Okay;
   }

   inline ERR setAmtColours(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->AmtColours = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const BMF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setTransIndex(const int Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setBytesPerPixel(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->BytesPerPixel = Value;
      return ERR::Okay;
   }

   inline ERR setBitsPerPixel(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->BitsPerPixel = Value;
      return ERR::Okay;
   }

   inline ERR setOpacity(const int Value) noexcept {
      this->Opacity = Value;
      return ERR::Okay;
   }

   inline ERR setBlendMode(const BLM Value) noexcept {
      this->BlendMode = Value;
      return ERR::Okay;
   }

   inline ERR setTransColour(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setBkgd(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setBkgdIndex(const int Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setColourSpace(const CS Value) noexcept {
      this->ColourSpace = Value;
      return ERR::Okay;
   }

   inline ERR setClipLeft(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setClipRight(const int Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setClipBottom(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setClipTop(const int Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setHandle(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, 0x08100300, Value);
   }

};

// Display class definition

#define VER_DISPLAY (1.000000)

// Display methods

namespace gfx {
struct WaitVBL { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct UpdatePalette { struct RGBPalette *NewPalette; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetDisplay { int X; int Y; int Width; int Height; int InsideWidth; int InsideHeight; int BitsPerPixel; double RefreshRate; int Flags; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SizeHints { int MinWidth; int MinHeight; int MaxWidth; int MaxHeight; int EnforceAspect; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetGamma { double Red; double Green; double Blue; GMF Flags; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetGammaLinear { double Red; double Green; double Blue; GMF Flags; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetMonitor { std::string_view Name; int MinH; int MaxH; int MinV; int MaxV; MON Flags; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Minimise { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CheckXWindow { static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetFrame { int Left; int Top; int Right; int Bottom; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objDisplay : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DISPLAY;
   static constexpr CSTRING CLASS_NAME = "Display";

   using create = kt::Create<objDisplay>;

   double   RefreshRate;  // Active display refresh rate.
   objBitmap * Bitmap;    // Reference to the display's bitmap information.
   SCR      Flags;        // Optional flag settings.
   int      Width;        // Defines the width of the display.
   int      Height;       // Defines the height of the display.
   int      X;            // Defines the horizontal coordinate of the display.
   int      Y;            // Defines the vertical coordinate of the display.
   int      BmpX;         // The horizontal coordinate of the bitmap within a display.
   int      BmpY;         // The vertical coordinate of the Bitmap within a display.
   OBJECTID BufferID;     // Double buffer bitmap
   int      TotalMemory;  // The total amount of user accessible RAM installed on the video card, or zero if unknown.
   int      MinHScan;     // The minimum horizontal scan rate of the display output device.
   int      MaxHScan;     // The maximum horizontal scan rate of the display output device.
   int      MinVScan;     // The minimum vertical scan rate of the display output device.
   int      MaxVScan;     // The maximum vertical scan rate of the display output device.
   DT       DisplayType;  // Identifies the active display backend.
   DPMS     PowerMode;    // The display's power management method.
   OBJECTID PopOverID;    // Enables pop-over support for hosted display windows.
   int      LeftMargin;   // In hosted mode, indicates the left-hand margin of the client window.
   int      RightMargin;  // In hosted mode, indicates the pixel margin between the client window and right window edge.
   int      TopMargin;    // In hosted mode, indicates the pixel margin between the client window and top window edge.
   int      BottomMargin; // In hosted mode, indicates the bottom margin of the client window.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR focus() noexcept { return Action(AC::Focus, this, nullptr); }
   inline ERR hide() noexcept { return Action(AC::Hide, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR move(double X, double Y, double Z) noexcept {
      struct acMove args = { X, Y, Z };
      return Action(AC::Move, this, &args);
   }
   inline ERR moveToBack() noexcept { return Action(AC::MoveToBack, this, nullptr); }
   inline ERR moveToFront() noexcept { return Action(AC::MoveToFront, this, nullptr); }
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
   inline ERR saveImage(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveImage args = { Dest, { ClassID } };
      return Action(AC::SaveImage, this, &args);
   }
   inline ERR saveSettings() noexcept { return Action(AC::SaveSettings, this, nullptr); }
   inline ERR show() noexcept { return Action(AC::Show, this, nullptr); }
   inline ERR waitVBL() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR updatePalette(struct RGBPalette * NewPalette) noexcept {
      struct gfx::UpdatePalette args = { NewPalette };
      return Action(AC(-2), this, &args);
   }
   inline ERR setDisplay(int X, int Y, int Width, int Height, int InsideWidth, int InsideHeight, int BitsPerPixel, double RefreshRate, int Flags) noexcept {
      struct gfx::SetDisplay args = { X, Y, Width, Height, InsideWidth, InsideHeight, BitsPerPixel, RefreshRate, Flags };
      return Action(AC(-3), this, &args);
   }
   inline ERR sizeHints(int MinWidth, int MinHeight, int MaxWidth, int MaxHeight, int EnforceAspect) noexcept {
      struct gfx::SizeHints args = { MinWidth, MinHeight, MaxWidth, MaxHeight, EnforceAspect };
      return Action(AC(-4), this, &args);
   }
   inline ERR setGamma(double Red, double Green, double Blue, GMF Flags) noexcept {
      struct gfx::SetGamma args = { Red, Green, Blue, Flags };
      return Action(AC(-5), this, &args);
   }
   inline ERR setGammaLinear(double Red, double Green, double Blue, GMF Flags) noexcept {
      struct gfx::SetGammaLinear args = { Red, Green, Blue, Flags };
      return Action(AC(-6), this, &args);
   }
   inline ERR setMonitor(const std::string_view &Name, int MinH, int MaxH, int MinV, int MaxV, MON Flags) noexcept {
      struct gfx::SetMonitor args = { Name, MinH, MaxH, MinV, MaxV, Flags };
      return Action(AC(-7), this, &args);
   }
   inline ERR minimise() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR checkXWindow() noexcept {
      return Action(AC(-9), this, nullptr);
   }
   inline ERR getFrame(int * Left, int * Top, int * Right, int * Bottom) noexcept {
      struct gfx::GetFrame args = { (int)0, (int)0, (int)0, (int)0 };
      ERR error = Action(AC(-10), this, &args);
      if (Left) *Left = args.Left;
      if (Top) *Top = args.Top;
      if (Right) *Right = args.Right;
      if (Bottom) *Bottom = args.Bottom;
      return error;
   }

   // Customised field getting

   inline ERR getRefreshRate(double &Value) noexcept {
      Value = this->RefreshRate;
      return ERR::Okay;
   }

   inline ERR getBitmap(objBitmap * &Value) noexcept {
      Value = this->Bitmap;
      return ERR::Okay;
   }

   inline ERR getFlags(SCR &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getWidth(int &Value) noexcept {
      Value = this->Width;
      return ERR::Okay;
   }

   inline ERR getHeight(int &Value) noexcept {
      Value = this->Height;
      return ERR::Okay;
   }

   inline ERR getX(int &Value) noexcept {
      Value = this->X;
      return ERR::Okay;
   }

   inline ERR getY(int &Value) noexcept {
      Value = this->Y;
      return ERR::Okay;
   }

   inline ERR getBmpX(int &Value) noexcept {
      Value = this->BmpX;
      return ERR::Okay;
   }

   inline ERR getBmpY(int &Value) noexcept {
      Value = this->BmpY;
      return ERR::Okay;
   }

   inline ERR getTotalMemory(int &Value) noexcept {
      Value = this->TotalMemory;
      return ERR::Okay;
   }

   inline ERR getMinHScan(int &Value) noexcept {
      Value = this->MinHScan;
      return ERR::Okay;
   }

   inline ERR getMaxHScan(int &Value) noexcept {
      Value = this->MaxHScan;
      return ERR::Okay;
   }

   inline ERR getMinVScan(int &Value) noexcept {
      Value = this->MinVScan;
      return ERR::Okay;
   }

   inline ERR getMaxVScan(int &Value) noexcept {
      Value = this->MaxVScan;
      return ERR::Okay;
   }

   inline ERR getDisplayType(DT &Value) noexcept {
      Value = this->DisplayType;
      return ERR::Okay;
   }

   inline ERR getPowerMode(DPMS &Value) noexcept {
      Value = this->PowerMode;
      return ERR::Okay;
   }

   inline ERR getLeftMargin(int &Value) noexcept {
      Value = this->LeftMargin;
      return ERR::Okay;
   }

   inline ERR getRightMargin(int &Value) noexcept {
      Value = this->RightMargin;
      return ERR::Okay;
   }

   inline ERR getTopMargin(int &Value) noexcept {
      Value = this->TopMargin;
      return ERR::Okay;
   }

   inline ERR getBottomMargin(int &Value) noexcept {
      Value = this->BottomMargin;
      return ERR::Okay;
   }

   inline ERR getManufacturer(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + 184));
      return ERR::Okay;
   }

   inline ERR getChipset(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + 216));
      return ERR::Okay;
   }

   inline ERR getDisplay(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + 248));
      return ERR::Okay;
   }

   inline ERR getDisplayMfr(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + 280));
      return ERR::Okay;
   }

   inline ERR getOpacity(double &Value) noexcept {
      Value = *((double *)(((int8_t *)this) + 312));
      return ERR::Okay;
   }

   inline ERR getGamma(std::span<double> &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      auto get_field = (ERR (*)(APTR, std::span<double> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getHDensity(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->GetValue(this, &Value);
   }

   inline ERR getVDensity(int &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->GetValue(this, &Value);
   }

   inline ERR getInsideWidth(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->GetValue(this, &Value);
   }

   inline ERR getInsideHeight(int &Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->GetValue(this, &Value);
   }

   inline ERR getResizeFeedback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getWindowHandle(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      return field->GetValue(this, &Value);
   }

   inline ERR getTitle(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getTotalResolutions(int &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setRefreshRate(const double Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setFlags(const SCR Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setWidth(const int Value) noexcept {
      auto field = &this->Class->Dictionary[28];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setHeight(const int Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setX(const int Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setY(const int Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setBmpX(const int Value) noexcept {
      this->BmpX = Value;
      return ERR::Okay;
   }

   inline ERR setBmpY(const int Value) noexcept {
      this->BmpY = Value;
      return ERR::Okay;
   }

   inline ERR setPowerMode(const DPMS Value) noexcept {
      this->PowerMode = Value;
      return ERR::Okay;
   }

   inline ERR setPopOver(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setGamma(std::span<const double> Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, 0x80101508, &Value);
   }

   inline ERR setHDensity(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setVDensity(const int Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setResizeFeedback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setWindowHandle(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(this, field, 0x08100308, Value);
   }

   inline ERR setTitle(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(this, field, 0x00904308, &Value);
   }

};

// Clipboard class definition

#define VER_CLIPBOARD (1.000000)

// Clipboard methods

namespace clip {
struct AddFile { CLIPTYPE Datatype; std::string_view Path; CEF Flags; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddObjects { CLIPTYPE Datatype; OBJECTID *Objects; CEF Flags; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetFiles { CLIPTYPE Filter; int Index; CLIPTYPE Datatype; kt::vector<std::string> *Files; CEF Flags; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddText { std::string_view String; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Remove { CLIPTYPE Datatype; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objClipboard : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CLIPBOARD;
   static constexpr CSTRING CLASS_NAME = "Clipboard";

   using create = kt::Create<objClipboard>;

   CPF Flags;    // Optional clipboard behaviour flags.

#ifdef PRV_CLIPBOARD
   FUNCTION RequestHandler;
#endif

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR addFile(CLIPTYPE Datatype, const std::string_view &Path, CEF Flags) noexcept {
      struct clip::AddFile args = { Datatype, Path, Flags };
      return Action(AC(-1), this, &args);
   }
   inline ERR addObjects(CLIPTYPE Datatype, OBJECTID * Objects, CEF Flags) noexcept {
      struct clip::AddObjects args = { Datatype, Objects, Flags };
      return Action(AC(-2), this, &args);
   }
   inline ERR getFiles(CLIPTYPE Filter, int Index, CLIPTYPE * Datatype, kt::vector<std::string> &Files, CEF * Flags) noexcept {
      struct clip::GetFiles args = { Filter, Index, (CLIPTYPE)0, &Files, (CEF)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Datatype) *Datatype = args.Datatype;
      if (Flags) *Flags = args.Flags;
      return error;
   }
   inline ERR addText(const std::string_view &String) noexcept {
      struct clip::AddText args = { String };
      return Action(AC(-4), this, &args);
   }
   inline ERR remove(CLIPTYPE Datatype) noexcept {
      struct clip::Remove args = { Datatype };
      return Action(AC(-5), this, &args);
   }

   // Customised field getting

   inline ERR getFlags(CPF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getRequestHandler(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }


   // Customised field setting

   inline ERR setFlags(const CPF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setRequestHandler(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

// Controller class definition

#define VER_CONTROLLER (1.000000)

class objController : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONTROLLER;
   static constexpr CSTRING CLASS_NAME = "Controller";

   using create = kt::Create<objController>;

   double LeftTrigger;    // Left trigger value between 0.0 and 1.0.
   double RightTrigger;   // Right trigger value between 0.0 and 1.0.
   double LeftStickX;     // Left analog stick value for X axis, between -1.0 and 1.0.
   double LeftStickY;     // Left analog stick value for Y axis, between -1.0 and 1.0.
   double RightStickX;    // Right analog stick value for X axis, between -1.0 and 1.0.
   double RightStickY;    // Right analog stick value for Y axis, between -1.0 and 1.0.
   CON    Buttons;        // Button values expressed as bit-fields.
   int    Port;           // The port number assigned to the controller.
   public:
   objController() : Port(-1) { }

   // Action stubs

   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getLeftTrigger(double &Value) noexcept {
      Value = this->LeftTrigger;
      return ERR::Okay;
   }

   inline ERR getRightTrigger(double &Value) noexcept {
      Value = this->RightTrigger;
      return ERR::Okay;
   }

   inline ERR getLeftStickX(double &Value) noexcept {
      Value = this->LeftStickX;
      return ERR::Okay;
   }

   inline ERR getLeftStickY(double &Value) noexcept {
      Value = this->LeftStickY;
      return ERR::Okay;
   }

   inline ERR getRightStickX(double &Value) noexcept {
      Value = this->RightStickX;
      return ERR::Okay;
   }

   inline ERR getRightStickY(double &Value) noexcept {
      Value = this->RightStickY;
      return ERR::Okay;
   }

   inline ERR getButtons(CON &Value) noexcept {
      Value = this->Buttons;
      return ERR::Okay;
   }

   inline ERR getPort(int &Value) noexcept {
      Value = this->Port;
      return ERR::Okay;
   }

   inline ERR getTotalPorts(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setPort(const int Value) noexcept {
      this->Port = Value;
      return ERR::Okay;
   }

};

// Pointer class definition

#define VER_POINTER (1.000000)

class objPointer : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::POINTER;
   static constexpr CSTRING CLASS_NAME = "Pointer";

   using create = kt::Create<objPointer>;

   double   Speed;         // Speed multiplier for pointer movement.
   double   Acceleration;  // The rate of acceleration for relative pointer movement.
   double   DoubleClick;   // The maximum interval between two clicks for a double click to be recognised.
   double   WheelSpeed;    // Defines a multiplier to be applied to the mouse wheel.
   double   X;             // The horizontal position of the pointer within its display.
   double   Y;             // The vertical position of the pointer within its display.
   double   OverX;         // The horizontal position of the pointer with respect to the object underneath the hot spot.
   double   OverY;         // The vertical position of the pointer with respect to the object underneath the hot spot.
   double   OverZ;         // The position of the Pointer within an object.
   int      MaxSpeed;      // Restricts the maximum speed of a pointer's movement.
   OBJECTID InputID;       // Declares the I/O object to read movement from.
   OBJECTID SurfaceID;     // The top-most surface that is under the pointer's hot spot.
   OBJECTID AnchorID;      // Can refer to a surface that the pointer has been anchored to.
   PTC      CursorID;      // Identifies the active cursor image.
   OBJECTID CursorOwnerID; // The object that currently owns the cursor state.
   PF       Flags;         // Optional flags.
   OBJECTID RestrictID;    // Refers to a surface when the pointer is restricted.
   int      HostX;         // Indicates the current position of the host cursor on Windows or X11
   int      HostY;         // Indicates the current position of the host cursor on Windows or X11
   objBitmap * Bitmap;     // Refers to bitmap in which custom cursor images can be drawn.
   OBJECTID DragSourceID;  // The object managing the current drag operation, as defined by ~Display.StartCursorDrag().
   int      DragItem;      // The currently dragged item, as defined by ~Display.StartCursorDrag().
   OBJECTID OverObjectID;  // Readable field that gives the ID of the object under the pointer.
   int      ClickSlop;     // A leniency value that assists in determining if the user intended to click or drag.

   // Action stubs

   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR hide() noexcept { return Action(AC::Hide, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR move(double X, double Y, double Z) noexcept {
      struct acMove args = { X, Y, Z };
      return Action(AC::Move, this, &args);
   }
   inline ERR moveToPoint(double X, double Y, double Z, MTF Flags) noexcept {
      struct acMoveToPoint moveto = { X, Y, Z, Flags };
      return Action(AC::MoveToPoint, this, &moveto);
   }
   inline ERR refresh() noexcept { return Action(AC::Refresh, this, nullptr); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR show() noexcept { return Action(AC::Show, this, nullptr); }

   // Customised field getting

   inline ERR getSpeed(double &Value) noexcept {
      Value = this->Speed;
      return ERR::Okay;
   }

   inline ERR getAcceleration(double &Value) noexcept {
      Value = this->Acceleration;
      return ERR::Okay;
   }

   inline ERR getDoubleClick(double &Value) noexcept {
      Value = this->DoubleClick;
      return ERR::Okay;
   }

   inline ERR getWheelSpeed(double &Value) noexcept {
      Value = this->WheelSpeed;
      return ERR::Okay;
   }

   inline ERR getX(double &Value) noexcept {
      Value = this->X;
      return ERR::Okay;
   }

   inline ERR getY(double &Value) noexcept {
      Value = this->Y;
      return ERR::Okay;
   }

   inline ERR getOverX(double &Value) noexcept {
      Value = this->OverX;
      return ERR::Okay;
   }

   inline ERR getOverY(double &Value) noexcept {
      Value = this->OverY;
      return ERR::Okay;
   }

   inline ERR getOverZ(double &Value) noexcept {
      Value = this->OverZ;
      return ERR::Okay;
   }

   inline ERR getMaxSpeed(int &Value) noexcept {
      Value = this->MaxSpeed;
      return ERR::Okay;
   }

   inline ERR getInput(OBJECTID &Value) noexcept {
      Value = this->InputID;
      return ERR::Okay;
   }

   inline ERR getSurface(OBJECTID &Value) noexcept {
      Value = this->SurfaceID;
      return ERR::Okay;
   }

   inline ERR getAnchor(OBJECTID &Value) noexcept {
      Value = this->AnchorID;
      return ERR::Okay;
   }

   inline ERR getCursor(PTC &Value) noexcept {
      Value = this->CursorID;
      return ERR::Okay;
   }

   inline ERR getCursorOwner(OBJECTID &Value) noexcept {
      Value = this->CursorOwnerID;
      return ERR::Okay;
   }

   inline ERR getFlags(PF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getRestrict(OBJECTID &Value) noexcept {
      Value = this->RestrictID;
      return ERR::Okay;
   }

   inline ERR getBitmap(objBitmap * &Value) noexcept {
      Value = this->Bitmap;
      return ERR::Okay;
   }

   inline ERR getDragSource(OBJECTID &Value) noexcept {
      Value = this->DragSourceID;
      return ERR::Okay;
   }

   inline ERR getDragItem(int &Value) noexcept {
      Value = this->DragItem;
      return ERR::Okay;
   }

   inline ERR getOverObject(OBJECTID &Value) noexcept {
      Value = this->OverObjectID;
      return ERR::Okay;
   }

   inline ERR getClickSlop(int &Value) noexcept {
      Value = this->ClickSlop;
      return ERR::Okay;
   }

   inline ERR getButtonState(int &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->GetValue(this, &Value);
   }

   inline ERR getButtonOrder(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }


   // Customised field setting

   inline ERR setSpeed(const double Value) noexcept {
      this->Speed = Value;
      return ERR::Okay;
   }

   inline ERR setAcceleration(const double Value) noexcept {
      this->Acceleration = Value;
      return ERR::Okay;
   }

   inline ERR setDoubleClick(const double Value) noexcept {
      this->DoubleClick = Value;
      return ERR::Okay;
   }

   inline ERR setWheelSpeed(const double Value) noexcept {
      this->WheelSpeed = Value;
      return ERR::Okay;
   }

   inline ERR setX(const double Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setY(const double Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setMaxSpeed(const int Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setInput(OBJECTID Value) noexcept {
      this->InputID = Value;
      return ERR::Okay;
   }

   inline ERR setSurface(OBJECTID Value) noexcept {
      this->SurfaceID = Value;
      return ERR::Okay;
   }

   inline ERR setCursor(const PTC Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->CursorID = Value;
      return ERR::Okay;
   }

   inline ERR setCursorOwner(OBJECTID Value) noexcept {
      this->CursorOwnerID = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const PF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setClickSlop(const int Value) noexcept {
      this->ClickSlop = Value;
      return ERR::Okay;
   }

   inline ERR setButtonOrder(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00904300, &Value);
   }

};

// Surface class definition

#define VER_SURFACE (1.000000)

// Surface methods

namespace drw {
struct InheritedFocus { OBJECTID FocusID; RNF Flags; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ExposeToDisplay { int X; int Y; int Width; int Height; EXF Flags; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InvalidateRegion { int X; int Y; int Width; int Height; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetDisplay { int X; int Y; int Width; int Height; int InsideWidth; int InsideHeight; int BitsPerPixel; double RefreshRate; int Flags; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetOpacity { double Value; double Adjustment; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddCallback { FUNCTION *Callback; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Minimise { static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveCallback { FUNCTION *Callback; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ScheduleRedraw { int RefreshRate; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objSurface : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SURFACE;
   static constexpr CSTRING CLASS_NAME = "Surface";

   using create = kt::Create<objSurface>;

   Unit     X;          // Determines the horizontal position of a surface object.
   Unit     Y;          // Determines the vertical position of a surface object.
   Unit     Width;      // Defines the width of a surface object.
   Unit     Height;     // Defines the height of a surface object.
   Unit     XOffset;    // Determines the horizontal offset of a surface object.
   Unit     YOffset;    // Determines the vertical offset of a surface object.
   OBJECTID DragID;     // Defines the Surface that moves when this surface is dragged.
   OBJECTID BufferID;   // Refers to the Bitmap that stores the surface's graphics.
   OBJECTID ParentID;   // Identifies the parent Surface.
   OBJECTID PopOverID;  // Keeps a surface in front of another surface in the Z order.
   int      MinWidth;   // Prevents the width of a surface object from shrinking beyond a certain value.
   int      MinHeight;  // Prevents the height of a surface object from shrinking beyond a certain value.
   int      MaxWidth;   // Prevents the width of a surface object from exceeding a certain value.
   int      MaxHeight;  // Prevents the height of a surface object from exceeding a certain value.
   OBJECTID DisplayID;  // Refers to the Display object that manages the surface's graphics.
   RNF      Flags;      // Controls optional surface behaviour.
   OBJECTID RootID;     // Surface that is acting as a root for many surface children (useful when applying translucency)
   DRAG     DragStatus; // Reports the current drag state when dragging is enabled.
   PTC      Cursor;     // Sets the pointer image used while the mouse is over the surface.
   struct RGB8 Colour;  // Defines the background colour used when clearing the surface.
   RT       Type;       // Internal surface type flags
   int      Modal;      // Sets the surface as modal (prevents user interaction with other surfaces).
   public:
   inline bool visible() const { return (Flags & RNF::VISIBLE) != RNF::NIL; }
   inline bool invisible() const { return (Flags & RNF::VISIBLE) IS RNF::NIL; }
   inline bool hasFocus() const { return (Flags & RNF::HAS_FOCUS) != RNF::NIL; }
   inline bool transparent() const { return (Flags & RNF::TRANSPARENT) != RNF::NIL; }
   inline bool disabled() const { return (Flags & RNF::DISABLED) != RNF::NIL; }
   inline bool isCursor() const { return (Flags & RNF::CURSOR) != RNF::NIL; }

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR focus() noexcept { return Action(AC::Focus, this, nullptr); }
   inline ERR hide() noexcept { return Action(AC::Hide, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR lostFocus() noexcept { return Action(AC::LostFocus, this, nullptr); }
   inline ERR move(double X, double Y, double Z) noexcept {
      struct acMove args = { X, Y, Z };
      return Action(AC::Move, this, &args);
   }
   inline ERR moveToBack() noexcept { return Action(AC::MoveToBack, this, nullptr); }
   inline ERR moveToFront() noexcept { return Action(AC::MoveToFront, this, nullptr); }
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
   inline ERR saveImage(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveImage args = { Dest, { ClassID } };
      return Action(AC::SaveImage, this, &args);
   }
   inline ERR show() noexcept { return Action(AC::Show, this, nullptr); }
   inline ERR inheritedFocus(OBJECTID FocusID, RNF Flags) noexcept {
      struct drw::InheritedFocus args = { FocusID, Flags };
      return Action(AC(-1), this, &args);
   }
   inline ERR exposeToDisplay(int X, int Y, int Width, int Height, EXF Flags) noexcept {
      struct drw::ExposeToDisplay args = { X, Y, Width, Height, Flags };
      return Action(AC(-2), this, &args);
   }
   inline ERR invalidateRegion(int X, int Y, int Width, int Height) noexcept {
      struct drw::InvalidateRegion args = { X, Y, Width, Height };
      return Action(AC(-3), this, &args);
   }
   inline ERR setDisplay(int X, int Y, int Width, int Height, int InsideWidth, int InsideHeight, int BitsPerPixel, double RefreshRate, int Flags) noexcept {
      struct drw::SetDisplay args = { X, Y, Width, Height, InsideWidth, InsideHeight, BitsPerPixel, RefreshRate, Flags };
      return Action(AC(-4), this, &args);
   }
   inline ERR setOpacity(double Value, double Adjustment) noexcept {
      struct drw::SetOpacity args = { Value, Adjustment };
      return Action(AC(-5), this, &args);
   }
   inline ERR addCallback(FUNCTION Callback) noexcept {
      struct drw::AddCallback args = { &Callback };
      return Action(AC(-6), this, &args);
   }
   inline ERR minimise() noexcept {
      return Action(AC(-7), this, nullptr);
   }
   inline ERR removeCallback(FUNCTION Callback) noexcept {
      struct drw::RemoveCallback args = { &Callback };
      return Action(AC(-8), this, &args);
   }
   inline ERR scheduleRedraw(int RefreshRate) noexcept {
      struct drw::ScheduleRedraw args = { RefreshRate };
      return Action(AC(-9), this, &args);
   }

   // Customised field getting

   inline ERR getX(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->GetValue(this, &Value);
   }

   inline ERR getY(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->GetValue(this, &Value);
   }

   inline ERR getWidth(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[26];
      return field->GetValue(this, &Value);
   }

   inline ERR getHeight(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->GetValue(this, &Value);
   }

   inline ERR getXOffset(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->GetValue(this, &Value);
   }

   inline ERR getYOffset(Unit &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->GetValue(this, &Value);
   }

   inline ERR getDrag(OBJECTID &Value) noexcept {
      Value = this->DragID;
      return ERR::Okay;
   }

   inline ERR getBuffer(OBJECTID &Value) noexcept {
      Value = this->BufferID;
      return ERR::Okay;
   }

   inline ERR getParent(OBJECTID &Value) noexcept {
      Value = this->ParentID;
      return ERR::Okay;
   }

   inline ERR getPopOver(OBJECTID &Value) noexcept {
      Value = this->PopOverID;
      return ERR::Okay;
   }

   inline ERR getMinWidth(int &Value) noexcept {
      Value = this->MinWidth;
      return ERR::Okay;
   }

   inline ERR getMinHeight(int &Value) noexcept {
      Value = this->MinHeight;
      return ERR::Okay;
   }

   inline ERR getMaxWidth(int &Value) noexcept {
      Value = this->MaxWidth;
      return ERR::Okay;
   }

   inline ERR getMaxHeight(int &Value) noexcept {
      Value = this->MaxHeight;
      return ERR::Okay;
   }

   inline ERR getDisplay(OBJECTID &Value) noexcept {
      Value = this->DisplayID;
      return ERR::Okay;
   }

   inline ERR getFlags(RNF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getDragStatus(DRAG &Value) noexcept {
      Value = this->DragStatus;
      return ERR::Okay;
   }

   inline ERR getCursor(PTC &Value) noexcept {
      Value = this->Cursor;
      return ERR::Okay;
   }

   inline ERR getColour(struct RGB8 * &Value) noexcept {
      Value = &this->Colour;
      return ERR::Okay;
   }

   inline ERR getModal(int &Value) noexcept {
      Value = this->Modal;
      return ERR::Okay;
   }

   inline ERR getWindowType(SWIN &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->GetValue(this, &Value);
   }

   inline ERR getAbsX(int &Value) noexcept {
      auto field = &this->Class->Dictionary[29];
      return field->GetValue(this, &Value);
   }

   inline ERR getAbsY(int &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->GetValue(this, &Value);
   }

   inline ERR getBitsPerPixel(int &Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->GetValue(this, &Value);
   }

   inline ERR getBottom(int &Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      return field->GetValue(this, &Value);
   }

   inline ERR getOpacity(double &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->GetValue(this, &Value);
   }

   inline ERR getRight(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->GetValue(this, &Value);
   }

   inline ERR getUserFocus(int &Value) noexcept {
      auto field = &this->Class->Dictionary[36];
      return field->GetValue(this, &Value);
   }

   inline ERR getVisible(int &Value) noexcept {
      auto field = &this->Class->Dictionary[28];
      return field->GetValue(this, &Value);
   }

   inline ERR getWindowHandle(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setX(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, FD_UNIT, &Value);
   }

   inline ERR setY(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_UNIT, &Value);
   }

   inline ERR setWidth(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[26];
      return field->WriteValue(this, field, FD_UNIT, &Value);
   }

   inline ERR setHeight(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->WriteValue(this, field, FD_UNIT, &Value);
   }

   inline ERR setXOffset(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_UNIT, &Value);
   }

   inline ERR setYOffset(const Unit Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_UNIT, &Value);
   }

   inline ERR setDrag(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setParent(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[37];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setPopOver(OBJECTID Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setMinWidth(const int Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setMinHeight(const int Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setMaxWidth(const int Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setMaxHeight(const int Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFlags(const RNF Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setCursor(const PTC Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setColour(const struct RGB8 Value) noexcept {
      this->Colour = Value;
      return ERR::Okay;
   }

   inline ERR setModal(const int Value) noexcept {
      auto field = &this->Class->Dictionary[32];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setWindowType(const SWIN Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setAbsX(const int Value) noexcept {
      auto field = &this->Class->Dictionary[29];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setAbsY(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setBitsPerPixel(const int Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setVisible(const int Value) noexcept {
      auto field = &this->Class->Dictionary[28];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setWindowHandle(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      return field->WriteValue(this, field, 0x08100308, Value);
   }

};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_DISPLAY [[maybe_unused]] static struct DisplayBase *DisplayBase = nullptr;
#else
#define JUMPTABLE_DISPLAY struct DisplayBase *DisplayBase = nullptr;
#endif

struct DisplayBase {
#ifndef KOTUKU_STATIC
   objPointer * (*_AccessPointer)(void);
   ERR (*_CheckIfChild)(OBJECTID Parent, OBJECTID Child);
   ERR (*_CopyArea)(objBitmap *Bitmap, objBitmap *Dest, BAF Flags, int X, int Y, int Width, int Height, int XDest, int YDest);
   ERR (*_CopyRawBitmap)(struct BitmapSurface *Surface, objBitmap *Dest, CSRF Flags, int X, int Y, int Width, int Height, int XDest, int YDest);
   ERR (*_CopySurface)(OBJECTID Surface, objBitmap *Bitmap, BDF Flags, int X, int Y, int Width, int Height, int XDest, int YDest);
   void (*_DrawPixel)(objBitmap *Bitmap, int X, int Y, uint32_t Colour);
   void (*_DrawRGBPixel)(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB);
   void (*_DrawRectangle)(objBitmap *Bitmap, int X, int Y, int Width, int Height, uint32_t Colour, BAF Flags);
   ERR (*_ExposeSurface)(OBJECTID Surface, int X, int Y, int Width, int Height, EXF Flags);
   void (*_GetColourFormat)(struct ColourFormat *Format, int BitsPerPixel, int RedMask, int GreenMask, int BlueMask, int AlphaMask);
   ERR (*_GetCursorInfo)(struct CursorInfo *Info, int Size);
   ERR (*_GetCursorPos)(double *X, double *Y);
   ERR (*_GetDisplayInfo)(OBJECTID Display, struct DisplayInfo **Info);
   DT (*_GetDisplayType)(void);
   CSTRING (*_GetInputTypeName)(JET Type);
   OBJECTID (*_GetModalSurface)(void);
   ERR (*_GetRelativeCursorPos)(OBJECTID Surface, double *X, double *Y);
   ERR (*_GetSurfaceCoords)(OBJECTID Surface, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height);
   ERR (*_GetSurfaceFlags)(OBJECTID Surface, RNF *Flags);
   ERR (*_GetSurfaceInfo)(OBJECTID Surface, struct SurfaceInfo **Info);
   OBJECTID (*_GetUserFocus)(void);
   ERR (*_GetVisibleArea)(OBJECTID Surface, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height);
   ERR (*_LockCursor)(OBJECTID Surface);
   uint32_t (*_ReadPixel)(objBitmap *Bitmap, int X, int Y);
   void (*_ReadRGBPixel)(objBitmap *Bitmap, int X, int Y, struct RGB8 **RGB);
   ERR (*_Resample)(objBitmap *Bitmap, struct ColourFormat *ColourFormat);
   ERR (*_RestoreCursor)(PTC Cursor, OBJECTID Owner);
   double (*_ScaleToDPI)(double Value);
   ERR (*_ScanDisplayModes)(const std::string_view &Filter, struct DisplayInfo *Info);
   void (*_SetClipRegion)(objBitmap *Bitmap, int Left, int Top, int Right, int Bottom);
   ERR (*_SetCursor)(OBJECTID Surface, CRF Flags, PTC Cursor, const std::string_view &Name, OBJECTID Owner);
   ERR (*_SetCursorPos)(double X, double Y);
   ERR (*_SetCustomCursor)(OBJECTID Surface, CRF Flags, objBitmap *Bitmap, int HotX, int HotY, OBJECTID Owner);
   ERR (*_SetHostOption)(HOST Option, int64_t Value);
   OBJECTID (*_SetModalSurface)(OBJECTID Surface);
   ERR (*_StartCursorDrag)(OBJECTID Source, int Item, const std::string_view &Datatypes, OBJECTID Surface);
   ERR (*_SubscribeInput)(FUNCTION *Callback, OBJECTID SurfaceFilter, JTYPE Mask, OBJECTID DeviceFilter, int *Handle);
   void (*_Sync)(objBitmap *Bitmap);
   ERR (*_UnlockCursor)(OBJECTID Surface);
   ERR (*_UnsubscribeInput)(int Handle);
   ERR (*_WindowHook)(OBJECTID SurfaceID, WH Event, FUNCTION *Callback);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_DISPLAY_MODULE)
extern struct DisplayBase *DisplayBase;
namespace gfx {
inline objPointer * AccessPointer(void) { return DisplayBase->_AccessPointer(); }
inline ERR CheckIfChild(OBJECTID Parent, OBJECTID Child) { return DisplayBase->_CheckIfChild(Parent,Child); }
inline ERR CopyArea(objBitmap *Bitmap, objBitmap *Dest, BAF Flags, int X, int Y, int Width, int Height, int XDest, int YDest) { return DisplayBase->_CopyArea(Bitmap,Dest,Flags,X,Y,Width,Height,XDest,YDest); }
inline ERR CopyRawBitmap(struct BitmapSurface *Surface, objBitmap *Dest, CSRF Flags, int X, int Y, int Width, int Height, int XDest, int YDest) { return DisplayBase->_CopyRawBitmap(Surface,Dest,Flags,X,Y,Width,Height,XDest,YDest); }
inline ERR CopySurface(OBJECTID Surface, objBitmap *Bitmap, BDF Flags, int X, int Y, int Width, int Height, int XDest, int YDest) { return DisplayBase->_CopySurface(Surface,Bitmap,Flags,X,Y,Width,Height,XDest,YDest); }
inline void DrawPixel(objBitmap *Bitmap, int X, int Y, uint32_t Colour) { return DisplayBase->_DrawPixel(Bitmap,X,Y,Colour); }
inline void DrawRGBPixel(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB) { return DisplayBase->_DrawRGBPixel(Bitmap,X,Y,RGB); }
inline void DrawRectangle(objBitmap *Bitmap, int X, int Y, int Width, int Height, uint32_t Colour, BAF Flags) { return DisplayBase->_DrawRectangle(Bitmap,X,Y,Width,Height,Colour,Flags); }
inline ERR ExposeSurface(OBJECTID Surface, int X, int Y, int Width, int Height, EXF Flags) { return DisplayBase->_ExposeSurface(Surface,X,Y,Width,Height,Flags); }
inline void GetColourFormat(struct ColourFormat *Format, int BitsPerPixel, int RedMask, int GreenMask, int BlueMask, int AlphaMask) { return DisplayBase->_GetColourFormat(Format,BitsPerPixel,RedMask,GreenMask,BlueMask,AlphaMask); }
inline ERR GetCursorInfo(struct CursorInfo *Info, int Size) { return DisplayBase->_GetCursorInfo(Info,Size); }
inline ERR GetCursorPos(double *X, double *Y) { return DisplayBase->_GetCursorPos(X,Y); }
inline ERR GetDisplayInfo(OBJECTID Display, struct DisplayInfo **Info) { return DisplayBase->_GetDisplayInfo(Display,Info); }
inline DT GetDisplayType(void) { return DisplayBase->_GetDisplayType(); }
inline CSTRING GetInputTypeName(JET Type) { return DisplayBase->_GetInputTypeName(Type); }
inline OBJECTID GetModalSurface(void) { return DisplayBase->_GetModalSurface(); }
inline ERR GetRelativeCursorPos(OBJECTID Surface, double *X, double *Y) { return DisplayBase->_GetRelativeCursorPos(Surface,X,Y); }
inline ERR GetSurfaceCoords(OBJECTID Surface, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height) { return DisplayBase->_GetSurfaceCoords(Surface,X,Y,AbsX,AbsY,Width,Height); }
inline ERR GetSurfaceFlags(OBJECTID Surface, RNF *Flags) { return DisplayBase->_GetSurfaceFlags(Surface,Flags); }
inline ERR GetSurfaceInfo(OBJECTID Surface, struct SurfaceInfo **Info) { return DisplayBase->_GetSurfaceInfo(Surface,Info); }
inline OBJECTID GetUserFocus(void) { return DisplayBase->_GetUserFocus(); }
inline ERR GetVisibleArea(OBJECTID Surface, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height) { return DisplayBase->_GetVisibleArea(Surface,X,Y,AbsX,AbsY,Width,Height); }
inline ERR LockCursor(OBJECTID Surface) { return DisplayBase->_LockCursor(Surface); }
inline uint32_t ReadPixel(objBitmap *Bitmap, int X, int Y) { return DisplayBase->_ReadPixel(Bitmap,X,Y); }
inline void ReadRGBPixel(objBitmap *Bitmap, int X, int Y, struct RGB8 **RGB) { return DisplayBase->_ReadRGBPixel(Bitmap,X,Y,RGB); }
inline ERR Resample(objBitmap *Bitmap, struct ColourFormat *ColourFormat) { return DisplayBase->_Resample(Bitmap,ColourFormat); }
inline ERR RestoreCursor(PTC Cursor, OBJECTID Owner) { return DisplayBase->_RestoreCursor(Cursor,Owner); }
inline double ScaleToDPI(double Value) { return DisplayBase->_ScaleToDPI(Value); }
inline ERR ScanDisplayModes(const std::string_view &Filter, struct DisplayInfo *Info) { return DisplayBase->_ScanDisplayModes(Filter,Info); }
inline void SetClipRegion(objBitmap *Bitmap, int Left, int Top, int Right, int Bottom) { return DisplayBase->_SetClipRegion(Bitmap,Left,Top,Right,Bottom); }
inline ERR SetCursor(OBJECTID Surface, CRF Flags, PTC Cursor, const std::string_view &Name, OBJECTID Owner) { return DisplayBase->_SetCursor(Surface,Flags,Cursor,Name,Owner); }
inline ERR SetCursorPos(double X, double Y) { return DisplayBase->_SetCursorPos(X,Y); }
inline ERR SetCustomCursor(OBJECTID Surface, CRF Flags, objBitmap *Bitmap, int HotX, int HotY, OBJECTID Owner) { return DisplayBase->_SetCustomCursor(Surface,Flags,Bitmap,HotX,HotY,Owner); }
inline ERR SetHostOption(HOST Option, int64_t Value) { return DisplayBase->_SetHostOption(Option,Value); }
inline OBJECTID SetModalSurface(OBJECTID Surface) { return DisplayBase->_SetModalSurface(Surface); }
inline ERR StartCursorDrag(OBJECTID Source, int Item, const std::string_view &Datatypes, OBJECTID Surface) { return DisplayBase->_StartCursorDrag(Source,Item,Datatypes,Surface); }
inline ERR SubscribeInput(FUNCTION *Callback, OBJECTID SurfaceFilter, JTYPE Mask, OBJECTID DeviceFilter, int *Handle) { return DisplayBase->_SubscribeInput(Callback,SurfaceFilter,Mask,DeviceFilter,Handle); }
inline void Sync(objBitmap *Bitmap) { return DisplayBase->_Sync(Bitmap); }
inline ERR UnlockCursor(OBJECTID Surface) { return DisplayBase->_UnlockCursor(Surface); }
inline ERR UnsubscribeInput(int Handle) { return DisplayBase->_UnsubscribeInput(Handle); }
inline ERR WindowHook(OBJECTID SurfaceID, WH Event, FUNCTION *Callback) { return DisplayBase->_WindowHook(SurfaceID,Event,Callback); }
} // namespace
#else
namespace gfx {
extern objPointer * AccessPointer(void);
extern ERR CheckIfChild(OBJECTID Parent, OBJECTID Child);
extern ERR CopyArea(objBitmap *Bitmap, objBitmap *Dest, BAF Flags, int X, int Y, int Width, int Height, int XDest, int YDest);
extern ERR CopyRawBitmap(struct BitmapSurface *Surface, objBitmap *Dest, CSRF Flags, int X, int Y, int Width, int Height, int XDest, int YDest);
extern ERR CopySurface(OBJECTID Surface, objBitmap *Bitmap, BDF Flags, int X, int Y, int Width, int Height, int XDest, int YDest);
extern void DrawPixel(objBitmap *Bitmap, int X, int Y, uint32_t Colour);
extern void DrawRGBPixel(objBitmap *Bitmap, int X, int Y, struct RGB8 *RGB);
extern void DrawRectangle(objBitmap *Bitmap, int X, int Y, int Width, int Height, uint32_t Colour, BAF Flags);
extern ERR ExposeSurface(OBJECTID Surface, int X, int Y, int Width, int Height, EXF Flags);
extern void GetColourFormat(struct ColourFormat *Format, int BitsPerPixel, int RedMask, int GreenMask, int BlueMask, int AlphaMask);
extern ERR GetCursorInfo(struct CursorInfo *Info, int Size);
extern ERR GetCursorPos(double *X, double *Y);
extern ERR GetDisplayInfo(OBJECTID Display, struct DisplayInfo **Info);
extern DT GetDisplayType(void);
extern CSTRING GetInputTypeName(JET Type);
extern OBJECTID GetModalSurface(void);
extern ERR GetRelativeCursorPos(OBJECTID Surface, double *X, double *Y);
extern ERR GetSurfaceCoords(OBJECTID Surface, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height);
extern ERR GetSurfaceFlags(OBJECTID Surface, RNF *Flags);
extern ERR GetSurfaceInfo(OBJECTID Surface, struct SurfaceInfo **Info);
extern OBJECTID GetUserFocus(void);
extern ERR GetVisibleArea(OBJECTID Surface, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height);
extern ERR LockCursor(OBJECTID Surface);
extern uint32_t ReadPixel(objBitmap *Bitmap, int X, int Y);
extern void ReadRGBPixel(objBitmap *Bitmap, int X, int Y, struct RGB8 **RGB);
extern ERR Resample(objBitmap *Bitmap, struct ColourFormat *ColourFormat);
extern ERR RestoreCursor(PTC Cursor, OBJECTID Owner);
extern double ScaleToDPI(double Value);
extern ERR ScanDisplayModes(const std::string_view &Filter, struct DisplayInfo *Info);
extern void SetClipRegion(objBitmap *Bitmap, int Left, int Top, int Right, int Bottom);
extern ERR SetCursor(OBJECTID Surface, CRF Flags, PTC Cursor, const std::string_view &Name, OBJECTID Owner);
extern ERR SetCursorPos(double X, double Y);
extern ERR SetCustomCursor(OBJECTID Surface, CRF Flags, objBitmap *Bitmap, int HotX, int HotY, OBJECTID Owner);
extern ERR SetHostOption(HOST Option, int64_t Value);
extern OBJECTID SetModalSurface(OBJECTID Surface);
extern ERR StartCursorDrag(OBJECTID Source, int Item, const std::string_view &Datatypes, OBJECTID Surface);
extern ERR SubscribeInput(FUNCTION *Callback, OBJECTID SurfaceFilter, JTYPE Mask, OBJECTID DeviceFilter, int *Handle);
extern void Sync(objBitmap *Bitmap);
extern ERR UnlockCursor(OBJECTID Surface);
extern ERR UnsubscribeInput(int Handle);
extern ERR WindowHook(OBJECTID SurfaceID, WH Event, FUNCTION *Callback);
} // namespace
#endif // KOTUKU_STATIC

// Direct ColourFormat versions

#define CFPackPixel(a,b,c,d)      ((((b)>>(a)->RedShift) & (a)->RedMask) << (a)->RedPos) | ((((c)>>(a)->GreenShift) & (a)->GreenMask) << (a)->GreenPos) | ((((d)>>(a)->BlueShift) & (a)->BlueMask) << (a)->BluePos)
#define CFPackPixelA(a,b,c,d,e)   ((((b)>>(a)->RedShift) & (a)->RedMask) << (a)->RedPos) | ((((c)>>(a)->GreenShift) & (a)->GreenMask) << (a)->GreenPos) | ((((d)>>(a)->BlueShift) & (a)->BlueMask) << (a)->BluePos) | ((((e)>>(a)->AlphaShift) & (a)->AlphaMask) << (a)->AlphaPos)
#define CFPackAlpha(a,b)          ((((b)>>(a)->AlphaShift) & (a)->AlphaMask) << (a)->AlphaPos)
#define CFPackPixelWB(a,b,c,d)    ((b) << (a)->RedPos) | ((c) << (a)->GreenPos) | ((d) << (a)->BluePos)
#define CFPackPixelWBA(a,b,c,d,e) ((b) << (a)->RedPos) | ((c) << (a)->GreenPos) | ((d) << (a)->BluePos) | ((e) << (a)->AlphaPos)
#define CFUnpackRed(a,b)          ((((b) >> (a)->RedPos) & (a)->RedMask) << (a)->RedShift)
#define CFUnpackGreen(a,b)        ((((b) >> (a)->GreenPos) & (a)->GreenMask) << (a)->GreenShift)
#define CFUnpackBlue(a,b)         ((((b) >> (a)->BluePos) & (a)->BlueMask) << (a)->BlueShift)
#define CFUnpackAlpha(a,b)        ((((b) >> (a)->AlphaPos) & (a)->AlphaMask))

namespace fl {
   using namespace kt;

constexpr FieldValue WindowType(SWIN Value) { return FieldValue(FID_WindowType, int(Value)); }

} // namespace
