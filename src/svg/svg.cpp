/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

Relevant SVG reference manuals:

https://www.w3.org/TR/SVGColor12/
https://www.w3.org/TR/SVG11/
https://www.w3.org/Graphics/SVG/Test/Overview.html

*********************************************************************************************************************/

//#define DEBUG
#define PRV_SVG
#include <unordered_map>
#include <string>
#include <sstream>
#include <charconv>
#include <list>
#include <variant>
#include <algorithm>
#include <cfloat>
#include <kotuku/main.h>
#include <kotuku/modules/picture.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/vector.h>
#include <kotuku/modules/display.h>
#include <kotuku/strings.hpp>
#include "svg_def.c"
#include <katana.h>
#include <math.h>
#include "../link/base64.h"

using namespace pf;

JUMPTABLE_CORE
JUMPTABLE_DISPLAY
JUMPTABLE_VECTOR

static OBJECTPTR clSVG = nullptr, clRSVG = nullptr, modDisplay = nullptr, modVector = nullptr, modPicture = nullptr;
static double glDisplayHDPI = 96, glDisplayVDPI = 96, glDisplayDPI = 96;

struct prvSVG { // Private variables for RSVG
   class objSVG *SVG;
};

struct svgInherit {
   OBJECTPTR Object;
   std::string ID;
};

struct svgLink {
   std::string ref;
};

struct svgID { // All elements using the 'id' attribute will be registered with one of these structures.
   int TagIndex;

   svgID(const int pTagIndex) {
      TagIndex = pTagIndex;
   }

   svgID() { TagIndex = -1; }
};

struct svgAnimState {
   VectorMatrix *matrix = nullptr;
   std::vector<class anim_transform *> transforms;
};

#include <kotuku/modules/svg.h>

class extSVG;
struct svgState;

#include "anim.h"

//********************************************************************************************************************

class extSVG : public objSVG {
   public:
   FUNCTION FrameCallback;
   ankerl::unordered_dense::map<std::string, XTag *> IDs;
   ankerl::unordered_dense::map<std::string, objFilterEffect *> Effects; // All effects, registered by their SVG identifier.
   double SVGVersion;
   double AnimEpoch;  // Epoch time for the animations.
   objXML *XML;
   objVectorScene *Scene;
   std::string Folder;
   std::string Colour = "rgb(0,0,0)"; // Default colour, used for 'currentColor' references
   class objVectorViewport *Viewport; // First viewport (the <svg> tag) to be created on parsing the SVG document.
   std::list<std::variant<anim_transform, anim_motion, anim_value>> Animations; // NB: Pointer stability is a container requirement
   ankerl::unordered_dense::map<OBJECTID, svgAnimState> Animatrix; // For animated transforms, a vector may have one matrix only.
   std::vector<std::unique_ptr<svgLink>> Links;
   std::vector<svgInherit> Inherit;
   std::vector<OBJECTID> Resources; // Resources to terminate if ENFORCE_TRACKING was enabled.
   ankerl::unordered_dense::map<uint32_t, std::vector<anim_base *>> StartOnBegin; // When the animation indicated by uint32_t begins, it must activate() the referenced anim_base
   ankerl::unordered_dense::map<uint32_t, std::vector<anim_base *>> StartOnEnd; // When the animation indicated by uint32_t ends, it must activate() the referenced anim_base
   TIMER AnimationTimer;
   int16_t  Cloning;  // Incremented when inside a duplicated tag space, e.g. due to a <use> tag
   bool  PreserveWS; // Preserve white-space
};

struct svgState {
   enum class DU : uint8_t {
      NIL = 0,
      PIXEL,  // px
      SCALED, // %: Scale to fill empty space
   };

   // Field Unit.  Makes it user to define field values that could be fixed or scaled.

   struct FUNIT {
      svgState *state;
      FIELD field_id;
      double value;
      DU type;

      constexpr FUNIT() noexcept : state(nullptr), field_id(0), value(0), type(DU::NIL) { }

      // With field
      explicit FUNIT(svgState *pState, FIELD pField, double pValue, DU pType = DU::NIL) noexcept : state(pState), field_id(pField), value(pValue), type(pType) { }

      FUNIT(svgState *pState, FIELD pField, const std::string &pValue, DU pType = DU::NIL, double pMin = -DBL_MAX) noexcept :
         FUNIT(pState, pValue, pType, pMin) { field_id = pField; }

      FUNIT(svgState *pState, FIELD pField, std::string_view pValue, DU pType = DU::NIL, double pMin = -DBL_MAX) noexcept :
         FUNIT(pState, pValue, pType, pMin) { field_id = pField; }

