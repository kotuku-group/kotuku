/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Font: Draws bitmap fonts and manages font meta information.

The Font class is provided for the purpose of bitmap font rendering and querying font meta information.  It supports
styles such as bold, italic and underlined text, along with extra features such as adjustable spacing and word
alignment. Fixed-point bitmap fonts are supported through the Windows .fon file format.  Truetype fonts are
not supported (refer to the @VectorText class for this feature).

Bitmap fonts must be stored in the `fonts:fixed/` directory in order to be recognised.  The process of font
installation and file management is managed by functions supplied in the Font module.

The Font class includes full support for the unicode character set through its support for UTF-8.  This gives you the
added benefit of being able to support international character sets with ease, but you must be careful not to use
character codes above #127 without being sure that they follow UTF-8 guidelines.  Find out more about UTF-8 at
this <a href="http://www.cl.cam.ac.uk/~mgk25/unicode.html">web page</a>.

Initialisation of a new font object can be as simple as declaring its #Point size and #Face name.  Font objects can
be difficult to alter post-initialisation, so all style and graphical selections must be defined on creation.  For
example, it is not possible to change styling from regular to bold format dynamically.  To support multiple styles
of the same font, create a font object for every style that requires support.  Basic settings such as colour, the
font string and text positioning are not affected by these limitations.

To draw a font string to a Bitmap object, start by setting the #Bitmap and #String fields.  The #X and #Y fields
determine string positioning and you can also use the #Align field to position a string to the right or center of the
surface area.

To clarify the terminology used in this documentation, please note the definitions for the following terms:

<list type="bullet">
<li>'Point' determines the size of a font.  The value is relative only to other point sizes of the same font face, i.e. two faces at the same point size are not necessarily the same height.</li>
<li>'Height' represents the 'vertical bearing' or point of the font, expressed as a pixel value.  The height does not cover for any leading at the top of the font, or the gutter space used for the tails on characters like 'g' and 'y'.</li>
<li>'Gutter' is the amount of space that a character can descend below the base line.  Characters like 'g' and 'y' are examples of characters that utilise the gutter space.  The gutter is also sometimes known as the 'external leading' or 'descent' of a character.</li>
<li>'LineSpacing' is the recommended pixel distance between each line that is printed with the font.</li>
<li>'Glyph' refers to a single font character.</li>
</>

Please note that in the majority of cases the @VectorText class should be used for drawing strings because the Font
class is not integrated with the display's vector scene graph.

-END-

*********************************************************************************************************************/

static BitmapCache * check_bitmap_cache(extFont *, FTF);
static ERR SET_Point(extFont *Self, double);
static ERR SET_Style(extFont *, CSTRING);

