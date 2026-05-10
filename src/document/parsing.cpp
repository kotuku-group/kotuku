// The parsing code converts XML data to a serial byte stream, after which the XML data can be discarded.  A DOM
// of the original XML content is *not* maintained.  After parsing, the stream will be ready for presentation via the
// layout code elsewhere in this code base.
//
// The stream consists of byte codes represented by the entity class.  Each type of code is represented by a C++
// class prefixed with 'bc'.  Each code type has a specific purpose such as defining a new font style, paragraph,
// hyperlink etc.  When a type is instantiated it will be assigned a UID and stored in the Codes hashmap.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <format>

static constexpr uint32_t HASH_let           = strhash("let");
static constexpr uint32_t HASH_for_each      = strhash("for-each");
static constexpr uint32_t HASH_min_width     = strhash("min-width");
static constexpr uint32_t HASH_min_height    = strhash("min-height");
static constexpr uint32_t HASH_minwidth      = strhash("minwidth");
static constexpr uint32_t HASH_minheight     = strhash("minheight");
static constexpr uint32_t HASH_maxwidth      = strhash("maxwidth");
static constexpr uint32_t HASH_maxheight     = strhash("maxheight");
static constexpr uint32_t HASH_xoffset       = strhash("xoffset");
static constexpr uint32_t HASH_yoffset       = strhash("yoffset");
static constexpr uint32_t HASH_insidewidth   = strhash("insidewidth");
static constexpr uint32_t HASH_insideheight  = strhash("insideheight");
static constexpr uint32_t HASH_labelwidth    = strhash("labelwidth");
static constexpr uint32_t HASH_gap           = strhash("gap");
static constexpr uint32_t HASH_object        = strhash("object");
static constexpr uint32_t HASH_params        = strhash("params");
static constexpr uint32_t HASH_args          = strhash("args");
static constexpr uint32_t HASH_vars          = strhash("vars");
static constexpr uint32_t HASH_meta          = strhash("meta");
static constexpr uint32_t HASH_layout        = strhash("layout");
static constexpr uint32_t HASH_loop          = strhash("loop");
static constexpr uint32_t HASH_doc           = strhash("doc");
static constexpr uint32_t HASH_state         = strhash("state");

//********************************************************************************************************************
// Presents a source XTag with either its original attributes or an AVT-expanded attribute overlay,
// whilst preserving the original child tree and identity fields for the rest of the parser.

struct tag_view {
   const XTag *Source = nullptr;
   const kt::vector<XMLAttrib> *AttribPtr = nullptr;
   int ID = 0;
   int ParentID = 0;
   int LineNo = 0;
   XTF Flags = XTF::NIL;
   uint32_t NamespaceID = 0;
   const kt::vector<XMLAttrib> &Attribs;
   const objXML::TAGS &Children;

   explicit tag_view(const XTag &Tag) :
      Source(&Tag), AttribPtr(&Tag.Attribs), ID(Tag.ID), ParentID(Tag.ParentID), LineNo(Tag.LineNo),
      Flags(Tag.Flags), NamespaceID(Tag.NamespaceID), Attribs(Tag.Attribs), Children(Tag.Children) { }

   tag_view(const XTag &Tag, const kt::vector<XMLAttrib> &PreparedAttribs) :
      Source(&Tag), AttribPtr(&PreparedAttribs), ID(Tag.ID), ParentID(Tag.ParentID), LineNo(Tag.LineNo),
      Flags(Tag.Flags), NamespaceID(Tag.NamespaceID), Attribs(PreparedAttribs), Children(Tag.Children) { }

   inline std::string_view name() const { return std::string_view(Attribs[0].Name); }
   inline bool hasContent() const { return (!Children.empty()) and (Children[0].Attribs[0].Name.empty()); }
   inline bool isContent() const { return Attribs[0].Name.empty(); }
   inline bool isTag() const { return !Attribs[0].Name.empty(); }

   inline const std::string * attrib(std::string_view Name) const {
      for (unsigned a=1; a < Attribs.size(); a++) {
         if (Attribs[a].Name IS Name) return &Attribs[a].Value;
      }
      return nullptr;
   }
};

struct diag_attrib_name {
   std::string_view Value;
};

static inline diag_attrib_name attrib_name(std::string_view Name)
{
   return { Name };
}

//********************************************************************************************************************
// State machine for the parser.  This information is discarded after parsing.

struct parser {
   struct process_table {
      struct bc_table *table;
      int row_col;
   };

   struct loop_frame {
      int index = 0;
      int iteration = 0;
      int start = 0;
      int end = 0;
      int step = 1;
      int count = 0;
      bool count_known = false;
      bool end_known = false;
      std::string alias_name;
   };

   struct loop_guard {
      parser *owner = nullptr;
      bool active = false;

      loop_guard(parser *Owner, const loop_frame &Frame) : owner(Owner), active(true) {
         owner->m_loop_stack.push_back(Frame);
      }

      ~loop_guard() {
         if ((active) and (owner) and (not owner->m_loop_stack.empty())) {
            owner->m_loop_stack.pop_back();
         }
      }

      inline loop_frame * frame() {
         if ((not active) or (not owner) or (owner->m_loop_stack.empty())) return nullptr;
         return &owner->m_loop_stack.back();
      }

      inline const loop_frame * frame() const {
         if ((not active) or (not owner) or (owner->m_loop_stack.empty())) return nullptr;
         return &owner->m_loop_stack.back();
      }
   };

   // RAII helper for <let> lexical bindings.  The selected value is installed into
   // m_state_values for the duration of the current scope and the previous binding
   // (if any) is restored automatically when the guard is destroyed.

   using xq_xml_owner = std::shared_ptr<objXML>;

   struct xq_value_binding {
      XPathValue value;
      xq_xml_owner owned_xml;
      kt::vector<objXML *> node_owners; // Aligned with value.node_set so preserved node sequences can reuse owner lookups.

      xq_value_binding() : value(XPVT::String) { }
      explicit xq_value_binding(const XPathValue &Value) : value(Value) {
         if (Value.Type IS XPVT::NodeSet) node_owners.resize(Value.node_set.size(), nullptr);
      }
   };

   struct state_guard {
      parser *owner = nullptr;
      std::string name;
      std::optional<xq_value_binding> previous_value;
      bool had_previous_value = false;
      bool active = false;

      state_guard(parser *Owner, std::string Name, const xq_value_binding &Value)
         : owner(Owner), name(std::move(Name)), active(true)
      {
         auto existing = owner->m_state_values.find(name);
         if (existing != owner->m_state_values.end()) {
            had_previous_value = true;
            previous_value = existing->second;
            existing->second = Value;
         }
         else owner->m_state_values.emplace(name, Value);
      }

      ~state_guard() {
         if ((not active) or (not owner)) return;

         if (had_previous_value and previous_value.has_value()) {
            auto existing = owner->m_state_values.find(name);
            if (existing != owner->m_state_values.end()) existing->second = *previous_value;
            else owner->m_state_values.emplace(name, *previous_value);
         }
         else owner->m_state_values.erase(name);
      }
   };

   // Temporary XQuery context override used by <for-each> so that '.' and relative
   // path expressions resolve against the current item rather than the parser's base
   // document node.

   struct xq_context_frame {
      objXML *xml = nullptr;
      const XTag *node = nullptr;
   };

   // RAII wrapper that pushes a transient XQuery context frame for the current parse
   // branch and pops it again when the scope exits.

   struct xq_context_guard {
      parser *owner = nullptr;
      bool active = false;

      xq_context_guard(parser *Owner, objXML *XML, const XTag *Node) : owner(Owner), active(true) {
         owner->m_xq_context_stack.push_back({ XML, Node });
      }

      ~xq_context_guard() {
         if ((active) and (owner) and (not owner->m_xq_context_stack.empty())) {
            owner->m_xq_context_stack.pop_back();
         }
      }
   };

   extDocument *Self;
   objXML *m_xml;
   objXML *m_source_xml = nullptr;
   objXML *m_doc_xml = nullptr;
   kt::vector<objXML *> m_doc_xml_history; // Retain replaced $doc trees until parse end so stored XQuery values stay valid.
   ankerl::unordered_dense::map<std::string, objXQuery *> m_xq_query_cache; // Compiled XQuery cache keyed by final statement.

   RSTREAM *m_stream;                 // Generated stream content
   std::unique_ptr<RSTREAM> m_stream_alloc;
   objXML *m_inject_xml = nullptr;
   const objXML::TAGS *m_inject_tag = nullptr, *m_header_tag = nullptr, *m_footer_tag = nullptr, *m_body_tag = nullptr;
   objTime *m_time = nullptr;
   kt::vector<loop_frame> m_loop_stack;
   kt::vector<xq_context_frame> m_xq_context_stack; // Active XQuery context overrides for <for-each>.
   ankerl::unordered_dense::map<std::string, xq_value_binding> m_state_values; // Lexical bindings exposed through $state.
   uint16_t m_paragraph_depth = 0;     // Incremented when inside <p> tags
   char  m_in_template = 0;
   bool  m_strip_feeds = false;
   bool  m_check_else  = false;
   bool  m_default_pattern   = false;
   bool  m_button_patterns   = false;
   bool  m_checkbox_patterns = false;
   bool  m_combobox_patterns = false;
   stream_char m_index;
   bc_font m_style; // Reflects the active font during parsing.
   std::stack<bc_list *> m_list_stack;
   std::stack<process_table> m_table_stack;

   parser(extDocument *pSelf, RSTREAM *pStream = nullptr) : Self(pSelf) {
      if (pStream) {
         m_stream = pStream;
         m_index = stream_char(pStream->size());
      }
      else {
         m_stream_alloc = std::make_unique<RSTREAM>();
         m_stream = m_stream_alloc.get();
         m_index = 0;
      }
   }

   parser(extDocument *pSelf, objXML *pXML, RSTREAM *pStream = nullptr) : Self(pSelf), m_xml(pXML), m_source_xml(pXML) {
      if (pStream) {
         m_stream = pStream;
         m_index = stream_char(pStream->size());
      }
      else {
         m_stream_alloc = std::make_unique<RSTREAM>();
         m_stream = m_stream_alloc.get();
         m_index = 0;
      }
   }

   inline TRF  parse_tag(const XTag &, IPF &);
   inline TRF  parse_tags(const objXML::TAGS &, IPF = IPF::NIL);
   inline TRF  parse_tags_with_style(const objXML::TAGS &, bc_font &, IPF = IPF::NIL);
   inline TRF  parse_tags_with_embedded_style(const objXML::TAGS &, bc_font &, IPF = IPF::NIL);
   inline void process_page(objXML *pXML);
   inline void tag_xml_content(XTag &, PXF);
   inline void trim_preformat();

   // Switching out the XML object is sometimes done for things like template injection

   inline objXML * change_xml(objXML *pXML) {
      auto old = m_xml;
      m_xml = pXML;
      return old;
   }

   inline void tag_advance(const tag_view &);
   inline void tag_body(const tag_view &);
   inline void tag_button(const tag_view &);
   inline void tag_call(const tag_view &);
   inline void tag_cell(const tag_view &);
   inline void tag_checkbox(const tag_view &);
   inline void tag_combobox(const tag_view &);
   inline void tag_data(const tag_view &);
   inline void tag_debug(const tag_view &) const;
   inline void tag_div(const tag_view &);
   inline void tag_editdef(const tag_view &);
   inline TRF  tag_for_each(const tag_view &, IPF &);
   inline void tag_font(const tag_view &);
   inline void tag_font_style(const objXML::TAGS &, FSO, std::string_view);
   inline void tag_head(const tag_view &);
   inline void tag_image(const tag_view &);
   inline void tag_include(const tag_view &);
   inline void tag_index(const tag_view &);
   inline void tag_input(const tag_view &);
   inline TRF  tag_let(const tag_view &, IPF &);
   inline void tag_li(const tag_view &);
   inline void tag_link(const tag_view &);
   inline void tag_list(const tag_view &);
   inline void tag_page(const tag_view &) const;
   inline void tag_paragraph(const tag_view &);
   inline void tag_parse(const tag_view &);
   inline void tag_pre(const objXML::TAGS &);
   inline void tag_print(const tag_view &);
   inline void tag_repeat(const tag_view &);
   inline void tag_row(const tag_view &);
   inline void tag_script(const tag_view &);
   inline void tag_svg(const tag_view &);
   inline void tag_table(const tag_view &);
   inline void tag_template(const tag_view &);
   inline void tag_trigger(const tag_view &);
   inline void tag_use(const tag_view &);
   inline bool check_para_attrib(const XMLAttrib &, bc_paragraph *, bc_font &);
   inline bool check_font_attrib(const XMLAttrib &, bc_font &, const tag_view * = nullptr);

   inline loop_frame * active_loop() {
      if (m_loop_stack.empty()) return nullptr;
      return &m_loop_stack.back();
   }

   inline const loop_frame * active_loop() const {
      if (m_loop_stack.empty()) return nullptr;
      return &m_loop_stack.back();
   }

   inline xq_context_frame * active_xq_context() {
      if (m_xq_context_stack.empty()) return nullptr;
      return &m_xq_context_stack.back();
   }

   inline const xq_context_frame * active_xq_context() const {
      if (m_xq_context_stack.empty()) return nullptr;
      return &m_xq_context_stack.back();
   }

   inline int current_loop_index() const {
      if (auto frame = active_loop()) return frame->index;
      else return 0;
   }

   inline bool resolve_loop_alias(std::string_view Name, std::string &Value) const {
      for (int i = std::ssize(m_loop_stack) - 1; i >= 0; i--) {
         auto &frame = m_loop_stack[i];
         if ((not frame.alias_name.empty()) and (frame.alias_name IS Name)) {
            Value = std::to_string(frame.index);
            return true;
         }
      }
      return false;
   }

   ~parser() {
      if (m_time) FreeResource(m_time);
      if (m_doc_xml) FreeResource(m_doc_xml);
      for (auto *xml : m_doc_xml_history) {
         if (xml) FreeResource(xml);
      }
      for (auto &[statement, query] : m_xq_query_cache) {
         if (query) FreeResource(query);
      }
   }

   inline void replace_doc_xml(objXML *XML) {
      if ((m_doc_xml) and (not (m_doc_xml IS XML))) {
         // Stored $state / <for-each> values may still point into the previous $doc tree.
         // Keep the old XML alive until the parser is destroyed rather than invalidating those nodes mid-parse.
         m_doc_xml_history.push_back(m_doc_xml);
      }
      m_doc_xml = XML;
   }