      // Without field
      explicit FUNIT(svgState *pState, double pValue, DU pType = DU::PIXEL) noexcept : state(pState), value(pValue), type(pType) { }

      FUNIT(svgState *pState, std::string pValue, DU pType = DU::NIL, double pMin = -DBL_MAX) noexcept :
         FUNIT(pState, std::string_view(pValue), pType, pMin) { }

      FUNIT(svgState *pState, std::string_view pValue, DU pType = DU::NIL, double pMin = -DBL_MAX) noexcept;

      constexpr bool empty() const noexcept { return (type == DU::NIL) || (!value); }
      constexpr void clear() noexcept { value = 0; type = DU::PIXEL; }

      operator double() const noexcept { return value; }
      operator DU() const noexcept { return type; }

      inline int64_t field() const noexcept {
         return (type == DU::SCALED) ? (field_id | TDOUBLE | TSCALE) : field_id | TDOUBLE;
      }

      inline bool valid_size() const noexcept { // Return true if this is a valid width/height
         return (value >= 0.001);
      }

      inline ERR set(OBJECTPTR Object) const noexcept {
         return Object->set(field_id, Unit(value, (type == DU::SCALED) ? (FD_DOUBLE|FD_SCALED) : FD_DOUBLE));
      }
   };

   std::string m_color;       // currentColor value, initialised to SVG.Colour
   std::string m_stop_color;
   std::string m_fill;        // Defaults to rgb(0,0,0)
   std::string m_stroke;      // Empty by default
   std::string m_font_size;
   std::string m_font_family;
   std::string m_display;
   std::string m_visibility;
   double  m_font_size_px;    // Converted from m_font_size
   double  m_stroke_width;    // 0 if undefined
   double  m_fill_opacity;    // -1 if undefined
   double  m_opacity;         // -1 if undefined
   double  m_stop_opacity;    // -1 if undefined
   int    m_font_weight;     // 0 if undefined
   RQ      m_path_quality;    // RQ::AUTO default
   VLJ     m_line_join;
   VLC     m_line_cap;
   VIJ     m_inner_join;

private:
   class extSVG *Self;
   objVectorScene *Scene;

   inline FUNIT UNIT(FIELD pField, double pValue, DU pType = DU::NIL) { return FUNIT(this, pField, pValue, pType); };
   inline FUNIT UNIT(FIELD pField, const std::string &pValue, DU pType = DU::NIL) { return FUNIT(this, pField, pValue, pType); };
   inline FUNIT UNIT(double pValue, DU pType = DU::PIXEL) { return FUNIT(this, pValue, pType); }
   inline FUNIT UNIT(const std::string &pValue, DU pType = DU::PIXEL) { return FUNIT(this, pValue, pType); }

public:
   svgState(class extSVG *pSVG) : m_color(pSVG->Colour), m_fill("rgb(0,0,0)"), m_font_size("12pt"), m_font_family("Noto Sans"), m_font_size_px(16), m_stroke_width(0),
      m_fill_opacity(-1), m_opacity(-1), m_stop_opacity(-1), m_font_weight(0), m_path_quality(RQ::AUTO),
      m_line_join(VLJ::NIL), m_line_cap(VLC::NIL), m_inner_join(VIJ::NIL),
      Self(pSVG), Scene(pSVG->Scene) { }

   void process_children(XTag &, OBJECTPTR) noexcept;
   void proc_svg(XTag &, OBJECTPTR, objVector *&) noexcept;

private:
   void applyTag(const XTag &) noexcept;
   void applyStateToVector(objVector *) const noexcept;
   const std::vector<GradientStop> process_gradient_stops(const XTag &) noexcept;
   ERR  set_property(objVector *, uint32_t, XTag &, const std::string) noexcept;
   ERR  process_tag(XTag &, XTag &, OBJECTPTR, objVector * &) noexcept;

