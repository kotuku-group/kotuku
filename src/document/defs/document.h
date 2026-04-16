
class extDocument;
struct bc_combobox;

typedef int INDEX;
using SEGINDEX = int;

static constexpr int MAX_PAGE_WIDTH   = 30000;
static constexpr int MAX_PAGE_HEIGHT  = 100000;
static constexpr int MIN_PAGE_WIDTH   = 20;
static constexpr int MAX_DEPTH        = 40;    // Limits recursion from tables-within-tables
static constexpr int WIDTH_LIMIT      = 4000;
static constexpr int MAX_WORD_TOKEN_WIDTH = 0x7fff; // Cached word widths are limited to signed 16-bit range.
static constexpr int DEFAULT_FONTSIZE = 14;    // 72DPI pixel size
static std::string DEFAULT_FONTSTYLE("Medium");
static std::string DEFAULT_FONTFACE("Noto Sans");
static std::string DEFAULT_FONTFILL("rgb(0,0,0)");

enum class TE : char {
   NIL = 0,
   WRAP_TABLE,
   REPASS_ROW_HEIGHT,
   EXTEND_PAGE
};

enum class CB : uint8_t { // Cell border options
   NIL    = 0x00,
   TOP    = 0x01,
   BOTTOM = 0x02,
   LEFT   = 0x04,
   RIGHT  = 0x08,
   ALL    = 0X0f
};

DEFINE_ENUM_FLAG_OPERATORS(CB)

enum class CELL : uint8_t {
   NIL = 0,
   ABORT,
   WRAP_TABLE_CELL,
   REPASS_ROW_HEIGHT
};

enum class RTD : uint8_t {
   NIL=0,
   OBJECT_TEMP,         // The object can be removed after parsing has finished
   OBJECT_UNLOAD,       // Default choice for object termination, terminates immediately
   OBJECT_UNLOAD_DELAY, // Use SendMessage() to terminate the object
   PERSISTENT_SCRIPT,   // The script can survive refreshes
   PERSISTENT_OBJECT    // The object can survive refreshes
};

DEFINE_ENUM_FLAG_OPERATORS(RTD)

enum class STYLE : uint8_t {
   NIL           = 0x00,
   INHERIT_STYLE = 0x01, // Inherit whatever font style applies at the insertion point.
   RESET_STYLE   = 0x02  // Current font style will be reset rather than defaulting to the most recent style at the insertion point.
};

DEFINE_ENUM_FLAG_OPERATORS(STYLE)

enum class PXF : int16_t {
   NIL       = 0,
   ARGS      = 0x0001,
   TRANSLATE = 0x0002
};

DEFINE_ENUM_FLAG_OPERATORS(PXF)

enum class LINK : uint8_t {
   NIL = 0,
   HREF,
   FUNCTION
};

DEFINE_ENUM_FLAG_OPERATORS(LINK)

enum {
   COND_NOT_EQUAL=1,
   COND_EQUAL,
   COND_LESS_THAN,
   COND_LESS_EQUAL,
   COND_GREATER_THAN,
   COND_GREATER_EQUAL
};

enum class SCODE : char {
   NIL = 0,
   TEXT,
   FONT,
   FONT_END,
   LINK,
   TABDEF,
   PARAGRAPH_END,
   PARAGRAPH_START,
   LINK_END,
   ADVANCE,
   LIST_START,
   LIST_END,
   TABLE_START,
   TABLE_END,
   ROW,
   CELL,
   ROW_END,
   INDEX_START,
   INDEX_END,
   XML,
   IMAGE,
   USE,
   BUTTON,
   CHECKBOX,
   COMBOBOX,
   INPUT,
   // End of list.  Functions affected by changing these codes are:
   //  BC_NAME()
   //  new_segment()
   END
};

DEFINE_ENUM_FLAG_OPERATORS(SCODE)

enum class ULD : uint8_t {
   NIL             = 0,
   TERMINATE       = 0x01,
   KEEP_PARAMETERS = 0x02,
   REFRESH         = 0x04,
   REDRAW          = 0x08
};

DEFINE_ENUM_FLAG_OPERATORS(ULD)

enum class NL : char {
   NONE = 0,
   PARAGRAPH
};

DEFINE_ENUM_FLAG_OPERATORS(NL)

enum class WRAP : char {
   DO_NOTHING = 0,
   EXTEND_PAGE,
   WRAPPED
};

DEFINE_ENUM_FLAG_OPERATORS(WRAP)

enum {
   STATE_OUTSIDE=0,
   STATE_ENTERED,
   STATE_INSIDE
};

enum class IPF : uint32_t {
   NIL = 0,
   NO_CONTENT   = 0x0001, // XML content data will be ignored
   FILTER_TABLE = 0x0008, // The tag is restricted to use within <table> sections.
   FILTER_ROW   = 0X0010, // The tag is restricted to use within <row> sections.
   FILTER_ALL   = (FILTER_TABLE|FILTER_ROW)
};

DEFINE_ENUM_FLAG_OPERATORS(IPF)

enum class TRF : uint32_t {
   NIL      = 0,
   BREAK    = 0x00000001,
   CONTINUE = 0x00000002
};

DEFINE_ENUM_FLAG_OPERATORS(TRF)

class RSTREAM;

#include "dunit.h"

//********************************************************************************************************************
// UI hooks for the client

struct ui_hooks {
   std::string on_click;     // Function to call after a button event in the UI
   std::string on_motion;    // Function to call after a motion event in the UI
   std::string on_crossing;  // Function to call after a crossing event in the UI (enter/leave)
   JTYPE events = JTYPE::NIL; // Input events that the client is interested in.
};

//********************************************************************************************************************

struct padding {
   double left = 0, top = 0, right = 0, bottom = 0;
   bool left_scl = false, right_scl = false, top_scl = false, bottom_scl = false;
   bool configured = false;

   padding() = default;

   padding(double pLeft, double pTop, double pRight, double pBottom) :
      left(pLeft), top(pTop), right(pRight), bottom(pBottom), configured(true) { }

   void parse(const std::string &Value);

   void scale_all() { left_scl = right_scl = top_scl = bottom_scl = true; }
};

//********************************************************************************************************************

