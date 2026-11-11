#pragma once

#include <kotuku/main.h>

#include <ft2build.h>
#include FT_SIZES_H
#include FT_MULTIPLE_MASTERS_H
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_OUTLINE_H

#include "kerning.h"
#include "gsub.h"

#define CF_BITMAP 0
#define CF_FREETYPE 1

class common_font {
public:
   int type;
   common_font(int pType) : type(pType) { }
};

//********************************************************************************************************************

struct CaseInsensitiveMap {
   bool operator() (const std::string &lhs, const std::string &rhs) const {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
   }
};

//********************************************************************************************************************
// Fonts are stored independently of VectorText objects so that they can be permanently cached.

class bmp_font : public common_font {
public:
   objFont *font = nullptr;

   bmp_font() : common_font(CF_BITMAP) { }
   bmp_font(objFont *pFont) : font(pFont), common_font(CF_BITMAP) { }

   ~bmp_font() {
      if (font) { FreeResource(font); font = nullptr; }
   }
};

//********************************************************************************************************************
// Fonts are stored independently of VectorText objects so that they can be permanently cached.

class freetype_font {
   public:
      struct glyph {
         agg::path_storage path; // AGG vector path generated from the freetype glyph
         double adv_x, adv_y;    // Pixel advances, these values should not be rounded
         int   glyph_index;     // Freetype glyph index; saves having to call a function for conversion
      };

      using METRIC_GROUP = std::vector<FT_Fixed>;

      struct axis_info { // A variation axis of the face, in design units
         uint32_t tag;
         double minimum, def, maximum;
      };
      using GLYPH_TABLE = ankerl::unordered_dense::map<uint32_t, glyph>; // Glyph index to outline and advances

      class ft_point : public common_font {
         public:
            GLYPH_TABLE glyphs;
            freetype_font *font = nullptr;
            FT_Size ft_size = nullptr;

            // These values are measured in the same pixel units as the font size (VectorText FontSize).  Like the
            // glyph outlines, they are independent of the display density.
            //
            // It is widely acknowledged that the metrics declared by font creators or their tools may not be
            // the precise glyph metrics in reality...

            double height;  // Full height equivalent to ascent + descent, does not include accents
            double ascent;  // Ascent from the baseline (cap height), does not include accents.  Typically matches the font-size in pixels
            double descent; // Number of pixels allocated below the baseline for glyphs, not including vertical whitespace
            double line_spacing; // The vertical advance for a newline, if not predefined then is set to height * 1.15
            double em_scale = 0; // Pixels per font unit, matching the scale of the glyph outlines
            METRIC_GROUP axis;
            gpos_kerning::instance kern_instance; // GPOS kerning state for this style instance
            std::vector<int> normalised_axes; // F2Dot14 coordinates shared by GSUB and GPOS
            bool axes_ready = false;
            bool kern_ready = false; // True once kern_instance has the design coordinates of this style
            bool kerning_enabled = true; // False if the font configuration sets Kerning = No
            int16_t size = false; // True if the size is below SMALL_TEXT_LIMIT
            FT_Pos embolden_x = 0, embolden_y = 0; // Outline emboldening for small sizes, in 26.6 pixels
            int16_t weight = 400; // Weight of the style (100 - 1000), see detect_weight()

            static constexpr int SMALL_TEXT_LIMIT = 10; // Special rules apply to text at or below this size
            static constexpr int EMBOLDEN_TEXT_LIMIT = 14;
            static constexpr int EMBOLDEN_TEXT_FULL  = 8;
            static constexpr int CONTRAST_WEIGHT_LIMIT = 300; // Small text above this weight is not boosted

            glyph & get_glyph_by_index(uint32_t);
            glyph & get_glyph(uint32_t Unicode) {
               return get_glyph_by_index(FT_Get_Char_Index(font->face, Unicode));
            }
            std::span<const int> normalised_coordinates();
            gpos_kerning::adjustment positioning(int PrevGlyph, int Glyph);
            double kerning(int PrevGlyph, int Glyph) {
               auto value = positioning(PrevGlyph, Glyph);
               return value.first_advance + value.second_advance;
            }

            double first_placement(int Glyph, int NextGlyph) {
               return positioning(Glyph, NextGlyph).first_placement;
            }

            double first_placement(int Glyph, const std::string_view &Remaining) {
               if (Remaining.empty()) return 0;
               int length;
               auto unicode = UTF8ReadValue(Remaining, &length);
               if (unicode IS '\n') return 0;
               return first_placement(Glyph, FT_Get_Char_Index(font->face, unicode));
            }

