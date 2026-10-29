#pragma once

// Name:      scintilla.h
// Copyright: Paul Manias © 2005-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_SCINTILLA (1)

#include <kotuku/modules/display.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/vector.h>
#include <kotuku/modules/xml.h>

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

// Marker symbols for DefineMarker(), as defined by the Scintilla library.

enum class SCMK : int {
   NIL = 0,
   CIRCLE = 0,
   LCORNER = 10,
   TCORNER = 11,
   BOX_PLUS = 12,
   BOX_PLUS_CONNECTED = 13,
   BOX_MINUS = 14,
   BOX_MINUS_CONNECTED = 15,
   LCORNER_CURVE = 16,
   TCORNER_CURVE = 17,
   CIRCLE_PLUS = 18,
   CIRCLE_PLUS_CONNECTED = 19,
   ROUND_RECT = 1,
   CIRCLE_MINUS = 20,
   CIRCLE_MINUS_CONNECTED = 21,
   BACKGROUND = 22,
   DOTS = 23,
   ARROWS = 24,
   FULL_RECT = 26,
   LEFT_RECT = 27,
   UNDERLINE = 29,
   ARROW = 2,
   SMALL_RECT = 3,
   SHORT_ARROW = 4,
   EMPTY = 5,
   ARROW_DOWN = 6,
   MINUS = 7,
   PLUS = 8,
   VLINE = 9,
};

// Margin identifiers, as reported to the MarginClick callback.

enum class SCMG : int {
   NIL = 0,
   LINE_NUMBERS = 0,
   SYMBOLS = 1,
   FOLDING = 2,
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
   FAIL_CLIPBOARD = 0x00000010,
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
struct SetFont { std::string_view Face; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReplaceText { std::string_view Find; std::string_view Replace; STF Flags; int Start; int End; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteLine { int Line; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectRange { int Start; int End; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertText { std::string_view String; int Pos; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetLine { int Line; std::span<int8_t> Buffer; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReplaceLine { int Line; std::string_view String; int Length; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GotoLine { int Line; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct TrimWhitespace { static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetPos { int Line; int Column; int Pos; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReportEvent { static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ScrollToPoint { int X; int Y; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Repaint { static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DefineMarker { int Marker; SCMK Symbol; struct RGB8 *Fore; struct RGB8 *Bkgd; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddMarker { int Line; int Marker; static const AC id = AC(-15); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveMarker { int Line; int Marker; static const AC id = AC(-16); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetMarkers { int Line; int Markers; static const AC id = AC(-17); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ToggleFold { int Line; static const AC id = AC(-18); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScintilla : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCINTILLA;
   static constexpr CSTRING CLASS_NAME = "Scintilla";

