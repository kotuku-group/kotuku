/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

This code utilises the work of the FreeType Project under the FreeType License.  For more information please refer to
the FreeType home page at www.freetype.org.

**********************************************************************************************************************

-MODULE-
Font: Provides font management functionality and hosts the Font class.

The Font module maintains the system font database and provides query functions for resolving font family names,
styles, file paths and metadata.  Fixed-size bitmap fonts are recognised through the Windows `.fon` format, while
TrueType fonts are scanned as scalable font resources.

Bitmap fonts can be opened and drawn directly by the @Font class.  Scalable TrueType rendering is handled by the
Vector module and @VectorText class; the Font module supplies the database information that allows those fonts to be
selected.

For a thorough introduction to typesetting history and terminology as it applies to computing, we recommend visiting
Google Fonts Knowledge page: https://fonts.google.com/knowledge

-END-

*********************************************************************************************************************/

#define PRV_FONT
#define PRV_FONT_MODULE
//#define DEBUG

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/display.h>

#include <sstream>
#include <kotuku/strings.hpp>
#include "../link/unicode.h"

using namespace kt;

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
   struct FontCharacter *prvChar;
   class BitmapCache *BmpCache;
   int prvLineCount;
   int prvStrWidth;
   int16_t prvBitmapHeight;
   int16_t prvLineCountCR;
   char prvEscape[2];
   uint8_t prvDefaultChar;

   extFont() {
      TabSize         = 8;
      prvDefaultChar  = '.';
      prvLineCountCR  = 1;
      Colour.Alpha    = 255;
      GlyphSpacing    = 1.0;
      Style           = "Regular";
   }
};

#include "font_def.c"

#include "font_bitmap.cpp"

static ERR add_font_class(void);
static void scan_truetype_folder(objConfig *);
static void scan_fixed_folder(objConfig *);
static ERR analyse_bmp_font(std::string_view, winfnt_header_fields *, std::string &, std::vector<uint16_t> &);
static void string_size(extFont *, std::string_view, int, int, int *, int *);

//********************************************************************************************************************
// Return the first unicode value from a given string address.

static int getutf8(CSTRING Value, uint32_t *Unicode) = delete;