/*********************************************************************************************************************

-ACTION-
Draw: Draws a font to a Bitmap.

Draws a font to a target Bitmap, starting at the coordinates of #X and #Y, using the characters in the font #String.

-ERRORS-
Okay
FieldNotSet: The Bitmap and/or String field has not been set.
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
            pf::SwitchContext ctx(modFont);
            SubscribeTimer(60.0, C_FUNCTION(bitmap_cache_cleaner), &glCacheTimer);
         }
      }
   }

   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }
   Self->~extFont();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR FONT_Init(extFont *Self)
{
   pf::Log log;
   int diff;
   FTF style;
   ERR error;
   FMETA meta = FMETA::NIL;

   if ((!Self->prvFace[0]) and (!Self->Path)) return log.warning(ERR::FieldNotSet);

   if (!Self->Point) Self->Point = global_point_size();

   if (!Self->Path) {
      CSTRING path = nullptr;
      if (fnt::SelectFont(Self->prvFace, Self->prvStyle, &path, &meta) IS ERR::Okay) {
         Self->set(FID_Path, path);
         FreeResource(path);
      }
      else {
         log.warning("Font \"%s\" (point %.2f, style %s) is not recognised.", Self->prvFace, Self->Point, Self->prvStyle);
         return ERR::Failed;
      }
   }

   // Check the bitmap cache to see if we have already loaded this font

   if (iequals("Bold", Self->prvStyle)) style = FTF::BOLD;
   else if (iequals("Italic", Self->prvStyle)) style = FTF::ITALIC;
   else if (iequals("Bold Italic", Self->prvStyle)) style = FTF::BOLD|FTF::ITALIC;
   else style = FTF::NIL;

   CACHE_LOCK lock(glCacheMutex);

   BitmapCache *cache = check_bitmap_cache(Self, style);

   if (cache); // The font exists in the cache
   else if (wildcmp("*.ttf", Self->Path)) return ERR::NoSupport;
   else {
      objFile::create file = { fl::Path(Self->Path), fl::Flags(FL::READ|FL::APPROXIMATE) };
      if (file.ok()) {
         // Check if the file is a Windows Bitmap Font

         winmz_header_fields mz_header;
         file->read(&mz_header, sizeof(mz_header));

         if (mz_header.magic IS ID_WINMZ) {
            file->seek(mz_header.lfanew, SEEK::START);

            winne_header_fields ne_header;
            if ((file->read(&ne_header, sizeof(ne_header)) IS ERR::Okay) and (ne_header.magic IS ID_WINNE)) {
               uint32_t res_offset = mz_header.lfanew + ne_header.resource_tab_offset;
               file->seek(res_offset, SEEK::START);

               // Count the number of fonts in the file

               int16_t size_shift = 0;
               uint16_t font_count = 0;
               int font_offset = 0;
               fl::ReadLE(*file, &size_shift);

               int16_t type_id;
               for ((error = fl::ReadLE(*file, &type_id)); (error IS ERR::Okay) and (type_id); error = fl::ReadLE(*file, &type_id)) {
                  int16_t count = 0;
                  fl::ReadLE(*file, &count);

                  if ((uint16_t)type_id IS 0x8008) {
                     font_count  = count;
                     file->get(FID_Position, font_offset);
                     font_offset += 4;
                     break;
                  }

                  file->seek(4 + (count * 12), SEEK::CURRENT);
               }

               if ((!font_count) or (!font_offset)) { // There are no fonts in the file
                  return log.warning(ERR::NoData);
               }

               file->seek(font_offset, SEEK::START);

               // Scan the list of available fonts to find the closest point size for our font

               auto fonts = std::make_unique<winFont[]>(font_count);

               for (int i=0; i < font_count; i++) {
                  uint16_t offset, size;
                  fl::ReadLE(*file, &offset);
                  fl::ReadLE(*file, &size);
                  fonts[i].Offset = offset<<size_shift;
                  fonts[i].Size   = size<<size_shift;
                  file->seek(8, SEEK::CURRENT);
               }

               int abs = 0x7fff;
               int wfi = 0;
               winfnt_header_fields face;
               for (int i=0; i < font_count; i++) {
                  file->seek((double)fonts[i].Offset, SEEK::START);

                  winfnt_header_fields header;
                  if (file->read(&header, sizeof(header)) IS ERR::Okay) {
                     if ((header.version != 0x200) and (header.version != 0x300)) {
                        // Font is written in an unsupported version
                        return log.warning(ERR::NoSupport);
                     }

                     if (header.file_type & 1) { // Font is in the non-supported vector font format."
                        return log.warning(ERR::NoSupport);
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
                  auto it = glBitmapCache.emplace(glBitmapCache.end(), face, Self->prvStyle, Self->Path, *file, fonts[wfi]);

                  if (it->Result IS ERR::Okay) cache = &(*it);
                  else {
                     ERR error = it->Result;
                     glBitmapCache.erase(it);
                     return error;
                  }
               }
            } // File is not a windows fixed font
         } // File is not a windows fixed font
      }
      else return log.warning(ERR::OpenFile);
   }

   if (cache) {
      Self->prvData     = cache->mData;
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

   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }

   log.detail("Family: %s, Style: %s, Point: %.2f, Height: %d", Self->prvFace, Self->prvStyle, Self->Point, Self->Height);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR FONT_NewPlacement(extFont *Self)
{
   new (Self) extFont;
   Self->TabSize         = 8;
   Self->prvDefaultChar  = '.';
   Self->prvLineCountCR  = 1;
   Self->Style           = Self->prvStyle;
   Self->Face            = Self->prvFace;
   Self->Colour.Alpha    = 255;
   Self->GlyphSpacing    = 1.0;
   strcopy("Regular", Self->prvStyle, sizeof(Self->prvStyle));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Align: Sets the position of a font string to an abstract alignment.

Use this field to set the alignment of a font string within its surface area.  This is an abstract means of positioning
in comparison to setting the #X and #Y fields directly.

-FIELD-
AlignHeight: The height to use when aligning the font string.

If the `VERTICAL` or `TOP` alignment options are used in the #Align field, the AlignHeight should be set so
that the alignment of the font string can be correctly calculated.  If the AlignHeight is not defined, the target
#Bitmap's height will be used when computing alignment.

-FIELD-
AlignWidth: The width to use when aligning the font string.

If the `HORIZONTAL` or `RIGHT` alignment options are used in the #Align field, the AlignWidth should be set so
that the alignment of the font string can be correctly calculated.  If the AlignWidth is not defined, the target
#Bitmap's width will be used when computing alignment.

-FIELD-
Ascent: The total number of pixels above the baseline.

The Ascent value reflects the total number of pixels above the baseline, including the #Leading value.

-FIELD-
Bitmap: The destination Bitmap to use when drawing a font.

-FIELD-
Bold: Set to `true` to enable bold styling.

Setting the Bold field to `true` prior to initialisation will enable bold styling.  This field is provided only for
convenience - we recommend that you set the Style field for determining font styling where possible.

*********************************************************************************************************************/

