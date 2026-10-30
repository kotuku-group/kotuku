/*********************************************************************************************************************

-CLASS-
VectorText: Extends the Vector class with support for generating text.

To create text along a path, set the @Vector.GuidePath field with a reference to any @Vector object that generates a path.  The
following extract illustrates the SVG equivalent of this feature:

<pre>
&lt;defs&gt;
  &lt;path id="myTextPath2" d="M75,20 l100,0 l100,30 q0,100 150,100"/&gt;
&lt;/defs&gt;

&lt;text x="10" y="100" stroke="#000000"&gt;
  &lt;textPath xlink:href="#myTextPath2"/&gt;
&lt;/text&gt;
</pre>

Support for bitmap fonts is included.  This feature is implemented by drawing bitmap characters to an internal @VectorImage
and then rendering that to the viewport as a rectangular path.  It is recommended that this feature is only used for
fast `1:1` rendering without transforms.  The user is otherwise better served through the use of scalable fonts.

-END-

Some notes about font rendering:

* Font glyphs should always be positioned with a rounded vertical baseline when drawn.  That is to say a Y coordinate of
  385 is fine and 385.76 is not.  The Freetype glyphs are hinted based on this assumption.  If a glyph's
  baseline can adopt any position, the hinting may as well be turned off.

* Whether or not a glyph's X coordinate should be rounded is a matter of preference.  An aligned glyph will be less
  blurry than its counterpart, but it will come at a cost of less accurate kerning.  If you're not rounding the X
  coordinate, the HINT_LIGHT option should probably be used because hinting will only be performed vertically in
  that case, while character widths and spacing remain unchanged.

* Some variable fonts may have hinting defined internally that does not harmonise with the Freetype settings.  It may
  be necessary to alter the 'Hinting' option for affected fonts in "fonts:options.cfg".

TODO
----
* ShapeInside and ShapeSubtract require implementation
* The use of globally shared Font objects currently isn't thread-safe.

*********************************************************************************************************************/

/*
textPath warping could be more accurate if the character angles were calculated on the mid-point of the character
rather than the bottom left corner.  However, this would be more computationally intensive and only useful in situations
where large glyphs were oriented around sharp corners.  The process would look something like this:

+ Compute the (x,y) of the character's middle vertex in context of the guide path.
+ Compute the angle from the middle vertex to the first vertex (start_x, start_y)
+ Compute the angle from the middle vertex to the last vertex (end_x, end_y)
+ Interpolate the two angles
+ Rotate the character around its mid point.
+ The final character position is moved to the mid-point rather than start_x,start_y
*/

#include <kotuku/modules/filesystem.h>

#include "agg_path_length.h"
#include "text_word_ranges.h"

const int DEFAULT_WEIGHT = 400;

objConfig *glFontConfig = nullptr;

//********************************************************************************************************************

class TextCursor {
private:
   void position(extVectorText *, int, int, bool);
   int  mColumn, mRow; // The column is the character position after taking UTF8 sequences into account.

public:
   APTR  timer;
   extVectorPolygon *vector;
   int  flash;
   int  preferredColumn; // Retained across consecutive vertical moves, including selection extension
   int  selectColumn, selectRow;

   TextCursor() :
      mColumn(0), mRow(0),
      timer(nullptr), vector(nullptr), flash(0), preferredColumn(-1),
      selectColumn(0), selectRow(0) { }

   ~TextCursor() {
      if (vector) { FreeResource(vector); vector = nullptr; }
      if (timer) { UpdateTimer(timer, 0); timer = 0; }
   }

   inline int column() { return mColumn; }
   inline int row() { return mRow; }

   inline void resetFlash() { flash = 0; }

   void selectedArea(extVectorText *Self, int *Row, int *Column, int *EndRow, int *EndColumn) const {
      if (selectRow < mRow) {
         *Row       = selectRow;
         *EndRow    = mRow;
         *Column    = selectColumn;
         *EndColumn = mColumn;
      }
      else if (selectRow IS mRow) {
         *Row       = selectRow;
         *EndRow    = mRow;
         if (selectColumn < mColumn) {
            *Column    = selectColumn;
            *EndColumn = mColumn;
         }
         else {
            *Column    = mColumn;
            *EndColumn = selectColumn;
         }
      }
      else {
         *Row       = mRow;
         *EndRow    = selectRow;
         *Column    = mColumn;
         *EndColumn = selectColumn;
      }
   }

   void move(extVectorText *, int, int, bool ValidateWidth = false);
   void extend(extVectorText *, int, int);
   void reset_vector(extVectorText *) const;
   void validate_position(extVectorText *) const;
};

//********************************************************************************************************************

class CharPos {
public:
   double x1, y1, x2, y2;
   double lineTop, lineHeight; // Full ascent-to-descent box, independent of the visible caret stroke.
   CharPos(double X1, double Y1, double X2, double Y2) :
      x1(X1), y1(Y1), x2(X2), y2(Y2), lineTop(Y1), lineHeight(Y2 - Y1) { }
};

//********************************************************************************************************************

enum class TextCharClass : uint8_t { WORD, SPACE, SEPARATOR };

class TextLine : public std::string {
public:
   TextLine() : std::string { } { }
   TextLine(const char *Value) : std::string{ Value } { }
   TextLine(const char *Value, size_t Total) : std::string{ Value, Total } { }
   TextLine(std::string Value) : std::string{ Value } { }

   std::vector<CharPos> chars;
   std::vector<TextCharClass> characterClasses;
   bool classesValid = false;

   void invalidate_text() {
      classesValid = false;
      chars.clear(); // Retain layout storage for the next generation.
   }

   inline int charLength(uint32_t Offset = 0) const { // Total number of bytes used by the char at Offset
      return UTF8CharLength(c_str() + Offset);
   }

   inline int utf8CharOffset(uint32_t Char) const { // Convert a character index to its byte offset
      return UTF8CharOffset(c_str(), Char);
   }

   inline int utf8Length() const { // Total number of unicode characters in the string
      return classesValid ? int(characterClasses.size()) : UTF8Length(c_str());
   }

   inline int lastChar() const { // Return a direct offset to the start of the last character.
      return length() - UTF8PrevLength(c_str(), length());
   }

   inline int prevChar(uint32_t Offset) const { // Return the direct offset to a previous character.
      return Offset - UTF8PrevLength(c_str(), Offset);
   }
};

//********************************************************************************************************************

static ERR cursor_timer(class extVectorText *, int64_t, int64_t);
static std::string selected_text(class extVectorText *);
static void clear_selection(class extVectorText *);
static void selection_changed(class extVectorText *);
static void report_cursor_move(class extVectorText *);
static void rebuild_selection(class extVectorText *);
static bool replace_selection(class extVectorText *, std::string_view);
static void insert_char(class extVectorText *, int);
static void generate_text(class extVectorText *, agg::path_storage &Path);
static void raster_text_to_bitmap(class extVectorText *);
static void key_event(evKey *, int, class extVectorText *);
static ERR reset_font(class extVectorText *, bool = false);
static ERR text_input_events(class extVector *Vector, const InputEvent *Events);
static ERR text_focus_event(class extVector *Vector, FM Event, OBJECTPTR EventObject, APTR Meta);
static void activate_text_input(class extVectorText *);

//********************************************************************************************************************

class extVectorText : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORTEXT;
   static constexpr CSTRING CLASS_NAME = "VectorText";
   using create = kt::Create<extVectorText>;

   std::vector<TextLine> txLines;
   FUNCTION txOnChange;
   FUNCTION txOnCursorMove;
   int txCursorUpdateDepth = 0;
   int txReportedRow = 0, txReportedColumn = 0, txReportedSelectRow = 0, txReportedSelectColumn = 0;
   bool txReportedSelection = false, txCursorChanged = false, txReportingCursor = false;
   double txInlineSize; // Enables word-wrapping
   Unit txX = Unit(0), txY = Unit(0);
   double txTextLength;
   double txFontSize;  // Em size in pixels (CSS px; FreeType runs at 72 DPI so 1pt = 1px).  Always a whole number.
   double txLetterSpacing; // SVG: Acts as a multiplier or fixed unit addition to the spacing of each glyph
   double txWidth; // Width of the text computed by path generation.  Not for client use as GetBoundary() can be used for that.
   double txStartOffset; // TODO
   double txSpacing; // TODO
   double txXOffset, txYOffset; // X,Y adjustment for ensuring that the cursor is visible.
   std::vector<double> txDX, txDY; // A series of spacing adjustments that apply on a per-character level.
   std::vector<double> txRotate; // A series of angles that will rotate each individual character.
   objFont *txBitmapFont;
   objBitmap *txAlphaBitmap; // Host for the bitmap font texture
   extVectorImage *txBitmapImage;
   common_font *txHandle;
   TextCursor txCursor;
   extVectorPath *txSelection;
   std::string txSelectFill = "rgba(0,120,215,0.35)";
   bool txSelectionHidden = false;
   bool txHasFocus = false;
   bool txDragging = false, txDragPending = false, txMouseShift = false;
   APTR txDragTimer = nullptr;
   double txDragX = 0, txDragY = 0, txClickX = 0, txClickY = 0;
   int64_t txClickTime = 0;
   int txClickCount = 0, txClickSlop = 4;
   std::string txFamily; // Family name(s) as requested by the client
   std::string txFontStyle;
   std::string txFontSizeString;
   APTR    txKeyEvent;
   OBJECTID txFocusID;
   OBJECTID txShapeInsideID;   // Enable word-wrapping within this shape
   OBJECTID txShapeSubtractID; // Subtract this shape from the path defined by shape-inside
   int  txTotalLines;
   int  txLineLimit, txCharLimit;
   int  txWeight; // 100 - 300 (Light), 400 (Normal), 700 (Bold), 900 (Boldest)
   ALIGN txAlignFlags;
   VTXF  txFlags;
   bool txScaledFontSize;
// bool txSpacingAndGlyphs:1;

   bool needs_caret_geometry() const {
      return (txFlags & (VTXF::EDITABLE|VTXF::SELECTABLE)) != VTXF::NIL;
   }

   extVectorText(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
      txFontStyle     = "Regular";
      GeneratePath    = (void (*)(extVector *, agg::path_storage &))&generate_text;
      StrokeWidth     = 0.0;
      txWeight        = DEFAULT_WEIGHT;
      txFontSize      = 16; // Pixel units, equivalent to 12pt
      txCharLimit     = 0x7fffffff;
      txFamily        = "Noto Sans";
      Fill.Colour     = FRGB(1, 1, 1, 1);
      txLetterSpacing = 1.0;
      DisableHitTesting = true;
   }

   ~extVectorText() {
      if (txHandle) {
         // TODO: This would be a good opportunity to garbage-collect stale glyphs
      }

      auto parent_view = ((extVector *)this)->ParentView;
      if ((parent_view) and (not parent_view->terminating()) and (parent_view->Scene) and
            (parent_view->Scene->SurfaceID)) {
         parent_view->subscribeInput(JTYPE::NIL, C_FUNCTION(text_input_events));
      }

      if (txDragTimer) UpdateTimer(txDragTimer, 0);
      if (txSelection) FreeResource(txSelection);
      if (txBitmapImage)  FreeResource(txBitmapImage);
      if (txAlphaBitmap)  FreeResource(txAlphaBitmap);
      if (txKeyEvent)     UnsubscribeEvent(txKeyEvent);
      release_callback(txOnChange);
      release_callback(txOnCursorMove);

      if (txFocusID) {
         if (kt::ScopedObjectLock<extVector> focus(txFocusID, 5000); focus.granted()) {
            focus->subscribeFeedback(FM::NIL, C_FUNCTION(text_focus_event));
         }
      }
   }
};

//********************************************************************************************************************
// Nested editing helpers may move both endpoints several times.  Report only their final state, after OnChange.

class CursorUpdate {
   extVectorText *mText;

public:
   explicit CursorUpdate(extVectorText *Text) : mText(Text) { mText->txCursorUpdateDepth++; }
   ~CursorUpdate() {
      if (--mText->txCursorUpdateDepth IS 0) report_cursor_move(mText);
   }
};

//********************************************************************************************************************

freetype_font::~freetype_font() {
   if (face) { FT_Done_Face(face); face = nullptr; }
}

//********************************************************************************************************************

inline double get_kerning(FT_Face Face, int Glyph, int PrevGlyph)
{
   if ((not Glyph) or (not PrevGlyph)) return 0;

   FT_Vector delta;
   FT_Get_Kerning(Face, PrevGlyph, Glyph, FT_KERNING_DEFAULT, &delta);
   return int26p6_to_dbl(delta.x);
}

