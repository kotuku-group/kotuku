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
#include <format>
#include <sstream>
#include <list>
#include <variant>
#include <cfloat>
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/vector.h>
#include <kotuku/modules/display.h>
#include <kotuku/strings.hpp>
#include "svg_def.c"
#include <katana.h>
#include "../link/base64.h"
#include "../xml/uri_utils.h"

using namespace kt;

JUMPTABLE_CORE
JUMPTABLE_DISPLAY
JUMPTABLE_VECTOR

static OBJECTPTR clSVG = nullptr, clRSVG = nullptr, modDisplay = nullptr, modVector = nullptr, modImage = nullptr;
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

struct SVGStringHash {
   using is_avalanching = void;
   using is_transparent = void;

   [[nodiscard]] size_t operator()(std::string_view Value) const noexcept {
      return ankerl::unordered_dense::hash<std::string_view>{}(Value);
   }

   [[nodiscard]] size_t operator()(const std::string &Value) const noexcept {
      return (*this)(std::string_view(Value));
   }

   [[nodiscard]] size_t operator()(CSTRING Value) const noexcept {
      return (*this)(std::string_view(Value));
   }
};

template<typename Value> using SVGStringLookupMap =
   ankerl::unordered_dense::map<std::string, Value, SVGStringHash, std::equal_to<>>;

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
   SVGStringLookupMap<XTag *> IDs;
   ankerl::unordered_dense::map<std::string, objFilterEffect *> Effects; // All effects, registered by their SVG identifier.
   double SVGVersion;
   double AnimEpoch;  // Epoch time for the animations.
   objXML *XML;
   std::string Folder;
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

   extSVG() {
      #ifdef __ANDROID__
         FrameRate = 30; // Choose a lower frame rate for Android devices, so as to minimise power consumption.
      #else
         FrameRate = 60;
      #endif
      Colour = "rgb(0,0,0)"; // Default colour, used for 'currentColor' references
   }

   ~extSVG();
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
   int     m_font_weight;     // 0 if undefined
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
   const kt::vector<GradientStop> process_gradient_stops(const XTag &) noexcept;
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
   ERR  proc_distalgradient(const XTag &) noexcept;
   ERR  proc_lineargradient(const XTag &) noexcept;
   ERR  proc_radialgradient(const XTag &) noexcept;

   void process_attrib(XTag &, objVector *) noexcept;
   void process_inherit_refs(XTag &) noexcept;
   void process_shape_children(XTag &, OBJECTPTR) noexcept;
   ERR  set_paint_server(objVector *, FIELD, const std::string);
   bool current_colour(objVector *, FRGB &) noexcept;

   void parse_contourgradient(const XTag &, objGradientContour *, std::string &) noexcept;
   void parse_conicgradient(const XTag &, objGradientConic *, std::string &) noexcept;
   void parse_diamondgradient(const XTag &, objGradientDiamond *, std::string &) noexcept;
   void parse_distalgradient(const XTag &, objGradientDistal *, std::string &) noexcept;
   void parse_lineargradient(const XTag &, objGradientLinear *, std::string &) noexcept;
   void parse_radialgradient(const XTag &, objGradientRadial *, std::string &) noexcept;
   bool parse_gradient_href(const std::string &, objGradient *) noexcept;
   void parse_gradient_hrefs(const XTag &, objGradient *, bool &) noexcept;
   void parse_gradient_defaults(kt::Log &, const XTag &, objGradient *, uint32_t, const std::string &,
      const std::string &, std::string &) noexcept;
   void set_gradient_stops(const XTag &, objGradient *, bool) noexcept;

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

