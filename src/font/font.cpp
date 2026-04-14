/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

This code utilises the work of the FreeType Project under the FreeType License.  For more information please refer to
the FreeType home page at www.freetype.org.

**********************************************************************************************************************

-MODULE-
Font: Provides font management functionality and hosts the Font class.

The Font module is responsible for managing the font database and provides support for client queries.  Fixed size
bitmap fonts are supported via the Windows .fon file format, while Truetype fonts are support for scalable fonts.

Bitmap fonts can be opened and drawn by the @Font class.  Drawing Truetype fonts is not supported by the Font module,
but is instead provided by the #Vector module and @VectorText class.

For a thorough introduction to typesetting history and terminology as it applies to computing, we recommend visiting
Google Fonts Knowledge page: https://fonts.google.com/knowledge

-END-

*********************************************************************************************************************/

#define PRV_FONT
#define PRV_FONT_MODULE
//#define DEBUG

#include <ft2build.h>
#include FT_SIZES_H
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_ADVANCES_H
#include FT_SFNT_NAMES_H

#undef FT_INT64  // Avoid Freetype clash

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/display.h>

#include <sstream>
#include <array>
#include <math.h>
#include <wchar.h>
#include <kotuku/strings.hpp>
#include "../link/unicode.h"

using namespace pf;

//********************************************************************************************************************
// This table determines what ASCII characters are treated as white-space for word-wrapping purposes.  You'll need to
// refer to an ASCII table to see what is going on here.

static const uint8_t glWrapBreaks[256] = {
   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0x0f
   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 0x1f
   1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, // 0x2f
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, // 0x3f
   1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x4f
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, // 0x5f
   1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x6f
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, // 0x7f
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x8f
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x9f
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xaf
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xbf
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xcf
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xdf
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0xef
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0  // 0xff
};

//********************************************************************************************************************

OBJECTPTR modFont = nullptr;

JUMPTABLE_DISPLAY
JUMPTABLE_CORE

static OBJECTPTR clFont = nullptr;
static OBJECTPTR modDisplay = nullptr;
static FT_Library glFTLibrary = nullptr;

#include "font_structs.h"

class extFont : public objFont {
   public:
   uint8_t *prvData;
   std::string prvBuffer;
   struct FontCharacter *prvChar;
   class BitmapCache *BmpCache;
   int prvLineCount;
   int prvStrWidth;
   int16_t prvBitmapHeight;
   int16_t prvLineCountCR;
   char prvEscape[2];
   char prvFace[32];
   char prvStyle[20];
   uint8_t prvDefaultChar;
};

#include "font_def.c"

#include "font_bitmap.cpp"

static ERR add_font_class(void);
static int getutf8(CSTRING, uint32_t *);
static void scan_truetype_folder(objConfig *);
static void scan_fixed_folder(objConfig *);
static ERR analyse_bmp_font(CSTRING, winfnt_header_fields *, std::string &, std::vector<uint16_t> &);
static void string_size(extFont *, CSTRING, int, int, int *, int *);

//********************************************************************************************************************
// Return the first unicode value from a given string address.

static int getutf8(CSTRING Value, uint32_t *Unicode)
{
   int i, len, code;

   if ((*Value & 0x80) != 0x80) {
      if (Unicode) *Unicode = *Value;
      return 1;
   }
   else if ((*Value & 0xe0) IS 0xc0) {
      len  = 2;
      code = *Value & 0x1f;
   }
   else if ((*Value & 0xf0) IS 0xe0) {
      len  = 3;
      code = *Value & 0x0f;
   }
   else if ((*Value & 0xf8) IS 0xf0) {
      len  = 4;
      code = *Value & 0x07;
   }
   else if ((*Value & 0xfc) IS 0xf8) {
      len  = 5;
      code = *Value & 0x03;
   }
   else if ((*Value & 0xfc) IS 0xfc) {
      len  = 6;
      code = *Value & 0x01;
   }
   else {
      // Unprintable character
      if (Unicode) *Unicode = 0;
      return 1;
   }

   for (i=1; i < len; ++i) {
      if (!Value[i] or ((Value[i] & 0xc0) != 0x80)) {
         code = -1;
         break;
      }
      code <<= 6;
      code |= Value[i] & 0x3f;
   }

   if (code IS -1) {
      if (Unicode) *Unicode = 0;
      return 1;
   }
   else {
      if (Unicode) *Unicode = code;
      return len;
   }
}

//********************************************************************************************************************
// Returns the global point size for font scaling.  This is set to 10 by default, but the user can change the setting
// in the interface style values.

static double glDefaultPoint = 10;
static bool glPointSet = false;