struct scroll_mgr {
   struct scroll_slider {
      double offset = 0;
      double length = 20;
   };

   struct scroll_bar {
      scroll_mgr *m_mgr = nullptr;
      objVectorViewport *m_bar_vp = nullptr; // Main viewport for managing the scrollbar
      objVectorViewport *m_slider_host = nullptr;
      objVectorViewport *m_slider_vp = nullptr;
      objVectorRectangle *m_slider_rect = nullptr;
      scroll_slider m_slider_pos;
      char m_direction = 0; // 'V' or 'H'
      double m_breadth = 10;

      scroll_slider calc_slider(double, double, double, double);
      void init(scroll_mgr *, char, objVectorViewport *);
      void clear();
   };

   extDocument *m_doc = nullptr;
   objVectorViewport *m_page = nullptr; // Monitored page
   objVectorViewport *m_view = nullptr; // Monitored owner of the page
   double m_min_width = 0;    // For dynamic width mode, this is the minimum required width
   bool m_fixed_mode = false;
   bool m_auto_adjust_view_size = true; // Automatically adjust the view to accomodate the visibility of the scrollbars

   scroll_bar m_vbar;
   scroll_bar m_hbar;

   scroll_mgr() {}

   void   init(extDocument *, objVectorViewport *, objVectorViewport *);
   void   scroll_page(double, double);
   void   recalc_sliders_from_view();
   void   fix_page_size(double, double);
   void   dynamic_page_size(double, double, double);
};

//********************************************************************************************************************
// Tab is used to represent interactive entities within the document that can be tabbed to.

struct tab {
   // The ref is a UID for the Type, so you can use it to find the tab in the document stream
   std::variant<int, uint32_t> ref; // For TT::VECTOR: VectorID; TT::LINK: LinkID
   TT    type;
   bool  active;     // true if the tabbable entity is active/visible

   tab(TT pType, BYTECODE pReference, bool pActive) : ref(pReference), type(pType), active(pActive) { }
};

//********************************************************************************************************************

struct edit_cell {
   CELL_ID cell_id;
   double x, y, width, height;
};

//********************************************************************************************************************

struct link_activated {
   std::map<std::string, std::string> Values;  // All key-values associated with the link.
};

//********************************************************************************************************************
// Every instruction in the document stream is represented by a stream_code entity.  The code refers to what the thing
// is, while the UID hash refers to further information in the Codes table.

struct stream_code {
   SCODE code;  // Type
   BYTECODE uid; // Lookup for the Codes table

   stream_code() : code(SCODE::NIL), uid(0) { }
   stream_code(SCODE pCode, BYTECODE pID) : code(pCode), uid(pID) { }
};

//********************************************************************************************************************

class entity {
public:
   BYTECODE uid;   // Unique identifier for lookup
   SCODE code = SCODE::NIL; // Byte code

   entity() { uid = glByteCodeID++; }
   entity(SCODE pCode) : code(pCode) { uid = glByteCodeID++; }
};

//********************************************************************************************************************

class docresource {
public:
   OBJECTID object_id;
   CLASSID class_id;
   RTD type;
   bool terminate = false; // If true, can be freed immediately and not on a delay

   docresource(OBJECTID pID, RTD pType, CLASSID pClassID = CLASSID::NIL) :
      object_id(pID), class_id(pClassID), type(pType) { }

   ~docresource() {
      if ((type IS RTD::PERSISTENT_SCRIPT) or (type IS RTD::PERSISTENT_OBJECT)) {
         if (terminate) FreeResource(object_id);
         else SendMessage(MSGID::FREE, MSF::NIL, &object_id, sizeof(OBJECTID));
      }
      else if (type IS RTD::OBJECT_UNLOAD_DELAY) {
         if (terminate) FreeResource(object_id);
         else SendMessage(MSGID::FREE, MSF::NIL, &object_id, sizeof(OBJECTID));
      }
      else if (type != RTD::NIL) FreeResource(object_id);
   }

   docresource(docresource &&other) noexcept { // Move constructor
      object_id = other.object_id;
      class_id  = other.class_id;
      type      = other.type;
      terminate = other.terminate;
      other.type = RTD::NIL;
   }

   docresource(const docresource &other) { // Copy constructor
      object_id = other.object_id;
      class_id  = other.class_id;
      type      = other.type;
      terminate = other.terminate;
   }

   docresource& operator=(docresource &&other) noexcept { // Move assignment
      if (this IS &other) return *this;
      object_id = other.object_id;
      class_id  = other.class_id;
      type      = other.type;
      terminate = other.terminate;
      other.type = RTD::NIL;
      return *this;
   }

   docresource& operator=(const docresource& other) { // Copy assignment
      if (this IS &other) return *this;
      object_id = other.object_id;
      class_id  = other.class_id;
      type      = other.type;
      terminate = other.terminate;
      return *this;
   }
};

//********************************************************************************************************************

struct case_insensitive_map {
   bool operator() (const std::string &lhs, const std::string &rhs) const {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
   }
};

//********************************************************************************************************************
// Basic font caching on an index basis.

struct font_entry {
   APTR handle;
   std::string face;
   std::string style;
   FontMetrics metrics; // Derived from vec::GetFontMetrics() at the time of caching
   int font_size; // 72 DPI pixel size
   ALIGN align;
   int16_t space_width = 0; // Cached pixel width of ' ' (matches m_space_width type)
   double zero_width = 0;   // Cached pixel width of '0' (for DU::CHAR unit conversion)

   font_entry(APTR pHandle, const std::string_view pFace, const std::string_view pStyle, double pSize) :
      handle(pHandle), face(pFace), style(pStyle), font_size(pSize), align(ALIGN::NIL) {
      vec::GetFontMetrics(pHandle, &metrics);
      double kerning = 0;
      space_width = int16_t(vec::CharWidth(pHandle, ' ', 0, &kerning) + kerning);
      kerning = 0;
      zero_width = vec::CharWidth(pHandle, '0', 0, &kerning) + kerning;
   }

   ~font_entry() { }