inline void report_change(extVectorText *Self)
{
   Self->txCursorChanged = true;
   if (Self->txOnChange.stale()) release_callback(Self->txOnChange);
   else if (Self->txOnChange.isC()) {
      auto routine = (void (*)(extVectorText *))Self->txOnChange.Routine;
      kt::SwitchContext context(Self->txOnChange.Context);
      routine(Self);
   }
   else if (Self->txOnChange.isScript()) {
      sc::Call(Self->txOnChange, std::to_array<ScriptArg>({
         { "VectorText", Self, FD_OBJECTPTR }
      }));
   }
}

//********************************************************************************************************************
// Measure the word at the start of String, for word-wrapping.  The word ends at whitespace, a control character or
// the end of the string.  Bytes are compared unsigned, so that the bytes of multi-byte UTF-8 characters belong to the
// word.  Returns the length of the word in bytes and in characters.

struct WordExtent {
   int bytes, chars;
};

static WordExtent word_extent(const char *String)
{
   WordExtent word = { 0, 0 };
   while ((String[word.bytes]) and (uint8_t(String[word.bytes]) > 0x20)) {
      for (++word.bytes; (String[word.bytes] & 0xc0) IS 0x80; word.bytes++);
      word.chars++;
   }
   return word;
}

//********************************************************************************************************************

static int string_width(extVectorText *Self, const std::string_view &String)
{
   const std::lock_guard lock(glFontMutex);

   auto pt = (freetype_font::ft_point *)Self->txHandle;

   FT_Activate_Size(pt->ft_size);

   int len        = 0;
   int widest     = 0;
   int prev_glyph = 0;
   int i = 0;
   while (i < std::ssize(String)) {
      if (String[i] IS '\n') {
         if (widest < len) widest = len;
         len = 0;
         i++;
      }
      else {
         uint32_t unicode;
         auto charlen = get_utf8(String, unicode, i);
         auto &glyph  = pt->get_glyph(unicode);
         len += glyph.adv_x * Self->txLetterSpacing;
         len += get_kerning(pt->font->face, glyph.glyph_index, prev_glyph);
         prev_glyph = glyph.glyph_index;
         i += charlen;
      }
   }

   if (widest > len) return widest;
   else return len;
}

/*********************************************************************************************************************
-ACTION-
Clipboard: Copies, cuts or pastes selected text through the system clipboard.

`CLIPMODE::COPY` copies #SelectedText as UTF-8.  `CLIPMODE::CUT` copies the selection and removes it only after the
clipboard accepts the text.  Both operations leave the clipboard and text unchanged when nothing is selected or
`VTXF::SECRET` is set in #TextFlags.

`CLIPMODE::PASTE` replaces the selection, or inserts at the cursor when nothing is selected.  CRLF and CR line endings
are normalised to LF.  With #LineLimit set to 1, each line ending becomes a space.  Other line limits reject a paste
that would exceed the limit after replacing the selected rows.  #CharLimit truncates each resulting line in character
columns, matching keyboard editing.  Text after a null byte is ignored.

Cut and paste require `VTXF::EDITABLE`.  Copy does not change the selection or call #OnChange.  Cut and paste call
#OnChange once if the text changes.

-ERRORS-
Okay: The operation completed, or there was nothing to copy or change.
NullArgs
Args: The clipboard mode is not supported.
ReadOnly: Cut or paste was requested for non-editable text.
NoData: No text is available to paste.
CreateObject: The Clipboard object could not be created.
OpenFile: The clipboard file could not be opened.
OutOfRange: The clipboard file is too large to read.
-END-

*********************************************************************************************************************/

