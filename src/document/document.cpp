/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

*********************************************************************************************************************/

//#define DBG_LAYOUT   // Enable layout log messages
//#define DBG_STREAM   // Dump the bytecode stream to the log
//#define DBG_SEGMENTS // Print list of segments
//#define DBG_WORDWRAP // Enable word-wrap log messages
//#define GUIDELINES   // Draw guidelines around all registered content segments

#if (!defined(NDEBUG) || defined(DBG_LAYOUT) || defined(DBG_STREAM) || defined(DBG_SEGMENTS))
 #define RETAIN_LOG_LEVEL TRUE
#endif

#ifdef DBG_LAYOUT
 #define DLAYOUT(...)   log.msg(__VA_ARGS__)
#else
 #define DLAYOUT(...)
#endif

#ifdef DBG_WORDWRAP
 #define DWRAP(...)   log.msg(__VA_ARGS__)
#else
 #define DWRAP(...)
#endif

#define PRV_DOCUMENT_MODULE
#define PRV_SURFACE

#include "defs/hashes.h"
#include "../link/unicode.h"
#include <kotuku/modules/xquery.h>
#include <cassert>

using BYTECODE = uint32_t;
using CELL_ID = uint32_t;

static BYTECODE glByteCodeID = 1;
static uint32_t glUID = 1000; // Use for generating unique/incrementing ID's, e.g. cell ID

using namespace kt;

JUMPTABLE_CORE
JUMPTABLE_FONT
JUMPTABLE_DISPLAY
JUMPTABLE_VECTOR

#include "defs/document.h"

//********************************************************************************************************************

static std::string glHighlight = "rgb(219 219 255 / 1)";

static OBJECTPTR clDocument = nullptr;
static OBJECTPTR modDisplay = nullptr, modFont = nullptr, modDocument = nullptr, modVector = nullptr;

//********************************************************************************************************************

std::vector<sorted_segment> & extDocument::get_sorted_segments()
{
   if ((!SortSegments.empty()) or (Segments.empty())) return SortSegments;

   auto sortseg_compare = [&] (sorted_segment &Left, sorted_segment &Right) {
      if (Left.y < Right.y) return 1;
      else if (Left.y > Right.y) return -1;
      else {
         if (Segments[Left.segment].area.X < Segments[Right.segment].area.X) return 1;
         else if (Segments[Left.segment].area.X > Segments[Right.segment].area.X) return -1;
         else return 0;
      }
   };

   // Build the SortSegments array (unsorted)

   SortSegments.resize(Segments.size());
   unsigned i;
   SEGINDEX seg;
   for (i=0, seg=0; seg < SEGINDEX(Segments.size()); seg++) {
      if ((Segments[seg].area.Height > 0) and (Segments[seg].area.Width > 0)) {
         SortSegments[i].segment = seg;
         SortSegments[i].y       = Segments[seg].area.Y;
         i++;
      }
   }

   // Shell sort

   unsigned j, h = 1;
   while (h < SortSegments.size() / 9) h = 3 * h + 1;

   for (; h > 0; h /= 3) {
      for (auto i=h; i < SortSegments.size(); i++) {
         sorted_segment temp = SortSegments[i];
         for (j=i; (j >= h) and (sortseg_compare(SortSegments[j - h], temp) < 0); j -= h) {
            SortSegments[j] = SortSegments[j - h];
         }
         SortSegments[j] = temp;
      }
   }

   return SortSegments;
}

//********************************************************************************************************************
// Function prototypes.

#include "defs/module_def.c"

struct layout; // Pre-def

static ERR  activate_cell_edit(extDocument *, int, stream_char);
static ERR  add_document_class(void);
static int add_tabfocus(extDocument *, TT, BYTECODE);
static void advance_tabfocus(extDocument *, int8_t);
static void deactivate_edit(extDocument *, bool);
static ERR  extract_script(extDocument *, std::string_view, objScript **, std::string &, std::string &);
static void error_dialog(const std::string, const std::string);
static void error_dialog(const std::string, ERR);
static SEGINDEX find_segment(std::vector<doc_segment> &, stream_char, bool);
static int  find_tabfocus(extDocument *, TT, BYTECODE);
static ERR  flash_cursor(extDocument *, int64_t, int64_t);
static int getutf8(CSTRING, int *);
static ERR  insert_text(extDocument *, RSTREAM *, stream_char &, const std::string_view, bool);
static ERR  insert_xml(extDocument *, RSTREAM *, objXML *, const objXML::TAGS &, int, STYLE = STYLE::NIL, IPF = IPF::NIL);
static ERR  key_event(objVectorViewport *, KQ, KEY, int);
static void layout_doc(extDocument *);
static ERR  load_doc(extDocument *, std::string, bool, ULD = ULD::NIL);
static void notify_disable_viewport(OBJECTPTR, ACTIONID, ERR, APTR);
static void notify_enable_viewport(OBJECTPTR, ACTIONID, ERR, APTR);
static void notify_focus_viewport(OBJECTPTR, ACTIONID, ERR, APTR);
static void notify_free_event(OBJECTPTR, ACTIONID, ERR, APTR);
static void notify_lostfocus_viewport(OBJECTPTR, ACTIONID, ERR, APTR);
static ERR  feedback_view(objVectorViewport *, FM);
static void process_parameters(extDocument *, const std::string_view);
static std::string_view read_unit(std::string_view, double &, bool &);
static void redraw(extDocument *, bool);
static ERR  report_event(extDocument *, DEF, entity *, KEYVALUE *);
static void reset_cursor(extDocument *);
static ERR  resolve_fontx_by_index(extDocument *, stream_char, double &);
static int  safe_file_path(extDocument *, std::string_view);
static void set_focus(extDocument *, int, CSTRING);
static void show_bookmark(extDocument *, std::string_view);
static std::string stream_to_string(RSTREAM &, stream_char, stream_char);
static ERR  unload_doc(extDocument *, ULD = ULD::NIL);
static bool valid_objectid(extDocument *, OBJECTID);
static bool view_area(extDocument *, double, double, double, double);