static double global_point_size(void)
{
   if (!glPointSet) {
      pf::Log log(__FUNCTION__);
      OBJECTID style_id;
      if (FindObject("glStyle", CLASSID::XML, FOF::NIL, &style_id) IS ERR::Okay) {
         pf::ScopedObjectLock<objXML> style(style_id, 3000);
         if (style.granted()) {
            char pointsize[20];
            glPointSet = true;
            if (acGetKey(style.obj, "/interface/@fontsize", pointsize, sizeof(pointsize)) IS ERR::Okay) {
               glDefaultPoint = strtod(pointsize, nullptr);
               if (glDefaultPoint < 6) glDefaultPoint = 6;
               else if (glDefaultPoint > 80) glDefaultPoint = 80;
               log.msg("Global font size is %.1f.", glDefaultPoint);
            }
         }
      }
      else log.warning("glStyle XML object is not available");
   }

   return glDefaultPoint;
}

//********************************************************************************************************************

inline void calc_lines(extFont *Self)
{
   if (Self->String) {
      if (Self->WrapEdge > 0) {
         string_size(Self, Self->String, -1, Self->WrapEdge - Self->X, nullptr, &Self->prvLineCount);
      }
      else Self->prvLineCount = Self->prvLineCountCR;
   }
   else Self->prvLineCount = 1;
}

//********************************************************************************************************************

static void string_size(extFont *Font, CSTRING String, int Chars, int Wrap, int *Width, int *Rows)
{
   uint32_t unicode;
   int16_t rowcount, wordwidth, lastword, tabwidth, charwidth;
   uint8_t line_abort, pchar;

   if ((!Font) or (!String)) return;
   if (!Font->initialised()) return;

   if (Chars IS FSS_LINE) {
      Chars = 0x7fffffff;
      line_abort = 1;
   }
   else {
      line_abort = 0;
      if (Chars < 0) Chars = 0x7fffffff;
   }

   if (Wrap <= 0) Wrap = 0x7fffffff;

   //log.msg("StringSize: %.10s, Wrap %d, Chars %d, Abort: %d", String, Wrap, Chars, line_abort);

   CSTRING start  = String;
   int x         = 0;
   int prevglyph = 0;
   int longest   = 0;
   int charcount = 0;
   int wordindex = 0;
   rowcount = line_abort ? 0 : 1;
   while ((*String) and (charcount < Chars)) {
      lastword = x;

      // Skip whitespace

      while ((*String) and (*String <= 0x20)) {
         if (*String IS ' ') x += Font->prvChar[' '].Advance * Font->GlyphSpacing;
         else if (*String IS '\t') {
            tabwidth = (Font->prvChar[' '].Advance * Font->GlyphSpacing) * Font->TabSize;
            if (tabwidth) x += pf::roundup(x, tabwidth);
         }
         else if (*String IS '\n') {
            if (lastword > longest) longest = lastword;
            x = 0;
            if (line_abort) {
               line_abort = 2;
               String++;
               break;
            }
            rowcount++;
         }
         String++;
         charcount++;
         prevglyph = 0;
      }

      if ((!*String) or (line_abort IS 2)) break;

      // Calculate the width of the discovered word

      wordindex = int(String - start);
      wordwidth = 0;
      charwidth = 0;

      while (charcount < Chars) {
         int charlen = getutf8(String, &unicode);

         if (Font->FixedWidth > 0) charwidth = Font->FixedWidth;
         else if (unicode < 256) charwidth = Font->prvChar[unicode].Advance * Font->GlyphSpacing;
         else charwidth = Font->prvChar[(int)Font->prvDefaultChar].Advance * Font->GlyphSpacing;

         if ((!x) and (x+wordwidth+charwidth >= Wrap)) {
            // This is the first word of the line and it exceeds the boundary, so we have to split it.

            lastword = wordwidth;
            wordwidth += charwidth; // This is just to ensure that a break occurs
            wordindex = (int)(String - start);
            break;
         }
         else {
            pchar = glWrapBreaks[(uint8_t)(*String)];
            wordwidth += charwidth;
            String += charlen;
            charcount++;

            // Break if the previous char was a wrap character or current char is whitespace.

            if ((pchar) or (*String <= 0x20)) break;
         }
      }

      // Check the width of the word against the wrap boundary

      if (x + wordwidth >= Wrap) {
         prevglyph = 0;
         if (lastword > longest) longest = lastword;
         rowcount++;
         if (line_abort) {
            x = 0;
            String = start + wordindex;
            break;
         }
         else x = wordwidth;
      }
      else x += wordwidth;
   }

   if (x > longest) longest = x;

   if (Rows) {
      if (line_abort) *Rows = int(String - start);
      else *Rows = rowcount;
   }

   if (Width) *Width = longest;
}