   inline void append_diagnostic(doc_diag_severity Severity, ERR Error, std::string_view Code,
      std::string Message, const XTag *Tag = nullptr, std::string_view TagName = {},
      std::string_view AttribName = {}) const
   {
      if (not Self) return;

      doc_diagnostic diagnostic;
      diagnostic.severity = Severity;
      diagnostic.error = Error;
      diagnostic.code.assign(Code);
      diagnostic.message = std::move(Message);
      diagnostic.path = Self->Path;
      diagnostic.page_name = Self->PageName;

      if (Tag) {
         diagnostic.line_no = Tag->LineNo;
         diagnostic.tag_id = Tag->ID;
         diagnostic.parent_id = Tag->ParentID;
         diagnostic.namespace_id = Tag->NamespaceID;

         if (not TagName.empty()) diagnostic.tag_name.assign(TagName);
         else if (not Tag->Attribs.empty()) diagnostic.tag_name = Tag->Attribs[0].Name;
      }

      if (not AttribName.empty()) diagnostic.attrib_name.assign(AttribName);

      Self->Diagnostics.push_back(std::move(diagnostic));
   }

   inline void append_diagnostic(doc_diag_severity Severity, ERR Error, std::string_view Code,
      std::string Message, const tag_view *Tag, std::string_view AttribName = {}) const
   {
      if (Tag) append_diagnostic(Severity, Error, Code, std::move(Message), Tag->Source, Tag->name(), AttribName);
      else append_diagnostic(Severity, Error, Code, std::move(Message), (const XTag *)nullptr);
   }

   inline void emit_diagnostic_log(doc_diag_severity Severity, const std::string &Message) const
   {
      kt::Log log("Parser");
      if (Severity IS doc_diag_severity::HINT) log.msg("Hint: %s", Message.c_str());
      else if (Severity IS doc_diag_severity::WARNING) log.warning("%s", Message.c_str());
      else log.warning("%s", Message.c_str());
   }

   template <class... Args> inline void log_hint(const tag_view *Tag, std::string_view Code,
      std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::HINT, message);
      append_diagnostic(doc_diag_severity::HINT, ERR::Okay, Code, std::move(message), Tag);
   }

   template <class... Args> inline void log_hint(const tag_view *Tag, std::string_view Code,
      diag_attrib_name AttribName, std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::HINT, message);
      append_diagnostic(doc_diag_severity::HINT, ERR::Okay, Code, std::move(message), Tag, AttribName.Value);
   }

   template <class... Args> inline void log_hint(const XTag *Tag, std::string_view Code,
      std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::HINT, message);
      append_diagnostic(doc_diag_severity::HINT, ERR::Okay, Code, std::move(message), Tag);
   }

   template <class... Args> inline void log_hint(const XTag *Tag, std::string_view Code,
      diag_attrib_name AttribName, std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::HINT, message);
      append_diagnostic(doc_diag_severity::HINT, ERR::Okay, Code, std::move(message), Tag, {}, AttribName.Value);
   }

   template <class... Args> inline void log_warning(const tag_view *Tag, std::string_view Code,
      std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::WARNING, message);
      append_diagnostic(doc_diag_severity::WARNING, ERR::Okay, Code, std::move(message), Tag);
   }

   template <class... Args> inline void log_warning(const tag_view *Tag, std::string_view Code,
      diag_attrib_name AttribName, std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::WARNING, message);
      append_diagnostic(doc_diag_severity::WARNING, ERR::Okay, Code, std::move(message), Tag, AttribName.Value);
   }

   template <class... Args> inline void log_warning(const XTag *Tag, std::string_view Code,
      std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::WARNING, message);
      append_diagnostic(doc_diag_severity::WARNING, ERR::Okay, Code, std::move(message), Tag);
   }

   template <class... Args> inline void log_warning(const XTag *Tag, std::string_view Code,
      diag_attrib_name AttribName, std::string_view Format, Args&&... ArgsList) const
   {
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::WARNING, message);
      append_diagnostic(doc_diag_severity::WARNING, ERR::Okay, Code, std::move(message), Tag, {}, AttribName.Value);
   }

   template <class... Args> inline void log_error(const tag_view *Tag, ERR Error, std::string_view Code,
      std::string_view Format, Args&&... ArgsList) const
   {
      Self->Error = Error;
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::ERROR, message);
      append_diagnostic(doc_diag_severity::ERROR, Error, Code, std::move(message), Tag);
   }

   template <class... Args> inline void log_error(const tag_view *Tag, ERR Error, std::string_view Code,
      diag_attrib_name AttribName, std::string_view Format, Args&&... ArgsList) const
   {
      Self->Error = Error;
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::ERROR, message);
      append_diagnostic(doc_diag_severity::ERROR, Error, Code, std::move(message), Tag, AttribName.Value);
   }

   template <class... Args> inline void log_error(const XTag *Tag, ERR Error, std::string_view Code,
      std::string_view Format, Args&&... ArgsList) const
   {
      Self->Error = Error;
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::ERROR, message);
      append_diagnostic(doc_diag_severity::ERROR, Error, Code, std::move(message), Tag);
   }

   template <class... Args> inline void log_error(const XTag *Tag, ERR Error, std::string_view Code,
      diag_attrib_name AttribName, std::string_view Format, Args&&... ArgsList) const
   {
      Self->Error = Error;
      auto message = std::vformat(Format, std::make_format_args(ArgsList...));
      emit_diagnostic_log(doc_diag_severity::ERROR, message);
      append_diagnostic(doc_diag_severity::ERROR, Error, Code, std::move(message), Tag, {}, AttribName.Value);
   }

   void config_default_pattern() {
      if (m_default_pattern) return;
      m_default_pattern = true;

      if (auto pattern = objVectorPattern::create::global({
            fl::Name("default_pattern"),
            fl::SpreadMethod(VSPREAD::PAD)
         })) {

         objVectorRectangle::create::global({
            fl::Name("default_widget_bkgd"),
            fl::Owner(pattern->Scene->Viewport->UID),
            fl::X(0), fl::Y(0), fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0)),
            fl::Stroke("white"), fl::StrokeWidth(1.0),
            fl::RoundX(SCALE(0.03)), fl::RoundY(SCALE(0.03)),
            fl::Fill("rgb(0 0 0 / .7)")
         });

         Self->Viewport->Scene->addDef("/widget/default", pattern);
      }
   }
};

//********************************************************************************************************************
// Converts XML to byte code, then displays the page that is referenced by the PageName field by calling
// layout_doc().  If the PageName is unspecified, we use the first <page> that has no name, otherwise the first page
// irrespective of the name.
//
// This function does not clear existing data, so you can use it to append new content to existing document content.

void parser::process_page(objXML *pXML)
{
   kt::Log log(__FUNCTION__);

   log.branch("Page: %s", Self->PageName.c_str());

   Self->Diagnostics.clear();

   if (not pXML) {
      log_error((const XTag *)nullptr, ERR::NoData, "doc.missing-source", "No XML source is available for document '{}'", Self->Path);
      return;
   }

   m_xml = pXML;
   m_source_xml = pXML;
   m_state_values.clear();
   m_xq_context_stack.clear();
   Self->RuntimeUID = std::to_string(AllocateID(IDTYPE::GLOBAL));

   // Look for the first page that matches the requested page name (if a name is specified).  Pages can be located anywhere
   // within the XML source - they don't have to be at the root.

   XTag *page = nullptr;
   for (auto &scan : m_xml->Tags) {
      if (not ("page" IS scan.Attribs[0].Name)) continue;

      if (not page) page = &scan;

      if (Self->PageName.empty()) break;
      else if (auto name = scan.attrib("name")) {
         if (Self->PageName IS *name) page = &scan;
      }
   }

   Self->Error = ERR::Okay;
   if ((page) and (not page->Children.empty())) {
      Self->PageTag = page;

      bool no_header = page->attrib("no-header") ? true : false;
      bool no_footer = page->attrib("no-footer") ? true : false;

      // Reset values that are specific to page state

      Self->Segments.clear();
      Self->SortSegments.clear();
      Self->TemplateArgs.clear();
      Self->SelectStart.reset();
      Self->SelectEnd.reset();
      Self->Links.clear();

      Self->XPosition      = 0;
      Self->YPosition      = 0;
      Self->UpdatingLayout = true;
      Self->Error          = ERR::Okay;

      m_index = stream_char(m_stream->size());
      parse_tags(m_xml->Tags, IPF::NO_CONTENT);

      if ((m_header_tag) and (not no_header)) {
         insert_xml(Self, m_stream, m_xml, m_header_tag[0], m_stream->size(), STYLE::RESET_STYLE);
      }

      if (m_body_tag) {
         kt::Log log(__FUNCTION__);
         log.traceBranch("Processing this page through the body tag.");

         auto xml = m_inject_xml;
         auto tags = m_inject_tag;
         m_inject_xml = m_xml;
         m_inject_tag = &page->Children;
         m_in_template++;

         insert_xml(Self, m_stream, m_inject_xml, m_body_tag[0], m_stream->size(), STYLE::RESET_STYLE);

         m_in_template--;
         m_inject_tag = tags;
         m_inject_xml = xml;
      }
      else {
         kt::Log log(__FUNCTION__);
         auto page_name = page->attrib("name");
         log.traceBranch("Processing page '%s'.", page_name ? page_name->c_str() : "");
         insert_xml(Self, m_stream, m_xml, page->Children, m_stream->size(), STYLE::RESET_STYLE);
      }

      if ((m_footer_tag) and (not no_footer)) {
         insert_xml(Self, m_stream, m_xml, m_footer_tag[0], m_stream->size(), STYLE::RESET_STYLE);
      }

      // If an error occurred then we have to kill the document as the stream may contain unsynchronised
      // byte codes (e.g. an unterminated SCODE::TABLE sequence).

      if (Self->Error != ERR::Okay) unload_doc(Self);
   }
   else {
      if (not Self->PageName.empty()) {
         log_error((const XTag *)nullptr, ERR::Search, "doc.page-not-found", "Failed to find page '{}' in document '{}'", Self->PageName, Self->Path);
         error_dialog("Load Failed", std::string("Failed to find page '") + Self->PageName + "' in document '" + Self->Path + "'.");
      }
      else {
         // If no name was specified and there is no page to process, revert to performing a standard insert

         auto orig_size = m_stream->size();
         Self->Error = insert_xml(Self, &Self->Stream, m_xml, m_xml->Tags, m_stream->size(), STYLE::NIL); //STYLE::RESET_STYLE);

         if (Self->Error != ERR::Okay) m_stream->data.resize(orig_size);
      }
   }
/*
   if ((not Self->Error) and (Self->MouseInPage)) {
      double x, y;
      if (not gfxGetRelativeCursorPos(Self->Page->UID, &x, &y)) {
         check_mouse_pos(Self, x, y);
      }
   }
*/
   if (not Self->PageProcessed) {
      for (auto &trigger : copy_triggers(Self, DRT::PAGE_PROCESSED)) {
         if (trigger.isScript()) {
            sc::Call(trigger);
         }
         else if (trigger.isC()) {
            auto routine = (void (*)(APTR, extDocument *, APTR))trigger.Routine;
            kt::SwitchContext context(trigger.Context);
            routine(trigger.Context, Self, trigger.Meta);
         }
      }
   }

   Self->PageProcessed = true;
}

#include "parsing_xquery.cpp"

//********************************************************************************************************************
// Intended for use from parse_tags(), this is the principal function for the parsing of XML tags.  Insertion into
// the stream will occur at Index, which is updated on completion.