   font_entry(font_entry &&other) noexcept { // Move constructor
      handle      = other.handle;
      metrics     = other.metrics;
      font_size   = other.font_size;
      face        = other.face;
      style       = other.style;
      align       = other.align;
      space_width = other.space_width;
      zero_width  = other.zero_width;
      other.handle = nullptr;
   }

   font_entry(const font_entry &other) { // Copy constructor
      handle      = other.handle;
      font_size   = other.font_size;
      metrics     = other.metrics;
      face        = other.face;
      style       = other.style;
      align       = other.align;
      space_width = other.space_width;
      zero_width  = other.zero_width;
   }

   font_entry& operator=(font_entry &&other) noexcept { // Move assignment
      if (this IS &other) return *this;
      handle      = other.handle;
      font_size   = other.font_size;
      metrics     = other.metrics;
      face        = other.face;
      style       = other.style;
      align       = other.align;
      space_width = other.space_width;
      zero_width  = other.zero_width;
      other.handle = nullptr;
      return *this;
   }

   font_entry& operator=(const font_entry& other) { // Copy assignment
      if (this IS &other) return *this;
      handle      = other.handle;
      font_size   = other.font_size;
      metrics     = other.metrics;
      face        = other.face;
      style       = other.style;
      align       = other.align;
      space_width = other.space_width;
      zero_width  = other.zero_width;
      return *this;
   }

   // NB: FontMetrics are not reported identically by all backends.
   // Bitmap fonts expose Height as the full region above the baseline, while FreeType-backed fonts may expose Ascent
   // for that role instead.

   double descent() const {
      auto gap = metrics.LineSpacing - metrics.Height - metrics.Descent;
      return metrics.Descent + gap;
   }
};

//********************************************************************************************************************
// bc_font has a dual purpose - it can maintain current font style information during parsing as well as being embedded
// in the document stream.

static std::mutex glFontsMutex;
static std::deque<font_entry> glFonts; // font_entry pointers must be kept stable

struct font_cache_key {
   std::string face;
   std::string style;
   int pixel_size = 0;

   bool operator==(const font_cache_key &Other) const {
      return (pixel_size IS Other.pixel_size) and (face == Other.face) and (style == Other.style);
   }
};

struct font_cache_hash {
   size_t operator()(const font_cache_key &Key) const {
      auto hash = std::hash<std::string> {}(Key.face);
      hash ^= std::hash<std::string> {}(Key.style) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
      hash ^= size_t(Key.pixel_size) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
      return hash;
   }
};

static ankerl::unordered_dense::map<font_cache_key, int16_t, font_cache_hash> glFontIndexCache;

struct bc_font : public entity {
private:
   int16_t font_index;    // Font lookup (will reflect the true font face, point size, style)
public:
   FSO   options;      // Style options, like underline
   ALIGN valign;       // Vertical alignment of text within the available line height
   std::string fill;   // Font fill instruction
   std::string face;   // The font face as requested by the client.  Might not match the font we actually use.
   std::string style;  // The font style as requested by the client.  Might not match the font we actually use.
   DUNIT req_size;     // Original font size as requested by the client.
   int   pixel_size;   // The font size in pixels, calculated from the req_size during layout

   bc_font(): font_index(-1), options(FSO::NIL), valign(ALIGN::BOTTOM), fill(DEFAULT_FONTFILL),
      face(DEFAULT_FONTFACE), style(DEFAULT_FONTSTYLE), pixel_size(0) { code = SCODE::FONT; }

   font_entry * layout_font(layout &);

   // Calling this is only valid after the layout process has completed, i.e. during drawing and UI operations

   font_entry * get_font() const {
      {
         std::lock_guard lk(glFontsMutex);
         if ((font_index < std::ssize(glFonts)) and (font_index >= 0)) return &glFonts[font_index];
      }

      pf::Log log(__FUNCTION__);
      log.error("A font_index is -1."); // An index of -1 means a call to layout_font() is missing.
      return &glFonts[0];
   }

   void apply(const bc_font &Other) {
      *this = Other;
      font_index = -1;
   }

   bc_font(const bc_font &Other) {
      // Copy another style and reset the index to -1 so that changes can refreshed
      *this = Other;
      font_index = -1;
   }

   int16_t index() const { return font_index; }
};

struct bc_font_end : public entity {
   bc_font_end() : entity(SCODE::FONT_END) { }
};

//********************************************************************************************************************
// stream_char provides indexing to specific characters in the stream.  It is designed to handle positional changes so
// that text string boundaries can be crossed without incident.
//
// The index and offset are set to -1 if the stream_char is invalidated.

struct stream_char {
   INDEX index;     // Byte code position within the stream
   size_t offset;   // Specific character offset within the bc_text.text string

   stream_char() : index(-1), offset(-1) { }
   stream_char(INDEX pIndex, uint32_t pOffset) : index(pIndex), offset(pOffset) { }
   stream_char(INDEX pIndex) : index(pIndex), offset(0) { }

   bool operator==(const stream_char &Other) const {
      return (this->index IS Other.index) and (this->offset IS Other.offset);
   }

   bool operator<(const stream_char &Other) const {
      if (this->index < Other.index) return true;
      else if ((this->index IS Other.index) and (this->offset < Other.offset)) return true;
      else return false;
   }

   bool operator>(const stream_char &Other) const {
      if (this->index > Other.index) return true;
      else if ((this->index IS Other.index) and (this->offset > Other.offset)) return true;
      else return false;
   }

   bool operator<=(const stream_char &Other) const {
      if (this->index < Other.index) return true;
      else if ((this->index IS Other.index) and (this->offset <= Other.offset)) return true;
      else return false;
   }

   bool operator>=(const stream_char &Other) const {
      if (this->index > Other.index) return true;
      else if ((this->index IS Other.index) and (this->offset >= Other.offset)) return true;
      else return false;
   }

   void operator+=(const int Value) {
      offset += Value;
   }

   inline void reset() { index = -1; offset = -1; }
   inline bool valid() { return index != -1; }

   inline void set(INDEX pIndex, uint32_t pOffset = 0) {
      index  = pIndex;
      offset = pOffset;
   }

   inline INDEX prev_code() {
      index--;
      if (index < 0) { index = -1; offset = -1; }
      else offset = 0;
      return index;
   }

   inline INDEX next_code() {
      offset = 0;
      index++;
      return index;
   }

