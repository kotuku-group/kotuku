#pragma once

// Name:      font.h
// Copyright: Paul Manias © 1998-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_FONT (1)

#include <kotuku/modules/display.h>

class objFont;

// Font flags

enum class FTF : uint32_t {
   NIL = 0,
   HEAVY_LINE = 0x00000001,
   BASE_LINE = 0x00000002,
   BOLD = 0x20000000,
   ITALIC = 0x40000000,
};

DEFINE_ENUM_FLAG_OPERATORS(FTF)

// Result flags for the SelectFont() function.

enum class FMETA : uint32_t {
   NIL = 0,
   SCALED = 0x00000001,
   VARIABLE = 0x00000002,
   HINT_NORMAL = 0x00000004,
   HINT_LIGHT = 0x00000008,
   HINT_INTERNAL = 0x00000010,
   HIDDEN = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(FMETA)

// Force hinting options for a font.

enum class HINT : int8_t {
   NIL = 0,
   NORMAL = 1,
   INTERNAL = 2,
   LIGHT = 3,
};

// Options for the StringSize() function.

#define FSS_ALL -1
#define FSS_LINE -2

struct FontList {
   struct FontList * Next;    // Pointer to the next entry in the list.
   STRING Name;               // The name of the font face.
   STRING Alias;              // Reference to another font Name if this is an alias.
   int *  Points;             // Pointer to an array of fixed point sizes supported by the font.
   STRING Styles;             // Supported styles are listed here in CSV format.
   STRING Axes;               // For variable fonts, lists all supported axis codes in CSV format
   int8_t Scalable;           // TRUE if the font is scalable.
   int8_t Variable;           // TRUE if the font has variable metrics.
   HINT   Hinting;            // Hinting options
   int8_t Hidden;             // TRUE if the font should be hidden from user font lists.
};

// Font class definition

#define VER_FONT (1.000000)

class objFont : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FONT;
   static constexpr CSTRING CLASS_NAME = "Font";

   using create = pf::Create<objFont>;

   double Point;           // The point size of a font.
   double GlyphSpacing;    // Adjusts the amount of spacing between each character.
   objBitmap * Bitmap;     // The destination Bitmap to use when drawing a font.
   STRING String;          // The string to use when drawing a Font.
   STRING Path;            // The path to a font file.
   STRING Style;           // Determines font styling.
   STRING Face;            // The name of a font face that is to be loaded on initialisation.
   struct RGB8 Outline;    // Defines the outline colour around a font.
   struct RGB8 Underline;  // Enables font underlining when set.
   struct RGB8 Colour;     // The font colour in RGB8 format.
   FTF    Flags;           // Optional flags.
   int    Gutter;          // The 'external leading' value, measured in pixels.  Applies to fixed fonts only.
   int    LineSpacing;     // The amount of spacing between each line.
   int    X;               // The starting horizontal position when drawing the font string.
   int    Y;               // The starting vertical position when drawing the font string.
   int    TabSize;         // Defines the tab size to use when drawing and manipulating a font string.
   int    WrapEdge;        // Enables word wrapping at a given boundary.
   int    FixedWidth;      // Forces a fixed pixel width to use for all glyphs.
   int    Height;          // The point size of the font, expressed in pixels.
   int    Leading;         // 'Internal leading' measured in pixels.  Applies to fixed fonts only.
   int    MaxHeight;       // The maximum possible pixel height per character.
   ALIGN  Align;           // Sets the position of a font string to an abstract alignment.
   int    AlignWidth;      // The width to use when aligning the font string.
   int    AlignHeight;     // The height to use when aligning the font string.
   int    Ascent;          // The total number of pixels above the baseline.
   int    EndX;            // Indicates the final horizontal coordinate after completing a draw operation.
   int    EndY;            // Indicates the final vertical coordinate after completing a draw operation.

   // Action stubs

   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field setting