static constexpr auto SVF_a                   = strhash("a");
static constexpr auto SVF_accumulate          = strhash("accumulate");
static constexpr auto SVF_achromatomaly       = strhash("achromatomaly");
static constexpr auto SVF_achromatopsia       = strhash("achromatopsia");
static constexpr auto SVF_additive            = strhash("additive");
static constexpr auto SVF_align               = strhash("align");
static constexpr auto SVF_amplitude           = strhash("amplitude");
static constexpr auto SVF_animate             = strhash("animate");
static constexpr auto SVF_animateColor        = strhash("animateColor");
static constexpr auto SVF_animateColour       = strhash("animateColour");
static constexpr auto SVF_animateMotion       = strhash("animateMotion");
static constexpr auto SVF_animateTransform    = strhash("animateTransform");
static constexpr auto SVF_append_path         = strhash("append-path");
static constexpr auto SVF_appendPath          = strhash("appendPath");
static constexpr auto SVF_arithmetic          = strhash("arithmetic");
static constexpr auto SVF_atop                = strhash("atop");
static constexpr auto SVF_attributeName       = strhash("attributeName");
static constexpr auto SVF_attributeType       = strhash("attributeType");
static constexpr auto SVF_azimuth             = strhash("azimuth");
static constexpr auto SVF_b                   = strhash("b");
static constexpr auto SVF_BackgroundAlpha     = strhash("BackgroundAlpha");
static constexpr auto SVF_BackgroundImage     = strhash("BackgroundImage");
static constexpr auto SVF_baseFrequency       = strhash("baseFrequency");
static constexpr auto SVF_baseProfile         = strhash("baseProfile");
static constexpr auto SVF_begin               = strhash("begin");
static constexpr auto SVF_bevel               = strhash("bevel");
static constexpr auto SVF_bias                = strhash("bias");
static constexpr auto SVF_blink               = strhash("blink");
static constexpr auto SVF_bold                = strhash("bold");
static constexpr auto SVF_bolder              = strhash("bolder");
static constexpr auto SVF_bottom              = strhash("bottom");
static constexpr auto SVF_brightness          = strhash("brightness");
static constexpr auto SVF_burn                = strhash("burn");
static constexpr auto SVF_butt                = strhash("butt");
static constexpr auto SVF_by                  = strhash("by");
static constexpr auto SVF_calcMode            = strhash("calcMode");
static constexpr auto SVF_circle              = strhash("circle");
static constexpr auto SVF_class               = strhash("class");
static constexpr auto SVF_clip                = strhash("clip");
static constexpr auto SVF_clip_path           = strhash("clip-path");
static constexpr auto SVF_clip_rule           = strhash("clip-rule");
static constexpr auto SVF_clipPath            = strhash("clipPath");
static constexpr auto SVF_clipPathUnits       = strhash("clipPathUnits");
static constexpr auto SVF_close               = strhash("close");
static constexpr auto SVF_color               = strhash("color");
static constexpr auto SVF_color_interpolation = strhash("color-interpolation");
static constexpr auto SVF_color_interpolation_filters = strhash("color-interpolation-filters");
static constexpr auto SVF_colormap            = strhash("colormap");
static constexpr auto SVF_colour              = strhash("colour");
static constexpr auto SVF_colour_interpolation = strhash("colour-interpolation");
static constexpr auto SVF_colourise           = strhash("colourise");
static constexpr auto SVF_colourmap           = strhash("colourmap");
static constexpr auto SVF_condensed           = strhash("condensed");
static constexpr auto SVF_conicGradient       = strhash("conicGradient");
static constexpr auto SVF_contourGradient     = strhash("contourGradient");
static constexpr auto SVF_contrast            = strhash("contrast");
static constexpr auto SVF_crossOrigin         = strhash("crossOrigin");
static constexpr auto SVF_cx                  = strhash("cx");
static constexpr auto SVF_cy                  = strhash("cy");
static constexpr auto SVF_d                   = strhash("d");
static constexpr auto SVF_darken              = strhash("darken");
static constexpr auto SVF_decay               = strhash("decay");
static constexpr auto SVF_decoding            = strhash("decoding");
static constexpr auto SVF_defs                = strhash("defs");
static constexpr auto SVF_desaturate          = strhash("desaturate");
static constexpr auto SVF_desc                = strhash("desc");
static constexpr auto SVF_deuteranomaly       = strhash("deuteranomaly");
static constexpr auto SVF_deuteranopia        = strhash("deuteranopia");
static constexpr auto SVF_diamondGradient     = strhash("diamondGradient");
static constexpr auto SVF_difference          = strhash("difference");
static constexpr auto SVF_diffuseConstant     = strhash("diffuseConstant");
static constexpr auto SVF_discrete            = strhash("discrete");
static constexpr auto SVF_display             = strhash("display");
static constexpr auto SVF_divisor             = strhash("divisor");
static constexpr auto SVF_dodge               = strhash("dodge");
static constexpr auto SVF_dur                 = strhash("dur");
static constexpr auto SVF_dx                  = strhash("dx");
static constexpr auto SVF_dy                  = strhash("dy");
static constexpr auto SVF_edgeMode            = strhash("edgeMode");
static constexpr auto SVF_elevation           = strhash("elevation");
static constexpr auto SVF_ellipse             = strhash("ellipse");
static constexpr auto SVF_enable_background   = strhash("enable-background");
static constexpr auto SVF_end                 = strhash("end");
static constexpr auto SVF_exclusion           = strhash("exclusion");
static constexpr auto SVF_expanded            = strhash("expanded");
static constexpr auto SVF_exponent            = strhash("exponent");
static constexpr auto SVF_externalResourcesRequired = strhash("externalResourcesRequired");
static constexpr auto SVF_extra_condensed     = strhash("extra-condensed");
static constexpr auto SVF_extra_expanded      = strhash("extra-expanded");
static constexpr auto SVF_feBlend             = strhash("feBlend");
static constexpr auto SVF_feBlur              = strhash("feBlur");
static constexpr auto SVF_feColorMatrix       = strhash("feColorMatrix");
static constexpr auto SVF_feColourMatrix      = strhash("feColourMatrix");
static constexpr auto SVF_feComponentTransfer = strhash("feComponentTransfer");
static constexpr auto SVF_feComposite         = strhash("feComposite");
static constexpr auto SVF_feConvolveMatrix    = strhash("feConvolveMatrix");
static constexpr auto SVF_feDiffuseLighting   = strhash("feDiffuseLighting");
static constexpr auto SVF_feDisplacementMap   = strhash("feDisplacementMap");
static constexpr auto SVF_feDistantLight      = strhash("feDistantLight");
static constexpr auto SVF_feDropShadow        = strhash("feDropShadow");
static constexpr auto SVF_feFlood             = strhash("feFlood");
static constexpr auto SVF_feGaussianBlur      = strhash("feGaussianBlur");
static constexpr auto SVF_feImage             = strhash("feImage");
static constexpr auto SVF_feMerge             = strhash("feMerge");
static constexpr auto SVF_feMergeNode         = strhash("feMergeNode");
static constexpr auto SVF_feMorphology        = strhash("feMorphology");
static constexpr auto SVF_feOffset            = strhash("feOffset");
static constexpr auto SVF_fePointLight        = strhash("fePointLight");
static constexpr auto SVF_feSpecularLighting  = strhash("feSpecularLighting");
static constexpr auto SVF_feSpotLight         = strhash("feSpotLight");
static constexpr auto SVF_feTile              = strhash("feTile");
static constexpr auto SVF_feTurbulence        = strhash("feTurbulence");
static constexpr auto SVF_feWaveFunction      = strhash("feWaveFunction");
static constexpr auto SVF_fill                = strhash("fill");
static constexpr auto SVF_fill_opacity        = strhash("fill-opacity");
static constexpr auto SVF_fill_rule           = strhash("fill-rule");
static constexpr auto SVF_FillPaint           = strhash("FillPaint");
static constexpr auto SVF_filter              = strhash("filter");
static constexpr auto SVF_filterRes           = strhash("filterRes");
static constexpr auto SVF_filterUnits         = strhash("filterUnits");
static constexpr auto SVF_flood_color         = strhash("flood-color");
static constexpr auto SVF_flood_colour        = strhash("flood-colour");
static constexpr auto SVF_flood_opacity       = strhash("flood-opacity");
static constexpr auto SVF_focalPoint          = strhash("focalPoint");
static constexpr auto SVF_font                = strhash("font");
static constexpr auto SVF_font_family         = strhash("font-family");
static constexpr auto SVF_font_size           = strhash("font-size");
static constexpr auto SVF_font_size_adjust    = strhash("font-size-adjust");
static constexpr auto SVF_font_stretch        = strhash("font-stretch");
static constexpr auto SVF_font_style          = strhash("font-style");
static constexpr auto SVF_font_variant        = strhash("font-variant");
static constexpr auto SVF_font_weight         = strhash("font-weight");
static constexpr auto SVF_frequency           = strhash("frequency");
static constexpr auto SVF_floor               = strhash("floor");
static constexpr auto SVF_from                = strhash("from");
static constexpr auto SVF_fx                  = strhash("fx");
static constexpr auto SVF_fy                  = strhash("fy");
static constexpr auto SVF_g                   = strhash("g");
static constexpr auto SVF_gamma               = strhash("gamma");
static constexpr auto SVF_gradientTransform   = strhash("gradientTransform");
static constexpr auto SVF_gradientUnits       = strhash("gradientUnits");
static constexpr auto SVF_hardLight           = strhash("hardLight");
static constexpr auto SVF_height              = strhash("height");
static constexpr auto SVF_href                = strhash("href");
static constexpr auto SVF_hue                 = strhash("hue");
static constexpr auto SVF_hueRotate           = strhash("hueRotate");
static constexpr auto SVF_id                  = strhash("id");
static constexpr auto SVF_identity            = strhash("identity");
static constexpr auto SVF_image               = strhash("image");
static constexpr auto SVF_image_rendering     = strhash("image-rendering");
static constexpr auto SVF_in                  = strhash("in");
static constexpr auto SVF_in2                 = strhash("in2");
static constexpr auto SVF_inherit             = strhash("inherit");
static constexpr auto SVF_intercept           = strhash("intercept");
static constexpr auto SVF_invert              = strhash("invert");
static constexpr auto SVF_invertRGB           = strhash("invertRGB");
static constexpr auto SVF_invertXAxis         = strhash("invertXAxis");
static constexpr auto SVF_invertYAxis         = strhash("invertYAxis");
static constexpr auto SVF_isolation_mode      = strhash("isolation-mode");
static constexpr auto SVF_jag                 = strhash("jag");
static constexpr auto SVF_join_path           = strhash("join-path");
static constexpr auto SVF_joinPath            = strhash("joinPath");
static constexpr auto SVF_k1                  = strhash("k1");
static constexpr auto SVF_k2                  = strhash("k2");
static constexpr auto SVF_k3                  = strhash("k3");
static constexpr auto SVF_k4                  = strhash("k4");
static constexpr auto SVF_kernelMatrix        = strhash("kernelMatrix");
static constexpr auto SVF_kernelUnitLength    = strhash("kernelUnitLength");
static constexpr auto SVF_kerning             = strhash("kerning");
static constexpr auto SVF_keyPoints           = strhash("keyPoints");
static constexpr auto SVF_keySplines          = strhash("keySplines");
static constexpr auto SVF_keyTimes            = strhash("keyTimes");
static constexpr auto SVF_kotuku_morph        = strhash("kotuku:morph");
static constexpr auto SVF_kotuku_pathTransition = strhash("kotuku:pathTransition");
static constexpr auto SVF_kotuku_shape        = strhash("kotuku:shape");
static constexpr auto SVF_kotuku_spiral       = strhash("kotuku:spiral");
static constexpr auto SVF_kotuku_transition   = strhash("kotuku:transition");
static constexpr auto SVF_kotuku_wave         = strhash("kotuku:wave");
static constexpr auto SVF_l                   = strhash("l");
static constexpr auto SVF_lengthAdjust        = strhash("lengthAdjust");
static constexpr auto SVF_letter_spacing      = strhash("letter-spacing");
static constexpr auto SVF_lighten             = strhash("lighten");
static constexpr auto SVF_lighter             = strhash("lighter");
static constexpr auto SVF_lighting_color      = strhash("lighting-color");
static constexpr auto SVF_lighting_colour     = strhash("lighting-colour");
static constexpr auto SVF_limitingConeAngle   = strhash("limitingConeAngle");
static constexpr auto SVF_line                = strhash("line");
static constexpr auto SVF_line_through        = strhash("line-through");
static constexpr auto SVF_linear              = strhash("linear");
static constexpr auto SVF_linearGradient      = strhash("linearGradient");
static constexpr auto SVF_loop_limit          = strhash("loop-limit");
static constexpr auto SVF_loopLimit           = strhash("loopLimit");
static constexpr auto SVF_luminanceToAlpha    = strhash("luminanceToAlpha");
static constexpr auto SVF_m                   = strhash("m");
static constexpr auto SVF_marker              = strhash("marker");
static constexpr auto SVF_marker_end          = strhash("marker-end");
static constexpr auto SVF_marker_mid          = strhash("marker-mid");
static constexpr auto SVF_marker_start        = strhash("marker-start");
static constexpr auto SVF_mask                = strhash("mask");
static constexpr auto SVF_maskContentUnits    = strhash("maskContentUnits");
static constexpr auto SVF_maskUnits           = strhash("maskUnits");
static constexpr auto SVF_matrix              = strhash("matrix");
static constexpr auto SVF_max                 = strhash("max");
static constexpr auto SVF_method              = strhash("method");
static constexpr auto SVF_middle              = strhash("middle");
static constexpr auto SVF_min                 = strhash("min");
static constexpr auto SVF_minus               = strhash("minus");
static constexpr auto SVF_miter               = strhash("miter");
static constexpr auto SVF_miter_clip          = strhash("miter-clip");
static constexpr auto SVF_miter_round         = strhash("miter-round");
static constexpr auto SVF_mod                 = strhash("mod");
static constexpr auto SVF_mode                = strhash("mode");
static constexpr auto SVF_multiplier          = strhash("multiplier");
static constexpr auto SVF_multiply            = strhash("multiply");
static constexpr auto SVF_n                   = strhash("n");
static constexpr auto SVF_n1                  = strhash("n1");
static constexpr auto SVF_n2                  = strhash("n2");
static constexpr auto SVF_n3                  = strhash("n3");
static constexpr auto SVF_narrower            = strhash("narrower");
static constexpr auto SVF_none                = strhash("none");
static constexpr auto SVF_normal              = strhash("normal");
static constexpr auto SVF_numeric_id          = strhash("numeric-id");
static constexpr auto SVF_numOctaves          = strhash("numOctaves");
static constexpr auto SVF_offset              = strhash("offset");
static constexpr auto SVF_opacity             = strhash("opacity");
static constexpr auto SVF_operator            = strhash("operator");
static constexpr auto SVF_order               = strhash("order");
static constexpr auto SVF_origin              = strhash("origin");
static constexpr auto SVF_out                 = strhash("out");
static constexpr auto SVF_over                = strhash("over");
static constexpr auto SVF_overflow            = strhash("overflow");
static constexpr auto SVF_overlay             = strhash("overlay");
static constexpr auto SVF_overline            = strhash("overline");
static constexpr auto SVF_path                = strhash("path");
static constexpr auto SVF_pathLength          = strhash("pathLength");
static constexpr auto SVF_pattern             = strhash("pattern");
static constexpr auto SVF_patternContentUnits = strhash("patternContentUnits");
static constexpr auto SVF_patternTransform    = strhash("patternTransform");
static constexpr auto SVF_patternUnits        = strhash("patternUnits");
static constexpr auto SVF_phi                 = strhash("phi");
static constexpr auto SVF_plus                = strhash("plus");
static constexpr auto SVF_points              = strhash("points");
static constexpr auto SVF_pointsAtX           = strhash("pointsAtX");
static constexpr auto SVF_pointsAtY           = strhash("pointsAtY");
static constexpr auto SVF_pointsAtZ           = strhash("pointsAtZ");
static constexpr auto SVF_polygon             = strhash("polygon");
static constexpr auto SVF_polyline            = strhash("polyline");
static constexpr auto SVF_preserveAlpha       = strhash("preserveAlpha");
static constexpr auto SVF_preserveAspectRatio = strhash("preserveAspectRatio");
static constexpr auto SVF_primitiveUnits      = strhash("primitiveUnits");
static constexpr auto SVF_protanomaly         = strhash("protanomaly");
static constexpr auto SVF_protanopia          = strhash("protanopia");
static constexpr auto SVF_r                   = strhash("r");
static constexpr auto SVF_radialGradient      = strhash("radialGradient");
static constexpr auto SVF_radius              = strhash("radius");
static constexpr auto SVF_rect                = strhash("rect");
static constexpr auto SVF_repeat              = strhash("repeat");
static constexpr auto SVF_repeatCount         = strhash("repeatCount");
static constexpr auto SVF_repeatDur           = strhash("repeatDur");
static constexpr auto SVF_requiredExtensions  = strhash("requiredExtensions");
static constexpr auto SVF_requiredFeatures    = strhash("requiredFeatures");
static constexpr auto SVF_resolution          = strhash("resolution");
static constexpr auto SVF_restart             = strhash("restart");
static constexpr auto SVF_result              = strhash("result");
static constexpr auto SVF_rotate              = strhash("rotate");
static constexpr auto SVF_round               = strhash("round");
static constexpr auto SVF_rx                  = strhash("rx");
static constexpr auto SVF_ry                  = strhash("ry");
static constexpr auto SVF_saturate            = strhash("saturate");
static constexpr auto SVF_scale               = strhash("scale");
static constexpr auto SVF_screen              = strhash("screen");
static constexpr auto SVF_distalGradient      = strhash("distalGradient");
static constexpr auto SVF_padding             = strhash("padding");
static constexpr auto SVF_seed                = strhash("seed");
static constexpr auto SVF_semi_condensed      = strhash("semi_condensed");
static constexpr auto SVF_semi_expanded       = strhash("semi_expanded");
static constexpr auto SVF_set                 = strhash("set");
static constexpr auto SVF_shape_rendering     = strhash("shape-rendering");
static constexpr auto SVF_slope               = strhash("slope");
static constexpr auto SVF_softLight           = strhash("softLight");
static constexpr auto SVF_SourceAlpha         = strhash("SourceAlpha");
static constexpr auto SVF_SourceGraphic       = strhash("SourceGraphic");
static constexpr auto SVF_spacing             = strhash("spacing");
static constexpr auto SVF_specularConstant    = strhash("specularConstant");
static constexpr auto SVF_specularExponent    = strhash("specularExponent");
static constexpr auto SVF_span                = strhash("span");
static constexpr auto SVF_spiral              = strhash("spiral");
static constexpr auto SVF_spreadMethod        = strhash("spreadMethod");
static constexpr auto SVF_square              = strhash("square");
static constexpr auto SVF_start               = strhash("start");
static constexpr auto SVF_startOffset         = strhash("startOffset");
static constexpr auto SVF_stdDeviation        = strhash("stdDeviation");
static constexpr auto SVF_step                = strhash("step");
static constexpr auto SVF_stitchTiles         = strhash("stitchTiles");
static constexpr auto SVF_stop_color          = strhash("stop-color");
static constexpr auto SVF_stop_opacity        = strhash("stop-opacity");
static constexpr auto SVF_string              = strhash("string");
static constexpr auto SVF_stroke              = strhash("stroke");
static constexpr auto SVF_stroke_dasharray        = strhash("stroke-dasharray");
static constexpr auto SVF_stroke_dashoffset       = strhash("stroke-dashoffset");
static constexpr auto SVF_stroke_inner_miterlimit = strhash("stroke-inner-miterlimit");
static constexpr auto SVF_stroke_innerjoin        = strhash("stroke-innerjoin");
static constexpr auto SVF_stroke_linecap          = strhash("stroke-linecap");
static constexpr auto SVF_stroke_linejoin         = strhash("stroke-linejoin");
static constexpr auto SVF_stroke_miterlimit       = strhash("stroke-miterlimit");
static constexpr auto SVF_stroke_miterlimit_theta = strhash("stroke-miterlimit-theta");
static constexpr auto SVF_stroke_opacity   = strhash("stroke-opacity");
static constexpr auto SVF_stroke_width     = strhash("stroke-width");
static constexpr auto SVF_StrokePaint      = strhash("StrokePaint");
static constexpr auto SVF_style            = strhash("style");
static constexpr auto SVF_surfaceScale     = strhash("surfaceScale");
static constexpr auto SVF_svg              = strhash("svg");
static constexpr auto SVF_switch           = strhash("switch");
static constexpr auto SVF_symbol           = strhash("symbol");
static constexpr auto SVF_systemLanguage   = strhash("systemLanguage");
static constexpr auto SVF_table            = strhash("table");
static constexpr auto SVF_tableValues      = strhash("tableValues");
static constexpr auto SVF_targetX          = strhash("targetX");
static constexpr auto SVF_targetY          = strhash("targetY");
static constexpr auto SVF_text             = strhash("text");
static constexpr auto SVF_text_anchor      = strhash("text-anchor");
static constexpr auto SVF_text_decoration  = strhash("text-decoration");
static constexpr auto SVF_textLength       = strhash("textLength");
static constexpr auto SVF_textPath         = strhash("textPath");
static constexpr auto SVF_thickness        = strhash("thickness");
static constexpr auto SVF_title            = strhash("title");
static constexpr auto SVF_to               = strhash("to");
static constexpr auto SVF_top              = strhash("top");
static constexpr auto SVF_transform        = strhash("transform");
static constexpr auto SVF_transition       = strhash("transition");
static constexpr auto SVF_tritanomaly      = strhash("tritanomaly");
static constexpr auto SVF_tritanopia       = strhash("tritanopia");
static constexpr auto SVF_type             = strhash("type");
static constexpr auto SVF_ultra_condensed  = strhash("ultra_condensed");
static constexpr auto SVF_ultra_expanded   = strhash("ultra_expanded");
static constexpr auto SVF_underline        = strhash("underline");
static constexpr auto SVF_units            = strhash("units");
static constexpr auto SVF_use              = strhash("use");
static constexpr auto SVF_values           = strhash("values");
static constexpr auto SVF_version          = strhash("version");
static constexpr auto SVF_vertices         = strhash("vertices");
static constexpr auto SVF_view_height      = strhash("view-height");
static constexpr auto SVF_view_width       = strhash("view-width");
static constexpr auto SVF_view_x           = strhash("view-x");
static constexpr auto SVF_view_y           = strhash("view-y");
static constexpr auto SVF_viewBox          = strhash("viewBox");
static constexpr auto SVF_visibility       = strhash("visibility");
static constexpr auto SVF_wider            = strhash("wider");
static constexpr auto SVF_width            = strhash("width");
static constexpr auto SVF_word_spacing     = strhash("word-spacing");
static constexpr auto SVF_x                = strhash("x");
static constexpr auto SVF_x1               = strhash("x1");
static constexpr auto SVF_x2               = strhash("x2");
static constexpr auto SVF_xChannelSelector = strhash("xChannelSelector");
static constexpr auto SVF_xlink_href       = strhash("xlink:href");
static constexpr auto SVF_xml_space        = strhash("xml:space");
static constexpr auto SVF_xmlns            = strhash("xmlns");
static constexpr auto SVF_xOffset          = strhash("xOffset");
static constexpr auto SVF_xor              = strhash("xor");
static constexpr auto SVF_y                = strhash("y");
static constexpr auto SVF_y1               = strhash("y1");
static constexpr auto SVF_y2               = strhash("y2");
static constexpr auto SVF_yChannelSelector = strhash("yChannelSelector");
static constexpr auto SVF_yOffset          = strhash("yOffset");
static constexpr auto SVF_z                = strhash("z");
static constexpr auto SVF_zoomAndPan       = strhash("zoomAndPan");
static constexpr auto SVF_morph            = strhash("morph");
static constexpr auto SVF_pathTransition   = strhash("pathTransition");
static constexpr auto SVF_shape            = strhash("shape");
static constexpr auto SVF_wave             = strhash("wave");
static constexpr auto SVF_inOut            = strhash("inOut");
static constexpr auto SVF_cubicIn          = strhash("cubicIn");
static constexpr auto SVF_cubicOut         = strhash("cubicOut");
static constexpr auto SVF_cubicInOut       = strhash("cubicInOut");
static constexpr auto SVF_easing           = strhash("easing");