   // NB: None of these support unicode.

   uint8_t get_char(RSTREAM &);
   uint8_t get_char(RSTREAM &, int);
   uint8_t get_prev_char(RSTREAM &);
   uint8_t get_prev_char_or_inline(RSTREAM &);
   void erase_char(RSTREAM &); // Erase a character OR an escape code.
   void next_char(RSTREAM &);
   void prev_char(RSTREAM &);
};

//********************************************************************************************************************
// Refer to layout::new_segment().  A segment represents graphical content, which can be in the form of text,
// graphics or both.  A segment can consist of one line only - so if the layout process encounters a boundary causing
// wordwrap then a new segment must be created.

struct doc_segment {
   stream_char start;       // Starting index (including character if text)
   stream_char stop;        // Stop at this index/character
   stream_char trim_stop;   // The stopping point when whitespace is removed
   FloatRect area;          // Dimensions of the segment.
   double  descent;         // The largest descent (gutter) value in pixels after taking into account all fonts used on the line
   double  align_width;     // Full width of this segment if it were non-breaking
   RSTREAM *stream;         // The stream that this segment refers to
   bool    edit;            // true if this segment represents content that can be edited
   bool    allow_merge;     // true if this segment can be merged with siblings that have allow_merge set to true

   inline double x(double Advance, FSO StyleOptions) {
      if ((StyleOptions & FSO::ALIGN_CENTER) != FSO::NIL) return Advance + ((align_width - area.Width) * 0.5);
      else if ((StyleOptions & FSO::ALIGN_RIGHT) != FSO::NIL) return Advance + (align_width - area.Width);
      else return Advance;
   }

   inline double y(ALIGN VAlign, font_entry *Font) {
      if ((VAlign & ALIGN::TOP) != ALIGN::NIL) return area.Y + Font->metrics.Ascent;
      else if ((VAlign & ALIGN::VERTICAL) != ALIGN::NIL) {
         const double avail_space = area.Height - descent;
         return area.Y + avail_space - ((avail_space - Font->metrics.Ascent) * 0.5);
      }
      else return area.Y + area.Height - descent;
   }
};

struct doc_clip {
   double left = 0, top = 0, right = 0, bottom = 0;
   INDEX index = 0; // The stream index of the object/table/item that is creating the clip.
   bool transparent = false; // If true, wrapping will not be performed around the clip region.
   std::string name;

   doc_clip() = default;

   doc_clip(double pLeft, double pTop, double pRight, double pBottom, int pIndex, bool pTransparent, const std::string &pName) :
      left(pLeft), top(pTop), right(pRight), bottom(pBottom), index(pIndex), transparent(pTransparent), name(pName) {

      if ((right - left > 20000) or (bottom - top > 20000)) {
         pf::Log log;
         log.warning("%s set invalid clip dimensions: %.0f,%.0f,%.0f,%.0f", name.c_str(), left, top, right, bottom);
         right = left;
         bottom = top;
      }
   }
};

struct doc_edit {
   int max_chars;
   std::string name;
   std::string on_enter, on_exit, on_change;
   std::vector<std::pair<std::string, std::string>> args;
   bool line_breaks;

   doc_edit() : max_chars(-1), args(0), line_breaks(false) { }
};

struct bc_link;
struct bc_cell;

struct mouse_over {
   std::string function; // name of function to call.
   double top, left, bottom, right;
   int element_id;
};

struct tablecol {
   double preset_width = 0;
   double min_width = 0;   // For assisting layout
   double width = 0;
   bool preset_width_rel = false;
};

//********************************************************************************************************************

struct bc_advance : public entity {
   DUNIT x, y;

   bc_advance() : x(0.0), y(0.0) { code = SCODE::ADVANCE; }
};

struct bc_index : public entity {
   uint32_t name_hash = 0;          // The name of the index is held here as a hash
   int    id = 0;                 // UID for matching to the correct bc_index_end
   double y = 0;                  // The cursor's vertical position of when the index was encountered during layout
   bool   visible = false;        // true if the content inside the index is visible (this is the default)
   bool   parent_visible = false; // true if the nearest parent index(es) will allow index content to be visible.  true is the default.  This allows Hide/ShowIndex() to manage themselves correctly

   bc_index(uint32_t pName, int pID, int pY, bool pVisible, bool pParentVisible) :
      name_hash(pName), id(pID), y(pY), visible(pVisible), parent_visible(pParentVisible) {
      code = SCODE::INDEX_START;
   }
};

struct bc_index_end : public entity {
   int id; // UID matching to the correct bc_index
   bc_index_end(int pID) : id(pID) { code = SCODE::INDEX_END; }
};

struct bc_link : public entity {
   GuardedObject<objVectorPath> path;
   LINK type;                     // Link type (either a function or hyperlink)
   ui_hooks hooks;                // UI hooks defined by the client
   std::string ref;               // Function name or a path, depending on the Type
   std::string hint;              // Hint/title to display when hovering
   std::vector<std::pair<std::string,std::string>> args;
   std::string fill;              // Fill instruction from the client
   bc_font font;                  // Font style from the parser

   bc_link() : type(LINK::NIL)
      { code = SCODE::LINK; }
};

struct bc_link_end : public entity {
   bc_link_end() { code = SCODE::LINK_END; }
};

struct bc_list : public entity {
   enum {
      ORDERED=0,
      BULLET,
      CUSTOM
   };

   std::string fill;                   // Fill to use for bullet points (valid for BULLET only).
   std::vector<std::string> buffer;    // Temp buffer, used for ordered lists
   int   start        = 1;            // Starting value for ordered lists (default: 1)
   DUNIT item_indent  = DUNIT(1.0, DU::LINE_HEIGHT); // Minimum indentation for text printed for each item
   DUNIT block_indent = DUNIT(1.0, DU::LINE_HEIGHT); // Indentation for each set of items
   int   item_num     = 0;
   int   order_insert = 0;
   DUNIT v_spacing    = DUNIT(0.5, DU::LINE_HEIGHT);  // Spacing between list items, equivalent to paragraph leading, expressed as a ratio
   uint8_t type       = BULLET;
   bool  repass       = false;