TRF parser::parse_tag(const XTag &Tag, IPF &Flags)
{
   kt::Log log(__FUNCTION__);

   if (Self->Error != ERR::Okay) {
      log.traceWarning("Error field is set, returning immediately.");
      return TRF::NIL;
   }

   auto result = TRF::NIL;
   if (Tag.isContent()) {
      if ((Flags & IPF::NO_CONTENT) IS IPF::NIL) {
         if (m_strip_feeds) {
            if (m_paragraph_depth) { // We must be in a paragraph to accept content as text
               unsigned i = 0;
               while ((Tag.Attribs[0].Value[i] IS '\n') or (Tag.Attribs[0].Value[i] IS '\r')) i++;
               if (i > 0) {
                  auto content = Tag.Attribs[0].Value.substr(i);
                  insert_text(Self, m_stream, m_index, content, ((m_style.options & FSO::PREFORMAT) != FSO::NIL));
               }
               else insert_text(Self, m_stream, m_index, Tag.Attribs[0].Value, ((m_style.options & FSO::PREFORMAT) != FSO::NIL));
            }
            m_strip_feeds = false;
         }
         else if (m_paragraph_depth) { // We must be in a paragraph to accept content as text
            insert_text(Self, m_stream, m_index, Tag.Attribs[0].Value, ((m_style.options & FSO::PREFORMAT) != FSO::NIL));
         }
      }
      return result;
   }

   kt::vector<XMLAttrib> prepared_attribs;
   const kt::vector<XMLAttrib> *active_attribs = &Tag.Attribs;
   if (auto err = xq_prepare_tag(this, Tag, prepared_attribs, active_attribs); err != ERR::Okay) {
      Self->Error = err;
      return TRF::NIL;
   }

   auto resolved_tag = (active_attribs IS &Tag.Attribs) ? tag_view(Tag) : tag_view(Tag, *active_attribs);

   auto tagname = std::string_view(resolved_tag.Attribs[0].Name);
   if (tagname.starts_with('$')) tagname.remove_prefix(1);
   auto tag_hash = strhash(tagname);

   if (Self->Templates) { // Check for templates first, as they can be used to override the default tag names.
      if (Self->RefreshTemplates) {
         Self->TemplateIndex.clear();

         for (const XTag &scan : Self->Templates->Tags) {
            for (unsigned i=0; i < scan.Attribs.size(); i++) {
               if ("name" IS scan.Attribs[i].Name) {
                  Self->TemplateIndex[strhash(scan.Attribs[i].Value)] = &scan;
               }
            }
         }

         Self->RefreshTemplates = false;
      }

      if (Self->TemplateIndex.contains(tag_hash)) {
         // Process the template by jumping into it.

         auto xml  = m_inject_xml;
         auto tags = m_inject_tag;
         m_inject_xml = m_xml;
         m_inject_tag = &resolved_tag.Children;
         m_in_template++;

         kt::Log log(__FUNCTION__);
         log.traceBranch("Executing template '%.*s'.", int(tagname.size()), tagname.data());

         Self->TemplateArgs.push_back({ &Tag, active_attribs });
         auto old_xml = change_xml(Self->Templates);

         parse_tags(Self->TemplateIndex[tag_hash]->Children, Flags);

         change_xml(old_xml);
         Self->TemplateArgs.pop_back();

         m_in_template--;
         m_inject_tag = tags;
         m_inject_xml = xml;

         return result;
      }
   }

   if ((Flags & IPF::NO_CONTENT) != IPF::NIL) { // Do nothing when content tags are not allowed
      switch (tag_hash) {
         case HASH_a:
         case HASH_link:
         case HASH_b:
         case HASH_div:
         case HASH_p:
         case HASH_font:
         case HASH_i:
         case HASH_li:
         case HASH_pre:
         case HASH_u:
         case HASH_list:
            log.trace("Content disabled on '%.*s', tag not processed.", int(tagname.size()), tagname.data());
            return result;
         default:
            break;
      }
   }

   if (tagname IS "let") {
      result = tag_let(resolved_tag, Flags);
      return result;
   }
   else if (tagname IS "for-each") {
      result = tag_for_each(resolved_tag, Flags);
      return result;
   }

   switch (tag_hash) {
      // Content tags (tags that affect text, the page layout etc)
      // The content is compulsory, otherwise tag has no effect
      case HASH_a:
      case HASH_link:
         if (not resolved_tag.Children.empty()) tag_link(resolved_tag);
         else log.trace("No content found in tag '%.*s'", int(tagname.size()), tagname.data());
         break;

      case HASH_b:
         if (not resolved_tag.Children.empty()) tag_font_style(resolved_tag.Children, FSO::NIL, "Bold");
         break;

      case HASH_div:
         if (not resolved_tag.Children.empty()) tag_div(resolved_tag);
         break;

      case HASH_p: tag_paragraph(resolved_tag); break;

      case HASH_font:
         if (not resolved_tag.Children.empty()) tag_font(resolved_tag);
         break;

      case HASH_i:
         if (not resolved_tag.Children.empty()) tag_font_style(resolved_tag.Children, FSO::NIL, "Italic");
         break;

      case HASH_li:
         if (not resolved_tag.Children.empty()) tag_li(resolved_tag);
         break;

      case HASH_pre:
         if (not resolved_tag.Children.empty()) tag_pre(resolved_tag.Children);
         break;

      case HASH_u: if (not resolved_tag.Children.empty()) tag_font_style(resolved_tag.Children, FSO::UNDERLINE, m_style.style); break;

      case HASH_list: if (not resolved_tag.Children.empty()) tag_list(resolved_tag); break;

      case HASH_advance: tag_advance(resolved_tag); break;

      case HASH_br:
         insert_text(Self, m_stream, m_index, "\n", true);
         Self->NoWhitespace = true;
         break;

      case HASH_button: tag_button(resolved_tag); break;

      case HASH_checkbox: tag_checkbox(resolved_tag); break;

      case HASH_combobox: tag_combobox(resolved_tag); break;

      case HASH_input: tag_input(resolved_tag); break;

      case HASH_image: tag_image(resolved_tag); break;

      // Conditional command tags

      case HASH_repeat: if (not resolved_tag.Children.empty()) tag_repeat(resolved_tag); break;

      case HASH_break:
         // Breaking stops executing all tags (within this section) beyond the breakpoint.  If in a loop, the loop
         // will stop executing.

         result = TRF::BREAK;
         break;

      case HASH_continue:
         // Continuing - does the same thing as a break but the loop continues.
         // If used when not in a loop, then all sibling tags are skipped.
         result = TRF::CONTINUE;
         break;

      case HASH_if:
         if (check_tag_conditions(this, resolved_tag)) { // Statement is true
            m_check_else = false;
            result = parse_tags(resolved_tag.Children, Flags);
         }
         else m_check_else = true;
         break;

      case HASH_elseif:
         if (m_check_else) {
            if (check_tag_conditions(this, resolved_tag)) { // Statement is true
               m_check_else = false;
               result = parse_tags(resolved_tag.Children, Flags);
            }
         }
         break;

      case HASH_else:
         if (m_check_else) {
            m_check_else = false;
            result = parse_tags(resolved_tag.Children, Flags);
         }
         break;

      case HASH_while: {
         if (not resolved_tag.Children.empty()) {
            loop_frame frame;
            frame.index = 0;
            frame.iteration = 0;
            frame.start = 0;
            frame.step = 1;

            loop_guard loop(this, frame);

            while (true) {
               if (not check_tag_conditions(this, resolved_tag)) break;
               if (Self->Error != ERR::Okay) break;

               result = parse_tags(resolved_tag.Children, Flags);
               if (Self->Error != ERR::Okay) break;

               if ((result & TRF::BREAK) != TRF::NIL) {
                  result = TRF::NIL;
                  break;
               }

               if (auto active_loop = loop.frame()) {
                  active_loop->index += active_loop->step;
                  active_loop->iteration++;
               }

               if ((result & TRF::CONTINUE) != TRF::NIL) {
                  result = TRF::NIL;
                  continue;
               }
            }
         }
         break;
      }

      // Special instructions

      case HASH_call: tag_call(resolved_tag); break;

      case HASH_debug: tag_debug(resolved_tag); break;

      case HASH_focus: Self->FocusIndex = Self->Tabs.size(); break;

      case HASH_include: tag_include(resolved_tag); break;

      case HASH_print: tag_print(resolved_tag); break;

      case HASH_parse: tag_parse(resolved_tag); break;

      case HASH_trigger: tag_trigger(resolved_tag); break;

      // Root level instructions

      case HASH_page: if (not resolved_tag.Children.empty()) tag_page(resolved_tag); break;

      case HASH_svg: if (not resolved_tag.Children.empty()) tag_svg(resolved_tag); break;

      // Table layout instructions

      case HASH_row:
         if ((Flags & IPF::FILTER_TABLE) IS IPF::NIL) {
            log_error(&resolved_tag, ERR::InvalidData, "doc.invalid-parent-tag", "Invalid use of <row> - applied to an invalid parent tag.");
         }
         else if (not resolved_tag.Children.empty()) tag_row(resolved_tag);
         break;

      case HASH_td: // HTML compatibility
      case HASH_cell:
         if ((Flags & IPF::FILTER_ROW) IS IPF::NIL) {
            log_error(&resolved_tag, ERR::InvalidData, "doc.invalid-parent-tag", "Invalid use of <cell> - applied to an invalid parent tag.");
         }
         else if (not resolved_tag.Children.empty()) tag_cell(resolved_tag);
         break;

      case HASH_table: if (not resolved_tag.Children.empty()) tag_table(resolved_tag); break;

      case HASH_tr: if (not resolved_tag.Children.empty()) tag_row(resolved_tag); break;

      // Others

      case HASH_data: tag_data(resolved_tag); break;

      case HASH_edit_def: tag_editdef(resolved_tag); break;

      case HASH_footer:
         if (not resolved_tag.Children.empty()) m_footer_tag = &resolved_tag.Children;
         break;

      case HASH_header:
         if (not resolved_tag.Children.empty()) m_header_tag = &resolved_tag.Children;
         break;

      case HASH_info: tag_head(resolved_tag); break;

      case HASH_inject: // This instruction can only be used from within a template.
         if (m_in_template) {
            if (m_inject_tag) {
               auto old_xml = change_xml(m_inject_xml);
               parse_tags(m_inject_tag[0], Flags);
               change_xml(old_xml);
            }
         }
         else log_warning(&resolved_tag, "doc.inject-outside-template",
            "<inject/> request detected but not used inside a template.");
         break;

      case HASH_use: tag_use(resolved_tag); break;

      case HASH_body: tag_body(resolved_tag); break;

      case HASH_index: tag_index(resolved_tag); break;

      case HASH_script: tag_script(resolved_tag); break;

      case HASH_template: tag_template(resolved_tag); break;

      default:
         if ((Flags & IPF::NO_CONTENT) IS IPF::NIL) {
            log_warning(&resolved_tag, "doc.unsupported-tag",
               "Tag '{}' unsupported as an instruction or template.", tagname);
         }
         else log_warning(&resolved_tag, "doc.content-restricted-tag",
            "Unrecognised tag '{}' used in a content-restricted area.", tagname);
         break;
   } // switch

   return result;
}

//********************************************************************************************************************
// See also process_page(), insert_xml()

TRF parser::parse_tags(const objXML::TAGS &Tags, IPF Flags)
{
   TRF result = TRF::NIL;

   for (const auto &tag : Tags) {
      // Note that Flags will carry state between multiple calls to parse_tag().  This allows if/else to work correctly.
      result = parse_tag(tag, Flags);
      if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
   }

   return result;
}

//********************************************************************************************************************

TRF parser::parse_tags_with_style(const objXML::TAGS &Tags, bc_font &Style, IPF Flags)
{
   bool font_change = false;

   if (Style.style != m_style.style) {
      font_change = true;
   }
   else if ((Style.options & (FSO::NO_WRAP|FSO::ALIGN_CENTER|FSO::ALIGN_RIGHT|FSO::PREFORMAT|FSO::UNDERLINE)) !=
            (m_style.options & (FSO::NO_WRAP|FSO::ALIGN_CENTER|FSO::ALIGN_RIGHT|FSO::PREFORMAT|FSO::UNDERLINE))) {
      font_change = true;
   }
   else if ((Style.valign & (ALIGN::TOP|ALIGN::VERTICAL|ALIGN::BOTTOM)) != (m_style.valign & (ALIGN::TOP|ALIGN::VERTICAL|ALIGN::BOTTOM))) {
      font_change = true;
   }
   else if ((Style.face != m_style.face) or (Style.req_size != m_style.req_size)) {
      font_change = true;
   }
   else if ((Style.fill != m_style.fill)) {
      font_change = true;
   }

   // Prevent em-based size inheritance from compounding on each nested style change.  When the
   // caller has not overridden req_size, any FONT_SIZE (em) value held in m_style has already
   // been resolved to pixels during the parent's layout pass.  If we allow the inherited em to
   // pass through to a new bc_font entry, layout_font() would re-multiply it by the parent's
   // current metrics.Height and compound the size.  Substituting 1em makes the nested entry
   // resolve to the parent's already-computed pixel size rather than doubling (or worse).

   if ((font_change) and (Style.req_size IS m_style.req_size) and
       (Style.req_size.type IS DU::FONT_SIZE)) {
      Style.req_size = DUNIT(1.0, DU::FONT_SIZE);
   }

   auto result = TRF::NIL;
   if (font_change) {
      Style.uid = alloc_bytecode_id();

      auto save_status = m_style;
      m_style = Style;
      m_stream->insert(m_index, m_style);

      for (const auto &tag : Tags) {
         result = parse_tag(tag, Flags);
         if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
      }

      m_style = save_status;
      m_stream->emplace<bc_font_end>(m_index);
   }
   else {
      for (const auto &tag : Tags) {
         // Note that Flags will carry state between multiple calls to parse_tag().  This allows if/else to work correctly.
         result = parse_tag(tag, Flags);
         if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
      }
   }

   return result;
}

//********************************************************************************************************************

TRF parser::parse_tags_with_embedded_style(const objXML::TAGS &Tags, bc_font &Style, IPF Flags)
{
   if (Tags.empty()) return TRF::NIL;

   Style.uid = alloc_bytecode_id();

   auto save_style = m_style;
   m_style = Style;

   TRF result = TRF::NIL;
   for (const auto &tag : Tags) {
      // Note that Flags will carry state between multiple calls to parse_tag().  This allows if/else to work correctly.
      result = parse_tag(tag, Flags);
      if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
   }

   m_style = save_style;
   return result;
}

//********************************************************************************************************************

bool parser::check_para_attrib(const XMLAttrib &Attrib, bc_paragraph *Para, bc_font &Style)
{
   switch (strhash(Attrib.Name)) {
      case HASH_no_wrap:
         Style.options |= FSO::NO_WRAP;
         return true;

      case HASH_v_align: {
         // Vertical alignment defines the vertical position for text in cases where the line height is greater than
         // the text itself (e.g. if an image is anchored in the line).
         ALIGN align = ALIGN::NIL;
         if ("top" IS Attrib.Value) align = ALIGN::TOP;
         else if ("center" IS Attrib.Value) align = ALIGN::VERTICAL;
         else if ("middle" IS Attrib.Value) align = ALIGN::VERTICAL; // synonym
         else if ("bottom" IS Attrib.Value) align = ALIGN::BOTTOM;

         if (align != ALIGN::NIL) {
            Style.valign &= ~(ALIGN::TOP|ALIGN::VERTICAL|ALIGN::BOTTOM);
            Style.valign |= align;
         }
         return true;
      }

      case HASH_kerning:  // REQUIRES CODE and DOCUMENTATION
         return true;

      case HASH_line_height:
         // Line height affects the advance of m_cursor_y whenever a word-wrap occurs.  It is expressed as a multiplier
         // that is applied to m_line.height.

         if (Para) Para->line_height = DUNIT(Attrib.Value, DU::TRUE_LINE_HEIGHT, DBL_MIN);
         return true;

      case HASH_trim:
         if (Para) Para->trim = true;
         return true;

      case HASH_indent:
         if (Para) {
            if (Attrib.Value.empty()) Para->indent = DUNIT(3.0, DU::LINE_HEIGHT);
            else Para->indent = DUNIT(Attrib.Value, DU::PIXEL, 0);
         }
         return true;
   }

   return false;
}

//********************************************************************************************************************
// To assist parsing of <p>, <font>, etc...

bool parser::check_font_attrib(const XMLAttrib &Attrib, bc_font &Style, const tag_view *Tag)
{
   switch (strhash(Attrib.Name)) {
      case HASH_colour:
         log_hint(Tag, "doc.deprecated-attribute", attrib_name(Attrib.Name),
            "Font '{}' attrib is deprecated, use 'fill'.", Attrib.Name);
         [[fallthrough]];
      case HASH_font_fill:
         [[fallthrough]];
      case HASH_fill:
         Style.fill = Attrib.Value;
         return true;

      case HASH_font_face:
         [[fallthrough]];
      case HASH_face: {
         auto face = std::string_view(Attrib.Value);
         auto size_sep = face.find(':');
         if (size_sep != std::string_view::npos) { // Font size follows
            auto str = Attrib.Value.c_str();
            Style.req_size = DUNIT(str + size_sep + 1);

            auto style_sep = face.find(':', size_sep + 1);
            if (style_sep != std::string_view::npos) { // Style follows
               Style.style = str + style_sep + 1;
            }

            face = face.substr(0, size_sep);
         }

         Style.face.assign(face);
         return true;
      }

      case HASH_font_size:
         [[fallthrough]];
      case HASH_size:
         Style.req_size = DUNIT(Attrib.Value);
         return true;

      case HASH_font_style:
         [[fallthrough]];
      case HASH_style:
         Style.style = Attrib.Value;
         return true;
   }

   return false;
}

//********************************************************************************************************************

