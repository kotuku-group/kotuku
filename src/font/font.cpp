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
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_OUTLINE_H

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/config.h>
#include <kotuku/modules/module.h>

#include <sstream>
#include <array>
#include <cfloat>
#include <set>
#include <span>
#include <charconv>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>
#include <kotuku/strings.hpp>
#include <kotuku/font_traits.hpp>
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

// Character sets that the font database reports coverage for.  Each set lists the inclusive ranges of its assigned
// code points, so that a font covering every character of a set scores 100%.  The ids are written to the Coverage
// key of fonts.cfg and form part of the database format.

struct unicode_range { uint32_t first, last; };

static constexpr unicode_range glBasicLatin[]   = { { 0x20, 0x7e } };
static constexpr unicode_range glLatin1[]       = { { 0xa0, 0xff } };
static constexpr unicode_range glLatinExtA[]    = { { 0x100, 0x17f } };
static constexpr unicode_range glLatinExtB[]    = { { 0x180, 0x24f } };
static constexpr unicode_range glPasifika[]     = { // Macron vowels, the ʻokina and the combining macron
   { 0x100, 0x101 }, { 0x112, 0x113 }, { 0x12a, 0x12b }, { 0x14c, 0x14d }, { 0x16a, 0x16b }, { 0x2bb, 0x2bb },
   { 0x304, 0x304 }
};
static constexpr unicode_range glVietnamese[]   = { // Latin Extended Additional, plus Đ Ơ Ư, the tone marks and ₫
   { 0x110, 0x111 }, { 0x1a0, 0x1a1 }, { 0x1af, 0x1b0 }, { 0x300, 0x301 }, { 0x303, 0x303 }, { 0x309, 0x309 },
   { 0x323, 0x323 }, { 0x1ea0, 0x1ef9 }, { 0x20ab, 0x20ab }
};
static constexpr unicode_range glGreek[]        = {
   { 0x370, 0x377 }, { 0x37a, 0x37f }, { 0x384, 0x38a }, { 0x38c, 0x38c }, { 0x38e, 0x3a1 }, { 0x3a3, 0x3ff }
};
static constexpr unicode_range glCyrillic[]     = { { 0x400, 0x4ff } };
static constexpr unicode_range glIPA[]          = { { 0x250, 0x2af } };
static constexpr unicode_range glArrowsMath[]   = { { 0x2190, 0x22ff } };
static constexpr unicode_range glBoxDrawing[]   = { { 0x2500, 0x257f } };
static constexpr unicode_range glEmoji[]        = {
   { 0x1f300, 0x1f64f }, { 0x1f680, 0x1f6d7 }, { 0x1f6dc, 0x1f6ec }, { 0x1f6f0, 0x1f6fc }, { 0x1f900, 0x1f9ff }
};
static constexpr unicode_range glCJK[]          = { { 0x4e00, 0x9fff } };

struct coverage_set {
   std::string_view id;
   std::span<const unicode_range> ranges;

   constexpr int total() const {
      int count = 0;
      for (auto &range : ranges) count += int(range.last - range.first + 1);
      return count;
   }

   constexpr bool contains(uint32_t Unicode) const {
      for (auto &range : ranges) {
         if ((Unicode >= range.first) and (Unicode <= range.last)) return true;
      }
      return false;
   }
};

static constexpr coverage_set glCoverageSets[] = {
   { "basic-latin", glBasicLatin }, { "latin-1", glLatin1 }, { "latin-ext-a", glLatinExtA },
   { "latin-ext-b", glLatinExtB }, { "pasifika", glPasifika }, { "vietnamese", glVietnamese }, { "greek", glGreek },
   { "cyrillic", glCyrillic }, { "ipa", glIPA }, { "arrows-math", glArrowsMath }, { "box-drawing", glBoxDrawing },
   { "emoji", glEmoji }, { "cjk", glCJK }
};

constexpr int COVERAGE_SETS = int(std::size(glCoverageSets));

// The range of a variable font axis, in design units.

struct axis_range {
   std::string tag;
   double min, def, max;
};

// Descriptive information for the font database, gathered from each file of a family and merged by add_details().

struct family_details {
   std::string cls;                           // Classification: Serif, Sans, Mono, Script, Display or Symbol
   std::vector<axis_range> axes;              // Variable font axes, in the order declared by the font
   std::set<std::string> features;            // OpenType layout feature tags, e.g. 'liga', 'kern'
   std::array<int, COVERAGE_SETS> coverage{}; // Count of the characters covered in each of glCoverageSets
   int characters = 0;                        // Total count of mapped characters
   bool regular = false;                      // True if cls was taken from an upright regular face
   bool analysed = false;

   void merge(const family_details &);
};

// Information extracted from a scalable font file.

struct scalable_face {
   std::string family;
   std::vector<std::string> styles; // Style names provided by the file
   family_details details;          // Only populated on request
   bool variable = false;
   int faces = 1;                  // Number of faces in the collection
};

// Each file owns its analysis results so that workers never modify the font database or another worker's results.

struct scalable_file {
   std::string path;
   std::string name;
   std::vector<std::pair<int, scalable_face>> faces; // Face index and successful analysis result
};

// A family registered by RegisterFont().  Private fonts are held apart from the font database so that they are never
// saved to fonts:fonts.cfg, are not shown in font lists and survive calls to RefreshFonts().  Access is serialised by
// the glConfig lock.  A std::list is used so that names returned by ResolveFamilyName() remain valid.

struct private_font {
   std::string name;
   std::vector<std::pair<std::string, std::string>> styles; // Style name, resolved file path
   FMETA meta = FMETA::SCALED|FMETA::HIDDEN;

   const std::string * find_style(std::string_view Style) const {
      for (auto &[style, path] : styles) {
         if (iequals(Style, style)) return &path;
      }
      return nullptr;
   }
};

static std::list<private_font> glPrivateFonts;

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

   extFont(objMetaClass *ClassPtr, OBJECTID ObjectID) : objFont(ClassPtr, ObjectID) {
      TabSize         = 8;
      prvDefaultChar  = '.';
      prvLineCountCR  = 1;
      Colour.Alpha    = 255;
      GlyphSpacing    = 1.0;
      Style           = "Regular";
   }

   ~extFont();
};

#include "font_def.c"

#include "font_bitmap.cpp"

static ERR add_font_class(void);
static ERR analyse_scalable_font(FT_Library, const std::string &, std::string_view, scalable_face &,
   bool = false, int = 0);
static void scan_scalable_folder(objConfig *, std::map<std::string, family_details> &);
static void scan_fixed_folder(objConfig *, std::map<std::string, family_details> &);
static void scan_host_folders(objConfig *, const std::map<std::string, family_details> &,
   std::map<std::string, family_details> &);