   bc_list() { code = SCODE::LIST_START; }
};

struct bc_list_end : public entity {
   bc_list_end() { code = SCODE::LIST_END; }
};

struct bc_table : public entity {
   GuardedObject<objVectorPath> path;
   GuardedObject<objVectorViewport> viewport;
   std::vector<PathCommand> seq;         // Commands to be assigned to 'path'
   std::vector<tablecol> columns;        // Table column management
   std::string fill, stroke;             // SVG stroke and fill instructions
   padding cell_padding;                 // Spacing inside each cell (margins)
   DUNIT  cell_v_spacing, cell_h_spacing; // Spacing between each cell
   double row_width = 0;                 // Assists in the computation of row width
   double x = 0, y = 0, width = 0, height = 0; // Run-time dimensions calculated during layout
   DUNIT  min_width, min_height;         // Client-defined minimum table width/height
   double cursor_x = 0, cursor_y = 0;    // Cursor coordinates
   DUNIT  stroke_width;                  // Stroke width
   size_t total_clips = 0;               // Temporary record of Document->Clips.size()
   int    rows = 0;                      // Total number of rows in table
   int    row_index = 0;                 // Current row being processed, generally for debugging
   uint8_t  compute_columns = 0;
   ALIGN  align = ALIGN::NIL;            // Horizontal alignment.  If defined, the table will be floating.
   bool   cells_expanded = false;        // false if the table cells have not been expanded to match the inside table width
   bool   reset_row_height = false;      // true if the height of all rows needs to be reset in the current pass
   bool   wrap = false;
   bool   collapsed = false;             // Equivalent to HTML collapsing, eliminates whitespace between rows and cells

   // Entry followed by the minimum width of each column

   bc_table() { code = SCODE::TABLE_START; }

   inline bool floating_x() {
      return (align & (ALIGN::LEFT|ALIGN::RIGHT|ALIGN::HORIZONTAL)) != ALIGN::NIL;
   }

   void computeColumns() { // Compute the default column widths
      if (!compute_columns) return;

      compute_columns = 0;
      cells_expanded = false;

      if (!columns.empty()) {
         for (unsigned j=0; j < columns.size(); j++) {
            //if (ComputeColumns IS 1) {
            //   Columns[j].width = 0;
            //   Columns[j].min_width = 0;
            //}

            if (columns[j].preset_width_rel) { // Percentage width value
               columns[j].width = columns[j].preset_width * width;
            }
            else if (columns[j].preset_width) { // Fixed width value
               columns[j].width = columns[j].preset_width;
            }
            else columns[j].width = 0;

            if (columns[j].min_width > columns[j].width) columns[j].width = columns[j].min_width;
         }
      }
      else columns.clear();
   }
};

struct bc_table_end : public entity {
   bc_table_end() { code = SCODE::TABLE_END; }
};

// It is recommended that font styling for paragraphs take advantage of the embedded font object.  Using a separate
// FONT code raises the chance of confusion for the user, because features like leading are calculated using the
// style registered in the paragraph.

class bc_paragraph : public entity {
   public:
   GuardedObject<objVector> icon; // Icon representation if this is an item
   bc_font font;         // Default font that applies to this paragraph.  Embedding the font style in this way ensures that vertical placement can be computed immediately without looking for a FONT code.
   std::string value = "";
   double x, y, height;  // Layout dimensions, manipulated at run-time
   DUNIT  block_indent;  // Indentation; also equivalent to setting a left margin value
   DUNIT  item_indent;   // For list items only.  This value is carried directly from bc_list.item_indent
   DUNIT  indent;        // Client specified indent value
   DUNIT  line_height;   // Spacing between paragraph lines on word-wrap, affects the cursor's vertical advance.  Expressed as a ratio of the m_line.line_height
   DUNIT  leading;       // Leading whitespace (minimum amount of space from the end of the last paragraph).  Expressed as a ratio of the default line height
   //double trailing;    // Not implemented: Trailing whitespace
   // Options
   bool list_item;       // True if this paragraph represents a list item
   bool trim;
   bool aggregate;

   bc_paragraph() : entity(SCODE::PARAGRAPH_START), x(0), y(0), height(0),
      block_indent(0.0, DU::PIXEL), item_indent(0.0, DU::PIXEL), indent(0.0, DU::PIXEL),
      line_height(1.0, DU::TRUE_LINE_HEIGHT), leading(1.0, DU::LINE_HEIGHT),
      list_item(false), trim(false), aggregate(false) { }

   bc_paragraph(const bc_font &Style) : bc_paragraph() {
      font.apply(Style);
   }
};

struct bc_paragraph_end : public entity {
   bc_paragraph_end() : entity(SCODE::PARAGRAPH_END) { }
};

struct bc_row : public entity {
   GuardedObject<objVectorRectangle> rect_fill;
   double y = 0;
   double row_height = 0; // height of all cells on this row, used when drawing the cells
   double min_height = 0;
   std::string stroke, fill;
   bool vertical_repass = false;

   bc_row() : entity(SCODE::ROW) { }
};

struct bc_row_end : public entity {
   bc_row_end() : entity(SCODE::ROW_END) { }
};

struct bc_cell : public entity {
   GuardedObject<objVectorViewport> viewport;
   GuardedObject<objVectorRectangle> rect_fill; // Custom cell filling
   GuardedObject<objVectorPath> border_path; // Only used when the border stroke is customised
   KEYVALUE args;                 // Cell attributes, intended for event hooks
   std::vector<doc_segment> segments;
   RSTREAM *stream;               // Internally managed byte code content for the cell
   CELL_ID cell_id = 0;           // UID for the cell
   int  column = 0;               // Column number that the cell starts in
   int  col_span = 1;             // Number of columns spanned by this cell (normally set to 1)
   int  row_span = 1;             // Number of rows spanned by this cell
   CB border = CB::NIL;           // Border options
   double x = 0, y = 0;           // Cell coordinates, relative to their container
   double width = 0, height = 0;  // Width and height of the cell
   DUNIT stroke_width;
   ui_hooks hooks;                // UI hooks defined by the client
   std::string edit_def;          // The edit definition that this cell is linked to (if any)
   std::string stroke;
   std::string fill;
   bool modified = false;         // Set to true when content in the cell has been modified
   // NOTE: Update the copy constructor if modifying the field list.