            ft_point() : common_font(CF_FREETYPE) { }

            // Overrides are metric options from a family alias, which take precedence over the family's own.

            ft_point(freetype_font &pFont, METRIC_GROUP &pMetrics, int pSize, const ConfigKeys &Overrides)
               : common_font(CF_FREETYPE) {
               font = &pFont;
               set_axis(pMetrics);
               FT_Set_Var_Design_Coordinates(pFont.face, axis.size(), axis.data());
               pFont.active_size = this;
               set_size(pSize, Overrides);
            }

            ft_point(freetype_font &pFont, int pSize, const ConfigKeys &Overrides) : common_font(CF_FREETYPE) {
               font = &pFont;
               set_size(pSize, Overrides);
            }

            ~ft_point() {
               // FT_Done_Face() will remove all FT_New_Size() allocations, interfering with
               // manual calls to FT_Done_Size()
               //
               //if (ft_size) { FT_Done_Size(ft_size); ft_size = NULL; }
            }

            void set_size(int Size, const ConfigKeys &Overrides) {
               if (!FT_New_Size(font->face, &ft_size)) {
                  FT_Activate_Size(ft_size);

                  // FreeType runs at 72 DPI so that 1pt = 1px and the em is exactly Size pixels.  The glyph outlines
                  // in get_glyph() and generate_text() are produced at this same scale.

                  FT_Set_Char_Size(font->face, 0, Size<<6, 72, 72);

                  // Kerning is computed from font units and scaled without rounding.

                  if (font->face->units_per_EM) em_scale = double(Size) / double(font->face->units_per_EM);

                  size = Size;
                  double embolden[2] = { 0, 0 };

                  // The FreeType metrics are scaled by a fixed 72/96 (DISPLAY_DPI), which acts as a calibration
                  // constant.  The display density must not be used here: the glyph outlines ignore it, so a
                  // density-dependent factor would shrink the metrics relative to the glyphs on high DPI displays,
                  // causing overlapping lines and misplaced bounds, carets and vertical alignment.  The Ascent,
                  // Descent and LineSpacing modifiers in the font options.cfg were tuned with this factor in place,
                  // so changing it requires those values to be re-tuned.

                  const double metric_scale = 72.0 / DISPLAY_DPI;

                  if FT_HAS_VERTICAL(ft_size->face) {
                     line_spacing = std::trunc(int26p6_to_dbl(ft_size->face->max_advance_height) * metric_scale);
                  }
                  else {
                     auto advance = ft_size->metrics.height + std::abs(ft_size->metrics.descender);
                     line_spacing = std::trunc(int26p6_to_dbl(advance) * metric_scale * 1.15);
                  }

                  ascent  = int26p6_to_dbl(ft_size->metrics.ascender) * metric_scale;
                  descent = std::abs(int26p6_to_dbl(ft_size->metrics.descender)) * metric_scale;

                  // Check if the client has applied metrics modifiers for this font.  The font module
                  // comes with a tool, font_metrics.tiri, for helping with these modifiers.  Options from the
                  // family's group are overridden by those of the alias that it was selected through, if any.

                  auto apply_options = [this, &embolden](const ConfigKeys &Keys) {
                     if (auto it = Keys.find("LineSpacing"); it != Keys.end()) {
                        line_spacing *= std::stod(it->second);
                     }
                     if (auto it = Keys.find("Descent"); it != Keys.end()) {
                        descent *= std::stod(it->second);
                     }
                     if (auto it = Keys.find("Ascent"); it != Keys.end()) {
                        ascent *= std::stod(it->second);
                     }
                     if (auto it = Keys.find("Kerning"); it != Keys.end()) {
                        kerning_enabled = not (kt::iequals(it->second, "No") or
                           kt::iequals(it->second, "False") or (it->second IS "0"));
                     }
                     if (auto it = Keys.find("Embolden"); it != Keys.end()) {
                        // Embolden = X[, Y] gives the strength in pixels at SMALL_TEXT_FULL and below.
                        // A missing Y defaults to a third of X.
                        double x = 0, y = -1;
                        if (sscanf(it->second.c_str(), "%lf , %lf", &x, &y) >= 1) {
                           if (y < 0) y = x / 3.0;
                           embolden[0] = std::max(0.0, x);
                           embolden[1] = std::max(0.0, y);
                        }
                     }
                  };

                  ConfigKeys options;
                  if (glFontConfig) {
                     kt::ScopedObjectLock<objConfig> config(glFontConfig, 500);
                     if (config.granted()) {
                        APTR group_ptr;
                        if (!glFontConfig->getData(group_ptr)) {
                           auto groups = (ConfigGroups *)group_ptr;
                           for (auto & [group, keys] : groups[0]) {
                              if (kt::iequals(group, font->face->family_name)) {
                                 options = keys;
                                 break;
                              }
                              else if (auto it = keys.find("Name"); it != keys.end()) {
                                 if (kt::iequals(it->second, font->face->family_name)) {
                                    options = keys;
                                    break;
                                 }
                              }
                           }
                        }
                     }
                  }

                  for (auto & [key, value] : Overrides) options[key] = value;
                  apply_options(options);

                  height  = ascent + descent;

                  if (Size < EMBOLDEN_TEXT_LIMIT) {
                     const double range = EMBOLDEN_TEXT_LIMIT - EMBOLDEN_TEXT_FULL;
                     auto taper = std::clamp(double(EMBOLDEN_TEXT_LIMIT - Size) / range, 0.0, 1.0);
                     embolden_x = FT_Pos(std::lround(embolden[0] * taper * 64.0));
                     embolden_y = FT_Pos(std::lround(embolden[1] * taper * 64.0));
                  }

                  weight = detect_weight();
               }
            }