static ERR GET_Bold(extFont *Self, int *Value)
{
   if ((Self->Flags & FTF::BOLD) != FTF::NIL) *Value = TRUE;
   else if (pf::strisearch("bold", Self->prvStyle) != -1) *Value = TRUE;
   else *Value = FALSE;
   return ERR::Okay;
}

static ERR SET_Bold(extFont *Self, int Value)
{
   if (Self->initialised()) {
      // If the font is initialised, setting the bold style is implicit
      return SET_Style(Self, "Bold");
   }
   else if ((Self->Flags & FTF::ITALIC) != FTF::NIL) {
      return SET_Style(Self, "Bold Italic");
   }
   else return SET_Style(Self, "Bold");
}

/*********************************************************************************************************************

-FIELD-
Colour: The font colour in !RGB8 format.

-FIELD-
EndX: Indicates the final horizontal coordinate after completing a draw operation.

The EndX and #EndY fields reflect the final coordinate of the font #String, following the most recent call to
the #Draw() action.

-FIELD-
EndY: Indicates the final vertical coordinate after completing a draw operation.

The #EndX and EndY fields reflect the final coordinate of the font #String, following the most recent call to
the #Draw() action.

-FIELD-
Face: The name of a font face that is to be loaded on initialisation.

The name of an installed font face must be specified here for initialisation.  If this field is undefined then the
initialisation process will use the user's preferred face.  A list of available faces can be obtained from the
~Font.GetList() function.

For convenience, the face string can also be extended with extra parameters so that the point size and style are
defined at the same time.  Extra parameters are delimited with the colon character and must follow a set order
defined as `face:pointsize:style:colour`.

Here are some examples:

<pre>
Noto Sans:12:Bold Italic:#ff0000
Courier:10.6
Charter:120%::255,128,255
</pre>

Multiple font faces can be specified in CSV format, e.g. `Sans Serif,Noto Sans`, which allows the closest matching
font to be selected if the first face is unavailable or unable to match the requested point size.

*********************************************************************************************************************/

