/*********************************************************************************************************************

-CLASS-
VectorText: Extends the Vector class with support for generating text.

To create text along a path, set the @Vector.Morph field with a reference to any @Vector object that generates a path.  The
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

+ Compute the (x,y) of the character's middle vertex in context of the morph path.
+ Compute the angle from the middle vertex to the first vertex (start_x, start_y)
+ Compute the angle from the middle vertex to the last vertex (end_x, end_y)
+ Interpolate the two angles
+ Rotate the character around its mid point.
+ The final character position is moved to the mid-point rather than start_x,start_y
*/

#include "agg_path_length.h"

const int DEFAULT_WEIGHT = 400;

static FIELD FID_FreetypeFace;
objConfig *glFontConfig = nullptr;

//********************************************************************************************************************

class TextCursor {
private:
   int  mColumn, mRow; // The column is the character position after taking UTF8 sequences into account.

public:
   APTR  timer;
   extVectorPoly *vector;
   int  flash;
   int  savePos;
   int  endColumn, endRow; // For area selections
   int  selectColumn, selectRow;

   TextCursor() :
      mColumn(0), mRow(0),
      timer(nullptr), vector(nullptr), flash(0), savePos(0),
      endColumn(0), endRow(0),
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
   void reset_vector(extVectorText *) const;
   void validate_position(extVectorText *) const;
};

//********************************************************************************************************************

class CharPos {
public:
   double x1, y1, x2, y2;
   CharPos(double X1, double Y1, double X2, double Y2) : x1(X1), y1(Y1), x2(X2), y2(Y2) { }
};

//********************************************************************************************************************

class TextLine : public std::string {
public:
   TextLine() : std::string { } { }
   TextLine(const char *Value) : std::string{ Value } { }
   TextLine(const char *Value, size_t Total) : std::string{ Value, Total } { }
   TextLine(std::string Value) : std::string{ Value } { }

   std::vector<CharPos> chars;

   inline int charLength(uint32_t Offset = 0) const { // Total number of bytes used by the char at Offset
      return UTF8CharLength(c_str() + Offset);
   }

   inline int utf8CharOffset(uint32_t Char) const { // Convert a character index to its byte offset
      return UTF8CharOffset(c_str(), Char);
   }

   inline int utf8Length() const { // Total number of unicode characters in the string
      return UTF8Length(c_str());
   }

   inline int lastChar() const { // Return a direct offset to the start of the last character.
      return length() - UTF8PrevLength(c_str(), length());
   }

   inline int prevChar(uint32_t Offset) const { // Return the direct offset to a previous character.
      return Offset - UTF8PrevLength(c_str(), Offset);
   }
};

//********************************************************************************************************************

class extVectorText : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORTEXT;
   static constexpr CSTRING CLASS_NAME = "VectorText";
   using create = pf::Create<extVectorText>;

   std::vector<TextLine> txLines;
   FUNCTION txValidateInput;
   FUNCTION txOnChange;
   double txInlineSize; // Enables word-wrapping
   double txX, txY;
   double txTextLength;
   double txFontSize;  // Font size measured in pixels @ 72 DPI.  Should always be a whole number.
   double txLetterSpacing; // SVG: Acts as a multiplier or fixed unit addition to the spacing of each glyph
   double txWidth; // Width of the text computed by path generation.  Not for client use as GetBoundary() can be used for that.
   double txStartOffset; // TODO
   double txSpacing; // TODO
   double txXOffset, txYOffset; // X,Y adjustment for ensuring that the cursor is visible.
   double *txDX, *txDY; // A series of spacing adjustments that apply on a per-character level.
   double *txRotate;  // A series of angles that will rotate each individual character.
   objFont *txBitmapFont;
   objBitmap *txAlphaBitmap; // Host for the bitmap font texture
   extVectorImage *txBitmapImage;
   common_font *txHandle;
   TextCursor txCursor;
   CSTRING txFamily; // Family name(s) as requested by the client
   APTR    txKeyEvent;
   OBJECTID txFocusID;
   OBJECTID txShapeInsideID;   // Enable word-wrapping within this shape
   OBJECTID txShapeSubtractID; // Subtract this shape from the path defined by shape-inside
   int  txTotalLines;
   int  txLineLimit, txCharLimit;
   int  txTotalRotate, txTotalDX, txTotalDY;
   int  txWeight; // 100 - 300 (Light), 400 (Normal), 700 (Bold), 900 (Boldest)
   ALIGN txAlignFlags;
   VTXF  txFlags;
   char  txFontStyle[30];
   bool txScaledFontSize;
   bool txXScaled:1;
   bool txYScaled:1;
// bool txSpacingAndGlyphs:1;
};

//********************************************************************************************************************

static void add_line(extVectorText *, std::string, int Offset, int Length, int Line = -1);
static ERR cursor_timer(extVectorText *, int64_t, int64_t);
static void delete_selection(extVectorText *);
static void insert_char(extVectorText *, int, int);
static void generate_text(extVectorText *, agg::path_storage &Path);
static void raster_text_to_bitmap(extVectorText *);
static void key_event(evKey *, int, extVectorText *);
static ERR reset_font(extVectorText *, bool = false);
static ERR text_input_events(extVector *, const InputEvent *);
static ERR text_focus_event(extVector *, FM, OBJECTPTR, APTR);

//********************************************************************************************************************

freetype_font::~freetype_font() {
   if (face) { FT_Done_Face(face); face = nullptr; }
}

//********************************************************************************************************************

inline void get_kerning_xy(FT_Face Face, int Glyph, int PrevGlyph, double &X, double &Y)
{
   FT_Vector delta;
   if (!FT_Get_Kerning(Face, PrevGlyph, Glyph, FT_KERNING_DEFAULT, &delta)) {
      X = int26p6_to_dbl(delta.x);
      Y = int26p6_to_dbl(delta.y);
   }
   else {
      X = 0;
      Y = 0;
   }
}

inline double get_kerning(FT_Face Face, int Glyph, int PrevGlyph)
{
   if ((not Glyph) or (not PrevGlyph)) return 0;

   FT_Vector delta;
   FT_Get_Kerning(Face, PrevGlyph, Glyph, FT_KERNING_DEFAULT, &delta);
   return int26p6_to_dbl(delta.x);
}