   ERR  proc_defs(XTag &, OBJECTPTR) noexcept;
   ERR  proc_set(XTag &, XTag &, OBJECTPTR) noexcept;
   ERR  proc_animate(XTag &, XTag &, OBJECTPTR) noexcept;
   ERR  proc_animate_colour(XTag &, XTag &, OBJECTPTR) noexcept;
   ERR  proc_animate_motion(XTag &, OBJECTPTR) noexcept;
   ERR  proc_animate_transform(XTag &, OBJECTPTR) noexcept;
   void proc_def_image(XTag &) noexcept;
   void proc_filter(XTag &) noexcept;
   void proc_group(XTag &, OBJECTPTR, objVector * &) noexcept;
   ERR  proc_image(XTag &, OBJECTPTR, objVector * &) noexcept;
   void proc_link(XTag &, OBJECTPTR, objVector * &Vector) noexcept;
   void proc_mask(XTag &) noexcept;
   void proc_pathtransition(XTag &) noexcept;
   void proc_pattern(XTag &) noexcept;
   ERR  proc_shape(CLASSID, XTag &, OBJECTPTR, objVector * &) noexcept;
   void proc_switch(XTag &, OBJECTPTR, objVector * &) noexcept;
   void proc_use(XTag &, OBJECTPTR) noexcept;
   void proc_clippath(XTag &) noexcept;
   void proc_morph(XTag &Tag, OBJECTPTR Parent) noexcept;
   ERR  proc_style(XTag &);
   void proc_symbol(XTag &Tag) noexcept;
   ERR  proc_conicgradient(const XTag &) noexcept;
   ERR  proc_contourgradient(const XTag &) noexcept;
   ERR  proc_diamondgradient(const XTag &) noexcept;
   ERR  proc_lineargradient(const XTag &) noexcept;
   ERR  proc_radialgradient(const XTag &) noexcept;

   void process_attrib(XTag &, objVector *) noexcept;
   void process_inherit_refs(XTag &) noexcept;
   void process_shape_children(XTag &, OBJECTPTR) noexcept;
   ERR  set_paint_server(objVector *, FIELD, const std::string);
   ERR  current_colour(objVector *, FRGB &) noexcept;

   void parse_contourgradient(const XTag &, objVectorGradient *, std::string &) noexcept;
   void parse_diamondgradient(const XTag &, objVectorGradient *, std::string &) noexcept;
   void parse_lineargradient(const XTag &, objVectorGradient *, std::string &) noexcept;
   void parse_radialgradient(const XTag &, objVectorGradient &, std::string &) noexcept;

   ERR  parse_fe_blur(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_colour_matrix(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_component_xfer(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_composite(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_convolve_matrix(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_displacement_map(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_flood(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_image(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_lighting(objVectorFilter *, XTag &, LT) noexcept;
   ERR  parse_fe_merge(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_morphology(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_offset(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_source(objVectorFilter * , XTag &) noexcept;
   ERR  parse_fe_turbulence(objVectorFilter *, XTag &) noexcept;
   ERR  parse_fe_wavefunction(objVectorFilter *, XTag &) noexcept;
};

//********************************************************************************************************************

static ERR  animation_timer(extSVG *, int64_t, int64_t);
static void convert_styles(objXML::TAGS &);
static double read_unit(std::string_view &, int64_t * = nullptr);

static ERR  init_svg(void);
static ERR  init_rsvg(void);

static void process_rule(extSVG *, objXML::TAGS &, KatanaRule *);

static ERR  save_svg_scan(extSVG *, objXML *, objVector *, int);
static ERR  save_svg_defs(extSVG *, objXML *, objVectorScene *, int);
static ERR  save_svg_scan_std(extSVG *, objXML *, objVector *, int);
static ERR  save_svg_transform(VectorMatrix *, std::stringstream &);

// Tracking for resources like patterns, gradients, filters, etc.  If ENFORCE_TRACKING is enabled, these resources 
// will be automatically terminated when the SVG is terminated.  Otherwise, they will persist until the client 
// terminates them or the process ends.

inline void track_object(extSVG *SVG, OBJECTPTR Object)
{
   if ((SVG->Flags & SVF::ENFORCE_TRACKING) != SVF::NIL) {
      SVG->Resources.emplace_back(Object->UID);
   }
}

//********************************************************************************************************************

#include "funit.cpp"
#include "utility.cpp"
#include "save_svg.cpp"

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("vector", &modVector, &VectorBase) != ERR::Okay) return ERR::InitModule;

   if (init_svg() != ERR::Okay) return ERR::AddClass;

   if (objModule::load("picture", &modPicture) IS ERR::Okay) { // RSVG has a Picture class dependency
      if (init_rsvg() != ERR::Okay) return ERR::AddClass;
   }

   update_dpi();

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (modDisplay) { FreeResource(modDisplay); modDisplay = nullptr; }
   if (modVector)  { FreeResource(modVector);  modVector = nullptr; }
   if (modPicture) { FreeResource(modPicture); modPicture = nullptr; }

   if (clSVG)  { FreeResource(clSVG);  clSVG = nullptr; }
   if (clRSVG) { FreeResource(clRSVG); clRSVG = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_svg.cpp"
#include "class_rsvg.cpp"

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_svg_module() { return &ModHeader; }