   void set_fill(const std::string);

   bc_cell(int pCellID, int pColumn);
   ~bc_cell();
   bc_cell(const bc_cell &Other);
};

enum class text_token_kind : uint8_t {
   WORD,
   WHITESPACE,
   NEWLINE
};

struct word_token {
   uint16_t start = 0;          // Inclusive byte offset into bc_text.text
   uint16_t stop = 0;           // Exclusive byte offset into bc_text.text
   int16_t width = -1;          // Cached pixel width for WORD tokens, -1 if unknown
   uint16_t last_char = 0;      // Last unicode codepoint in the token, used to restore kerning state on cache hits
   text_token_kind kind = text_token_kind::WORD;

   inline uint16_t length() const {
      return stop - start;
   }

   inline bool is_word() const {
      return kind IS text_token_kind::WORD;
   }

   inline bool is_whitespace() const {
      return kind IS text_token_kind::WHITESPACE;
   }

   inline bool is_newline() const {
      return kind IS text_token_kind::NEWLINE;
   }

   inline void invalidate_width() {
      width = -1;
      last_char = 0;
   }
};

struct bc_text : public entity {
   std::string text;
   std::vector<objVectorText *> vector_text;
   std::vector<word_token> tokens;
   bool formatted = false;
   bool tokens_dirty = true;    // Set when text content changes, requiring retokenisation
   bool widths_dirty = true;    // Set when font context changes, requiring width recalculation
   uint32_t width_cache_generation = 0;
   SEGINDEX segment = -1; // Reference to the first segment that manages this text string.

   bc_text() { code = SCODE::TEXT; }
   bc_text(std::string_view pText) : text(pText) { code = SCODE::TEXT; tokens.reserve(pText.length()/4); }
   bc_text(std::string_view pText, bool pFormatted) : text(pText), formatted(pFormatted) { code = SCODE::TEXT; tokens.reserve(pText.length()/4); }

   void invalidate_tokens() {
      tokens_dirty = true;
      widths_dirty = true;
      width_cache_generation = 0;
   }

   void invalidate_widths() {
      widths_dirty = true;
      width_cache_generation = 0;
      for (auto &t : tokens) t.invalidate_width();
   }

   void tokenise();
};

struct bc_use : public entity {
   std::string id; // Reference to a symbol registered in the Document's SVG object
   bool processed = false;

   bc_use() { code = SCODE::USE; }
   bc_use(std::string pID) : id(pID) { code = SCODE::USE; }
};

struct bc_xml : public entity {
   OBJECTID object_id = 0;   // Reference to the object
   bool owned = false;      // true if the object is owned by a parent (not subject to normal document layout)
   bc_xml() { code = SCODE::XML; }
};

//********************************************************************************************************************
// Common widget management structure

struct widget_mgr {
   std::string name;                   // Client provided name identifier
   std::string label;
   std::string fill;                   // Default fill instruction
   std::string alt_fill;               // Alternative fill instruction for state changes
   std::string font_fill;              // Default fill instruction for user input text
   GuardedObject<objVectorViewport> viewport;
   GuardedObject<objVectorRectangle> rect;    // A vector will host the widget and define a clipping mask for it
   padding pad, final_pad;             // Padding defines external whitespace around the widget
   DUNIT width, height;                // Client can define a fixed width/height, or leave at 0 for auto-sizing
   DUNIT def_size = DUNIT(1.0, DU::FONT_SIZE); // Default height or width if not otherwise specified.
   double final_width, final_height;   // Final dimensions computed during layout
   double label_width = 0;  // If a label is specified, the label_width & pad is in addition to final_width
   DUNIT label_pad;                    // Note that pad can be declared in relative display units
   double x = 0;                       // For floating widgets only, horizontal position calculated during layout
   ALIGN align = ALIGN::NIL;           // NB: If horizontal alignment is defined then the widget is treated as floating.
   bool alt_state = false, internal_page = false;
   bool align_to_text = false;         // Widgets with internal text (buttons, input, combobox) can look best if their internal text aligns with the baseline.
   uint8_t label_pos = 1;                // 0 = left, 1 = right

   inline bool floating_y() {
      return false;
   }

   inline bool floating_x() {
      return (align & (ALIGN::LEFT|ALIGN::RIGHT|ALIGN::HORIZONTAL)) != ALIGN::NIL;
   }

   constexpr double full_height() const { return final_height + final_pad.top + final_pad.bottom; }
};

//********************************************************************************************************************

struct dropdown_item {
   std::string id, value, content, icon;
   dropdown_item(std::string pContent) : content(pContent) { }
};

struct doc_menu {
   GuardedObject<objSurface>  m_surface; // Surface container for the menu UI
   objVectorScene *           m_scene;
   objDocument *              m_doc;     // Independent document for managing the menu layout
   objVectorViewport *        m_view;
   std::vector<dropdown_item> m_items;   // List of items to appear in the menu
   std::function<void(doc_menu &, dropdown_item &)> m_callback; // Callback for item selection
   std::variant<bc_combobox *> m_ref;    // User customisable reference.
   std::string m_style;                  // Optional style override
   scroll_mgr m_scroll;

   // Font options for items in the list

   std::string m_font_face;
   std::string m_font_style;
   int m_font_size;

   int64_t m_show_time = 0; // Time of last acShow()
   int64_t m_hide_time = 0; // Time of last acHide()

   objSurface * create(double);
   objSurface * get();
   void define_font(font_entry *);
   void toggle(objVectorViewport *);
   void reposition(objVectorViewport *);
   void refresh();

   doc_menu() { }

   doc_menu(std::function<void(doc_menu &, dropdown_item &)> pCallback) : m_callback(pCallback) { }

   void show() {
      acShow(*m_surface);
      m_show_time = PreciseTime();
   }

   void hide() {
      acHide(*m_surface);
   }
};

//********************************************************************************************************************

struct bc_button : public entity, widget_mgr {
   padding inner_padding;  // Defines padding around the button's content.  Not to be confused with the widget_mgr outer padding
   RSTREAM *stream;
   std::vector<doc_segment> segments;