void parser::trim_preformat()
{
   auto i = m_index.index - 1;
   for (; i > 0; i--) {
      if (m_stream[0][i].code IS SCODE::TEXT) {
         auto &text = m_stream->lookup<bc_text>(i);

         static const std::string ws(" \t\f\v\n\r");
         if (auto found = text.text.find_last_not_of(ws); found != std::string::npos) {
            text.text.erase(found + 1);
            text.invalidate_tokens();
            break;
         }
         else {
            text.text.clear();
            text.invalidate_tokens();
         }
      }
      else break;
   }
}

//********************************************************************************************************************
// Advances the cursor.  It is only possible to advance positively on either axis.

void parser::tag_advance(const tag_view &Tag)
{
   auto &adv = m_stream->emplace<bc_advance>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_x: adv.x = DUNIT(Tag.Attribs[i].Value, DU::PIXEL); break;
         case HASH_y: adv.y = DUNIT(Tag.Attribs[i].Value, DU::PIXEL); break;
      }

      adv.x.value = std::abs(adv.x.value);
      adv.y.value = std::abs(adv.y.value);
   }
}

//********************************************************************************************************************
// NB: If a <body> tag contains any children, it is treated as a template and must contain an <inject/> tag so that
// the XML insertion point is known.

void parser::tag_body(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   static const int MAX_BODY_MARGIN = 500;

   // Body tag needs to be placed before any content

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_clip_path: {
            OBJECTPTR clip;
            if (Self->Scene->findDef(Tag.Attribs[i].Value.c_str(), &clip) IS ERR::Okay) {
               Self->Page->set(FID_Mask, clip);
            }
            break;
         }

         case HASH_cursor_stroke:
            Self->CursorStroke = Tag.Attribs[i].Value;
            break;

         case HASH_link:
            Self->LinkFill = Tag.Attribs[i].Value;
            break;

         // This subroutine supports "N" for setting all margins to "N" and "L T R B" for setting individual
         // margins clockwise

         case HASH_margins: {
            bool rel;
            auto str = std::string_view(Tag.Attribs[i].Value);

            str = read_unit(str, Self->LeftMargin, rel);

            if (not str.empty()) str = read_unit(str, Self->TopMargin, rel);
            else Self->TopMargin = Self->LeftMargin;

            if (not str.empty()) str = read_unit(str, Self->RightMargin, rel);
            else Self->RightMargin = Self->TopMargin;

            if (not str.empty()) str = read_unit(str, Self->BottomMargin, rel);
            else Self->BottomMargin = Self->RightMargin;

            if (Self->LeftMargin < 0) Self->LeftMargin = 0;
            else if (Self->LeftMargin > MAX_BODY_MARGIN) Self->LeftMargin = MAX_BODY_MARGIN;

            if (Self->TopMargin < 0) Self->TopMargin = 0;
            else if (Self->TopMargin > MAX_BODY_MARGIN) Self->TopMargin = MAX_BODY_MARGIN;

            if (Self->RightMargin < 0) Self->RightMargin = 0;
            else if (Self->RightMargin > MAX_BODY_MARGIN) Self->RightMargin = MAX_BODY_MARGIN;

            if (Self->BottomMargin < 0) Self->BottomMargin = 0;
            else if (Self->BottomMargin > MAX_BODY_MARGIN) Self->BottomMargin = MAX_BODY_MARGIN;

            break;
         }

         case HASH_select_fill: // Fill to use when a link is selected (using the tab key to get to a link will select it)
            Self->LinkSelectFill = Tag.Attribs[i].Value;
            break;

         case HASH_fill:
            Self->Background = Tag.Attribs[i].Value;
            break;

         case HASH_face:
            [[fallthrough]];
         case HASH_font_face:
            Self->FontFace = Tag.Attribs[i].Value;
            break;

         case HASH_font_style:
            [[fallthrough]];
         case HASH_style:
            Self->FontStyle = Tag.Attribs[i].Value;
            break;

         case HASH_font_size: {
            // Default font point size, which must be fixed.  Relative sizes like 'em' are not permitted.
            auto val = DUNIT(Tag.Attribs[i].Value);
            switch (val.type) {
               case DU::PIXEL: Self->FontSize = val.value; break;
               default:
                  log_warning(&Tag, "doc.invalid-font-unit", attrib_name(Tag.Attribs[i].Name),
                     "Invalid font size unit '{}'.", Tag.Attribs[i].Value);
            }
            break;
         }

         case HASH_font_colour: // DEPRECATED, use font fill
            log_hint(&Tag, "doc.deprecated-attribute", attrib_name(Tag.Attribs[i].Name),
               "The font-colour attribute is deprecated, use font-fill.");
            [[fallthrough]];
         case HASH_font_fill: // Default font fill
            Self->FontFill = Tag.Attribs[i].Value;
            break;

         case HASH_v_link:
            Self->VisitedLinkFill = Tag.Attribs[i].Value;
            break;

         case HASH_page_width:
            [[fallthrough]];
         case HASH_width:
            Self->PageWidth.read(Tag.Attribs[i].Value);
            if (Self->PageWidth.scaled()) Self->PageWidth.Value = std::clamp(Self->PageWidth.Value, 0.001, 10.0);
            else Self->PageWidth.Value = std::clamp(Self->PageWidth.Value, 1.0, 6000.0);

            if (Self->PageWidth.scaled()) log.msg("Page width forced to %g%%.", Self->PageWidth.Value * 100.0);
            else log.msg("Page width forced to %gpx", Self->PageWidth.Value);
            break;

         default:
            log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
               "Body attribute {}={} is not supported.", Tag.Attribs[i].Name, Tag.Attribs[i].Value);
            break;
      }
   }

   // Overwrite the default Style attributes with the client's choices

   m_style.options  = FSO::NIL;
   m_style.fill     = Self->FontFill;
   m_style.face     = Self->FontFace;
   m_style.style    = Self->FontStyle;
   m_style.req_size = DUNIT(Self->FontSize, DU::PIXEL);

   if (not Tag.Children.empty()) m_body_tag = &Tag.Children;
}

//********************************************************************************************************************
// Use this instruction to call a function during the parsing of the document.
//
// The only argument required by this tag is 'function'.  All following attributes are treated as arguments that are
// passed to the called procedure (note that arguments are passed in the order in which they appear).
//
// Global arguments can be set against the script object itself if the argument is prefixed with an underscore.
//
// To call a function that isn't in the default script, simply specify the name of the target script before the
// function name, split with a dot, e.g. "script.function".
//
// <call function="[script].function" arg1="" arg2="" _global=""/>

void parser::tag_call(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);
   objScript *script = Self->DefaultScript;

   std::string function;
   if (std::ssize(Tag.Attribs) > 1) {
      if ("function" IS Tag.Attribs[1].Name) {
         auto function_ref = std::string_view(Tag.Attribs[1].Value);
         if (auto i = function_ref.find('.');  i != std::string_view::npos) {
            auto script_name = function_ref.substr(0, i);
            auto script_name_text = std::string(script_name);

            OBJECTID id;
            if (FindObject(script_name_text.c_str(), CLASSID::NIL, FOF::NIL, &id) IS ERR::Okay) script = (objScript *)GetObjectPtr(id);

            function.assign(function_ref.substr(i + 1));
         }
         else function = Tag.Attribs[1].Value;
      }
   }

   if (function.empty()) {
      log_error(&Tag, ERR::Syntax, "doc.call-missing-function", "The first attribute to <call/> must be a function reference.");
      return;
   }

   if (not script) {
      log_error(&Tag, ERR::Failed, "doc.call-no-script", "No script is available in this document for the requested <call/>.");
      return;
   }

   {
      kt::Log log(__FUNCTION__);
      log.traceBranch("Calling script #%d function '%s'", script->UID, function.c_str());

      if (Tag.Attribs.size() > 2) {
         std::vector<ScriptArg> args;

         unsigned index = 0;
         for (unsigned i=2; i < Tag.Attribs.size(); i++) {
            if (Tag.Attribs[i].Name.starts_with('_')) { // Global variable setting
               acSetKey(script, Tag.Attribs[i].Name.c_str()+1, Tag.Attribs[i].Value.c_str());
            }
            else if (args[index].Name[0] IS '@') {
               args.emplace_back(Tag.Attribs[i].Name.c_str() + 1, Tag.Attribs[i].Value);
            }
            else args.emplace_back(Tag.Attribs[i].Name.c_str(), Tag.Attribs[i].Value);
         }

         script->exec(function.c_str(), args.data(), args.size());
      }
      else script->exec(function.c_str(), nullptr, 0);
   }

   // Check for a result and print it

   CSTRING *results;
   int size;
   if ((script->get(FID_Results, results, size) IS ERR::Okay) and (size > 0)) {
      auto xmlinc = objXML::create::global(fl::Statement(results[0]), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS));
      if (xmlinc) {
         auto old_xml = change_xml(xmlinc);
         parse_tags(xmlinc->Tags);
         change_xml(old_xml);

         // Add the created XML object to the document rather than destroying it

         Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
      }
      else log_error(&Tag, ERR::Syntax, "doc.call-result-xml-parse-failed",
         "<call/> returned content that could not be parsed as XML/RIPL.");
      FreeResource(results);
   }
}

//********************************************************************************************************************
// A button can have both an on and off pattern, but for our purposes we'll have one pattern and rely on the click
// action to provide feedback that the button has been pressed.

static const char glButtonSVG[] = R"-(
<svg width="100%" height="100%">
  <defs>
    <linearGradient id="darkEdge" x1="0" y1="1" x2="0" y2="0" gradientUnits="objectBoundingBox">
      <stop stop-color="black" stop-opacity="1" offset="0"/>
      <stop stop-color="oklch(0.115 0.000 89.876)" stop-opacity="1" offset="0.84"/>
      <stop stop-color="oklch(0.754 0.000 89.876)" stop-opacity="1" offset="1"/>
    </linearGradient>

    <filter id="dropShadow" color-interpolation-filters="sRGB" primitiveUnits="objectBoundingBox">
      <feGaussianBlur stdDeviation="0.013"/>
    </filter>

    <linearGradient id="shading" gradientUnits="objectBoundingBox" x1="0" y1="0" x2="0" y2="1.04">
      <stop stop-color="white" stop-opacity="0.50" offset="0"/>
      <stop stop-color="oklch(0.596 0.000 89.876)" stop-opacity="0" offset="0.5"/>
      <stop stop-color="black" stop-opacity="0.54" offset="1"/>
    </linearGradient>
  </defs>

  <rect opacity="0.6" fill="black" filter="url(#dropShadow)" width="95%" height="93%"
    x="2.5%" y="4%" ry="7.5%" rx="7.5%"/>
  <rect fill="oklch(0.477 0.028 264.267)" width="95%" height="93%" x="2.5%" y="2.5%" ry="7.5%" rx="7.5%"/>
  <rect rx="7.5%" ry="7.5%" width="95%" height="93%" x="2.5%" y="2.5%" fill="none" stroke="url(#darkEdge)"
    stroke-width="0.5%" stroke-linecap="round" stroke-opacity="0.7" stroke-linejoin="round" stroke-miterlimit="4"/>
  <rect rx="7.5%" ry="7.5%" width="95%" height="93%" x="2.5%" y="2.5%" fill="url(#shading)"/>
</svg>)-";

void parser::tag_button(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   bc_button &widget = m_stream->emplace<bc_button>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto hash = strhash(Tag.Attribs[i].Name);
      auto &value = Tag.Attribs[i].Value;
      if (hash IS HASH_fill)          widget.fill   = value;
      else if (hash IS HASH_alt_fill) widget.alt_fill = value;
      else if (hash IS HASH_name)     widget.name   = value;
      else if (hash IS HASH_width)    widget.width  = DUNIT(value);
      else if (hash IS HASH_height)   widget.height = DUNIT(value);
      else if (hash IS HASH_padding)  widget.pad.parse(value); // Outer padding
      else if (hash IS HASH_cell_padding) widget.inner_padding.parse(value); // Inner padding
      else log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
         "<button> unsupported attribute '{}'.", Tag.Attribs[i].Name);
   }

   widget.internal_page = true;

   if (not m_button_patterns) {
      // Load up the default pattern values for buttons (irrespective of this button utilising them or not)
      m_button_patterns = true;

      if (auto pattern_active = objVectorPattern::create::global({
            fl::Name("button_active"),
            fl::SpreadMethod(VSPREAD::CLIP)
         })) {

         auto svg = objSVG::create { fl::Target(pattern_active->Scene), fl::Statement(glButtonSVG) };

         if (svg.ok()) {
            FreeResource(*svg);
         }
         else { // Revert to a basic rectangle if the SVG didn't process
            objVectorRectangle::create::global({
               fl::Owner(pattern_active->Scene->Viewport->UID),
               fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0)),
               fl::Stroke("oklch(0.371 0.000 89.876 / 0.500)"), fl::StrokeWidth(2.0),
               fl::RoundX(SCALE(0.1)),
               fl::Fill("oklch(0.000 0.000 0.000 / 0.125)")
            });
         }

         Self->Viewport->Scene->addDef("/widget/button/active", pattern_active);
      }

      if (auto pattern_inactive = objVectorPattern::create::global({
            fl::Name("button_inactive"),
            fl::SpreadMethod(VSPREAD::CLIP)
         })) {

         auto svg = objSVG::create { fl::Target(pattern_inactive->Scene), fl::Statement(glButtonSVG) };

         if (svg.ok()) {
            FreeResource(*svg);
         }
         else {
            objVectorRectangle::create::global({
               fl::Owner(pattern_inactive->Scene->Viewport->UID),
               fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0)),
               fl::Stroke("oklch(0.000 0.000 0.000 / 0.250)"), fl::StrokeWidth(2.0),
               fl::RoundX(SCALE(0.1)),
               fl::Fill("oklch(1.000 0.000 89.876 / 0.370)")
            });
         }

         Self->Viewport->Scene->addDef("/widget/button/inactive", pattern_inactive);
      }
   }

   if (widget.fill.empty())      widget.fill      = "url(#/widget/button/inactive)";
   if (widget.font_fill.empty()) widget.font_fill = "rgb(255 255 255 / .86)";

   widget.def_size = DUNIT(1.7, DU::FONT_SIZE);

   if (not Tag.Children.empty()) {
      Self->NoWhitespace = true; // Reset whitespace flag: false allows whitespace at the start of the cell, true prevents whitespace

      parser parse(Self, widget.stream);
      parse.m_in_template = m_in_template;
      parse.m_inject_tag  = m_inject_tag;
      parse.m_inject_xml  = m_inject_xml;

      auto new_style = m_style;
      new_style.options = FSO::ALIGN_CENTER;
      new_style.valign  = ALIGN::CENTER;
      new_style.fill    = widget.font_fill;

      parse.m_paragraph_depth++;
      parse.parse_tags_with_style(Tag.Children, new_style);
      parse.m_paragraph_depth--;
   }

   Self->NoWhitespace = false; // Widgets are treated as inline characters
}