//********************************************************************************************************************

static objConfig *glConfig = nullptr; // Font database

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   pf::Log log;

   CoreBase = argCoreBase;

   argModule->get(FID_Root, modFont);

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::LoadModule;

   if (FT_Init_FreeType(&glFTLibrary)) return log.warning(ERR::LoadModule);

   LOC type;
   bool refresh = (AnalysePath("fonts:fonts.cfg", &type) != ERR::Okay) or (type != LOC::FILE);

   if ((glConfig = objConfig::create::global(fl::Name("cfgSystemFonts"), fl::Path("fonts:fonts.cfg")))) {
      if (refresh) fnt::RefreshFonts();

      ConfigGroups *groups;
      if (not ((glConfig->get(FID_Data, groups) IS ERR::Okay) and (!groups->empty()))) {
         log.error("Failed to build a database of valid fonts.");
         return ERR::Failed;
      }

      // Merge tailored font options into the machine-generated database

      glConfig->mergeFile("fonts:options.cfg");
   }
   else {
      log.error("Failed to load or prepare the font configuration file.");
      return ERR::Failed;
   }

   return add_font_class();
}

static ERR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, glFunctions);
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   if (glCacheTimer) { UpdateTimer(glCacheTimer, 0);  glCacheTimer = nullptr; }
   if (glFTLibrary)  { FT_Done_FreeType(glFTLibrary); glFTLibrary  = nullptr; }
   if (glConfig)     { FreeResource(glConfig);        glConfig     = nullptr; }
   if (clFont)       { FreeResource(clFont);          clFont       = nullptr; }
   if (modDisplay)   { FreeResource(modDisplay);      modDisplay   = nullptr; }
   glBitmapCache.clear();
   return ERR::Okay;
}