   bc_button();
   ~bc_button();
};

struct bc_checkbox : public entity, widget_mgr {
   bc_checkbox() { code = SCODE::CHECKBOX; }
   GuardedObject<objVectorText> label_text;
   bool processed = false;
};

struct bc_combobox : public entity, widget_mgr {
   GuardedObject<objVectorText> label_text;
   GuardedObject<objVectorViewport> clip_vp;
   objVectorText *input;
   doc_menu menu;
   std::string style;
   std::string value;
   std::string last_good_input;

   static void callback(struct doc_menu &, struct dropdown_item &);

   bc_combobox() : menu(&callback) { code = SCODE::COMBOBOX; align_to_text = true; }
};

struct bc_input : public entity, widget_mgr {
   std::string value;
   GuardedObject<objVectorText> label_text;
   GuardedObject<objVectorViewport> clip_vp;
   bool secret = false;

   bc_input() {
      code = SCODE::INPUT;
      align_to_text = true;
   }
};

struct bc_image : public entity, widget_mgr {
   // Images inherit from widget graphics management since the rules are identical
   // Images are inline by default and aligned to the text baseline (matches HTML)
   bc_image() {
      code = SCODE::IMAGE;
      align_to_text = true;
      align = ALIGN::BOTTOM;
   }
};

//********************************************************************************************************************

struct vp_to_entity {
   std::variant<bc_cell *, bc_checkbox *, bc_image *, bc_input *, bc_combobox *, bc_button *> widget;
   bool hover = false;                   // True if the mouse pointer is hovering over the entity
};

//********************************************************************************************************************

struct ui_link {
   bc_link origin;                       // A copy of the original link information (stable pointers are unavailable)
   FloatRect area;                       // Occupied area in the UI
   stream_char cursor_start, cursor_end; // Starting position and end of the link's segment
   std::vector<PathCommand> path;
   RSTREAM *stream;
   bool hover = false;                   // True if the mouse pointer is hovering over the link

   void exec(extDocument *);

   void append_link() {
      path.push_back({ .Type = PE::Move, .X = area.X, .Y = area.Y });
      path.push_back({ .Type = PE::HLineRel, .X = area.Width, });
      path.push_back({ .Type = PE::VLineRel, .Y = area.Height });
      path.push_back({ .Type = PE::HLineRel, .X = -area.Width, });
      path.push_back({ .Type = PE::ClosePath });
   }
};

//********************************************************************************************************************

using CODEVAR = std::variant<bc_text, bc_advance, bc_table, bc_table_end, bc_row, bc_row_end, bc_paragraph,
      bc_paragraph_end, bc_cell, bc_link, bc_link_end, bc_list, bc_list_end, bc_index, bc_index_end,
      bc_font, bc_font_end, bc_xml, bc_image, bc_use, bc_button, bc_checkbox, bc_combobox, bc_input>;

using CODEMAP = std::unordered_map<uint32_t, CODEVAR>; // Pointer stability is required and guaranteed by unordered_map

class RSTREAM {
public:
   std::vector<stream_code> data;
   CODEMAP codes;

   RSTREAM() { data.reserve(8 * 1024); }

   RSTREAM(RSTREAM &Other) {
      data = Other.data;
      codes = Other.codes;
   }

   void clear() {
      data.clear();
      codes.clear();
   }

   std::size_t size() const { return data.size(); }

   // Overloading [] operator to access elements in array style

   stream_code& operator[](const int Index) { return data[Index]; }

   const stream_code& operator[](const int Index) const { return data[Index]; }

   template <class T> T & lookup(const stream_char Index);
   template <class T> T & lookup(const INDEX Index);

   template <class T = entity> T & insert(stream_char &, T &);
   template <class T = entity> T & emplace(stream_char &, T &);
   template <class T = entity> T & emplace(stream_char &);

   inline INDEX find_cell(CELL_ID);
   inline INDEX find_editable_cell(std::string_view);
};

//********************************************************************************************************************

class sorted_segment { // Efficient lookup to the doc_segment array, sorted by vertical position
   public:
   SEGINDEX segment;
   double y;
};

//********************************************************************************************************************

struct doc_layout_metrics {
   uint32_t root_passes = 0;
   uint32_t do_layout_calls = 0;
   uint32_t page_extend_restarts = 0;
   uint32_t page_extend_requests = 0;
   uint32_t vertical_repasses = 0;
   uint32_t table_wrap_restarts = 0;
   uint32_t row_repasses = 0;
   uint32_t cell_layouts = 0;
   uint32_t check_wordwrap_calls = 0;
   uint32_t wrap_through_clips_calls = 0;
   uint32_t new_segment_calls = 0;
   uint32_t clip_count_peak = 0;
   uint32_t segment_count_peak = 0;

   inline void reset() {
      root_passes = 0;
      do_layout_calls = 0;
      page_extend_restarts = 0;
      page_extend_requests = 0;
      vertical_repasses = 0;
      table_wrap_restarts = 0;
      row_repasses = 0;
      cell_layouts = 0;
      check_wordwrap_calls = 0;
      wrap_through_clips_calls = 0;
      new_segment_calls = 0;
      clip_count_peak = 0;
      segment_count_peak = 0;
   }
};

//********************************************************************************************************************

struct glyph_cache_key {
   uintptr_t handle = 0;
   uint32_t glyph = 0;
   uint32_t prev_glyph = 0;

   bool operator==(const glyph_cache_key &Other) const {
      return (handle IS Other.handle) and (glyph IS Other.glyph) and (prev_glyph IS Other.prev_glyph);
   }
};

struct glyph_cache_hash {
   size_t operator()(const glyph_cache_key &Key) const {
      auto hash = size_t(Key.handle);
      hash ^= size_t(Key.glyph) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
      hash ^= size_t(Key.prev_glyph) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
      return hash;
   }
};

struct glyph_cache_value {
   double advance = 0;
};

//********************************************************************************************************************