//********************************************************************************************************************

void parser::tag_checkbox(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   bc_checkbox &widget = m_stream->emplace<bc_checkbox>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto hash = strhash(Tag.Attribs[i].Name);
      auto &value = Tag.Attribs[i].Value;

      if (hash IS HASH_label)      widget.label = value;
      else if (hash IS HASH_name)  widget.name  = value;
      else if (hash IS HASH_fill)  widget.fill  = value;
      else if (hash IS HASH_width) widget.width = DUNIT(value);
      else if (hash IS HASH_label_pos) {
         if ("left" IS value) widget.label_pos = 0;
         else if ("right" IS value) widget.label_pos = 1;
      }
      else if (hash IS HASH_value) {
         widget.alt_state = (value IS "1") or (value IS "true");
      }
      else log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
         "<checkbox> unsupported attribute '{}'.", Tag.Attribs[i].Name);
   }

   if (widget.fill.empty()) widget.fill = "url(#/widget/checkbox/off)";

   if (widget.alt_fill.empty()) widget.alt_fill = "url(#/widget/checkbox/on)";

   if (not m_checkbox_patterns) {
      m_checkbox_patterns = true;

      if (auto pattern_on = objVectorPattern::create::global({
            fl::Name("checkbox_on"),
            fl::SpreadMethod(VSPREAD::CLIP)
         })) {

         auto vp = pattern_on->Scene->Viewport;
         objVectorRectangle::create::global({
            fl::Owner(vp->UID),
            fl::X(-8), fl::Y(-8), fl::Width(54), fl::Height(54),
            fl::Stroke("white"), fl::StrokeWidth(2.0),
            fl::RoundX(6), fl::RoundY(6),
            fl::Fill("rgb(0 0 0 / .7)")
         });

         objVectorPath::create::global({
            fl::Owner(vp->UID),
            fl::Sequence("M4.75 15.0832 15.8333 26.1665 33.2498 4 38 8.75 15.8333 35.6665 0 19.8332 4.75 15.0832Z"),
            fl::Fill("white")
         });

         vp->setFields(fl::AspectRatio(ARF::X_MIN|ARF::Y_MIN|ARF::MEET),
            fl::ViewX(-8), fl::ViewY(-8), fl::ViewWidth(54), fl::ViewHeight(54));

         Self->Viewport->Scene->addDef("/widget/checkbox/on", pattern_on);
      }

      if (auto pattern_off = objVectorPattern::create::global({
            fl::Name("checkbox_off"),
            fl::SpreadMethod(VSPREAD::CLIP)
         })) {

         auto vp = pattern_off->Scene->Viewport;
         objVectorRectangle::create::global({
            fl::Owner(vp->UID),
            fl::X(-8), fl::Y(-8), fl::Width(54), fl::Height(54),
            fl::Stroke("white"), fl::StrokeWidth(2.0),
            fl::RoundX(6), fl::RoundY(6),
            fl::Fill("rgb(0 0 0 / .7)")
         });

         objVectorPath::create::global({
            fl::Owner(vp->UID),
            fl::Sequence("M4.75 15.0832 15.8333 26.1665 33.2498 4 38 8.75 15.8333 35.6665 0 19.8332 4.75 15.0832Z"),
            fl::Fill("rgb(255 255 255 / .5)")
         });

         vp->setFields(fl::AspectRatio(ARF::X_MIN|ARF::Y_MIN|ARF::MEET),
            fl::ViewX(-8), fl::ViewY(-8), fl::ViewWidth(54), fl::ViewHeight(54));

         Self->Viewport->Scene->addDef("/widget/checkbox/off", pattern_off);
      }
   }

   widget.def_size = DUNIT(1.4, DU::FONT_SIZE);

   if (not widget.label.empty()) widget.label_pad = DUNIT(0.5, DU::FONT_SIZE);

   if (not widget.name.empty()) Self->Vars[widget.name] = widget.alt_state ? "1" : "0";

   Self->NoWhitespace = false; // Widgets are treated as inline characters
}

//********************************************************************************************************************

void parser::tag_combobox(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   bc_combobox &widget = m_stream->emplace<bc_combobox>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto hash = strhash(Tag.Attribs[i].Name);
      auto &value = Tag.Attribs[i].Value;
      if (hash IS HASH_label)          widget.label = value;
      else if (hash IS HASH_value)     widget.value = value;
      else if (hash IS HASH_fill)      widget.fill = value;
      else if (hash IS HASH_font_fill) widget.font_fill = value;
      else if (hash IS HASH_name)      widget.name = value;
      else if (hash IS HASH_label_pos) {
         if ("left" IS value) widget.label_pos = 0;
         else if ("right" IS value) widget.label_pos = 1;
      }
      else if (hash IS HASH_width) widget.width = DUNIT(value);
      else log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
         "<combobox> unsupported attribute '{}'.", Tag.Attribs[i].Name);
   }

   // Process <option/> tags for the drop-down menu.
   // The content within each option is used as presentation in the drop-down list.
   // The 'value' attrib, if declared, will appear in the combobox for the selected item.
   // The 'id' attrib, if declared, is a UID hidden from the user.

   if (not Tag.Children.empty()) {
      for (auto &scan : Tag.Children) {
         if ("style" IS scan.name()) {
            // Client is overriding the decorator: A custom SVG background is expected, defs and body
            // adjustments may also be provided.
            if (scan.hasContent()) {
               STRING xml_ser;
               if (m_xml->serialise(scan.Children[0].ID, XMF::INCLUDE_SIBLINGS, &xml_ser) IS ERR::Okay) {
                  widget.style = xml_ser;
                  FreeResource(xml_ser);
               }
            }
         }
         else if ("option" IS scan.name()) {
            std::string value;

            if (not scan.Children.empty()) {
               STRING xml_ser;
               if (m_xml->serialise(scan.Children[0].ID, XMF::INCLUDE_SIBLINGS, &xml_ser) IS ERR::Okay) {
                  value = xml_ser;
                  FreeResource(xml_ser);
               }
            }

            if (not value.empty()) {
               auto &option = widget.menu.m_items.emplace_back(value);

               auto id = scan.attrib("id");
               if ((id) and (not id->empty())) option.id = *id;

               auto val = scan.attrib("value");
               if ((val) and (not val->empty())) option.value = *val;

               auto icon = scan.attrib("icon");
               if (icon) option.icon = *icon;
            }
         }
      }
   }

   if (not m_combobox_patterns) {
      // The combobox uses the default fill pattern with an arrow button overlayed on the right.
      m_combobox_patterns = true;

      if (auto pattern_cb = objVectorPattern::create::global({
            fl::Name("combobox"),
            fl::SpreadMethod(VSPREAD::CLIP)
         })) {

         const int PAD = 8;
         auto vp = pattern_cb->Scene->Viewport;
         auto rect = objVectorRectangle::create::global({ // Button background
            fl::Owner(vp->UID),
            fl::X(-(PAD-1)), fl::Y(-(PAD-1)), fl::Width(29+((PAD-1)*2)), fl::Height(29+((PAD-1)*2)),
            fl::Fill("rgb(0 0 0 / .7)")
         });

         std::array<double, 8> round = { 0, 0, 6, 6, 6, 6, 0, 0 };
         rect->set(FID_Rounding, round);

         objVectorPath::create::global({ // Down arrow
            fl::Owner(vp->UID),
            fl::Sequence("M14.5 16.1 26.1 4.5 26.1 12.9 14.5 24.5 2.9 12.9 2.9 4.5 14.5 16.1Z"), // 80% size
            //fl::Sequence("M14.5 16.5 29 2 29 12.5 14.5 27 0 12.5 0 2 14.5 16.5Z"), // Original size; 29x29
            fl::Fill("rgb(255 255 255 / .86)")
         });

         vp->setFields(fl::AspectRatio(ARF::X_MAX|ARF::Y_MIN|ARF::MEET),
            fl::ViewX(-PAD), fl::ViewY(-PAD),
            fl::ViewWidth(29+(PAD*2)), fl::ViewHeight(29+(PAD*2)));

         Self->Viewport->Scene->addDef("/widget/combobox", pattern_cb);
      }
   }

   if (widget.fill.empty()) {
      config_default_pattern();
      widget.fill = "url(#/widget/default);url(#/widget/combobox)";
   }

   if (widget.font_fill.empty()) widget.font_fill = "white";

   widget.def_size  = DUNIT(1.7, DU::FONT_SIZE);
   widget.label_pad = DUNIT(0.5, DU::FONT_SIZE);

   if (not widget.name.empty()) Self->Vars[widget.name] = widget.value;

   Self->NoWhitespace = false; // Widgets are treated as inline characters
}

//********************************************************************************************************************

void parser::tag_input(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   bc_input &widget = m_stream->emplace<bc_input>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &value = Tag.Attribs[i].Value;
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_label:     widget.label = value; break;
         case HASH_value:     widget.value = value; break;
         case HASH_fill:      widget.fill  = value; break;
         case HASH_width:     widget.width = DUNIT(value); break;
         case HASH_font_fill: widget.font_fill = value; break;
         case HASH_name:      widget.name = value; break;
         case HASH_secret:    if (iequals(value, "true")) widget.secret = true; break;
         case HASH_label_pos:
            if ("left" IS value) widget.label_pos = 0;
            else if ("right" IS value) widget.label_pos = 1;
            break;
        default:
            log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
               "<input> unsupported attribute '{}'.", Tag.Attribs[i].Name);
            break;
      }
   }

   if (widget.fill.empty()) {
      config_default_pattern();
      widget.fill = "url(#/widget/default)";
   }

   if (widget.font_fill.empty()) widget.font_fill = "white";

   widget.def_size  = DUNIT(1.7, DU::FONT_SIZE);
   widget.label_pad = DUNIT(0.5, DU::FONT_SIZE);

   if (not widget.name.empty()) Self->Vars[widget.name] = widget.value;

   Self->NoWhitespace = false; // Widgets are treated as inline characters
}

//********************************************************************************************************************

void parser::tag_debug(const tag_view &Tag) const
{
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("msg" IS Tag.Attribs[i].Name) {
         log_hint(&Tag, "doc.debug-message", attrib_name(Tag.Attribs[i].Name), "{}", Tag.Attribs[i].Value);
      }
   }
}

//********************************************************************************************************************
// Declaring <svg> anywhere can execute an SVG statement of any kind, with the caveat that it will target the
// Page viewport (or View if 'background' is used).  The SVG feature should only be used for the creation of resources
// that can then be referred to in the document as named patterns, or via the 'use' option for symbols.
//
// This tag can only be used ONCE per document.  Potentially we could improve this by appending to the existing
// SVG object via data feeds.

void parser::tag_svg(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   if (Self->SVG) {
      log_error(&Tag, ERR::AlreadyDefined, "doc.duplicate-svg", "Illegal attempt to declare <svg/> more than once.");
      return;
   }

   objVectorViewport *target = Self->Page;
   auto filtered_attribs = Tag.Attribs;
   for (int i=1; i < std::ssize(filtered_attribs); i++) {
      if ("placement" IS filtered_attribs[i].Name) {
         if ("foreground" IS filtered_attribs[i].Value) target = Self->Page;
         else if ("background" IS filtered_attribs[i].Value) target = Self->View;
         filtered_attribs.erase(filtered_attribs.begin() + i);
         i--;
      }
   }

   std::ostringstream buffer;
   serialise_tag_fragment(tag_view(*Tag.Source, filtered_attribs), buffer);

   auto xml_svg = buffer.str();
   if (not xml_svg.empty()) {
      if ((Self->SVG = objSVG::create::local({ fl::Statement(xml_svg), fl::Target(target) }))) {
         if (target IS Self->View) acMoveToFront(Self->Page); // Put the page back in front of the background objects
      }
      else Self->Error = ERR::CreateObject;
   }
}

//********************************************************************************************************************
// The <use> tag allows SVG <symbol> declarations to be injected into the parent viewport (e.g. table cells).
// SVG objects that are created in this way are treated as dynamically rendered background graphics.  All text will
// be laid on top with no clipping considerations.
//
// If more sophisticated inline or float embedding is required, the <image> tag is probably more applicable to the
// client.

void parser::tag_use(const tag_view &Tag)
{
   std::string id;
   for (int i = 1; i < std::ssize(Tag.Attribs); i++) {
      if ("href" IS Tag.Attribs[i].Name) {
         id = Tag.Attribs[i].Value;
      }
   }

   if (not id.empty()) {
      auto &use = m_stream->emplace<bc_use>(m_index);
      use.id = id;
   }
}

//********************************************************************************************************************
// Use div to structure the document in a similar way to paragraphs.  The main difference is that it impacts on style
// attributes only, avoiding the declaration of paragraph start and end points and won't cause line breaks.

void parser::tag_div(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   auto new_style = m_style;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("align" IS Tag.Attribs[i].Name) {
         auto align = FSO::NIL;
         auto valid = true;
         if ((Tag.Attribs[i].Value IS "center") or (Tag.Attribs[i].Value IS "middle")) {
            align = FSO::ALIGN_CENTER;
         }
         else if (Tag.Attribs[i].Value IS "right") {
            align = FSO::ALIGN_RIGHT;
         }
         else if (not (Tag.Attribs[i].Value IS "left")) {
            log_warning(&Tag, "doc.invalid-alignment", attrib_name(Tag.Attribs[i].Name),
               "Alignment type '{}' is not supported.", Tag.Attribs[i].Value);
            valid = false;
         }

         if (valid) {
            new_style.options &= ~(FSO::ALIGN_CENTER|FSO::ALIGN_RIGHT);
            new_style.options |= align;
         }
      }
      else if (check_para_attrib(Tag.Attribs[i], 0, new_style));
      else check_font_attrib(Tag.Attribs[i], new_style, &Tag);
   }

   parse_tags_with_style(Tag.Children, new_style);
}

//********************************************************************************************************************
// Creates a new edit definition.  These are stored in a linked list.  Edit definitions are used by referring to them
// by name in table cells.