   using create = kt::Create<objScintilla>;
   objScintilla(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string Face;                // The font family used for drawing and measuring document text.
   std::string Path;                // Identifies the location of a text file to load.
   SEF   EventFlags;                // Specifies events that need to be reported from the Scintilla object.
   SCIF  Flags;                     // Optional flags.
   objVectorViewport * Viewport;    // The viewport that will host the editor.
   objVectorViewport * Focus;       // Defines the viewport that is monitored for user focus changes.
   objVectorViewport * View;        // An internally created viewport that clips the visible content of the editor.
   objVectorViewport * Page;        // An internally created viewport that represents the extent of the document.
   int   LeftMargin;                // The amount of white-space at the left side of the page.
   int   RightMargin;               // Defines the amount of white-space at the right side of the page.
   struct RGB8 LineHighlight;       // The colour to use when highlighting the line that contains the user's cursor.
   struct RGB8 SelectFore;          // Defines the colour of selected text.  Supports alpha blending.
   struct RGB8 SelectBkgd;          // Defines the background colour of selected text.  Supports alpha blending.
   struct RGB8 BkgdColour;          // Defines the background colour.  Alpha blending is not supported.
   struct RGB8 CursorColour;        // Defines the colour of the text cursor.  Alpha blending is not supported.
   struct RGB8 TextColour;          // Defines the default colour of foreground text.  Alpha blending is not supported.
   int   CursorRow;                 // The current row of the text cursor.
   int   CursorCol;                 // The current column of the text cursor.
   SCLEX Lexer;                     // The lexer for document styling is defined here.
   int   Modified;                  // Returns true if the document has been modified and not saved.
   int   FontSize;                  // The font em size in pixels.

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR clipboard(CLIPMODE Mode) noexcept {
      struct acClipboard args = { Mode };
      return Action(AC::Clipboard, this, &args);
   }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, std::span<const int8_t> Buffer) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer };
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
   inline ERR setFont(const std::string_view &Face) noexcept {
      struct sci::SetFont args = { Face };
      return Action(AC(-1), this, &args);
   }
   inline ERR replaceText(const std::string_view &Find, const std::string_view &Replace, STF Flags, int Start, int End) noexcept {
      struct sci::ReplaceText args = { Find, Replace, Flags, Start, End };
      return Action(AC(-2), this, &args);
   }
   inline ERR deleteLine(int Line) noexcept {
      struct sci::DeleteLine args = { Line };
      return Action(AC(-3), this, &args);
   }
   inline ERR selectRange(int Start, int End) noexcept {
      struct sci::SelectRange args = { Start, End };
      return Action(AC(-4), this, &args);
   }
   inline ERR insertText(const std::string_view &String, int Pos) noexcept {
      struct sci::InsertText args = { String, Pos };
      return Action(AC(-5), this, &args);
   }
   inline ERR getLine(int Line, std::span<int8_t> Buffer) noexcept {
      struct sci::GetLine args = { Line, Buffer };
      return Action(AC(-6), this, &args);
   }
   inline ERR replaceLine(int Line, const std::string_view &String, int Length) noexcept {
      struct sci::ReplaceLine args = { Line, String, Length };
      return Action(AC(-7), this, &args);
   }
   inline ERR gotoLine(int Line) noexcept {
      struct sci::GotoLine args = { Line };
      return Action(AC(-8), this, &args);
   }
   inline ERR trimWhitespace() noexcept {
      return Action(AC(-9), this, nullptr);
   }
   inline ERR getPos(int Line, int Column, int * Pos) noexcept {
      struct sci::GetPos args = { Line, Column, (int)0 };
      ERR error = Action(AC(-10), this, &args);
      if (Pos) *Pos = args.Pos;
      return error;
   }
   inline ERR reportEvent() noexcept {
      return Action(AC(-11), this, nullptr);
   }
   inline ERR scrollToPoint(int X, int Y) noexcept {
      struct sci::ScrollToPoint args = { X, Y };
      return Action(AC(-12), this, &args);
   }
   inline ERR repaint() noexcept {
      return Action(AC(-13), this, nullptr);
   }
   inline ERR defineMarker(int Marker, SCMK Symbol, struct RGB8 * Fore, struct RGB8 * Bkgd) noexcept {
      struct sci::DefineMarker args = { Marker, Symbol, Fore, Bkgd };
      return Action(AC(-14), this, &args);
   }
   inline ERR addMarker(int Line, int Marker) noexcept {
      struct sci::AddMarker args = { Line, Marker };
      return Action(AC(-15), this, &args);
   }
   inline ERR removeMarker(int Line, int Marker) noexcept {
      struct sci::RemoveMarker args = { Line, Marker };
      return Action(AC(-16), this, &args);
   }
   inline ERR getMarkers(int Line, int * Markers) noexcept {
      struct sci::GetMarkers args = { Line, (int)0 };
      ERR error = Action(AC(-17), this, &args);
      if (Markers) *Markers = args.Markers;
      return error;
   }
   inline ERR toggleFold(int Line) noexcept {
      struct sci::ToggleFold args = { Line };
      return Action(AC(-18), this, &args);
   }

   // Customised field getting