static int getutf8(std::string_view Value, uint32_t *Unicode)
{
   int i, len, code;

   if (Value.empty()) {
      if (Unicode) *Unicode = 0;
      return 0;
   }

   const auto first = uint8_t(Value[0]);
   if ((first & 0x80) != 0x80) {
      if (Unicode) *Unicode = first;
      return 1;
   }
   else if ((first & 0xe0) IS 0xc0) {
      len  = 2;
      code = first & 0x1f;
   }
   else if ((first & 0xf0) IS 0xe0) {
      len  = 3;
      code = first & 0x0f;
   }
   else if ((first & 0xf8) IS 0xf0) {
      len  = 4;
      code = first & 0x07;
   }
   else if ((first & 0xfc) IS 0xf8) {
      len  = 5;
      code = first & 0x03;
   }
   else if ((first & 0xfc) IS 0xfc) {
      len  = 6;
      code = first & 0x01;
   }
   else {
      // Unprintable character
      if (Unicode) *Unicode = 0;
      return 1;
   }

   for (i=1; i < len; ++i) {
      if ((size_t(i) >= Value.size()) or ((uint8_t(Value[i]) & 0xc0) != 0x80)) {
         code = -1;
         break;
      }
      code <<= 6;
      code |= uint8_t(Value[i]) & 0x3f;
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

template <class T>
constexpr T tab_advance(T Num, T Alignment) {
   return ((Num + Alignment) / Alignment) * Alignment;
}

//********************************************************************************************************************
// Returns the global point size for font scaling.  This is set to 10 by default, but the user can change the setting
// in the interface style values.

static double glDefaultPoint = 10;
static bool glPointSet = false;
static std::mutex glPointMutex;

static double global_point_size(void)
{
   const std::lock_guard<std::mutex> lock(glPointMutex);

   if (not glPointSet) {
      kt::Log log(__FUNCTION__);
      OBJECTID style_id;
      if (FindObject("glStyle", CLASSID::XML, &style_id) IS ERR::Okay) {
         kt::ScopedObjectLock<objXML> style(style_id, 3000);
         if (style.granted()) {
            std::string pointsize;
            if (acGetKey(style.obj, "/interface/@fontsize", pointsize) IS ERR::Okay) {
               glDefaultPoint = strtod(pointsize.c_str(), nullptr);
               if (glDefaultPoint < 6) glDefaultPoint = 6;
               else if (glDefaultPoint > 80) glDefaultPoint = 80;
               log.msg("Global font size is %.1f.", glDefaultPoint);
            }
            glPointSet = true;
         }
      }
      else log.warning("glStyle XML object is not available");
   }

   return glDefaultPoint;
}

//********************************************************************************************************************

inline void calc_lines(extFont *Self)
{
   if (not Self->String.empty()) {
      if (Self->WrapEdge > 0) {
         string_size(Self, Self->String, -1, Self->WrapEdge - Self->X, nullptr, &Self->prvLineCount);
      }
      else Self->prvLineCount = Self->prvLineCountCR;
   }
   else Self->prvLineCount = 1;
}

//********************************************************************************************************************
// For use by calc_lines()

static void string_size(extFont *Font, std::string_view String, int Chars, int Wrap, int *Width, int *Rows)
{
   uint32_t unicode;
   int16_t rowcount, wordwidth, lastword, tabwidth, charwidth;
   uint8_t line_abort, pchar;

   if ((not Font) or String.empty()) return;
   if (not Font->initialised()) return;

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

   int x         = 0;
   int longest   = 0;
   int charcount = 0;
   int wordindex = 0;
   size_t cursor = 0;
   rowcount = line_abort ? 0 : 1;
   while ((cursor < String.size()) and (charcount < Chars)) {
      lastword = x;

      // Skip whitespace

      while ((cursor < String.size()) and (uint8_t(String[cursor]) <= 0x20)) {
         auto ch = uint8_t(String[cursor]);
         if (ch IS ' ') x += Font->prvChar[' '].Advance * Font->GlyphSpacing;
         else if (ch IS '\t') {
            tabwidth = (Font->prvChar[' '].Advance * Font->GlyphSpacing) * Font->TabSize;
            if (tabwidth) x = tab_advance<int>(x, tabwidth);
         }
         else if (ch IS '\n') {
            if (lastword > longest) longest = lastword;
            x = 0;
            if (line_abort) {
               line_abort = 2;
               cursor++;
               break;
            }
            rowcount++;
         }
         cursor++;
         charcount++;
      }

      if ((cursor >= String.size()) or (line_abort IS 2)) break;

      // Calculate the width of the discovered word

      wordindex = int(cursor);
      wordwidth = 0;
      charwidth = 0;

      while ((cursor < String.size()) and (charcount < Chars)) {
         int charlen = getutf8(String.substr(cursor), &unicode);
         if (charlen <= 0) break;

         if (Font->FixedWidth > 0) charwidth = Font->FixedWidth;
         else if (unicode < 256) charwidth = Font->prvChar[unicode].Advance * Font->GlyphSpacing;
         else charwidth = Font->prvChar[(int)Font->prvDefaultChar].Advance * Font->GlyphSpacing;

         if ((not x) and (x + wordwidth + charwidth >= Wrap)) {
            // This is the first word of the line and it exceeds the boundary, so we have to split it.

            lastword = wordwidth;
            wordwidth += charwidth; // This is just to ensure that a break occurs
            wordindex = int(cursor);
            break;
         }
         else {
            pchar = glWrapBreaks[uint8_t(String[cursor])];
            wordwidth += charwidth;
            cursor += size_t(charlen);
            charcount++;

            // Break if the previous char was a wrap character or current char is whitespace.

            if ((pchar) or (cursor >= String.size()) or (uint8_t(String[cursor]) <= 0x20)) break;
         }
      }

      // Check the width of the word against the wrap boundary

      if (x + wordwidth >= Wrap) {
         if (lastword > longest) longest = lastword;
         rowcount++;
         if (line_abort) {
            x = 0;
            cursor = size_t(wordindex);
            break;
         }
         else x = wordwidth;
      }
      else x += wordwidth;
   }

   if (x > longest) longest = x;

   if (Rows) {
      if (line_abort) *Rows = int(cursor);
      else *Rows = rowcount;
   }

   if (Width) *Width = longest;
}

//********************************************************************************************************************

static objConfig *glConfig = nullptr; // Font database

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log;

   CoreBase = argCoreBase;

   argModule->get(FID_Root, modFont);

   auto cleanup = [] {
      if (glFTLibrary) { FT_Done_FreeType(glFTLibrary); glFTLibrary = nullptr; }
      if (glConfig)    { FreeResource(glConfig);        glConfig    = nullptr; }
      if (clFont)      { FreeResource(clFont);          clFont      = nullptr; }
      if (modDisplay)  { FreeResource(modDisplay);      modDisplay  = nullptr; }
   };

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::LoadModule;

   if (FT_Init_FreeType(&glFTLibrary)) {
      cleanup();
      return log.warning(ERR::LoadModule);
   }

   LOC type;
   bool refresh = (AnalysePath("fonts:fonts.cfg", &type) != ERR::Okay) or (type != LOC::FILE);

   if ((glConfig = objConfig::create::global(fl::Name("cfgSystemFonts"), fl::Path("fonts:fonts.cfg")))) {
      if (refresh) {
         if (auto error = fnt::RefreshFonts(); error != ERR::Okay) {
            cleanup();
            return error;
         }
      }

      ConfigGroups *groups;
      if (not ((glConfig->get(FID_Data, groups) IS ERR::Okay) and (not groups->empty()))) {
         log.error("Failed to build a database of valid fonts.");
         cleanup();
         return ERR::Failed;
      }

      // Merge tailored font options into the machine-generated database

      glConfig->mergeFile("fonts:options.cfg");
   }
   else {
      log.error("Failed to load or prepare the font configuration file.");
      cleanup();
      return ERR::Failed;
   }

   if (auto error = add_font_class(); error != ERR::Okay) {
      cleanup();
      return error;
   }

   return ERR::Okay;
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

Returns the pixel width of a bitmap font character.  `Char` is interpreted as a Unicode character value.  Bitmap fonts
provide a maximum of 256 glyph slots; unsupported characters fall back to the font's default character.

The font's #GlyphSpacing value is not included in the returned width.  If `Font` is `NULL` or has no character table,
the result is `0`.

-INPUT-
obj(Font) Font: The font to use for calculating the character width.
uint Char: A Unicode character value.

-RESULT-
int: The pixel width of the character, or `0` if it cannot be measured.

-TAGS-
pure-query

*********************************************************************************************************************/

int CharWidth(objFont *Font, uint32_t Char)
{
   if (not Font) return 0;

   auto font = (extFont *)Font;
   if (Font->FixedWidth > 0) return Font->FixedWidth;
   else if ((Char < 256) and (font->prvChar)) return font->prvChar[Char].Advance;
   else return font->prvChar ? font->prvChar[(int)font->prvDefaultChar].Advance : 0;
}

/*********************************************************************************************************************

-FUNCTION-
GetList: Returns a list of all available system fonts.

Returns a linked list of available system fonts.  The returned list is allocated as a single resource and must be
released with `FreeResource()` when it is no longer required.

-INPUT-
&!struct(FontList) Result: The font list is returned here.

-ERRORS-
Okay
NullArgs
AccessObject: Access to the font database was denied, or the object does not exist.
AllocMemory
NoData

-TAGS-
caller-owns-result, creates-resource, blocking

*********************************************************************************************************************/

ERR GetList(FontList **Result)
{
   kt::Log log(__FUNCTION__);

   if (not Result) return log.warning(ERR::NullArgs);

   log.branch();

   *Result = NULL;

   kt::ScopedObjectLock<objConfig> config(glConfig, 3000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   size_t size = 0;
   ConfigGroups *groups;
   if (glConfig->get(FID_Data, groups) IS ERR::Okay) {
      for (auto & [group, keys] : groups[0]) {
         size += sizeof(FontList) + keys["Name"].size() + 1 + keys["Styles"].size() + 1;
         if (keys.contains("Alias")) size += keys["Alias"].size() + 1;
         if (keys.contains("Axes")) size += keys["Axes"].size() + 1;

         if (auto point_it = keys.find("Points"); (point_it != keys.end()) and (not point_it->second.empty())) {
            size_t point_count = 1;
            for (auto ch : point_it->second) {
               if (ch IS ',') point_count++;
            }
            size += sizeof(int) - 1 + ((point_count + 1) * sizeof(int));
         }
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
            if ((it != keys.end()) and (not it->second.empty())) {
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
                  if (not fontpoints.empty()) {
                     auto align = (uintptr_t)buffer % sizeof(int);
                     if (align) buffer += sizeof(int) - align;

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

Calculates the pixel width of `String` using the supplied Font object's current metrics and spacing settings.  Line
feeds are handled by measuring each line independently and returning the width of the longest line.

Word wrapping is not applied, even if #WrapEdge has been set on the Font object.

-INPUT-
obj(Font) Font: An initialised font object.
strview String: The string to be calculated.
int Chars: The maximum number of characters to measure, or `-1` to measure the entire string.

-RESULT-
int: The pixel width of the string, or `0` if `Font` is `NULL`, uninitialised or `String` is empty.

-TAGS-
pure-query

*********************************************************************************************************************/

int StringWidth(objFont *Font, const std::string_view &String, int Chars)
{
   if ((not Font) or String.empty()) return 0;
   if (not Font->initialised()) return 0;

   auto font = (extFont *)Font;
   auto str = String;
   if (Chars < 0) Chars = 0x7fffffff;

   int len    = 0;
   int widest = 0;
   int whitespace = 0;
   while ((not str.empty()) and (Chars > 0)) {
      if (str.front() IS '\n') {
         if (widest < len) widest = len - whitespace;
         len  = 0; // Reset
         str.remove_prefix(1);
         Chars--;
         whitespace = 0;
      }
      else if (str.front() IS '\t') {
         int16_t tabwidth = (font->prvChar[' '].Advance * Font->GlyphSpacing) * Font->TabSize;
         if (tabwidth) len = tab_advance<int>(len, tabwidth);
         str.remove_prefix(1);
         Chars--;
         whitespace = 0;
      }
      else {
         uint32_t unicode;
         str.remove_prefix(size_t(getutf8(str, &unicode)));
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

Resolves a font family `Name` and preferred `Style` to a font file path.  The family name must exist in the font
database.  If the requested style is unavailable, the function falls back to the face's regular style or first
registered style.

-INPUT-
strview Name:  The name of a font face to search for (case insensitive).
strview Style: The preferred style, e.g. `Bold` or `Italic`.
^&string Path: The location of the best-matching font file is returned in this parameter.
&int(FMETA) Meta: Optional, returns additional meta information about the font file.

-ERRORS-
Okay
NullArgs
AccessObject: Access to the font database was denied, or the object does not exist.
AllocMemory
Search: Unable to find a suitable font.

-TAGS-
blocking, case-insensitive
-END-

*********************************************************************************************************************/

ERR SelectFont(const std::string_view &Name, const std::string_view &Style, std::string *Path, FMETA *Meta)
{
   kt::Log log(__FUNCTION__);

   log.branch("%.*s:%.*s", int(Name.size()), Name.data(), int(Style.size()), Style.data());

   if (Name.empty() or (not Path)) return log.warning(ERR::NullArgs);

   *Path = "";

   kt::ScopedObjectLock<objConfig> config(glConfig, 5000);
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

   auto get_font_path = [](ConfigKeys &Keys, const std::string &Style, std::string *Path) {
      if (Keys.contains(Style)) {
         *Path = Keys[Style];
         return ERR::Okay;
      }
      else if (not iequals("Regular", Style)) {
         if (Keys.contains("Regular")) {
            *Path = Keys["Regular"];
            return ERR::Okay;
         }
      }
      return ERR::Search;
   };

   std::string style_name(Style);
   kt::camelcase(style_name);

   for (auto & [group, keys] : groups[0]) {
      if (not iequals(Name, keys["Name"])) continue;

      auto error = get_font_path(keys, style_name, Path);
      if (error IS ERR::Okay) {
         if (Meta) *Meta = get_meta(keys);
         return ERR::Okay;
      }
      else if (error != ERR::Search) return error;

      log.traceWarning("Requested style '%s' not available, choosing first style.", style_name.c_str());

      std::string styles = keys.contains("Styles") ? keys["Styles"] : "Regular";
      auto end = styles.find(",");
      if (end IS std::string::npos) end = styles.size();
      std::string first_style = styles.substr(0, end);

      if (keys.contains(first_style)) {
         *Path = keys[first_style];
         if (Meta) *Meta = get_meta(keys);
         return ERR::Okay;
      }
      else return ERR::Search;
   }

   log.warning("The \"%.*s\" font is not available.", int(Name.size()), Name.data());
   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
RefreshFonts: Refreshes the system font list with up-to-date font information.

Scans the `fonts:` volume and rebuilds the font database.

Refreshing fonts can take an extensive amount of time because each font file must be analysed for family, style,
metric and metadata information.  The `fonts:fonts.cfg` file is rewritten on completion.

-ERRORS-
Okay
AccessObject: Access to the font database was denied, or the object does not exist.
GetField: Failed to read font database entries after scanning.
OpenFile: Failed to open `fonts:fonts.cfg` for writing.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

ERR RefreshFonts(void)
{
   kt::Log log(__FUNCTION__);

   log.branch();

   kt::ScopedObjectLock<objConfig> config(glConfig, 3000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   if (auto error = acClear(glConfig); error != ERR::Okay) return error; // Clear out existing font information

   scan_fixed_folder(glConfig);
   scan_truetype_folder(glConfig);

   if (auto error = glConfig->sortByKey("", false); error != ERR::Okay) return error; // Sort by font name.

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
            if (not v.compare(0, 6, "fonts:")) styles.push_front(k);
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
   else return log.warning(ERR::GetField);

   // Save the font configuration file

   objFile::create file = { fl::Path("fonts:fonts.cfg"), fl::Flags(FL::NEW|FL::WRITE) };
   if (file.ok()) return glConfig->saveToObject(*file);
   else return log.warning(ERR::OpenFile);
}

/*********************************************************************************************************************

-FUNCTION-
ResolveFamilyName: Convert a CSV family string to a single family name.

Converts a CSV family string to one resolved family name.  `String` is parsed from left to right, with each family name
or wildcard tested against the font database in order.  If a single asterisk is used to terminate the list, the system
default is returned when no earlier name matches.

Individual names may use the common wildcards `?` and `*`; for example, `Times New *` can match `Times New Roman` if
it is available.

The returned `Result` is borrowed storage.  Copy it immediately if it needs to survive a later font database refresh.

-INPUT-
strview String: A CSV family string to resolve.
&strview Result: The resolved family name is returned in this parameter.

-ERRORS-
Okay
NullArgs
AccessObject: Access to the font database was denied, or the object does not exist.
GetField
Search: It was not possible to resolve the String to a known font family.

-TAGS-
volatile-result, null-terminated-result, blocking, case-insensitive

-END-

*********************************************************************************************************************/

ERR ResolveFamilyName(const std::string_view &String, std::string_view *Result)
{
   kt::Log log(__FUNCTION__);

   if ((String.empty()) or (not Result)) return ERR::NullArgs;

   kt::ScopedObjectLock<objConfig> config(glConfig, 5000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   ConfigGroups *groups;
   if (glConfig->get(FID_Data, groups) != ERR::Okay) return log.warning(ERR::GetField);

   std::vector<std::string> names;
   kt::split(String, std::back_inserter(names));

   for (auto &name : names) {
      kt::ltrim(name, "'\"");
      kt::rtrim(name, "'\"");

      if (name.empty()) continue;

      std::vector<std::string> visited_aliases;

      for (;;) {
         if ((name[0] IS '*') and (not name[1])) {
            // Default family requested - use the first font declaring a "Default" key
            for (auto & [group, keys] : groups[0]) {
               if (keys.contains("Default")) {
                  *Result = keys["Name"];
                  return ERR::Okay;
               }
            }

            *Result = "Noto Sans";
            return ERR::Okay;
         }

         bool alias_restart = false;
         bool alias_loop = false;

         for (auto & [group, keys] : groups[0]) {
            if (kt::wildcmp(name, keys["Name"])) {
               if (auto it = keys.find("Alias"); it != keys.end()) {
                  const std::string &alias = it->second;
                  if (not alias.empty()) {
                     for (auto &visited : visited_aliases) {
                        if (iequals(visited, keys["Name"])) {
                           alias_loop = true;
                           break;
                        }
                     }

                     if (alias_loop) break;

                     visited_aliases.push_back(keys["Name"]);
                     name = alias;
                     alias_restart = true;
                     break;
                  }
               }

               *Result = keys["Name"];
               return ERR::Okay;
            }
         }

         if (alias_loop) {
            log.warning("Detected a cyclic alias while resolving family \"%.*s\"", int(String.size()), String.data());
            break;
         }
         else if (alias_restart) continue;
         else break;
      }
   }

   log.msg("Failed to resolve family \"%.*s\"", int(String.size()), String.data());
   return ERR::Search;
}

} // namespace

//********************************************************************************************************************

static void scan_truetype_folder(objConfig *Config)
{
   kt::Log log(__FUNCTION__);

   log.branch("Scanning for truetype fonts.");

   std::string ttpath;
   if (ResolvePath("fonts:truetype/", RSF::NO_FILE_CHECK|RSF::PATH, &ttpath) IS ERR::Okay) {
      DirInfo *dir;
      if (OpenDir(ttpath, RDF::FILE, &dir) IS ERR::Okay) {
         LocalResource free_dir(dir);

         auto ttpath_len = ttpath.size();
         while (ScanDir(dir) IS ERR::Okay) {
            ttpath.resize(ttpath_len);
            ttpath.append(dir->Info->Name);

            FT_Face ftface;
            FT_Open_Args open = { .flags = FT_OPEN_PATHNAME, .pathname = (FT_String *)ttpath.c_str() };
            if (not FT_Open_Face(glFTLibrary, &open, 0, &ftface)) {
               if (not FT_IS_SCALABLE(ftface)) { // Sanity check
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

               kt::rtrim(group);

               Config->write(group.c_str(), "Name", group);
               Config->write(group.c_str(), "Scalable", "Yes");

               if (FT_HAS_MULTIPLE_MASTERS(ftface)) {
                  // A single ttf file can contain multiple named styles
                  Config->write(group.c_str(), "Variable", "Yes");

                  FT_MM_Var *mvar;
                  if (not FT_Get_MM_Var(ftface, &mvar)) {
                     FT_UInt index;
                     if (not FT_Get_Default_Named_Instance(ftface, &index)) {
                        char buffer[100];
                        auto name_table_size = FT_Get_Sfnt_Name_Count(ftface);
                        for (FT_UInt s=0; (s < mvar->num_namedstyles); s++) {
                           for (int n=int(name_table_size)-1; n >= 0; n--) {
                              FT_SfntName sft_name;
                              if (not FT_Get_Sfnt_Name(ftface, n, &sft_name)) {
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

                  if ((ftface->style_name) and (not iequals("regular", ftface->style_name))) {
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
   kt::Log log(__FUNCTION__);

   log.branch("Scanning for fixed fonts.");

   DirInfo *dir;
   if (OpenDir("fonts:fixed/", RDF::FILE, &dir) IS ERR::Okay) {
      LocalResource free_dir(dir);

      while (ScanDir(dir) IS ERR::Okay) {
         std::string location("fonts:fixed/");
         location.append(dir->Info->Name);

         winfnt_header_fields header;
         std::vector<uint16_t> points;
         std::string facename;
         if (analyse_bmp_font(location, &header, facename, points) IS ERR::Okay) {
            log.detail("Detected font file \"%.*s\", name: %s", int(location.size()), location.data(), facename.c_str());

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

            if (style IS FTF::BOLD) Config->write(gs, "Bold", location);
            else if (style IS FTF::ITALIC) Config->write(gs, "Italic", location);
            else if (style IS (FTF::BOLD|FTF::ITALIC)) Config->write(gs, "Bold Italic", location);
            else {
               // Font is regular, which also means we can convert it to bold/italic with some code
               Config->write(gs, "Regular", location);
               Config->write(gs, "Bold", location);
               Config->write(gs, "Bold Italic", location);
               Config->write(gs, "Italic", location);
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
         else log.warning("Failed to analyse %.*s", int(location.size()), location.data());
      }
   }
   else log.warning("Failed to scan directory fonts:fixed/");
}

//********************************************************************************************************************

static ERR analyse_bmp_font(std::string_view Path, winfnt_header_fields *Header, std::string &FaceName, std::vector<uint16_t> &Points)
{
   kt::Log log(__FUNCTION__);
   char face[50];

   if ((Path.empty()) or (not Header)) return ERR::NullArgs;

   objFile::create file = { fl::Path(Path), fl::Flags(FL::READ) };
   if (file.ok()) {
      std::vector<winFont> fonts;
      if (auto error = read_winfont_entries(*file, fonts); error IS ERR::NoData) {
         log.warning("There are no fonts in file \"%.*s\"", int(Path.size()), Path.data());
         return ERR::Failed;
      }
      else if (error != ERR::Okay) return error;

      // Read font point sizes

      for (auto &font : fonts) {
         file->seekStart(font.Offset);
         if (file->read(Header, sizeof(winfnt_header_fields)) IS ERR::Okay) {
            Points.push_back(Header->nominal_point_size);
         }
      }

      // Go to the first font in the file and read the font header

      file->seekStart(fonts[0].Offset);

      if (file->read(Header, sizeof(winfnt_header_fields)) != ERR::Okay) return ERR::Read;

      if (auto error = validate_winfnt_header(*Header, Path); error != ERR::Okay) return error;

      // Extract the name of the font

      file->seekStart(fonts[0].Offset + Header->face_name_offset);

      int i;
      for (i=0; (size_t)i < sizeof(face)-1; i++) {
         ERR result = file->read(face+i, 1);
         if ((result != ERR::Okay) or (not face[i])) break;
      }
      face[i] = 0;
      FaceName = face;

      return ERR::Okay;
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