static ERR VECTORTEXT_Clipboard(extVectorText *Self, struct acClipboard *Args)
{
   CursorUpdate update(Self);
   if ((not Args) or (Args->Mode IS CLIPMODE::NIL)) return ERR::NullArgs;
   if ((Args->Mode != CLIPMODE::COPY) and (Args->Mode != CLIPMODE::CUT) and
       (Args->Mode != CLIPMODE::PASTE)) return ERR::Args;
   if ((Args->Mode != CLIPMODE::COPY) and ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL)) {
      return ERR::ReadOnly;
   }

   if (Args->Mode != CLIPMODE::PASTE) {
      const auto text = selected_text(Self);
      if (text.empty()) return ERR::Okay;
      objClipboard::create clipboard = { };
      if (not clipboard.ok()) return ERR::CreateObject;
      if (auto error = clipboard->addText(text); error != ERR::Okay) return error;
      if ((Args->Mode IS CLIPMODE::CUT) and replace_selection(Self, "")) report_change(Self);
      return ERR::Okay;
   }

   objClipboard::create clipboard = { };
   if (not clipboard.ok()) return ERR::CreateObject;
   kt::vector<std::string> files;
   if (auto error = clipboard->getFiles(CLIPTYPE::TEXT, 0, nullptr, files, nullptr);
       error != ERR::Okay) return error;
   if (files.empty()) return ERR::NoData;

   objFile::create file = { fl::Path(files[0]), fl::Flags(FL::READ) };
   if (not file.ok()) return ERR::OpenFile;
   int64_t size;
   if (auto error = file->getSize(size); error != ERR::Okay) return error;
   if (size <= 0) return ERR::NoData;
   if (size > 0x7fffffff) return ERR::OutOfRange;

   std::string content(size_t(size), '\0');
   int length;
   if (auto error = file->read(std::span<int8_t>((int8_t *)content.data(), content.size()), &length);
       error != ERR::Okay) return error;
   content.resize(size_t(length));
   if (auto end = content.find('\0'); end != std::string::npos) content.resize(end);
   if (content.empty()) return ERR::NoData;

   std::string text;
   text.reserve(content.size());
   for (size_t i=0; i < content.size(); i++) {
      auto ch = content[i];
      if (ch IS '\r') {
         if ((i + 1 < content.size()) and (content[i + 1] IS '\n')) i++;
         ch = '\n';
      }
      if ((ch IS '\n') and (Self->txLineLimit IS 1)) ch = ' ';
      text += ch;
   }
   if (replace_selection(Self, text)) report_change(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DeleteLine: Deletes any line number.

This method deletes lines from a text object.  You only need to specify the line number to have it deleted.  If the
line number does not exist, then the call will fail.  The text graphic will be updated as a result of calling this
method.

-INPUT-
int Line: The line number that you want to delete.  If negative, the last line will be deleted.

-ERRORS-
Okay: The line was deleted.
Args: The Line value was out of the valid range.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR VECTORTEXT_DeleteLine(extVectorText *Self, struct vt::DeleteLine *Args)
{
   CursorUpdate update(Self);
   if (Self->txLines.empty()) return ERR::Okay;
   const bool changed = (Self->txLines.size() > 1) or not Self->txLines.front().empty();

   if ((not Args) or (Args->Line < 0)) Self->txLines.pop_back();
   else if ((size_t)Args->Line < Self->txLines.size()) Self->txLines.erase(Self->txLines.begin() + Args->Line);
   else return ERR::Args;

   Self->txCursorChanged |= changed;
   mark_dirty(Self, RC::BASE_PATH);

   if ((Args) and (Self->txCursor.row() IS Args->Line)) {
      Self->txCursor.move(Self, Self->txCursor.row(), 0);
   }
   else if ((size_t)Self->txCursor.row() >= Self->txLines.size()) {
      Self->txCursor.move(Self, Self->txLines.size()-1, Self->txCursor.column());
   }

   clear_selection(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SelectArea: Selects an area of text.

SelectArea() marks a region of text as selected, starting at the position given by `Row` and `Column`.  The cursor is
moved to the end of the region at `EndRow` and `EndColumn`, which may come before the start for a reversed selection.
Columns are character positions, so UTF-8 sequences count as one column each.  A negative `EndRow` refers to the
last line and a negative `EndColumn` refers to the end of its line, so `SelectArea(0, 0, -1, -1)` selects all of the
text.  Positions beyond the text are clamped to its limits.

While an area is selected the `AREA_SELECTED` bit is set in #TextFlags.  For editable text, the next character typed,
or a press of Backspace, Delete, Clear or Enter, will replace or delete the selected text.  Moving the cursor without
extending the selection deselects the area.

Text with `VTXF::EDITABLE` or `VTXF::SELECTABLE` displays the selected area with the #SelectFill painter.  The highlight
is hidden when focus is lost and restored when focus returns; the selection range is retained.

If the start and end positions are identical, the cursor is moved and no area is selected.

-INPUT-
int Row: The line at which the selection starts.
int Column: The character column at which the selection starts.
int EndRow: The line at which the selection ends.  If negative, the last line is used.
int EndColumn: The character column at which the selection ends.  If negative, the end of the line is used.

-ERRORS-
Okay: The area was selected.
NullArgs: No arguments were provided.
NoData: The object contains no text.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR VECTORTEXT_SelectArea(extVectorText *Self, struct vt::SelectArea *Args)
{
   CursorUpdate update(Self);
   if (not Args) return ERR::NullArgs;
   if (Self->txLines.empty()) return ERR::NoData;

   auto last_row = int(Self->txLines.size()) - 1;

   auto row = std::clamp(Args->Row, 0, last_row);
   auto column = std::clamp(Args->Column, 0, Self->txLines[row].utf8Length());

   auto end_row = (Args->EndRow < 0) ? last_row : std::min(Args->EndRow, last_row);
   auto end_length = Self->txLines[end_row].utf8Length();
   auto end_column = (Args->EndColumn < 0) ? end_length : std::min(Args->EndColumn, end_length);

   Self->txCursor.move(Self, row, column);
   Self->txCursor.extend(Self, end_row, end_column);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetCaretBounds: Returns the line box at a character position.

Use GetCaretBounds to find where the caret sits at a given character position, for example to position a popup or
scroll a container so that the cursor stays in view.  The result describes a vertical line: X is the caret position,
and Y and Height cover the line from the font's ascent to its descent.

Coordinates are in the parent viewport's coordinate space and include the scroll offset that keeps the cursor visible.
Transforms applied to the VectorText are not included.

Out-of-range Row and Column values are clamped to the nearest valid position.  A Column equal to the length of the line
returns the position after the last character.

Caret geometry is only maintained when `VTXF::EDITABLE` or `VTXF::SELECTABLE` is set in #TextFlags.  The text layout
is regenerated if necessary before the bounds are read.

-INPUT-
int Row: Logical row, or a negative value for the current cursor row.
int Column: Character column, or a negative value for the current cursor column.
&double X: Receives the horizontal caret position.
&double Y: Receives the top of the line box.
&double Height: Receives the full ascent-to-descent height.

-ERRORS-
Okay
NullArgs
NoData: Neither `EDITABLE` nor `SELECTABLE` is set in #TextFlags, or no caret positions have been calculated.
NoSupport: The text follows a guide path.
-END-

*********************************************************************************************************************/

static ERR VECTORTEXT_GetCaretBounds(extVectorText *Self, struct vt::GetCaretBounds *Args)
{
   if (not Args) return ERR::NullArgs;

   validate_object_link(Self->GuidePath);
   if (Self->GuidePath) return ERR::NoSupport;
   if (not Self->needs_caret_geometry()) return ERR::NoData;

   gen_vector_tree(Self);

   if (Self->txLines.empty()) return ERR::NoData;

   const auto row = std::clamp(Args->Row < 0 ? Self->txCursor.row() : Args->Row, 0, int(Self->txLines.size()) - 1);
   const auto &line = Self->txLines[row];

   if (line.chars.empty()) return ERR::NoData;

   const auto column = std::clamp(Args->Column < 0 ? Self->txCursor.column() : Args->Column,
      0, int(line.chars.size()) - 1);
   const auto &caret = line.chars[column];
   
   Args->X = Self->FinalX - Self->txXOffset + caret.x1;
   Args->Y = Self->FinalY - Self->txYOffset + caret.lineTop;
   Args->Height = caret.lineHeight;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORTEXT_Init(extVectorText *Self)
{
   if (Self->needs_caret_geometry()) {
      if (not Self->txFocusID) {
         if (Self->ParentView) Self->txFocusID = Self->ParentView->UID;
      }

      if (kt::ScopedObjectLock<extVector> focus(Self->txFocusID, 5000); focus.granted()) {
         focus->subscribeFeedback(FM::HAS_FOCUS|FM::CHILD_HAS_FOCUS|FM::LOST_FOCUS, C_FUNCTION(text_focus_event));
      }

      // The editing cursor inherits transforms from the VectorText as a direct child.

      if ((Self->txFlags & VTXF::EDITABLE) != VTXF::NIL) {
         Self->txCursor.vector = extVectorPolygon::create::global(
            fl::Name("VTCursor"),
            fl::X1(0), fl::Y1(0), fl::X2(1), fl::Y2(1),
            fl::Closed(false),
            fl::Stroke("rgb(255,0,0,255)"),
            fl::StrokeWidth(1.25),
            fl::Visibility(VIS::HIDDEN));
         if (not Self->txCursor.vector) return ERR::CreateObject;
      }

      Self->txSelection = extVectorPath::create::global(
         fl::Name("VTSelection"), fl::Owner(Self->UID), fl::Fill(Self->txSelectFill),
         fl::StrokeWidth(0), fl::Visibility(VIS::HIDDEN));
      if (not Self->txSelection) return ERR::CreateObject;
      Self->txSelection->DisableHitTesting = true;
      if (Self->txCursor.vector) acMoveToFront(Self->txCursor.vector);

      if (Self->txLines.empty()) Self->txLines.emplace_back(std::string(""));

      if ((Self->ParentView) and (Self->ParentView->Scene->SurfaceID)) {
         Self->ParentView->subscribeInput(JTYPE::BUTTON|JTYPE::MOVEMENT, C_FUNCTION(text_input_events));
      }
   }

   return reset_font(Self, true);
}

/*********************************************************************************************************************
-FIELD-
Align: Defines the alignment of the text string.

This field specifies the horizontal alignment of the text string.  The standard alignment flags are supported in the
form of `ALIGN::LEFT`, `ALIGN::HORIZONTAL` and `ALIGN::RIGHT`.

In addition, the SVG equivalent values of `start`, `middle` and `end` are supported and map directly to the formerly
mentioned align flags.

*********************************************************************************************************************/

static ERR TEXT_GET_Align(extVectorText *Self, ALIGN *Value)
{
   *Value = Self->txAlignFlags;
   return ERR::Okay;
}

static ERR TEXT_SET_Align(extVectorText *Self, ALIGN Value)
{
   Self->txAlignFlags = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CharLimit: Limits the characters allowed on each logical line during editing.

CharLimit sets the maximum number of characters on each logical line, counted in UTF-8 characters rather than bytes.
The limit applies to each line separately, not to the text as a whole.  Use #LineLimit to restrict the number of lines.

The limit applies to keyboard input and to pastes made through the #Clipboard() action.  If an edit would exceed the
limit, the line is cut off at the limit and any characters past that point are discarded.  Deletions are always
permitted.

Writing to the #String field bypasses the limit, and reducing CharLimit does not shorten existing text.  The default
value is the maximum positive 32-bit integer, so there is effectively no limit.  A value of 0 prevents any characters
from being entered.  Negative values are rejected with `ERR::OutOfRange`.

*********************************************************************************************************************/

static ERR TEXT_GET_CharLimit(extVectorText *Self, int *Value)
{
   *Value = Self->txCharLimit;
   return ERR::Okay;
}

static ERR TEXT_SET_CharLimit(extVectorText *Self, int Value)
{
   if (Value < 0) return ERR::OutOfRange;

   Self->txCharLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CursorColumn: The current column position of the cursor.

*********************************************************************************************************************/

static ERR TEXT_GET_CursorColumn(extVectorText *Self, int *Value)
{
   *Value = Self->txCursor.column();
   return ERR::Okay;
}

static ERR TEXT_SET_CursorColumn(extVectorText *Self, int Value)
{
   if (Value >= 0) {
      Self->txCursor.move(Self, Self->txCursor.row(), Value, true);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
CursorRow: The current line position of the cursor.

*********************************************************************************************************************/

static ERR TEXT_GET_CursorRow(extVectorText *Self, int *Value)
{
   *Value = Self->txCursor.row();
   return ERR::Okay;
}

static ERR TEXT_SET_CursorRow(extVectorText *Self, int Value)
{
   if (Value >= 0) {
      Self->txCursor.move(Self, Value, Self->txCursor.column(), true);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Descent: The font descent measured in pixels.

Use Descent to retrieve the height of the font descent region, which is the space reserved below the baseline.  The
value is measured in the same pixel units as #FontSize, so it is unaffected by the display density and is subject to
the same transforms as the text.

*********************************************************************************************************************/

static ERR TEXT_GET_Descent(extVectorText *Self, int *Value)
{
   if (not Self->txHandle) {
      if (auto error = reset_font(Self); error != ERR::Okay) return error;
   }

   if (Self->txHandle->type IS CF_BITMAP) *Value = Self->txBitmapFont->Gutter;
   else if (Self->txHandle->type IS CF_FREETYPE) {
      *Value = ((freetype_font::ft_point *)Self->txHandle)->descent;
   }
   else *Value = 1;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
DisplayHeight: The font height measured in pixels.

Use DisplayHeight to retrieve the height of the font, measured in the same pixel units as #FontSize.  For scalable
fonts the value is the sum of the ascent and #Descent.  The value does not account for the display density or for
transforms applied to the text.

*********************************************************************************************************************/

static ERR TEXT_GET_DisplayHeight(extVectorText *Self, int *Value)
{
   if (not Self->txHandle) {
      if (auto error = reset_font(Self); error != ERR::Okay) return error;
   }

   if (Self->txBitmapFont) *Value = Self->txBitmapFont->MaxHeight;
   else if (Self->txHandle->type IS CF_FREETYPE) {
      *Value = ((freetype_font::ft_point *)Self->txHandle)->height;
   }
   else *Value = 1;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
DisplaySize: The FontSize converted to points.

DisplaySize returns the #FontSize converted from pixels to points at the fixed reference density of 96 DPI, calculated
as `FontSize * 72 / 96` and truncated to an integer.  For example, a #FontSize of `16` returns `12`.  The display
density is not taken into account.

This value matches #Point, except that #Point is rounded to the nearest integer rather than truncated.

*********************************************************************************************************************/

static ERR TEXT_GET_DisplaySize(extVectorText *Self, int *Value)
{
   if (not Self->txHandle) {
      if (auto error = reset_font(Self); error != ERR::Okay) return error;
   }

   *Value = int(Self->txFontSize * 72.0 / DISPLAY_DPI);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DX: Adjusts horizontal spacing on a per-character basis.

If a single value is provided, it represents the new relative X coordinate for the current text position for
rendering the glyphs corresponding to the first character within this element or any of its descendants.  The current
text position is shifted along the x-axis of the current user coordinate system by the provided value before the first
character's glyphs are rendered.

If a series of values is provided, then the values represent incremental shifts along the x-axis for the current text
position before rendering the glyphs corresponding to the first n characters within this element or any of its
descendants. Thus, before the glyphs are rendered corresponding to each character, the current text position resulting
from drawing the glyphs for the previous character within the current ‘text’ element is shifted along the X axis of the
current user coordinate system by length.

If more characters exist than values, then for each of these extra characters: (a) if an ancestor Text object
specifies a relative X coordinate for the given character via a #DX field, then the current text position
is shifted along the x-axis of the current user coordinate system by that amount (nearest ancestor has precedence),
else (b) no extra shift along the x-axis occurs.

*********************************************************************************************************************/

static ERR TEXT_GET_DX(extVectorText *Self, std::span<double> &Array)
{
   Array = std::span<double>(Self->txDX.data(), Self->txDX.size());
   return ERR::Okay;
}

static ERR TEXT_SET_DX(extVectorText *Self, std::span<const double> &Array)
{
   if ((not Array.data()) or Array.empty()) Self->txDX.clear();
   else Self->txDX.assign(Array.begin(), Array.end());
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
DY: Adjusts vertical spacing on a per-character basis.

This field follows the same rules described in #DX.

*********************************************************************************************************************/

static ERR TEXT_GET_DY(extVectorText *Self, std::span<double> &Array)
{
   Array = std::span<double>(Self->txDY.data(), Self->txDY.size());
   return ERR::Okay;
}

static ERR TEXT_SET_DY(extVectorText *Self, std::span<const double> &Array)
{
   if ((not Array.data()) or Array.empty()) Self->txDY.clear();
   else Self->txDY.assign(Array.begin(), Array.end());
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OnChange: Receive notifications for changes to the text string.

Set this field with a function reference to receive notifications whenever the text string changes.

The callback function prototype is `void Function(*VectorText)`.

*********************************************************************************************************************/

static ERR TEXT_GET_OnChange(extVectorText *Self, FUNCTION * &Value)
{
   if (Self->txOnChange.defined()) {
      Value = &Self->txOnChange;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR TEXT_SET_OnChange(extVectorText *Self, FUNCTION *Value)
{
   release_callback(Self->txOnChange);
   if (Value) {
      Self->txOnChange = *Value;
      if (Self->txOnChange.defined()) Self->txOnChange.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
OnCursorMove: Receives notifications after cursor or selection changes.

Set this field with a function reference to be notified when the cursor position, selected area or text content
changes.  This is commonly used to update status displays or to keep the cursor in view with #GetCaretBounds().

An operation that moves the cursor or selection more than once triggers a single notification, which reports the
final state.  When the text changes, #OnChange is called first.  Text layout is regenerated before the callback runs,
so caret geometry and cursor fields are up to date.  Cursor or selection changes made from inside the callback do not
trigger another notification.

Notifications are only sent after initialisation, and only when `VTXF::EDITABLE` or `VTXF::SELECTABLE` is set in
#TextFlags.

The callback function prototype is `void Function(*VectorText)`.

*********************************************************************************************************************/

static ERR TEXT_GET_OnCursorMove(extVectorText *Self, FUNCTION * &Value)
{
   if (Self->txOnCursorMove.defined()) {
      Value = &Self->txOnCursorMove;
      return ERR::Okay;
   }
   return ERR::FieldNotSet;
}

static ERR TEXT_SET_OnCursorMove(extVectorText *Self, FUNCTION *Value)
{
   release_callback(Self->txOnCursorMove);
   if (Value) {
      Self->txOnCursorMove = *Value;
      if (Self->txOnCursorMove.defined()) Self->txOnCursorMove.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Face: Defines the font face/family to use in rendering the text string.

The family name of the principal font for rendering text is specified here.

It is possible to list multiple fonts in CSV format in case the first-choice font is unavailable.  For instance,
`Arial,Noto Sans` would select the Noto Sans font if Arial was unavailable in the font database.  The name of the
closest matching font will be stored as the Face value.

*********************************************************************************************************************/

static ERR TEXT_GET_Face(extVectorText *Self, std::string_view &Value)
{
   Value = Self->txFamily;
   return ERR::Okay;
}

static ERR TEXT_SET_Face(extVectorText *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::InvalidValue;

   std::string_view name;
   if (!fnt::ResolveFamilyName(Value, &name)) {
      Self->txFamily = name.empty() ? "Noto Sans" : name;
   }
   else Self->txFamily = "Noto Sans"; // Better to resort to a default than fail completely

   if (Self->initialised()) return reset_font(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Focus: Refers to the object that will be monitored for user focussing.

A VectorText object with `VTXF::EDITABLE` or `VTXF::SELECTABLE` becomes active when its nearest viewport receives the
focus.  Setting the Focus field to a different vector in the scene graph redirects monitoring to it.

Changing this value post-initialisation has no effect.

*********************************************************************************************************************/

static ERR TEXT_GET_Focus(extVectorText *Self, OBJECTID *Value)
{
   *Value = Self->txFocusID;
   return ERR::Okay;
}

static ERR TEXT_SET_Focus(extVectorText *Self, OBJECTID Value)
{
   Self->txFocusID = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Font: Copies key meta information from a Font or other VectorText to create a matching text object.

To create a VectorText object that uses a matching typeset from another @Font or VectorText object, set this field
with a reference to that object.  This can only be done prior to initialisation and the other object must have been
initialised.

*********************************************************************************************************************/

static ERR TEXT_SET_Font(extVectorText *Self, OBJECTPTR Value)
{
   // Setting the Font with a reference to an external font object will copy across the configuration of
   // that font.  It is recommended that the external font is initialised beforehand.

   if (Value->baseClassID() IS CLASSID::FONT) {
      auto other = (objFont *)Value;

      Self->txFamily         = other->Face.empty() ? "Noto Sans" : other->Face;
      Self->txFontSize       = std::max(1.0, std::round(other->Point * (96.0 / 72.0)));
      Self->txScaledFontSize = false;
      Self->txFontStyle      = other->Style.empty() ? "Regular" : other->Style;

      if (Self->initialised()) return reset_font(Self);
      else return ERR::Okay;
   }
   else if (Value->classID() IS CLASSID::VECTORTEXT) {
      auto other = (extVectorText *)Value;

      Self->txFamily         = other->txFamily;
      Self->txFontSize       = other->txFontSize;
      Self->txScaledFontSize = false;
      Self->txFontStyle      = other->txFontStyle;

      if (Self->initialised()) return reset_font(Self);
      else return ERR::Okay;
   }
   else return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
Fill: Defines the fill painter using SVG's IRI format.

The painter used for filling a vector path can be defined through this field using SVG compatible formatting.  The
string is parsed through the ~Vector.ReadPainter() function.  Please refer to it for further details on
valid formatting.

Fallback colours and layered fill painters are supported in the same way as the @Vector.Fill field.  Please refer to it
for further details.

*********************************************************************************************************************/
// Override the existing Vector Fill field - this is required for bitmap fonts as they need a path reset to be
// triggered when decorative changes occur.

static ERR TEXT_GET_Fill(extVectorText *Self, std::string_view &Value)
{
   Value = Self->FillString;
   return ERR::Okay;
}

static ERR TEXT_SET_Fill(extVectorText *Self, const std::string_view &Value)
{
   Self->FillString.clear();
   Self->reset_fill();

   if (Value.empty()) {
      return ERR::Okay;
   }

   if (auto error = read_fill_painters(Self->Scene, Value, Self->Fill, Self->ExtFill); !error) {
      Self->FillString = Value;

      // Bitmap font reset/redraw required
      if (Self->txBitmapFont) reset_path(Self);

      return ERR::Okay;
   }
   else {
      Self->reset_fill();
      return error;
   }
}

/*********************************************************************************************************************
-FIELD-
FontSize: Defines the vertical size of the font.

The FontSize is equivalent to the SVG `font-size` attribute and refers to the height of the font from the dominant
baseline to the hanging baseline.  This would mean that a capitalised letter without accents should fill the entire
vertical space defined by FontSize.

The default unit of FontSize is the pixel, and the value defines the em size of the font.  A FontSize of `16` produces
an em that is 16 pixels high in the text's coordinate space, which matches the behaviour of CSS and SVG.  The display
density is not applied, so the size on the display only changes if the text is transformed.

Standard unit measurements such as `px`, `pt`, `pc`, `in`, `cm`, `mm` and `em` are supported by appending them after
the numeric value.  Physical units are converted to pixels at a fixed reference density of 96 DPI, irrespective of the
display, so `12pt` is equivalent to `16px`.  Clients that need a physical size on a high density display must scale
the value themselves, for example by multiplying a point size by `DisplayDPI / 96`.  The `em` unit is fixed at 16
pixels.  The final value is rounded to the nearest whole pixel, so `11pt` (14.67px) becomes `15px`.  A size that
rounds to less than one pixel is rejected with `ERR::OutOfRange`.

Reading FontSize returns the pixel size as a numeric string without a unit suffix.

NOTE: The FontSize is derived from information defined by the font author.  Due to opinionated views on how font
height is determined, the value is not transferrable between fonts.  The primary difference is that some fonts will
include the space for accents in the FontSize value, while others will not.

*********************************************************************************************************************/

static ERR TEXT_GET_FontSize(extVectorText *Self, std::string_view &Value)
{
   Self->txFontSizeString = std::to_string(Self->txFontSize);
   Value = Self->txFontSizeString;
   return ERR::Okay;
}

static ERR TEXT_SET_FontSize(extVectorText *Self, const std::string_view &Value)
{
   bool pct;

   CSTRING val = Value.data();
   auto size = read_unit(val, pct);

   // TODO: With respect to supporting sub-pixel point sizes and being cache-friendly, we could try caching fonts
   // at pre-determined point sizes (4,6,8,10,12,14,20,30,40,50,60,...) and then use scaling to cater to other
   // 'non-standard' sizes.  This would allow for hinting to remain effective when regular point sizes are being chosen.

   // Rounding ensures that physical units resolve to the nearest pixel, e.g. 11pt is 14.67px and becomes 15px.

   if (auto new_size = std::round(size); new_size >= 1) {
      if (Self->txFontSize IS new_size) return ERR::Okay;
      Self->txFontSize = new_size;
      Self->txScaledFontSize = pct;
      if (Self->initialised()) return reset_font(Self);
      else return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
FontStyle: Determines font styling.

Unique styles for a font can be selected through the FontStyle field.  Conventional font styles are `Bold`,
`Bold Italic`, `Italic` and `Regular` (the default).  Because TrueType fonts can use any style name that the
designer chooses such as `Thin`, `Narrow` or `Wide`, read `fonts:fonts.cfg` for a definitive list of available
style names.

Errors are not returned if the style name is invalid or unavailable.

*********************************************************************************************************************/

static ERR TEXT_GET_FontStyle(extVectorText *Self, std::string_view &Value)
{
   Value = Self->txFontStyle;
   return ERR::Okay;
}

static ERR TEXT_SET_FontStyle(extVectorText *Self, const std::string_view &Value)
{
   if (Value.empty()) Self->txFontStyle = "Regular";
   else Self->txFontStyle = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
InlineSize: Enables word-wrapping at a fixed area size.

The inline-size property allows one to set the wrapping area to a rectangular shape. The computed value of the
property sets the width of the rectangle for horizontal text and the height of the rectangle for vertical text.
The other dimension (height for horizontal text, width for vertical text) is of infinite length. A value of zero
(the default) disables the creation of a wrapping area.

*********************************************************************************************************************/

static ERR TEXT_GET_InlineSize(extVectorText *Self, double *Value)
{
   *Value = Self->txInlineSize;
   return ERR::Okay;
}

static ERR TEXT_SET_InlineSize(extVectorText *Self, double Value)
{
   Self->txInlineSize = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
LetterSpacing: Private.  Currently unsupported.
-END-
*********************************************************************************************************************/

// SVG standard, presuming this inserts space as opposed to acting as a multiplier

static ERR TEXT_GET_LetterSpacing(extVectorText *Self, double *Value)
{
   *Value = Self->txLetterSpacing;
   return ERR::Okay;
}

static ERR TEXT_SET_LetterSpacing(extVectorText *Self, double Value)
{
   Self->txLetterSpacing = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
LineLimit: Restricts the total number of lines allowed in a text object.

Set the LineLimit field to restrict the maximum number of lines permitted in a text object.  It is common to set this
field to a value of 1 for input boxes that have a limited amount of space available.

*********************************************************************************************************************/

static ERR TEXT_GET_LineLimit(extVectorText *Self, int *Value)
{
   *Value = Self->txLineLimit;
   return ERR::Okay;
}

static ERR TEXT_SET_LineLimit(extVectorText *Self, int Value)
{
   Self->txLineLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
LineSpacing: The number of pixels from one line to the next.

This field can be queried for the vertical advance between each line of text.  The value is measured in the same pixel
units as #FontSize, so it is unaffected by the display density and is subject to the same transforms as the text.

*********************************************************************************************************************/

static ERR TEXT_GET_LineSpacing(extVectorText *Self, int *Value)
{
   if (not Self->txHandle) {
      if (auto error = reset_font(Self); error != ERR::Okay) return error;
   }

   if (Self->txBitmapFont) {
      *Value = Self->txBitmapFont->LineSpacing;
      return ERR::Okay;
   }
   else if (Self->txHandle->type IS CF_FREETYPE) {
      *Value = ((freetype_font::ft_point *)Self->txHandle)->line_spacing;
      return ERR::Okay;
   }
   else {
      *Value = 1;
      return ERR::InvalidState;
   }
}

/*********************************************************************************************************************
-FIELD-
Point: Returns the point-size of the font.

Reading the Point value will return the point-size of the font, calculated as `FontSize * 72 / 96` and rounded to the
nearest integer.  The conversion uses the fixed reference density of 96 DPI that applies to all #FontSize units, not
the density of the display.

*********************************************************************************************************************/

static ERR TEXT_GET_Point(extVectorText *Self, int *Value)
{
   *Value = std::round(Self->txFontSize * (72.0 / DISPLAY_DPI));
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
SelectColumn: Indicates the column position of a selection's beginning.

If the user has selected an area of text, the starting column of that area will be indicated by this field.  If an area
has not been selected, the value of the SelectColumn field is undefined.

To check whether or not an area has been selected, test the `AREA_SELECTED` bit in the #TextFlags field.

*********************************************************************************************************************/

static ERR TEXT_GET_SelectColumn(extVectorText *Self, int *Value)
{
   *Value = Self->txCursor.selectColumn;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
SelectRow: Indicates the line position of a selection's beginning.

If the user has selected an area of text, the starting row of that area will be indicated by this field.  If an area
has not been selected, the value of the SelectRow field is undefined.

To check whether or not an area has been selected, test the `AREA_SELECTED` bit in the #TextFlags field.

*********************************************************************************************************************/

static ERR TEXT_GET_SelectRow(extVectorText *Self, int *Value)
{
   *Value = Self->txCursor.selectRow;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Spacing: Private.  Not currently implemented.

*********************************************************************************************************************/

static ERR TEXT_GET_Spacing(extVectorText *Self, double *Value)
{
   *Value = Self->txSpacing;
   return ERR::Okay;
}

static ERR TEXT_SET_Spacing(extVectorText *Self, double Value)
{
   Self->txSpacing = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
StartOffset: Private.  Not currently implemented.

*********************************************************************************************************************/

static ERR TEXT_GET_StartOffset(extVectorText *Self, double *Value)
{
   *Value = Self->txStartOffset;
   return ERR::Okay;
}

static ERR TEXT_SET_StartOffset(extVectorText *Self, double Value)
{
   Self->txStartOffset = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
TextFlags: Controls text presentation, editing and selection.

-END-
*********************************************************************************************************************/

static ERR TEXT_GET_Flags(extVectorText *Self, VTXF *Value)
{
   *Value = Self->txFlags;
   return ERR::Okay;
}

static ERR TEXT_SET_Flags(extVectorText *Self, VTXF Value)
{
   CursorUpdate update(Self);
   const auto previous_flags = Self->txFlags;
   constexpr auto input_flags = VTXF::EDITABLE|VTXF::SELECTABLE;
   const bool had_caret_geometry = Self->needs_caret_geometry();
   Self->txFlags = Value;
   if ((Value & VTXF::NO_AUTO_OFFSET) != VTXF::NIL) {
      Self->txXOffset = 0;
      Self->txYOffset = 0;
      reset_path(Self);
   }
   if (Self->needs_caret_geometry() and not had_caret_geometry) reset_path(Self);
   if ((Value & VTXF::EDITABLE) IS VTXF::NIL) {
      if (Self->txCursor.vector) Self->txCursor.vector->setVisibility(VIS::HIDDEN);
      if (Self->txCursor.timer) { UpdateTimer(Self->txCursor.timer, 0); Self->txCursor.timer = nullptr; }
   }
   if (Self->initialised() and Self->txHasFocus and Self->needs_caret_geometry() and
       ((previous_flags & input_flags) != (Value & input_flags))) {
      activate_text_input(Self);
   }
   else selection_changed(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X: The x coordinate of the text.

The x-axis coordinate of the text is specified here as a fixed value.  Scaled coordinates are not supported.

*********************************************************************************************************************/

static ERR TEXT_GET_X(extVectorText *Self, Unit &Value)
{
   Value = Self->txX;
   return ERR::Okay;
}

static ERR TEXT_SET_X(extVectorText *Self, Unit &Value)
{
   Self->txX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y: The base-line y coordinate of the text.

The Y-axis coordinate of the text is specified here as a fixed value.  Scaled coordinates are not supported.

Unlike other vector shapes, the Y coordinate positions the text from its base line rather than the top of the shape.

*********************************************************************************************************************/

static ERR TEXT_GET_Y(extVectorText *Self, Unit &Value)
{
   Value = Self->txY;
   return ERR::Okay;
}

static ERR TEXT_SET_Y(extVectorText *Self, Unit &Value)
{
   Self->txY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Rotate: Applies vertical spacing on a per-character basis.

Applies supplemental rotation about the current text position for all of the glyphs in the text string.

If multiple values are provided, then the first number represents the supplemental rotation for the glyphs
corresponding to the first character within this element or any of its descendants, the second number represents the
supplemental rotation for the glyphs that correspond to the second character, and so on.

If more numbers are provided than there are characters, then the extra numbers will be ignored.

If more characters are provided than numbers, then for each of these extra characters the rotation value specified by
the last number must be used.

If the attribute is not specified and if an ancestor 'text' or 'tspan' element specifies a supplemental rotation for a
given character via a 'rotate' attribute, then the given supplemental rotation is applied to the given character
(nearest ancestor has precedence). If there are more characters than numbers specified in the ancestor's 'rotate'
attribute, then for each of these extra characters the rotation value specified by the last number must be used.

This supplemental rotation has no impact on the rules by which current text position is modified as glyphs get rendered
and is supplemental to any rotation due to text on a path and to 'glyph-orientation-horizontal' or
'glyph-orientation-vertical'.

*********************************************************************************************************************/

static ERR TEXT_GET_Rotate(extVectorText *Self, std::span<double> &Array)
{
   Array = std::span<double>(Self->txRotate.data(), Self->txRotate.size());
   return ERR::Okay;
}

static ERR TEXT_SET_Rotate(extVectorText *Self, std::span<const double> &Array)
{
   if ((not Array.data()) or Array.empty()) Self->txRotate.clear();
   else Self->txRotate.assign(Array.begin(), Array.end());
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
SelectFill: The painter used to highlight selected editable text.

Accepts the same colours, paint servers and layered painters as @Vector.Fill.  The default is
`rgba(0,120,215,0.35)`.  The highlight is drawn over the glyphs, so a translucent painter keeps them legible.
Selection highlights follow the text's transforms and are hidden while focus is lost.

*********************************************************************************************************************/

static ERR TEXT_GET_SelectFill(extVectorText *Self, std::string_view &Value)
{
   Value = Self->txSelectFill;
   return ERR::Okay;
}

static ERR TEXT_SET_SelectFill(extVectorText *Self, const std::string_view &Value)
{
   if (Self->txSelection) {
      if (auto error = Self->txSelection->setFill(Value); error != ERR::Okay) {
         Self->txSelection->setFill(Self->txSelectFill);
         return error;
      }
   }
   Self->txSelectFill = Value;
   selection_changed(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
SelectedText: The selected text, with line breaks separating selected lines.

Returns an empty string when there is no selection or the `SECRET` flag is set in #TextFlags.

*********************************************************************************************************************/

static ERR TEXT_GET_SelectedText(extVectorText *Self, std::string &Value)
{
   Value = selected_text(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
ShapeInside: Reference a vector shape to define a content area that enables word-wrapping.

This property enables word-wrapping in which the text will conform to the path of a @Vector shape.  Internally this is
achieved by rendering the vector path as a mask and then fitting the text within the mask without crossing its
boundaries.

This feature is computationally expensive and the use of #InlineSize is preferred if the text can be wrapped to
a rectangular area.

*********************************************************************************************************************/

static ERR TEXT_GET_ShapeInside(extVectorText *Self, OBJECTID *Value)
{
   *Value = Self->txShapeInsideID;
   return ERR::Okay;
}

static ERR TEXT_SET_ShapeInside(extVectorText *Self, OBJECTID Value)
{
   Self->txShapeInsideID = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
ShapeSubtract: Excludes a portion of the content area from the wrapping area.

This property can be used in conjunction with #ShapeInside to further restrict the content area that is available
for word-wrapping.  It has no effect if #ShapeInside is undefined.

*********************************************************************************************************************/

static ERR TEXT_GET_ShapeSubtract(extVectorText *Self, OBJECTID *Value)
{
   *Value = Self->txShapeSubtractID;
   return ERR::Okay;
}

static ERR TEXT_SET_ShapeSubtract(extVectorText *Self, OBJECTID Value)
{
   Self->txShapeSubtractID = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
String: The string to use for drawing the glyphs is defined here.

The string for drawing the glyphs is defined here in UTF-8 format.  Lines are separated by line feeds.

The retrieved string contains every line of the text, separated by line feeds, including any changes made by the user
while editing.  Line breaks introduced by word-wrapping (see #InlineSize) are not included.

*********************************************************************************************************************/

static ERR TEXT_GET_String(extVectorText *Self, std::string &Value)
{
   Value.clear();
   for (size_t i=0; i < Self->txLines.size(); i++) {
      if (i > 0) Value.push_back('\n');
      Value.append(Self->txLines[i]);
   }
   return ERR::Okay;
}

static ERR TEXT_SET_String(extVectorText *Self, const std::string_view &Value)
{
   CursorUpdate update(Self);
   std::string previous;
   TEXT_GET_String(Self, previous);
   Self->txCursorChanged |= previous != Value;
   Self->txLines.clear();

   if (not Value.empty()) {
      std::string_view scan(Value);
      while (not scan.empty()) {
         size_t total;
         for (total=0; (total < scan.size()) and (scan[total] != '\n'); total++);
         Self->txLines.emplace_back(std::string(scan.data(), total));
         scan.remove_prefix(total);
         if ((not scan.empty()) and (scan[0] IS '\n')) scan.remove_prefix(1);
      }
      if (Value.back() IS '\n') Self->txLines.emplace_back();
   }

   reset_path(Self);
   Self->txCursor.move(Self, Self->txCursor.row(), Self->txCursor.column(), true);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
TextLength: The expected length of the text after all computations have been taken into account.

The purpose of this attribute is to allow exact alignment of the text graphic in the computed result.  If the computed
text width does not match this value, then the text will be scaled to match the TextLength.

*********************************************************************************************************************/

// NB: Internally we can fulfil TextLength requirements simply by checking the width of the text path boundary
// and if they don't match, apply a rescale transformation just prior to drawing (Width * (TextLength / Width))

static ERR TEXT_GET_TextLength(extVectorText *Self, double *Value)
{
   *Value = Self->txTextLength;
   return ERR::Okay;
}

static ERR TEXT_SET_TextLength(extVectorText *Self, double Value)
{
   Self->txTextLength = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
TextWidth: The pixel width of the widest line in the #String field.

This field will return the pixel width of the widest line in the #String field.  The result is not modified by
transforms.

*********************************************************************************************************************/

static ERR TEXT_GET_TextWidth(extVectorText *Self, int *Value)
{
   if (not Self->initialised()) return ERR::NotInitialised;

   if (not Self->txHandle) {
      if (auto error = reset_font(Self); error != ERR::Okay) return error;
   }

   int width = 0;
   for (auto &line : Self->txLines) {
      if (Self->txBitmapFont) {
         auto w = fnt::StringWidth(Self->txBitmapFont, line, -1);
         if (w > width) width = w;
      }
      else {
         auto w = string_width(Self, line);
         if (w > width) width = w;
      }
   }
   *Value = width;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
TotalLines: The total number of lines stored in the object.

*********************************************************************************************************************/

static ERR TEXT_GET_TotalLines(extVectorText *Self, int *Value)
{
   *Value = Self->txLines.size();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Weight: Defines the level of boldness in the text.

The weight value determines the level of boldness in the text.  A default value of 400 will render the text in its
normal state.  Lower values between 100 to 300 render the text in a light format, while high values in the range of
400 - 900 result in boldness.

Please note that setting the Weight will give it priority over the #FontStyle value.
-END-
*********************************************************************************************************************/

static ERR TEXT_GET_Weight(extVectorText *Self, int *Value)
{
   *Value = Self->txWeight;
   return ERR::Okay;
}

static ERR TEXT_SET_Weight(extVectorText *Self, int Value)
{
   if ((Value >= 100) and (Value <= 900)) {
      Self->txWeight = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

//********************************************************************************************************************
// Calculate the cursor that would be displayed at this character position and save it to the
// line's chars array.

static void calc_caret_position(TextLine &Line, agg::trans_affine &Transform, double Ascent, double Descent)
{
   agg::path_storage cursor_path;
   double cx1, cy1, cx2, cy2;

   cursor_path.move_to(0, -Ascent * 1.2);
   cursor_path.line_to(0, Ascent * 0.1);
   agg::conv_transform<agg::path_storage, agg::trans_affine> trans_cursor(cursor_path, Transform);

   trans_cursor.vertex(&cx1, &cy1);
   trans_cursor.vertex(&cx2, &cy2);
   Line.chars.emplace_back(cx1, cy1, cx2, cy2);
   double top_x = 0, top_y = -Ascent, bottom_x = 0, bottom_y = Descent;
   Transform.transform(&top_x, &top_y);
   Transform.transform(&bottom_x, &bottom_y);
   Line.chars.back().lineTop = std::min(top_y, bottom_y);
   Line.chars.back().lineHeight = std::abs(bottom_y - top_y);
}

//********************************************************************************************************************

extern void set_text_final_xy(extVectorText *Vector)
{
   Unit x = Vector->txX, y = Vector->txY;

   if (x.scaled()) x = Unit(x * get_parent_width(Vector));
   if (y.scaled()) y = Unit(y * get_parent_height(Vector));

   if ((Vector->txAlignFlags & ALIGN::RIGHT) != ALIGN::NIL) x = x - Vector->txWidth;
   else if ((Vector->txAlignFlags & ALIGN::HORIZONTAL) != ALIGN::NIL) x = x - (Vector->txWidth * 0.5);

   if (Vector->txBitmapFont) {
      // Rastered fonts need an adjustment because the Y coordinate corresponds to the base-line.
      y = y - (Vector->txBitmapFont->Height + Vector->txBitmapFont->Leading);
   }

   Vector->FinalX = x + Vector->txXOffset;
   Vector->FinalY = y + Vector->txYOffset;
}

//********************************************************************************************************************
// (Re)loads the font for a text object.  This is a resource intensive exercise that should be avoided until the object
// is ready to initialise.

static ERR reset_font(extVectorText *Vector, bool Force)
{
   if ((not Vector->initialised()) and (not Force)) return ERR::NotInitialised;

   kt::Log log;
   if (auto error = get_font(log, Vector->txFamily, Vector->txFontStyle, Vector->txWeight, Vector->txFontSize, &Vector->txHandle); !error) {
      if (Vector->txHandle->type IS CF_BITMAP) {
         Vector->txBitmapFont = ((bmp_font *)Vector->txHandle)->font;
         Vector->txFontSize = std::trunc(double(Vector->txBitmapFont->Height) * (DISPLAY_DPI / 72.0));
      }
      mark_dirty(Vector, RC::DIRTY);
      return ERR::Okay;
   }
   else return log.warning(error);
 }

//********************************************************************************************************************

static ERR cursor_timer(extVectorText *Self, int64_t Elapsed, int64_t CurrentTime)
{
   if (((Self->txFlags & VTXF::EDITABLE) != VTXF::NIL) and (Self->txCursor.vector)) {
      kt::Log log(__FUNCTION__);
      Self->txCursor.flash ^= 1;
      Self->txCursor.vector->setVisibility(Self->txCursor.flash ? VIS::VISIBLE : VIS::HIDDEN);
      acDraw(Self);
      return ERR::Okay;
   }
   else {
      Self->txCursor.timer = 0;
      return ERR::Terminate;
   }
}

//********************************************************************************************************************

static void activate_text_input(extVectorText *Self)
{
   if (not Self->needs_caret_geometry()) return;
   Self->txSelectionHidden = false;
   selection_changed(Self);

   if (not Self->txKeyEvent) {
      SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(key_event, Self), &Self->txKeyEvent);
   }

   if (((Self->txFlags & VTXF::EDITABLE) != VTXF::NIL) and Self->txCursor.vector) {
      acMoveToFront(Self->txCursor.vector);
      if (Self->txCursor.timer) UpdateTimer(Self->txCursor.timer, 1.0);
      else SubscribeTimer(0.8, C_FUNCTION(cursor_timer), &Self->txCursor.timer);
      Self->txCursor.resetFlash();
      Self->txCursor.vector->setVisibility(VIS::VISIBLE);
   }
   acDraw(Self);
}

//********************************************************************************************************************

static ERR text_focus_event(class extVector *Vector, FM Event, OBJECTPTR EventObject, APTR Meta)
{
   auto Self = (extVectorText *)CurrentContext();

   if ((Event & FM::HAS_FOCUS) != FM::NIL) {
      Self->txHasFocus = true;
      activate_text_input(Self);
   }
   else if ((Event & (FM::LOST_FOCUS|FM::CHILD_HAS_FOCUS)) != FM::NIL) {
      Self->txHasFocus = false;
      if (Self->txCursor.vector) Self->txCursor.vector->setVisibility(VIS::HIDDEN);
      if (Self->txCursor.timer)  { UpdateTimer(Self->txCursor.timer, 0); Self->txCursor.timer = 0; }
      if (Self->txKeyEvent)      { UnsubscribeEvent(Self->txKeyEvent); Self->txKeyEvent = nullptr; }

      Self->txDragging = false;
      Self->txDragPending = false;
      Self->txMouseShift = false;
      Self->txClickCount = 0;
      if (Self->txDragTimer) { UpdateTimer(Self->txDragTimer, 0); Self->txDragTimer = nullptr; }
      Self->txSelectionHidden = true;
      selection_changed(Self);

      acDraw(Self);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

struct TextPosition {
   int row, column;
};

static void select_mouse_word(extVectorText *, TextPosition);

// Carets use text coordinates; the pointer uses scene coordinates.  The complete transform includes alignment,
// scrolling, the text position and parent viewboxes.  Distance to a segment also handles rotated caret edges.
static TextPosition hit_test(extVectorText *Self, double X, double Y, bool Character = false, int Row = -1)
{
   gen_vector_tree(Self);
   double shortest_dist = std::numeric_limits<double>::max();
   TextPosition nearest { std::max(0, Row), 0 };
   for (int row=0; row < std::ssize(Self->txLines); row++) {
      if ((Row >= 0) and (row != Row)) continue;
      const auto &chars = Self->txLines[row].chars;
      for (int col=0; col + (Character ? 1 : 0) < std::ssize(chars); col++) {
         double x1 = chars[col].x1, y1 = chars[col].y1;
         double x2 = chars[col].x2, y2 = chars[col].y2;
         if (Character) {
            // Measure along the glyph's centre line: its right half still belongs to this character even
            // when the nearest insertion caret is at the beginning of the next word or whitespace run.
            x1 = (chars[col].x1 + chars[col].x2) * 0.5;
            y1 = (chars[col].y1 + chars[col].y2) * 0.5;
            x2 = (chars[col + 1].x1 + chars[col + 1].x2) * 0.5;
            y2 = (chars[col + 1].y1 + chars[col + 1].y2) * 0.5;
         }
         Self->Transform.transform(&x1, &y1);
         Self->Transform.transform(&x2, &y2);
         const auto dx = x2 - x1, dy = y2 - y1;
         const auto length = dx * dx + dy * dy;
         const auto t = length > 0 ? std::clamp(((X - x1) * dx + (Y - y1) * dy) / length, 0.0, 1.0) : 0.0;
         const auto distance = std::hypot(X - x1 - t * dx, Y - y1 - t * dy);
         if (distance < shortest_dist) {
            shortest_dist = distance;
            nearest = { row, col };
         }
      }
   }
   return nearest;
}

static void extend_mouse_selection(extVectorText *Self)
{
   Self->txDragPending = false;
   const auto position = hit_test(Self, Self->txDragX, Self->txDragY);
   if ((position.row != Self->txCursor.row()) or (position.column != Self->txCursor.column())) {
      Self->txCursor.extend(Self, position.row, position.column);
      Self->txCursor.resetFlash();
   }
}

// Coalesce pointer movement into one selection update per frame.  Release flushes a pending endpoint immediately.
static ERR text_drag_timer(extVectorText *Self, int64_t Elapsed, int64_t CurrentTime)
{
   Self->txDragTimer = nullptr;
   if (Self->txDragging and Self->txDragPending) extend_mouse_selection(Self);
   return ERR::Terminate;
}

static ERR text_input_events(class extVector *Vector, const InputEvent *Events)
{
   auto Self = (extVectorText *)CurrentContext();
   if (not Self->needs_caret_geometry()) return ERR::Okay;
   for (; Events; Events = Events->Next) {
      CursorUpdate update(Self);
      if ((Events->Type IS JET::LMB) and ((Events->Flags & JTYPE::REPEATED) IS JTYPE::NIL)) {
         if (Events->Value <= 0) {
            if (Self->txDragging and Self->txDragPending) extend_mouse_selection(Self);
            Self->txDragging = false;
            if (Self->txDragTimer) { UpdateTimer(Self->txDragTimer, 0); Self->txDragTimer = nullptr; }
            continue;
         }

         const auto position = hit_test(Self, Events->AbsX, Events->AbsY);
         double interval = 0.3;
         int slop = 4;
         OBJECTID pointer_id;
         if (FindObject("SystemPointer", CLASSID::POINTER, &pointer_id) IS ERR::Okay) {
            if (kt::ScopedObjectLock<objPointer> pointer(pointer_id); pointer.granted()) {
               interval = pointer->DoubleClick;
               slop = pointer->ClickSlop;
            }
         }
         Self->txClickSlop = slop;
         const auto elapsed = Events->Timestamp - Self->txClickTime;
         const bool consecutive = (Self->txClickCount > 0) and (elapsed >= 0) and
            (elapsed <= interval * 1000000.0) and
            (std::hypot(Events->AbsX - Self->txClickX, Events->AbsY - Self->txClickY) <= slop);
         Self->txClickCount = consecutive ? Self->txClickCount % 3 + 1 : 1;
         Self->txClickTime = Events->Timestamp;
         Self->txClickX = Events->AbsX;
         Self->txClickY = Events->AbsY;

         if (Self->txMouseShift) {
            Self->txCursor.extend(Self, position.row, position.column);
            Self->txClickCount = 1;
         }
         else if ((Self->txClickCount IS 3) and not Self->txLines.empty()) {
            Self->txCursor.move(Self, position.row, 0);
            if (position.row + 1 < std::ssize(Self->txLines)) Self->txCursor.extend(Self, position.row + 1, 0);
            else Self->txCursor.extend(Self, position.row, Self->txLines[position.row].utf8Length());
         }
         else if (Self->txClickCount IS 2) {
            select_mouse_word(Self, hit_test(Self, Events->AbsX, Events->AbsY, true, position.row));
         }
         else Self->txCursor.move(Self, position.row, position.column);
         Self->txCursor.resetFlash();
         Self->txDragging = true;
         Self->txDragPending = false;
      }
      else if (Self->txDragging and ((Events->Flags & JTYPE::MOVEMENT) != JTYPE::NIL)) {
         Self->txDragX = Events->AbsX;
         Self->txDragY = Events->AbsY;
         Self->txDragPending = true;
         // Moving beyond click slop breaks the multi-click sequence.
         if (std::hypot(Events->AbsX - Self->txClickX, Events->AbsY - Self->txClickY) > Self->txClickSlop) {
            Self->txClickCount = 0;
         }
         if (not Self->txDragTimer) SubscribeTimer(1.0 / 60.0, C_FUNCTION(text_drag_timer), &Self->txDragTimer);
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static TextCharClass text_character_class(uint32_t Unicode)
{
   auto in_ranges = [Unicode](const auto &Ranges) {
      const auto match = std::lower_bound(Ranges.begin(), Ranges.end(), Unicode,
         [](const TextUnicodeRange &Range, uint32_t Value) { return Range.last < Value; });
      return (match != Ranges.end()) and (match->first <= Unicode);
   };
   if ((Unicode IS '_') or in_ranges(glTextWordRanges)) return TextCharClass::WORD;
   if (in_ranges(glTextWhitespaceRanges)) return TextCharClass::SPACE;
   return TextCharClass::SEPARATOR;
}

// Shared Unicode classification for keyboard boundaries and mouse word/separator/whitespace runs.
static const std::vector<TextCharClass> & text_character_classes(TextLine &Line)
{
   if (Line.classesValid) return Line.characterClasses;
   auto &classes = Line.characterClasses;
   classes.clear();
   auto remaining = std::string_view(Line);
   while (not remaining.empty()) {
      int length;
      const auto unicode = UTF8ReadValue(remaining, &length);
      classes.push_back(text_character_class(unicode));
      remaining.remove_prefix(std::max(1, length));
   }
   Line.classesValid = true;
   return classes;
}

static void select_mouse_word(extVectorText *Self, TextPosition Position)
{
   if (Self->txLines.empty()) return;
   const auto &classes = text_character_classes(Self->txLines[Position.row]);
   if (classes.empty()) {
      Self->txCursor.move(Self, Position.row, 0);
      return;
   }
   int first = std::min(Position.column, int(classes.size()) - 1), last = first + 1;
   const auto type = classes[first];
   while ((first > 0) and (classes[first - 1] IS type)) first--;
   while ((last < std::ssize(classes)) and (classes[last] IS type)) last++;
   Self->txCursor.move(Self, Position.row, first);
   Self->txCursor.extend(Self, Position.row, last);
}

// Classification survives navigation and is invalidated only when a line's text changes.
// A logical line break is whitespace, so word movement can cross empty lines.
static TextPosition word_boundary(extVectorText *Self, TextPosition Position, bool Forward)
{
   auto character = [&](TextPosition At) -> std::optional<TextCharClass> {
      const auto &classes = text_character_classes(Self->txLines[At.row]);
      if (At.column < std::ssize(classes)) return classes[At.column];
      if (At.row + 1 < std::ssize(Self->txLines)) return TextCharClass::SPACE;
      return std::nullopt;
   };
   auto advance = [&]() {
      const auto &classes = text_character_classes(Self->txLines[Position.row]);
      if (Position.column < std::ssize(classes)) Position.column++;
      else { Position.row++; Position.column = 0; }
   };
   auto previous = [&]() -> std::optional<TextPosition> {
      if (Position.column > 0) return TextPosition { Position.row, Position.column - 1 };
      if (Position.row IS 0) return std::nullopt;
      const auto &classes = text_character_classes(Self->txLines[Position.row - 1]);
      return TextPosition { Position.row - 1, int(classes.size()) };
   };

   if (Forward) {
      auto type = character(Position);
      if (type and *type != TextCharClass::SPACE) {
         do { advance(); } while (character(Position) IS type);
      }
      while (character(Position) IS TextCharClass::SPACE) advance();
   }
   else {
      auto before = previous();
      while (before and (character(*before) IS TextCharClass::SPACE)) {
         Position = *before;
         before = previous();
      }
      if (before) {
         const auto type = character(*before);
         do {
            Position = *before;
            before = previous();
         } while (before and (character(*before) IS type));
      }
   }
   return Position;
}

static bool navigate_text(extVectorText *Self, const evKey *Event)
{
   const bool shift = (Event->Qualifiers & KQ::SHIFT) != KQ::NIL;
   const bool ctrl = (Event->Qualifiers & KQ::CTRL) != KQ::NIL;
   const bool system_keys = (Self->txFlags & VTXF::NO_SYS_KEYS) IS VTXF::NIL;
   if (ctrl and (Event->Code IS KEY::A)) {
      if (system_keys and not Self->txLines.empty()) {
         Self->txCursor.move(Self, 0, 0);
         Self->txCursor.extend(Self, int(Self->txLines.size()) - 1, Self->txLines.back().utf8Length());
      }
      return true;
   }
   switch (Event->Code) {
      case KEY::LEFT: case KEY::RIGHT: case KEY::UP: case KEY::DOWN: case KEY::HOME: case KEY::END: break;
      default: return false;
   }

   Self->txCursor.validate_position(Self);
   auto &cursor = Self->txCursor;
   TextPosition position { cursor.row(), cursor.column() };
   if (not shift and ((Event->Code IS KEY::LEFT) or (Event->Code IS KEY::RIGHT)) and
       ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL)) {
      int row, column, end_row, end_column;
      cursor.selectedArea(Self, &row, &column, &end_row, &end_column);
      if (Event->Code IS KEY::LEFT) cursor.move(Self, row, column);
      else cursor.move(Self, end_row, end_column);
      return true;
   }

   int preferred_column = -1;
   switch (Event->Code) {
      case KEY::HOME: position.column = 0; break;
      case KEY::END: position.column = Self->txLines[position.row].utf8Length(); break;
      case KEY::LEFT:
         if (ctrl and system_keys) position = word_boundary(Self, position, false);
         else if (position.column > 0) position.column--;
         else if (position.row > 0) {
            position.row--;
            position.column = Self->txLines[position.row].utf8Length();
         }
         break;
      case KEY::RIGHT:
         if (ctrl and system_keys) position = word_boundary(Self, position, true);
         else if (position.column < Self->txLines[position.row].utf8Length()) position.column++;
         else if (position.row + 1 < std::ssize(Self->txLines)) { position.row++; position.column = 0; }
         break;
      case KEY::UP: case KEY::DOWN:
         preferred_column = cursor.preferredColumn >= 0 ? cursor.preferredColumn : position.column;
         position.row = std::clamp(position.row + (Event->Code IS KEY::UP ? -1 : 1),
            0, int(Self->txLines.size()) - 1);
         position.column = std::min(preferred_column, Self->txLines[position.row].utf8Length());
         break;
      default: break;
   }
   if (shift) cursor.extend(Self, position.row, position.column);
   else cursor.move(Self, position.row, position.column, true);
   cursor.preferredColumn = preferred_column;
   return true;
}

static void key_event(evKey *Event, int Size, extVectorText *Self)
{
   CursorUpdate update(Self);
   if (not Self->needs_caret_geometry()) return;
   Self->txMouseShift = (Event->Qualifiers & KQ::SHIFT) != KQ::NIL;
   if ((Event->Qualifiers & KQ::PRESSED) IS KQ::NIL) return;

   kt::Log log(__FUNCTION__);

   log.trace("$%.8x, Value: %d", int(Event->Qualifiers), int(Event->Code));

   if (((Self->txFlags & VTXF::EDITABLE) != VTXF::NIL) and Self->txCursor.vector) {
      Self->txCursor.resetFlash(); // Reset the flashing cursor to make it visible
      Self->txCursor.vector->setVisibility(VIS::VISIBLE);
   }

   if (((Self->txFlags & VTXF::NO_SYS_KEYS) IS VTXF::NIL) and ((Event->Qualifiers & KQ::CTRL) != KQ::NIL)) {
      switch(Event->Code) {
         case KEY::C: // Copy
            acClipboard(Self, CLIPMODE::COPY);
            return;

         case KEY::X: // Cut
            if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;
            acClipboard(Self, CLIPMODE::CUT);
            return;

         case KEY::V: // Paste
            if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;
            acClipboard(Self, CLIPMODE::PASTE);
            return;

         case KEY::K: { // Delete line
            if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;
            const bool changed = (Self->txLines.size() > 1) or
               ((not Self->txLines.empty()) and (not Self->txLines.front().empty()));
            if ((((objVectorText *)Self)->deleteLine(Self->txCursor.row()) IS ERR::Okay) and changed) {
               report_change(Self);
            }
            return;
         }

         case KEY::Z: // Undo
            if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;
            return;

         case KEY::Y: // Redo
            if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;
            return;

         default:
            break;
      }
   }

   if (navigate_text(Self, Event)) return;

   if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;

   if ((Event->Qualifiers & KQ::NOT_PRINTABLE) IS KQ::NIL) { // and (!(Flags & KQ::INSTRUCTIONKEYS))
      // Printable character handling

      insert_char(Self, Event->Unicode);
      return;
   }

   switch(Event->Code) {
   case KEY::BACKSPACE:
      if ((Self->txFlags & VTXF::AREA_SELECTED) IS VTXF::NIL) {
         Self->txCursor.validate_position(Self);
         const auto row = Self->txCursor.row(), column = Self->txCursor.column();
         if (column > 0) Self->txCursor.extend(Self, row, column - 1);
         else if (row > 0) Self->txCursor.extend(Self, row - 1, Self->txLines[row - 1].utf8Length());
      }
      if (replace_selection(Self, "")) report_change(Self);
      break;

   case KEY::CLEAR:
      if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) {
         if (replace_selection(Self, "")) report_change(Self);
      }
      else {
         Self->txCursor.move(Self, Self->txCursor.row(), 0);
         ((objVectorText *)Self)->deleteLine(Self->txCursor.row());
         report_change(Self);
      }
      break;

   case KEY::DELETE:
      if ((Self->txFlags & VTXF::AREA_SELECTED) IS VTXF::NIL) {
         Self->txCursor.validate_position(Self);
         const auto row = Self->txCursor.row(), column = Self->txCursor.column();
         if (column < Self->txLines[row].utf8Length()) Self->txCursor.extend(Self, row, column + 1);
         else if (row + 1 < std::ssize(Self->txLines)) Self->txCursor.extend(Self, row + 1, 0);
      }
      if (replace_selection(Self, "")) report_change(Self);
      break;

   case KEY::TAB:
      break;

   case KEY::ENTER:
   case KEY::NP_ENTER:
      if (replace_selection(Self, "\n")) report_change(Self);
      break;

   case KEY::INSERT:
      if ((Self->txFlags & VTXF::OVERWRITE) != VTXF::NIL) Self->txFlags &= ~VTXF::OVERWRITE;
      else Self->txFlags |= VTXF::OVERWRITE;
      break;

   default:
      break;
   }
}

//********************************************************************************************************************

struct SelectionRange {
   int row, column, end_row, end_column;
};

static std::optional<SelectionRange> selection_range(extVectorText *Self)
{
   if (((Self->txFlags & VTXF::AREA_SELECTED) IS VTXF::NIL) or Self->txLines.empty()) return std::nullopt;
   SelectionRange range;
   Self->txCursor.selectedArea(Self, &range.row, &range.column, &range.end_row, &range.end_column);
   return range;
}

static std::string selected_text(extVectorText *Self)
{
   if ((Self->txFlags & VTXF::SECRET) != VTXF::NIL) return {};
   const auto range = selection_range(Self);
   if (not range) return {};

   std::string result;
   for (int row=range->row; row <= range->end_row; row++) {
      const auto &line = Self->txLines[row];
      const auto start = row IS range->row ? line.utf8CharOffset(range->column) : 0;
      const auto end = row IS range->end_row ? line.utf8CharOffset(range->end_column) : int(line.size());
      if (row != range->row) result += '\n';
      result.append(line, start, end - start);
   }
   return result;
}

// Builds closed quads in text coordinates.  Ordinary glyphs with matching caret heights form a single run;
// transitions and guide paths retain individual quads so their transformed caret edges are preserved.
static void rebuild_selection(extVectorText *Self)
{
   auto selection = Self->txSelection;
   if (not selection) return;
   auto &commands = selection->Commands;
   commands.clear();
   const auto range = selection_range(Self);

   auto quad = [&](const CharPos &Start, const CharPos &End) {
      commands.push_back(PathCommand { .Type=PE::Move, .X=Start.x1, .Y=Start.y1 });
      commands.push_back(PathCommand { .Type=PE::Line, .X=End.x1, .Y=End.y1 });
      commands.push_back(PathCommand { .Type=PE::Line, .X=End.x2, .Y=End.y2 });
      commands.push_back(PathCommand { .Type=PE::Line, .X=Start.x2, .Y=Start.y2 });
      commands.push_back(PathCommand { .Type=PE::ClosePath });
   };
   auto same_height = [](const CharPos &Left, const CharPos &Right) {
      return (std::abs(Left.x1 - Left.x2) < 0.001) and (std::abs(Right.x1 - Right.x2) < 0.001) and
         (std::abs(Left.y1 - Right.y1) < 0.001) and (std::abs(Left.y2 - Right.y2) < 0.001);
   };

   if (range) {
      const bool plain = not Self->Transition and not Self->GuidePath;
      for (int row=range->row; row <= range->end_row; row++) {
         const auto &line = Self->txLines[row];
         if (line.chars.empty()) continue;
         const auto last = int(line.chars.size()) - 1;
         const auto start = std::min(row IS range->row ? range->column : 0, last);
         const auto end = std::min(row IS range->end_row ? range->end_column : line.utf8Length(), last);
         auto run_start = line.chars[start];
         auto run_end = run_start;
         bool have_run = false;
         int byte_offset = 0, byte_column = 0;
         for (int col=start; col < end; col++) {
            const auto &left = line.chars[col];
            auto right = line.chars[col + 1];
            if (plain and not same_height(left, right)) {
               // A wrapped word starts on another visual line.  Finish this glyph on its own line instead
               // of joining its highlight diagonally to the next line's first caret.
               int char_length;
               // Advance from the previous wrap boundary, never rescan the line prefix.
               byte_offset += UTF8CharOffset(line.c_str() + byte_offset, col - byte_column);
               byte_column = col;
               const auto glyph = std::string_view(line).substr(byte_offset);
               const auto unicode = UTF8ReadValue(glyph, &char_length);
               const auto width = Self->txBitmapFont ? fnt::CharWidth(Self->txBitmapFont, unicode) :
                  string_width(Self, glyph.substr(0, char_length));
               right = left;
               right.x1 += width;
               right.x2 += width;
            }
            if (have_run and plain and same_height(run_end, left) and same_height(left, right)) {
               run_end = right;
            }
            else {
               if (have_run) quad(run_start, run_end);
               run_start = left;
               run_end = right;
               have_run = true;
            }
         }
         if (have_run) quad(run_start, run_end);
         if (row < range->end_row) {
            auto left = line.chars[end];
            auto right = left;
            const auto gap = std::max(1.0, Self->txFontSize * 0.33);
            const auto height = std::hypot(left.x2 - left.x1, left.y2 - left.y1);
            // Extend along the caret's perpendicular, preserving its orientation on a guide path.
            const auto dx = height > 0 ? (left.y2 - left.y1) / height * gap : gap;
            const auto dy = height > 0 ? -(left.x2 - left.x1) / height * gap : 0.0;
            right.x1 += dx;
            right.x2 += dx;
            right.y1 += dy;
            right.y2 += dy;
            quad(left, right);
         }
      }
   }
   selection->CommandsChanged = true;
   reset_path(selection);
   mark_dirty(selection, RC::TRANSFORM);
   selection->setVisibility(range and not Self->txSelectionHidden ? VIS::VISIBLE : VIS::HIDDEN);
}

// The generic guide-path morph runs after generate_text().  Keep editing geometry in the same coordinates
// as the morphed glyphs before rebuilding the child overlay.
void morph_text_carets(extVectorText *Self, const agg::trans_single_path &Transform, double YOffset)
{
   for (auto &line : Self->txLines) {
      for (auto &caret : line.chars) {
         caret.y1 += YOffset;
         caret.y2 += YOffset;
         Transform.transform(&caret.x1, &caret.y1);
         Transform.transform(&caret.x2, &caret.y2);
      }
   }
   Self->txCursor.reset_vector(Self);
   rebuild_selection(Self);
}

static void selection_changed(extVectorText *Self)
{
   // Path regeneration also rebuilds the highlight, using the new glyph positions.  Text without a highlight
   // stays lazy, so that properties set after the string (e.g. GuidePath) still apply to the first generation.
   if (Self->txSelection and Self->initialised() and Self->dirty()) gen_vector_tree(Self);
   rebuild_selection(Self);
   // Selection changes do not report text edits.  An unattached scene has no surface to redraw.
   if (Self->Scene and Self->Scene->SurfaceID) acDraw(Self);
   report_cursor_move(Self);
}

static void report_cursor_move(extVectorText *Self)
{
   if (Self->txCursorUpdateDepth or Self->txReportingCursor) return;
   const bool selected = (Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL;
   const bool changed = Self->txCursorChanged or (Self->txReportedRow != Self->txCursor.row()) or
      (Self->txReportedColumn != Self->txCursor.column()) or (Self->txReportedSelection != selected) or
      (selected and ((Self->txReportedSelectRow != Self->txCursor.selectRow) or
       (Self->txReportedSelectColumn != Self->txCursor.selectColumn)));
   auto remember_position = [&]() {
      Self->txReportedRow = Self->txCursor.row();
      Self->txReportedColumn = Self->txCursor.column();
      Self->txReportedSelectRow = Self->txCursor.selectRow;
      Self->txReportedSelectColumn = Self->txCursor.selectColumn;
      Self->txReportedSelection = (Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL;
      Self->txCursorChanged = false;
   };
   remember_position();
   if (not changed or not Self->initialised() or not Self->needs_caret_geometry()) return;

   gen_vector_tree(Self);
   Self->txReportingCursor = true;
   if (Self->txOnCursorMove.stale()) release_callback(Self->txOnCursorMove);
   else if (Self->txOnCursorMove.isC()) {
      auto routine = (void (*)(extVectorText *))Self->txOnCursorMove.Routine;
      kt::SwitchContext context(Self->txOnCursorMove.Context);
      routine(Self);
   }
   else if (Self->txOnCursorMove.isScript()) {
      sc::Call(Self->txOnCursorMove, std::to_array<ScriptArg>({
         { "VectorText", Self, FD_OBJECTPTR }
      }));
   }
   Self->txReportingCursor = false;
   remember_position();
}

static void clear_selection(extVectorText *Self)
{
   if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) {
      Self->txFlags &= ~VTXF::AREA_SELECTED;
      selection_changed(Self);
   }
}

static bool replace_selection(extVectorText *Self, std::string_view Value)
{
   auto range = selection_range(Self);
   if (not range and Value.empty()) return false;
   if (Self->txLines.empty()) Self->txLines.resize(1);
   if (not range) {
      Self->txCursor.validate_position(Self);
      range = SelectionRange { Self->txCursor.row(), Self->txCursor.column(),
         Self->txCursor.row(), Self->txCursor.column() };
   }

   // Most keystrokes only change one line.  Preserve its buffers and leave subsequent rows in place.
   if ((range->row IS range->end_row) and (Value.find('\n') IS std::string_view::npos)) {
      auto &line = Self->txLines[range->row];
      const auto start = line.utf8CharOffset(range->column);
      const auto end = line.utf8CharOffset(range->end_column);
      int inserted_columns = 0;
      for (auto remaining = Value; not remaining.empty();) {
         int length;
         UTF8ReadValue(remaining, &length);
         remaining.remove_prefix(std::max(1, length));
         inserted_columns++;
      }
      const auto total_columns = line.utf8Length() - (range->end_column - range->column) + inserted_columns;
      // Limit truncation uses the general path, which compares the final result before reporting a change.
      if (Value.empty() or (total_columns <= Self->txCharLimit)) {
         const bool changed = std::string_view(line).substr(start, end - start) != Value;
         if (changed) {
            line.replace(start, end - start, Value);
            line.invalidate_text();
            mark_dirty(Self, RC::BASE_PATH);
         }
         Self->txCursor.move(Self, range->row, range->column + inserted_columns, true);
         return changed;
      }
   }

   std::vector<TextLine> replacement;
   auto prefix = Self->txLines[range->row].substr(0, Self->txLines[range->row].utf8CharOffset(range->column));
   replacement.emplace_back(prefix);
   for (size_t start=0; start < Value.size();) {
      const auto end = Value.find('\n', start);
      if (end IS std::string_view::npos) {
         replacement.back().append(Value.substr(start));
         break;
      }
      replacement.back().append(Value.substr(start, end - start));
      replacement.emplace_back();
      start = end + 1;
   }

   const auto total_rows = Self->txLines.size() - (range->end_row - range->row + 1) + replacement.size();
   if ((Self->txLineLimit > 0) and (replacement.size() > 1) and
       (total_rows > size_t(Self->txLineLimit))) return false;

   const auto cursor_row = range->row + int(replacement.size()) - 1;
   auto cursor_column = replacement.back().utf8Length();
   replacement.back().append(Self->txLines[range->end_row],
      Self->txLines[range->end_row].utf8CharOffset(range->end_column), std::string::npos);
   for (auto &line : replacement) {
      if (not Value.empty() and line.utf8Length() > Self->txCharLimit) {
         line.resize(line.utf8CharOffset(Self->txCharLimit));
      }
   }
   cursor_column = std::min(cursor_column, replacement.back().utf8Length());

   bool changed = replacement.size() != size_t(range->end_row - range->row + 1);
   for (size_t i=0; not changed and i < replacement.size(); i++) {
      if (replacement[i].compare(Self->txLines[range->row + i]) != 0) changed = true;
   }
   if (not changed) {
      Self->txCursor.move(Self, cursor_row, cursor_column, true);
      return false;
   }

   // Reuse overlapping rows; shift the remaining rows only when the line count changes.
   const auto old_rows = range->end_row - range->row + 1;
   const auto common_rows = std::min(old_rows, int(replacement.size()));
   for (int i=0; i < common_rows; i++) {
      auto &line = Self->txLines[range->row + i];
      line.assign(replacement[i]);
      line.invalidate_text();
   }
   if (int(replacement.size()) < old_rows) {
      Self->txLines.erase(Self->txLines.begin() + range->row + common_rows,
         Self->txLines.begin() + range->end_row + 1);
   }
   else if (int(replacement.size()) > old_rows) {
      Self->txLines.insert(Self->txLines.begin() + range->end_row + 1,
         std::make_move_iterator(replacement.begin() + common_rows), std::make_move_iterator(replacement.end()));
   }
   mark_dirty(Self, RC::BASE_PATH);
   Self->txCursor.move(Self, cursor_row, cursor_column, true);
   return true;
}

//********************************************************************************************************************
// Note: This function validates boundaries except for the column going beyond the string length.

void TextCursor::position(extVectorText *Vector, int Row, int Column, bool ValidateWidth)
{
   if (Vector->txLines.empty()) Vector->txLines.resize(1);
   Row = std::clamp(Row, 0, int(Vector->txLines.size()) - 1);
   Column = std::max(0, Column);
   if (ValidateWidth) {
      const auto max_col = Vector->txLines[Row].utf8Length();
      Column = std::min(Column, max_col);
   }
   mRow = Row;
   mColumn = Column;
   preferredColumn = -1;
   reset_vector(Vector);
}

void TextCursor::move(extVectorText *Vector, int Row, int Column, bool ValidateWidth)
{
   Vector->txFlags &= ~VTXF::AREA_SELECTED;
   position(Vector, Row, Column, ValidateWidth);
   selectRow = mRow;
   selectColumn = mColumn;
   selection_changed(Vector);
}

void TextCursor::extend(extVectorText *Vector, int Row, int Column)
{
   if ((Vector->txFlags & VTXF::AREA_SELECTED) IS VTXF::NIL) {
      selectRow = mRow;
      selectColumn = mColumn;
   }
   position(Vector, Row, Column, true);
   if ((selectRow != mRow) or (selectColumn != mColumn)) Vector->txFlags |= VTXF::AREA_SELECTED;
   else Vector->txFlags &= ~VTXF::AREA_SELECTED;
   selection_changed(Vector);
}

//********************************************************************************************************************

void TextCursor::reset_vector(extVectorText *Vector) const
{
   if (Vector->needs_caret_geometry() and not Vector->txLines.empty()) {
      auto &line = Vector->txLines[mRow];

      if (not line.chars.empty()) {
         auto col = mColumn;
         if ((size_t)col >= line.chars.size()) col = line.chars.size() - 1;
         if (Vector->txCursor.vector) {
            Vector->txCursor.vector->Points[0].X = line.chars[col].x1 + 0.5;
            Vector->txCursor.vector->Points[0].Y = line.chars[col].y1;
            Vector->txCursor.vector->Points[1].X = line.chars[col].x2 + 0.5;
            Vector->txCursor.vector->Points[1].Y = line.chars[col].y2;
            reset_path(Vector->txCursor.vector);
            mark_dirty(Vector->txCursor.vector, RC::TRANSFORM);
         }

         Unit x = Vector->txX, y = Vector->txY;

         if (x.scaled()) x = Unit(x * get_parent_width(Vector));
         if (y.scaled()) y = Unit(y * get_parent_height(Vector));

         // If the cursor X,Y lies outside of the parent viewport, offset the text so that it remains visible to
         // the user.

         if ((not Vector->GuidePath) and Vector->ParentView and
             ((Vector->txFlags & VTXF::NO_AUTO_OFFSET) IS VTXF::NIL)) {
            auto p_width = Vector->ParentView->vpFixedWidth;
            double xo = 0;
            const double CURSOR_MARGIN = Vector->txFontSize * 0.5;
            if (p_width > 8) {
               if (x + line.chars[col].x1 <= 0) xo = x + line.chars[col].x1;
               else if (x + line.chars[col].x1 + CURSOR_MARGIN > p_width) xo = -(x + line.chars[col].x1 + CURSOR_MARGIN - p_width);
            }

            auto p_height = Vector->ParentView->vpFixedHeight;
            double yo = 0;
            if ((line.chars[col].y1 > 0) and (p_height > Vector->txFontSize)) {
               if (y + line.chars[col].y1 <= 0) yo = -(y + line.chars[col].y1);
               else if (y + line.chars[col].y2 > p_height) {
                  yo = -(y + line.chars[col].y2 - p_height + CURSOR_MARGIN);
               }
            }

            if ((xo != Vector->txXOffset) or (yo != Vector->txYOffset)) {
               Vector->txXOffset = xo;
               Vector->txYOffset = yo;

               reset_path(Vector);
            }
         }
      }
   }
}

//********************************************************************************************************************
// Move the cursor if it's outside the line boundary.

void TextCursor::validate_position(extVectorText *Self) const
{
   auto row = mRow;
   auto col = mColumn;

   if (Self->txLines.empty()) Self->txLines.resize(1);
   if ((size_t)row >= Self->txLines.size()) row = Self->txLines.size() - 1;

   if (Self->txLines[row].empty()) {
      if (col != 0) col = 0;
   }
   else {
      auto max_col = Self->txLines[row].utf8Length();
      if (col > max_col) col = max_col;
   }

   if ((row != mRow) or (col != mColumn)) Self->txCursor.move(Self, row, col);
}

//********************************************************************************************************************

static void insert_char(extVectorText *Self, int Unicode)
{
   if ((not Self) or (not Unicode)) return;
   Self->txCursor.validate_position(Self);
   const auto column = Self->txCursor.column();

   char buffer[6];
   const auto length = UTF8WriteValue(Unicode, std::span<int8_t>((int8_t *)buffer, 6));
   if (length <= 0) return;

   if (((Self->txFlags & (VTXF::OVERWRITE|VTXF::AREA_SELECTED)) IS VTXF::OVERWRITE) and
       (column < Self->txLines[Self->txCursor.row()].utf8Length())) {
      Self->txCursor.extend(Self, Self->txCursor.row(), column + 1);
   }
   if (replace_selection(Self, std::string_view(buffer, length))) report_change(Self);
}

//********************************************************************************************************************

#include "text_path.cpp"
#include "text_bitmap.cpp"
#include "text_def.cpp"

static const FieldDef clTextAlign[] = {
   { "Left",       ALIGN::LEFT },
   { "Horizontal", ALIGN::HORIZONTAL },
   { "Right",      ALIGN::RIGHT },
   // SVG synonyms
   { "Start",      ALIGN::LEFT },
   { "Middle",     ALIGN::HORIZONTAL },
   { "End",        ALIGN::RIGHT },
   { nullptr, 0 }
};

static const FieldArray clTextFields[] = {
   { "X",             FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, TEXT_GET_X, TEXT_SET_X },
   { "Y",             FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, TEXT_GET_Y, TEXT_SET_Y },
   { "Weight",        FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, TEXT_GET_Weight, TEXT_SET_Weight },
   { "String",        FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_RW|FDF_PURE, TEXT_GET_String, TEXT_SET_String },
   { "Align",         FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW|FDF_PURE, TEXT_GET_Align, TEXT_SET_Align, &clTextAlign },
   { "Fill",          FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW|FDF_PURE, TEXT_GET_Fill, TEXT_SET_Fill }, // Override
   { "Face",          FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW|FDF_PURE, TEXT_GET_Face, TEXT_SET_Face },
   { "FontSize",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW, TEXT_GET_FontSize, TEXT_SET_FontSize },
   { "FontStyle",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_RI|FDF_PURE, TEXT_GET_FontStyle, TEXT_SET_FontStyle },
   { "Descent",       FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_Descent },
   { "DisplayHeight", FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_DisplayHeight },
   { "DisplaySize",   FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_DisplaySize },
   { "DX",            FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_DX, TEXT_SET_DX },
   { "DY",            FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_DY, TEXT_SET_DY },
   { "InlineSize",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_InlineSize, TEXT_SET_InlineSize },
   { "LetterSpacing", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_LetterSpacing, TEXT_SET_LetterSpacing },
   { "Point",         FDF_VIRTUAL|FDF_INT|FDF_R|FDF_PURE, TEXT_GET_Point },
   { "LineSpacing",   FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_LineSpacing },
   { "Rotate",        FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_Rotate, TEXT_SET_Rotate },
   { "ShapeInside",   FDF_VIRTUAL|FDF_OBJECTID|FDF_RW|FDF_PURE, TEXT_GET_ShapeInside, TEXT_SET_ShapeInside, CLASSID::VECTOR },
   { "ShapeSubtract", FDF_VIRTUAL|FDF_OBJECTID|FDF_RW|FDF_PURE, TEXT_GET_ShapeSubtract, TEXT_SET_ShapeSubtract, CLASSID::VECTOR },
   { "TextLength",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_TextLength, TEXT_SET_TextLength },
   { "TextFlags",     FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW|FDF_PURE, TEXT_GET_Flags, TEXT_SET_Flags, &clVectorTextVTXF },
   { "TextWidth",     FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_TextWidth },
   { "StartOffset",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_StartOffset, TEXT_SET_StartOffset },
   { "Spacing",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, TEXT_GET_Spacing, TEXT_SET_Spacing },
   { "Font",          FDF_VIRTUAL|FDF_OBJECT|FDF_I, nullptr, TEXT_SET_Font },
   // Non-SVG fields related to real-time text editing
   { "OnChange",      FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE, TEXT_GET_OnChange, TEXT_SET_OnChange },
   { "OnCursorMove",  FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE, TEXT_GET_OnCursorMove, TEXT_SET_OnCursorMove },
   { "Focus",         FDF_VIRTUAL|FDF_OBJECTID|FDF_RI|FDF_PURE, TEXT_GET_Focus, TEXT_SET_Focus },
   { "CursorColumn",  FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_CursorColumn, TEXT_SET_CursorColumn },
   { "CursorRow",     FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_CursorRow, TEXT_SET_CursorRow },
   { "TotalLines",    FDF_VIRTUAL|FDF_INT|FDF_R|FDF_PURE, TEXT_GET_TotalLines },
   { "SelectRow",     FDF_VIRTUAL|FDF_INT|FDF_R|FDF_PURE, TEXT_GET_SelectRow },
   { "SelectColumn",  FDF_VIRTUAL|FDF_INT|FDF_R|FDF_PURE, TEXT_GET_SelectColumn },
   { "LineLimit",     FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, TEXT_GET_LineLimit, TEXT_SET_LineLimit },
   { "CharLimit",     FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, TEXT_GET_CharLimit, TEXT_SET_CharLimit },
   { "SelectedText",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R, TEXT_GET_SelectedText },
   { "SelectFill",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW, TEXT_GET_SelectFill, TEXT_SET_SelectFill },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_text(void)
{
   OBJECTID id;
   if (!FindObject("cfgSystemFonts", CLASSID::CONFIG, &id)) {
      glFontConfig = (objConfig *)GetObjectPtr(id);
   }

   clVectorText = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORTEXT),
      fl::Name("VectorText"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorTextActions),
      fl::Methods(clVectorTextMethods),
      fl::Fields(clTextFields),
      fl::Size(sizeof(extVectorText)),
      fl::Path(MOD_PATH));

   return clVectorText ? ERR::Okay : ERR::AddClass;
}