namespace fnt {

/*********************************************************************************************************************

-FUNCTION-
CharWidth: Returns the width of a character.

This function will return the pixel width of a bitmap font character.  The character is specified as a unicode value
in the Char parameter.

The font's GlyphSpacing value is not used in calculating the character width.

-INPUT-
obj(Font) Font: The font to use for calculating the character width.
uint Char: A unicode character.

-RESULT-
int: The pixel width of the character will be returned.

*********************************************************************************************************************/

int CharWidth(objFont *Font, uint32_t Char)
{
   auto font = (extFont *)Font;
   if (Font->FixedWidth > 0) return Font->FixedWidth;
   else if ((Char < 256) and (font->prvChar)) return font->prvChar[Char].Advance;
   else return font->prvChar ? font->prvChar[(int)font->prvDefaultChar].Advance : 0;
}

/*********************************************************************************************************************

-FUNCTION-
GetList: Returns a list of all available system fonts.

This function returns a linked list of all available system fonts.  The list must be terminated once it is no longer
required.

-INPUT-
&!struct(FontList) Result: The font list is returned here.

-ERRORS-
Okay
NullArgs
AccessObject: Access to the font database was denied, or the object does not exist.

*********************************************************************************************************************/

ERR GetList(FontList **Result)
{
   pf::Log log(__FUNCTION__);

   if (!Result) return log.warning(ERR::NullArgs);

   log.branch();

   *Result = NULL;

   pf::ScopedObjectLock<objConfig> config(glConfig, 3000);
   if (!config.granted()) return log.warning(ERR::AccessObject);

   size_t size = 0;
   ConfigGroups *groups;
   if (glConfig->get(FID_Data, groups) IS ERR::Okay) {
      for (auto & [group, keys] : groups[0]) {
         size += sizeof(FontList) + keys["Name"].size() + 1 + keys["Styles"].size() + 1 + (keys["Points"].size()*4) + 1;
         if (keys.contains("Alias")) size += keys["Alias"].size() + 1;
         if (keys.contains("Axes")) size += keys["Axes"].size() + 1;
      }

      FontList *list, *last_list = nullptr;
      if (AllocMemory(size, MEM::DATA, &list) IS ERR::Okay) {
         auto buffer = (STRING)(list + groups->size());
         *Result = list;

         for (auto & [group, keys] : groups[0]) {
            last_list = list;
            list->Next = list + 1;

            if (keys.contains("Name")) {
               list->Name = buffer;
               buffer += strcopy(keys["Name"], buffer) + 1;
            }

            if (keys.contains("Hidden")) {
               if (iequals("Yes", keys["Hidden"])) list->Hidden = true;
            }

            auto it = keys.find("Alias");
            if ((it != keys.end()) and (!it->second.empty())) {
               list->Alias = buffer;
               buffer += strcopy(keys["Alias"], buffer) + 1;
               // An aliased font can define a Name and Hidden values only.
            }
            else {
               if (keys.contains("Styles")) {
                  list->Styles = buffer;
                  buffer += strcopy(keys["Styles"], buffer) + 1;
               }

               if (keys.contains("Scalable")) {
                  if (iequals("Yes", keys["Scalable"])) list->Scalable = true;
               }

               if (keys.contains("Variable")) {
                  if (iequals("Yes", keys["Variable"])) list->Variable = true;
               }

               if (keys.contains("Hinting")) {
                  if (iequals("Normal", keys["Hinting"])) list->Hinting = HINT::NORMAL;
                  else if (iequals("Internal", keys["Hinting"])) list->Hinting = HINT::INTERNAL;
                  else if (iequals("Light", keys["Hinting"])) list->Hinting = HINT::LIGHT;
               }

               if (keys.contains("Axes")) {
                  list->Axes = buffer;
                  buffer += strcopy(keys["Axes"], buffer) + 1;
               }

               list->Points = nullptr;
               if (keys.contains("Points")) {
                  auto fontpoints = std::string_view(keys["Points"]);
                  if (!fontpoints.empty()) {
                     list->Points = (int *)buffer;
                     std::size_t i = 0;
                     for (int16_t j=0; i != std::string::npos; j++) {
                        ((int *)buffer)[0] = svtonum<int>(fontpoints);
                        buffer += sizeof(int);
                        if (i = fontpoints.find(','); i != std::string::npos) fontpoints.remove_prefix(i+1);
                     }
                     ((int *)buffer)[0] = 0;
                     buffer += sizeof(int);
                  }
               }
            }

            list++;
         }

         if (last_list) last_list->Next = nullptr;

         return ERR::Okay;
      }
      else return ERR::AllocMemory;
   }
   else return ERR::NoData;
}

/*********************************************************************************************************************

-FUNCTION-
StringWidth: Returns the pixel width of any given string in relation to a font's settings.

This function calculates the pixel width of any string in relation to a font's object definition.  The routine takes
into account any line feeds that might be specified in the String, so if the String contains 8 lines, then the width
of the longest line will be returned.

Word wrapping will not be taken into account, even if it has been enabled in the font object.

-INPUT-
obj(Font) Font: An initialised font object.
cstr String: The string to be calculated.
int Chars: The number of characters (not bytes, so consider UTF-8 serialisation) to be used in calculating the string length, or -1 to use the entire string.

-RESULT-
int: The pixel width of the string is returned - this will be zero if there was an error or the string is empty.

*********************************************************************************************************************/

int StringWidth(objFont *Font, CSTRING String, int Chars)
{
   if ((!Font) or (!String)) return 0;
   if (!Font->initialised()) return 0;

   auto font = (extFont *)Font;
   CSTRING str = String;
   if (Chars < 0) Chars = 0x7fffffff;

   int len    = 0;
   int widest = 0;
   int whitespace = 0;
   while ((*str) and (Chars > 0)) {
      if (*str IS '\n') {
         if (widest < len) widest = len - whitespace;
         len  = 0; // Reset
         str++;
         Chars--;
         whitespace = 0;
      }
      else if (*str IS '\t') {
         int16_t tabwidth = (font->prvChar[' '].Advance * Font->GlyphSpacing) * Font->TabSize;
         if (tabwidth) len = pf::roundup(len, tabwidth);
         str++;
         Chars--;
         whitespace = 0;
      }
      else {
         uint32_t unicode;
         str += getutf8(str, &unicode);
         Chars--;

         int advance;
         if (Font->FixedWidth > 0) advance = Font->FixedWidth;
         else if ((unicode < 256) and (font->prvChar) and (font->prvChar[unicode].Advance)) {
            advance = font->prvChar[unicode].Advance;
         }
         else advance = font->prvChar[(int)font->prvDefaultChar].Advance;

         int final_advance = advance * Font->GlyphSpacing;
         len += final_advance;
         whitespace = final_advance - advance;
      }
   }

   if (widest > len) return widest;
   else return len - whitespace;
}

/*********************************************************************************************************************

-FUNCTION-
SelectFont: Searches for a 'best fitting' font file, based on family name and style.

This function resolves a font family Name and Style to a font file path.  It works on a best efforts basis; the Name
must exist but the Style is a non-mandatory preference.

The resulting Path must be freed once it is no longer required.

-INPUT-
cstr Name:  The name of a font face to search for (case insensitive).
cstr Style: The required style, e.g. Bold or Italic.  Using camel-case for each word is compulsory.
&!cstr Path: The location of the best-matching font file is returned in this parameter.
&int(FMETA) Meta: Optional, returns additional meta information about the font file.

-ERRORS-
Okay
NullArgs
AccessObject: Access to the font database was denied, or the object does not exist.
Search: Unable to find a suitable font.
-END-

*********************************************************************************************************************/

ERR SelectFont(CSTRING Name, CSTRING Style, CSTRING *Path, FMETA *Meta)
{
   pf::Log log(__FUNCTION__);

   log.branch("%s:%s", Name, Style);

   if (not Name) return log.warning(ERR::NullArgs);

   pf::ScopedObjectLock<objConfig> config(glConfig, 5000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   ConfigGroups *groups;
   if (glConfig->get(FID_Data, groups) != ERR::Okay) return ERR::Search;

   auto get_meta = [](ConfigKeys &Group) {
      auto meta = FMETA::NIL;
      if (Group.contains("Hinting")) {
         if (iequals("Normal", Group["Hinting"])) meta |= FMETA::HINT_NORMAL;
         else if (iequals("Internal", Group["Hinting"])) meta |= FMETA::HINT_INTERNAL;
         else if (iequals("Light", Group["Hinting"])) meta |= FMETA::HINT_LIGHT;
      }

      if (Group.contains("Variable")) meta |= FMETA::VARIABLE;
      if (Group.contains("Scalable")) meta |= FMETA::SCALED;
      if (Group.contains("Hidden"))   meta |= FMETA::HIDDEN;
      return meta;
   };

   auto get_font_path = [](ConfigKeys &Keys, const std::string &Style) {
      if (Keys.contains(Style)) return strclone(Keys[Style]);
      else if (!iequals("Regular", Style)) {
         if (Keys.contains("Regular")) return strclone(Keys["Regular"]);
      }
      return STRING(nullptr);
   };

   std::string style_name(Style);
   pf::camelcase(style_name);

   for (auto & [group, keys] : groups[0]) {
      if (!iequals(Name, keys["Name"])) continue;

      if ((*Path = get_font_path(keys, style_name))) {
         if (Meta) *Meta = get_meta(keys);
         return ERR::Okay;
      }

      log.traceWarning("Requested style '%s' not available, choosing first style.", style_name.c_str());

      std::string styles = keys.contains("Styles") ? keys["Styles"] : "Regular";
      auto end = styles.find(",");
      if (end IS std::string::npos) end = styles.size();
      std::string first_style = styles.substr(0, end);

      if (keys.contains(first_style)) {
         *Path = strclone(keys[first_style]);
         if (Meta) *Meta = get_meta(keys);
         return ERR::Okay;
      }
      else return ERR::Search;
   }

   log.warning("The \"%s\" font is not available.", Name);
   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
RefreshFonts: Refreshes the system font list with up-to-date font information.

This function scans the `fonts:` volume and refreshes the font database.

Refreshing fonts can take an extensive amount of time as each font file needs to be completely analysed for
information.  The `fonts:fonts.cfg` file will be re-written on completion to reflect current font settings.

-ERRORS-
Okay: Fonts were successfully refreshed.
AccessObject: Access to the font database was denied, or the object does not exist.
-END-

*********************************************************************************************************************/

ERR RefreshFonts(void)
{
   pf::Log log(__FUNCTION__);

   log.branch();

   pf::ScopedObjectLock<objConfig> config(glConfig, 3000);
   if (!config.granted()) return log.warning(ERR::AccessObject);

   acClear(glConfig); // Clear out existing font information

   scan_fixed_folder(glConfig);
   scan_truetype_folder(glConfig);

   glConfig->sortByKey(nullptr, false); // Sort the font names into alphabetical order

   // Create a style list for each font, e.g.
   //
   //    Bold Italic = fonts:fixed/courier.fon
   //    Bold = fonts:truetype/Courier Prime Bold.ttf
   //    Styles = Bold,Bold Italic,Italic,Regular

   ConfigGroups *groups;
   if (glConfig->get(FID_Data, groups) IS ERR::Okay) {
      for (auto & [group, keys] : *groups) {
         std::list <std::string> styles;
         for (auto & [k, v] : keys) {
            if (!v.compare(0, 6, "fonts:")) styles.push_front(k);
         }

         styles.sort();
         std::ostringstream style_list;
         for (int i=0; not styles.empty(); i++) {
            if (i) style_list << ",";
            style_list << styles.front();
            styles.pop_front();
         }

         keys["Styles"] = style_list.str();
      }
   }

   // Save the font configuration file

   objFile::create file = { fl::Path("fonts:fonts.cfg"), fl::Flags(FL::NEW|FL::WRITE) };
   if (file.ok()) glConfig->saveToObject(*file);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ResolveFamilyName: Convert a CSV family string to a single family name.

Use ResolveFamilyName() to convert complex CSV family strings to a single family name.  The provided String will be
parsed in sequence, with priority given from left to right.  If a single asterisk is used to terminate the list, it is
guaranteed that the system default will be returned if no valid match is made.

It is valid for individual names to utilise the common wildcards `?` and `*` to make a match.  E.g. `Times New *`
would be able to match to `Times New Roman` if available.

-INPUT-
cstr String: A CSV family string to resolve.
&cstr Result: The nominated family name is returned in this parameter.

-ERRORS-
Okay:
NullArgs:
AccessObject: Access to the font database was denied, or the object does not exist.
GetField:
Search: It was not possible to resolve the String to a known font family.
-END-

*********************************************************************************************************************/

ERR ResolveFamilyName(CSTRING String, CSTRING *Result)
{
   pf::Log log(__FUNCTION__);

   if ((!String) or (!Result)) return ERR::NullArgs;

   pf::ScopedObjectLock<objConfig> config(glConfig, 5000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   ConfigGroups *groups;
   if (glConfig->get(FID_Data, groups) != ERR::Okay) return log.warning(ERR::GetField);

   std::vector<std::string> names;
   pf::split(std::string(String), std::back_inserter(names));

   for (auto &name : names) {
      pf::ltrim(name, "'\"");
      pf::rtrim(name, "'\"");

restart:
      if ((name[0] IS '*') and (!name[1])) {
         // Default family requested - use the first font declaring a "Default" key
         for (auto & [group, keys] : groups[0]) {
            if (keys.contains("Default")) {
               *Result = keys["Name"].c_str();
               return ERR::Okay;
            }
         }

         *Result = "Noto Sans";
         return ERR::Okay;
      }

      for (auto & [group, keys] : groups[0]) {
         if (pf::wildcmp(name, keys["Name"])) {
            if (auto it = keys.find("Alias"); it != keys.end()) {
               const std::string &alias = it->second;
               if (!alias.empty()) {
                  name = alias;
                  goto restart;
               }
            }

            *Result = keys["Name"].c_str();
            return ERR::Okay;
         }
      }
   }

   log.msg("Failed to resolve family \"%s\"", String);
   return ERR::Search;
}

} // namespace

//********************************************************************************************************************

static void scan_truetype_folder(objConfig *Config)
{
   pf::Log log(__FUNCTION__);

   log.branch("Scanning for truetype fonts.");

   std::string ttpath;
   if (ResolvePath("fonts:truetype/", RSF::NO_FILE_CHECK|RSF::PATH, &ttpath) IS ERR::Okay) {
      DirInfo *dir;
      if (OpenDir(ttpath.c_str(), RDF::FILE, &dir) IS ERR::Okay) {
         LocalResource free_dir(dir);

         auto ttpath_len = ttpath.size();
         while (ScanDir(dir) IS ERR::Okay) {
            ttpath.resize(ttpath_len);
            ttpath.append(dir->Info->Name);

            FT_Face ftface;
            FT_Open_Args open = { .flags = FT_OPEN_PATHNAME, .pathname = (FT_String *)ttpath.c_str() };
            if (not FT_Open_Face(glFTLibrary, &open, 0, &ftface)) {
               if (!FT_IS_SCALABLE(ftface)) { // Sanity check
                  FT_Done_Face(ftface);
                  continue;
               }

               log.msg("Detected font file \"%s\", name: %s, style: %s", ttpath.c_str(), ftface->family_name, ftface->style_name);

               std::string group;
               if (ftface->family_name) group.assign(ftface->family_name);
               else {
                  unsigned i;
                  for (i=0; dir->Info->Name[i] and (dir->Info->Name[i] != '.'); i++);
                  group.assign(dir->Info->Name, i);
               }

               // Strip any style references out of the font name and keep them as style flags

               FTF style = FTF::NIL;
               if (ftface->style_name) {
                  if (auto pos = group.find(" Bold"); pos != std::string::npos) {
                     group.replace(pos, 5, "");
                     style |= FTF::BOLD;
                  }

                  if (auto pos = group.find(" Italic"); pos != std::string::npos) {
                     group.replace(pos, 7, "");
                     style |= FTF::ITALIC;
                  }
               }

               pf::rtrim(group);

               Config->write(group.c_str(), "Name", group);
               Config->write(group.c_str(), "Scalable", "Yes");

               if (FT_HAS_MULTIPLE_MASTERS(ftface)) {
                  // A single ttf file can contain multiple named styles
                  Config->write(group.c_str(), "Variable", "Yes");

                  FT_MM_Var *mvar;
                  if (!FT_Get_MM_Var(ftface, &mvar)) {
                     FT_UInt index;
                     if (!FT_Get_Default_Named_Instance(ftface, &index)) {
                        char buffer[100];
                        auto name_table_size = FT_Get_Sfnt_Name_Count(ftface);
                        for (FT_UInt s=0; (s < mvar->num_namedstyles); s++) {
                           for (int n=int(name_table_size)-1; n >= 0; n--) {
                              FT_SfntName sft_name;
                              if (!FT_Get_Sfnt_Name(ftface, n, &sft_name)) {
                                 if (sft_name.name_id IS mvar->namedstyle[s].strid) {
                                    // Decode UTF16 Big Endian
                                    int out = 0;
                                    auto str = (uint16_t *)sft_name.string;
                                    uint16_t prev_unicode = 0;
                                    for (FT_UInt i=0; (i < sft_name.string_len>>1) and (out < std::ssize(buffer)-8); i++) {
                                       uint16_t unicode = (str[i]>>8) | (uint8_t(str[i])<<8);
                                       if ((unicode >= 'A') and (unicode <= 'Z')) {
                                          if ((i > 0) and (prev_unicode >= 'a') and (prev_unicode <= 'z')) {
                                             buffer[out++] = ' ';
                                          }
                                       }
                                       out += UTF8WriteValue(unicode, buffer+out, std::ssize(buffer)-out);
                                       prev_unicode = unicode;
                                    }
                                    buffer[out] = 0;

                                    std::string path("fonts:truetype/");
                                    path.append(dir->Info->Name);
                                    Config->write(group.c_str(), buffer, path);
                                    break;
                                 }
                              }
                           }
                        }

                        std::ostringstream axes;
                        for (unsigned a=0; a < mvar->num_axis; a++) {
                           if (a > 0) axes << ',';
                           auto tag = (unsigned char *)&mvar->axis[a].tag;
                           axes << tag[3] << tag[2] << tag[1] << tag[0];
                        }
                        Config->write(group.c_str(), "Axes", axes.str());
                     }

                     FT_Done_MM_Var(glFTLibrary, mvar);
                  }
               }
               else {
                  // Add the style with a link to the font file location

                  std::string path("fonts:truetype/");
                  path.append(dir->Info->Name);

                  if ((ftface->style_name) and (!iequals("regular", ftface->style_name))) {
                     Config->write(group.c_str(), ftface->style_name, path);
                  }
                  else {
                     if (style IS FTF::BOLD) Config->write(group.c_str(), "Bold", path);
                     else if (style IS FTF::ITALIC) Config->write(group.c_str(), "Italic", path);
                     else if (style IS (FTF::BOLD|FTF::ITALIC)) Config->write(group.c_str(), "Bold Italic", path);
                     else Config->write(group.c_str(), "Regular", path);
                  }
               }

               FT_Done_Face(ftface);
            }
         }
      }
      else log.warning("Failed to open the fonts:truetype/ directory.");
   }
}

//********************************************************************************************************************

static void scan_fixed_folder(objConfig *Config)
{
   pf::Log log(__FUNCTION__);

   log.branch("Scanning for fixed fonts.");

   DirInfo *dir;
   if (OpenDir("fonts:fixed/", RDF::FILE, &dir) IS ERR::Okay) {
      while (ScanDir(dir) IS ERR::Okay) {
         std::string location("fonts:fixed/");
         location.append(dir->Info->Name);
         auto src = location.c_str();

         winfnt_header_fields header;
         std::vector<uint16_t> points;
         std::string facename;
         if (analyse_bmp_font(src, &header, facename, points) IS ERR::Okay) {
            log.detail("Detected font file \"%s\", name: %s", src, facename.c_str());

            if (facename.empty()) continue;
            std::string group(facename);

            // Strip any style references out of the font name and keep them as style flags

            FTF style = FTF::NIL;

            {
               auto n = group.find(" Bold");
               if (n != std::string::npos) {
                  group.erase(n, 5);
                  style |= FTF::BOLD;
               }
            }

            {
               auto n = group.find(" Italic");
               if (n != std::string::npos) {
                  group.erase(n, 7);
                  style |= FTF::ITALIC;
               }
            }

            if (header.italic) style |= FTF::ITALIC;
            if (header.weight >= 600) style |= FTF::BOLD;

            {
               auto n = group.length();
               while ((n > 0) and (group[n-1] <= 0x20)) n--;
               if (n != group.length()) group.resize(n);
            }

            auto gs = group.c_str();
            Config->write(gs, "Name", gs);

            // Add the style with a link to the font file location

            if (style IS FTF::BOLD) Config->write(gs, "Bold", src);
            else if (style IS FTF::ITALIC) Config->write(gs, "Italic", src);
            else if (style IS (FTF::BOLD|FTF::ITALIC)) Config->write(gs, "Bold Italic", src);
            else {
               // Font is regular, which also means we can convert it to bold/italic with some code
               Config->write(gs, "Regular", src);
               Config->write(gs, "Bold", src);
               Config->write(gs, "Bold Italic", src);
               Config->write(gs, "Italic", src);
            }

            std::ostringstream out;
            bool comma = false;
            for (auto &point : points) {
               if (comma) out << ',';
               else comma = true;
               out << point;
            }

            Config->write(gs, "Points", out.str());
         }
         else log.warning("Failed to analyse %s", src);
      }
      FreeResource(dir);
   }
   else log.warning("Failed to scan directory fonts:fixed/");
}

//********************************************************************************************************************

static ERR analyse_bmp_font(CSTRING Path, winfnt_header_fields *Header, std::string &FaceName, std::vector<uint16_t> &Points)
{
   pf::Log log(__FUNCTION__);
   winmz_header_fields mz_header;
   winne_header_fields ne_header;
   int i, res_offset, font_offset;
   uint16_t size_shift, font_count, count;
   char face[50];

   if ((!Path) or (!Header)) return ERR::NullArgs;

   objFile::create file = { fl::Path(Path), fl::Flags(FL::READ) };
   if (file.ok()) {
      file->read(&mz_header, sizeof(mz_header));

      if (mz_header.magic IS ID_WINMZ) {
         file->seekStart(mz_header.lfanew);

         if ((file->read(&ne_header, sizeof(ne_header)) IS ERR::Okay) and (ne_header.magic IS ID_WINNE)) {
            res_offset = mz_header.lfanew + ne_header.resource_tab_offset;
            file->seekStart(res_offset);

            font_count  = 0;
            font_offset = 0;
            size_shift  = 0;
            fl::ReadLE(*file, &size_shift);

            uint16_t type_id = 0;
            for (fl::ReadLE(*file, &type_id); type_id; fl::ReadLE(*file, &type_id)) {
               if (fl::ReadLE(*file, &count) IS ERR::Okay) {
                  if (type_id IS 0x8008) {
                     font_count  = count;
                     file->get(FID_Position, font_offset);
                     font_offset = font_offset + 4;
                     break;
                  }

                  file->seekCurrent(4 + count * 12);
               }
            }

            if ((!font_count) or (!font_offset)) {
               log.warning("There are no fonts in file \"%s\"", Path);
               return ERR::Failed;
            }

            file->seekStart(font_offset);

            {
               auto fonts = std::make_unique<winFont[]>(font_count);

               // Get the offset and size of each font entry

               for (int i=0; i < font_count; i++) {
                  uint16_t offset = 0, size = 0;
                  fl::ReadLE(*file, &offset);
                  fl::ReadLE(*file, &size);
                  fonts[i].Offset = offset<<size_shift;
                  fonts[i].Size   = size<<size_shift;
                  file->seekCurrent(8);
               }

               // Read font point sizes

               for (i=0; i < font_count; i++) {
                  file->seekStart(fonts[i].Offset);
                  if (file->read(Header, sizeof(winfnt_header_fields)) IS ERR::Okay) {
                     Points.push_back(Header->nominal_point_size);
                  }
               }

               // Go to the first font in the file and read the font header

               file->seekStart(fonts[0].Offset);

               if (file->read(Header, sizeof(winfnt_header_fields)) != ERR::Okay) {
                  return ERR::Read;
               }

                // NOTE: 0x100 indicates the Microsoft vector font format, which we do not support.

               if ((Header->version != 0x200) and (Header->version != 0x300)) {
                  log.warning("Font \"%s\" is written in unsupported version %d / $%x.", Path, Header->version, Header->version);
                  return ERR::NoSupport;
               }

               if (Header->file_type & 1) {
                  log.warning("Font \"%s\" is in the non-supported vector font format.", Path);
                  return ERR::NoSupport;
               }

               // Extract the name of the font

               file->seekStart(fonts[0].Offset + Header->face_name_offset);

               for (i=0; (size_t)i < sizeof(face)-1; i++) {
                  ERR result = file->read(face+i, 1);
                  if ((result != ERR::Okay) or (!face[i])) break;
               }
               face[i] = 0;
               FaceName = face;
            }

            return ERR::Okay;
         }
         else return ERR::NoSupport;
      }
      else return ERR::NoSupport;
   }
   else return ERR::File;
}

//********************************************************************************************************************

#include "class_font.cpp"

//********************************************************************************************************************

static STRUCTS glStructures = {
   { "FontList", sizeof(FontList) }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_font_module() { return &ModHeader; }
