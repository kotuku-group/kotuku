/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Font: Draws bitmap fonts and manages font meta information.

The Font class renders fixed-size bitmap fonts to a @Bitmap and exposes metrics for the selected font face.  It
supports bold, italic and underlined text, as well as adjustable glyph spacing, line spacing, tab width and alignment.
Bitmap fonts are loaded from Windows `.fon` files.  Scalable TrueType rendering is provided by @VectorText instead.

Bitmap fonts must be stored in the `fonts:fixed/` directory to be recognised during font database refreshes.  Use the
Font module's query and refresh functions to inspect or rebuild the installed font list.

Font strings are interpreted as UTF-8, allowing Unicode text to be supplied through the #String field.  Do not insert
arbitrary byte values above `127` unless they are valid UTF-8 sequences.  Invalid or unsupported glyphs fall back to
the font's default character.

Initialisation of a new Font object can be as simple as declaring its #Point size and #Face name.  Font face, size and
style selections should be set before initialisation because the loaded bitmap data is cached for that combination.
To use multiple styles of the same face, create one Font object for each required style.  Runtime drawing properties
such as colour, #String, #X, #Y and alignment can still be changed between draw operations.

To draw a string, set #Bitmap and #String, then call #Draw().  The #X and #Y fields set the starting position.  Use
#Align together with #AlignWidth and #AlignHeight when text needs to be positioned within a larger surface area.

This documentation uses the following font terminology:

<list type="bullet">
<li>`Point` is the requested size of the font.  It is relative to other point sizes of the same face; two faces at the
same point size are not necessarily the same pixel height.</li>
<li>`Height` is the vertical bearing of the font expressed in pixels.  It excludes top leading and the gutter used by
descending glyphs.</li>
<li>`Gutter` is the space below the baseline used by descending glyphs such as `g` and `y`.  It is also known as the
external leading or descent.</li>
<li>`LineSpacing` is the recommended pixel distance from one baseline to the next.</li>
<li>`Glyph` is a single rendered character image.</li>
</list>

Use @VectorText for most display text.  The Font class draws directly to a @Bitmap and is not integrated with the
display vector scene graph.

-END-

*********************************************************************************************************************/

static BitmapCache * check_bitmap_cache(extFont *, FTF);
static ERR SET_Point(extFont *Self, double);
static ERR SET_Style(extFont *, const std::string_view &);

/*********************************************************************************************************************

-ACTION-
Draw: Draws a font to a Bitmap.

Draws #String to the target #Bitmap, starting at #X and #Y after any configured alignment and baseline adjustments have
been applied.

-ERRORS-
Okay
FieldNotSet: The #Bitmap field has not been set.
-END-

*********************************************************************************************************************/

static ERR draw_bitmap_font(extFont *);

static ERR FONT_Draw(extFont *Self)
{
   return draw_bitmap_font(Self);
}

//********************************************************************************************************************