static ERR GET_WorkingPath(extDocument *, CSTRING *);

#ifdef DBG_STREAM
static void print_stream(RSTREAM &);
#endif

//********************************************************************************************************************

template <class T> inline void remove_cursor(T a) { draw_cursor(a, false); }

//********************************************************************************************************************

static const std::array<std::string_view, int(SCODE::END)> strCodes = {
   "?", "Text", "Font", "FontEnd", "Link", "TabDef", "PE", "P", "EndLink", "Advance", "List", "ListEnd",
   "Table", "TableEnd", "Row", "Cell", "RowEnd", "Index", "IndexEnd", "XML", "Image", "Use", "Button", "Checkbox",
   "Combobox", "Input"
};

template <class T> inline const std::string_view & BC_NAME(RSTREAM &Stream, T Index) {
   return strCodes[int(Stream[Index].code)];
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   argModule->get(FID_Root, modDocument);

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("font", &modFont, &FontBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("vector", &modVector, &VectorBase) != ERR::Okay) return ERR::InitModule;

   OBJECTID style_id;
   if (FindObject("glStyle", CLASSID::XML, FOF::NIL, &style_id) IS ERR::Okay) {
      char buffer[32];
      if (acGetKey(GetObjectPtr(style_id), "/colours/@DocumentHighlight", buffer, sizeof(buffer)) IS ERR::Okay) {
         glHighlight.assign(buffer);
      }
   }

   // Set the first entry of glFonts with the default font face.

   CSTRING resolved_face;
   if (fnt::ResolveFamilyName(DEFAULT_FONTFACE.c_str(), &resolved_face) IS ERR::Okay) {
      APTR new_handle = nullptr;
      if (vec::GetFontHandle(resolved_face, DEFAULT_FONTSTYLE.c_str(), 400, DEFAULT_FONTSIZE, &new_handle) IS ERR::Okay) {
         glFonts.emplace_back(new_handle, resolved_face, DEFAULT_FONTSTYLE, DEFAULT_FONTSIZE);
         glFontIndexCache.try_emplace(font_cache_key { resolved_face, DEFAULT_FONTSTYLE, DEFAULT_FONTSIZE }, 0);
      }
      else return ERR::Failed;
   }
   else return ERR::Failed;

   return add_document_class();
}

static ERR MODExpunge(void)
{
   glFontIndexCache.clear();
   glFonts.clear();

   if (modVector)  { FreeResource(modVector);  modVector  = nullptr; }
   if (modDisplay) { FreeResource(modDisplay); modDisplay = nullptr; }
   if (modFont)    { FreeResource(modFont);    modFont    = nullptr; }

   if (clDocument) { FreeResource(clDocument); clDocument = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

inline INDEX RSTREAM::find_cell(CELL_ID ID)
{
   for (INDEX i=0; i < std::ssize(data); i++) {
      if (data[i].code IS SCODE::CELL) {
         auto &cell = std::get<bc_cell>(codes[data[i].uid]);
         if ((ID) and (ID IS cell.cell_id)) return i;
      }
   }

   return -1;
}

inline INDEX RSTREAM::find_editable_cell(std::string_view EditDef)
{
   for (INDEX i=0; i < std::ssize(data); i++) {
      if (data[i].code IS SCODE::CELL) {
         auto &cell = lookup<bc_cell>(i);
         if (EditDef == cell.edit_def) return i;
      }
   }

   return -1;
}

//********************************************************************************************************************

inline doc_edit * find_editdef(extDocument *Self, std::string_view Name)
{
   auto it = Self->EditDefs.find(Name);
   if (it != Self->EditDefs.end()) return &it->second;
   else return nullptr;
}

//********************************************************************************************************************
// Layout the document with a lower log level to cut back on noise.

inline void layout_doc_fast(extDocument *Self)
{
#ifndef RETAIN_LOG_LEVEL
   kt::LogLevel level(2);
#endif

   layout_doc(Self);
}

//********************************************************************************************************************

#include "scrollbar.cpp"
#include "streamchar.cpp"
#include "parsing.cpp"
#include "class/fields.cpp"
#include "class/document_class.cpp"
#include "debug.cpp"
#include "functions.cpp"
#include "ui.cpp"
#include "layout.cpp"
#include "menu.cpp"
#include "draw.cpp"
#include "entities.cpp"
#include "dunit.cpp"

//********************************************************************************************************************

static ERR add_document_class(void)
{
   clDocument = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::DOCUMENT),
      fl::ClassVersion(VER_DOCUMENT),
      fl::Name("Document"),
      fl::Category(CCF::GUI),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::Actions(clDocumentActions),
      fl::Methods(clDocumentMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extDocument)),
      fl::Path(MOD_PATH),
      fl::FileExtension("rpl|ripple|ripl"),
      fl::Icon("filetypes/text"));

   return clDocument ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_document_module() { return &ModHeader; }