class extDocument : public objDocument {
   public:
   FUNCTION EventCallback;
   KEYVALUE Vars;   // Variables as defined by the client program.  Transparently accessible like URI params.  Names have priority over params.
   KEYVALUE Params; // Incoming parameters provided via the URI
   std::map<uint32_t, XTag *>   TemplateIndex;
   std::vector<OBJECTID>       UIObjects;    // List of temporary objects in the UI
   std::vector<doc_segment>    Segments;
   std::vector<sorted_segment> SortSegments; // Used for UI interactivity when determining who is front-most
   std::vector<ui_link>        Links;
   ankerl::unordered_dense::map<OBJECTID, vp_to_entity> VPToEntity; // Lookup table for VP -> StreamCode.
   std::vector<mouse_over>     MouseOverChain;
   std::vector<docresource>    Resources; // Tracks resources that are page related.  Terminated on page unload.
   std::vector<tab>            Tabs;
   std::vector<edit_cell>      EditCells;
   doc_layout_metrics          LayoutMetrics;
   ankerl::unordered_dense::map<glyph_cache_key, glyph_cache_value, glyph_cache_hash> GlyphAdvanceCache;
   ankerl::unordered_dense::map<std::string_view, doc_edit> EditDefs;
   std::array<std::vector<FUNCTION>, size_t(DRT::END)> Triggers;
   std::vector<const XTag *> TemplateArgs; // If a template is called, the tag is referred here so that args can be pulled from it
   std::string FontFace;       // Default font face
   std::string WidthCacheFontFace;
   RSTREAM Stream;             // Internal stream buffer
   stream_char SelectStart, SelectEnd;  // Selection start & end (stream index)
   stream_char CursorIndex;    // Position of the cursor if text is selected, or edit mode is active.  It reflects the position at which entered text will be inserted.
   stream_char SelectIndex;    // The end of the selected text area, if text is selected.
   std::string Path;           // Optional file to load on Init()
   std::string PageName;       // Page name to load from the Path
   std::string Bookmark;       // Bookmark name processed from the Path
   std::string WorkingPath;    // String storage for the WorkingPath field
   std::string LinkFill, VisitedLinkFill, LinkSelectFill, FontFill, Highlight;
   std::string Background;     // Background fill instruction
   std::string CursorStroke;   // Stroke instruction for the text cursor
   std::string FontStyle;      // Default font style, usually set to Regular
   std::string WidthCacheFontStyle;
   objXML *Templates;          // All templates for the current document are stored here
   objXML *PretextXML;         // Execute this XML prior to loading a new page.
   objXQuery *Query;           // Standard XQuery object for xquery evaluations
   objSVG *SVG;                // Allocated by the <svg> tag
   objVectorRectangle *Bkgd;   // Background fill object
   XTag    *PageTag;           // Refers to a specific page that is being processed for the layout
   objScript *ClientScript;    // Allows the developer to define a custom default script.
   objScript *DefaultScript;
   doc_edit  *ActiveEditDef; // As for ActiveEditCell, but refers to the active editing definition
   objVectorScene *Scene;    // A document specific scene is required to keep our resources away from the host
   double VPWidth, VPHeight; // Dimensions of the host Viewport
   double FontSize;          // The default font-size, measured in 72 DPI pixels
   double WidthCacheFontSize = 0;
   double MinPageWidth;      // Internal value for managing the page width, speeds up layout processing
   Unit   PageWidth;         // Width of the widest section of the document page.  Can be pre-defined by the client for a fixed or relative width
   double LeftMargin, TopMargin, RightMargin, BottomMargin;
   double CalcWidth;         // Final page width calculated from the layout process
   double XPosition, YPosition; // Scrolling offset
   double ClickX, ClickY;
   double SelectCharX;        // The x coordinate of the SelectIndex character
   double CursorCharX;        // The x coordinate of the CursorIndex character
   double PointerX, PointerY; // Current pointer coordinates on the document surface
   int    TemplatesModified;  // For tracking modifications to Self->Templates (compared to Self->Templates->Modified)
   SEGINDEX ClickSegment;     // The index of the segment that the user clicked on
   SEGINDEX MouseOverSegment; // The index of the segment that the mouse is currently positioned over
   TIMER    FlashTimer;       // For flashing the cursor
   CELL_ID  ActiveEditCellID; // If editing is active, this refers to the ID of the cell being edited
   uint32_t ActiveEditCRC;      // CRC for cell editing area, used for managing onchange notifications
   uint32_t WidthCacheGeneration = 1;
   int16_t  FocusIndex;         // Tab focus index
   int16_t  Invisible;          // Incremented for sections within a hidden index
   uint8_t  Processing;         // If > 0, the page layout is being altered
   bool   RefreshTemplates; // True if the template index requires refreshing.
   bool   UpdatingLayout;   // True if the page layout is in the process of being updated
   bool   PageProcessed;    // True if the parsing of page content has been completed
   bool   NoWhitespace;     // True if the parser should stop injecting whitespace characters
   bool   HasFocus;         // True if the main viewport has the focus
   bool   CursorState;      // True if the edit cursor is on, false if off.  Used for flashing of the cursor

   std::vector<sorted_segment> & get_sorted_segments();

   inline void invalidate_text_width_cache() {
      WidthCacheGeneration++;
      if (!WidthCacheGeneration) WidthCacheGeneration = 1;
   }
};

bc_button::bc_button() {
   code = SCODE::BUTTON;
   stream = new RSTREAM();
   align_to_text = true;
}

bc_button::~bc_button() {
   delete stream;
}

bc_cell::~bc_cell() {
   delete stream;
}

bc_cell::bc_cell(int pCellID, int pColumn)
{
   code    = SCODE::CELL;
   cell_id = pCellID;
   column  = pColumn;
   stream  = new RSTREAM();
}

bc_cell::bc_cell(const bc_cell &Other) {
   if (Other.stream) stream = new RSTREAM(Other.stream[0]);
   cell_id = Other.cell_id;
   column = Other.column;
   col_span = Other.col_span;
   row_span = Other.row_span;
   border = Other.border;
   x = Other.x, y = Other.y;
   width = Other.width, height = Other.height;
   stroke_width = Other.stroke_width;
   hooks = Other.hooks;
   edit_def = Other.edit_def;
   args = Other.args;
   segments = Other.segments;
   stroke = Other.stroke;
   fill = Other.fill;
   modified = Other.modified;
}