            // Returns the weight of this style on the CSS scale of 100 - 1000.  A variable face reports the value of
            // its wght axis, otherwise the OS/2 weight class is used.  The OS/2 value of a variable face describes
            // its default instance only, so the axis takes precedence.

            int detect_weight() {
               static constexpr FT_ULong TAG_WGHT = FT_MAKE_TAG('w', 'g', 'h', 't');
               for (size_t i=0; (i < font->axes.size()) and (i < axis.size()); i++) {
                  if (font->axes[i].tag IS TAG_WGHT) return int(std::lround(double(axis[i]) / 65536.0));
               }

               if (auto os2 = (TT_OS2 *)FT_Get_Sfnt_Table(font->face, FT_SFNT_OS2); os2 and (os2->version != 0xffff)) {
                  // Some legacy fonts use the 1 - 9 scale of early Windows versions.
                  if ((os2->usWeightClass >= 1) and (os2->usWeightClass <= 9)) return os2->usWeightClass * 100;
                  if (os2->usWeightClass >= 100) return std::min(int(os2->usWeightClass), 1000);
               }

               return (font->face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
            }

            inline void set_axis(METRIC_GROUP &metrics) {
               axis = metrics;
            }

            // Small text receives a coverage boost, except for heavier weights whose stems are already wide enough
            // to fill a pixel column; boosting them closes the counters.

            inline bool increase_contrast() {
               if (embolden_x or embolden_y) return false;
               return size <= SMALL_TEXT_LIMIT or ((size <= EMBOLDEN_TEXT_LIMIT) and (weight <= CONTRAST_WEIGHT_LIMIT));
            }
      };

      using SIZE_CACHE = std::map<int, ft_point>; // font-size = glyph cache
      using STYLE_CACHE = std::map<std::string, SIZE_CACHE, CaseInsensitiveMap>;
      using METRIC_TABLE = std::map<std::string, METRIC_GROUP, CaseInsensitiveMap>;

   public:
      FT_Face face = nullptr;
      STYLE_CACHE style_cache; // Lists all known styles and contains the glyph cache for each style
      METRIC_TABLE metrics; // For variable fonts, these are pre-defined metrics with style names
      std::vector<axis_info> axes; // For variable fonts, the variation axes in face order
      FMETA meta = FMETA::NIL;
      int glyph_flags = 0;
      uint32_t family_hash = 0; // strihash() of the resolved family name that first loaded this face
      ft_point *active_size = nullptr;
      gpos_kerning gpos; // GPOS pair kerning, loaded on first use by get_gpos()
      bool gpos_loaded = false;
      gsub_table gsub; // Opt-in substitution, independent of size and variable instance
      bool gsub_loaded = false;

      freetype_font()  { }
      freetype_font(FT_Face pFace, STYLE_CACHE &pStyles, METRIC_TABLE &pMetrics, std::vector<axis_info> &pAxes,
         FMETA pMeta, uint32_t pFamilyHash, HINT pHint)
         : face(pFace), style_cache(pStyles), metrics(pMetrics), axes(pAxes), meta(pMeta), family_hash(pFamilyHash) {
         set_hinting(pHint);
      }

      // Selects the glyph load flags.  A HINT value overrides the Hinting option from the font configuration, while
      // HINT::NIL reverts to it.  Cached glyphs are not affected; see clear_glyphs().