static ERR FONT_Free(extFont *Self)
{
   CACHE_LOCK lock(glCacheMutex);

   if (Self->BmpCache) {
      // Reduce the usage count.  Use a timed delay on freeing the font in case it is used again.
      Self->BmpCache->OpenCount--;
      if (!Self->BmpCache->OpenCount) {
         if (!glCacheTimer) {
            kt::SwitchContext ctx(modFont);
            SubscribeTimer(60.0, C_FUNCTION(bitmap_cache_cleaner), &glCacheTimer);
         }
      }
   }

   Self->~extFont();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR FONT_Init(extFont *Self)
{
   kt::Log log;
   int diff;
   FTF style;
   FMETA meta = FMETA::NIL;

   if ((Self->Face.empty()) and (Self->Path.empty())) return log.warning(ERR::FieldNotSet);

   if (!Self->Point) Self->Point = global_point_size();

   if (Self->Path.empty()) {
      std::string path;
      if (auto error = fnt::SelectFont(Self->Face, Self->Style, &path, &meta); error IS ERR::Okay) {
         error = Self->set(FID_Path, path);
         if (error != ERR::Okay) return error;
      }
      else {
         log.warning("Font \"%s\" (point %.2f, style %s) is not recognised.", Self->Face.c_str(), Self->Point,
            Self->Style.c_str());
         return error;
      }
   }

   // Check the bitmap cache to see if we have already loaded this font

   style = font_style_flags(Self->Style);

   CACHE_LOCK lock(glCacheMutex);

   BitmapCache *cache = check_bitmap_cache(Self, style);

   if (cache); // The font exists in the cache
   else if (wildcmp("*.ttf", Self->Path)) return ERR::NoSupport;
   else {
      objFile::create file = { fl::Path(Self->Path), fl::Flags(FL::READ|FL::APPROXIMATE) };
      if (file.ok()) {
         std::vector<winFont> fonts;
         if (auto error = read_winfont_entries(*file, fonts); error IS ERR::NoData) return log.warning(error);
         else if (error != ERR::Okay) return error;

         // Scan the list of available fonts to find the closest point size for our font

         int abs = 0x7fff;
         int wfi = 0;
         winfnt_header_fields face;
         for (int i=0; i < int(fonts.size()); i++) {
            file->seekStart(fonts[i].Offset);

            winfnt_header_fields header;
            if (file->read(&header, sizeof(header)) IS ERR::Okay) {
               if (auto error = validate_winfnt_header(header, ""); error != ERR::Okay) {
                  return log.warning(error);
               }

               if (header.pixel_width <= 0) header.pixel_width = header.pixel_height;

               if ((diff = Self->Point - header.nominal_point_size) < 0) diff = -diff;

               if (diff < abs) {
                  face = header;
                  abs  = diff;
                  wfi  = i;
               }
            }
            else return log.warning(ERR::Read);
         }

         // Check the bitmap cache again to ensure that the discovered font is not already loaded.  This is important
         // if the cached font wasn't originally found due to variation in point size.

         Self->Point = face.nominal_point_size;
         cache = check_bitmap_cache(Self, style);
         if (!cache) { // Load the font into the cache
            auto it = glBitmapCache.emplace(glBitmapCache.end(), face, Self->Style, Self->Path, *file, fonts[wfi]);

            if (it->Result IS ERR::Okay) cache = &(*it);
            else {
               ERR error = it->Result;
               glBitmapCache.erase(it);
               return error;
            }
         }
      }
      else return log.warning(ERR::OpenFile);
   }

   if (cache) {
      Self->prvData     = cache->mData.data();
      Self->Ascent      = cache->Header.ascent;
      Self->Point       = cache->Header.nominal_point_size;
      Self->Height      = cache->Header.ascent - cache->Header.internal_leading + cache->Header.external_leading;
      Self->Leading     = cache->Header.internal_leading;
      Self->Gutter      = cache->Header.external_leading;
      if (!Self->Gutter) Self->Gutter = cache->Header.pixel_height - Self->Height - cache->Header.internal_leading;
      Self->LineSpacing += cache->Header.pixel_height; // Add to any preset linespacing rather than over-riding
      Self->MaxHeight   = cache->Header.pixel_height; // Supposedly the pixel_height includes internal and external leading values (?)
      Self->prvBitmapHeight = cache->Header.pixel_height;
      Self->prvDefaultChar  = cache->Header.first_char + cache->Header.default_char;

      // If this is a monospaced font, set the FixedWidth field

      if (cache->Header.avg_width IS cache->Header.max_width) {
         Self->FixedWidth = cache->Header.avg_width;
      }

      Self->prvChar = cache->Chars;
      Self->Flags |= cache->StyleFlags;

      cache->OpenCount++;

      Self->BmpCache = cache;
   }
   else return ERR::NoSupport;

   // Remove the location string to reduce resource usage

   Self->Path.clear();

   log.detail("Family: %s, Style: %s, Point: %.2f, Height: %d", Self->Face.c_str(), Self->Style.c_str(),
      Self->Point, Self->Height);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR FONT_NewPlacement(extFont *Self)
{
   new (Self) extFont;
   Self->TabSize         = 8;
   Self->prvDefaultChar  = '.';
   Self->prvLineCountCR  = 1;
   Self->Colour.Alpha    = 255;
   Self->GlyphSpacing    = 1.0;
   Self->Style           = "Regular";
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Align: Sets the position of a font string to an abstract alignment.

Sets the alignment of the font string within the #AlignWidth and #AlignHeight area.  This is used in addition to the
#X and #Y drawing coordinates.

-FIELD-
AlignHeight: The height to use when aligning the font string.

Defines the height of the area used for vertical alignment.  If this field is `0`, the target #Bitmap height is used.

-FIELD-
AlignWidth: The width to use when aligning the font string.

Defines the width of the area used for horizontal alignment.  If this field is `0`, the target #Bitmap width is used.

-FIELD-
Ascent: The total number of pixels above the baseline.

Reflects the total number of pixels above the baseline, including #Leading.

-FIELD-
Bitmap: The destination Bitmap to use when drawing a font.

-FIELD-
Bold: Set to `true` to enable bold styling.

Setting this field before initialisation selects the `Bold` style, or `Bold Italic` if #Italic is also set.  Prefer
#Style when setting an exact style name.

*********************************************************************************************************************/

static ERR GET_Bold(extFont *Self, int *Value)
{
   if ((Self->Flags & FTF::BOLD) != FTF::NIL) *Value = TRUE;
   else if (kt::strisearch("bold", Self->Style) != -1) *Value = TRUE;
   else *Value = FALSE;
   return ERR::Okay;
}

static ERR SET_Bold(extFont *Self, int Value)
{
   const bool bold = Value != FALSE;
   const bool italic = ((Self->Flags & FTF::ITALIC) != FTF::NIL) or (kt::strisearch("italic", Self->Style) != -1);

   if (bold and italic) return SET_Style(Self, "Bold Italic");
   else if (bold) return SET_Style(Self, "Bold");
   else if (italic) return SET_Style(Self, "Italic");
   else return SET_Style(Self, "Regular");
}

/*********************************************************************************************************************

-FIELD-
Colour: The font colour in !RGB8 format.

-FIELD-
EndX: Indicates the final horizontal coordinate after completing a draw operation.

Reflects the final horizontal coordinate reached by the most recent #Draw() operation.

-FIELD-
EndY: Indicates the final vertical coordinate after completing a draw operation.

Reflects the final vertical coordinate reached by the most recent #Draw() operation.

-FIELD-
Face: The name of a font face that is to be loaded on initialisation.

The name of an installed font face must be specified before initialisation unless #Path is set directly.  A list of
available faces can be obtained from ~Font.GetList().

The face string can include optional point size, style and colour values in the form `face:pointsize:style:colour`.
Omitted middle values may be left empty.

Here are some examples:

<pre>
Noto Sans:12:Bold Italic:#ff0000
Courier:10.6
Charter:120%::255,128,255
</pre>

Multiple font faces can be specified in CSV format, e.g. `Sans Serif,Noto Sans`.  Names are resolved from left to
right.

*********************************************************************************************************************/

static ERR SET_Face(extFont *Self, const std::string_view &Value)
{
   if (not Value.empty()) {
      std::string_view final_name;
      auto i = Value.find(':');
      auto face = Value.substr(0, i);
      if (auto error = fnt::ResolveFamilyName(face, &final_name); error IS ERR::Okay) {
         Self->Face.assign(final_name);
      }
      else return error;

      if (i IS std::string::npos) return ERR::Okay;

      // Extract the point size

      auto point = Value.substr(i + 1);
      i = point.find(':');
      auto point_value = point.substr(0, i);
      if (not point_value.empty()) {
         double pt = 0;
         auto result = std::from_chars(point_value.data(), point_value.data() + point_value.size(), pt);
         if (result.ec IS std::errc()) SET_Point(Self, pt);
      }

      if (i IS std::string::npos) return ERR::Okay;

      // Extract the style string

      auto style = point.substr(i + 1);
      i = style.find(':');

      SET_Style(Self, style.substr(0, i));

      if (i IS std::string::npos) return ERR::Okay;

      // Extract the colour string

      Self->set(FID_Colour, style.substr(i + 1));
   }
   else Self->Face.clear();

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FixedWidth: Forces a fixed pixel width to use for all glyphs.

Forces all glyphs to advance by the specified pixel width.  If the value is less than the widest glyph, rendered glyphs
can overlap.

-FIELD-
Flags:  Optional flags.

*********************************************************************************************************************/

static ERR SET_Flags(extFont *Self, FTF Value)
{
   Self->Flags = (Self->Flags & FTF(0xff000000)) | (Value & FTF(0x00ffffff));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Gutter: The 'external leading' value, measured in pixels.  Applies to fixed fonts only.

Reflects the external leading, or descent area, below the baseline.

-FIELD-
Height: The point size of the font, expressed in pixels.

Reflects the initialised font height in pixels.  It does not include #Leading; use #Ascent when the leading-inclusive
distance above the baseline is required.

The height is calculated on initialisation and can be read at any time.

-FIELD-
GlyphSpacing: Adjusts the amount of spacing between each character.

Adjusts horizontal glyph advance as a multiplier of each glyph's normal width.  The default value is `1.0`.

Using negative values is valid, and can lead to text being printed backwards.

-FIELD-
Italic: Set to `true` to enable italic styling.

Setting this field before initialisation selects the `Italic` style, or `Bold Italic` if #Bold is also set.  Prefer
#Style when setting an exact style name.

*********************************************************************************************************************/

static ERR GET_Italic(extFont *Self, int *Value)
{
   if ((Self->Flags & FTF::ITALIC) != FTF::NIL) *Value = TRUE;
   else if (kt::strisearch("italic", Self->Style) != -1) *Value = TRUE;
   else *Value = FALSE;
   return ERR::Okay;
}

static ERR SET_Italic(extFont *Self, int Value)
{
   const bool italic = Value != FALSE;
   const bool bold = ((Self->Flags & FTF::BOLD) != FTF::NIL) or (kt::strisearch("bold", Self->Style) != -1);

   if (bold and italic) return SET_Style(Self, "Bold Italic");
   else if (bold) return SET_Style(Self, "Bold");
   else if (italic) return SET_Style(Self, "Italic");
   else return SET_Style(Self, "Regular");
}

/*********************************************************************************************************************

-FIELD-
Leading: 'Internal leading' measured in pixels.  Applies to fixed fonts only.

-FIELD-
LineCount: The total number of lines in a font string.

Returns the number of lines in #String.  If #WrapEdge is set, wrapped lines are included in the count.

*********************************************************************************************************************/

static ERR GET_LineCount(extFont *Self, int *Value)
{
   if (!Self->prvLineCount) calc_lines(Self);
   *Value = Self->prvLineCount;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LineSpacing: The amount of spacing between each line.

Defines the vertical distance from one line baseline to the next.  It is initialised from the selected font and can be
increased or decreased to adjust line spacing.  If negative, later lines are drawn upward.

If set before initialisation, the value is added to the font's normal line spacing instead of replacing it.
For instance, setting the LineSpacing to 2 will result in an extra 2 pixels being added to the font's spacing.

-FIELD-
Path: The path to a font file.

Defines the exact font file to load before initialisation.  This bypasses normal face-name resolution through the font
database.

This feature is ideal for use when distributing custom fonts with an application.

*********************************************************************************************************************/

static ERR SET_Path(extFont *Self, const std::string_view &Value)
{
   if (!Self->initialised()) {
      Self->Path.assign(Value);
      return ERR::Okay;
   }
   else return ERR::Failed;
}

static ERR GET_Location(extFont *Self, std::string_view &Value)
{
   Value = Self->Path;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxHeight: The maximum possible pixel height per character.

Reflects the maximum bitmap height for the selected font at the current point size.

-FIELD-
Opacity: Determines the level of translucency applied to a font.

Determines the translucency level of the font fill colour.  The default setting is `100`, meaning fully opaque.
Lower values blend the glyphs with the destination #Bitmap.

Please note that the use of translucency will always have an impact on the time it normally takes to draw a font.

*********************************************************************************************************************/

static ERR GET_Opacity(extFont *Self, double *Value)
{
   *Value = (Self->Colour.Alpha * 100)>>8;
   return ERR::Okay;
}

static ERR SET_Opacity(extFont *Self, double Value)
{
   if (Value >= 100) Self->Colour.Alpha = 255;
   else if (Value <= 0) Self->Colour.Alpha = 0;
   else Self->Colour.Alpha = int(Value * (255.0 / 100.0));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Outline: Defines the outline colour around a font.

Draws a one-pixel outline around bitmap glyphs when set to an !RGB8 colour with a non-zero alpha component.  Set the
field to `NULL` or use an alpha value of zero to disable outlining.

-FIELD-
Point: The point size of a font.

Defines the requested font size in points.

When setting the point size of a bitmap font, the system will try and find the closest matching value for the requested
point size.  For instance, if you request a fixed font at point 11 and the closest size is point 8, the system will
drop the font to point 8.

*********************************************************************************************************************/

static ERR GET_Point(extFont *Self, double *Value)
{
   *Value = Self->Point;
   return ERR::Okay;
}

static ERR SET_Point(extFont *Self, double Value)
{
   if (Value < 1) Value = 1;
   Self->Point = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
String: The string to use when drawing a Font.

The String field must be defined to draw text with a Font object.  It must contain valid UTF-8.  Line feed characters
start a new line during #Draw().

If a string contains characters that are not supported by a font, those characters will be printed using a default
character from the font.

*********************************************************************************************************************/

static ERR SET_String(extFont *Self, const std::string_view &Value)
{
   if (Self->String.size() IS Value.size()) {
      if (std::string_view(Self->String) IS Value) return ERR::Okay;
   }

   Self->prvLineCount   = 0;
   Self->prvStrWidth    = 0; // Reset the string width for GET_Width
   Self->prvLineCountCR = 1; // Line count (carriage returns only)

   if (not Value.empty()) {
      for (auto ch : Value) {
         if (ch IS '\n') Self->prvLineCountCR++;
      }

      Self->String.assign(Value);
   }
   else Self->String.clear();

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Style: Determines font styling.

Selects the preferred style for initialisation.  If the selected face does not provide that style, regular styling or
the first registered style is used.

Bitmap fonts are a special case if a bold or italic style is selected.  In this situation the system can automatically
convert the font to that style even if the correct graphics set does not exist.

Conventional font styles are `Bold`, `Bold Italic`, `Italic` and `Regular` (the default).

*********************************************************************************************************************/

static ERR SET_Style(extFont *Self, const std::string_view &Value)
{
   if (Value.empty()) Self->Style = "Regular";
   else Self->Style.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TabSize: Defines the tab size to use when drawing and manipulating a font string.

Controls the tab interval, measured in character columns.

The default tab size is `8`.  This field only affects tab characters in #String.

-FIELD-
Underline: Enables font underlining when set.

Draws an underline using the supplied !RGB8 colour.  Set the field to `NULL` or use an alpha value of zero to disable
underlining.

-FIELD-
Width: Returns the pixel width of a string.

Returns the pixel width of #String using the current font metrics and wrapping settings.  If #String is empty, the
result is `0`.

*********************************************************************************************************************/

static ERR GET_Width(extFont *Self, int *Value)
{
   if (Self->String.empty()) {
      *Value = 0;
      return ERR::Okay;
   }

   if ((!Self->prvStrWidth) or ((Self->Align & (ALIGN::HORIZONTAL|ALIGN::RIGHT)) != ALIGN::NIL) or (Self->WrapEdge)){
      if (Self->WrapEdge > 0) {
         string_size(Self, Self->String, FSS_ALL, Self->WrapEdge - Self->X, &Self->prvStrWidth, nullptr);
      }
      else string_size(Self, Self->String, FSS_ALL, 0, &Self->prvStrWidth, nullptr);
   }

   *Value = Self->prvStrWidth;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
WrapEdge: Enables word wrapping at a given boundary.

Enables word wrapping when set to a value greater than zero.  Wrapping occurs when a word extends beyond this X
coordinate.

-FIELD-
X: The starting horizontal position when drawing the font string.

Defines the starting horizontal coordinate for #Draw().  The default coordinate is `0`.

-FIELD-
Y: The starting vertical position when drawing the font string.

Defines the starting vertical coordinate for #Draw().  The default coordinate is `0`.

-FIELD-
YOffset: Additional offset value that is added to vertically aligned fonts.

Returns the vertical offset applied by `VERTICAL` or `BOTTOM` alignment.  Add this value to #Y to determine the
aligned drawing baseline.
-END-

*********************************************************************************************************************/

static ERR GET_YOffset(extFont *Self, int *Value)
{
   if (Self->prvLineCount < 1) calc_lines(Self);

   if ((Self->Align & ALIGN::VERTICAL) != ALIGN::NIL) {
      int offset = (Self->AlignHeight - (Self->Height + (Self->LineSpacing * (Self->prvLineCount-1))))>>1;
      offset += (Self->LineSpacing - Self->MaxHeight)>>1; // Adjust for spacing between each individual line
      *Value = offset;
   }
   else if ((Self->Align & ALIGN::BOTTOM) != ALIGN::NIL) {
      *Value = Self->AlignHeight - (Self->MaxHeight + (Self->LineSpacing * (Self->prvLineCount-1)));
   }
   else *Value = 0;

   return ERR::Okay;
}

//********************************************************************************************************************

static const char * calc_line_layout(extFont *Self, std::string_view String, int *LineWidth, int *WrapIndex,
   int *XCoord)
{
   if (String.empty()) {
      *LineWidth = 0;
      *WrapIndex = 0;
   }
   else {
      int wrap = (Self->WrapEdge > 0) ? (Self->WrapEdge - Self->X) : 0;
      string_size(Self, String, FSS_LINE, wrap, LineWidth, WrapIndex);
   }

   if ((Self->Align & (ALIGN::HORIZONTAL|ALIGN::RIGHT)) != ALIGN::NIL) {
      if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) {
         *XCoord = Self->X + ((Self->AlignWidth - *LineWidth)>>1);
      }
      else *XCoord = Self->X + Self->AlignWidth - *LineWidth;
   }
   else *XCoord = Self->X;

   return String.data() + *WrapIndex;
}

//********************************************************************************************************************

static ERR draw_bitmap_font(extFont *Self)
{
   kt::Log log(__FUNCTION__);
   objBitmap *bitmap;
   RGB8 rgb;
   static const uint8_t table[] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
   uint8_t *xdata, *data;
   int linewidth, offset, wrapindex, charlen;
   uint32_t unicode, ocolour;
   int startx, xpos, ex, ey, sx, sy, xinc;
   int bytewidth, alpha, charwidth;
   bool draw_line;
   #define CHECK_LINE_CLIP(font,y,bmp) if (((y)-1 < (bmp)->Clip.Bottom) and ((y) + (font)->prvBitmapHeight + 1 > (bmp)->Clip.Top)) draw_line = true; else draw_line = false;

   // Validate settings for fixed font type

   if (not (bitmap = Self->Bitmap)) return log.warning(ERR::FieldNotSet);
   if (Self->String.empty()) return ERR::Okay;

   ERR error = ERR::Okay;
   auto str = std::string_view(Self->String);
   int dxcoord = Self->X;
   int dycoord = Self->Y;

   if (!Self->AlignWidth)  Self->AlignWidth  = bitmap->Width;
   if (!Self->AlignHeight) Self->AlignHeight = bitmap->Height;

   GET_YOffset(Self, &offset);
   dycoord = dycoord + offset - Self->Leading;

   if ((Self->Flags & FTF::BASE_LINE) != FTF::NIL) {
      dycoord -= (Self->Ascent - Self->Leading); // - 1;
   }

   const char *wrapstr = calc_line_layout(Self, str, &linewidth, &wrapindex, &dxcoord);
   const char *line_start = str.data();

   uint32_t colour  = bitmap->getColour(Self->Colour);
   uint32_t ucolour = bitmap->getColour(Self->Underline);

   if (Self->Outline.Alpha > 0) {
      Self->BmpCache->get_outline();
      ocolour = bitmap->getColour(Self->Outline);
   }
   else ocolour = 0;

   if (acLock(bitmap) != ERR::Okay) return log.warning(ERR::Lock);

   int dx = 0, dy = 0;
   startx = dxcoord;
   CHECK_LINE_CLIP(Self, dycoord, bitmap);
   while (not str.empty()) {
      if (str.front() IS '\n') { // Reset the font to a new line
         if (Self->Underline.Alpha > 0) {
            gfx::DrawRectangle(bitmap, startx, dycoord + Self->Height + 1, dxcoord-startx, ((Self->Flags & FTF::HEAVY_LINE) != FTF::NIL) ? 2 : 1, ucolour, BAF::FILL);
         }

         str.remove_prefix(1);

         while ((not str.empty()) and (uint8_t(str.front()) <= 0x20)) {
            if (str.front() IS '\n') dycoord += Self->LineSpacing;
            str.remove_prefix(1);
         }

         wrapstr = calc_line_layout(Self, str, &linewidth, &wrapindex, &dxcoord);
         line_start = str.data();
         startx = dxcoord;
         dycoord += Self->LineSpacing;
         CHECK_LINE_CLIP(Self, dycoord, bitmap);
      }
      else if (str.front() IS '\t') {
         int16_t tabwidth = (Self->prvChar['o'].Advance * Self->GlyphSpacing) * Self->TabSize;
         if (tabwidth) dxcoord = Self->X + tab_advance<int>(dxcoord - Self->X, tabwidth);
         str.remove_prefix(1);
      }
      else {
         charlen = getutf8(str, &unicode);
         if (charlen <= 0) break;

         if ((unicode > 255) or (!Self->prvChar) or (!Self->prvChar[unicode].Advance)) {
            unicode = Self->prvDefaultChar;
         }

         if (Self->FixedWidth > 0) charwidth = Self->FixedWidth;
         else charwidth = Self->prvChar[unicode].Advance;

         // Wordwrap management

         if ((str.data() >= wrapstr) and (str.data() > line_start)) {
            dxcoord = Self->X;
            dycoord += Self->LineSpacing;

            while ((not str.empty()) and (uint8_t(str.front()) <= 0x20)) {
               if (str.front() IS '\n') dycoord += Self->LineSpacing;
               str.remove_prefix(1);
            }
            if (str.empty()) break;

            wrapstr = calc_line_layout(Self, str, &linewidth, &wrapindex, &dxcoord);
            line_start = str.data();
            CHECK_LINE_CLIP(Self, dycoord, bitmap);
         }

         str.remove_prefix(size_t(charlen));

         if ((unicode > 0x20) and (draw_line)) {
            if (Self->Outline.Alpha > 0) { // Outline support
               auto outline = Self->BmpCache->get_outline();
               if (outline) {
                  auto data = outline + Self->prvChar[unicode].OutlineOffset;
                  bytewidth = (Self->prvChar[unicode].Width + 9)>>3;

                  sx = dxcoord - 1;
                  ex = sx + Self->prvChar[unicode].Width + 2;

                  if (ex > bitmap->Clip.Right) ex = bitmap->Clip.Right;

                  xinc = 0;
                  if (sx < bitmap->Clip.Left) {
                     xinc = bitmap->Clip.Left - sx;
                     sx = bitmap->Clip.Left;
                  }

                  sy = dycoord - 1;

                  ey = sy + Self->prvBitmapHeight + 2;
                  if (ey > bitmap->Clip.Bottom) ey = bitmap->Clip.Bottom;

                  if (sy < bitmap->Clip.Top) {
                     data += bytewidth * (bitmap->Clip.Top - sy);
                     sy = bitmap->Clip.Top;
                  }

                  if (Self->Outline.Alpha < 255) {
                     alpha = 255 - Self->Outline.Alpha;
                     for (dy=sy; dy < ey; dy++) {
                        xpos = xinc;
                        for (dx=sx; dx < ex; dx++) {
                           if (data[xpos>>3] & (0x80>>(xpos & 0x7))) {
                              bitmap->ReadUCRPixel(bitmap, dx, dy, &rgb);
                              rgb.Red   = Self->Outline.Red   + (((rgb.Red   - Self->Outline.Red) * alpha)>>8);
                              rgb.Green = Self->Outline.Green + (((rgb.Green - Self->Outline.Green) * alpha)>>8);
                              rgb.Blue  = Self->Outline.Blue  + (((rgb.Blue  - Self->Outline.Blue) * alpha)>>8);
                              bitmap->DrawUCRPixel(bitmap, dx, dy, &rgb);
                           }
                           xpos++;
                        }
                        data += bytewidth;
                     }
                  }
                  else {
                     for (dy=sy; dy < ey; dy++) {
                        xpos = xinc;
                        for (dx=sx; dx < ex; dx++) {
                           if (data[xpos>>3] & (0x80>>(xpos & 0x7))) {
                              bitmap->DrawUCPixel(bitmap, dx, dy, ocolour);
                           }
                           xpos++;
                        }
                        data += bytewidth;
                     }
                  }
               }
            }

            data = Self->prvData + Self->prvChar[unicode].Offset;
            bytewidth = (Self->prvChar[unicode].Width + 7)>>3;

            // Horizontal coordinates

            sx = dxcoord;

            ex = sx + Self->prvChar[unicode].Width;
            if (ex > bitmap->Clip.Right) ex = bitmap->Clip.Right;

            xinc = 0;
            if (sx < bitmap->Clip.Left) {
               xinc = bitmap->Clip.Left - sx;
               sx = bitmap->Clip.Left;
            }

            // Vertical coordinates

            sy = dycoord;

            ey = sy + Self->prvBitmapHeight;
            if (ey > bitmap->Clip.Bottom) ey = bitmap->Clip.Bottom;

            if (sy < bitmap->Clip.Top) {
               data += bytewidth * (bitmap->Clip.Top - sy);
               sy = bitmap->Clip.Top;
            }

            if (Self->Colour.Alpha < 255) {
               alpha = 255 - Self->Colour.Alpha;
               for (dy=sy; dy < ey; dy++) {
                  xpos = xinc;
                  for (dx=sx; dx < ex; dx++) {
                     if (data[xpos>>3] & (0x80>>(xpos & 0x7))) {
                        bitmap->ReadUCRPixel(bitmap, dx, dy, &rgb);
                        rgb.Red   = Self->Colour.Red   + (((rgb.Red   - Self->Colour.Red) * alpha)>>8);
                        rgb.Green = Self->Colour.Green + (((rgb.Green - Self->Colour.Green) * alpha)>>8);
                        rgb.Blue  = Self->Colour.Blue  + (((rgb.Blue  - Self->Colour.Blue) * alpha)>>8);
                        bitmap->DrawUCRPixel(bitmap, dx, dy, &rgb);
                     }
                     xpos++;
                  }
                  data += bytewidth;
               }
            }
            else {
               if (bitmap->BytesPerPixel IS 4) {
                  auto dest = (uint32_t *)(bitmap->Data + (sx<<2) + (sy * bitmap->LineWidth));
                  for (dy=sy; dy < ey; dy++) {
                     xpos = xinc & 0x07;
                     xdata = data + (xinc>>3);
                     for (dx=0; dx < ex-sx; dx++) {
                        if (*xdata & table[xpos++]) dest[dx] = colour;
                        if (xpos > 7) {
                           xpos = 0;
                           xdata++;
                        }
                     }
                     dest = (uint32_t *)(((uint8_t *)dest) + bitmap->LineWidth);
                     data += bytewidth;
                  }
               }
               else if (bitmap->BytesPerPixel IS 2) {
                  auto dest = (uint16_t *)(bitmap->Data + (sx<<1) + (sy * bitmap->LineWidth));
                  for (dy=sy; dy < ey; dy++) {
                     xpos = xinc & 0x07;
                     xdata = data + (xinc>>3);
                     for (dx=0; dx < ex-sx; dx++) {
                        if (*xdata & table[xpos++]) dest[dx] = colour;
                        if (xpos > 7) {
                           xpos = 0;
                           xdata++;
                        }
                     }
                     dest = (uint16_t *)(((uint8_t *)dest) + bitmap->LineWidth);
                     data += bytewidth;
                  }
               }
               else if (bitmap->BitsPerPixel IS 8) {
                  if ((bitmap->Flags & BMF::MASK) != BMF::NIL) {
                     if ((bitmap->Flags & BMF::INVERSE_ALPHA) != BMF::NIL) colour = 0;
                     else colour = 255;
                  }

                  auto dest = (uint8_t *)(bitmap->Data + sx + (sy * bitmap->LineWidth));
                  for (dy=sy; dy < ey; dy++) {
                     xpos = xinc & 0x07;
                     xdata = data + (xinc>>3);
                     for (dx=0; dx < ex-sx; dx++) {
                        if (*xdata & table[xpos++]) dest[dx] = (uint8_t)colour;
                        if (xpos > 7) {
                           xpos = 0;
                           xdata++;
                        }
                     }
                     dest = (uint8_t *)(((uint8_t *)dest) + bitmap->LineWidth);
                     data += bytewidth;
                  }
               }
               else {
                  for (dy=sy; dy < ey; dy++) {
                     xpos = xinc & 0x07;
                     xdata = data + (xinc>>3);
                     for (dx=sx; dx < ex; dx++) {
                        if (*xdata & table[xpos++]) bitmap->DrawUCPixel(bitmap, dx, dy, colour);
                        if (xpos > 7) {
                           xpos = 0;
                           xdata++;
                        }
                     }
                     data += bytewidth;
                  }
               }
            }
         }

         dxcoord += charwidth * Self->GlyphSpacing;
      }
   } // while (not str.empty())

   // Draw an underline for the current line if underlining is turned on

   if (Self->Underline.Alpha > 0) {
      if ((Self->Flags & FTF::BASE_LINE) != FTF::NIL) sy = dycoord;
      else sy = dycoord + Self->Height + Self->Leading + 1;
      gfx::DrawRectangle(bitmap, startx, sy, dxcoord-startx, ((Self->Flags & FTF::HEAVY_LINE) != FTF::NIL) ? 2 : 1, ucolour, BAF::FILL);
   }

   Self->EndX = dxcoord;
   Self->EndY = dycoord + Self->Leading;

   acUnlock(bitmap);

   return error;
}

//********************************************************************************************************************

#include "class_font_def.c"

static const FieldArray clFontFields[] = {
   { "Point",        FDF_DOUBLE|FDF_RW|FDF_PURE, GET_Point, SET_Point },
   { "GlyphSpacing", FDF_DOUBLE|FDF_RW },
   { "Bitmap",       FDF_OBJECT|FDF_RW, nullptr, nullptr, CLASSID::BITMAP },
   { "String",       FDF_CPPSTRING|FDF_RW, nullptr, SET_String },
   { "Path",         FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "Style",        FDF_CPPSTRING|FDF_RI, nullptr, SET_Style },
   { "Face",         FDF_CPPSTRING|FDF_RI, nullptr, SET_Face },
   { "Outline",      FDF_ARRAY|FD_BYTE|FDF_RW, nullptr, nullptr, 4 },
   { "Underline",    FDF_ARRAY|FD_BYTE|FDF_RW, nullptr, nullptr, 4 },
   { "Colour",       FDF_ARRAY|FD_BYTE|FDF_RW, nullptr, nullptr, 4 },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, SET_Flags, clFontFlags },
   { "Gutter",       FDF_INT|FDF_RI },
   { "LineSpacing",  FDF_INT|FDF_RW },
   { "X",            FDF_INT|FDF_RW },
   { "Y",            FDF_INT|FDF_RW },
   { "TabSize",      FDF_INT|FDF_RW },
   { "WrapEdge",     FDF_INT|FDF_RW },
   { "FixedWidth",   FDF_INT|FDF_RW },
   { "Height",       FDF_INT|FDF_RI },
   { "Leading",      FDF_INT|FDF_R },
   { "MaxHeight",    FDF_INT|FDF_RI },
   { "Align",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, clFontAlign },
   { "AlignWidth",   FDF_INT|FDF_RW },
   { "AlignHeight",  FDF_INT|FDF_RW },
   { "Ascent",       FDF_INT|FDF_R },
   { "EndX",         FDF_INT|FDF_RW },
   { "EndY",         FDF_INT|FDF_RW },
   // Virtual fields
   { "Bold",         FDF_VIRTUAL|FDF_INT|FDF_RW, GET_Bold, SET_Bold },
   { "Italic",       FDF_VIRTUAL|FDF_INT|FDF_RW, GET_Italic, SET_Italic },
   { "LineCount",    FDF_VIRTUAL|FDF_INT|FDF_R, GET_LineCount },
   { "Location",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_SYNONYM|FDF_RW|FDF_PURE, GET_Location, SET_Path },
   { "Opacity",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, GET_Opacity, SET_Opacity },
   { "Width",        FDF_VIRTUAL|FDF_INT|FDF_R, GET_Width },
   { "YOffset",      FDF_VIRTUAL|FDF_INT|FDF_R, GET_YOffset },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_font_class(void)
{
   clFont = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FONT),
      fl::ClassVersion(VER_FONT),
      fl::Name("Font"),
      fl::Category(CCF::GRAPHICS),
      fl::FileExtension("font|fnt|ttf|fon"),
      fl::FileDescription("Font"),
      fl::Icon("filetypes/font"),
      fl::Actions(clFontActions),
      fl::Fields(clFontFields),
      fl::Size(sizeof(extFont)),
      fl::Path(MOD_PATH));

   return clFont ? ERR::Okay : ERR::AddClass;
}