static ERR SET_Face(extFont *Self, STRING Value)
{
   if ((Value) and (Value[0])) {
      CSTRING final_name;
      if (fnt::ResolveFamilyName(Value, &final_name) IS ERR::Okay) {
         strcopy(final_name, Self->prvFace, std::ssize(Self->prvFace));
      }

      int i, j;
      for (i=0; Value[i] and Value[i] != ':'; i++);
      if (!Value[i]) return ERR::Okay;

      // Extract the point size

      Value += i;
      double pt = strtod(Value, &Value);
      SET_Point(Self, pt);

      i = 0;
      while ((*Value) and (*Value != ':')) Value++;
      if (!*Value) return ERR::Okay;

      // Extract the style string

      i++;
      for (j=0; (Value[i]) and (Value[i] != ':') and (j < std::ssize(Self->prvStyle)-1); j++) Self->prvStyle[j] = Value[i++];
      Self->prvStyle[j] = 0;

      if (Value[i] != ':') return ERR::Okay;

      // Extract the colour string

      i++;
      Self->set(FID_Colour, Value + i);
   }
   else Self->prvFace[0] = 0;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FixedWidth: Forces a fixed pixel width to use for all glyphs.

The FixedWidth value imposes a preset pixel width for all glyphs in the font.  It is important to note that if the
fixed width value is less than the widest glyph, the glyphs will overlap each other.

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

This field reflects the 'external leading' value (also known as the 'gutter'), measured in pixels.

-FIELD-
Height: The point size of the font, expressed in pixels.

The point size of the font is expressed in this field as a pixel measurement.  It does not not include the leading value
(refer to #Ascent if leading is required).

The height is calculated on initialisation and can be read at any time.

-FIELD-
GlyphSpacing: Adjusts the amount of spacing between each character.

This field adjusts the horizontal spacing between each glyph, technically known as kerning between each font
character.  The value is expressed as a multiplier of the width of each character, and defaults to `1.0`.

Using negative values is valid, and can lead to text being printed backwards.

-FIELD-
Italic: Set to `true` to enable italic styling.

Setting the Italic field to `true` prior to initialisation will enable italic styling.  This field is provided for
convenience only - we recommend that you set the #Style field for determining font styling where possible.

*********************************************************************************************************************/

static ERR GET_Italic(extFont *Self, int *Value)
{
   if ((Self->Flags & FTF::ITALIC) != FTF::NIL) *Value = TRUE;
   else if (pf::strisearch("italic", Self->prvStyle) != -1) *Value = TRUE;
   else *Value = FALSE;
   return ERR::Okay;
}

static ERR SET_Italic(extFont *Self, int Value)
{
   if (Self->initialised()) {
      // If the font is initialised, setting the italic style is implicit
      return SET_Style(Self, "Italic");
   }
   else if ((Self->Flags & FTF::BOLD) != FTF::NIL) {
      return SET_Style(Self, "Bold Italic");
   }
   else return SET_Style(Self, "Italic");
}

/*********************************************************************************************************************

-FIELD-
Leading: 'Internal leading' measured in pixels.  Applies to fixed fonts only.

-FIELD-
LineCount: The total number of lines in a font string.

This field indicates the number of lines that are present in a #font's String field.  If word wrapping is enabled,
this will be taken into account in the resulting figure.

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

This field defines the amount of space between each line that is printed with a font object.  It is set automatically
during initialisation to reflect the recommended distance between each line.   The client can increase or decrease
this value to make finer adjustments to the line spacing.  If negative, the text will be printed in a reverse vertical
direction with each new line.

If set prior to initialisation, the value will be added to the font's normal line-spacing instead of over-riding it.
For instance, setting the LineSpacing to 2 will result in an extra 2 pixels being added to the font's spacing.

-FIELD-
Path: The path to a font file.

This field can be defined prior to initialisation.  It can be used to refer to the exact location of a font data file,
in opposition to the normal practice of loading fonts that are installed on the host system.

This feature is ideal for use when distributing custom fonts with an application.

*********************************************************************************************************************/

static ERR SET_Path(extFont *Self, CSTRING Value)
{
   if (!Self->initialised()) {
      if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }
      if (Value) Self->Path = strclone(Value);
      return ERR::Okay;
   }
   else return ERR::Failed;
}

/*********************************************************************************************************************

-FIELD-
MaxHeight: The maximum possible pixel height per character.

This field reflects the maximum possible pixel height per character, covering the entire character set at the current
point size.

-FIELD-
Opacity: Determines the level of translucency applied to a font.

This field determines the translucency level of a font graphic.  The default setting is 100%, which means that the font
will not be translucent.  Any other value set here will alter the impact of a font's graphic over the destination
#Bitmap.  High values will retain the boldness of the font, while low values can render it close to invisible.

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

An outline can be drawn around a font by setting the Outline field to an !RGB8 colour.  The outline can be turned off by
writing this field with a `NULL` value or setting the alpha component to zero.  Outlining is currently supported for
bitmap fonts only.

-FIELD-
Point: The point size of a font.

The point size defines the size of a font in point units, at a ratio of 1:72 DPI.

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

The String field must be defined in order to draw text with a font object.  A string must consist of a valid sequence
of UTF-8 characters.  Line feeds are allowed (whenever a line feed is reached, the Draw() action will start printing on
the next line).  Drawing will stop when the null termination character is reached.

If a string contains characters that are not supported by a font, those characters will be printed using a default
character from the font.

*********************************************************************************************************************/

static ERR SET_String(extFont *Self, CSTRING Value)
{
   if ((Value) and (Self->String) and (std::string_view(Value) IS Self->String)) return ERR::Okay;

   Self->prvLineCount = 0;
   Self->prvStrWidth  = 0; // Reset the string width for GET_Width
   Self->prvLineCountCR = 1; // Line count (carriage returns only)

   if ((Value) and (*Value)) {
      int i;
      for (i=0; Value[i]; i++) if (Value[i] IS '\n') Self->prvLineCountCR++;

      Self->prvBuffer.assign(Value);
      Self->String = (STRING)Self->prvBuffer.c_str();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Style: Determines font styling.

The style of a font can be selected by setting the Style field.  This comes into effect only if the font actually
supports the specified style as part of its graphics set.  If the style is unsupported, the regular styling of the
face will be used on initialisation.

Bitmap fonts are a special case if a bold or italic style is selected.  In this situation the system can automatically
convert the font to that style even if the correct graphics set does not exist.

Conventional font styles are `Bold`, `Bold Italic`, `Italic` and `Regular` (the default).

*********************************************************************************************************************/

static ERR SET_Style(extFont *Self, CSTRING Value)
{
   if ((!Value) or (!Value[0])) strcopy("Regular", Self->prvStyle, sizeof(Self->prvStyle));
   else strcopy(Value, Self->prvStyle, sizeof(Self->prvStyle));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TabSize: Defines the tab size to use when drawing and manipulating a font string.

The TabSize value controls the interval between tabs, measured in characters.

The default tab size is 8 and the TabSize only comes into effect when tab characters are used in the font #String.

-FIELD-
Underline: Enables font underlining when set.

To underline a font string, set the Underline field to the colour that should be used to draw the underline.
Underlining can be turned off by writing this field with a `NULL` value or setting the alpha component to zero.

-FIELD-
Width: Returns the pixel width of a string.

Read this virtual field to obtain the pixel width of a font string.  You must have already set a string in the font for
this to work, or a width of zero will be returned.

*********************************************************************************************************************/

static ERR GET_Width(extFont *Self, int *Value)
{
   if (!Self->String) {
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

Word wrapping is enabled by setting the WrapEdge field to a value greater than zero.  Wrapping occurs whenever the
right-most edge of any word in the font string extends past the coordinate indicated by the WrapEdge.

-FIELD-
X: The starting horizontal position when drawing the font string.

When drawing font strings, the X and #Y fields define the position that the string will be drawn to in the target
surface.  The default coordinates are `(0, 0)`.

-FIELD-
Y: The starting vertical position when drawing the font string.

When drawing font strings, the #X and Y fields define the position that the string will be drawn to in the target
surface.  The default coordinates are `(0, 0)`.

-FIELD-
YOffset: Additional offset value that is added to vertically aligned fonts.

Fonts that are aligned vertically (either in the center or bottom edge of the drawing area) will have a vertical offset
value.  Reading that value from this field and adding it to the Y field will give you an accurate reading of where
the string will be drawn.
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

static ERR draw_bitmap_font(extFont *Self)
{
   pf::Log log(__FUNCTION__);
   objBitmap *bitmap;
   RGB8 rgb;
   static const uint8_t table[] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
   uint8_t *xdata, *data;
   int linewidth, offset, charclip, wrapindex, charlen;
   uint32_t unicode, ocolour;
   int16_t startx, xpos, ex, ey, sx, sy, xinc;
   int16_t bytewidth, alpha, charwidth;
   bool draw_line;
   #define CHECK_LINE_CLIP(font,y,bmp) if (((y)-1 < (bmp)->Clip.Bottom) and ((y) + (font)->prvBitmapHeight + 1 > (bmp)->Clip.Top)) draw_line = true; else draw_line = false;

   // Validate settings for fixed font type

   if (!(bitmap = Self->Bitmap)) { log.warning("The Bitmap field is not set."); return ERR::FieldNotSet; }
   if (!Self->String) return ERR::FieldNotSet;
   if (!Self->String[0]) return ERR::Okay;

   ERR error = ERR::Okay;
   STRING str = Self->String;
   int dxcoord = Self->X;
   int dycoord = Self->Y;

   if (!Self->AlignWidth)  Self->AlignWidth  = bitmap->Width;
   if (!Self->AlignHeight) Self->AlignHeight = bitmap->Height;

   GET_YOffset(Self, &offset);
   dycoord = dycoord + offset - Self->Leading;

   if ((Self->Flags & FTF::BASE_LINE) != FTF::NIL) {
      dycoord -= (Self->Ascent - Self->Leading); // - 1;
   }

   string_size(Self, str, FSS_LINE, (Self->WrapEdge > 0) ? (Self->WrapEdge - Self->X) : 0, &linewidth, &wrapindex);
   CSTRING wrapstr = str + wrapindex;

   // If horizontal centering is required, calculate the correct horizontal starting coordinate.

   if ((Self->Align & (ALIGN::HORIZONTAL|ALIGN::RIGHT)) != ALIGN::NIL) {
      if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) {
         dxcoord = Self->X + ((Self->AlignWidth - linewidth)>>1);
      }
      else dxcoord = Self->X + Self->AlignWidth - linewidth;
   }

   uint32_t colour  = bitmap->getColour(Self->Colour);
   uint32_t ucolour = bitmap->getColour(Self->Underline);

   if (Self->Outline.Alpha > 0) {
      Self->BmpCache->get_outline();
      ocolour = bitmap->getColour(Self->Outline);
   }
   else ocolour = 0;

   charclip = Self->WrapEdge - 8; //(Self->prvChar['.'].Advance<<2); //8

   if (acLock(bitmap) != ERR::Okay) return log.warning(ERR::Lock);

   int16_t dx = 0, dy = 0;
   startx = dxcoord;
   CHECK_LINE_CLIP(Self, dycoord, bitmap);
   while (*str) {
      if (*str IS '\n') { // Reset the font to a new line
         if (Self->Underline.Alpha > 0) {
            gfx::DrawRectangle(bitmap, startx, dycoord + Self->Height + 1, dxcoord-startx, ((Self->Flags & FTF::HEAVY_LINE) != FTF::NIL) ? 2 : 1, ucolour, BAF::FILL);
         }

         str++;

         while ((*str) and (*str <= 0x20)) { if (*str IS '\n') dycoord += Self->LineSpacing; str++; }
         string_size(Self, str, FSS_LINE, (Self->WrapEdge > 0) ? (Self->WrapEdge - Self->X) : 0, &linewidth, &wrapindex);
         wrapstr = str + wrapindex;

         if ((Self->Align & (ALIGN::HORIZONTAL|ALIGN::RIGHT)) != ALIGN::NIL) {
            if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) dxcoord = Self->X + ((Self->AlignWidth - linewidth)>>1);
            else dxcoord = Self->X + Self->AlignWidth - linewidth;
         }
         else dxcoord = Self->X;

         startx = dxcoord;
         dycoord += Self->LineSpacing;
         CHECK_LINE_CLIP(Self, dycoord, bitmap);
      }
      else if (*str IS '\t') {
         int16_t tabwidth = (Self->prvChar['o'].Advance * Self->GlyphSpacing) * Self->TabSize;
         if (tabwidth) dxcoord = Self->X + pf::roundup(dxcoord - Self->X, tabwidth);
         str++;
      }
      else {
         charlen = getutf8(str, &unicode);

         if ((unicode > 255) or (!Self->prvChar) or (!Self->prvChar[unicode].Advance)) {
            unicode = Self->prvDefaultChar;
         }

         if (Self->FixedWidth > 0) charwidth = Self->FixedWidth;
         else charwidth = Self->prvChar[unicode].Advance;

         // Wordwrap management

         if (str >= wrapstr) {
            dxcoord = Self->X;
            dycoord += Self->LineSpacing;

            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') dycoord += Self->LineSpacing; str++; }
            string_size(Self, str, FSS_LINE, Self->WrapEdge - dxcoord, &linewidth, &wrapindex);
            wrapstr = str + wrapindex;

            if ((Self->Align & (ALIGN::HORIZONTAL|ALIGN::RIGHT)) != ALIGN::NIL) {
               if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) dxcoord = Self->X + ((Self->AlignWidth - linewidth)>>1);
               else dxcoord = Self->X + Self->AlignWidth - linewidth;
            }
            CHECK_LINE_CLIP(Self, dycoord, bitmap);
         }

         str += charlen;

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
   } // while (*str)

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

static const FieldDef AlignFlags[] = {
   { "Right",      ALIGN::RIGHT      }, { "Left",     ALIGN::LEFT },
   { "Bottom",     ALIGN::BOTTOM     }, { "Top",      ALIGN::TOP },
   { "Horizontal", ALIGN::HORIZONTAL }, { "Vertical", ALIGN::VERTICAL },
   { "Center",     ALIGN::CENTER     }, { "Middle",   ALIGN::MIDDLE },
   { nullptr, 0 }
};

static const FieldArray clFontFields[] = {
   { "Point",        FDF_DOUBLE|FDF_RW, GET_Point, SET_Point },
   { "GlyphSpacing", FDF_DOUBLE|FDF_RW },
   { "Bitmap",       FDF_OBJECT|FDF_RW, nullptr, nullptr, CLASSID::BITMAP },
   { "String",       FDF_STRING|FDF_RW, nullptr, SET_String },
   { "Path",         FDF_STRING|FDF_RW, nullptr, SET_Path },
   { "Style",        FDF_STRING|FDF_RI, nullptr, SET_Style },
   { "Face",         FDF_STRING|FDF_RI, nullptr, SET_Face },
   { "Outline",      FDF_RGB|FDF_RW },
   { "Underline",    FDF_RGB|FDF_RW },
   { "Colour",       FDF_RGB|FDF_RW },
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
   { "Align",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, AlignFlags },
   { "AlignWidth",   FDF_INT|FDF_RW },
   { "AlignHeight",  FDF_INT|FDF_RW },
   { "Ascent",       FDF_INT|FDF_R },
   { "EndX",         FDF_INT|FDF_RW },
   { "EndY",         FDF_INT|FDF_RW },
   // Virtual fields
   { "Bold",         FDF_VIRTUAL|FDF_INT|FDF_RW, GET_Bold, SET_Bold },
   { "Italic",       FDF_VIRTUAL|FDF_INT|FDF_RW, GET_Italic, SET_Italic },
   { "LineCount",    FDF_VIRTUAL|FDF_INT|FDF_R, GET_LineCount },
   { "Location",     FDF_VIRTUAL|FDF_STRING|FDF_SYNONYM|FDF_RW, nullptr, SET_Path },
   { "Opacity",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, GET_Opacity, SET_Opacity },
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
      fl::FileExtension("*.font|*.fnt|*.ttf|*.fon"),
      fl::FileDescription("Font"),
      fl::Icon("filetypes/font"),
      fl::Actions(clFontActions),
      fl::Fields(clFontFields),
      fl::Size(sizeof(extFont)),
      fl::Path(MOD_PATH));

   return clFont ? ERR::Okay : ERR::AddClass;
}
