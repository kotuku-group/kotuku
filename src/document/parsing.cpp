// The parsing code converts XML data to a serial byte stream, after which the XML data can be discarded.  A DOM
// of the original XML content is *not* maintained.  After parsing, the stream will be ready for presentation via the
// layout code elsewhere in this code base.
//
// The stream consists of byte codes represented by the entity class.  Each type of code is represented by a C++
// class prefixed with 'bc'.  Each code type has a specific purpose such as defining a new font style, paragraph,
// hyperlink etc.  When a type is instantiated it will be assigned a UID and stored in the Codes hashmap.

#include <cfloat>

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

      xq_value_binding() : value(XPVT::String) { }
      explicit xq_value_binding(const XPathValue &Value) : value(Value) { }
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
      XTag *node = nullptr;
   };

   // RAII wrapper that pushes a transient XQuery context frame for the current parse
   // branch and pops it again when the scope exits.

   struct xq_context_guard {
      parser *owner = nullptr;
      bool active = false;

      xq_context_guard(parser *Owner, objXML *XML, XTag *Node) : owner(Owner), active(true) {
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
   pf::vector<objXML *> m_doc_xml_history; // Retain replaced $doc trees until parse end so stored XQuery values stay valid.
   ankerl::unordered_dense::map<std::string, objXQuery *> m_xq_query_cache; // Compiled XQuery cache keyed by final statement.

   RSTREAM *m_stream;                 // Generated stream content
   std::unique_ptr<RSTREAM> m_stream_alloc;
   objXML *m_inject_xml = nullptr;
   objXML::TAGS *m_inject_tag = nullptr, *m_header_tag = nullptr, *m_footer_tag = nullptr, *m_body_tag = nullptr;
   objTime *m_time = nullptr;
   pf::vector<loop_frame> m_loop_stack;
   pf::vector<xq_context_frame> m_xq_context_stack; // Active XQuery context overrides for <for-each>.
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

   inline TRF  parse_tag(XTag &, IPF &);
   inline TRF  parse_tags(objXML::TAGS &, IPF = IPF::NIL);
   inline TRF  parse_tags_with_style(objXML::TAGS &, bc_font &, IPF = IPF::NIL);
   inline TRF  parse_tags_with_embedded_style(objXML::TAGS &, bc_font &, IPF = IPF::NIL);
   inline void process_page(objXML *pXML);
   inline void tag_xml_content(XTag &, PXF);
   inline void trim_preformat(extDocument *);

   // Switching out the XML object is sometimes done for things like template injection

   inline objXML * change_xml(objXML *pXML) {
      auto old = m_xml;
      m_xml = pXML;
      return old;
   }

   inline void tag_advance(XTag &);
   inline void tag_body(XTag &);
   inline void tag_button(XTag &);
   inline void tag_call(XTag &);
   inline void tag_cell(XTag &);
   inline void tag_checkbox(XTag &);
   inline void tag_combobox(XTag &);
   inline void tag_data(XTag &);
   inline void tag_debug(XTag &);
   inline void tag_div(XTag &);
   inline void tag_editdef(XTag &);
   inline TRF  tag_for_each(XTag &, IPF &);
   inline void tag_font(XTag &);
   inline void tag_font_style(objXML::TAGS &, FSO, std::string_view);
   inline void tag_head(XTag &);
   inline void tag_image(XTag &);
   inline void tag_include(XTag &);
   inline void tag_index(XTag &);
   inline void tag_input(XTag &);
   inline TRF  tag_let(XTag &, IPF &);
   inline void tag_li(XTag &);
   inline void tag_link(XTag &);
   inline void tag_list(XTag &);
   inline void tag_page(XTag &);
   inline void tag_paragraph(XTag &);
   inline void tag_parse(XTag &);
   inline void tag_pre(objXML::TAGS &);
   inline void tag_print(XTag &);
   inline void tag_repeat(XTag &);
   inline void tag_row(XTag &);
   inline void tag_script(XTag &);
   inline void tag_svg(XTag &);
   inline void tag_table(XTag &);
   inline void tag_template(XTag &);
   inline void tag_trigger(XTag &);
   inline void tag_use(XTag &);
   inline bool check_para_attrib(const XMLAttrib &, bc_paragraph *, bc_font &);
   inline bool check_font_attrib(const XMLAttrib &, bc_font &);

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
         if ((not frame.alias_name.empty()) and (iequals(frame.alias_name, Name))) {
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
            fl::Fill("rgba(0,0,0,.7)")
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
   pf::Log log(__FUNCTION__);

   log.branch("Page: %s", Self->PageName.c_str());

   if (not pXML) { Self->Error = ERR::NoData; return; }
   m_xml = pXML;
   m_source_xml = pXML;
   m_state_values.clear();
   m_xq_context_stack.clear();
   Self->RuntimeUID = std::to_string(AllocateID(IDTYPE::GLOBAL));

   // Look for the first page that matches the requested page name (if a name is specified).  Pages can be located anywhere
   // within the XML source - they don't have to be at the root.

   XTag *page = nullptr;
   for (auto &scan : m_xml->Tags) {
      if (not iequals("page", scan.Attribs[0].Name)) continue;

      if (not page) page = &scan;

      if (Self->PageName.empty()) break;
      else if (auto name = scan.attrib("name")) {
         if (iequals(Self->PageName, *name)) page = &scan;
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
         pf::Log log(__FUNCTION__);
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
         pf::Log log(__FUNCTION__);
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
         auto msg = std::string("Failed to find page '") + Self->PageName + "' in document '" + Self->Path + "'.";
         error_dialog("Load Failed", msg);
         Self->Error = ERR::Search;
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
      for (auto &trigger : Self->Triggers[int(DRT::PAGE_PROCESSED)]) {
         if (trigger.isScript()) {
            sc::Call(trigger);
         }
         else if (trigger.isC()) {
            auto routine = (void (*)(APTR, extDocument *, APTR))trigger.Routine;
            pf::SwitchContext context(trigger.Context);
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

TRF parser::parse_tag(XTag &Tag, IPF &Flags)
{
   pf::Log log(__FUNCTION__);

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

   pf::vector<xq_attrib_change> prepared_attribs;
   if (auto err = xq_prepare_attribs(this, Tag, prepared_attribs); err != ERR::Okay) {
      Self->Error = err;
      return TRF::NIL;
   }

   auto restore_attribs = pf::Defer([&]() {
      xq_restore_attribs(Tag, prepared_attribs);
   });

   auto tagname = Tag.Attribs[0].Name;
   if (tagname.starts_with('$')) tagname.erase(0, 1);
   auto tag_hash = strihash(tagname);

   if (Self->Templates) { // Check for templates first, as they can be used to override the default tag names.
      if (Self->RefreshTemplates) {
         Self->TemplateIndex.clear();

         for (XTag &scan : Self->Templates->Tags) {
            for (unsigned i=0; i < scan.Attribs.size(); i++) {
               if (iequals("name", scan.Attribs[i].Name)) {
                  Self->TemplateIndex[strihash(scan.Attribs[i].Value)] = &scan;
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
         m_inject_tag = &Tag.Children;
         m_in_template++;

         pf::Log log(__FUNCTION__);
         log.traceBranch("Executing template '%s'.", tagname.c_str());

         Self->TemplateArgs.push_back(&Tag);
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
            log.trace("Content disabled on '%s', tag not processed.", tagname.c_str());
            return result;
         default:
            break;
      }
   }

   if (iequals(tagname, "let")) {
      result = tag_let(Tag, Flags);
      return result;
   }
   else if (iequals(tagname, "for-each")) {
      result = tag_for_each(Tag, Flags);
      return result;
   }

   switch (tag_hash) {
      // Content tags (tags that affect text, the page layout etc)
      // The content is compulsory, otherwise tag has no effect
      case HASH_a:
      case HASH_link:
         if (not Tag.Children.empty()) tag_link(Tag);
         else log.trace("No content found in tag '%s'", tagname.c_str());
         break;

      case HASH_b:
         if (not Tag.Children.empty()) tag_font_style(Tag.Children, FSO::NIL, "Bold");
         break;

      case HASH_div:
         if (not Tag.Children.empty()) tag_div(Tag);
         break;

      case HASH_p: tag_paragraph(Tag); break;

      case HASH_font:
         if (not Tag.Children.empty()) tag_font(Tag);
         break;

      case HASH_i:
         if (not Tag.Children.empty()) tag_font_style(Tag.Children, FSO::NIL, "Italic");
         break;

      case HASH_li:
         if (not Tag.Children.empty()) tag_li(Tag);
         break;

      case HASH_pre:
         if (not Tag.Children.empty()) tag_pre(Tag.Children);
         break;

      case HASH_u: if (not Tag.Children.empty()) tag_font_style(Tag.Children, FSO::UNDERLINE, m_style.style); break;

      case HASH_list: if (not Tag.Children.empty()) tag_list(Tag); break;

      case HASH_advance: tag_advance(Tag); break;

      case HASH_br:
         insert_text(Self, m_stream, m_index, "\n", true);
         Self->NoWhitespace = true;
         break;

      case HASH_button: tag_button(Tag); break;

      case HASH_checkbox: tag_checkbox(Tag); break;

      case HASH_combobox: tag_combobox(Tag); break;

      case HASH_input: tag_input(Tag); break;

      case HASH_image: tag_image(Tag); break;

      // Conditional command tags

      case HASH_repeat: if (not Tag.Children.empty()) tag_repeat(Tag); break;

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
         if (check_tag_conditions(this, Tag)) { // Statement is true
            m_check_else = false;
            result = parse_tags(Tag.Children, Flags);
         }
         else m_check_else = true;
         break;

      case HASH_elseif:
         if (m_check_else) {
            if (check_tag_conditions(this, Tag)) { // Statement is true
               m_check_else = false;
               result = parse_tags(Tag.Children, Flags);
            }
         }
         break;

      case HASH_else:
         if (m_check_else) {
            m_check_else = false;
            result = parse_tags(Tag.Children, Flags);
         }
         break;

      case HASH_while: {
         if (not Tag.Children.empty()) {
            loop_frame frame;
            frame.index = 0;
            frame.iteration = 0;
            frame.start = 0;
            frame.step = 1;

            loop_guard loop(this, frame);

            while (true) {
               if (not check_tag_conditions(this, Tag)) break;
               if (Self->Error != ERR::Okay) break;

               result = parse_tags(Tag.Children, Flags);
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

      case HASH_call: tag_call(Tag); break;

      case HASH_debug: tag_debug(Tag); break;

      case HASH_focus: Self->FocusIndex = Self->Tabs.size(); break;

      case HASH_include: tag_include(Tag); break;

      case HASH_print: tag_print(Tag); break;

      case HASH_parse: tag_parse(Tag); break;

      case HASH_trigger: tag_trigger(Tag); break;

      // Root level instructions

      case HASH_page: if (not Tag.Children.empty()) tag_page(Tag); break;

      case HASH_svg: if (not Tag.Children.empty()) tag_svg(Tag); break;

      // Table layout instructions

      case HASH_row:
         if ((Flags & IPF::FILTER_TABLE) IS IPF::NIL) {
            log.warning("Invalid use of <row> - Applied to invalid parent tag.");
            Self->Error = ERR::InvalidData;
         }
         else if (not Tag.Children.empty()) tag_row(Tag);
         break;

      case HASH_td: // HTML compatibility
      case HASH_cell:
         if ((Flags & IPF::FILTER_ROW) IS IPF::NIL) {
            log.warning("Invalid use of <cell> - Applied to invalid parent tag.");
            Self->Error = ERR::InvalidData;
         }
         else if (not Tag.Children.empty()) tag_cell(Tag);
         break;

      case HASH_table: if (not Tag.Children.empty()) tag_table(Tag); break;

      case HASH_tr: if (not Tag.Children.empty()) tag_row(Tag); break;

      // Others

      case HASH_data: tag_data(Tag); break;

      case HASH_edit_def: tag_editdef(Tag); break;

      case HASH_footer:
         if (not Tag.Children.empty()) m_footer_tag = &Tag.Children;
         break;

      case HASH_header:
         if (not Tag.Children.empty()) m_header_tag = &Tag.Children;
         break;

      case HASH_info: tag_head(Tag); break;

      case HASH_inject: // This instruction can only be used from within a template.
         if (m_in_template) {
            if (m_inject_tag) {
               auto old_xml = change_xml(m_inject_xml);
               parse_tags(m_inject_tag[0], Flags);
               change_xml(old_xml);
            }
         }
         else log.warning("<inject/> request detected but not used inside a template.");
         break;

      case HASH_use: tag_use(Tag); break;

      case HASH_body: tag_body(Tag); break;

      case HASH_index: tag_index(Tag); break;

      case HASH_script: tag_script(Tag); break;

      case HASH_template: tag_template(Tag); break;

      default:
         if ((Flags & IPF::NO_CONTENT) IS IPF::NIL) {
            log.warning("Tag '%s' unsupported as an instruction or template.", tagname.c_str());
         }
         else log.warning("Unrecognised tag '%s' used in a content-restricted area.", tagname.c_str());
         break;
   } // switch

   return result;
}

//********************************************************************************************************************
// See also process_page(), insert_xml()

TRF parser::parse_tags(objXML::TAGS &Tags, IPF Flags)
{
   TRF result = TRF::NIL;

   for (auto &tag : Tags) {
      // Note that Flags will carry state between multiple calls to parse_tag().  This allows if/else to work correctly.
      result = parse_tag(tag, Flags);
      if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
   }

   return result;
}

//********************************************************************************************************************

TRF parser::parse_tags_with_style(objXML::TAGS &Tags, bc_font &Style, IPF Flags)
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
      Style.uid = glByteCodeID++;

      auto save_status = m_style;
      m_style = Style;
      m_stream->insert(m_index, m_style);

      for (auto &tag : Tags) {
         result = parse_tag(tag, Flags);
         if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
      }

      m_style = save_status;
      m_stream->emplace<bc_font_end>(m_index);
   }
   else {
      for (auto &tag : Tags) {
         // Note that Flags will carry state between multiple calls to parse_tag().  This allows if/else to work correctly.
         result = parse_tag(tag, Flags);
         if ((Self->Error != ERR::Okay) or ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL)) break;
      }
   }

   return result;
}

//********************************************************************************************************************

TRF parser::parse_tags_with_embedded_style(objXML::TAGS &Tags, bc_font &Style, IPF Flags)
{
   if (Tags.empty()) return TRF::NIL;

   Style.uid = glByteCodeID++;

   auto save_style = m_style;
   m_style = Style;

   TRF result = TRF::NIL;
   for (auto &tag : Tags) {
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
   switch (strihash(Attrib.Name)) {
      case HASH_no_wrap:
         Style.options |= FSO::NO_WRAP;
         return true;

      case HASH_v_align: {
         // Vertical alignment defines the vertical position for text in cases where the line height is greater than
         // the text itself (e.g. if an image is anchored in the line).
         ALIGN align = ALIGN::NIL;
         if (iequals("top", Attrib.Value)) align = ALIGN::TOP;
         else if (iequals("center", Attrib.Value)) align = ALIGN::VERTICAL;
         else if (iequals("middle", Attrib.Value)) align = ALIGN::VERTICAL; // synonym
         else if (iequals("bottom", Attrib.Value)) align = ALIGN::BOTTOM;

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

bool parser::check_font_attrib(const XMLAttrib &Attrib, bc_font &Style)
{
   pf::Log log;

   switch (strihash(Attrib.Name)) {
      case HASH_colour:
         log.warning("Font 'colour' attrib is deprecated, use 'fill'");
         [[fallthrough]];
      case HASH_font_fill:
         [[fallthrough]];
      case HASH_fill:
         Style.fill = Attrib.Value;
         return true;

      case HASH_font_face:
         [[fallthrough]];
      case HASH_face: {
         auto j = Attrib.Value.find(':');
         if (j != std::string::npos) { // Font size follows
            auto str = Attrib.Value.c_str();
            j++;
            Style.req_size = DUNIT(str+j);
            j = Attrib.Value.find(':', j);
            if (j != std::string::npos) { // Style follows
               j++;
               Style.style = str+j;
            }
         }

         Style.face = Attrib.Value.substr(0, j);
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

void parser::trim_preformat(extDocument *Self)
{
   auto i = m_index.index - 1;
   for (; i > 0; i--) {
      if (m_stream[0][i].code IS SCODE::TEXT) {
         auto &text = m_stream->lookup<bc_text>(i);

         static const std::string ws(" \t\f\v\n\r");
         auto found = text.text.find_last_not_of(ws);
         if (found != std::string::npos) {
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

void parser::tag_advance(XTag &Tag)
{
   auto &adv = m_stream->emplace<bc_advance>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strihash(Tag.Attribs[i].Name)) {
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

void parser::tag_body(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   static const int MAX_BODY_MARGIN = 500;

   // Body tag needs to be placed before any content

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strihash(Tag.Attribs[i].Name)) {
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
            auto str = Tag.Attribs[i].Value.c_str();

            str = read_unit(str, Self->LeftMargin, rel);

            if (*str) str = read_unit(str, Self->TopMargin, rel);
            else Self->TopMargin = Self->LeftMargin;

            if (*str) str = read_unit(str, Self->RightMargin, rel);
            else Self->RightMargin = Self->TopMargin;

            if (*str) str = read_unit(str, Self->BottomMargin, rel);
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
               default: log.warning("Invalid font size unit '%s'.", Tag.Attribs[i].Value.c_str());
            }
            break;
         }

         case HASH_font_colour: // DEPRECATED, use font fill
            log.warning("The font-colour attrib is deprecated, use font-fill.");
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
            log.warning("Body attribute %s=%s not supported.", Tag.Attribs[i].Name.c_str(), Tag.Attribs[i].Value.c_str());
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

void parser::tag_call(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   objScript *script = Self->DefaultScript;

   std::string function;
   if (std::ssize(Tag.Attribs) > 1) {
      if (iequals("function", Tag.Attribs[1].Name)) {
         if (auto i = Tag.Attribs[1].Value.find('.');  i != std::string::npos) {
            auto script_name = Tag.Attribs[1].Value.substr(0, i);

            OBJECTID id;
            if (FindObject(script_name.c_str(), CLASSID::NIL, FOF::NIL, &id) IS ERR::Okay) script = (objScript *)GetObjectPtr(id);

            function.assign(Tag.Attribs[1].Value, i + 1);
         }
         else function = Tag.Attribs[1].Value;
      }
   }

   if (function.empty()) {
      log.warning("The first attribute to <call/> must be a function reference.");
      Self->Error = ERR::Syntax;
      return;
   }

   if (not script) {
      log.warning("No script in this document for a requested <call/>.");
      Self->Error = ERR::Failed;
      return;
   }

   {
      pf::Log log(__FUNCTION__);
      log.traceBranch("Calling script #%d function '%s'", script->UID, function.c_str());

      if (Tag.Attribs.size() > 2) {
         std::vector<ScriptArg> args;

         unsigned index = 0;
         for (unsigned i=2; i < Tag.Attribs.size(); i++) {
            if (Tag.Attribs[i].Name[0] IS '_') { // Global variable setting
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

void parser::tag_button(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   bc_button &widget = m_stream->emplace<bc_button>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto hash = strihash(Tag.Attribs[i].Name);
      auto &value = Tag.Attribs[i].Value;
      if (hash IS HASH_fill)          widget.fill   = value;
      else if (hash IS HASH_alt_fill) widget.alt_fill = value;
      else if (hash IS HASH_name)     widget.name   = value;
      else if (hash IS HASH_width)    widget.width  = DUNIT(value);
      else if (hash IS HASH_height)   widget.height = DUNIT(value);
      else if (hash IS HASH_padding)  widget.pad.parse(value); // Outer padding
      else if (hash IS HASH_cell_padding) widget.inner_padding.parse(value); // Inner padding
      else log.warning("<button> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
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
   if (widget.font_fill.empty()) widget.font_fill = "rgba(255,255,255,.86)";

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

void parser::tag_checkbox(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   bc_checkbox &widget = m_stream->emplace<bc_checkbox>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto hash = strihash(Tag.Attribs[i].Name);
      auto &value = Tag.Attribs[i].Value;

      if (hash IS HASH_label)      widget.label = value;
      else if (hash IS HASH_name)  widget.name  = value;
      else if (hash IS HASH_fill)  widget.fill  = value;
      else if (hash IS HASH_width) widget.width = DUNIT(value);
      else if (hash IS HASH_label_pos) {
         if (iequals("left", value)) widget.label_pos = 0;
         else if (iequals("right", value)) widget.label_pos = 1;
      }
      else if (hash IS HASH_value) {
         widget.alt_state = (value IS "1") or (value IS "true");
      }
      else log.warning("<checkbox> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
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
            fl::Fill("rgba(0,0,0,.7)")
         });

         objVectorPath::create::global({
            fl::Owner(vp->UID),
            fl::Sequence("M4.75 15.0832 15.8333 26.1665 33.2498 4 38 8.75 15.8333 35.6665 0 19.8332 4.75 15.0832Z"),
            fl::Fill("rgba(255,255,255,.5)")
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

void parser::tag_combobox(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   bc_combobox &widget = m_stream->emplace<bc_combobox>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto hash = strihash(Tag.Attribs[i].Name);
      auto &value = Tag.Attribs[i].Value;
      if (hash IS HASH_label)          widget.label = value;
      else if (hash IS HASH_value)     widget.value = value;
      else if (hash IS HASH_fill)      widget.fill = value;
      else if (hash IS HASH_font_fill) widget.font_fill = value;
      else if (hash IS HASH_name)      widget.name = value;
      else if (hash IS HASH_label_pos) {
         if (iequals("left", value)) widget.label_pos = 0;
         else if (iequals("right", value)) widget.label_pos = 1;
      }
      else if (hash IS HASH_width) widget.width = DUNIT(value);
      else log.warning("<combobox> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
   }

   // Process <option/> tags for the drop-down menu.
   // The content within each option is used as presentation in the drop-down list.
   // The 'value' attrib, if declared, will appear in the combobox for the selected item.
   // The 'id' attrib, if declared, is a UID hidden from the user.

   if (not Tag.Children.empty()) {
      for (auto &scan : Tag.Children) {
         if (iequals("style", scan.name())) {
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
         else if (iequals("option", scan.name())) {
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
            fl::Fill("rgba(0,0,0,.7)")
         });

         std::array<double, 8> round = { 0, 0, 6, 6, 6, 6, 0, 0 };
         rect->set(FID_Rounding, round);

         objVectorPath::create::global({ // Down arrow
            fl::Owner(vp->UID),
            fl::Sequence("M14.5 16.1 26.1 4.5 26.1 12.9 14.5 24.5 2.9 12.9 2.9 4.5 14.5 16.1Z"), // 80% size
            //fl::Sequence("M14.5 16.5 29 2 29 12.5 14.5 27 0 12.5 0 2 14.5 16.5Z"), // Original size; 29x29
            fl::Fill("rgba(255,255,255,.86)")
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

void parser::tag_input(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   bc_input &widget = m_stream->emplace<bc_input>(m_index);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &value = Tag.Attribs[i].Value;
      switch (strihash(Tag.Attribs[i].Name)) {
         case HASH_label:     widget.label = value; break;
         case HASH_value:     widget.value = value; break;
         case HASH_fill:      widget.fill  = value; break;
         case HASH_width:     widget.width = DUNIT(value); break;
         case HASH_font_fill: widget.font_fill = value; break;
         case HASH_name:      widget.name = value; break;
         case HASH_secret:    if (iequals(value, "true")) widget.secret = true; break;
         case HASH_label_pos:
            if (iequals("left", value)) widget.label_pos = 0;
            else if (iequals("right", value)) widget.label_pos = 1;
            break;
         default:
            log.warning("<input> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
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

void parser::tag_debug(XTag &Tag)
{
   pf::Log log("DocMsg");
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("msg", Tag.Attribs[i].Name)) log.warning("%s", Tag.Attribs[i].Value.c_str());
   }
}

//********************************************************************************************************************
// Declaring <svg> anywhere can execute an SVG statement of any kind, with the caveat that it will target the
// Page viewport (or View if 'background' is used).  The SVG feature should only be used for the creation of resources
// that can then be referred to in the document as named patterns, or via the 'use' option for symbols.
//
// This tag can only be used ONCE per document.  Potentially we could improve this by appending to the existing
// SVG object via data feeds.

void parser::tag_svg(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   if (Self->SVG) {
      Self->Error = ERR::AlreadyDefined;
      log.warning("Illegal attempt to declare <svg/> more than once.");
      return;
   }

   objVectorViewport *target = Self->Page;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("placement", Tag.Attribs[i].Name)) {
         if (iequals("foreground", Tag.Attribs[i].Value)) target = Self->Page;
         else if (iequals("background", Tag.Attribs[i].Value)) target = Self->View;
         Tag.Attribs.erase(Tag.Attribs.begin() + i);
         i--;
      }
   }

   STRING xml_svg;
   if (auto err = m_xml->serialise(Tag.ID, XMF::NIL, &xml_svg); err IS ERR::Okay) {
      if ((Self->SVG = objSVG::create::local({ fl::Statement(xml_svg), fl::Target(target) }))) {
         if (target IS Self->View) { // Put the page back in front of the background objects
            acMoveToFront(Self->Page);
         }
      }
      else Self->Error = ERR::CreateObject;

      FreeResource(xml_svg);
   }
}

//********************************************************************************************************************
// The <use> tag allows SVG <symbol> declarations to be injected into the parent viewport (e.g. table cells).
// SVG objects that are created in this way are treated as dynamically rendered background graphics.  All text will
// be laid on top with no clipping considerations.
//
// If more sophisticated inline or float embedding is required, the <image> tag is probably more applicable to the
// client.

void parser::tag_use(XTag &Tag)
{
   std::string id;
   for (int i = 1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("href", Tag.Attribs[i].Name)) {
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

void parser::tag_div(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   auto new_style = m_style;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("align", Tag.Attribs[i].Name)) {
         auto align = FSO::NIL;
         auto valid = true;
         if ((iequals(Tag.Attribs[i].Value, "center")) or
             (iequals(Tag.Attribs[i].Value, "middle"))) {
            align = FSO::ALIGN_CENTER;
         }
         else if (iequals(Tag.Attribs[i].Value, "right")) {
            align = FSO::ALIGN_RIGHT;
         }
         else if (not iequals(Tag.Attribs[i].Value, "left")) {
            log.warning("Alignment type '%s' not supported.", Tag.Attribs[i].Value.c_str());
            valid = false;
         }

         if (valid) {
            new_style.options &= ~(FSO::ALIGN_CENTER|FSO::ALIGN_RIGHT);
            new_style.options |= align;
         }
      }
      else if (check_para_attrib(Tag.Attribs[i], 0, new_style));
      else check_font_attrib(Tag.Attribs[i], new_style);
   }

   parse_tags_with_style(Tag.Children, new_style);
}

//********************************************************************************************************************
// Creates a new edit definition.  These are stored in a linked list.  Edit definitions are used by referring to them
// by name in table cells.

void parser::tag_editdef(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   doc_edit edit;
   std::string name;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strihash(Tag.Attribs[i].Name)) {
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

void parser::tag_head(XTag &Tag)
{
   // The head contains information about the document

   for (auto &scan : Tag.Children) {
      // Anything allocated here needs to be freed in unload_doc()
      if (iequals("title", scan.name())) {
         if (scan.hasContent()) {
            if (Self->Title) FreeResource(Self->Title);
            Self->Title = pf::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if (iequals("author", scan.name())) {
         if (scan.hasContent()) {
            if (Self->Author) FreeResource(Self->Author);
            Self->Author = pf::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if (iequals("copyright", scan.name())) {
         if (scan.hasContent()) {
            if (Self->Copyright) FreeResource(Self->Copyright);
            Self->Copyright = pf::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if (iequals("keywords", scan.name())) {
         if (scan.hasContent()) {
            if (Self->Keywords) FreeResource(Self->Keywords);
            Self->Keywords = pf::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
      else if (iequals("description", scan.name())) {
         if (scan.hasContent()) {
            if (Self->Description) FreeResource(Self->Description);
            Self->Description = pf::strclone(scan.Children[0].Attribs[0].Value);
         }
      }
   }
}

//********************************************************************************************************************
// Include XML from another RIPL file.

void parser::tag_include(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("src", Tag.Attribs[i].Name)) {
         if (auto xmlinc = objXML::create::local(fl::Path(Tag.Attribs[i].Value), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS))) {
            auto old_xml = change_xml(xmlinc);
            parse_tags(xmlinc->Tags);
            Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
            change_xml(old_xml);
         }
         else log.warning("Failed to include '%s'", Tag.Attribs[i].Value.c_str());
      }
      else if (iequals("volatile", Tag.Attribs[i].Name)) {
         // Instruct the cache manager that it should always check if the source requires reloading, irrespective of the
         // amount of time that has passed since the last load.
      }
   }

   log.warning("<include> directive missing required 'src' element.");
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

void parser::tag_image(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   bc_image img;
   img.def_size = DUNIT(0.9, DU::FONT_SIZE);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &value = Tag.Attribs[i].Value;

      switch (strihash(Tag.Attribs[i].Name)) {
         case HASH_float:
         case HASH_align:
            // Setting the horizontal alignment of an image will cause it to float above the text.
            // If the image is declared inside a paragraph, it will be completely de-anchored as a result.
            {
               auto align = ALIGN::NIL;
               auto valid = true;
            switch (strihash(value)) {
               case HASH_left:   align = ALIGN::LEFT; break;
               case HASH_right:  align = ALIGN::RIGHT; break;
               case HASH_center: align = ALIGN::HORIZONTAL; break;
               case HASH_middle: align = ALIGN::HORIZONTAL; break; // synonym
               default:
                  log.warning("Invalid alignment value '%s'", value.c_str());
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
            switch(strihash(value)) {
               case HASH_top:    align = ALIGN::TOP; break;
               case HASH_center: align = ALIGN::VERTICAL; break;
               case HASH_middle: align = ALIGN::VERTICAL; break; // synonym
               case HASH_bottom: align = ALIGN::BOTTOM; break;
               default:
                  log.warning("Invalid valign value '%s'", value.c_str());
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
            log.warning("<image> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
      }
   }

   if (not img.fill.empty()) {
      if (img.width.value <= 0) img.width.clear(); // Zero is equivalent to 'auto', meaning on-the-fly computation
      if (img.height.value <= 0) img.height.clear();

      if (not img.floating_x()) Self->NoWhitespace = false; // Images count as characters when inline.
      m_stream->emplace(m_index, img);
   }
   else {
      log.warning("No src defined for <image> tag.");
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

void parser::tag_index(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   uint32_t name = 0;
   bool visible = true;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("name", Tag.Attribs[i].Name)) {
         name = strihash(Tag.Attribs[i].Value);
      }
      else if (iequals("hide", Tag.Attribs[i].Name)) {
         visible = false;
      }
      else log.warning("<index> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
   }

   if ((not name) and (not Tag.Children.empty())) {
      if (Tag.Children[0].isContent()) name = strihash(Tag.Children[0].Attribs[0].Value);
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

void parser::tag_link(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   bc_link link;
   bool select = false;
   link.fill = Self->LinkFill;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strihash(Tag.Attribs[i].Name)) {
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
            else log.warning("<a|link> unsupported attribute '%s'", Tag.Attribs[i].Name.c_str());
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

void parser::tag_list(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   bc_list list;

   list.fill     = m_style.fill; // Default fill matches the current font colour
   list.item_num = list.start;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &name  = Tag.Attribs[i].Name;
      auto &value = Tag.Attribs[i].Value;
      if (iequals("fill", name)) {
         list.fill = value;
      }
      else if (iequals("indent", name)) {
         // Affects the indenting to apply to child items.
         list.block_indent = DUNIT(value, DU::PIXEL);
      }
      else if (iequals("v-spacing", name)) {
         // Affects the vertical advance from one list-item paragraph to the next.
         // Equivalent to paragraph leading, not v-spacing, which affects each line
         list.v_spacing = DUNIT(value, DU::LINE_HEIGHT);
         if (list.v_spacing.value < 0) list.v_spacing.clear();
      }
      else if (iequals("type", name)) {
         if (iequals("bullet", value)) {
            list.type = bc_list::BULLET;
         }
         else if (iequals("ordered", value)) {
            list.type = bc_list::ORDERED;
            list.item_indent.clear();
         }
         else if (iequals("custom", value)) {
            list.type = bc_list::CUSTOM;
            list.item_indent.clear();
         }
      }
      else log.msg("Unknown list attribute '%s'", name.c_str());
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

void parser::tag_paragraph(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   m_paragraph_depth++;

   bc_paragraph para(m_style);

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("align", Tag.Attribs[i].Name)) {
         auto align = FSO::NIL;
         auto valid = true;
         if ((iequals(Tag.Attribs[i].Value, "center")) or
             (iequals(Tag.Attribs[i].Value, "middle"))) {
            align = FSO::ALIGN_CENTER;
         }
         else if (iequals(Tag.Attribs[i].Value, "right")) {
            align = FSO::ALIGN_RIGHT;
         }
         else if (not iequals(Tag.Attribs[i].Value, "left")) {
            log.warning("Alignment type '%s' not supported.", Tag.Attribs[i].Value.c_str());
            valid = false;
         }

         if (valid) {
            para.font.options &= ~(FSO::ALIGN_CENTER|FSO::ALIGN_RIGHT);
            para.font.options |= align;
         }
      }
      else if (iequals("leading", Tag.Attribs[i].Name)) {
         // The leading is a line height multiplier that applies to the first line in the paragraph only.
         // It is typically used for things like headers.

         para.leading = DUNIT(Tag.Attribs[i].Value, DU::LINE_HEIGHT, DBL_MIN);
      }
      else if (check_para_attrib(Tag.Attribs[i], &para, para.font));
      else check_font_attrib(Tag.Attribs[i], para.font);
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

void parser::tag_template(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   if (m_in_template) return;

   // Validate the template (must have a name)

   int n;
   for (n=1; n < std::ssize(Tag.Attribs); n++) {
      if ((iequals("name", Tag.Attribs[n].Name)) and (not Tag.Attribs[n].Value.empty())) break;
   }

   if (n >= std::ssize(Tag.Attribs)) {
      log.warning("A <template> is missing a name attribute.");
      return;
   }

   STRING strxml;
   if (m_xml->serialise(Tag.ID, XMF::NIL, &strxml) IS ERR::Okay) {
      // Remove any existing tag that uses the same name.
      if (Self->TemplateIndex.contains(strihash(Tag.Attribs[n].Value))) {
         Self->Templates->removeTag(Tag.ID, 1);
      }

      Self->Templates->insertXML(Self->Templates->Tags[0].ID, XMI::END, strxml, 0);
      FreeResource(strxml);

      Self->RefreshTemplates = true; // Force a refresh of the TemplateIndex because the pointers will be changed
   }
   else log.warning("Failed to convert template %d to an XML string.", Tag.ID);
}

//********************************************************************************************************************

void parser::tag_font(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   auto new_style = m_style;
   bool preformat = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("preformat", Tag.Attribs[i].Name)) {
         new_style.options |= FSO::PREFORMAT;
         preformat = true;
         m_strip_feeds = true;
      }
      else check_font_attrib(Tag.Attribs[i], new_style);
   }

   parse_tags_with_style(Tag.Children, new_style);

   if (preformat) trim_preformat(Self);
}

//********************************************************************************************************************
// The use of pre will turn off the automated whitespace management so that all whitespace is parsed as-is.  It does
// not switch to a monospaced font.

void parser::tag_pre(objXML::TAGS &Children)
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

   trim_preformat(Self);
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

void parser::tag_script(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   objScript *script;
   ERR error;

   std::string type = "tiri";
   std::string src, cachefile, name;
   bool defaultscript = false;
   bool persistent = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto tagname = Tag.Attribs[i].Name.c_str();
      if (*tagname IS '$') tagname++;
      if (*tagname IS '@') continue; // Variables are set later

      if (iequals("type", tagname)) {
         type = Tag.Attribs[i].Value;
      }
      else if (iequals("persistent", tagname)) {
         // A script that is marked as persistent will survive refreshes
         persistent = true;
      }
      else if (iequals("src", tagname)) {
         if (safe_file_path(Self, Tag.Attribs[i].Value)) {
            src = Tag.Attribs[i].Value;
         }
         else {
            log.warning("Security violation - cannot set script src to: %s", Tag.Attribs[i].Value.c_str());
            return;
         }
      }
      else if (iequals("cache-file", tagname)) {
         // Currently the security risk of specifying a cache file is that you could overwrite files on the user's PC,
         // so for the time being this requires unrestricted mode.

         if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) {
            cachefile = Tag.Attribs[i].Value;
         }
         else {
            log.warning("Security violation - cannot set script cachefile to: %s", Tag.Attribs[i].Value.c_str());
            return;
         }
      }
      else if (iequals("name", tagname)) {
         name = Tag.Attribs[i].Value;
      }
      else if (iequals("default", tagname)) {
         defaultscript = true;
      }
      else if (iequals("external", tagname)) {
         // Reference an external script as the default for function calls
         if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) {
            OBJECTID id;
            if (FindObject(Tag.Attribs[i].Value.c_str(), CLASSID::NIL, FOF::NIL, &id) IS ERR::Okay) {
               Self->DefaultScript = (objScript *)GetObjectPtr(id);
               return;
            }
            else {
               log.warning("Failed to find external script '%s'", Tag.Attribs[i].Value.c_str());
               return;
            }
         }
         else {
            log.warning("Security violation - cannot reference external script '%s'", Tag.Attribs[i].Value.c_str());
            return;
         }
      }
   }

   if ((persistent) and (name.empty())) name = "mainscript";

   if (src.empty()) {
      if ((Tag.Children.empty()) or (not Tag.Children[0].Attribs[0].Name.empty()) or (Tag.Children[0].Attribs[0].Value.empty())) {
         // Ignore if script holds no content
         log.warning("<script/> tag does not contain content.");
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

   if (iequals("tiri", type)) {
      error = NewLocalObject(CLASSID::TIRI, &script);
   }
   else {
      error = ERR::NoSupport;
      log.warning("Unsupported script type '%s'", type.c_str());
   }

   if (error IS ERR::Okay) {
      if (not name.empty()) SetName(script, name.c_str());

      if (not src.empty()) script->setPath(src);
      else {
         std::string content = xml::GetContent(Tag);
         if (not content.empty()) script->setStatement(content);
      }

      if (not cachefile.empty()) script->setCacheFile(cachefile);

      // Object references are to be limited in scope to the Document object

      //script->setObjectScope(Self->Head.UID);

      // Pass custom arguments in the script tag

      for (unsigned i=1; i < Tag.Attribs.size(); i++) {
         auto tagname = Tag.Attribs[i].Name.c_str();
         if (*tagname IS '$') tagname++;
         if (*tagname IS '@') acSetKey(script, tagname+1, Tag.Attribs[i].Value.c_str());
      }

      if (InitObject(script) IS ERR::Okay) {
         // Pass document arguments to the script

         KEYVALUE *vs;
         if ((script->get(FID_Variables, vs) IS ERR::Okay) and (vs) and (vs->size() > 0)) {
            Self->Vars   = *vs;
            Self->Params = *vs;
         }

         if (acActivate(script) IS ERR::Okay) { // Persistent scripts survive refreshes.
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
            }
         }
         else FreeResource(script);
      }
      else FreeResource(script);
   }
}

//********************************************************************************************************************
// Supports FSO::UNDERLINE and named styles

void parser::tag_font_style(objXML::TAGS &Children, FSO StyleFlag, std::string_view StyleName)
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

void parser::tag_li(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   if (m_list_stack.empty()) {
      log.warning("<li> not used inside a <list> tag.");
      return;
   }

   auto &list = m_list_stack.top();

   bc_paragraph para(m_style);
   para.list_item   = true;
   para.item_indent = list->item_indent;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto tagname = Tag.Attribs[i].Name.c_str();
      if (*tagname IS '$') tagname++;

      if (iequals("value", tagname)) {
         para.value = Tag.Attribs[i].Value;
      }
      else if (iequals("aggregate", tagname)) {
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
void parser::tag_repeat(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   std::string index_name;
   int loop_start = 0, loop_end = 0, count = 0, step = 0;
   bool have_end = false;
   bool have_count = false;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("start", Tag.Attribs[i].Name)) {
         loop_start = std::stoi(Tag.Attribs[i].Value);
         if (loop_start < 0) loop_start = 0;
      }
      else if (iequals("count", Tag.Attribs[i].Name)) {
         count = std::stoi(Tag.Attribs[i].Value);
         if (count < 0) {
            log.warning("Invalid count value of %d", count);
            return;
         }
         have_count = true;
      }
      else if (iequals("end", Tag.Attribs[i].Name)) {
         loop_end = std::stoi(Tag.Attribs[i].Value);
         have_end = true;
      }
      else if (iequals("step", Tag.Attribs[i].Name)) {
         step = std::stoi(Tag.Attribs[i].Value);
      }
      else if (iequals("index", Tag.Attribs[i].Name)) {
         // If an index name is specified, the programmer will need to refer to it as [@indexname] and [%index] will
         // remain unchanged from any parent repeat loop.

         index_name = Tag.Attribs[i].Value;
      }
      else if (iequals("select", Tag.Attribs[i].Name)) {
         log.warning("<repeat select=\"...\"> is not supported.  Use <for-each select=\"...\"> for sequence iteration.");
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

void parser::tag_table(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   auto &table = m_stream->emplace<bc_table>(m_index);
   table.min_width  = DUNIT(1, DU::PIXEL);
   table.min_height = DUNIT(1, DU::PIXEL);

   std::string columns;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto &value = Tag.Attribs[i].Value;
      switch (strihash(Tag.Attribs[i].Name)) {
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
            switch(strihash(value)) {
               case HASH_left:   table.align = ALIGN::LEFT; break;
               case HASH_right:  table.align = ALIGN::RIGHT; break;
               case HASH_center: table.align = ALIGN::HORIZONTAL; break;
               case HASH_middle: table.align = ALIGN::HORIZONTAL; break; // synonym
               default: log.warning("Invalid alignment value '%s'", value.c_str()); break;
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
      std::vector<std::string> list;
      for (unsigned i=0; i < columns.size(); ) {
         auto end = columns.find(',', i);
         if (end IS std::string::npos) end = columns.size();
         auto val = columns.substr(i, end-i);
         trim(val);
         list.push_back(val);
         i = end + 1;
      }

      size_t i;
      for (i=0; (i < table.columns.size()) and (i < list.size()); i++) {
         table.columns[i].preset_width = strtod(list[i].c_str(), nullptr);
         if (list[i].find_first_of('%') != std::string::npos) {
            table.columns[i].preset_width *= 0.01;
            table.columns[i].preset_width_rel = true;
            if ((table.columns[i].preset_width < 0.0000001) or (table.columns[i].preset_width > 1.0)) {
               log.warning("A <table> column value is invalid.");
               Self->Error = ERR::InvalidDimension;
            }
         }
      }

      if (i < table.columns.size()) log.warning("Warning - columns attribute '%s' did not define %d columns.", columns.c_str(), int(table.columns.size()));
   }

   bc_table_end end;
   m_stream->emplace(m_index, end);

   Self->NoWhitespace = true; // Setting this to true will prevent the possibility of blank spaces immediately following the table.
}

//********************************************************************************************************************

void parser::tag_row(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   if (m_table_stack.empty()) {
      log.warning("<row> not defined inside <table> section.");
      Self->Error = ERR::InvalidData;
      return;
   }

   bc_row escrow;

   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      if (iequals("height", Tag.Attribs[i].Name)) {
         escrow.min_height = std::clamp(strtod(Tag.Attribs[i].Value.c_str(), nullptr), 0.0, 4000.0);
      }
      else if (iequals("fill", Tag.Attribs[i].Name))   escrow.fill   = Tag.Attribs[i].Value;
      else if (iequals("stroke", Tag.Attribs[i].Name)) escrow.stroke = Tag.Attribs[i].Value;
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

void parser::tag_cell(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   auto new_style = m_style;
   static uint8_t edit_recurse = 0;

   if (m_table_stack.empty()) {
      log.warning("<cell> not defined inside <table> section.");
      Self->Error = ERR::InvalidData;
      return;
   }

   bc_cell cell(glUID++, m_table_stack.top().row_col);
   bool select = false;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strihash(Tag.Attribs[i].Name)) {
         case HASH_border: {
            std::vector<std::string> list;
            pf::split(Tag.Attribs[i].Value, std::back_inserter(list));

            for (auto &v : list) {
               if (iequals("all", v))         cell.border = CB::ALL;
               else if (iequals("top", v))    cell.border |= CB::TOP;
               else if (iequals("left", v))   cell.border |= CB::LEFT;
               else if (iequals("bottom", v)) cell.border |= CB::BOTTOM;
               else if (iequals("right", v))  cell.border |= CB::RIGHT;
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
               log.warning("Edit cells cannot be embedded recursively.");
               Self->Error = ERR::Recursion;
               return;
            }
            cell.edit_def = Tag.Attribs[i].Value;

            if (not Self->EditDefs.contains(Tag.Attribs[i].Value)) {
               log.warning("Edit definition '%s' does not exist.", Tag.Attribs[i].Value.c_str());
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

void parser::tag_page(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   if (auto name = Tag.attrib("name")) {
      auto str = name->c_str();
      while (*str) {
         if (((*str >= 'A') and (*str <= 'Z')) or
             ((*str >= 'a') and (*str <= 'z')) or
             ((*str >= '0') and (*str <= '9'))) {
            // Character is valid
         }
         else {
            log.warning("Page has an invalid name of '%s'.  Character support is limited to [A-Z,a-z,0-9].", name->c_str());
            break;
         }
         str++;
      }
   }
}

//********************************************************************************************************************
// Usage: <trigger event="resize" function="script.function"/>

void parser::tag_trigger(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   DRT trigger_code;
   objScript *script;
   int64_t function_id;

   std::string event, function_name;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      switch (strihash(Tag.Attribs[i].Name)) {
         case HASH_event: event = Tag.Attribs[i].Value; break;
         case HASH_function: function_name = Tag.Attribs[i].Value; break;
      }
   }

   if ((not event.empty()) and (not function_name.empty())) {
      // These are described in the documentation for the AddListener method

      switch(strihash(event)) {
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
            log.warning("Trigger event '%s' for function '%s' is not recognised.", event.c_str(), function_name.c_str());
            return;
      }

      // Get the script

      std::string args;
      if (extract_script(Self, function_name.c_str(), &script, function_name, args) IS ERR::Okay) {
         if (script->getProcedureID(function_name.c_str(), &function_id) IS ERR::Okay) {
            Self->Triggers[int(trigger_code)].emplace_back(FUNCTION(script, function_id));
         }
         else log.warning("Unable to resolve '%s' in script #%d to a function ID (the procedure may not exist)", function_name.c_str(), script->UID);
      }
      else log.warning("The script for '%s' is not available - check if it is declared prior to the trigger tag.", function_name.c_str());
   }
}