inline void report_change(extVectorText *Self)
{
   if (Self->txOnChange.isC()) {
      auto routine = (void (*)(extVectorText *))Self->txOnChange.Routine;
      pf::SwitchContext context(Self->txOnChange.Context);
      routine(Self);
   }
   else if (Self->txOnChange.isScript()) {
      sc::Call(Self->txOnChange, std::to_array<ScriptArg>({
         { "VectorText", Self, FD_OBJECTPTR }
      }));
   }
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
-END-

*********************************************************************************************************************/

static ERR VECTORTEXT_DeleteLine(extVectorText *Self, struct vt::DeleteLine *Args)
{
   if (Self->txLines.empty()) return ERR::Okay;

   if ((not Args) or (Args->Line < 0)) Self->txLines.pop_back();
   else if ((size_t)Args->Line < Self->txLines.size()) Self->txLines.erase(Self->txLines.begin() + Args->Line);
   else return ERR::Args;

   mark_dirty(Self, RC::BASE_PATH);

   if ((Args) and (Self->txCursor.row() IS Args->Line)) {
      Self->txCursor.move(Self, Self->txCursor.row(), 0);
   }
   else if ((size_t)Self->txCursor.row() >= Self->txLines.size()) {
      Self->txCursor.move(Self, Self->txLines.size()-1, Self->txCursor.column());
   }

   Self->txFlags &= ~VTXF::AREA_SELECTED;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORTEXT_Free(extVectorText *Self)
{
   Self->txLines.~vector<TextLine>();
   Self->txCursor.~TextCursor();

   if (Self->txHandle) {
      // TODO: This would be a good opportunity to garbage-collect stale glyphs
   }

   if ((((extVector *)Self)->ParentView) and (((extVector *)Self)->ParentView->Scene->SurfaceID)) {
      ((extVector *)Self)->ParentView->subscribeInput(JTYPE::NIL, C_FUNCTION(text_input_events));
   }

   if (Self->txBitmapImage)  { FreeResource(Self->txBitmapImage); Self->txBitmapImage = nullptr; }
   if (Self->txAlphaBitmap)  { FreeResource(Self->txAlphaBitmap); Self->txAlphaBitmap = nullptr; }
   if (Self->txFamily)       { FreeResource(Self->txFamily); Self->txFamily = nullptr; }
   if (Self->txDX)           { FreeResource(Self->txDX); Self->txDX = nullptr; }
   if (Self->txDY)           { FreeResource(Self->txDY); Self->txDY = nullptr; }
   if (Self->txKeyEvent)     { UnsubscribeEvent(Self->txKeyEvent); Self->txKeyEvent = nullptr; }

   if (Self->txFocusID) {
      if (pf::ScopedObjectLock<extVector> focus(Self->txFocusID, 5000); focus.granted()) {
         focus->subscribeFeedback(FM::NIL, C_FUNCTION(text_focus_event));
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORTEXT_Init(extVectorText *Self)
{
   if ((Self->txFlags & VTXF::EDITABLE) != VTXF::NIL) {
      if (not Self->txFocusID) {
         if (Self->ParentView) Self->txFocusID = Self->ParentView->UID;
      }

      if (pf::ScopedObjectLock<extVector> focus(Self->txFocusID, 5000); focus.granted()) {
         focus->subscribeFeedback(FM::HAS_FOCUS|FM::CHILD_HAS_FOCUS|FM::LOST_FOCUS, C_FUNCTION(text_focus_event));
      }

      // The editing cursor will inherit transforms from the VectorText as long as it is a direct child.

      if ((Self->txCursor.vector = extVectorPoly::create::global(
            fl::Name("VTCursor"),
            fl::X1(0), fl::Y1(0), fl::X2(1), fl::Y2(1),
            fl::Closed(false),
            fl::Stroke("rgb(255,0,0,255)"),
            fl::StrokeWidth(1.25),
            fl::Visibility(VIS::HIDDEN)))) {
      }
      else return ERR::CreateObject;

      if (Self->txLines.empty()) Self->txLines.emplace_back(std::string(""));

      if ((Self->ParentView) and (Self->ParentView->Scene->SurfaceID)) {
         Self->ParentView->subscribeInput(JTYPE::BUTTON, C_FUNCTION(text_input_events));
      }
   }

   return reset_font(Self, true);
}

//********************************************************************************************************************

static ERR VECTORTEXT_NewObject(extVectorText *Self)
{
   new (&Self->txLines) std::vector<TextLine>;
   new (&Self->txCursor) TextCursor;

   strcopy("Regular", Self->txFontStyle, sizeof(Self->txFontStyle));
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_text;
   Self->StrokeWidth  = 0.0;
   Self->txWeight     = DEFAULT_WEIGHT;
   Self->txFontSize   = 16; // Pixel units @ 72 DPI
   Self->txCharLimit  = 0x7fffffff;
   Self->txFamily     = strclone("Noto Sans");
   Self->Fill[0].Colour  = FRGB(1, 1, 1, 1);
   Self->txLetterSpacing = 1.0;
   Self->DisableHitTesting = true;
   return ERR::Okay;
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
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CharLimit: Limits the total characters allowed in the string.

Set the CharLimit field to limit the number of characters that can appear in the string.  The minimum possible value
is 0 for no characters.  If the object is in edit mode then the user will be unable to extend the string beyond the
limit.

Note that it is valid for the #String length to exceed the limit if set manually.  Only the display of the string
characters will be affected by the CharLimit value.

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
      Self->txCursor.move(Self, Self->txCursor.row(), Value);
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
      if (Value < Self->txTotalLines) Self->txCursor.move(Self, Value, Self->txCursor.column());
      else Self->txCursor.move(Self, Self->txTotalLines - 1, Self->txCursor.column());
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Descent: The font descent measured in pixels, after DPI conversion.

Use Descent to retrieve the height of the font descent region in actual display pixels, after DPI conversion has been
taken into account.

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
DisplayHeight: The font height measured in pixels, after DPI conversion.

Use DisplayHeight to retrieve the font height in actual display pixels, after DPI conversion has been taken into
account.  The height includes the top region reserved for accents, but excludes the descent value.

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
DisplaySize: The FontSize measured in pixels, after DPI conversion.

Use DisplaySize to retrieve the #FontSize in actual display pixels, after DPI conversion has been taken into account.
For example, if #FontSize is set to `16` in a 96 DPI environment, the resulting value is `12` after performing the
calculation `16 * 72 / 96`.

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

static ERR TEXT_GET_DX(extVectorText *Self, double **Values, int *Elements)
{
   *Values = Self->txDX;
   *Elements = Self->txTotalDX;
   return ERR::Okay;
}

static ERR TEXT_SET_DX(extVectorText *Self, double *Values, int Elements)
{
   if (Self->txDX) { FreeResource(Self->txDX); Self->txDX = nullptr; Self->txTotalDX = 0; }

   if ((Values) and (Elements > 0)) {
      if (AllocMemory(sizeof(double) * Elements, MEM::DATA, &Self->txDX) IS ERR::Okay) {
         copymem(Values, Self->txDX, Elements * sizeof(double));
         Self->txTotalDX = Elements;
         reset_path(Self);
         return ERR::Okay;
      }
      else return ERR::AllocMemory;
   }
   else return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
DY: Adjusts vertical spacing on a per-character basis.

This field follows the same rules described in #DX.

*********************************************************************************************************************/

static ERR TEXT_GET_DY(extVectorText *Self, double **Values, int *Elements)
{
   *Values   = Self->txDY;
   *Elements = Self->txTotalDY;
   return ERR::Okay;
}

static ERR TEXT_SET_DY(extVectorText *Self, double *Values, int Elements)
{
   if (Self->txDY) { FreeResource(Self->txDY); Self->txDY = nullptr; Self->txTotalDY = 0; }

   if ((Values) and (Elements > 0)) {
      if (AllocMemory(sizeof(double) * Elements, MEM::DATA, &Self->txDY) IS ERR::Okay) {
         copymem(Values, Self->txDY, Elements * sizeof(double));
         Self->txTotalDY = Elements;
         reset_path(Self);
         return ERR::Okay;
      }
      else return ERR::AllocMemory;
   }
   else return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OnChange: Receive notifications for changes to the text string.

Set this field with a function reference to receive notifications whenever the text string changes.

The callback function prototype is `void Function(*VectorText)`.

*********************************************************************************************************************/

static ERR TEXT_GET_OnChange(extVectorText *Self, FUNCTION **Value)
{
   if (Self->txOnChange.defined()) {
      *Value = &Self->txOnChange;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR TEXT_SET_OnChange(extVectorText *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->txOnChange.isScript()) UnsubscribeAction(Self->txOnChange.Context, AC::Free);
      Self->txOnChange = *Value;
   }
   else Self->txOnChange.clear();
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

static ERR TEXT_GET_Face(extVectorText *Self, CSTRING *Value)
{
   *Value = Self->txFamily;
   return ERR::Okay;
}

static ERR TEXT_SET_Face(extVectorText *Self, CSTRING Value)
{
   if (Value) {
      if (Self->txFamily) { FreeResource(Self->txFamily); Self->txFamily = nullptr; }

      CSTRING name;
      if (fnt::ResolveFamilyName(Value, &name) IS ERR::Okay) {
         Self->txFamily = strclone(name);
      }
      else Self->txFamily = strclone("Noto Sans"); // Better to resort to a default than fail completely

      if (Self->initialised()) return reset_font(Self);

      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Focus: Refers to the object that will be monitored for user focussing.

A VectorText object in edit mode will become active when its nearest viewport receives the focus.  Setting the Focus
field to a different vector in the scene graph will redirect monitoring to it.

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

      if (Self->txFamily) { FreeResource(Self->txFamily); Self->txFamily = nullptr; }

      Self->txFamily = strclone(other->Face);
      Self->txFontSize = std::trunc(other->Point * (96.0 / 72.0));
      Self->txScaledFontSize = false;
      strcopy(other->Style, Self->txFontStyle);

      if (Self->initialised()) return reset_font(Self);
      else return ERR::Okay;
   }
   else if (Value->classID() IS CLASSID::VECTORTEXT) {
      auto other = (extVectorText *)Value;

      if (Self->txFamily) { FreeResource(Self->txFamily); Self->txFamily = nullptr; }

      Self->txFamily = strclone(other->txFamily);
      Self->txFontSize = other->txFontSize;
      Self->txScaledFontSize = false;
      strcopy(other->txFontStyle, Self->txFontStyle);

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

It is possible to enable dual-fill painting via this field, whereby a second fill operation can follow the first by
separating them with a semi-colon `;` character.  This feature makes it easy to use a common background fill and
follow it with an independent foreground, alleviating the need for additional vector objects.  Be aware that this
feature is intended for programmed use-cases and is not SVG compliant.

*********************************************************************************************************************/
// Override the existing Vector Fill field - this is required for bitmap fonts as they need a path reset to be
// triggered when decorative changes occur.

static ERR TEXT_GET_Fill(extVectorText *Self, CSTRING *Value)
{
   *Value = Self->FillString;
   return ERR::Okay;
}

static ERR TEXT_SET_Fill(extVectorText *Self, CSTRING Value)
{
   if (Self->FillString) { FreeResource(Self->FillString); Self->FillString = nullptr; }

   CSTRING next;
   if (auto error = vec::ReadPainter(Self->Scene, Value, &Self->Fill[0], &next); error IS ERR::Okay) {
      Self->FillString = strclone(Value);

      if (next) {
         vec::ReadPainter(Self->Scene, next, &Self->Fill[1], nullptr);
         Self->FGFill = true;
      }
      else Self->FGFill = false;

      // Bitmap font reset/redraw required
      if (Self->txBitmapFont) reset_path(Self);

      return ERR::Okay;
   }
   else return error;

}

/*********************************************************************************************************************
-FIELD-
FontSize: Defines the vertical size of the font.

The FontSize is equivalent to the SVG `font-size` attribute and refers to the height of the font from the dominant
baseline to the hanging baseline.  This would mean that a capitalised letter without accents should fill the entire
vertical space defined by FontSize.

The default unit value of FontSize is in pixels at a resolution of 72 DPI.  This means that if the display
is configured to a more common 96 DPI for instance, the actual pixel size on the display will be `FontSize * 72 / 96`.
Point sizes are also measured at a constant ratio of `1/72` irrespective of display settings, and this may need to
factor into precise size calculations.

Standard unit measurements such as `px`, `em` and `pt` are supported by appending them after the numeric value.
1em is equivalent to the 'default font size', which is typically 16px unless modified.

When retrieving the font size, the resulting string must be freed by the client when no longer in use.

*********************************************************************************************************************/

static ERR TEXT_GET_FontSize(extVectorText *Self, CSTRING *Value)
{
   *Value = strclone(std::to_string(Self->txFontSize));
   return ERR::Okay;
}

static ERR TEXT_SET_FontSize(extVectorText *Self, CSTRING Value)
{
   bool pct;
   auto size = read_unit(Value, pct);

   // TODO: With respect to supporting sub-pixel point sizes and being cache-friendly, we could try caching fonts
   // at pre-determined point sizes (4,6,8,10,12,14,20,30,40,50,60,...) and then use scaling to cater to other
   // 'non-standard' sizes.  This would allow for hinting to remain effective when regular point sizes are being chosen.

   if (size > 0) {
      auto new_size = std::trunc(size);
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
designer chooses such as `Thin`, `Narrow` or `Wide`, use ~Font.GetList() for a definitive list of available
style names.

Errors are not returned if the style name is invalid or unavailable.

*********************************************************************************************************************/

static ERR TEXT_GET_FontStyle(extVectorText *Self, CSTRING *Value)
{
   *Value = Self->txFontStyle;
   return ERR::Okay;
}

static ERR TEXT_SET_FontStyle(extVectorText *Self, CSTRING Value)
{
   if ((not Value) or (not Value[0])) strcopy("Regular", Self->txFontStyle, sizeof(Self->txFontStyle));
   else strcopy(Value, Self->txFontStyle, sizeof(Self->txFontStyle));
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

This field can be queried for the amount of space between each line, measured in display pixels.

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

Reading the Point value will return the point-size of the font, calculated as `FontSize * 72 / DisplayDPI`.

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

To check whether or not an area has been selected, test the `AREA_SELECTED` bit in the #Flags field.

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

To check whether or not an area has been selected, test the `AREA_SELECTED` bit in the #Flags field.

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
TextFlags: Private.  Optional flags.

-END-
*********************************************************************************************************************/

static ERR TEXT_GET_Flags(extVectorText *Self, VTXF *Value)
{
   *Value = Self->txFlags;
   return ERR::Okay;
}

static ERR TEXT_SET_Flags(extVectorText *Self, VTXF Value)
{
   Self->txFlags = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X: The x coordinate of the text.

The x-axis coordinate of the text is specified here as a fixed value.  Scaled coordinates are not supported.

*********************************************************************************************************************/

static ERR TEXT_GET_X(extVectorText *Self, Unit *Value)
{
   Value->set(Self->txX);
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

static ERR TEXT_GET_Y(extVectorText *Self, Unit *Value)
{
   Value->set(Self->txY);
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

static ERR TEXT_GET_Rotate(extVectorText *Self, double **Values, int *Elements)
{
   *Values = Self->txRotate;
   *Elements = Self->txTotalRotate;
   return ERR::Okay;
}

static ERR TEXT_SET_Rotate(extVectorText *Self, double *Values, int Elements)
{
   if (Self->txRotate) { FreeResource(Self->txRotate); Self->txRotate = nullptr; Self->txTotalRotate = 0; }

   if (AllocMemory(sizeof(double) * Elements, MEM::DATA, &Self->txRotate) IS ERR::Okay) {
      copymem(Values, Self->txRotate, Elements * sizeof(double));
      Self->txTotalRotate = Elements;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
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

The string for drawing the glyphs is defined here in UTF-8 format.

When retrieving a string that contains return codes, only the first line of text is returned.

*********************************************************************************************************************/

static ERR TEXT_GET_String(extVectorText *Self, CSTRING *Value)
{
   if (Self->txLines.size() > 0) {
      *Value = Self->txLines[0].c_str();
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR TEXT_SET_String(extVectorText *Self, CSTRING Value)
{
   Self->txLines.clear();

   if (Value) {
      while (*Value) {
         size_t total;
         for (total=0; (Value[total]) and (Value[total] != '\n'); total++);
         Self->txLines.emplace_back(std::string(Value, total));
         Value += total;
         if (*Value IS '\n') Value++;
      }
   }
   else Self->txLines.emplace_back("");

   reset_path(Self);
   if (Self->txCursor.vector) Self->txCursor.validate_position(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
TextLength: The expected length of the text after all computations have been taken into account.

The purpose of this attribute is to allow exact alignment of the text graphic in the computed result.  If the
#Width that is initially computed does not match this value, then the text will be scaled to match the
TextLength.

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
         auto w = fnt::StringWidth(Self->txBitmapFont, line.c_str(), -1);
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

static void calc_caret_position(TextLine &Line, agg::trans_affine &transform, double FontSize, double PathScale = 1.0)
{
   agg::path_storage cursor_path;
   double cx1, cy1, cx2, cy2;

   cursor_path.move_to(0, -(FontSize * PathScale) - (FontSize * 0.2));
   cursor_path.line_to(0, FontSize * 0.1);
   agg::conv_transform<agg::path_storage, agg::trans_affine> trans_cursor(cursor_path, transform);

   trans_cursor.vertex(&cx1, &cy1);
   trans_cursor.vertex(&cx2, &cy2);
   Line.chars.emplace_back(cx1, cy1, cx2, cy2);
}

static void calc_caret_position(TextLine &Line, double FontSize, double PathScale = 1.0)
{
   agg::path_storage cursor_path;
   double cx1, cy1, cx2, cy2;

   cursor_path.move_to(0, -(FontSize * PathScale) - (FontSize * 0.2));
   cursor_path.line_to(0, FontSize * 0.1);
   cursor_path.vertex(&cx1, &cy1);
   cursor_path.vertex(&cx2, &cy2);
   Line.chars.emplace_back(cx1, cy1, cx2, cy2);
}

//********************************************************************************************************************

extern void set_text_final_xy(extVectorText *Vector)
{
   double x = Vector->txX, y = Vector->txY;

   if (Vector->txXScaled) x *= get_parent_width(Vector);
   if (Vector->txYScaled) y *= get_parent_height(Vector);

   if ((Vector->txAlignFlags & ALIGN::RIGHT) != ALIGN::NIL) x -= Vector->txWidth;
   else if ((Vector->txAlignFlags & ALIGN::HORIZONTAL) != ALIGN::NIL) x -= Vector->txWidth * 0.5;

   if (Vector->txBitmapFont) {
      // Rastered fonts need an adjustment because the Y coordinate corresponds to the base-line.
      y -= Vector->txBitmapFont->Height + Vector->txBitmapFont->Leading;
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

   pf::Log log;
   if (auto error = get_font(log, Vector->txFamily, Vector->txFontStyle, Vector->txWeight, Vector->txFontSize, &Vector->txHandle); error IS ERR::Okay) {
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
      pf::Log log(__FUNCTION__);
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

static void add_line(extVectorText *Self, std::string String, int Offset, int Length, int Line)
{
   if (Length < 0) Length = String.length();

   // Stop the string from exceeding the acceptable character limit

   if (Length >= Self->txCharLimit) {
      int i = 0;
      for (int unicodelen=0, i=0; (i < Length) and (unicodelen < Self->txCharLimit); unicodelen++) {
         for (++i; (String[i] & 0xc0) IS 0x80; i++);
      }
      Length = i;
   }

   if (Line < 0) {
      Self->txLines.emplace_back(String.substr(Offset, Length));
   }
   else {
      TextLine tl(String.substr(Offset, Length));
      Self->txLines.insert(Self->txLines.begin() + Line, 1, tl);
   }

   acDraw(Self);
}

//********************************************************************************************************************

static ERR text_focus_event(extVector *Vector, FM Event, OBJECTPTR EventObject, APTR Meta)
{
   auto Self = (extVectorText *)CurrentContext();

   if ((Event & FM::HAS_FOCUS) != FM::NIL) {
      if (((Self->txFlags & VTXF::EDITABLE) != VTXF::NIL) and (Self->txCursor.vector)) {
         acMoveToFront(Self->txCursor.vector);

         if (Self->txCursor.timer) UpdateTimer(Self->txCursor.timer, 1.0);
         else {
            SubscribeTimer(0.8, C_FUNCTION(cursor_timer), &Self->txCursor.timer);

            if (not Self->txKeyEvent) {
               SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(key_event, Self), &Self->txKeyEvent);
            }
         }

         Self->txCursor.resetFlash();
         Self->txCursor.vector->setVisibility(VIS::VISIBLE);
         acDraw(Self);
      }
   }
   else if ((Event & (FM::LOST_FOCUS|FM::CHILD_HAS_FOCUS)) != FM::NIL) {
      if (Self->txCursor.vector) Self->txCursor.vector->setVisibility(VIS::HIDDEN);
      if (Self->txCursor.timer)  { UpdateTimer(Self->txCursor.timer, 0); Self->txCursor.timer = 0; }
      if (Self->txKeyEvent)      { UnsubscribeEvent(Self->txKeyEvent); Self->txKeyEvent = nullptr; }

      // When a simple input line loses the focus, all selections are deselected

      if (Self->txLineLimit IS 1) {
         if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) Self->txFlags &= ~VTXF::AREA_SELECTED;
      }

      acDraw(Self);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR text_input_events(extVector *Vector, const InputEvent *Events)
{
   auto Self = (extVectorText *)CurrentContext();

   pf::Log log(__FUNCTION__);

   for (; Events; Events = Events->Next) {
      if ((Events->Type IS JET::LMB) and ((Events->Flags & JTYPE::REPEATED) IS JTYPE::NIL) and (Events->Value IS 1)) {
         // Determine the nearest caret position to the clicked point.

         if (Self->txLines.empty()) {
            Self->txCursor.move(Self, 0, 0);
            Self->txCursor.resetFlash();
            continue;
         }

         agg::trans_affine transform;
         if (Self->txLines.size() > 1) {
            apply_parent_transforms(Self, transform);
         }

         double shortest_dist = 100000000000;
         int nearest_row = 0, nearest_col = 0;
         int row = 0;

         // This lambda finds the closest caret entry point relative to the click position.
         // TODO: If the transforms are limited to scaling and translation, we can optimise further
         // by dropping the dist() check and comparing against the X axis only.

         auto find_insertion = [&](TextLine &line) {
            int coli = 0;
            for (auto &col : line.chars) {
               double mx = Self->FinalX + ((col.x1 + col.x2) * 0.5); // Calculate the caret midpoint
               double my = Self->FinalY + ((col.y1 + col.y2) * 0.5);
               double d = std::abs(dist(Events->X, Events->Y, mx, my)); // Distance to the midpoint.

               if (d < shortest_dist) {
                  shortest_dist = d;
                  nearest_row = row;
                  nearest_col = coli;
               }
               coli++;
            }
         };

         if (Self->txLines.size() IS 1) {
            // For single rows we can skip the boundary check and calculate the caret position immediately.
            find_insertion(Self->txLines[0]);
         }
         else {
            for (row=0; row < std::ssize(Self->txLines); row++) {
               agg::path_storage path;

               if (Self->txBitmapFont) {
                  double offset = Self->txBitmapFont->LineSpacing * row;
                  path.move_to(0, -Self->txBitmapFont->LineSpacing + offset);
                  path.line_to(Self->txWidth, -Self->txBitmapFont->LineSpacing + offset);
                  path.line_to(Self->txWidth, offset);
                  path.line_to(0, offset);
                  path.close_polygon();
               }
               else if (Self->txHandle->type IS CF_FREETYPE) {
                  auto pt = (freetype_font::ft_point *)Self->txHandle;

                  double offset = pt->line_spacing * row;
                  path.move_to(0, -pt->line_spacing + offset);
                  path.line_to(Self->txWidth, -pt->line_spacing + offset);
                  path.line_to(Self->txWidth, offset);
                  path.line_to(0, offset);
                  path.close_polygon();
               }

               path.transform(transform);

               double bx1, bx2, by1, by2;
               bounding_rect_single(path, 0, &bx1, &by1, &bx2, &by2);

               if ((Events->AbsX >= bx1) and (Events->AbsY >= by1) and (Events->AbsX < bx2) and (Events->AbsX < by2)) {
                  find_insertion(Self->txLines[row]);
                  break;
               }
            }

            // Default to the first row if the click wasn't within a known boundary
            if (row >= std::ssize(Self->txLines)) find_insertion(Self->txLines[0]);
         }

         Self->txCursor.move(Self, nearest_row, nearest_col);
         Self->txCursor.resetFlash();
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void key_event(evKey *Event, int Size, extVectorText *Self)
{
   if ((Event->Qualifiers & KQ::PRESSED) IS KQ::NIL) return;

   pf::Log log(__FUNCTION__);

   log.trace("$%.8x, Value: %d", int(Event->Qualifiers), int(Event->Code));

   Self->txCursor.resetFlash(); // Reset the flashing cursor to make it visible
   Self->txCursor.vector->setVisibility(VIS::VISIBLE);

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

         case KEY::K: // Delete line
            if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) return;
            ((objVectorText *)Self)->deleteLine(Self->txCursor.row());
            return;

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

   if ((Event->Qualifiers & KQ::NOT_PRINTABLE) IS KQ::NIL) { // and (!(Flags & KQ::INSTRUCTIONKEYS))
      // Printable character handling

      if ((Self->txFlags & VTXF::EDITABLE) IS VTXF::NIL) {
         log.trace("Object does not have the EDIT flag set.");
         return;
      }

      if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) delete_selection(Self);

      insert_char(Self, Event->Unicode, Self->txCursor.column());
      return;
   }

   switch(Event->Code) {
   case KEY::BACKSPACE:
      if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) delete_selection(Self);
      else if (Self->txCursor.column() > 0) {
         if ((size_t)Self->txCursor.column() > Self->txLines[Self->txCursor.row()].length()) {
            Self->txCursor.move(Self, Self->txCursor.row(), Self->txLines[Self->txCursor.row()].lastChar()-1);
         }
         else Self->txCursor.move(Self, Self->txCursor.row(), Self->txCursor.column()-1);

         auto offset = Self->txLines[Self->txCursor.row()].utf8CharOffset(Self->txCursor.column());
         Self->txLines[Self->txCursor.row()].replace(offset, Self->txLines[Self->txCursor.row()].charLength(offset), "");
      }
      else if (Self->txCursor.row() > 0) { // The current line will be shifted up into the line above it
         if ((not Self->txLines[Self->txCursor.row()-1].empty()) or (not Self->txLines[Self->txCursor.row()].empty())) {
            Self->txLines[Self->txCursor.row()-1] += Self->txLines[Self->txCursor.row()];
            Self->txLines.erase(Self->txLines.begin() + Self->txCursor.row());
            Self->txCursor.move(Self, Self->txCursor.row()-1, Self->txLines[Self->txCursor.row()].utf8Length());
         }
         else {
            Self->txLines.erase(Self->txLines.begin() + Self->txCursor.row());
            Self->txCursor.move(Self, Self->txCursor.row()-1, Self->txLines[Self->txCursor.row()-1].length());
         }
      }
      else; // Do nothing, cursor at (0,0)

      mark_dirty(Self, RC::BASE_PATH);
      acDraw(Self);
      report_change(Self);
      break;

   case KEY::CLEAR:
      if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) delete_selection(Self);
      else {
         Self->txCursor.move(Self, Self->txCursor.row(), 0);
         ((objVectorText *)Self)->deleteLine(Self->txCursor.row());
      }
      report_change(Self);
      break;

   case KEY::DELETE:
      if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) delete_selection(Self);
      else if ((size_t)Self->txCursor.column() < Self->txLines[Self->txCursor.row()].length()) {
         auto offset = Self->txLines[Self->txCursor.row()].utf8CharOffset(Self->txCursor.column());
         Self->txLines[Self->txCursor.row()].replace(offset, Self->txLines[Self->txCursor.row()].charLength(offset), "");
      }
      else if ((size_t)Self->txCursor.row() < Self->txLines.size() - 1) { // The next line is going to be pulled up into the current line
         Self->txLines[Self->txCursor.row()] += Self->txLines[Self->txCursor.row()+1];
         Self->txLines.erase(Self->txLines.begin() + Self->txCursor.row() + 1);
      }
      mark_dirty(Self, RC::BASE_PATH);
      acDraw(Self);
      report_change(Self);
      break;

   case KEY::END:
      Self->txCursor.move(Self, Self->txCursor.row(), Self->txLines[Self->txCursor.row()].length());
      break;

   case KEY::TAB:
      break;

   case KEY::ENTER:
   case KEY::NP_ENTER: {
      if ((Self->txLineLimit) and (Self->txLines.size() >= (size_t)Self->txLineLimit)) break;

      if ((Self->txFlags & VTXF::AREA_SELECTED) != VTXF::NIL) delete_selection(Self);
      if (Self->txLines.empty()) Self->txLines.resize(1);

      auto row    = Self->txCursor.row();
      auto offset = Self->txLines[row].utf8CharOffset(Self->txCursor.column());
      Self->txCursor.move(Self, row + 1, 0);
      add_line(Self, Self->txLines[row], offset, Self->txLines[row].length() - offset, row+1);

      if (not offset) Self->txLines[row].clear();
      else Self->txLines[row].replace(offset, Self->txLines[row].length() - offset, "");
      mark_dirty(Self, RC::BASE_PATH);
      acDraw(Self);
      report_change(Self);
      break;
   }

   case KEY::HOME:
      Self->txCursor.move(Self, Self->txCursor.row(), 0);
      break;

   case KEY::INSERT:
      if ((Self->txFlags & VTXF::OVERWRITE) != VTXF::NIL) Self->txFlags &= ~VTXF::OVERWRITE;
      else Self->txFlags |= VTXF::OVERWRITE;
      break;

   case KEY::LEFT:
      Self->txCursor.resetFlash();
      Self->txCursor.vector->setVisibility(VIS::VISIBLE);
      if (Self->txCursor.column() > 0) Self->txCursor.move(Self, Self->txCursor.row(), Self->txCursor.column()-1);
      else if (Self->txCursor.row() > 0) Self->txCursor.move(Self, Self->txCursor.row()-1, Self->txLines[Self->txCursor.row()-1].utf8Length());
      break;

   case KEY::RIGHT:
      Self->txCursor.resetFlash();
      Self->txCursor.vector->setVisibility(VIS::VISIBLE);
      if (not Self->txLines.empty()) {
         if (Self->txCursor.column() < Self->txLines[Self->txCursor.row()].utf8Length()) {
            Self->txCursor.move(Self, Self->txCursor.row(), Self->txCursor.column()+1);
         }
         else if ((size_t)Self->txCursor.row() < Self->txLines.size() - 1) {
            Self->txCursor.move(Self, Self->txCursor.row()+1, 0);
         }
      }
      break;

   case KEY::DOWN:
   case KEY::UP:
      Self->txCursor.resetFlash();
      Self->txCursor.vector->setVisibility(VIS::VISIBLE);
      if (((Event->Code IS KEY::UP) and (Self->txCursor.row() > 0)) or
          ((Event->Code IS KEY::DOWN) and ((size_t)Self->txCursor.row() < Self->txLines.size()-1))) {
         int end_column;

         // Determine the current true position of the current cursor column, in UTF-8.  Then determine the cursor
         // character that we are going to be at when we end up at the row above us.

         if (((Self->txCursor.row() << 16) | Self->txCursor.column()) IS Self->txCursor.savePos) {
            end_column = Self->txCursor.endColumn;
         }
         else {
            Self->txCursor.endColumn = 0;
            end_column = Self->txCursor.column();
         }

         int colchar = 0;
         int col = 0;
         int i = 0;
         while (((size_t)i < Self->txLines[Self->txCursor.row()].length()) and (colchar < end_column)) {
            col++;
            colchar++;
            for (++i; (Self->txLines[Self->txCursor.row()][i] & 0xc0) == 0x80; i++);
         }

         Self->txFlags &= ~VTXF::AREA_SELECTED;

         int new_row;
         if (Event->Code IS KEY::UP) new_row = Self->txCursor.row() - 1;
         else new_row = Self->txCursor.row() + 1;

         int new_column;
         for (new_column=0, i=0; (col > 0) and ((size_t)i < Self->txLines[new_row].length());) {
            col--;
            new_column++;
            for (++i; (Self->txLines[new_row][i] & 0xc0) IS 0x80; i++);
         }

         if (new_column > Self->txCursor.endColumn) Self->txCursor.endColumn = new_column;
         Self->txCursor.savePos = ((int)new_row << 16) | new_column;
         Self->txCursor.move(Self, new_row, new_column);
         acDraw(Self);
      }
      break;

   default:
      break;
   }
}

//********************************************************************************************************************

static void delete_selection(extVectorText *Self)
{
   Self->txFlags &= ~VTXF::AREA_SELECTED;

   int row, column, end_row, end_column;
   Self->txCursor.selectedArea(Self, &row, &column, &end_row, &end_column);
   column = Self->txLines[row].utf8CharOffset(column);
   end_column = Self->txLines[end_row].utf8CharOffset(end_column);

   if (row IS end_row) { // Selection limited to one line
      Self->txLines[row].replace(column, end_column - column, "");
      Self->txCursor.move(Self, row, column);
   }
   else {
      Self->txLines[row].resize(column); // Truncate to end of the first line
      Self->txCursor.move(Self, row, column);

      row++;
      if (row < end_row) Self->txLines.erase(Self->txLines.begin() + row, Self->txLines.begin() + end_row);

      if (end_column > 0) Self->txLines[row].replace(0, end_column, "");
   }

   mark_dirty(Self, RC::BASE_PATH);
}

//********************************************************************************************************************
// Note: This function validates boundaries except for the column going beyond the string length.

void TextCursor::move(extVectorText *Vector, int Row, int Column, bool ValidateWidth)
{
   Vector->txFlags &= ~VTXF::AREA_SELECTED;

   if (Row < 0) Row = 0;
   else if ((size_t)Row >= Vector->txLines.size()) {
      if (not Vector->txLines.empty()) Row = (int)Vector->txLines.size() - 1;
   }

   if (Column < 0) Column = 0;
   else if (ValidateWidth) {
      if (not Vector->txLines.empty()) {
         int max_col = Vector->txLines[mRow].utf8Length();
         if (Column > max_col) Column = max_col;
      }
   }

   mRow = Row;
   mColumn = Column;

   reset_vector(Vector);

   acDraw(Vector);
}

//********************************************************************************************************************

void TextCursor::reset_vector(extVectorText *Vector) const
{
   if (Vector->txCursor.vector) {
      auto &line = Vector->txLines[mRow];

      if (not line.chars.empty()) {
         auto col = mColumn;
         if ((size_t)col >= line.chars.size()) col = line.chars.size() - 1;
         Vector->txCursor.vector->Points[0].X = line.chars[col].x1 + 0.5;
         Vector->txCursor.vector->Points[0].Y = line.chars[col].y1;
         Vector->txCursor.vector->Points[1].X = line.chars[col].x2 + 0.5;
         Vector->txCursor.vector->Points[1].Y = line.chars[col].y2;
         reset_path(Vector->txCursor.vector);

         // If the cursor X,Y lies outside of the parent viewport, offset the text so that it remains visible to
         // the user.

         if ((not Vector->Morph) and (Vector->ParentView)) {
            auto p_width = Vector->ParentView->vpFixedWidth;
            double xo = 0;
            const double CURSOR_MARGIN = Vector->txFontSize * 0.5;
            if (p_width > 8) {
               if (Vector->txX + line.chars[col].x1 <= 0) xo = Vector->txX + line.chars[col].x1;
               else if (Vector->txX + line.chars[col].x1 + CURSOR_MARGIN > p_width) xo = -(Vector->txX + line.chars[col].x1 + CURSOR_MARGIN - p_width);
            }

            auto p_height = Vector->ParentView->vpFixedHeight;
            double yo = 0;
            if ((mRow > 0) and (p_height > Vector->txFontSize)) {
               if (Vector->txY + line.chars[col].y1 <= 0) yo = Vector->txY + line.chars[col].y1;
               else if (Vector->txY + line.chars[col].y2 > p_height) yo = -(Vector->txY + line.chars[col].y2 - p_height + CURSOR_MARGIN);
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
   if ((size_t)row > Self->txLines.size()) row = Self->txLines.size() - 1;

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

static void insert_char(extVectorText *Self, int Unicode, int Column)
{
   if ((not Self) or (not Unicode)) return;

   mark_dirty(Self, RC::BASE_PATH);

   char buffer[6];
   int charlen = UTF8WriteValue(Unicode, buffer, 6);

   if (Self->txLines.empty()) {
      Self->txLines.emplace_back(std::string(buffer, charlen));
      Self->txCursor.move(Self, 0, 1);
   }
   else if (Self->txLines[Self->txCursor.row()].empty()) {
      if (Self->txCharLimit < 1) return;
      Self->txLines[Self->txCursor.row()].append(buffer, charlen);
      Self->txCursor.move(Self, Self->txCursor.row(), 1);
   }
   else {
      auto offset = Self->txLines[Self->txCursor.row()].utf8CharOffset(Column);

      if (((Self->txFlags & VTXF::OVERWRITE) != VTXF::NIL) and ((size_t)offset < Self->txLines[Self->txCursor.row()].length())) {
         Self->txLines[Self->txCursor.row()].replace(offset, Self->txLines[Self->txCursor.row()].charLength(offset), "");
      }

      Self->txLines[Self->txCursor.row()].insert(offset, buffer, charlen);

      // Enforce character limits (delete the character at the end of this line to keep it within limits)

      if (Self->txLines[Self->txCursor.row()].utf8Length() > Self->txCharLimit) {
         Self->txLines[Self->txCursor.row()].resize(Self->txLines[Self->txCursor.row()].lastChar());
      }

      Self->txCursor.move(Self, Self->txCursor.row(), Self->txCursor.column() + 1, true);
   }

   report_change(Self);
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
   { "X",             FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, TEXT_GET_X, TEXT_SET_X },
   { "Y",             FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, TEXT_GET_Y, TEXT_SET_Y },
   { "Weight",        FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_Weight, TEXT_SET_Weight },
   { "String",        FDF_VIRTUAL|FDF_STRING|FDF_RW, TEXT_GET_String, TEXT_SET_String },
   { "Align",         FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, TEXT_GET_Align, TEXT_SET_Align, &clTextAlign },
   { "Face",          FDF_VIRTUAL|FDF_STRING|FDF_RW, TEXT_GET_Face, TEXT_SET_Face },
   { "Fill",          FDF_VIRTUAL|FDF_STRING|FDF_RW, TEXT_GET_Fill, TEXT_SET_Fill },
   { "FontSize",      FDF_VIRTUAL|FDF_ALLOC|FDF_STRING|FDF_RW, TEXT_GET_FontSize, TEXT_SET_FontSize },
   { "FontStyle",     FDF_VIRTUAL|FDF_STRING|FDF_RI, TEXT_GET_FontStyle, TEXT_SET_FontStyle },
   { "Descent",       FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_Descent },
   { "DisplayHeight", FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_DisplayHeight },
   { "DisplaySize",   FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_DisplaySize },
   { "DX",            FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW, TEXT_GET_DX, TEXT_SET_DX },
   { "DY",            FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW, TEXT_GET_DY, TEXT_SET_DY },
   { "InlineSize",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, TEXT_GET_InlineSize, TEXT_SET_InlineSize },
   { "LetterSpacing", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, TEXT_GET_LetterSpacing, TEXT_SET_LetterSpacing },
   { "Point",         FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_Point },
   { "LineSpacing",   FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_LineSpacing },
   { "Rotate",        FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW, TEXT_GET_Rotate, TEXT_SET_Rotate },
   { "ShapeInside",   FDF_VIRTUAL|FDF_OBJECTID|FDF_RW, TEXT_GET_ShapeInside, TEXT_SET_ShapeInside, CLASSID::VECTOR },
   { "ShapeSubtract", FDF_VIRTUAL|FDF_OBJECTID|FDF_RW, TEXT_GET_ShapeSubtract, TEXT_SET_ShapeSubtract, CLASSID::VECTOR },
   { "TextLength",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, TEXT_GET_TextLength, TEXT_SET_TextLength },
   { "TextFlags",     FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, TEXT_GET_Flags, TEXT_SET_Flags, &clVectorTextVTXF },
   { "TextWidth",     FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_TextWidth },
   { "StartOffset",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, TEXT_GET_StartOffset, TEXT_SET_StartOffset },
   { "Spacing",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, TEXT_GET_Spacing, TEXT_SET_Spacing },
   { "Font",          FDF_VIRTUAL|FDF_OBJECT|FDF_I, nullptr, TEXT_SET_Font },
   // Non-SVG fields related to real-time text editing
   { "OnChange",      FDF_VIRTUAL|FDF_FUNCTIONPTR|FDF_RW, TEXT_GET_OnChange, TEXT_SET_OnChange },
   { "Focus",         FDF_VIRTUAL|FDF_OBJECTID|FDF_RI, TEXT_GET_Focus, TEXT_SET_Focus },
   { "CursorColumn",  FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_CursorColumn, TEXT_SET_CursorColumn },
   { "CursorRow",     FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_CursorRow, TEXT_SET_CursorRow },
   { "TotalLines",    FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_TotalLines },
   { "SelectRow",     FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_SelectRow },
   { "SelectColumn",  FDF_VIRTUAL|FDF_INT|FDF_R, TEXT_GET_SelectColumn },
   { "LineLimit",     FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_LineLimit, TEXT_SET_LineLimit },
   { "CharLimit",     FDF_VIRTUAL|FDF_INT|FDF_RW, TEXT_GET_CharLimit, TEXT_SET_CharLimit },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_text(void)
{
   FID_FreetypeFace = strihash("FreetypeFace");

   OBJECTID id;
   if (FindObject("cfgSystemFonts", CLASSID::CONFIG, FOF::NIL, &id) IS ERR::Okay) {
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