      void set_hinting(HINT Hint) {
         if ((Hint IS HINT::INTERNAL) or ((Hint IS HINT::NIL) and ((meta & FMETA::HINT_INTERNAL) != FMETA::NIL))) {
            glyph_flags = FT_LOAD_TARGET_NORMAL|FT_LOAD_FORCE_AUTOHINT;
         }
         else if ((Hint IS HINT::LIGHT) or ((Hint IS HINT::NIL) and ((meta & FMETA::HINT_LIGHT) != FMETA::NIL))) {
            glyph_flags = FT_LOAD_TARGET_LIGHT;
         }
         else { // HINT::NORMAL and the default use the font's hinting information (FT_LOAD_TARGET_NORMAL is zero)
            glyph_flags = FT_LOAD_DEFAULT;
            if (stub_hinted(face)) glyph_flags |= FT_LOAD_NO_AUTOHINT;
         }
      }

      // Returns the GPOS kerning reader for the face, loading it on the first call.  A face without usable GPOS
      // kerning returns a disabled reader.

      gpos_kerning & get_gpos() {
         if (not gpos_loaded) {
            gpos_loaded = true;
            if (FT_IS_SFNT(face)) gpos.load(load_sfnt_table(TTAG_GPOS), load_sfnt_table(TTAG_GDEF));
         }
         return gpos;
      }

      gsub_table & get_gsub() {
         if (not gsub_loaded) {
            gsub_loaded = true;
            if (FT_IS_SFNT(face)) {
               gsub.load(load_sfnt_table(TTAG_GSUB), load_sfnt_table(TTAG_GDEF), face->num_glyphs);
            }
         }
         return gsub;
      }

      std::vector<uint8_t> load_sfnt_table(FT_ULong Tag) {
         FT_ULong size = 0;
         if (FT_Load_Sfnt_Table(face, Tag, 0, nullptr, &size) or (not size)) return {};
         std::vector<uint8_t> table(size);
         if (FT_Load_Sfnt_Table(face, Tag, 0, table.data(), &size)) return {};
         return table;
      }

      // Discards the cached glyphs of every style and size, forcing them to be reloaded with the current flags.

      void clear_glyphs() {
         for (auto & [style, sizes] : style_cache) {
            for (auto & [size, point] : sizes) point.glyphs.clear();
         }
      }

      // FreeType 2.14 auto-hints TrueType fonts that have no 'fpgm' table and a 'prep' table of up to 7 bytes, where
      // 2.13 required both to be empty.  Unhinted fonts that ship a stub 'prep' (e.g. Noto Sans) would otherwise
      // switch to the auto-hinter, whose hinted advances change letter spacing.  This keeps such fonts on the native
      // loader to retain the original glyph layout.

      static bool stub_hinted(FT_Face Face) {
         if ((not FT_IS_SFNT(Face)) or (not FT_IS_SCALABLE(Face))) return false;

         // A null buffer with a valid length pointer only queries the table size.

         FT_ULong fpgm_size = 0, prep_size = 0, glyf_size = 0;
         FT_Load_Sfnt_Table(Face, TTAG_fpgm, 0, nullptr, &fpgm_size);
         FT_Load_Sfnt_Table(Face, TTAG_prep, 0, nullptr, &prep_size);
         if ((fpgm_size != 0) or (prep_size > 7)) return false;

         FT_Load_Sfnt_Table(Face, TTAG_glyf, 0, nullptr, &glyf_size);
         if (glyf_size IS 0) return false; // Not glyf-based, e.g. CFF or bitmap-only

         auto maxp = (TT_MaxProfile *)FT_Get_Sfnt_Table(Face, FT_SFNT_MAXP);
         return (prep_size > 0) or (maxp and (maxp->maxSizeOfInstructions > 0));
      }

      ~freetype_font();
};

extern ERR get_font(kt::Log &Log, std::string_view, std::string_view, int, int, common_font **);

// Caching note: Although it is policy for cached fonts to be permanently retained, it is not necessary for the
// glyphs themselves to be permanently cached.  Future resource management should therefore actively remove
// glyphs that have gone stale.

extern std::recursive_mutex glFontMutex;
extern ankerl::unordered_dense::map<uint32_t, std::unique_ptr<bmp_font>> glBitmapFonts;
extern ankerl::unordered_dense::map<uint32_t, std::unique_ptr<freetype_font>> glFreetypeFonts;
extern ankerl::unordered_dense::map<uint32_t, HINT> glHintOverrides; // Family name hash to SetFontHinting() value

extern FT_Library glFTLibrary;
