#pragma once

// Name:      scintilla.h
// Copyright: Paul Manias © 2005-2025
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_SCINTILLA (1)

#include <kotuku/modules/display.h>
#include <kotuku/modules/font.h>

class objScintilla;
class objScintillaSearch;

// Scintilla Lexers.  These codes originate from the Scintilla library.

enum class SCLEX : int {
   NIL = 0,
   ERRORLIST = 10,
   MAKEFILE = 11,
   BATCH = 12,
   TIRI = 15,
   DIFF = 16,
   PASCAL = 18,
   RUBY = 22,
   VBSCRIPT = 28,
   ASP = 29,
   PYTHON = 2,
   ASSEMBLER = 34,
   CSS = 38,
   CPP = 3,
   HTML = 4,
   XML = 5,
   BASH = 62,
   PHPSCRIPT = 69,
   PERL = 6,
   REBOL = 71,
   SQL = 7,
   VB = 8,
   PROPERTIES = 9,
};

// Optional flags.

enum class SCIF : uint32_t {
   NIL = 0,
   DISABLED = 0x00000001,
   DETECT_LEXER = 0x00000002,
   EDIT = 0x00000004,
   EXT_PAGE = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(SCIF)

// Flags for EventCallback and EventFlags

enum class SEF : uint32_t {
   NIL = 0,
   MODIFIED = 0x00000001,
   CURSOR_POS = 0x00000002,
   FAIL_RO = 0x00000004,
   NEW_CHAR = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(SEF)

// Scintilla search flags.

enum class STF : uint32_t {
   NIL = 0,
   CASE = 0x00000001,
   MOVE_CURSOR = 0x00000002,
   SCAN_SELECTION = 0x00000004,
   BACKWARDS = 0x00000008,
   EXPRESSION = 0x00000010,
   WRAP = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(STF)

// Scintilla class definition

#define VER_SCINTILLA (1.000000)

// Scintilla methods

namespace sci {
struct SetFont { CSTRING Face; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReplaceText { CSTRING Find; CSTRING Replace; STF Flags; int Start; int End; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteLine { int Line; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectRange { int Start; int End; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertText { CSTRING String; int Pos; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetLine { int Line; STRING Buffer; int Length; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReplaceLine { int Line; CSTRING String; int Length; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GotoLine { int Line; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct TrimWhitespace { static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetPos { int Line; int Column; int Pos; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReportEvent { static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ScrollToPoint { int X; int Y; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScintilla : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCINTILLA;
   static constexpr CSTRING CLASS_NAME = "Scintilla";

   using create = pf::Create<objScintilla>;