static ERR build_style_lists(objConfig *);
static ERR refresh_host_fonts(void);
static void merge_host_fonts(objConfig *);
static void write_family_details(objConfig *, const std::map<std::string, family_details> &);
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
      if (!FindObject("glStyle", CLASSID::XML, &style_id)) {
         kt::ScopedObjectLock<objXML> style(style_id, 3000);
         if (style.granted()) {
            std::string pointsize;
            if (!acGetKey(style.obj, "/interface/@fontsize", pointsize)) {
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
static constexpr std::string_view HOST_FONTS_PATH = "user:config/host_fonts.cfg";

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log;

   CoreBase = argCoreBase;

   modFont = (OBJECTPTR)((objModule *)argModule)->Root;

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
         if (auto error = fnt::RefreshFonts(std::string_view(), RFF::SCAN_HOST); error != ERR::Okay) {
            cleanup();
            return error;
         }
      }

      ConfigGroups *groups;
      if (not ((!glConfig->getGroups(groups)) and (not groups->empty()))) {
         log.error("Failed to build a database of valid fonts.");
         cleanup();
         return ERR::NoData;
      }

      // Merge host fonts and tailored font options into the machine-generated database.  RefreshFonts() has
      // already done this if a refresh was performed.

      if (not refresh) {
         if ((AnalysePath(HOST_FONTS_PATH, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
            if (objConfig::create host = { fl::Path(HOST_FONTS_PATH) }; host.ok()) merge_host_fonts(*host);
         }
         else if (auto error = refresh_host_fonts(); error != ERR::Okay) {
            // Write an empty host database so that the failed scan is not repeated on every initialisation.  A
            // RefreshFonts() call with SCAN_HOST will rebuild it.

            log.warning("Host font scan failed: %s", GetErrorMsg(error));
            objFile::create blank = { fl::Path(HOST_FONTS_PATH), fl::Flags(FL::NEW|FL::WRITE) };
            if (not blank.ok()) {
               log.warning("Unable to write a blank host font database to \"%s\"", HOST_FONTS_PATH.data());
            }
         }
         glConfig->mergeFile("fonts:options.cfg");
      }
   }
   else {
      log.error("Failed to load or prepare the font configuration file.");
      cleanup();
      return ERR::CreateObject;
   }

   if (auto error = add_font_class(); error != ERR::Okay) {
      cleanup();
      return error;
   }

   return ERR::Okay;
}

static ERR MODOpen(OBJECTPTR Module)
{
   ((objModule *)Module)->setFunctionList(glFunctions);
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
   glPrivateFonts.clear();
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
database or have been registered with ~RegisterFont().

The `Style` can be a style name such as `Semi Bold Italic`, or a style string with descriptors such as
`weight=Medium;width=Condensed;italic=yes`.  The `weight`, `width`, `italic` and `slant` descriptors are used to choose
the file; other descriptors are ignored here.  See the VectorText `FontStyle` field for the full syntax.

If the requested style is unavailable, the nearest available style is chosen with the CSS font matching order: width
first, then italic, then weight.  For instance, a request for `Semi Bold` on a family that only provides `Regular`,
`Italic`, `Bold` and `Bold Italic` resolves to `Bold`.  Style names that do not describe a weight, width or slope are
only chosen if the family has no other styles.

Families registered with ~RegisterFont() are searched first and resolved exclusively from their registered files, so
a private family will shadow a system family of the same name.  The `Meta` result for a private family includes the
`HIDDEN` flag.

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
   return SelectFontFace(Name, Style, Path, Meta, nullptr);
}

/*********************************************************************************************************************

-FUNCTION-
SelectFontFace: Selects a font file and its face within a collection.

Uses the same family and style matching as ~SelectFont(), and also returns the zero-based face index needed to open
font collections with FreeType.  File paths remain ordinary paths without an encoded index.  Fonts whose database
entries have no face index use face zero.

-INPUT-
strview Name: The font family name, matched case insensitively.
strview Style: The preferred style name or style descriptors, as accepted by ~SelectFont().
^&string Path: The selected font file path.
&int(FMETA) Meta: Optional, receives the font metadata flags.
&int FaceIndex: Optional, receives the zero-based face index within the selected file.

-ERRORS-
Okay
NullArgs
AccessObject
Search

-TAGS-
blocking, case-insensitive
-END-

*********************************************************************************************************************/

ERR SelectFontFace(const std::string_view &Name, const std::string_view &Style, std::string *Path, FMETA *Meta,
   int *FaceIndex)
{
   kt::Log log(__FUNCTION__);

   log.branch("%.*s:%.*s", int(Name.size()), Name.data(), int(Style.size()), Style.data());

   if (Name.empty() or (not Path)) return log.warning(ERR::NullArgs);

   *Path = "";
   if (FaceIndex) *FaceIndex = 0;

   kt::ScopedObjectLock<objConfig> config(glConfig, 5000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   ConfigGroups *groups;
   if (glConfig->getGroups(groups) != ERR::Okay) return ERR::Search;

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

   std::string style_name = kt::font_traits::selection_name(Style);
   kt::camelcase(style_name);
   auto requested = kt::font_traits::style_traits(Style);
   auto named_traits = kt::font_traits::name_traits(style_name);
   // A generated name is only exact when it preserves all requested traits, without rounding.
   const bool exact_name = (named_traits.weight IS requested.weight) and
      (named_traits.width IS requested.width) and (named_traits.italic IS requested.italic);

   // Returns the index of the available style that is the nearest match to the request.  Names that do not
   // describe traits are only considered if no other names are available.

   auto nearest_style = [&requested](const std::vector<std::string_view> &Names) {
      std::vector<kt::font_traits::traits> candidates;
      std::vector<int> index;
      for (int pass=0; (pass < 2) and candidates.empty(); pass++) {
         for (int i=0; i < int(Names.size()); i++) {
            auto traits = kt::font_traits::name_traits(Names[i]);
            if ((pass IS 0) and (not traits.recognised)) continue;
            candidates.push_back(traits);
            index.push_back(i);
         }
      }
      auto best = kt::font_traits::nearest_match(requested, candidates);
      return (best >= 0) ? index[best] : -1;
   };

   for (auto &font : glPrivateFonts) {
      if (not iequals(Name, font.name)) continue;

      auto path = exact_name ? font.find_style(style_name) : nullptr;
      if (not path) {
         std::vector<std::string_view> names;
         for (auto &[style, file] : font.styles) names.push_back(style);
         auto best = nearest_style(names);
         if (best < 0) return log.warning(ERR::Search);
         log.trace("Requested style '%s' not available, choosing '%s'.", style_name.c_str(),
            font.styles[best].first.c_str());
         path = &font.styles[best].second;
      }

      *Path = *path;
      if (Meta) *Meta = font.meta;
      return ERR::Okay;
   }

   for (auto & [group, keys] : groups[0]) {
      if (not iequals(Name, keys["Name"])) continue;

      auto set_face_index = [&keys, FaceIndex](const std::string &Style) {
         if (not FaceIndex) return;
         if (auto it = keys.find("FaceIndex:" + Style); it != keys.end()) {
            int index = 0;
            auto result = std::from_chars(it->second.data(), it->second.data() + it->second.size(), index);
            if ((result.ec IS std::errc()) and (result.ptr IS it->second.data() + it->second.size()) and
                (index >= 0) and (index <= 0xffff)) *FaceIndex = index;
         }
      };

      if (auto it = keys.find(style_name); exact_name and (it != keys.end())) {
         *Path = it->second;
         set_face_index(style_name);
         if (Meta) *Meta = get_meta(keys);
         return ERR::Okay;
      }

      std::vector<std::string> styles;
      if (auto it = keys.find("Styles"); it != keys.end()) {
         std::string_view list(it->second);
         while (not list.empty()) {
            auto end = list.find(',');
            auto name = kt::font_traits::trim(list.substr(0, end));
            if ((not name.empty()) and keys.contains(std::string(name))) styles.emplace_back(name);
            list = (end IS std::string_view::npos) ? std::string_view() : list.substr(end + 1);
         }
      }
      else if (keys.contains("Regular")) styles.emplace_back("Regular");

      std::vector<std::string_view> names(styles.begin(), styles.end());
      auto best = nearest_style(names);
      if (best < 0) return ERR::Search;

      log.trace("Requested style '%s' not available, choosing '%s'.", style_name.c_str(), styles[best].c_str());
      *Path = keys[styles[best]];
      set_face_index(styles[best]);
      if (Meta) *Meta = get_meta(keys);
      return ERR::Okay;
   }

   log.warning("The \"%.*s\" font is not available.", int(Name.size()), Name.data());
   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
RefreshFonts: Refreshes the system font list with up-to-date font information.

Scans the `fonts:` volume and rebuilds the font database.  The tailored options in `fonts:options.cfg`, such as
family aliases and metric modifiers, are merged into the database again once the refresh is complete.

Refreshing fonts can take an extensive amount of time because each font file must be analysed for family, style,
metric and metadata information.  The font database is saved to `fonts:fonts.cfg` on completion, unless an alternative
location is specified in `Path`.

Fonts registered with ~RegisterFont() are private to the process and are neither cleared nor saved by a refresh.

If the `SCAN_HOST` flag is set, the host's native font folders are scanned for scalable fonts after the `fonts:`
volume.  On Linux the folders are, in order: `$XDG_DATA_HOME/fonts` (defaulting to `~/.local/share/fonts`), `~/.fonts`,
the `fonts` folder of each `$XDG_DATA_DIRS` entry (defaulting to `/usr/local/share:/usr/share`), `/usr/share/fonts` and
`/usr/local/share/fonts`.  On Windows the folders are, in order: the per-user font folder
`%LOCALAPPDATA%\Microsoft\Windows\Fonts` and the system font folder `%SystemRoot%\Fonts`.  The folders are scanned recursively for `ttf`, `otf`, `ttc` and `otc` files, and host fonts
are recorded by their absolute paths.  If a host family style is found in more than one folder, the first folder in the
list above takes precedence.  Every face in a collection is scanned.  For faces after the first, the zero-based index
is stored in the `FaceIndex:Style` key, where `Style` is the corresponding style name, and is returned by
~SelectFontFace().  A style without this key uses face zero.  Host
scanning is not supported on other platforms at this time, and the flag is ignored there.

On Windows, host font paths are recorded in UTF-8 but FreeType opens files using the active code page.  Unless the
active code page is UTF-8, host fonts with non-ASCII characters in their paths cannot be opened and are skipped.

Host fonts are saved to their own database at `user:config/host_fonts.cfg`, regardless of `Path`, so that the `fonts:`
database remains exclusive to Kotuku's fonts.  The host database is merged into the font database after it is saved,
and before `fonts:options.cfg`.  If `SCAN_HOST` is not set, the host database from an earlier scan is merged instead,
if it exists.  Families provided by the `fonts:` volume take precedence and are never mixed with host files of the same
name.  On module initialisation, a missing host database is built independently of an existing `fonts:` database;
a failure to save that per-user cache does not prevent font loading.  To stop using host fonts, delete `user:config/host_fonts.cfg` and refresh the fonts.

Host font folders can hold thousands of files, so expect a host scan to take considerably longer than a standard
refresh.  Host fonts that are subsequently removed will remain in the host database until the next host scan.

Each family is recorded as a group, keyed by family name, in which each style name refers to the file that provides
it.  The following keys describe the family as a whole:

<types type="Key">
<type name="Name">The family name.</type>
<type name="Styles">A sorted CSV list of the family's style names.</type>
<type name="Scalable">`Yes` if the family is an outline font.  Bitmap families omit this key.</type>
<type name="Variable">`Yes` if the family is a variable font.</type>
<type name="Points">A CSV list of the point sizes available.  Bitmap families record their strikes.  A scalable family
only has this key if it is defined in `fonts:options.cfg`, which restricts the family to the listed sizes.</type>
<type name="Class">The family's classification: `Serif`, `Sans`, `Mono`, `Script`, `Display` or `Symbol`.  The class is
read from the OS/2 PANOSE and family class data of outline fonts and the pitch and family field of bitmap fonts.  If an
outline font does not declare a class, its serifs are measured on the capital I and T.  The key is omitted if the class
cannot be determined.</type>
<type name="Axes">Variable families only: a CSV list of axis tags, e.g. `wght,wdth`.</type>
<type name="AxisRanges">Variable families only: the range of each axis as `tag:min:default:max`, e.g.
`wght:100:400:900,wdth:62.5:100:100`.  The ranges of all files in the family are combined.</type>
<type name="Features">A sorted CSV list of the OpenType layout features found in the GSUB and GPOS tables, e.g.
`kern,liga,tnum`.  A legacy `kern` table is reported as `kern`.</type>
<type name="Coverage">The percentage of each character set that is mapped by the family, as `id:percent`, e.g.
`basic-latin:100,greek:89`.  Percentages are rounded down, so `100` indicates complete coverage, and a set with any
coverage reports at least `1`.  Sets that have no coverage are omitted.  The set ids are `basic-latin`, `latin-1`,
`latin-ext-a`, `latin-ext-b`, `pasifika` (macron vowels and the ʻokina), `vietnamese`, `greek`, `cyrillic`, `ipa`,
`arrows-math`, `box-drawing`, `emoji` and `cjk`.</type>
<type name="Characters">The count of characters mapped by the family's largest file.</type>
</types>

Bitmap fonts are treated as Latin-1 for coverage purposes, which matches how the Font class maps their characters.

-INPUT-
strview Path: Optional.  Overrides the default `fonts:fonts.cfg` location that the font database is saved to.
int(RFF) Flags: Optional flags.

-ERRORS-
Okay
AccessObject: Access to the font database was denied, or the object does not exist.
GetField: Failed to read font database entries after scanning.
OpenFile: Failed to open the font database file for writing.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

ERR RefreshFonts(const std::string_view &Path, RFF Flags)
{
   kt::Log log(__FUNCTION__);

   log.branch("Flags: $%.8x", int(Flags));

   kt::ScopedObjectLock<objConfig> config(glConfig, 3000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   if (auto error = acClear(glConfig); error != ERR::Okay) return error; // Clear out existing font information

   std::map<std::string, family_details> details;
   scan_fixed_folder(glConfig, details);
   scan_scalable_folder(glConfig, details);
   write_family_details(glConfig, details);

   if (auto error = build_style_lists(glConfig); error != ERR::Okay) return error;

   // Save the font configuration file

   ERR error;
   objFile::create file = { fl::Path(Path.empty() ? "fonts:fonts.cfg" : Path), fl::Flags(FL::NEW|FL::WRITE) };
   if (file.ok()) error = glConfig->saveToObject(*file);
   else error = log.warning(ERR::OpenFile);

   // Host fonts are kept in a separate database so that the fonts: database remains exclusive to Kotuku.  If the host
   // is not scanned, the database from an earlier scan is used.

   if ((Flags & RFF::SCAN_HOST) != RFF::NIL) {
      if (auto host_error = refresh_host_fonts(); (host_error != ERR::Okay) and (error IS ERR::Okay)) {
         error = host_error;
      }
   }
   else {
      LOC type;
      if ((AnalysePath(HOST_FONTS_PATH, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
         if (objConfig::create host = { fl::Path(HOST_FONTS_PATH) }; host.ok()) {
            merge_host_fonts(*host);
         }
      }
   }

   // Restore the tailored font options, e.g. aliases and metric modifiers.  They are merged after saving so that the
   // saved file remains machine-generated.

   glConfig->mergeFile("fonts:options.cfg");
   return error;
}

/*********************************************************************************************************************

-FUNCTION-
ResolveFamilyName: Convert a CSV family string to a single family name.

Converts a CSV family string to one resolved family name.  `String` is parsed from left to right, with each family name
or wildcard tested against the font database in order.  If a single asterisk is used to terminate the list, the system
default is returned when no earlier name matches.

Individual names may use the common wildcards `?` and `*`; for example, `Times New *` can match `Times New Roman` if
it is available.  Families registered with ~RegisterFont() are tested before the font database.

A family can be an alias of another, as declared by the `Alias` key in `fonts:options.cfg`.  Aliases are followed to
the family that they refer to, and the name of the first alias that was matched is returned in `Alias`.  The alias's
group in the font configuration can then be consulted for its `Style` and metric options.  `Alias` is empty if the
family was not resolved through an alias.

The returned `Result` and `Alias` are borrowed storage.  Copy them immediately if they need to survive a later font
database refresh.

-INPUT-
strview String: A CSV family string to resolve.
&strview Result: The resolved family name is returned in this parameter.
&strview Alias: Optional.  Returns the name of the alias that the family was resolved through, or an empty string.

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

ERR ResolveFamilyName(const std::string_view &String, std::string_view *Result, std::string_view *Alias)
{
   kt::Log log(__FUNCTION__);

   if ((String.empty()) or (not Result)) return ERR::NullArgs;


   kt::ScopedObjectLock<objConfig> config(glConfig, 5000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   ConfigGroups *groups;
   if (glConfig->getGroups(groups) != ERR::Okay) return log.warning(ERR::GetField);

   std::vector<std::string> names;
   kt::split(String, std::back_inserter(names));

   for (auto &name : names) {
      kt::ltrim(name, "'\"");
      kt::rtrim(name, "'\"");

      if (name.empty()) continue;

      if (Alias) *Alias = std::string_view(); // Reset if an earlier name ended in an unresolved alias
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

         for (auto &font : glPrivateFonts) {
            if (kt::wildcmp(name, font.name)) {
               *Result = font.name;
               return ERR::Okay;
            }
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

                     if ((Alias) and visited_aliases.empty()) *Alias = keys["Name"];
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

   if (Alias) *Alias = std::string_view();
   log.msg("Failed to resolve family \"%.*s\"", int(String.size()), String.data());
   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
RegisterFont: Makes a font file available to the running process.

Registers a scalable font file, such as a TrueType or OpenType file, so that its family can be referenced by name in
the same way as an installed font.  This allows an application to use fonts that are distributed with it, without
installing them to the `fonts:` volume.

The family and style names are read from the file.  A variable font registers each of its named styles.  The resolved
family name is returned in `Family` and can be used with the @VectorText.Face field, ~SelectFont() and
~ResolveFamilyName().  Registering further files for the same family adds their styles to it, with the most recently
registered file taking precedence for any style that is provided more than once.

Registered fonts are private to the running process.  They are not saved to the font database, do not appear in the
font lists that are built from `fonts:fonts.cfg` and are unaffected by ~RefreshFonts().  A registered family takes
precedence over an installed family of the same name.  Registrations last until the Font module is expunged; there is
no means of removing them.

-INPUT-
strview Path: The location of the font file.
^&string Family: Optional, returns the family name of the registered font.

-ERRORS-
Okay
NullArgs
AccessObject: Access to the font database was denied, or the object does not exist.
ResolvePath: The `Path` could not be resolved to an existing file.
InvalidData: The file is not a recognised font format.
NoSupport: The file is not a scalable font.

-TAGS-
blocking

-END-

*********************************************************************************************************************/

ERR RegisterFont(const std::string_view &Path, std::string *Family)
{
   kt::Log log(__FUNCTION__);

   log.branch("%.*s", int(Path.size()), Path.data());

   if (Path.empty()) return log.warning(ERR::NullArgs);

   std::string resolved;
   if (ResolvePath(Path, RSF::NIL, &resolved) != ERR::Okay) return log.warning(ERR::ResolvePath);

   kt::ScopedObjectLock<objConfig> config(glConfig, 5000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   std::string_view file_name(resolved);
   if (auto sep = file_name.find_last_of("/\\"); sep != std::string_view::npos) file_name.remove_prefix(sep + 1);

   scalable_face face;
   if (auto error = analyse_scalable_font(glFTLibrary, resolved, file_name, face); error != ERR::Okay) {
      return log.warning(error);
   }

   auto font = std::find_if(glPrivateFonts.begin(), glPrivateFonts.end(), [&face](const private_font &Font) {
      return iequals(Font.name, face.family);
   });

   if (font IS glPrivateFonts.end()) font = glPrivateFonts.insert(glPrivateFonts.end(), { .name = face.family });

   if (face.variable) font->meta |= FMETA::VARIABLE;

   for (auto &style : face.styles) {
      auto it = std::find_if(font->styles.begin(), font->styles.end(), [&style](const auto &Entry) {
         return iequals(Entry.first, style);
      });

      if (it != font->styles.end()) it->second = resolved;
      else font->styles.emplace_back(style, resolved);
   }

   log.msg("Registered family \"%s\" with %d style(s).", font->name.c_str(), int(face.styles.size()));

   if (Family) *Family = font->name;
   return ERR::Okay;
}

} // namespace

//********************************************************************************************************************
// Sorts the families of a font database and creates a style list for each one, e.g.
//
//    Bold Italic = fonts:fixed/courier.fon
//    Bold = fonts:scalable/Courier Prime Bold.ttf
//    Styles = Bold,Bold Italic,Italic,Regular
//
// Style keys are identified by their values, which are either fonts: paths or absolute host paths.  No other key in a
// family group has a value that resembles a path.

static bool is_style_path(std::string_view Value)
{
   if (Value.starts_with("fonts:") or Value.starts_with('/')) return true;
#ifdef _WIN32
   if (Value.starts_with("\\\\")) return true; // UNC path
   if ((Value.size() >= 3) and (std::isalpha((unsigned char)Value[0])) and (Value[1] IS ':') and
       ((Value[2] IS '\\') or (Value[2] IS '/'))) return true; // Drive path, e.g. C:\Windows\Fonts\arial.ttf
#endif
   return false;
}

static ERR build_style_lists(objConfig *Config)
{
   kt::Log log(__FUNCTION__);

   if (auto error = Config->sortByKey("", false); error != ERR::Okay) return error; // Sort by font name.

   ConfigGroups *groups;
   if (Config->getGroups(groups) != ERR::Okay) return log.warning(ERR::GetField);

   for (auto & [group, keys] : *groups) {
      std::list <std::string> styles;
      for (auto & [k, v] : keys) {
         if (is_style_path(v)) styles.push_front(k);
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

   return ERR::Okay;
}

//********************************************************************************************************************
// Merges a host font database into glConfig, which must be locked by the caller.  Host families that clash with a
// family in glConfig are removed first, so that a family provided by the fonts: volume is never mixed with host files.
// This can occur if a family is added to the fonts: volume after the host was scanned.

static void merge_host_fonts(objConfig *Host)
{
   kt::Log log(__FUNCTION__);

   ConfigGroups *local, *host;
   if ((glConfig->getGroups(local) != ERR::Okay) or (Host->getGroups(host) != ERR::Okay)) return;

   std::vector<std::string> clashes;
   for (auto & [group, keys] : *host) {
      auto clash = std::ranges::any_of(*local, [&group](const ConfigGroup &Local) {
         return iequals(Local.first, group);
      });
      if (clash) clashes.push_back(group);
   }

   for (auto &name : clashes) {
      log.detail("Host family \"%s\" is provided by fonts:, skipping.", name.c_str());
      Host->deleteGroup(name);
   }

   glConfig->merge(Host);
   glConfig->sortByKey("", false);
}

//********************************************************************************************************************
// Builds only the per-user host database.  Saving the cache is optional during module initialisation; the scanned
// fonts are still merged when saving fails.  The fonts: database is never written here.

static ERR refresh_host_fonts(void)
{
   kt::Log log(__FUNCTION__);
   kt::ScopedObjectLock<objConfig> config(glConfig, 3000);
   if (not config.granted()) return log.warning(ERR::AccessObject);

   ConfigGroups *groups;
   if (glConfig->getGroups(groups) != ERR::Okay) return log.warning(ERR::GetField);
   std::map<std::string, family_details> local;
   for (auto &[family, keys] : *groups) local.try_emplace(family);

   objConfig::create host = { };
   if (host.ok()) {
      std::map<std::string, family_details> details;
      scan_host_folders(*host, local, details);
      write_family_details(*host, details);

      ERR error;
      if (error = build_style_lists(*host); error IS ERR::Okay) {
         objFile::create host_file = { fl::Path(HOST_FONTS_PATH), fl::Flags(FL::NEW|FL::WRITE) };
         if (host_file.ok()) error = host->saveToObject(*host_file);
         else error = log.warning(ERR::OpenFile);

         merge_host_fonts(*host);
      }

      return error;
   }
   else return log.warning(ERR::CreateObject);
}

//********************************************************************************************************************
// Decode a UTF-16 big endian SFNT name to UTF-8.  A space is inserted wherever a capital follows a lower-case letter,
// so that a style name such as 'SemiBold' is presented as 'Semi Bold'.

static std::string decode_sfnt_name(const FT_SfntName &Name)
{
   char buffer[100];
   int out = 0;
   const FT_Byte *str = Name.string; // Read as bytes: the string has no uint16_t storage or alignment guarantee
   uint16_t prev_unicode = 0;
   for (FT_UInt i=0; (i < Name.string_len>>1) and (out < std::ssize(buffer)-8); i++) {
      uint16_t unicode = uint16_t((str[i*2]<<8) | str[(i*2)+1]);
      if ((unicode >= 'A') and (unicode <= 'Z')) {
         if ((i > 0) and (prev_unicode >= 'a') and (prev_unicode <= 'z')) {
            buffer[out++] = ' ';
         }
      }
      out += UTF8WriteValue(unicode, std::span<int8_t>((int8_t *)(buffer + out), std::size(buffer) - size_t(out)));
      prev_unicode = unicode;
   }
   return std::string(buffer, out);
}

//********************************************************************************************************************
// Convert an SFNT tag to its four character string, e.g. 'wght'.

static std::string tag_string(FT_ULong Tag)
{
   return std::string({ char(Tag >> 24), char((Tag >> 16) & 0xff), char((Tag >> 8) & 0xff), char(Tag & 0xff) });
}

//********************************************************************************************************************
// Classify a face as Serif, Sans, Mono, Script, Display or Symbol from its OS/2 table.  PANOSE is preferred, with the
// IBM family class as a fallback.  An empty result means that the font does not declare a classification.

static std::string_view classify_face(FT_Face Face)
{
   if (FT_IS_FIXED_WIDTH(Face)) return "Mono";

   auto os2 = (TT_OS2 *)FT_Get_Sfnt_Table(Face, FT_SFNT_OS2);
   if ((not os2) or (os2->version IS 0xffff)) return { };

   // PANOSE: https://monotype.github.io/panose/pan1.htm

   auto &panose = os2->panose;
   switch (panose[0]) { // Family kind
      case 2: // Latin Text
         if (panose[3] IS 9) return "Mono"; // Proportion: monospaced
         if ((panose[1] >= 11) and (panose[1] <= 15)) return "Sans"; // Serif style: sans variants, flared, rounded
         if ((panose[1] >= 2) and (panose[1] <= 10)) return "Serif";
         break;
      case 3: return "Script";  // Latin Hand Written
      case 4: return "Display"; // Latin Decorative
      case 5: return "Symbol";  // Latin Symbol
   }

   // IBM font family class: https://learn.microsoft.com/en-us/typography/opentype/spec/ibmfc

   switch (os2->sFamilyClass >> 8) {
      case 1: case 2: case 3: case 4: case 5: case 7: return "Serif";
      case 8:  return "Sans";
      case 9:  return "Display"; // Ornamentals
      case 10: return "Script";
      case 12: return "Symbol";
   }

   return { };
}

//********************************************************************************************************************
// Test for a serif at the foot of a glyph's stem.  A serif glyph is considerably wider just above the baseline than at
// the middle of its stem, while a sans glyph is a plain stem.  Slices are compared rather than the bounding box, so
// that the slant of an italic does not affect the result.  Returns false if the glyph cannot be measured.

static bool measure_foot(FT_Face Face, FT_ULong Char, bool &Serif)
{
   auto glyph = FT_Get_Char_Index(Face, Char);
   if (not glyph) return false;
   if (FT_Load_Glyph(Face, glyph, FT_LOAD_NO_SCALE|FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP)) return false;
   if (Face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;

   auto &outline = Face->glyph->outline;
   if (outline.n_points < 4) return false;

   FT_BBox box;
   FT_Outline_Get_CBox(&outline, &box);
   if (box.yMax <= box.yMin) return false;

   // Return the horizontal extent of the outline at height Y.  Control points are treated as polygon vertices, which
   // is sufficient for the straight edges of a stem.

   auto slice_width = [&outline](double Y) {
      double left = DBL_MAX, right = -DBL_MAX;
      int start = 0;
      for (int c=0; c < outline.n_contours; c++) {
         int end = outline.contours[c];
         for (int i=start; i <= end; i++) {
            auto &a = outline.points[i];
            auto &b = outline.points[(i IS end) ? start : i + 1];
            if ((a.y <= Y) != (b.y <= Y)) {
               double x = a.x + ((Y - a.y) * double(b.x - a.x) / double(b.y - a.y));
               left  = std::min(left, x);
               right = std::max(right, x);
            }
         }
         start = end + 1;
      }
      return (right > left) ? right - left : 0.0;
   };

   const double height = box.yMax - box.yMin;
   const double stem = slice_width(box.yMin + (height * 0.5));
   const double foot = slice_width(box.yMin + (height * 0.02));
   if ((stem <= 0) or (foot <= 0)) return false;
   Serif = foot / stem > 1.5;
   return true;
}

//********************************************************************************************************************
// Classify a face that does not declare a classification by measuring its capital I and T.  Both must have serifs,
// as pixel and display faces often give only the I serifs to distinguish it from l.

static std::string_view measure_serifs(FT_Face Face)
{
   bool serif_i, serif_t;
   if (not measure_foot(Face, 'I', serif_i)) return { };
   if (not measure_foot(Face, 'T', serif_t)) return { };
   return (serif_i and serif_t) ? "Serif" : "Sans";
}

//********************************************************************************************************************
// Add the feature tags of a GSUB or GPOS table to Features.  Only the table header and feature records are read, as
// the layout tables of large fonts can be sizeable.

static void read_layout_features(FT_Face Face, FT_ULong Table, std::set<std::string> &Features)
{
   auto read = [Face, Table](FT_Long Offset, std::span<FT_Byte> Buffer) {
      FT_ULong length = Buffer.size();
      return not FT_Load_Sfnt_Table(Face, Table, Offset, Buffer.data(), &length);
   };

   std::array<FT_Byte, 8> header;
   if (not read(0, header)) return;

   FT_Long list = (header[6] << 8) | header[7]; // FeatureList offset
   if (not list) return;

   std::array<FT_Byte, 2> count_bytes;
   if (not read(list, count_bytes)) return;

   const int count = (count_bytes[0] << 8) | count_bytes[1];
   if (not count) return;

   std::vector<FT_Byte> records(size_t(count) * 6); // Tag (4 bytes), Feature offset (2 bytes)
   if (not read(list + 2, records)) return;

   for (int i=0; i < count; i++) {
      std::string tag((const char *)records.data() + (i * 6), 4);
      if (std::all_of(tag.begin(), tag.end(), [](char C) { return (C >= 0x20) and (C < 0x7f); })) {
         kt::rtrim(tag);
         if (not tag.empty()) Features.insert(tag);
      }
   }
}

//********************************************************************************************************************
// Gather the classification, variable axis ranges, layout features and character coverage of a face.

static void analyse_face_details(FT_Library Library, FT_Face Face, family_details &Details)
{
   Details.analysed = true;
   Details.cls      = classify_face(Face);
   if (Details.cls.empty()) Details.cls = measure_serifs(Face);
   Details.regular  = not (Face->style_flags & (FT_STYLE_FLAG_ITALIC|FT_STYLE_FLAG_BOLD));

   if (FT_HAS_MULTIPLE_MASTERS(Face)) {
      FT_MM_Var *mvar;
      if (not FT_Get_MM_Var(Face, &mvar)) {
         for (FT_UInt a=0; a < mvar->num_axis; a++) {
            auto &axis = mvar->axis[a];
            Details.axes.push_back({ tag_string(axis.tag), axis.minimum / 65536.0, axis.def / 65536.0,
               axis.maximum / 65536.0 });
         }
         FT_Done_MM_Var(Library, mvar);
      }
   }

   read_layout_features(Face, TTAG_GSUB, Details.features);
   read_layout_features(Face, TTAG_GPOS, Details.features);

   FT_ULong kern_length = 0; // A legacy kern table provides kerning without a GPOS 'kern' feature
   if ((not FT_Load_Sfnt_Table(Face, TTAG_kern, 0, nullptr, &kern_length)) and (kern_length > 0)) {
      Details.features.insert("kern");
   }

   // Count the mapped characters of each coverage set.  FT_Get_Next_Char() walks the active charmap, which FreeType
   // selects as a Unicode map where the font provides one.

   if ((Face->charmap) and (Face->charmap->encoding IS FT_ENCODING_UNICODE)) {
      FT_UInt glyph;
      for (auto code = FT_Get_First_Char(Face, &glyph); glyph; code = FT_Get_Next_Char(Face, code, &glyph)) {
         Details.characters++;
         for (int i=0; i < COVERAGE_SETS; i++) {
            if (glCoverageSets[i].contains(uint32_t(code))) Details.coverage[i]++;
         }
      }
   }
}

//********************************************************************************************************************
// Merge the details of another file of the same family.  Axis ranges, features and coverage are combined, while the
// classification of an upright regular face takes precedence.

void family_details::merge(const family_details &Other)
{
   if (not Other.analysed) return;

   if ((not analysed) or (cls.empty()) or ((Other.regular) and (not regular) and (not Other.cls.empty()))) {
      cls = Other.cls;
      regular = Other.regular;
   }

   for (auto &other : Other.axes) {
      auto it = std::find_if(axes.begin(), axes.end(), [&other](const axis_range &Axis) {
         return Axis.tag IS other.tag;
      });

      if (it IS axes.end()) axes.push_back(other);
      else {
         it->min = std::min(it->min, other.min);
         it->max = std::max(it->max, other.max);
         if (Other.regular) it->def = other.def;
      }
   }

   features.insert(Other.features.begin(), Other.features.end());
   for (int i=0; i < COVERAGE_SETS; i++) coverage[i] = std::max(coverage[i], Other.coverage[i]);
   characters = std::max(characters, Other.characters);
   analysed = true;
}

//********************************************************************************************************************
// Analyse a scalable font file and return its family name and the style names that it provides.  FileName is used to
// name the family if the file does not declare one.  If Details is true, the descriptive information for the font
// database is also gathered.  FaceIndex selects a face in a collection, and Face.faces reports the collection size.
// Library must be exclusive to the calling thread.  Callers using glFTLibrary must hold the glConfig lock.

static ERR analyse_scalable_font(FT_Library Library, const std::string &Path, std::string_view FileName,
   scalable_face &Face, bool Details, int FaceIndex)
{
   kt::Log log(__FUNCTION__);

   FT_Face ftface;
   FT_Open_Args open = { .flags = FT_OPEN_PATHNAME, .pathname = (FT_String *)Path.c_str() };
   if (FT_Open_Face(Library, &open, FaceIndex, &ftface)) return ERR::InvalidData;

   Face.faces = int(ftface->num_faces);

   if (not FT_IS_SCALABLE(ftface)) { // Sanity check
      FT_Done_Face(ftface);
      return ERR::NoSupport;
   }

   log.msg("Detected font file \"%s\", name: %s, style: %s", Path.c_str(), ftface->family_name, ftface->style_name);

   if (ftface->family_name) Face.family.assign(ftface->family_name);
   else Face.family.assign(FileName.substr(0, FileName.find('.')));

   // Strip any style references out of the font name and keep them as style flags

   FTF style = FTF::NIL;
   if (ftface->style_name) {
      if (auto pos = Face.family.find(" Bold"); pos != std::string::npos) {
         Face.family.replace(pos, 5, "");
         style |= FTF::BOLD;
      }

      if (auto pos = Face.family.find(" Italic"); pos != std::string::npos) {
         Face.family.replace(pos, 7, "");
         style |= FTF::ITALIC;
      }
   }

   kt::rtrim(Face.family);

   if (FT_HAS_MULTIPLE_MASTERS(ftface)) { // A single ttf file can contain multiple named styles
      Face.variable = true;

      FT_MM_Var *mvar;
      if (not FT_Get_MM_Var(ftface, &mvar)) {
         FT_UInt index;
         if (not FT_Get_Default_Named_Instance(ftface, &index)) {
            auto name_table_size = FT_Get_Sfnt_Name_Count(ftface);
            for (FT_UInt s=0; s < mvar->num_namedstyles; s++) {
               for (int n=int(name_table_size)-1; n >= 0; n--) {
                  FT_SfntName sft_name;
                  if (not FT_Get_Sfnt_Name(ftface, n, &sft_name)) {
                     if (sft_name.name_id IS mvar->namedstyle[s].strid) {
                        Face.styles.push_back(decode_sfnt_name(sft_name));
                        break;
                     }
                  }
               }
            }
         }

         FT_Done_MM_Var(Library, mvar);
      }
   }
   else if ((ftface->style_name) and (not iequals("regular", ftface->style_name))) {
      Face.styles.push_back(ftface->style_name);
   }
   else if (style IS FTF::BOLD) Face.styles.push_back("Bold");
   else if (style IS FTF::ITALIC) Face.styles.push_back("Italic");
   else if (style IS (FTF::BOLD|FTF::ITALIC)) Face.styles.push_back("Bold Italic");

   // Some fonts pad their style names with whitespace, which would not survive a round trip through the config file.

   for (auto &name : Face.styles) kt::trim(name);
   std::erase_if(Face.styles, [](const std::string &Name) { return Name.empty(); });

   if (Face.styles.empty()) Face.styles.push_back("Regular");

   if (Details) analyse_face_details(Library, ftface, Face.details);

   FT_Done_Face(ftface);
   return ERR::Okay;
}

//********************************************************************************************************************
// Analyse files in parallel, retaining discovery order for the later database merge.  Each worker has its own
// FreeType library; the calling thread participates using glFTLibrary while holding the glConfig lock.  If a worker
// cannot initialise FreeType, the remaining threads still consume all files.  Collections are only scanned for host
// fonts, preserving the fonts: volume's existing use of face zero.

static void analyse_scalable_files(std::vector<scalable_file> &Files, bool Collections = false)
{
   if (Files.empty()) return;

   std::atomic<size_t> next_file = 0;
   auto analyse = [&Files, &next_file, Collections](FT_Library Library) {
      while (true) {
         auto index = next_file.fetch_add(1, std::memory_order_relaxed);
         if (index >= Files.size()) break;

         auto &file = Files[index];
         int faces = 1;
         for (int face_index=0; face_index < faces; face_index++) {
            scalable_face face;
            auto error = analyse_scalable_font(Library, file.path, file.name, face, true, face_index);
            if ((Collections) and (face_index IS 0)) faces = face.faces;
            if (error IS ERR::Okay) file.faces.emplace_back(face_index, std::move(face));
         }
      }
   };

   // Limit simultaneous file reads and FreeType libraries on machines with many logical CPUs.

   auto worker_count = std::min(Files.size(), size_t(std::clamp(std::thread::hardware_concurrency(), 1u, 8u)));
   std::vector<std::jthread> workers;
   workers.reserve(worker_count - 1);
   for (size_t i=1; i < worker_count; i++) {
      workers.emplace_back([&analyse]() {
         FT_Library library = nullptr;
         if (FT_Init_FreeType(&library)) return;
         analyse(library);
         FT_Done_FreeType(library);
      });
   }

   analyse(glFTLibrary);
   // workers join before analyse, next_file or Files can go out of scope or be read by the caller.
}

//********************************************************************************************************************

static void scan_scalable_folder(objConfig *Config, std::map<std::string, family_details> &Details)
{
   kt::Log log(__FUNCTION__);

   log.branch("Scanning for scalable fonts.");

   std::string sfpath;
   if (!ResolvePath("fonts:scalable/", RSF::NO_FILE_CHECK, &sfpath)) {
      DirInfo *dir;
      if (!OpenDir(sfpath, RDF::FILE, &dir)) {
         LocalResource free_dir(dir);

         std::vector<scalable_file> files;
         while (!ScanDir(dir)) {
            files.push_back({ .path = sfpath + dir->Info->Name, .name = dir->Info->Name });
         }

         analyse_scalable_files(files);
         for (auto &file : files) {
            if (file.faces.empty()) continue;
            auto &face = file.faces.front().second;
            std::string path = "fonts:scalable/" + file.name;

            Config->write(face.family, "Name", face.family);
            Config->write(face.family, "Scalable", "Yes");
            if (face.variable) Config->write(face.family, "Variable", "Yes");
            for (auto &style : face.styles) Config->write(face.family, style, path);
            Details[face.family].merge(face.details);
         }
      }
      else log.warning("Failed to open the fonts:scalable/ directory.");
   }
}

//********************************************************************************************************************
// Converts a filesystem path to a UTF-8 string.  On Windows, path::string() converts to the active code page and can
// throw, whereas u8string() is always lossless.

static std::string path_to_utf8(const std::filesystem::path &Path)
{
   auto u8 = Path.u8string();
   return std::string((const char *)u8.data(), u8.size());
}

//********************************************************************************************************************
// Returns the host's native font folders in order of precedence.  Relative XDG paths are invalid according to the XDG
// Base Directory specification and are ignored.  On Windows, per-user fonts (Windows 10 1809 and later) take precedence
// over the system font folder.

static std::vector<std::filesystem::path> host_font_folders(void)
{
   std::vector<std::filesystem::path> folders;
#ifdef _WIN32
   if (auto local = _wgetenv(L"LOCALAPPDATA"); (local) and (local[0])) {
      std::filesystem::path local_path(local);
      if (local_path.is_absolute()) folders.emplace_back(local_path / L"Microsoft" / L"Windows" / L"Fonts");
   }

   const wchar_t *windir = _wgetenv(L"SystemRoot");
   if ((not windir) or (not windir[0])) windir = _wgetenv(L"WINDIR");
   if ((windir) and (windir[0]) and (std::filesystem::path(windir).is_absolute())) {
      folders.emplace_back(std::filesystem::path(windir) / L"Fonts");
   }
   else folders.emplace_back(L"C:\\Windows\\Fonts");
#elif defined(__linux__)
   auto home = getenv("HOME");
   bool valid_home = (home) and (home[0] IS '/');

   if (auto data_home = getenv("XDG_DATA_HOME"); (data_home) and (data_home[0] IS '/')) {
      folders.emplace_back(std::filesystem::path(data_home) / "fonts");
   }
   else if (valid_home) folders.emplace_back(std::filesystem::path(home) / ".local/share/fonts");

   if (valid_home) folders.emplace_back(std::filesystem::path(home) / ".fonts");

   std::string_view data_dirs = "/usr/local/share:/usr/share";
   if (auto env = getenv("XDG_DATA_DIRS"); (env) and (env[0])) data_dirs = env;

   while (not data_dirs.empty()) {
      auto end = data_dirs.find(':');
      auto dir = data_dirs.substr(0, end);
      if (dir.starts_with('/')) folders.emplace_back(std::filesystem::path(dir) / "fonts");
      data_dirs = (end IS std::string_view::npos) ? std::string_view() : data_dirs.substr(end + 1);
   }

   folders.emplace_back("/usr/share/fonts");
   folders.emplace_back("/usr/local/share/fonts");
#endif
   return folders;
}

//********************************************************************************************************************
// Recursively scans the host's native font folders for scalable fonts.  Families already provided by the fonts: volume
// are skipped so that local and host files are never mixed.  Folders and files are tracked by their canonical paths,
// which prevents symbolic links from causing loops or duplicate entries.

static void scan_host_folders(objConfig *Config, const std::map<std::string, family_details> &Local,
   std::map<std::string, family_details> &Details)
{
   kt::Log log(__FUNCTION__);

   log.branch("Scanning host font folders.");

   std::vector<std::string> local_families;
   for (auto &[family, details] : Local) local_families.push_back(family);

   auto is_local = [&local_families](std::string_view Family) {
      return std::ranges::any_of(local_families, [Family](const std::string &Local) { return iequals(Family, Local); });
   };

   std::vector<scalable_file> files;
   std::set<std::filesystem::path> visited;
   const auto options = std::filesystem::directory_options::follow_directory_symlink |
      std::filesystem::directory_options::skip_permission_denied;

   for (auto &root : host_font_folders()) {
      std::error_code err;
      auto canonical_root = std::filesystem::canonical(root, err);
      if ((err) or (not std::filesystem::is_directory(canonical_root, err))) continue;
      if (not visited.insert(canonical_root).second) continue;

      log.msg("Scanning folder \"%s\"", path_to_utf8(canonical_root).c_str());

      std::error_code iter_err;
      std::filesystem::recursive_directory_iterator it(canonical_root, options, iter_err), end;
      for (; (not iter_err) and (it != end); it.increment(iter_err)) {
         auto &entry = *it;

         if (entry.is_directory(err)) {
            auto folder = std::filesystem::canonical(entry.path(), err);
            if ((err) or (not visited.insert(folder).second)) it.disable_recursion_pending();
            continue;
         }

         if (not entry.is_regular_file(err)) continue;

         auto ext = path_to_utf8(entry.path().extension());
         if ((not iequals(ext, ".ttf")) and (not iequals(ext, ".otf")) and
             (not iequals(ext, ".ttc")) and (not iequals(ext, ".otc"))) continue;

         auto file = std::filesystem::canonical(entry.path(), err);
         if ((err) or (not visited.insert(file).second)) continue;

         files.push_back({ .path = path_to_utf8(file), .name = path_to_utf8(file.filename()) });
      }

      if (iter_err) {
         log.warning("Scan of \"%s\" ended early: %s", path_to_utf8(canonical_root).c_str(),
            iter_err.message().c_str());
      }
   }

   analyse_scalable_files(files, true);
   std::set<std::pair<std::string, std::string>> host_styles; // Family and style pairs recorded so far
   for (auto &file : files) {
      for (auto &[face_index, face] : file.faces) {
         if (is_local(face.family)) {
            log.detail("Family \"%s\" is provided by fonts:, skipping \"%s\"", face.family.c_str(), file.path.c_str());
            continue;
         }

         bool recorded = false;
         for (auto &style : face.styles) {
            if (not host_styles.emplace(face.family, style).second) continue;
            Config->write(face.family, style, file.path);
            if (face_index > 0) Config->write(face.family, "FaceIndex:" + style, std::to_string(face_index));
            recorded = true;
         }

         if (not recorded) continue;

         Config->write(face.family, "Name", face.family);
         Config->write(face.family, "Scalable", "Yes");
         if (face.variable) Config->write(face.family, "Variable", "Yes");
         Details[face.family].merge(face.details);
      }
   }
}

//********************************************************************************************************************

static void scan_fixed_folder(objConfig *Config, std::map<std::string, family_details> &Details)
{
   kt::Log log(__FUNCTION__);

   log.branch("Scanning for fixed fonts.");

   DirInfo *dir;
   if (!OpenDir("fonts:fixed/", RDF::FILE, &dir)) {
      LocalResource free_dir(dir);

      while (!ScanDir(dir)) {
         std::string location("fonts:fixed/");
         location.append(dir->Info->Name);

         winfnt_header_fields header;
         std::vector<uint16_t> points;
         std::string facename;
         if (!analyse_bmp_font(location, &header, facename, points)) {
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

            // The classification is declared by the family bits of the pitch and family field, as defined for the
            // Windows LOGFONT structure.  Bitmap fonts map their characters to Latin-1 for rendering.

            family_details details;
            details.analysed = true;
            details.regular  = style IS FTF::NIL;
            if ((header.pixel_width) or (not (header.pitch_and_family & 1))) details.cls = "Mono";
            else switch (header.pitch_and_family & 0xf0) {
               case 0x10: details.cls = "Serif"; break;   // FF_ROMAN
               case 0x20: details.cls = "Sans"; break;    // FF_SWISS
               case 0x30: details.cls = "Mono"; break;    // FF_MODERN
               case 0x40: details.cls = "Script"; break;  // FF_SCRIPT
               case 0x50: details.cls = "Display"; break; // FF_DECORATIVE
            }

            for (uint32_t code=header.first_char; code <= header.last_char; code++) {
               details.characters++;
               for (int i=0; i < COVERAGE_SETS; i++) {
                  if (glCoverageSets[i].contains(code)) details.coverage[i]++;
               }
            }

            Details[group].merge(details);
         }
         else log.warning("Failed to analyse %.*s", int(location.size()), location.data());
      }
   }
   else log.warning("Failed to scan directory fonts:fixed/");
}

//********************************************************************************************************************
// Write the merged details of each family to the font database.  Coverage is expressed as a percentage of each set,
// rounded down so that 100 always indicates full coverage.  Sets with no coverage are omitted.

static void write_family_details(objConfig *Config, const std::map<std::string, family_details> &Details)
{
   for (auto &[family, details] : Details) {
      if (not details.analysed) continue;

      if (not details.cls.empty()) Config->write(family, "Class", details.cls);

      if (not details.axes.empty()) {
         std::ostringstream tags, ranges;
         for (size_t a=0; a < details.axes.size(); a++) {
            auto &axis = details.axes[a];
            if (a) { tags << ','; ranges << ','; }
            tags << axis.tag;
            ranges << axis.tag << ':' << axis.min << ':' << axis.def << ':' << axis.max;
         }
         Config->write(family, "Axes", tags.str());
         Config->write(family, "AxisRanges", ranges.str());
      }

      if (not details.features.empty()) {
         std::ostringstream out;
         for (auto it = details.features.begin(); it != details.features.end(); it++) {
            if (it != details.features.begin()) out << ',';
            out << *it;
         }
         Config->write(family, "Features", out.str());
      }

      std::ostringstream coverage;
      for (int i=0; i < COVERAGE_SETS; i++) {
         if (not details.coverage[i]) continue;
         auto pct = std::max(1, (details.coverage[i] * 100) / glCoverageSets[i].total());
         if (coverage.tellp() > 0) coverage << ',';
         coverage << glCoverageSets[i].id << ':' << pct;
      }
      if (coverage.tellp() > 0) Config->write(family, "Coverage", coverage.str());

      Config->write(family, "Characters", std::to_string(details.characters));
   }
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
         return ERR::NoData;
      }
      else if (error != ERR::Okay) return error;

      // Read font point sizes

      for (auto &font : fonts) {
         file->seekStart(font.Offset);
         if (!file->read(std::span<int8_t>((int8_t *)Header, sizeof(winfnt_header_fields)))) {
            Points.push_back(Header->nominal_point_size);
         }
      }

      // Go to the first font in the file and read the font header

      file->seekStart(fonts[0].Offset);

      if (file->read(std::span<int8_t>((int8_t *)Header, sizeof(winfnt_header_fields))) != ERR::Okay) return ERR::Read;

      if (auto error = validate_winfnt_header(*Header, Path); error != ERR::Okay) return error;

      // Extract the name of the font

      file->seekStart(fonts[0].Offset + Header->face_name_offset);

      int i;
      for (i=0; (size_t)i < sizeof(face)-1; i++) {
         ERR result = file->read(std::span<int8_t>((int8_t *)face + i, 1));
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

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_font_module() { return &ModHeader; }