static constexpr auto glSVGNamespace = strhash("http://www.w3.org/2000/svg");
static constexpr auto glKotukuNamespace = strhash("http://www.kotuku.dev/namespaces/kotuku");
static constexpr auto glKotukuSVGNamespace = strhash("http://www.kotuku.dev/xmlns/svg");

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

static std::string_view svg_local_name(const XTag &Tag) noexcept
{
   std::string_view name(Tag.name());

   if (auto colon = name.find(':'); colon != std::string_view::npos) return name.substr(colon + 1);
   else return name;
}

//********************************************************************************************************************

static uint32_t svg_tag_hash(const XTag &Tag) noexcept
{
   if ((Tag.NamespaceID) and (Tag.NamespaceID != glSVGNamespace) and (Tag.NamespaceID != glKotukuNamespace) and
       (Tag.NamespaceID != glKotukuSVGNamespace)) return 0;

   if ((Tag.NamespaceID IS glKotukuNamespace) or (Tag.NamespaceID IS glKotukuSVGNamespace)) {
      switch (strhash(svg_local_name(Tag))) {
         case SVF_morph:          return SVF_kotuku_morph;
         case SVF_pathTransition: return SVF_kotuku_pathTransition;
         case SVF_shape:          return SVF_kotuku_shape;
         case SVF_spiral:         return SVF_kotuku_spiral;
         case SVF_transition:     return SVF_kotuku_transition;
         case SVF_wave:           return SVF_kotuku_wave;
         default:                 return 0;
      }
   }

   if (!Tag.NamespaceID) return strhash(Tag.name());
   else return strhash(svg_local_name(Tag));
}

//********************************************************************************************************************

static bool svg_tag_is(const XTag &Tag, uint32_t Hash) noexcept
{
   return svg_tag_hash(Tag) IS Hash;
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

   if (!objModule::load("image", &modImage)) { // RSVG has a Image class dependency
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
   if (modImage) { FreeResource(modImage); modImage = nullptr; }

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