   inline ERR getFace(std::string_view &Value) noexcept {
      Value = this->Face;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getEventFlags(SEF &Value) noexcept {
      Value = this->EventFlags;
      return ERR::Okay;
   }

   inline ERR getFlags(SCIF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getViewport(objVectorViewport * &Value) noexcept {
      Value = this->Viewport;
      return ERR::Okay;
   }

   inline ERR getFocus(objVectorViewport * &Value) noexcept {
      Value = this->Focus;
      return ERR::Okay;
   }

   inline ERR getView(objVectorViewport * &Value) noexcept {
      Value = this->View;
      return ERR::Okay;
   }

   inline ERR getPage(objVectorViewport * &Value) noexcept {
      Value = this->Page;
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

   inline ERR getLineHighlight(struct RGB8 * &Value) noexcept {
      Value = &this->LineHighlight;
      return ERR::Okay;
   }

   inline ERR getSelectFore(struct RGB8 * &Value) noexcept {
      Value = &this->SelectFore;
      return ERR::Okay;
   }

   inline ERR getSelectBkgd(struct RGB8 * &Value) noexcept {
      Value = &this->SelectBkgd;
      return ERR::Okay;
   }

   inline ERR getBkgdColour(struct RGB8 * &Value) noexcept {
      Value = &this->BkgdColour;
      return ERR::Okay;
   }

   inline ERR getCursorColour(struct RGB8 * &Value) noexcept {
      Value = &this->CursorColour;
      return ERR::Okay;
   }

   inline ERR getTextColour(struct RGB8 * &Value) noexcept {
      Value = &this->TextColour;
      return ERR::Okay;
   }

   inline ERR getCursorRow(int &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getCursorCol(int &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getLexer(SCLEX &Value) noexcept {
      Value = this->Lexer;
      return ERR::Okay;
   }

   inline ERR getModified(int &Value) noexcept {
      Value = this->Modified;
      return ERR::Okay;
   }

   inline ERR getFontSize(int &Value) noexcept {
      Value = this->FontSize;
      return ERR::Okay;
   }

   inline ERR getAllowTabs(int &Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->GetValue(this, &Value);
   }

   inline ERR getAutoIndent(int &Value) noexcept {
      auto field = &this->Class->Dictionary[37];
      return field->GetValue(this, &Value);
   }

   inline ERR getFileDrop(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getFoldingMarkers(int &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->GetValue(this, &Value);
   }

   inline ERR getLineCount(int &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getLineNumbers(int &Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      return field->GetValue(this, &Value);
   }

   inline ERR getMarginClick(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getShowWhitespace(int &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->GetValue(this, &Value);
   }

   inline ERR getEventCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getString(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSymbols(int &Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->GetValue(this, &Value);
   }

   inline ERR getTabWidth(int &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->GetValue(this, &Value);
   }

   inline ERR getWordwrap(int &Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setFace(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setEventFlags(const SEF Value) noexcept {
      this->EventFlags = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const SCIF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setViewport(objVectorViewport * Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Viewport = Value;
      return ERR::Okay;
   }

   inline ERR setFocus(objVectorViewport * Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Focus = Value;
      return ERR::Okay;
   }

   inline ERR setLeftMargin(const int Value) noexcept {
      auto field = &this->Class->Dictionary[40];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setRightMargin(const int Value) noexcept {
      auto field = &this->Class->Dictionary[39];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setLineHighlight(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setSelectFore(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[33];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setSelectBkgd(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[29];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setBkgdColour(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setCursorColour(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setTextColour(const struct RGB8 & Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setLexer(const SCLEX Value) noexcept {
      auto field = &this->Class->Dictionary[26];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setModified(const int Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFontSize(const int Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setThemeXML(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[28];
      return field->WriteValue(this, field, 0x08100201, Value);
   }

   inline ERR setAllowTabs(const int Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setAutoIndent(const int Value) noexcept {
      auto field = &this->Class->Dictionary[37];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFileDrop(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setFoldingMarkers(const int Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setLineNumbers(const int Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setMarginClick(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOrigin(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00804200, &Value);
   }

   inline ERR setShowWhitespace(const int Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setEventCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setString(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setSymbols(const int Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setTabWidth(const int Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setWordwrap(const int Value) noexcept {
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

// ScintillaSearch class definition

#define VER_SCINTILLASEARCH (1.000000)

// ScintillaSearch methods

namespace ss {
struct Find { int From; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objScintillaSearch : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SCINTILLASEARCH;
   static constexpr CSTRING CLASS_NAME = "ScintillaSearch";

   using create = kt::Create<objScintillaSearch>;
   objScintillaSearch(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   objScintilla * Scintilla;    // Targets a Scintilla object for searching.
   std::string Text;            // The string sequence to search for.
   STF Flags;                   // Optional flags.
   int Start;                   // The start of the most recent match.
   int End;                     // The end of the most recent match.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR next() noexcept { return Action(AC::Next, this, nullptr); }
   inline ERR prev() noexcept { return Action(AC::Prev, this, nullptr); }
   inline ERR find(int From) noexcept {
      struct ss::Find args = { From };
      return Action(AC(-1), this, &args);
   }

   // Customised field getting

   inline ERR getScintilla(objScintilla * &Value) noexcept {
      Value = this->Scintilla;
      return ERR::Okay;
   }

   inline ERR getText(std::string_view &Value) noexcept {
      Value = this->Text;
      return ERR::Okay;
   }

   inline ERR getFlags(STF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getStart(int &Value) noexcept {
      Value = this->Start;
      return ERR::Okay;
   }

   inline ERR getEnd(int &Value) noexcept {
      Value = this->End;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setScintilla(objScintilla * Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Scintilla = Value;
      return ERR::Okay;
   }

   inline ERR setText(const std::string_view &Value) noexcept {
      this->Text = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const STF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

};