   inline ERR setPoint(const double Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

   inline ERR setGlyphSpacing(const double Value) noexcept {
      this->GlyphSpacing = Value;
      return ERR::Okay;
   }

   inline ERR setBitmap(objBitmap * Value) noexcept {
      this->Bitmap = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setString(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[26];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setStyle(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   template <class T> inline ERR setFace(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   inline ERR setOutline(const struct RGB8 Value) noexcept {
      this->Outline = Value;
      return ERR::Okay;
   }

   inline ERR setUnderline(const struct RGB8 Value) noexcept {
      this->Underline = Value;
      return ERR::Okay;
   }

   inline ERR setColour(const struct RGB8 Value) noexcept {
      this->Colour = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const FTF Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setGutter(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Gutter = Value;
      return ERR::Okay;
   }

   inline ERR setLineSpacing(const int Value) noexcept {
      this->LineSpacing = Value;
      return ERR::Okay;
   }

   inline ERR setX(const int Value) noexcept {
      this->X = Value;
      return ERR::Okay;
   }

   inline ERR setY(const int Value) noexcept {
      this->Y = Value;
      return ERR::Okay;
   }

   inline ERR setTabSize(const int Value) noexcept {
      this->TabSize = Value;
      return ERR::Okay;
   }

   inline ERR setWrapEdge(const int Value) noexcept {
      this->WrapEdge = Value;
      return ERR::Okay;
   }

   inline ERR setFixedWidth(const int Value) noexcept {
      this->FixedWidth = Value;
      return ERR::Okay;
   }

   inline ERR setHeight(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Height = Value;
      return ERR::Okay;
   }

   inline ERR setMaxHeight(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->MaxHeight = Value;
      return ERR::Okay;
   }

   inline ERR setAlign(const ALIGN Value) noexcept {
      this->Align = Value;
      return ERR::Okay;
   }

   inline ERR setAlignWidth(const int Value) noexcept {
      this->AlignWidth = Value;
      return ERR::Okay;
   }

   inline ERR setAlignHeight(const int Value) noexcept {
      this->AlignHeight = Value;
      return ERR::Okay;
   }

   inline ERR setEndX(const int Value) noexcept {
      this->EndX = Value;
      return ERR::Okay;
   }

   inline ERR setEndY(const int Value) noexcept {
      this->EndY = Value;
      return ERR::Okay;
   }

   inline ERR setBold(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setItalic(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setOpacity(const double Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(target, field, FD_DOUBLE, &Value, 1);
   }

};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_FONT [[maybe_unused]] static struct FontBase *FontBase = nullptr;
#else
#define JUMPTABLE_FONT struct FontBase *FontBase = nullptr;
#endif

struct FontBase {
#ifndef KOTUKU_STATIC
   ERR (*_GetList)(struct FontList **Result);
   int (*_StringWidth)(objFont *Font, CSTRING String, int Chars);
   int (*_CharWidth)(objFont *Font, uint32_t Char);
   ERR (*_RefreshFonts)(void);
   ERR (*_SelectFont)(CSTRING Name, CSTRING Style, CSTRING *Path, FMETA *Meta);
   ERR (*_ResolveFamilyName)(CSTRING String, CSTRING *Result);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_FONT_MODULE)
extern struct FontBase *FontBase;
namespace fnt {
inline ERR GetList(struct FontList **Result) { return FontBase->_GetList(Result); }
inline int StringWidth(objFont *Font, CSTRING String, int Chars) { return FontBase->_StringWidth(Font,String,Chars); }
inline int CharWidth(objFont *Font, uint32_t Char) { return FontBase->_CharWidth(Font,Char); }
inline ERR RefreshFonts(void) { return FontBase->_RefreshFonts(); }
inline ERR SelectFont(CSTRING Name, CSTRING Style, CSTRING *Path, FMETA *Meta) { return FontBase->_SelectFont(Name,Style,Path,Meta); }
inline ERR ResolveFamilyName(CSTRING String, CSTRING *Result) { return FontBase->_ResolveFamilyName(String,Result); }
} // namespace
#else
namespace fnt {
extern ERR GetList(struct FontList **Result);
extern int StringWidth(objFont *Font, CSTRING String, int Chars);
extern int CharWidth(objFont *Font, uint32_t Char);
extern ERR RefreshFonts(void);
extern ERR SelectFont(CSTRING Name, CSTRING Style, CSTRING *Path, FMETA *Meta);
extern ERR ResolveFamilyName(CSTRING String, CSTRING *Result);
} // namespace
#endif // KOTUKU_STATIC