void parser::tag_editdef(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   doc_edit edit;
   std::string name;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_max_chars:
            edit.max_chars = std::stoi(Tag.Attribs[i].Value);
            if (edit.max_chars < 0) edit.max_chars = -1;
            break;

         case HASH_name: name = Tag.Attribs[i].Value; break;

         case HASH_select_fill: break;

         case HASH_line_breaks:
            if (Tag.Attribs[i].Value IS "true") edit.line_breaks = true;
            else if (Tag.Attribs[i].Value IS "1") edit.line_breaks = true;
            break;

         case HASH_edit_fonts:
         case HASH_edit_images:
         case HASH_edit_tables:
         case HASH_edit_all:
            break;

         case HASH_on_change:
            if (not Tag.Attribs[i].Value.empty()) edit.on_change = Tag.Attribs[i].Value;
            break;

         case HASH_on_exit:
            if (not Tag.Attribs[i].Value.empty()) edit.on_exit = Tag.Attribs[i].Value;
            break;

         case HASH_on_enter:
            if (not Tag.Attribs[i].Value.empty()) edit.on_enter = Tag.Attribs[i].Value;
            break;

         default:
            if (Tag.Attribs[i].Name[0] IS '@') {
               edit.args.emplace_back(make_pair(Tag.Attribs[i].Name, Tag.Attribs[i].Value));
            }
            else if (Tag.Attribs[i].Name[0] IS '_') {
               edit.args.emplace_back(make_pair(Tag.Attribs[i].Name, Tag.Attribs[i].Value));
            }
      }
   }

   if (not name.empty()) Self->EditDefs[name] = std::move(edit);
}

//********************************************************************************************************************
// Use of <meta> for custom information is allowed and is ignored by the parser.

void parser::tag_head(const tag_view &Tag)
{
   // The head contains information about the document

   for (auto &scan : Tag.Children) {
      // Anything allocated here needs to be freed in unload_doc()
      std::string_view name = scan.name();
      if ("title" IS name) {
         if (scan.hasContent()) {
            if (Self->Title) FreeResource(Self->Title);
            Self->Title = kt::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if ("author" IS name) {
         if (scan.hasContent()) {
            if (Self->Author) FreeResource(Self->Author);
            Self->Author = kt::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if ("copyright" IS name) {
         if (scan.hasContent()) {
            if (Self->Copyright) FreeResource(Self->Copyright);
            Self->Copyright = kt::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if ("keywords" IS name) {
         if (scan.hasContent()) {
            if (Self->Keywords) FreeResource(Self->Keywords);
            Self->Keywords = kt::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if ("description" IS name) {
         if (scan.hasContent()) {
            if (Self->Description) FreeResource(Self->Description);
            Self->Description = kt::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else log_hint(&scan, "doc.unknown-head-tag", "Unknown head tag '{}'.", name);
   }
}

//********************************************************************************************************************
// Include XML from another RIPL file.

void parser::tag_include(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);
   bool found_src = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("src" IS Tag.Attribs[i].Name) {
         found_src = true;
         if (auto xmlinc = objXML::create::local(fl::Path(Tag.Attribs[i].Value), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS))) {
            auto old_xml = change_xml(xmlinc);
            parse_tags(xmlinc->Tags);
            Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
            change_xml(old_xml);
         }
         else log_warning(&Tag, "doc.include-failed", attrib_name(Tag.Attribs[i].Name),
            "Failed to include '{}'.", Tag.Attribs[i].Value);
      }
      else if ("volatile" IS Tag.Attribs[i].Name) {
         // Instruct the cache manager that it should always check if the source requires reloading, irrespective of the
         // amount of time that has passed since the last load.
      }
   }

   if (not found_src) {
      log_warning(&Tag, "doc.include-missing-src",
         "<include> directive is missing the required 'src' attribute.");
   }
}

//********************************************************************************************************************
// Bitmap and vector images are supported as vector rectangles that reference a pattern name.  Images need to be
// loaded as resources in an <svg> tag and can then be referenced by name.  Technically any pattern type can be
// referenced as an image - so if the client wants to refer to a gradient for example, that is perfectly legal.
//
// Images are inline by default.  Whitespace on either side is never blocked, whether inline or floating.
// Blocking whitespace can be achieved by embedding the image within <p> tags.
//
// A benefit to rendering SVG images in the <defs> area is that they are converted to cached bitmap textures ahead of
// time.  This provides a considerable speed boost when drawing them, at a potential cost to image quality.

void parser::tag_image(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   bc_image img;
   img.def_size = DUNIT(0.9, DU::FONT_SIZE);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &value = Tag.Attribs[i].Value;

      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_float:
         case HASH_align:
            // Setting the horizontal alignment of an image will cause it to float above the text.
            // If the image is declared inside a paragraph, it will be completely de-anchored as a result.
            {
               auto align = ALIGN::NIL;
               auto valid = true;

               switch (strhash(value)) {
                  case HASH_left:   align = ALIGN::LEFT; break;
                  case HASH_right:  align = ALIGN::RIGHT; break;
                  case HASH_center: align = ALIGN::HORIZONTAL; break;
                  case HASH_middle: align = ALIGN::HORIZONTAL; break; // synonym
                  default:
                     log_warning(&Tag, "doc.invalid-alignment", attrib_name(Tag.Attribs[i].Name),
                        "Invalid alignment value '{}'.", value);
                     valid = false;
                     break;
               }

               if (valid) {
                  img.align &= ~(ALIGN::LEFT|ALIGN::RIGHT|ALIGN::HORIZONTAL);
                  img.align |= align;
               }
            }
            break;

         case HASH_v_align:
            // If the image is anchored and the line is taller than the image, the image can be vertically aligned.
            {
               auto align = ALIGN::NIL;
               auto valid = true;

               switch(strhash(value)) {
                  case HASH_top:    align = ALIGN::TOP; break;
                  case HASH_center: align = ALIGN::VERTICAL; break;
                  case HASH_middle: align = ALIGN::VERTICAL; break; // synonym
                  case HASH_bottom: align = ALIGN::BOTTOM; break;
                  default:
                     log_warning(&Tag, "doc.invalid-vertical-alignment", attrib_name(Tag.Attribs[i].Name),
                        "Invalid valign value '{}'.", value);
                     valid = false;
                     break;
               }

               if (valid) {
                  img.align &= ~(ALIGN::TOP|ALIGN::VERTICAL|ALIGN::BOTTOM);
                  img.align |= align;
               }
            }
            break;

         case HASH_padding: img.pad.parse(value); break;
         case HASH_fill:    img.fill = value; break;
         case HASH_src:     img.fill = value; break;
         case HASH_width:   img.width  = DUNIT(value); break;
         case HASH_height:  img.height = DUNIT(value); break;

         default:
            log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
               "<image> unsupported attribute '{}'.", Tag.Attribs[i].Name);
      }
   }

   if (not img.fill.empty()) {
      if (img.width.value <= 0) img.width.clear(); // Zero is equivalent to 'auto', meaning on-the-fly computation
      if (img.height.value <= 0) img.height.clear();

      if (not img.floating_x()) Self->NoWhitespace = false; // Images count as characters when inline.
      m_stream->emplace(m_index, img);
   }
   else {
      log_warning(&Tag, "doc.image-missing-src", "No src is defined for the <image> tag.");
      return;
   }
}

//********************************************************************************************************************
// Indexes set bookmarks that can be used for quick-scrolling to document sections.  They can also be used to mark
// sections of content that may require run-time modification.
//
// <index name="News">
//   <p>Something in here.</p>
// </index>
//
// If the name attribute is not specified, an attempt will be made to derive the name from the first immediate string
// of the index' content, e.g:
//
//   <index>News</>
//
// The developer can use indexes to bookmark areas of code that are of interest.  The FindIndex() method is used for
// this purpose.

void parser::tag_index(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   uint32_t name = 0;
   bool visible = true;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("name" IS Tag.Attribs[i].Name) {
         name = strhash(Tag.Attribs[i].Value);
      }
      else if ("hide" IS Tag.Attribs[i].Name) {
         visible = false;
      }
      else log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
         "<index> unsupported attribute '{}'.", Tag.Attribs[i].Name);
   }

   if ((not name) and (not Tag.Children.empty())) {
      if (Tag.Children[0].isContent()) name = strhash(Tag.Children[0].Attribs[0].Value);
   }

   if (name) {
      bc_index index(name, glUID++, 0, visible, Self->Invisible ? false : true);

      auto &stream_index = m_stream->emplace(m_index, index);

      if (not Tag.Children.empty()) {
         if (not visible) Self->Invisible++;
         parse_tags(Tag.Children);
         if (not visible) Self->Invisible--;
      }

      bc_index_end end(stream_index.id);
      m_stream->emplace(m_index, end);
   }
   else if (not Tag.Children.empty()) parse_tags(Tag.Children);
}

//********************************************************************************************************************
// If calling a function with 'onclick', all arguments must be identified with the @ prefix.  Parameters will be
// passed to the function in the order in which they are given.  Global values can be set against the document
// object itself, if a parameter is prefixed with an underscore.
//
// Script objects can be specifically referenced when calling a function, e.g. "myscript.function".  If no script
// object is referenced, then it is assumed that the default script contains the function.
//
// <a href="http://" onclick="function" fill="rgb" @arg1="" @arg2="" _global=""/>
//
// Dummy links that specify neither an href or onclick value can be useful in embedded documents if the
// EventCallback feature is used.

void parser::tag_link(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   bc_link link;
   bool select = false;
   link.fill = Self->LinkFill;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_href:
            if (link.type IS LINK::NIL) {
               link.ref = Tag.Attribs[i].Value;
               link.type = LINK::HREF;
            }
            break;

         case HASH_title: // 'title' is the http equivalent of our 'hint'
            [[fallthrough]];
         case HASH_hint:
            link.hint = Tag.Attribs[i].Value;
            break;

         case HASH_on_click:
            if (link.type IS LINK::NIL) { // Function to execute on click
               link.ref = Tag.Attribs[i].Value;
               link.type = LINK::FUNCTION;
            }
            break;

         case HASH_on_motion: // Function to execute on cursor motion
            link.hooks.on_motion = Tag.Attribs[i].Value;
            break;

         case HASH_on_crossing: // Function to execute on cursor crossing in/out
            link.hooks.on_crossing = Tag.Attribs[i].Value;
            break;

         case HASH_fill: link.fill = Tag.Attribs[i].Value; break;

         case HASH_select: select = true; break;

         default:
            if (Tag.Attribs[i].Name.starts_with('@')) link.args.push_back(make_pair(Tag.Attribs[i].Name, Tag.Attribs[i].Value));
            else if (Tag.Attribs[i].Name.starts_with('_')) link.args.push_back(make_pair(Tag.Attribs[i].Name, Tag.Attribs[i].Value));
            else log_warning(&Tag, "doc.unsupported-attribute", attrib_name(Tag.Attribs[i].Name),
               "<a|link> unsupported attribute '{}'.", Tag.Attribs[i].Name);
      }
   }

   if ((link.type != LINK::NIL) or (not Tag.Children.empty())) {
      // Font modifications are saved with the link as opposed to inserting a new bc_font as it's a lot cleaner
      // this way - especially for run-time modifications.

      link.font = bc_font(m_style);
      link.font.options |= FSO::UNDERLINE;
      link.font.fill = link.fill;

      auto &stream_link = m_stream->emplace(m_index, link);

      parse_tags_with_embedded_style(Tag.Children, stream_link.font);

      m_stream->emplace<bc_link_end>(m_index);

      // Links are added to the list of tab locations

      auto i = add_tabfocus(Self, TT::LINK, stream_link.uid);
      if (select) Self->FocusIndex = i;
   }
   else parse_tags(Tag.Children);
}

//********************************************************************************************************************

void parser::tag_list(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);
   bc_list list;

   list.fill     = m_style.fill; // Default fill matches the current font colour
   list.item_num = list.start;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &name  = Tag.Attribs[i].Name;
      auto &value = Tag.Attribs[i].Value;
      if ("fill" IS name) {
         list.fill = value;
      }
      else if ("indent" IS name) {
         // Affects the indenting to apply to child items.
         list.block_indent = DUNIT(value, DU::PIXEL);
      }
      else if ("v-spacing" IS name) {
         // Affects the vertical advance from one list-item paragraph to the next.
         // Equivalent to paragraph leading, not v-spacing, which affects each line
         list.v_spacing = DUNIT(value, DU::LINE_HEIGHT);
         if (list.v_spacing.value < 0) list.v_spacing.clear();
      }
      else if ("type" IS name) {
         if ("bullet" IS value) {
            list.type = bc_list::BULLET;
         }
         else if ("ordered" IS value) {
            list.type = bc_list::ORDERED;
            list.item_indent.clear();
         }
         else if ("custom" IS value) {
            list.type = bc_list::CUSTOM;
            list.item_indent.clear();
         }
      }
      else log_warning(&Tag, "doc.unsupported-attribute", attrib_name(name),
         "<list> unsupported attribute '{}'.", name);
   }

   auto &stream_list = m_stream->emplace(m_index, list);
   m_list_stack.push(&stream_list);

      // Refer to tag_li() to see how list items are managed

      if (not Tag.Children.empty()) parse_tags(Tag.Children);

   m_list_stack.pop();
   m_stream->emplace<bc_list_end>(m_index);

   Self->NoWhitespace = true;
}

//********************************************************************************************************************
// Also see check_para_attrib() for paragraph attributes.

void parser::tag_paragraph(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   m_paragraph_depth++;

   bc_paragraph para(m_style);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("align" IS Tag.Attribs[i].Name) {
         auto align = FSO::NIL;
         auto valid = true;
         if ((iequals(Tag.Attribs[i].Value, "center")) or (iequals(Tag.Attribs[i].Value, "middle"))) {
            align = FSO::ALIGN_CENTER;
         }
         else if (iequals(Tag.Attribs[i].Value, "right")) {
            align = FSO::ALIGN_RIGHT;
         }
         else if (not iequals(Tag.Attribs[i].Value, "left")) {
            log_warning(&Tag, "doc.invalid-alignment", attrib_name(Tag.Attribs[i].Name),
               "Alignment type '{}' is not supported.", Tag.Attribs[i].Value);
            valid = false;
         }

         if (valid) {
            para.font.options &= ~(FSO::ALIGN_CENTER|FSO::ALIGN_RIGHT);
            para.font.options |= align;
         }
      }
      else if ("leading" IS Tag.Attribs[i].Name) {
         // The leading is a line height multiplier that applies to the first line in the paragraph only.
         // It is typically used for things like headers.

         para.leading = DUNIT(Tag.Attribs[i].Value, DU::LINE_HEIGHT, DBL_MIN);
      }
      else if (check_para_attrib(Tag.Attribs[i], &para, para.font));
      else check_font_attrib(Tag.Attribs[i], para.font, &Tag);
   }

   auto &stream_para = m_stream->emplace(m_index, para);

   Self->NoWhitespace = stream_para.trim;

   parse_tags_with_embedded_style(Tag.Children, stream_para.font);

   bc_paragraph_end end;
   m_stream->emplace(m_index, end);
   Self->NoWhitespace = true;
   m_paragraph_depth--;
}