   objFont * Font;               // Refers to the font that is used for drawing text in the document.
   CSTRING   Path;               // Identifies the location of a text file to load.
   SEF       EventFlags;         // Specifies events that need to be reported from the Scintilla object.
   OBJECTID  SurfaceID;          // Refers to the Surface targeted by the Scintilla object.
   SCIF      Flags;              // Optional flags.
   OBJECTID  FocusID;            // Defines the object that is monitored for user focus changes.
   int       Visible;            // If TRUE, indicates the Scintilla object is visible in the target Surface.
   int       LeftMargin;         // The amount of white-space at the left side of the page.
   int       RightMargin;        // Defines the amount of white-space at the right side of the page.
   struct RGB8 LineHighlight;    // The colour to use when highlighting the line that contains the user's cursor.
   struct RGB8 SelectFore;       // Defines the colour of selected text.  Supports alpha blending.
   struct RGB8 SelectBkgd;       // Defines the background colour of selected text.  Supports alpha blending.
   struct RGB8 BkgdColour;       // Defines the background colour.  Alpha blending is not supported.
   struct RGB8 CursorColour;     // Defines the colour of the text cursor.  Alpha blending is not supported.
   struct RGB8 TextColour;       // Defines the default colour of foreground text.  Supports alpha blending.
   int       CursorRow;          // The current row of the text cursor.
   int       CursorCol;          // The current column of the text cursor.
   SCLEX     Lexer;              // The lexer for document styling is defined here.
   int       Modified;           // Returns true if the document has been modified and not saved.

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR clipboard(CLIPMODE Mode) noexcept {
      struct acClipboard args = { Mode };
      return Action(AC::Clipboard, this, &args);
   }
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
   inline ERR focus() noexcept { return Action(AC::Focus, this, nullptr); }
   inline ERR hide() noexcept { return Action(AC::Hide, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR redo(int Steps) noexcept {
      struct acRedo args = { Steps };
      return Action(AC::Redo, this, &args);
   }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR show() noexcept { return Action(AC::Show, this, nullptr); }
   inline ERR undo(int Steps) noexcept {
      struct acUndo args = { Steps };
      return Action(AC::Undo, this, &args);
   }
   inline ERR setFont(CSTRING Face) noexcept {
      struct sci::SetFont args = { Face };
      return(Action(AC(-1), this, &args));
   }
   inline ERR replaceText(CSTRING Find, CSTRING Replace, STF Flags, int Start, int End) noexcept {
      struct sci::ReplaceText args = { Find, Replace, Flags, Start, End };
      return(Action(AC(-2), this, &args));
   }
   inline ERR deleteLine(int Line) noexcept {
      struct sci::DeleteLine args = { Line };
      return(Action(AC(-3), this, &args));
   }
   inline ERR selectRange(int Start, int End) noexcept {
      struct sci::SelectRange args = { Start, End };
      return(Action(AC(-4), this, &args));
   }
   inline ERR insertText(CSTRING String, int Pos) noexcept {
      struct sci::InsertText args = { String, Pos };
      return(Action(AC(-5), this, &args));
   }
   inline ERR getLine(int Line, STRING Buffer, int Length) noexcept {
      struct sci::GetLine args = { Line, Buffer, Length };
      return(Action(AC(-6), this, &args));
   }
   inline ERR replaceLine(int Line, CSTRING String, int Length) noexcept {
      struct sci::ReplaceLine args = { Line, String, Length };
      return(Action(AC(-7), this, &args));
   }
   inline ERR gotoLine(int Line) noexcept {
      struct sci::GotoLine args = { Line };
      return(Action(AC(-8), this, &args));
   }
   inline ERR trimWhitespace() noexcept {
      return(Action(AC(-9), this, nullptr));
   }
   inline ERR getPos(int Line, int Column, int * Pos) noexcept {
      struct sci::GetPos args = { Line, Column, (int)0 };
      ERR error = Action(AC(-10), this, &args);
      if (Pos) *Pos = args.Pos;
      return(error);
   }
   inline ERR reportEvent() noexcept {
      return(Action(AC(-11), this, nullptr));
   }
   inline ERR scrollToPoint(int X, int Y) noexcept {
      struct sci::ScrollToPoint args = { X, Y };
      return(Action(AC(-12), this, &args));
   }

   // Customised field setting

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setEventFlags(const SEF Value) noexcept {
      this->EventFlags = Value;
      return ERR::Okay;
   }

   inline ERR setSurface(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->SurfaceID = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const SCIF Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setFocus(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->FocusID = Value;
      return ERR::Okay;
   }

   inline ERR setVisible(const int Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Visible = Value;
      return ERR::Okay;
   }

   inline ERR setLeftMargin(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[32];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setRightMargin(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setLineHighlight(const struct RGB8 * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(target, field, 0x01081300, Value, Elements);
   }

   inline ERR setSelectFore(const struct RGB8 * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(target, field, 0x01081500, Value, Elements);
   }

   inline ERR setSelectBkgd(const struct RGB8 * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[33];
      return field->WriteValue(target, field, 0x01081500, Value, Elements);
   }

   inline ERR setBkgdColour(const struct RGB8 * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(target, field, 0x01081300, Value, Elements);
   }

   inline ERR setCursorColour(const struct RGB8 * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(target, field, 0x01081300, Value, Elements);
   }

   inline ERR setTextColour(const struct RGB8 * Value, int Elements) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(target, field, 0x01081300, Value, Elements);
   }

   inline ERR setCursorRow(const int Value) noexcept {
      this->CursorRow = Value;
      return ERR::Okay;
   }

   inline ERR setCursorCol(const int Value) noexcept {
      this->CursorCol = Value;
      return ERR::Okay;
   }

   inline ERR setLexer(const SCLEX Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setModified(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setAllowTabs(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setAutoIndent(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setFileDrop(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   inline ERR setFoldingMarkers(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setLineNumbers(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setOrigin(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setShowWhitespace(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setEventCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setString(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setSymbols(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[28];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setTabWidth(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setWordwrap(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[29];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

};

// ScintillaSearch class definition

#define VER_SCINTILLASEARCH (1.000000)

// ScintillaSearch methods

namespace ss {
struct Next { int Pos; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Prev { int Pos; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Find { int Pos; STF Flags; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScintillaSearch : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCINTILLASEARCH;
   static constexpr CSTRING CLASS_NAME = "ScintillaSearch";

   using create = pf::Create<objScintillaSearch>;

   objScintilla * Scintilla;    // Targets a Scintilla object for searching.
   CSTRING Text;                // The string sequence to search for.
   STF     Flags;               // Optional flags.
   int     Start;               // Start of the current/most recent selection
   int     End;                 // End of the current/most recent selection

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR next(int * Pos) noexcept {
      struct ss::Next args = { (int)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Pos) *Pos = args.Pos;
      return(error);
   }
   inline ERR prev(int * Pos) noexcept {
      struct ss::Prev args = { (int)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Pos) *Pos = args.Pos;
      return(error);
   }
   inline ERR find(int * Pos, STF Flags) noexcept {
      struct ss::Find args = { (int)0, Flags };
      ERR error = Action(AC(-3), this, &args);
      if (Pos) *Pos = args.Pos;
      return(error);
   }

   // Customised field setting

   inline ERR setScintilla(objScintilla * Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Scintilla = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setText(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setFlags(const STF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

};