//********************************************************************************************************************
// Templates can be used to create custom tags.

void parser::tag_template(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   if (m_in_template) return;

   // Validate the template (must have a name)

   int n;
   for (n=1; n < std::ssize(Tag.Attribs); n++) {
      if (("name" IS Tag.Attribs[n].Name) and (not Tag.Attribs[n].Value.empty())) break;
   }

   if (n >= std::ssize(Tag.Attribs)) {
      log_warning(&Tag, "doc.template-missing-name",
         "A <template> is missing a name attribute.");
      return;
   }

   STRING strxml;
   if (m_xml->serialise(Tag.ID, XMF::NIL, &strxml) IS ERR::Okay) {
      // Remove any existing tag that uses the same name.
      if (Self->TemplateIndex.contains(strhash(Tag.Attribs[n].Value))) {
         Self->Templates->removeTag(Tag.ID, 1);
      }

      Self->Templates->insertXML(Self->Templates->Tags[0].ID, XMI::END, strxml, 0);
      FreeResource(strxml);

      Self->RefreshTemplates = true; // Force a refresh of the TemplateIndex because the pointers will be changed
   }
   else log_warning(&Tag, "doc.template-serialise-failed",
      "Failed to convert template {} to an XML string.", Tag.ID);
}

//********************************************************************************************************************

void parser::tag_font(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   auto new_style = m_style;
   bool preformat = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("preformat" IS Tag.Attribs[i].Name) {
         new_style.options |= FSO::PREFORMAT;
         preformat = true;
         m_strip_feeds = true;
      }
      else check_font_attrib(Tag.Attribs[i], new_style, &Tag);
   }

   parse_tags_with_style(Tag.Children, new_style);

   if (preformat) trim_preformat();
}

//********************************************************************************************************************
// The use of pre will turn off the automated whitespace management so that all whitespace is parsed as-is.  It does
// not switch to a monospaced font.

void parser::tag_pre(const objXML::TAGS &Children)
{
   auto save = m_strip_feeds;
   m_strip_feeds = true;

   if ((m_style.options & FSO::PREFORMAT) IS FSO::NIL) {
      auto new_style = m_style;
      new_style.options |= FSO::PREFORMAT;
      parse_tags_with_style(Children, new_style);
   }
   else parse_tags(Children);

   m_strip_feeds = save;

   trim_preformat();
}

//********************************************************************************************************************
// By default, a script will be activated when the parser encounters it in the document.  If the script returns a
// result string, that result is assumed to be valid XML and is processed by the parser as such.
//
// If the script contains functions, those functions can be called at any time, either during the parsing process or
// when the document is displayed.
//
// The first script encountered by the parser will serve as the default source for all function calls.  If you need to
// call functions in other scripts then you need to access them by name - e.g. 'myscript.function()'.
//
// Only the first section of content enclosed within the <script> tag (CDATA) is accepted by the script parser.

void parser::tag_script(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);
   objScript *script;
   ERR error;

   std::string type = "tiri";
   std::string src, cachefile, name;
   bool defaultscript = false;
   bool persistent = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto tagname = std::string_view(Tag.Attribs[i].Name);
      if (tagname.starts_with('$')) tagname.remove_prefix(1);
      if (tagname.starts_with('@')) continue; // Variables are set later

      if ("type" IS tagname) {
         type = Tag.Attribs[i].Value;
      }
      else if ("persistent" IS tagname) {
         // A script that is marked as persistent will survive refreshes
         persistent = true;
      }
      else if ("src" IS tagname) {
         if (safe_file_path(Self, Tag.Attribs[i].Value)) {
            src = Tag.Attribs[i].Value;
         }
         else {
            log_error(&Tag, ERR::NoPermission, "doc.script-src-denied", attrib_name(Tag.Attribs[i].Name),
               "Security violation - cannot set script src to '{}'.", Tag.Attribs[i].Value);
            return;
         }
      }
      else if ("cache-file" IS tagname) {
         // Currently the security risk of specifying a cache file is that you could overwrite files on the user's PC,
         // so for the time being this requires unrestricted mode.

         if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) {
            cachefile = Tag.Attribs[i].Value;
         }
         else {
            log_error(&Tag, ERR::NoPermission, "doc.script-cachefile-denied", attrib_name(Tag.Attribs[i].Name),
               "Security violation - cannot set script cachefile to '{}'.", Tag.Attribs[i].Value);
            return;
         }
      }
      else if ("name" IS tagname) {
         name = Tag.Attribs[i].Value;
      }
      else if ("default" IS tagname) {
         defaultscript = true;
      }
      else if ("external" IS tagname) {
         // Reference an external script as the default for function calls
         if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) {
            OBJECTID id;
            if (FindObject(Tag.Attribs[i].Value.c_str(), CLASSID::NIL, FOF::NIL, &id) IS ERR::Okay) {
               Self->DefaultScript = (objScript *)GetObjectPtr(id);
               return;
            }
            else {
               log_warning(&Tag, "doc.script-reference-missing", attrib_name(Tag.Attribs[i].Name),
                  "Failed to find external script '{}'.", Tag.Attribs[i].Value);
               return;
            }
         }
         else {
            log_error(&Tag, ERR::NoPermission, "doc.script-reference-denied", attrib_name(Tag.Attribs[i].Name),
               "Security violation - cannot reference external script '{}'.", Tag.Attribs[i].Value);
            return;
         }
      }
   }

   if ((persistent) and (name.empty())) name = "mainscript";

   if (src.empty()) {
      if ((Tag.Children.empty()) or (not Tag.Children[0].Attribs[0].Name.empty()) or (Tag.Children[0].Attribs[0].Value.empty())) {
         // Ignore if script holds no content
         log_warning(&Tag, "doc.script-missing-content", "<script/> tag does not contain content.");
         return;
      }
   }

   // If the script is persistent and already exists in the resource cache, do nothing further.

   if (persistent) {
      for (auto &resource : Self->Resources) {
         if (resource.type IS RTD::PERSISTENT_SCRIPT) {
            script = (objScript *)GetObjectPtr(resource.object_id);
            if (iequals(name, script->Name)) {
               log.msg("Persistent script discovered.");
               if ((not Self->DefaultScript) or (defaultscript)) Self->DefaultScript = script;
               return;
            }
         }
      }
   }

   if ("tiri" IS type) {
      if (error = NewLocalObject(CLASSID::TIRI, &script); error != ERR::Okay) {
         log_error(&Tag, error, "doc.script-create-failed", "Failed to create a script object for <script type=\"{}\">.", type);
         return;
      }
   }
   else {
      log_error(&Tag, ERR::NoSupport, "doc.unsupported-script-type", "Unsupported script type '{}'.", type);
      return;
   }

   if (not name.empty()) SetName(script, name.c_str());

   if (not src.empty()) script->setPath(src);
   else {
      std::string content = xml::GetContent(*Tag.Source);
      if (not content.empty()) script->setStatement(content);
   }

   if (not cachefile.empty()) script->setCacheFile(cachefile);

   // Object references are to be limited in scope to the Document object

   //script->setObjectScope(Self->Head.UID);

   // Pass custom arguments in the script tag

   for (unsigned i=1; i < Tag.Attribs.size(); i++) {
      auto tagname = std::string_view(Tag.Attribs[i].Name);
      if (tagname.starts_with('$')) tagname.remove_prefix(1);
      if (tagname.starts_with('@')) {
         tagname.remove_prefix(1);
         acSetKey(script, tagname.data(), Tag.Attribs[i].Value.c_str());
      }
   }

   if (auto init_error = InitObject(script); init_error != ERR::Okay) {
      log_error(&Tag, init_error, "doc.script-init-failed", "Failed to initialise script '{}'.", name.empty() ? type : name);
      FreeResource(script);
      return;
   }

   // Pass document arguments to the script

   KEYVALUE *vs;
   if ((script->get(FID_Variables, vs) IS ERR::Okay) and (vs) and (vs->size() > 0)) {
      Self->Vars   = *vs;
      Self->Params = *vs;
   }

   if (auto err = acActivate(script); err != ERR::Okay) {
      log_error(&Tag, err, "doc.script-activate-failed", "Failed to activate script '{}'.", name.empty() ? type : name);
      FreeResource(script);
      return;
   }

   Self->Resources.emplace_back(script->UID, persistent ? RTD::PERSISTENT_SCRIPT : RTD::OBJECT_UNLOAD_DELAY);

   if ((not Self->DefaultScript) or (defaultscript)) {
      log.msg("Script #%d is the default script for this document.", script->UID);
      Self->DefaultScript = script;
   }

   // Any results returned from the script are processed as XML

   CSTRING *results;
   int size;
   if ((script->get(FID_Results, results, size) IS ERR::Okay) and (size > 0)) {
      auto xmlinc = objXML::create::global(fl::Statement(results[0]), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS));
      if (xmlinc) {
         auto old_xml = change_xml(xmlinc);
         parse_tags(xmlinc->Tags);
         change_xml(old_xml);

         // Add the created XML object to the document rather than destroying it

         Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
      }
      else log_error(&Tag, ERR::Syntax, "doc.script-result-xml-parse-failed",
         "<script/> returned content that could not be parsed as XML/RIPL.");
   }
}

//********************************************************************************************************************
// Supports FSO::UNDERLINE and named styles

void parser::tag_font_style(const objXML::TAGS &Children, FSO StyleFlag, std::string_view StyleName)
{
   if (((m_style.options & StyleFlag) != StyleFlag) or (m_style.style != StyleName)) {
      auto new_status = m_style;
      new_status.options |= StyleFlag;
      new_status.style = StyleName;
      parse_tags_with_style(Children, new_status);
   }
   else parse_tags(Children);
}

//********************************************************************************************************************
// List item parser.  List items are essentially paragraphs with automated indentation management.

void parser::tag_li(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   if (m_list_stack.empty()) {
      log_warning(&Tag, "doc.li-outside-list", "<li> was not used inside a <list> tag.");
      return;
   }

   auto &list = m_list_stack.top();

   bc_paragraph para(m_style);
   para.list_item   = true;
   para.item_indent = list->item_indent;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      std::string_view tagname = Tag.Attribs[i].Name;
      if (tagname.starts_with('$')) tagname.remove_prefix(1);

      if ("value" IS tagname) {
         para.value = Tag.Attribs[i].Value;
      }
      else if ("aggregate" IS tagname) {
         if (Tag.Attribs[i].Value IS "true") para.aggregate = true;
         else if (Tag.Attribs[i].Value IS "1") para.aggregate = true;
      }
      else check_para_attrib(Tag.Attribs[i], &para, para.font);
   }

   m_paragraph_depth++;

   if ((list->type IS bc_list::CUSTOM) and (not para.value.empty())) {
      auto &stream_para = m_stream->emplace(m_index, para);

         parse_tags_with_embedded_style(Tag.Children, stream_para.font);

      m_stream->emplace<bc_paragraph_end>(m_index);
   }
   else if (list->type IS bc_list::ORDERED) {
      auto list_size = list->buffer.size();
      list->buffer.push_back(std::to_string(list->item_num) + ".");

      // ItemNum is reset because a child list could be created

      auto save_item = list->item_num;
      list->item_num = 1;

      if (para.aggregate) for (auto &p : list->buffer) para.value += p;
      else para.value = list->buffer.back();

      auto &stream_para = m_stream->emplace(m_index, para);

         parse_tags_with_embedded_style(Tag.Children, stream_para.font);

      m_stream->emplace<bc_paragraph_end>(m_index);

      list->item_num = save_item;
      list->buffer.resize(list_size);

      list->item_num++;
   }
   else { // BULLET
      auto &stream_para = m_stream->emplace(m_index, para);

         parse_tags_with_embedded_style(Tag.Children, stream_para.font);

      m_stream->emplace<bc_paragraph_end>(m_index);
      Self->NoWhitespace = true;
   }

   m_paragraph_depth--;
}

//********************************************************************************************************************
void parser::tag_repeat(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   std::string index_name;
   int loop_start = 0, loop_end = 0, count = 0, step = 0;
   bool have_end = false;
   bool have_count = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("start" IS Tag.Attribs[i].Name) {
         loop_start = std::stoi(Tag.Attribs[i].Value);
         if (loop_start < 0) loop_start = 0;
      }
      else if ("count" IS Tag.Attribs[i].Name) {
         count = std::stoi(Tag.Attribs[i].Value);
         if (count < 0) {
            log_warning(&Tag, "doc.invalid-repeat-count", attrib_name(Tag.Attribs[i].Name),
               "Invalid count value of {}.", count);
            return;
         }
         have_count = true;
      }
      else if ("end" IS Tag.Attribs[i].Name) {
         loop_end = std::stoi(Tag.Attribs[i].Value);
         have_end = true;
      }
      else if ("step" IS Tag.Attribs[i].Name) {
         step = std::stoi(Tag.Attribs[i].Value);
      }
      else if ("index" IS Tag.Attribs[i].Name) {
         // If an index name is specified, the programmer will need to refer to it as [@indexname] and [%index] will
         // remain unchanged from any parent repeat loop.

         index_name = Tag.Attribs[i].Value;
      }
      else if ("select" IS Tag.Attribs[i].Name) {
         log_warning(&Tag, "doc.repeat-select-unsupported", attrib_name(Tag.Attribs[i].Name),
            "<repeat select=\"...\"> is not supported. Use <for-each select=\"...\"> for sequence iteration.");
         Self->Error = ERR::InvalidData;
         return;
      }
   }

   if (not step) {
      if ((have_end) and (loop_end < loop_start)) step = -1;
      else step = 1;
   }

   // Validation - ensure that it will be possible for the repeat loop to execute correctly without the chance of
   // infinite looping.
   //
   // If the user set both count and end attributes, the count attribute will be given the priority here.

   if (have_count) {
      if (count IS 0) return;
      loop_end = loop_start + (count * step);
      have_end = true;
   }
   else if (not have_end) return;

   if (step > 0) {
      if (loop_end < loop_start) step = -step;
   }
   else if (loop_end > loop_start) step = -step;

   log.traceBranch("Performing a repeat loop (start: %d, end: %d, step: %d).", loop_start, loop_end, step);

   auto loop_count = compute_numeric_loop_count(loop_start, loop_end, step);
   if (loop_count <= 0) return;

   loop_frame frame;
   frame.index = loop_start;
   frame.iteration = 0;
   frame.start = loop_start;
   frame.end = loop_end;
   frame.step = step;
   frame.count = loop_count;
   frame.count_known = true;
   frame.end_known = true;
   frame.alias_name = index_name;

   loop_guard loop(this, frame);

   while (auto active_loop = loop.frame()) {
      if (not loop_index_in_range(active_loop->index, active_loop->end, active_loop->step)) break;

      auto result = parse_tags(Tag.Children);
      if (Self->Error != ERR::Okay) break;
      if ((result & TRF::BREAK) != TRF::NIL) break;

      active_loop = loop.frame();
      if (not active_loop) break;

      active_loop->index += active_loop->step;
      active_loop->iteration++;

      if ((result & TRF::CONTINUE) != TRF::NIL) continue;
   }

   log.trace("insert_child:","Repeat loop ends.");
}

//********************************************************************************************************************
// <table columns="10%,90%" width="100" height="100" fill="oklch(0.600 0.000 89.876)">
//  <row><cell>Activate<brk/>This activates the object.</cell></row>
//  <row><cell span="2">Reset</cell></row>
// </table>
//
// <table width="100" height="100" fill="oklch(0.600 0.000 89.876)">
//  <cell>Activate</cell><cell>This activates the object.</cell>
//  <cell colspan="2">Reset</cell>
// </table>
//
// The only acceptable child tags inside a <table> section are row, brk and cell tags.  Command tags are acceptable
// (repeat, if statements, etc).  The table byte code is typically generated as SCODE::TABLE_START, SCODE::ROW,
// SCODE::CELL..., SCODE::ROW_END, SCODE::TABLE_END.

void parser::tag_table(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   auto &table = m_stream->emplace<bc_table>(m_index);
   table.min_width  = DUNIT(1, DU::PIXEL);
   table.min_height = DUNIT(1, DU::PIXEL);

   std::string columns;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &value = Tag.Attribs[i].Value;
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_columns:
            // Column preferences are processed only when the end of the table marker has been reached.
            columns = value;
            break;

         case HASH_width:
            table.min_width = DUNIT(value, DU::PIXEL, DBL_MIN);
            break;

         case HASH_height:
            table.min_height = DUNIT(value, DU::PIXEL, DBL_MIN);
            break;

         case HASH_fill:
            table.fill = value;
            break;

         case HASH_stroke:
            table.stroke = value;
            if (table.stroke_width.empty()) table.stroke_width = DUNIT(1.0, DU::PIXEL);
            break;

         case HASH_collapsed: // Collapsed tables do not have spacing (defined by 'spacing' or 'h-spacing') on the edges
            table.collapsed = true;
            break;

         case HASH_spacing: // Spacing between the cells (H & V)
            table.cell_v_spacing = DUNIT(value, DU::PIXEL, 0);
            table.cell_h_spacing = table.cell_v_spacing;
            break;

         case HASH_v_spacing: // Spacing between the cells (V)
            table.cell_v_spacing = DUNIT(value, DU::PIXEL, 0);
            break;

         case HASH_h_spacing: // Spacing between the cells (H)
            table.cell_h_spacing = DUNIT(value, DU::PIXEL, 0);
            break;

         case HASH_align:
            switch(strhash(value)) {
               case HASH_left:   table.align = ALIGN::LEFT; break;
               case HASH_right:  table.align = ALIGN::RIGHT; break;
               case HASH_center: table.align = ALIGN::HORIZONTAL; break;
               case HASH_middle: table.align = ALIGN::HORIZONTAL; break; // synonym
               default:
                  log_warning(&Tag, "doc.invalid-alignment", attrib_name(Tag.Attribs[i].Name),
                     "Invalid alignment value '{}'.", value);
                  break;
            }
            break;

         case HASH_margins: // synonym
         case HASH_cell_padding: // Equivalent to CSS cell-padding
            table.cell_padding.parse(value);
            break;

         case HASH_stroke_width:
            table.stroke_width = std::clamp(strtod(value.c_str(), nullptr), 0.0, 255.0);
            break;
      }
   }

   m_table_stack.push(process_table { &table, 0 });

      parse_tags(Tag.Children, IPF::NO_CONTENT|IPF::FILTER_TABLE);

   m_table_stack.pop();

   if (not columns.empty()) { // The columns value, if supplied is arranged as a CSV list of column widths
      size_t parsed_columns = 0;
      auto remaining = std::string_view(columns);
      for (; (parsed_columns < table.columns.size()) and (not remaining.empty()); parsed_columns++) {
         auto end = remaining.find(',');
         auto value = (end IS std::string_view::npos) ? remaining : remaining.substr(0, end);

         auto start_trim = value.find_first_not_of(" \t\f\v\n\r");
         if (start_trim IS std::string_view::npos) value = std::string_view();
         else {
            auto end_trim = value.find_last_not_of(" \t\f\v\n\r");
            value = value.substr(start_trim, (end_trim - start_trim) + 1);
         }

         auto column_value = std::string(value);
         table.columns[parsed_columns].preset_width = strtod(column_value.c_str(), nullptr);
         if (value.find('%') != std::string_view::npos) {
            table.columns[parsed_columns].preset_width *= 0.01;
            table.columns[parsed_columns].preset_width_rel = true;
            if ((table.columns[parsed_columns].preset_width < 0.0000001) or (table.columns[parsed_columns].preset_width > 1.0)) {
               log_warning(&Tag, "doc.invalid-table-column", "A <table> column value is invalid.");
               Self->Error = ERR::InvalidDimension;
            }
         }

         if (end IS std::string_view::npos) break;
         remaining.remove_prefix(end + 1);
      }

      if (parsed_columns < table.columns.size()) {
         log_warning(&Tag, "doc.incomplete-table-columns", attrib_name("columns"),
            "columns attribute '{}' did not define all {} columns.", columns, int(table.columns.size()));
      }
   }

   bc_table_end end;
   m_stream->emplace(m_index, end);

   Self->NoWhitespace = true; // Setting this to true will prevent the possibility of blank spaces immediately following the table.
}

//********************************************************************************************************************

void parser::tag_row(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   if (m_table_stack.empty()) {
      log_error(&Tag, ERR::InvalidData, "doc.row-outside-table", "<row> is not defined inside a <table> section.");
      return;
   }

   bc_row escrow;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if ("height" IS Tag.Attribs[i].Name) {
         escrow.min_height = std::clamp(strtod(Tag.Attribs[i].Value.c_str(), nullptr), 0.0, 4000.0);
      }
      else if ("fill" IS Tag.Attribs[i].Name)   escrow.fill   = Tag.Attribs[i].Value;
      else if ("stroke" IS Tag.Attribs[i].Name) escrow.stroke = Tag.Attribs[i].Value;
   }

   auto &table = m_table_stack.top();

   m_stream->emplace(m_index, escrow);

   table.table->rows++;
   table.row_col = 0;

   if (not Tag.Children.empty()) {
      parse_tags(Tag.Children, IPF::NO_CONTENT|IPF::FILTER_ROW);
   }

   bc_row_end end;
   m_stream->emplace(m_index, end);

   if (table.row_col > std::ssize(table.table->columns)) {
      table.table->columns.resize(table.row_col);
   }
}

//********************************************************************************************************************

void parser::tag_cell(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);
   auto new_style = m_style;
   static uint8_t edit_recurse = 0;

   if (m_table_stack.empty()) {
      log_error(&Tag, ERR::InvalidData, "doc.cell-outside-table",
         "<cell> is not defined inside a <table> section.");
      return;
   }

   bc_cell cell(glUID++, m_table_stack.top().row_col);
   bool select = false;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_border: {
            std::vector<std::string> list;
            kt::split(Tag.Attribs[i].Value, std::back_inserter(list));

            for (auto &v : list) {
               if ("all" IS v)         cell.border = CB::ALL;
               else if ("top" IS v)    cell.border |= CB::TOP;
               else if ("left" IS v)   cell.border |= CB::LEFT;
               else if ("bottom" IS v) cell.border |= CB::BOTTOM;
               else if ("right" IS v)  cell.border |= CB::RIGHT;
            }

            break;
         }

         case HASH_col_span:
            cell.col_span = std::clamp(int(std::stoi(Tag.Attribs[i].Value)), 1, 1000);
            break;

         case HASH_row_span:
            cell.row_span = std::clamp(int(std::stoi(Tag.Attribs[i].Value)), 1, 1000);
            break;

         case HASH_edit:
            if (edit_recurse) {
               log_error(&Tag, ERR::Recursion, "doc.edit-cell-recursion", "Edit cells cannot be embedded recursively.");
               return;
            }
            cell.edit_def = Tag.Attribs[i].Value;

            if (not Self->EditDefs.contains(Tag.Attribs[i].Value)) {
               log_warning(&Tag, "doc.missing-edit-definition", attrib_name(Tag.Attribs[i].Name),
                  "Edit definition '{}' does not exist.", Tag.Attribs[i].Value);
               cell.edit_def.clear();
            }
            break;

         case HASH_stroke:
            cell.stroke = Tag.Attribs[i].Value;
            if (cell.stroke_width.empty()) {
               cell.stroke_width = m_table_stack.top().table->stroke_width;
               if (cell.stroke_width.empty()) cell.stroke_width = DUNIT(1.0, DU::PIXEL);
            }
            break;

         case HASH_select: select = true; break;

         case HASH_fill: cell.fill = Tag.Attribs[i].Value; break;

         case HASH_stroke_width: cell.stroke_width = DUNIT(Tag.Attribs[i].Value, DU::PIXEL); break;

         case HASH_no_wrap: new_style.options |= FSO::NO_WRAP; break;

         // NOTE: For the following events, if the client is embedding a document with the intention of using
         // event hooks, they can opt to define an empty string so that the relevant input_events flag is set.

         case HASH_on_click:
            cell.hooks.events |= JTYPE::BUTTON;
            cell.hooks.on_click = Tag.Attribs[i].Value;
            break;

         case HASH_on_motion:
            cell.hooks.events |= JTYPE::MOVEMENT;
            cell.hooks.on_motion = Tag.Attribs[i].Value;
            break;

         case HASH_on_crossing:
            cell.hooks.events |= JTYPE::CROSSING;
            cell.hooks.on_crossing = Tag.Attribs[i].Value;
            break;

         default:
            if (Tag.Attribs[i].Name.starts_with('@')) {
               cell.args[Tag.Attribs[i].Name.substr(1)] = Tag.Attribs[i].Value;
            }
            else cell.args[Tag.Attribs[i].Name] = Tag.Attribs[i].Value;
      }
   }

   if (not cell.edit_def.empty()) edit_recurse++;

   // Edit sections enforce preformatting, which means that all whitespace entered by the user will be taken
   // into account.  The following check sets FSO::PREFORMAT if it hasn't been set already.

   auto cell_index = m_index;

   if (not Tag.Children.empty()) {
      Self->NoWhitespace = true; // Reset whitespace flag: false allows whitespace at the start of the cell, true prevents whitespace

      if ((not cell.edit_def.empty()) and ((m_style.options & FSO::PREFORMAT) IS FSO::NIL)) {
         new_style.options |= FSO::PREFORMAT;
      }

      // Cell content is managed in an internal stream

      parser parse(Self, cell.stream);

      parse.m_in_template = m_in_template;
      parse.m_inject_tag  = m_inject_tag;
      parse.m_inject_xml  = m_inject_xml;
      parse.m_paragraph_depth++;
      parse.parse_tags_with_style(Tag.Children, new_style);
      parse.m_paragraph_depth--;
   }

   auto &stream_cell = m_stream->emplace(m_index, cell);

   m_table_stack.top().row_col += stream_cell.col_span;

   if (not stream_cell.edit_def.empty()) {
      // Links are added to the list of tabbable points

      int tab = add_tabfocus(Self, TT::EDIT, stream_cell.cell_id);
      if (select) Self->FocusIndex = tab;
   }

   if (not stream_cell.edit_def.empty()) edit_recurse--;
}

//********************************************************************************************************************
// No response is required for page tags, but we can check for validity.

void parser::tag_page(const tag_view &Tag) const
{
   kt::Log log(__FUNCTION__);
   if (auto name = Tag.attrib("name")) {
      auto page_name = std::string_view(*name);
      if (not std::ranges::all_of(page_name, [](char Character) {
         return std::isalnum((unsigned char)Character) != 0;
      })) {
         log_warning(&Tag, "doc.invalid-page-name", attrib_name("name"),
            "Page has an invalid name of '{}'. Character support is limited to [A-Z,a-z,0-9].", *name);
      }
   }
}

//********************************************************************************************************************
// Usage: <trigger event="resize" function="script.function"/>

void parser::tag_trigger(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);
   DRT trigger_code;
   objScript *script;
   int64_t function_id;

   std::string event, function_name;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strhash(Tag.Attribs[i].Name)) {
         case HASH_event: event = Tag.Attribs[i].Value; break;
         case HASH_function: function_name = Tag.Attribs[i].Value; break;
      }
   }

   if ((not event.empty()) and (not function_name.empty())) {
      // These are described in the documentation for the AddListener method

      switch(strhash(event)) {
         case HASH_after_layout:    trigger_code = DRT::AFTER_LAYOUT; break;
         case HASH_before_layout:   trigger_code = DRT::BEFORE_LAYOUT; break;
         case HASH_on_click:        trigger_code = DRT::USER_CLICK; break;
         case HASH_on_release:      trigger_code = DRT::USER_CLICK_RELEASE; break;
         case HASH_on_motion:       trigger_code = DRT::USER_MOVEMENT; break;
         case HASH_refresh:         trigger_code = DRT::REFRESH; break;
         case HASH_focus:           trigger_code = DRT::GOT_FOCUS; break;
         case HASH_focus_lost:      trigger_code = DRT::LOST_FOCUS; break;
         case HASH_leaving_page:    trigger_code = DRT::LEAVING_PAGE; break;
         case HASH_page_processed:  trigger_code = DRT::PAGE_PROCESSED; break;
         default:
            log_warning(&Tag, "doc.invalid-trigger-event", attrib_name("event"),
               "Trigger event '{}' for function '{}' is not recognised.", event, function_name);
            return;
      }

      // Get the script

      std::string args;
      if (extract_script(Self, function_name, &script, function_name, args) IS ERR::Okay) {
         if (script->getProcedureID(function_name.c_str(), &function_id) IS ERR::Okay) {
            Self->Triggers[int(trigger_code)].emplace_back(FUNCTION(script, function_id));
         }
         else log_warning(&Tag, "doc.trigger-procedure-missing", attrib_name("function"),
            "Unable to resolve '{}' in script #{} to a function ID (the procedure may not exist).",
            function_name, script->UID);
      }
      else log_warning(&Tag, "doc.trigger-script-missing", attrib_name("function"),
         "The script for '{}' is not available - check if it is declared prior to the trigger tag.", function_name);
   }
   else log_warning(&Tag, "doc.trigger-missing-attrib",
      "<trigger> requires both event and function attributes.");
}
