/*

The parsing code converts XML data to a serial byte stream, after which the XML data can be discarded.  A DOM
of the original XML content is *not* maintained.  After parsing, the stream will be ready for presentation via the
layout code elsewhere in this code base.

The stream consists of byte codes represented by the entity class.  Each type of code is represented by a C++
class prefixed with 'bc'.  Each code type has a specific purpose such as defining a new font style, paragraph,
hyperlink etc.  When a type is instantiated it will be assigned a UID and stored in the Codes hashmap.

*/

#include <cfloat>

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
         if ((active) and (owner) and (!owner->m_loop_stack.empty())) {
            owner->m_loop_stack.pop_back();
         }
      }

      inline loop_frame * frame() {
         if ((!active) or (!owner) or (owner->m_loop_stack.empty())) return nullptr;
         return &owner->m_loop_stack.back();
      }

      inline const loop_frame * frame() const {
         if ((!active) or (!owner) or (owner->m_loop_stack.empty())) return nullptr;
         return &owner->m_loop_stack.back();
      }
   };

   extDocument *Self;
   objXML *m_xml;

   RSTREAM *m_stream;                 // Generated stream content
   std::unique_ptr<RSTREAM> m_stream_alloc;
   objXML *m_inject_xml = nullptr;
   objXML::TAGS *m_inject_tag = nullptr, *m_header_tag = nullptr, *m_footer_tag = nullptr, *m_body_tag = nullptr;
   objTime *m_time = nullptr;
   pf::vector<loop_frame> m_loop_stack;
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

   parser(extDocument *pSelf, objXML *pXML, RSTREAM *pStream = nullptr) : Self(pSelf), m_xml(pXML) {
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

   inline ERR  calc(const std::string &, double *, std::string &);
   inline TRF  parse_tag(XTag &, IPF &);
   inline TRF  parse_tags(objXML::TAGS &, IPF = IPF::NIL);
   inline TRF  parse_tags_with_style(objXML::TAGS &, bc_font &, IPF = IPF::NIL);
   inline TRF  parse_tags_with_embedded_style(objXML::TAGS &, bc_font &, IPF = IPF::NIL);
   inline void process_page(objXML *pXML);
   inline void tag_xml_content(XTag &, PXF);
   inline void translate_attrib_args(pf::vector<XMLAttrib> &);
   inline void translate_args(const std::string &, std::string &);
   inline void translate_calc(std::string &, size_t);
   inline void translate_param(std::string &, size_t);
   inline void translate_reserved(std::string &, size_t, bool &);
   inline ERR  tag_xml_content_eval(std::string &);
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
   inline void tag_debug(XTag &);
   inline void tag_div(XTag &);
   inline void tag_editdef(XTag &);
   inline void tag_font(XTag &);
   inline void tag_font_style(objXML::TAGS &, FSO, std::string_view);
   inline void tag_head(XTag &);
   inline void tag_image(XTag &);
   inline void tag_include(XTag &);
   inline void tag_index(XTag &);
   inline void tag_input(XTag &);
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
   inline void tag_object(XTag &);
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

   inline int current_loop_index() const {
      if (auto frame = active_loop()) return frame->index;
      else return 0;
   }

   inline bool resolve_loop_alias(std::string_view Name, std::string &Value) const {
      for (int i = std::ssize(m_loop_stack) - 1; i >= 0; i--) {
         auto &frame = m_loop_stack[i];
         if ((!frame.alias_name.empty()) and (iequals(frame.alias_name, Name))) {
            Value = std::to_string(frame.index);
            return true;
         }
      }
      return false;
   }

   ~parser() {
      if (m_time) FreeResource(m_time);
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
            fl::Stroke("rgb(255,255,255)"), fl::StrokeWidth(1.0),
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

   if (!pXML) { Self->Error = ERR::NoData; return; }
   m_xml = pXML;

   // Look for the first page that matches the requested page name (if a name is specified).  Pages can be located anywhere
   // within the XML source - they don't have to be at the root.

   XTag *page = nullptr;
   for (auto &scan : m_xml->Tags) {
      if (!iequals("page", scan.Attribs[0].Name)) continue;

      if (!page) page = &scan;

      if (Self->PageName.empty()) break;
      else if (auto name = scan.attrib("name")) {
         if (iequals(Self->PageName, *name)) page = &scan;
      }
   }

   Self->Error = ERR::Okay;
   if ((page) and (!page->Children.empty())) {
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

      if ((m_header_tag) and (!no_header)) {
         insert_xml(Self, m_stream, m_xml, m_header_tag[0], m_stream->size(), STYLE::RESET_STYLE);
      }

      if (m_body_tag) {
         pf::Log log(__FUNCTION__);
         log.traceBranch("Processing this page through the body tag.");

         auto tags = m_inject_tag;
         m_inject_tag = &page->Children;
         m_in_template++;

         insert_xml(Self, m_stream, m_inject_xml, m_body_tag[0], m_stream->size(), STYLE::RESET_STYLE);

         m_in_template--;
         m_inject_tag = tags;
      }
      else {
         pf::Log log(__FUNCTION__);
         auto page_name = page->attrib("name");
         log.traceBranch("Processing page '%s'.", page_name ? page_name->c_str() : "");
         insert_xml(Self, m_stream, m_xml, page->Children, m_stream->size(), STYLE::RESET_STYLE);
      }

      if ((m_footer_tag) and (!no_footer)) {
         insert_xml(Self, m_stream, m_xml, m_footer_tag[0], m_stream->size(), STYLE::RESET_STYLE);
      }

      // If an error occurred then we have to kill the document as the stream may contain unsynchronised
      // byte codes (e.g. an unterminated SCODE::TABLE sequence).

      if (Self->Error != ERR::Okay) unload_doc(Self);
   }
   else {
      if (!Self->PageName.empty()) {
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
   if ((!Self->Error) and (Self->MouseInPage)) {
      DOUBLE x, y;
      if (!gfxGetRelativeCursorPos(Self->Page->UID, &x, &y)) {
         check_mouse_pos(Self, x, y);
      }
   }
*/
   if (!Self->PageProcessed) {
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

//********************************************************************************************************************
// Determine whether an attribute on a given tag should be treated as an XQuery expression (evaluated as a whole) or
// as an attribute value template (AVT) with embedded `{...}` expression fragments.  Attributes so classified are
// bypassed by translate_attrib_args() so that their source text is preserved for late XQuery evaluation by the tag
// handler.
//
// Stage 1 scope:
//   if@test, elseif@test, while@test       -> XQuery expression attributes
//   print@select, parse@select             -> XQuery expression attributes
//   print@value                            -> AVT

static bool is_xq_expression_attrib(uint32_t TagHash, std::string_view AttribName)
{
   auto name = AttribName;
   if ((!name.empty()) and (name.front() IS '$')) name.remove_prefix(1);

   if (iequals("test", name)) {
      return (TagHash IS HASH_if) or (TagHash IS HASH_elseif) or (TagHash IS HASH_while);
   }
   else if (iequals("select", name)) {
      return (TagHash IS HASH_print) or (TagHash IS HASH_parse);
   }
   return false;
}

static bool is_xq_avt_attrib(uint32_t TagHash, std::string_view AttribName, std::string_view AttribValue)
{
   auto name = AttribName;
   if ((!name.empty()) and (name.front() IS '$')) name.remove_prefix(1);

   if ((TagHash IS HASH_print) and (iequals("value", name))) {
      // Only bypass legacy [%...] substitution when the value contains AVT markers.
      return (AttribValue.find('{') != std::string_view::npos) or
             (AttribValue.find('}') != std::string_view::npos);
   }
   return false;
}

//********************************************************************************************************************
// Translate all arguments found in a list of XML attributes.
//
// XQuery-aware attributes (test, select, and AVT-capable value attributes on selected tags) are bypassed here so that
// their source text is preserved untouched for evaluation by the tag handler.

void parser::translate_attrib_args(pf::vector<XMLAttrib> &Attribs)
{
   if (Attribs[0].isContent()) return;

   std::string_view tag_name(Attribs[0].Name);
   if ((!tag_name.empty()) and (tag_name.front() IS '$')) tag_name.remove_prefix(1);
   auto tag_hash = strihash(tag_name);

   for (int attrib=1; attrib < std::ssize(Attribs); attrib++) {
      if (Attribs[attrib].Name.starts_with('$')) continue;

      if (is_xq_expression_attrib(tag_hash, Attribs[attrib].Name)) continue;
      if (is_xq_avt_attrib(tag_hash, Attribs[attrib].Name, Attribs[attrib].Value)) continue;

      std::string output;
      translate_args(Attribs[attrib].Value, output);
      Attribs[attrib].Value = output;
   }
}

//********************************************************************************************************************
// XQuery evaluation helpers for Stage 1.
//
// These helpers are intentionally thin wrappers around the XQuery object class so that parser-visible behaviour mirrors
// a standard xml:evaluate() call.  The XML context is optional; when absent, the evaluator still supports literal and
// function-based expressions.  Errors from compilation or evaluation propagate to the caller via the returned ERR and
// the document Self->Error field.

enum class XQEval { STRING, BOOLEAN };

static inline void xq_map_append_value(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key,
   const XPathValue &Value)
{
   XPathMapEntry entry;
   entry.key.assign(Key);
   entry.value.items.push_back(Value);
   Storage->entries.push_back(std::move(entry));
}

static inline void xq_map_append_string(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key,
   std::string_view Value)
{
   XPathValue result(XPVT::String);
   result.StringValue.assign(Value);
   xq_map_append_value(Storage, Key, result);
}

static inline void xq_map_append_number(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key, double Value)
{
   XPathValue result(XPVT::Number);
   result.NumberValue = Value;
   xq_map_append_value(Storage, Key, result);
}

static XPathValue xq_keyvalue_to_map(const KEYVALUE &Source)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   for (const auto &entry : Source) {
      xq_map_append_string(result.map_storage, entry.first, entry.second);
   }

   return result;
}

static XPathValue xq_template_args_to_map(extDocument *Self)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   if (Self->TemplateArgs.empty()) return result;

   auto args = Self->TemplateArgs.back();
   if (!args) return result;

   for (int i=1; i < std::ssize(args->Attribs); i++) {
      auto name = std::string_view(args->Attribs[i].Name);
      if ((!name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
      if (name.empty()) continue;
      xq_map_append_string(result.map_storage, name, args->Attribs[i].Value);
   }

   return result;
}

static XPathValue xq_meta_to_map(extDocument *Self)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   if (Self->Title) xq_map_append_string(result.map_storage, "title", Self->Title);
   if (Self->Author) xq_map_append_string(result.map_storage, "author", Self->Author);
   if (Self->Description) xq_map_append_string(result.map_storage, "description", Self->Description);
   if (Self->Copyright) xq_map_append_string(result.map_storage, "copyright", Self->Copyright);
   if (Self->Keywords) xq_map_append_string(result.map_storage, "keywords", Self->Keywords);
   if (!Self->Path.empty()) xq_map_append_string(result.map_storage, "path", Self->Path);

   if (Self->PageTag) {
      if (auto current_page = Self->PageTag->attrib("name")) {
         xq_map_append_string(result.map_storage, "current-page", *current_page);
      }

      if (auto next_page = Self->PageTag->attrib("next-page")) {
         xq_map_append_string(result.map_storage, "next-page", *next_page);
      }

      if (auto prev_page = Self->PageTag->attrib("prev-page")) {
         xq_map_append_string(result.map_storage, "prev-page", *prev_page);
      }
   }

   return result;
}

static XPathValue xq_layout_to_map(extDocument *Self)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   xq_map_append_number(result.map_storage, "view-width", Self->VPWidth);
   xq_map_append_number(result.map_storage, "view-height", Self->VPHeight);
   xq_map_append_number(result.map_storage, "page-height", Self->PageHeight);

   if (Self->CalcWidth > 0) xq_map_append_number(result.map_storage, "page-width", Self->CalcWidth);
   else if (Self->VPWidth > 0) xq_map_append_number(result.map_storage, "page-width", Self->VPWidth);

   return result;
}

static XPathValue xq_loop_to_map(parser *Parser)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   auto frame = Parser->active_loop();
   if (!frame) return result;

   xq_map_append_number(result.map_storage, "index", frame->index);
   xq_map_append_number(result.map_storage, "iteration", frame->iteration);
   xq_map_append_number(result.map_storage, "start", frame->start);
   xq_map_append_number(result.map_storage, "step", frame->step);
   if (frame->count_known) xq_map_append_number(result.map_storage, "count", frame->count);
   if (frame->end_known) xq_map_append_number(result.map_storage, "end", frame->end);
   return result;
}

static bool loop_index_in_range(int Index, int End, int Step)
{
   if (Step > 0) return Index < End;
   else if (Step < 0) return Index > End;
   else return false;
}

static int compute_numeric_loop_count(int Start, int End, int Step)
{
   if (Step > 0) {
      if (Start >= End) return 0;
      return ((End - Start - 1) / Step) + 1;
   }
   else if (Step < 0) {
      auto distance = Start - End;
      if (distance <= 0) return 0;
      return ((distance - 1) / (-Step)) + 1;
   }
   else return 0;
}

static ERR xq_resolve_runtime_scope(objXQuery *Query, std::string_view Name, XPathValue *Result, APTR Meta)
{
   auto Self = (extDocument *)CurrentContext();

   if (Name IS "params") {
      *Result = xq_keyvalue_to_map(Self->Params);
      return ERR::Okay;
   }
   else if (Name IS "args") {
      *Result = xq_template_args_to_map(Self);
      return ERR::Okay;
   }
   else if (Name IS "vars") {
      *Result = xq_keyvalue_to_map(Self->Vars);
      return ERR::Okay;
   }
   else if (Name IS "meta") {
      *Result = xq_meta_to_map(Self);
      return ERR::Okay;
   }
   else if (Name IS "layout") {
      *Result = xq_layout_to_map(Self);
      return ERR::Okay;
   }
   else if (Name IS "loop") {
      *Result = xq_loop_to_map((parser *)Meta);
      return ERR::Okay;
   }
   else return ERR::Search;
}

static ERR xq_eval_helper(parser *Parser, objXML *XMLContext, XTag *ContextTag, const std::string &Expression,
   XQEval Mode, std::string &OutString, bool &OutBoolean)
{
   pf::Log log(__FUNCTION__);
   auto Self = Parser->Self;

   OutString.clear();
   OutBoolean = false;

   if (Expression.empty()) return ERR::Okay;

   std::string eval_expression;
   if (Mode IS XQEval::BOOLEAN) eval_expression = "boolean(" + Expression + ")";
   else eval_expression = Expression;

   if (not Self->Query) {
      if (NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&Self->Query) IS ERR::Okay) {
         if (Self->Query->init() != ERR::Okay) {
            Self->Error = ERR::Init;
            FreeResource(Self->Query);
            Self->Query = nullptr;
            return log.warning(ERR::Init);
         }
      }
      else {
         Self->Error = ERR::NewObject;
         return log.warning(ERR::NewObject);
      }
   }

   Self->Query->setResolveVariable(C_FUNCTION(xq_resolve_runtime_scope, Parser));

   if (Self->Query->setStatement(eval_expression) IS ERR::Okay) {
      if (auto err = Self->Query->evaluate(XMLContext, ContextTag ? ContextTag->ID : 0, XEF::NIL); err != ERR::Okay) {
         CSTRING msg;
         if (Self->Query->get(FID_ErrorMsg, msg) IS ERR::Okay) {
            log.warning("XQuery error evaluating \"%s\": %s", Expression.c_str(), msg ? msg : "(none)");
         }
         else log.warning("XQuery error evaluating \"%s\".", Expression.c_str());
         Self->Error = err;
         return err;
      }

      CSTRING result;
      if (Self->Query->get(FID_ResultString, result) IS ERR::Okay) {
         if (Mode IS XQEval::STRING) {
            if (result) OutString.assign(result);
         }
         else if (result and iequals("true", result)) OutBoolean = true;
      }

      return ERR::Okay;
   }
   else {
      Self->Error = ERR::SetField;
      return ERR::SetField;
   }
}

// Expand a string containing attribute value template fragments `{...}`, evaluating each embedded XQuery expression
// and concatenating the result with literal parts.  Follows the same escaping rules as the XQuery tokeniser so that
// `{{` and `}}` are preserved as literal `{` and `}`.

static ERR xq_expand_avt(parser *Parser, objXML *XMLContext, XTag *ContextTag, const std::string &Input,
   std::string &Output)
{
   pf::Log log(__FUNCTION__);
   Output.clear();

   size_t pos = 0;
   const size_t len = Input.size();

   while (pos < len) {
      char ch = Input[pos];

      if (ch IS '{') {
         if ((pos + 1 < len) and (Input[pos + 1] IS '{')) {
            Output += '{';
            pos += 2;
            continue;
         }

         // Scan expression body, respecting nested braces and quoted strings.
         size_t expr_start = pos + 1;
         size_t scan = expr_start;
         int depth = 1;
         while ((scan < len) and (depth > 0)) {
            char sc = Input[scan];
            if ((sc IS '\'') or (sc IS '"')) {
               char quote = sc;
               scan++;
               while (scan < len) {
                  char inner = Input[scan++];
                  if (inner IS '\\' and scan < len) { scan++; continue; }
                  if (inner IS quote) break;
               }
               continue;
            }
            else if (sc IS '{') { depth++; scan++; }
            else if (sc IS '}') { depth--; if (depth IS 0) break; else scan++; }
            else scan++;
         }

         if (depth != 0) {
            log.warning("Unterminated attribute value template: %s", Input.c_str());
            return ERR::Syntax;
         }

         std::string expr = Input.substr(expr_start, scan - expr_start);
         pos = scan + 1;

         std::string evaluated;
         bool unused;
         auto err = xq_eval_helper(Parser, XMLContext, ContextTag, expr, XQEval::STRING, evaluated, unused);
         if (err != ERR::Okay) return err;
         Output += evaluated;
      }
      else if (ch IS '}' and (pos + 1 < len) and (Input[pos + 1] IS '}')) {
         Output += '}';
         pos += 2;
      }
      else {
         Output += ch;
         pos++;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// This function converts arguments such as [@arg] in a string.
//
// Calculations can also be performed, e.g. [=5+7]
//
// The escape code for brackets are &lsqr; and &rsqr; (not in the XML escape code standard and thus are unconverted up
// until this function is reached).
//
// If an attribute name is prefixed with '$' then no translation of the attribute value is attempted.
//
// URI parameters are referenced with [@param:default_val]
//
// If a major error occurs during processing, the function will abort, returning the error and also setting the Error
// field to the resulting error code.  The most common reason for an abort is a buffer overflow or memory allocation
// problems, so a complete abort of document processing is advisable.
//
// RESERVED WORDS
//    %index        Current loop index, if within a repeat loop.
//    %id           A unique ID that is regenerated on each document refresh.
//    %self         ID of the document object.
//    %platform     Windows, Linux or Native.
//    %random       Random string of 9 digits.
//    %current-page name of the current page.
//    %next-page    name of the next page.
//    %prev-page    name of the previous page.
//    %path         Current working path.
//    %author       Document author.
//    %description  Document description.
//    %copyright    Document copyright.
//    %keywords     Document keywords.
//    %title        Document title.
//    %font         Face, point size and style of the current font.
//    %font-face    Face of the current font.
//    %font-fill    Paint-fill instruction for the current font.
//    %font-size    Pixel size of the current font, scaled to 72 DPI.
//    %line-no      The current 'line' (technically segmented line) in the document.
//    %content      Inject content (same as <inject/> but usable inside tag attributes)
//    %tm-day       The current day (0 - 31)
//    %tm-month     The current month (1 - 12)
//    %tm-year      The current year (2008+)
//    %tm-hour      The current hour (0 - 23)
//    %tm-minute    The current minute (0 - 59)
//    %tm-second    The current second (0 - 59)
//    %view-height  Height of the document's available viewing area
//    %view-width   Width of the the document's available viewing area.

void parser::translate_args(const std::string &Input, std::string &Output)
{
   pf::Log log(__FUNCTION__);

   bool time_queried = false;
   Output = Input;

   // Do nothing if there are no special references being used

   {
      unsigned i;
      for (i=0; i < Input.size(); i++) {
         if (Input[i] IS '[') break;
         if ((Input[i] IS '&') and
             ((startswith("&lsqr;", std::string_view(Input.begin()+i, Input.end()))) or
              (startswith("&rsqr;", std::string_view(Input.begin()+i, Input.end()))))) break;
      }
      if (i >= Input.size()) return;
   }

   for (auto pos = std::ssize(Output)-1; pos >= 0; pos--) {
      if (Output[pos] IS '&') {
         if (startswith("&lsqr;", std::string_view(Output.begin()+pos, Output.end()))) Output.replace(pos, 6, "[");
         else if (startswith("&rsqr;", std::string_view(Output.begin()+pos, Output.end()))) Output.replace(pos, 6, "]");
      }
      else if (Output[pos] IS '[') {
         if (Output[pos+1] IS '=') { // Perform a calcuation within [= ... ]
            translate_calc(Output, pos);
         }
         else if (Output[pos+1] IS '@') {
            translate_param(Output, pos);
         }
         else if (Output[pos+1] IS '%') {
            translate_reserved(Output, pos, time_queried);
         }
         else { // Object translation, can be [object] or [object.field]
            // Make sure that there is a closing bracket

            int balance = 1;
            unsigned end;
            for (end=pos+1; (end < Output.size()) and (balance > 0); end++) {
               if (Output[end] IS '[') balance++;
               else if (Output[end] IS ']') balance--;
            }

            if (Output[end] != ']') {
               log.warning("Object reference missing square end bracket.");
               break;
            }
            end++;

            auto name = Output.substr(pos+1, Output.find_first_of(".]")-pos);

            // Get the object ID

            OBJECTID objectid = 0;
            if (!name.empty()) {
               if (FindObject(name.c_str(), CLASSID::NIL, FOF::SMART_NAMES, &objectid) IS ERR::Okay) {
                  if ((Self->Flags & DCF::UNRESTRICTED) IS DCF::NIL) {
                     // Only consider objects that are children of the document
                     bool valid = false;
                     for (auto parent_id = GetOwnerID(objectid); parent_id; parent_id = GetOwnerID(parent_id)) {
                        if (parent_id IS Self->UID) {
                           valid = true;
                           break;
                        }
                     }
                     if (!valid) objectid = 0;
                  }
               }

               if (objectid) {
                  if (valid_objectid(Self, objectid)) {
                     if (auto dot = Output.find('.'); dot != std::string::npos) { // Object makes a field reference
                        pf::ScopedObjectLock object(objectid, 2000);
                        if (object.granted()) {
                           OBJECTPTR target;
                           auto fieldname = Output.substr(dot+1, end-(dot+1));
                           if (auto classfield = FindField(object.obj, strihash(fieldname), &target)) {
                              std::string str;
                              if (target->get(classfield->FieldID, str) IS ERR::Okay) Output.replace(pos, end-pos, str);
                              else Output.replace(pos, end-pos, "");
                           }
                           else if (CheckAction(object.obj, AC::GetKey) IS ERR::Okay) {
                              // Get field as an unlisted type and manage any buffer overflow
                              std::string tbuffer;
                              tbuffer.reserve(4096);
repeat:
                              tbuffer[tbuffer.capacity()-1] = 0;
                              struct acGetKey var = { fieldname.c_str(), tbuffer.data(), int(tbuffer.size()) };
                              if (Action(AC::GetKey, object.obj, &var) IS ERR::Okay) {
                                 if (tbuffer[tbuffer.capacity()-1]) {
                                    tbuffer.reserve(tbuffer.capacity() * 2);
                                    goto repeat;
                                 }
                                 Output.replace(pos, end-pos, tbuffer);
                              }
                           }
                           else Output.replace(pos, end-pos, "");
                        }
                        else Output.replace(pos, end-pos, "");
                     }
                     else { // Convert the object reference to an ID
                        Output.replace(pos, end-pos, "#" + std::to_string(objectid));
                     }
                  }
                  else log.warning("Access denied to object '%s' #%d", name.c_str(), objectid);
               }
               else log.warning("Object '%s' does not exist.", name.c_str());
            }
         }
      }
   }
}

//********************************************************************************************************************

void parser::translate_calc(std::string &Output, size_t pos)
{
   std::string temp;
   temp.reserve(Output.size());
   unsigned j = 0;
   auto end = pos + 2;
   while ((end < Output.size()) and (Output[end] != ']')) {
      if (Output[end] IS '\'') {
         temp[j++] = '\'';
         for (++end; (Output[end]) and (Output[end] != '\''); end++) temp[j++] = Output[end];
         if (Output[end]) temp[j++] = Output[end++];
      }
      else if (Output[end] IS '"') {
         temp[j++] = '"';
         for (++end; (Output[end]) and (Output[end] != '"'); end++);
         if (Output[end]) temp[j++] = Output[end++];
      }
      else temp[j++] = Output[end++];
   }
   if (end < Output.size()) end++; // Skip ']'
   std::string calcbuffer;
   calc(temp, 0, calcbuffer);
   Output.replace(pos, end-pos, calcbuffer);
}

//********************************************************************************************************************
// Translate argument reference.
// Valid examples: [@arg] [@arg:defaultvalue] [@arg:"value[]"] [@arg:'value[[]]]']

void parser::translate_param(std::string &Output, size_t pos)
{
   char terminator = ']';
   auto name_end = Output.find_first_of("]:", pos);
   if (name_end IS std::string::npos) return; // Invalid format

   auto argname = Output.substr(pos+2, name_end-pos-2);

   auto true_end = name_end;
   if (Output[true_end] IS ':') {
      true_end++;
      if ((Output[name_end] IS '\'') or (Output[name_end] IS '"')) {
         terminator = Output[name_end];
         for (++true_end; (true_end < Output.size()) and (Output[true_end] != terminator); true_end++);
         true_end++; // Skip terminator and the end bracket should appear immediately after.
         if ((true_end >= Output.size()) or (Output[true_end] != ']')) return;
      }
      else {
         true_end = Output.find(']', true_end);
         if (true_end IS std::string::npos) return; // Invalid format
      }
   }

   if (!Self->TemplateArgs.empty()) {
      bool processed = false;
      for (auto it=Self->TemplateArgs.rbegin(); (!processed) and (it != Self->TemplateArgs.rend()); it++) {
         auto args = *it;
         for (int arg=1; arg < std::ssize(args->Attribs); arg++) {
            if (!iequals(args->Attribs[arg].Name, argname)) continue;
            Output.replace(pos, true_end+1-pos, args->Attribs[arg].Value);
            processed = true;
            break;
         }
      }

      if (processed) return;
   }

   // Check against global arguments / variables

   std::string loop_value;
   if (resolve_loop_alias(argname, loop_value)) {
      Output.replace(pos, true_end+1-pos, loop_value);
   }
   else if (Self->Vars.contains(argname)) {
      Output.replace(pos, true_end+1-pos, Self->Vars[argname]);
   }
   else if (Self->Params.contains(argname)) {
      Output.replace(pos, true_end+1-pos, Self->Params[argname]);
   }
   else if (Output[name_end] IS ':') { // Resort to the default value
      name_end++;
      if ((Output[name_end] IS '\'') or (Output[name_end] IS '"')) {
         auto default_value = Output.substr(name_end + 1, true_end);
         Output.replace(pos, true_end+1-pos, default_value);
      }
      else {
         auto default_value = Output.substr(name_end, true_end - name_end);
         Output.replace(pos, true_end+1-pos, default_value);
      }
   }
   else Output.replace(pos, true_end+1-pos, "");
}

//********************************************************************************************************************

void parser::translate_reserved(std::string &Output, size_t pos, bool &time_queried)
{
   if (!Output.compare(pos, sizeof("[%index]")-1, "[%index]")) {
      Output.replace(pos, sizeof("[%index]")-1, std::to_string(current_loop_index()));
   }
   else if (!Output.compare(pos, sizeof("[%id]")-1, "[%id]")) {
      Output.replace(pos, sizeof("[%id]")-1, std::to_string(Self->UID));
   }
   else if (!Output.compare(pos, sizeof("[%self]")-1, "[%self]")) {
      Output.replace(pos, sizeof("[%self]")-1, std::to_string(Self->UID));
   }
   else if (!Output.compare(pos, sizeof("[%platform]")-1, "[%platform]")) {
      Output.replace(pos, sizeof("[%platform]")-1, GetSystemState()->Platform);
   }
   else if (!Output.compare(pos, sizeof("[%random]")-1, "[%random]")) {
      // Generate a random string of digits
      std::string random;
      random.resize(10);
      for (unsigned j=0; j < random.size(); j++) random[j] = '0' + (rand() % 10);
      Output.replace(pos, sizeof("[%random]")-1, random);
   }
   else if (!Output.compare(pos, sizeof("[%current-page]")-1, "[%current-page]")) {
      if (Self->PageTag) {
         if (auto page_name = Self->PageTag[0].attrib("name")) {
            Output.replace(pos, sizeof("[%current-page]")-1, page_name[0]);
         }
         else Output.replace(pos, sizeof("[%current-page]")-1, "");
      }
      else Output.replace(pos, sizeof("[%current-page]")-1, "");
   }
   else if (!Output.compare(pos, sizeof("[%next-page]")-1, "[%next-page]")) {
      if (Self->PageTag) {
         auto next = Self->PageTag->attrib("next-page");
         Output.replace(pos, sizeof("[%next-page]")-1, next ? *next : "");
      }
   }
   else if (!Output.compare(pos, sizeof("[%prev-page]")-1, "[%prev-page]")) {
      if (Self->PageTag) {
         auto next = Self->PageTag->attrib("prev-page");
         Output.replace(pos, sizeof("[%prev-page]")-1, next ? *next : "");
      }
   }
   else if (!Output.compare(pos, sizeof("[%path]"), "[%path]")) {
      CSTRING workingpath = "";
      Self->get(FID_WorkingPath, workingpath);
      if (!workingpath) workingpath = "";
      Output.replace(pos, sizeof("[%path]")-1, workingpath);
   }
   else if (!Output.compare(pos, sizeof("[%author]")-1, "[%author]")) {
      Output.replace(pos, sizeof("[%author]")-1, Self->Author);
   }
   else if (!Output.compare(pos, sizeof("[%description]")-1, "[%description]")) {
      Output.replace(pos, sizeof("[%description]")-1, Self->Description);
   }
   else if (!Output.compare(pos, sizeof("[%copyright]")-1, "[%copyright]")) {
      Output.replace(pos, sizeof("[%copyright]")-1, Self->Copyright);
   }
   else if (!Output.compare(pos, sizeof("[%keywords]")-1, "[%keywords]")) {
      Output.replace(pos, sizeof("[%keywords]")-1, Self->Keywords);
   }
   else if (!Output.compare(pos, sizeof("[%title]")-1, "[%title]")) {
      Output.replace(pos, sizeof("[%title]")-1, Self->Title);
   }
   else if (!Output.compare(pos, sizeof("[%font-style]")-1, "[%font-style]")) {
      Output.replace(pos, sizeof("[%font-style]")-1, m_style.style);
   }
   else if (!Output.compare(pos, sizeof("[%font-face]")-1, "[%font-face]")) {
      Output.replace(pos, sizeof("[%font-face]")-1, m_style.face);
   }
   else if (!Output.compare(pos, sizeof("[%font-fill]")-1, "[%font-fill]")) {
      Output.replace(pos, sizeof("[%font-fill]")-1, m_style.fill);
   }
   else if (!Output.compare(pos, sizeof("[%font-size]")-1, "[%font-size]")) {
      char buffer[28];
      switch(m_style.req_size.type) {
         case DU::PIXEL:
            snprintf(buffer, sizeof(buffer), "%gpx", m_style.req_size.value);
            break;
         case DU::FONT_SIZE:
            snprintf(buffer, sizeof(buffer), "%gem", m_style.req_size.value);
            break;
         case DU::SCALED:
            snprintf(buffer, sizeof(buffer), "%g%%", m_style.req_size.value * 100.0);
            break;
         default:
            snprintf(buffer, sizeof(buffer), "%dpx", DEFAULT_FONTSIZE);
            break;
      }
      Output.replace(pos, sizeof("[%font-size]")-1, buffer);
   }
   else if (!Output.compare(pos, sizeof("[%line-no]")-1, "[%line-no]")) {
      auto num = std::to_string(Self->Segments.size());
      Output.replace(pos, sizeof("[%line-no]")-1, num);
   }
   else if (!Output.compare(pos, sizeof("[%content]")-1, "[%content]")) {
      if ((m_in_template) and (m_inject_tag)) {
         std::string content = xml::GetContent(m_inject_tag[0][0]);
         Output.replace(pos, sizeof("[%content]")-1, content);

         //if (!xmlSerialise(m_inject_xml, m_inject_tag[0][0].ID, XMF::INCLUDE_SIBLINGS, &content)) {
         //   Output.replace(pos, sizeof("[%content]")-1, content);
         //   FreeResource(content);
         //}
      }
   }
   else if (!Output.compare(pos, sizeof("[%tm-day]")-1, "[%tm-day]")) {
      if (!m_time) m_time = objTime::create::local();
      if (!time_queried) { acQuery(m_time); time_queried = true; }
      Output.replace(pos, sizeof("[%tm-day]")-1, std::to_string(m_time->Day));
   }
   else if (!Output.compare(pos, sizeof("[%tm-month]")-1, "[%tm-month]")) {
      if (!m_time) m_time = objTime::create::local();
      if (!time_queried) { acQuery(m_time); time_queried = true; }
      Output.replace(pos, sizeof("[%tm-month]")-1, std::to_string(m_time->Month));
   }
   else if (!Output.compare(pos, sizeof("[%tm-year]")-1, "[%tm-year]")) {
      if (!m_time) m_time = objTime::create::local();
      if (!time_queried) { acQuery(m_time); time_queried = true; }
      Output.replace(pos, sizeof("[%tm-year]")-1, std::to_string(m_time->Year));
   }
   else if (!Output.compare(pos, sizeof("[%tm-hour]")-1, "[%tm-hour]")) {
      if (!m_time) m_time = objTime::create::local();
      if (!time_queried) { acQuery(m_time); time_queried = true; }
      Output.replace(pos, sizeof("[%tm-hour]")-1, std::to_string(m_time->Hour));
   }
   else if (!Output.compare(pos, sizeof("[%tm-minute]")-1, "[%tm-minute]")) {
      if (!m_time) m_time = objTime::create::local();
      if (!time_queried) { acQuery(m_time); time_queried = true; }
      Output.replace(pos, sizeof("[%tm-minute]")-1, std::to_string(m_time->Minute));
   }
   else if (!Output.compare(pos, sizeof("[%tm-second]")-1, "[%tm-second]")) {
      if (!m_time) m_time = objTime::create::local();
      if (!time_queried) { acQuery(m_time); time_queried = true; }
      Output.replace(pos, sizeof("[%tm-second]")-1, std::to_string(m_time->Second));
   }
   else if (!Output.compare(pos, sizeof("[%version]")-1, "[%version]")) {
      Output.replace(pos, sizeof("[%version]")-1, RIPL_VERSION);
   }
   else if (!Output.compare(pos, sizeof("[%view-height]")-1, "[%view-height]")) {
      char buffer[28];
      snprintf(buffer, sizeof(buffer), "%g", Self->VPHeight);
      Output.replace(pos, sizeof("[%view-height]")-1, buffer);
   }
   else if (!Output.compare(pos, sizeof("[%view-width]")-1, "[%view-width]")) {
      char buffer[28];
      snprintf(buffer, sizeof(buffer), "%g", Self->VPWidth);
      Output.replace(pos, sizeof("[%view-width]")-1, buffer);
   }
}

//********************************************************************************************************************

static int8_t datatype(std::string_view String)
{
   size_t i = 0;
   while ((i < String.size()) and (String[i]) and (String[i] <= 0x20)) i++; // Skip white-space

   if ((i < String.size()) and (String[i] IS '0') and (i+1 < String.size()) and (String[i+1] IS 'x')) {
      for (i+=2; (i < String.size()) and String[i]; i++) {
         if (((String[i] >= '0') and (String[i] <= '9')) or
             ((String[i] >= 'A') and (String[i] <= 'F')) or
             ((String[i] >= 'a') and (String[i] <= 'f')));
         else return 's';
      }
      return 'h';
   }

   bool is_number = true;
   bool is_float  = false;

   for (; (i < String.size()) and (String[i]) and (is_number); i++) {
      if (((String[i] < '0') or (String[i] > '9')) and (String[i] != '.') and (String[i] != '-')) is_number = false;
      if (String[i] IS '.') is_float = true;
   }

   if ((is_float) and (is_number)) return 'f';
   else if (is_number) return 'i';
   else return 's';
}

//********************************************************************************************************************

static bool eval_condition(const std::string &String)
{
   pf::Log log(__FUNCTION__);

   static const FieldDef table[] = {
      { "<>", COND_NOT_EQUAL },
      { "!=", COND_NOT_EQUAL },
      { "=",  COND_EQUAL },
      { "==", COND_EQUAL },
      { "<",  COND_LESS_THAN },
      { "<=", COND_LESS_EQUAL },
      { ">",  COND_GREATER_THAN },
      { ">=", COND_GREATER_EQUAL },
      { nullptr, 0 }
   };

   int start = 0;
   while ((start < int(String.size())) and (unsigned(String[start]) <= 0x20)) start++;

   bool reverse = false;

   // Find the condition statement

   size_t i;
   for (i=start; i < String.size(); i++) {
      if ((String[i] IS '!') and (i+1 < String.size()) and (String[i+1] IS '=')) break;
      if (String[i] IS '>') break;
      if (String[i] IS '<') break;
      if (String[i] IS '=') break;
   }

   // If there is no condition statement, evaluate the statement as an integer

   if (i >= String.size()) {
      if (std::stoi(String)) return true;
      else return false;
   }

   auto cpos = i;

   // Extract Test value

   while ((i > 0) and (String[i-1] IS ' ')) i--;
   std::string test(String, 0, i);

   // Condition value

   int condition = 0;
   {
      char cond[4];
      int c;
      for (i=cpos,c=0; (c < 2) and (i < String.size()) and ((String[i] IS '!') or (String[i] IS '=') or (String[i] IS '>') or (String[i] IS '<')); i++) {
         cond[c++] = String[i];
      }
      cond[c] = 0;

      for (unsigned j=0; table[j].Name; j++) {
         if (iequals(cond, table[j].Name)) {
            condition = table[j].Value;
            break;
         }
      }
   }

   while ((i < String.size()) and (String[i]) and (unsigned(String[i]) <= 0x20)) i++; // skip white-space

   bool truth = false;
   if (!test.empty()) {
      if (condition) {
         // Convert the If->Compare to its specified type

         auto cmp_type  = datatype(std::string_view(String.begin()+i, String.end()));
         auto test_type = datatype(test);

         if (((test_type IS 'i') or (test_type IS 'f')) and ((cmp_type IS 'i') or (cmp_type IS 'f'))) {
            auto cmp_float  = strtod(String.c_str()+i, nullptr);
            auto test_float = strtod(test.c_str(), nullptr);
            switch (condition) {
               case COND_NOT_EQUAL:     if (test_float != cmp_float) truth = true; break;
               case COND_EQUAL:         if (test_float IS cmp_float) truth = true; break;
               case COND_LESS_THAN:     if (test_float <  cmp_float) truth = true; break;
               case COND_LESS_EQUAL:    if (test_float <= cmp_float) truth = true; break;
               case COND_GREATER_THAN:  if (test_float >  cmp_float) truth = true; break;
               case COND_GREATER_EQUAL: if (test_float >= cmp_float) truth = true; break;
               default: log.warning("Unsupported condition type %d.", condition);
            }
         }
         else if (condition IS COND_EQUAL) {
            truth = iequals(test, String.c_str()+i);
         }
         else if (condition IS COND_NOT_EQUAL) {
            truth = !iequals(test, String.c_str()+i);
         }
         else log.warning("String comparison for condition %d not possible.", condition);
      }
      else log.warning("No test condition in \"%s\".", String.c_str());
   }
   else log.warning("No test value in \"%s\".", String.c_str());

   if (reverse) return truth ^ 1;
   else return truth;
}

//********************************************************************************************************************
// Used by if, elseif, while statements to check the satisfaction of conditions.
//
// Stage 1 XQuery integration: the `test` attribute is recognised as a standalone XQuery expression.  When present it
// takes precedence over the legacy statement/exists/notnull/null/not attributes.  The result of the expression is
// coerced via XQuery's effective boolean value rules (boolean(...)).

static bool check_tag_conditions(parser *Parser, XTag &Tag)
{
   pf::Log log("eval");

   // Check for an XQuery test attribute first so that it overrides legacy condition attributes.
   for (unsigned i=1; i < Tag.Attribs.size(); i++) {
      auto name = std::string_view(Tag.Attribs[i].Name);
      if ((!name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
      if (iequals("test", name)) {
         std::string result;
         bool boolean_result = false;
         auto err = xq_eval_helper(Parser, Parser->m_xml, &Tag, Tag.Attribs[i].Value,
            XQEval::BOOLEAN, result, boolean_result);
         if (err != ERR::Okay) {
            log.warning("XQuery test failed: %s", Tag.Attribs[i].Value.c_str());
            Parser->Self->Error = err;
            return false;
         }
         log.trace("Test: %s -> %s", Tag.Attribs[i].Value.c_str(), boolean_result ? "true" : "false");
         return boolean_result;
      }
   }

   bool satisfied = false;
   bool reverse = false;
   for (unsigned i=1; i < Tag.Attribs.size(); i++) {
      if (iequals("statement", Tag.Attribs[i].Name)) {
         satisfied = eval_condition(Tag.Attribs[i].Value);
         log.trace("Statement: %s", Tag.Attribs[i].Value);
         break;
      }
      else if (iequals("exists", Tag.Attribs[i].Name)) {
         OBJECTID object_id;
         if (FindObject(Tag.Attribs[i].Value.c_str(), CLASSID::NIL, FOF::SMART_NAMES, &object_id) IS ERR::Okay) {
            satisfied = valid_objectid(Parser->Self, object_id) ? true : false;
         }
         break;
      }
      else if (iequals("notnull", Tag.Attribs[i].Name)) {
         log.trace("NotNull: %s", Tag.Attribs[i].Value);
         if (Tag.Attribs[i].Value.empty()) satisfied = false;
         else if (Tag.Attribs[i].Value IS "0") satisfied = false;
         else satisfied = true;
      }
      else if ((iequals("isnull", Tag.Attribs[i].Name)) or (iequals("null", Tag.Attribs[i].Name))) {
         log.trace("IsNull: %s", Tag.Attribs[i].Value);
            if (Tag.Attribs[i].Value.empty()) satisfied = true;
            else if (Tag.Attribs[i].Value IS "0") satisfied = true;
            else satisfied = false;
      }
      else if (iequals("not", Tag.Attribs[i].Name)) {
         reverse = true;
      }
   }

   // Check for a not condition and invert the satisfied value if found

   if (reverse) satisfied = satisfied ^ 1;

   return satisfied;
}

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

   XTag *object_template = nullptr;

   auto saved_attribs = Tag.Attribs;
   translate_attrib_args(Tag.Attribs);

   auto tagname = Tag.Attribs[0].Name;
   if (tagname.starts_with('$')) tagname.erase(0, 1);
   auto tag_hash = strihash(tagname);
   object_template = nullptr;

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
      Tag.Attribs = saved_attribs;
      return result;
   }

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
         // Process the template by jumping into it.  Key-values in the tag are added to a sequential
         // list that will be processed in reverse by translate_attrib_args().

         auto xml  = m_inject_xml;
         auto tags = m_inject_tag;
         m_inject_xml = xml;
         m_inject_tag = &Tag.Children;
         m_in_template++;

         pf::Log log(__FUNCTION__);
         log.traceBranch("Executing template '%s'.", tagname.c_str());

         Self->TemplateArgs.push_back(&Tag);
         auto old_xml = change_xml(Self->Templates);

         parse_tags(Self->TemplateIndex[tag_hash]->Children, Flags);

         change_xml(old_xml);
         Self->TemplateArgs.pop_back();

         Tag.Attribs = saved_attribs;

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
            Tag.Attribs = saved_attribs;
            return result;
         default:
            break;
      }
   }

   switch (tag_hash) {
      // Content tags (tags that affect text, the page layout etc)
      // The content is compulsory, otherwise tag has no effect
      case HASH_a:
      case HASH_link:
         if (!Tag.Children.empty()) tag_link(Tag);
         else log.trace("No content found in tag '%s'", tagname.c_str());
         break;

      case HASH_b:
         if (!Tag.Children.empty()) tag_font_style(Tag.Children, FSO::NIL, "Bold");
         break;

      case HASH_div:
         if (!Tag.Children.empty()) tag_div(Tag);
         break;

      case HASH_p: tag_paragraph(Tag); break;

      case HASH_font:
         if (!Tag.Children.empty()) tag_font(Tag);
         break;

      case HASH_i:
         if (!Tag.Children.empty()) tag_font_style(Tag.Children, FSO::NIL, "Italic");
         break;

      case HASH_li:
         if (!Tag.Children.empty()) tag_li(Tag);
         break;

      case HASH_pre:
         if (!Tag.Children.empty()) tag_pre(Tag.Children);
         break;

      case HASH_u: if (!Tag.Children.empty()) tag_font_style(Tag.Children, FSO::UNDERLINE, m_style.style); break;

      case HASH_list: if (!Tag.Children.empty()) tag_list(Tag); break;

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

      case HASH_repeat: if (!Tag.Children.empty()) tag_repeat(Tag); break;

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
         if (!Tag.Children.empty()) {
            loop_frame frame;
            frame.index = 0;
            frame.iteration = 0;
            frame.start = 0;
            frame.step = 1;

            loop_guard loop(this, frame);

            while (true) {
               XTag condition_tag = Tag;
               condition_tag.Attribs = saved_attribs;
               translate_attrib_args(condition_tag.Attribs);

               if (!check_tag_conditions(this, condition_tag)) break;
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

      case HASH_page: if (!Tag.Children.empty()) tag_page(Tag); break;

      case HASH_svg: if (!Tag.Children.empty()) tag_svg(Tag); break;

      // Table layout instructions

      case HASH_row:
         if ((Flags & IPF::FILTER_TABLE) IS IPF::NIL) {
            log.warning("Invalid use of <row> - Applied to invalid parent tag.");
            Self->Error = ERR::InvalidData;
         }
         else if (!Tag.Children.empty()) tag_row(Tag);
         break;

      case HASH_td: // HTML compatibility
      case HASH_cell:
         if ((Flags & IPF::FILTER_ROW) IS IPF::NIL) {
            log.warning("Invalid use of <cell> - Applied to invalid parent tag.");
            Self->Error = ERR::InvalidData;
         }
         else if (!Tag.Children.empty()) tag_cell(Tag);
         break;

      case HASH_table: if (!Tag.Children.empty()) tag_table(Tag); break;

      case HASH_tr: if (!Tag.Children.empty()) tag_row(Tag); break;

      // Others

      case HASH_data: break; // Intentionally does nothing

      case HASH_edit_def: tag_editdef(Tag); break;

      case HASH_footer:
         if (!Tag.Children.empty()) m_footer_tag = &Tag.Children;
         break;

      case HASH_header:
         if (!Tag.Children.empty()) m_header_tag = &Tag.Children;
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

   Tag.Attribs = saved_attribs;
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

   if (!Tag.Children.empty()) m_body_tag = &Tag.Children;
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

   if (!script) {
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

const char glButtonSVG[] = R"-(
<svg width="100%" height="100%">
  <defs>
    <linearGradient id="darkEdge" x1="0" y1="1" x2="0" y2="0" gradientUnits="objectBoundingBox">
      <stop stop-color="#000000" stop-opacity="1" offset="0"/>
      <stop stop-color="#050505" stop-opacity="1" offset="0.84"/>
      <stop stop-color="#afafaf" stop-opacity="1" offset="1"/>
    </linearGradient>

    <filter id="dropShadow" color-interpolation-filters="sRGB" primitiveUnits="objectBoundingBox">
      <feGaussianBlur stdDeviation="0.013"/>
    </filter>

    <linearGradient id="shading" gradientUnits="objectBoundingBox" x1="0" y1="0" x2="0" y2="1.04">
      <stop stop-color="#ffffff" stop-opacity="0.50" offset="0"/>
      <stop stop-color="#7f7f7f" stop-opacity="0" offset="0.5"/>
      <stop stop-color="#000000" stop-opacity="0.54" offset="1"/>
    </linearGradient>
  </defs>

  <rect opacity="0.6" fill="rgb(0,0,0)" filter="url(#dropShadow)" width="95%" height="93%"
    x="2.5%" y="4%" ry="7.5%" rx="7.5%"/>
  <rect fill="#555d6d" width="95%" height="93%" x="2.5%" y="2.5%" ry="7.5%" rx="7.5%"/>
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

   if (!m_button_patterns) {
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
               fl::Stroke("rgba(64,64,64,.5)"), fl::StrokeWidth(2.0),
               fl::RoundX(SCALE(0.1)),
               fl::Fill("rgba(0,0,0,.125)")
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
               fl::Stroke("rgba(0,0,0,.25)"), fl::StrokeWidth(2.0),
               fl::RoundX(SCALE(0.1)),
               fl::Fill("rgba(255,255,255,.37)")
            });
         }

         Self->Viewport->Scene->addDef("/widget/button/inactive", pattern_inactive);
      }
   }

   if (widget.fill.empty())      widget.fill      = "url(#/widget/button/inactive)";
   if (widget.font_fill.empty()) widget.font_fill = "rgba(255,255,255,.86)";

   widget.def_size = DUNIT(1.7, DU::FONT_SIZE);

   if (!Tag.Children.empty()) {
      Self->NoWhitespace = true; // Reset whitespace flag: false allows whitespace at the start of the cell, true prevents whitespace

      parser parse(Self, widget.stream);

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

   if (!m_checkbox_patterns) {
      m_checkbox_patterns = true;

      if (auto pattern_on = objVectorPattern::create::global({
            fl::Name("checkbox_on"),
            fl::SpreadMethod(VSPREAD::CLIP)
         })) {

         auto vp = pattern_on->Scene->Viewport;
         objVectorRectangle::create::global({
            fl::Owner(vp->UID),
            fl::X(-8), fl::Y(-8), fl::Width(54), fl::Height(54),
            fl::Stroke("rgb(255,255,255)"), fl::StrokeWidth(2.0),
            fl::RoundX(6), fl::RoundY(6),
            fl::Fill("rgba(0,0,0,.7)")
         });

         objVectorPath::create::global({
            fl::Owner(vp->UID),
            fl::Sequence("M4.75 15.0832 15.8333 26.1665 33.2498 4 38 8.75 15.8333 35.6665 0 19.8332 4.75 15.0832Z"),
            fl::Fill("rgb(255,255,255)")
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
            fl::Stroke("rgb(255,255,255)"), fl::StrokeWidth(2.0),
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

   if (!widget.label.empty()) widget.label_pad = DUNIT(0.5, DU::FONT_SIZE);

   if (!widget.name.empty()) Self->Vars[widget.name] = widget.alt_state ? "1" : "0";

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

   if (!Tag.Children.empty()) {
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

            if (!scan.Children.empty()) {
               STRING xml_ser;
               if (m_xml->serialise(scan.Children[0].ID, XMF::INCLUDE_SIBLINGS, &xml_ser) IS ERR::Okay) {
                  value = xml_ser;
                  FreeResource(xml_ser);
               }
            }

            if (!value.empty()) {
               auto &option = widget.menu.m_items.emplace_back(value);

               auto id = scan.attrib("id");
               if ((id) and (!id->empty())) option.id = *id;

               auto val = scan.attrib("value");
               if ((val) and (!val->empty())) option.value = *val;

               auto icon = scan.attrib("icon");
               if (icon) option.icon = *icon;
            }
         }
      }
   }

   if (!m_combobox_patterns) {
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

   if (widget.font_fill.empty()) widget.font_fill = "rgb(255,255,255)";

   widget.def_size  = DUNIT(1.7, DU::FONT_SIZE);
   widget.label_pad = DUNIT(0.5, DU::FONT_SIZE);

   if (!widget.name.empty()) Self->Vars[widget.name] = widget.value;

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

   if (widget.font_fill.empty()) widget.font_fill = "rgb(255,255,255)";

   widget.def_size  = DUNIT(1.7, DU::FONT_SIZE);
   widget.label_pad = DUNIT(0.5, DU::FONT_SIZE);

   if (!widget.name.empty()) Self->Vars[widget.name] = widget.value;

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

   if (!id.empty()) {
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
         else if (!iequals(Tag.Attribs[i].Value, "left")) {
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
            if (!Tag.Attribs[i].Value.empty()) edit.on_change = Tag.Attribs[i].Value;
            break;

         case HASH_on_exit:
            if (!Tag.Attribs[i].Value.empty()) edit.on_exit = Tag.Attribs[i].Value;
            break;

         case HASH_on_enter:
            if (!Tag.Attribs[i].Value.empty()) edit.on_enter = Tag.Attribs[i].Value;
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

   if (!name.empty()) Self->EditDefs[name] = std::move(edit);
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
// Parse a string value as XML

void parser::tag_parse(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   // The value attribute will contain XML.  We will parse the XML as if it were part of the document source.  This feature
   // is typically used when pulling XML information out of an object field.
   //
   // Stage 1 XQuery integration:
   //   select="..."  evaluates an XQuery expression and parses the resulting string as XML/RIPL content.

   if (std::ssize(Tag.Attribs) > 1) {
      // Prefer select over value when both are present.
      for (int i=1; i < std::ssize(Tag.Attribs); i++) {
         auto name = std::string_view(Tag.Attribs[i].Name);
         if ((!name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
         if (iequals("select", name)) {
            std::string result;
            bool unused;
            auto err = xq_eval_helper(this, m_xml, &Tag, Tag.Attribs[i].Value,
               XQEval::STRING, result, unused);
            if (err != ERR::Okay) {
               log.warning("<parse select=\"%s\"> failed.", Tag.Attribs[i].Value.c_str());
               Self->Error = err;
               return;
            }

            log.traceBranch("Parsing XQuery result as XML...");
            auto xmlinc = objXML::create::local(fl::Statement(result),
               fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS));
            if (!xmlinc) {
               log.warning("<parse select> produced non-parseable XML: %.120s", result.c_str());
               Self->Error = ERR::Syntax;
               return;
            }
            auto old_xml = change_xml(xmlinc);
            parse_tags(xmlinc->Tags);
            change_xml(old_xml);
            Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
            return;
         }
      }

      if ((iequals("value", Tag.Attribs[1].Name)) or
          (iequals("$value", Tag.Attribs[1].Name))) {
         log.traceBranch("Parsing string value as XML...");

         if (auto xmlinc = objXML::create::local(fl::Statement(Tag.Attribs[1].Value), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS))) {
            auto old_xml = change_xml(xmlinc);
            parse_tags(xmlinc->Tags);
            change_xml(old_xml);

            // Add the created XML object to the document rather than destroying it

            Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
         }
      }
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

   if (!img.fill.empty()) {
      if (img.width.value <= 0) img.width.clear(); // Zero is equivalent to 'auto', meaning on-the-fly computation
      if (img.height.value <= 0) img.height.clear();

      if (!img.floating_x()) Self->NoWhitespace = false; // Images count as characters when inline.
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

   if ((!name) and (!Tag.Children.empty())) {
      if (Tag.Children[0].isContent()) name = strihash(Tag.Children[0].Attribs[0].Value);
   }

   if (name) {
      bc_index index(name, glUID++, 0, visible, Self->Invisible ? false : true);

      auto &stream_index = m_stream->emplace(m_index, index);

      if (!Tag.Children.empty()) {
         if (!visible) Self->Invisible++;
         parse_tags(Tag.Children);
         if (!visible) Self->Invisible--;
      }

      bc_index_end end(stream_index.id);
      m_stream->emplace(m_index, end);
   }
   else if (!Tag.Children.empty()) parse_tags(Tag.Children);
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

   if ((link.type != LINK::NIL) or (!Tag.Children.empty())) {
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

      if (!Tag.Children.empty()) parse_tags(Tag.Children);

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
         else if (!iequals(Tag.Attribs[i].Value, "left")) {
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

void parser::tag_print(XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   // Copy the content from the value attribute into the document stream.  If used inside an object, the data is sent
   // to that object as XML.
   //
   // Stage 1 XQuery integration:
   //   select="..."  evaluates an XQuery expression and inserts its string value
   //   value="..."   may contain `{...}` attribute value template fragments

   if (Tag.Attribs.size() > 1) {
      // Prefer select over value when both are present.
      for (int i=1; i < std::ssize(Tag.Attribs); i++) {
         auto name = std::string_view(Tag.Attribs[i].Name);
         if ((!name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
         if (iequals("select", name)) {
            std::string result;
            bool unused;
            auto err = xq_eval_helper(this, m_xml, &Tag, Tag.Attribs[i].Value,
               XQEval::STRING, result, unused);
            if (err != ERR::Okay) {
               log.warning("<print select=\"%s\"> failed.", Tag.Attribs[i].Value.c_str());
               Self->Error = err;
               return;
            }
            insert_text(Self, m_stream, m_index, result, (m_style.options & FSO::PREFORMAT) != FSO::NIL);
            return;
         }
      }

      auto tagname = Tag.Attribs[1].Name.c_str();
      if (*tagname IS '$') tagname++;

      if (iequals("value", tagname)) {
         // If the attribute contains AVT fragments, expand them via XQuery.  Otherwise preserve existing behaviour.
         const auto &raw = Tag.Attribs[1].Value;
         bool has_avt = (raw.find('{') != std::string::npos) or (raw.find('}') != std::string::npos);

         if (has_avt) {
            std::string expanded;
            if (auto err = xq_expand_avt(this, m_xml, &Tag, raw, expanded); err != ERR::Okay) {
               log.warning("<print value=\"%s\"> AVT expansion failed.", raw.c_str());
               Self->Error = err;
               return;
            }
            insert_text(Self, m_stream, m_index, expanded, (m_style.options & FSO::PREFORMAT) != FSO::NIL);
         }
         else insert_text(Self, m_stream, m_index, raw, (m_style.options & FSO::PREFORMAT) != FSO::NIL);
      }
      else if (iequals("src", Tag.Attribs[1].Name)) {
         // This option is only supported in unrestricted mode
         if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) {
            CacheFile *cache;
            if (LoadFile(Tag.Attribs[1].Value.c_str(), LDF::NIL, &cache) IS ERR::Okay) {
               insert_text(Self, m_stream, m_index, std::string((CSTRING)cache->Data), (m_style.options & FSO::PREFORMAT) != FSO::NIL);
               UnloadFile(cache);
            }
         }
         else log.warning("Cannot <print src.../> unless in unrestricted mode.");
      }
   }
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
      if ((iequals("name", Tag.Attribs[n].Name)) and (!Tag.Attribs[n].Value.empty())) break;
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

ERR parser::calc(const std::string &String, double *Result, std::string &Output)
{
   enum SIGN { PLUS=1, MINUS, MULTIPLY, DIVIDE, MODULO };

   if (Result) *Result = 0;

   Output.clear();

   // Search for brackets and translate them first

   std::string in(String);
   while (true) {
      // Find the last bracketed reference

      int last_bracket = 0;
      for (unsigned i=0; i < in.size(); i++) {
         if (in[i] IS '\'') { // Skip anything in quotes
            i++;
            while (in[i]) {
               if (in[i] IS '\\') {
                  i++; // Skip backslashes and the following character
                  if (!in[i]) break;
               }
               else if (in[i] IS '\'') break;
               i++;
            }
            if (in[i] IS '\'') i++;
         }
         else if (in[i] IS '(') last_bracket = i;
      }

      if (last_bracket > 0) { // Bracket found, translate its contents
         int end;
         for (end=last_bracket+1; (in[end]) and (in[end-1] != ')'); end++);
         std::string buf(in, last_bracket, end - last_bracket);

         double calc_float;
         std::string out;
         calc(buf.c_str()+1, &calc_float, out);
         in.replace(last_bracket, end - last_bracket, out);
      }
      else break;
   }

   // Perform the calculation

   STRING end;
   int16_t precision = 9;
   double total   = 0;
   double overall = 0;
   int index     = 0;
   SIGN sign      = PLUS;
   bool number    = false;
   for (unsigned s=0; in[s];) {
      if (unsigned(in[s]) <= 0x20); // Do nothing with whitespace
      else if (in[s] IS '\'') {
         if (number) { // Write the current floating point number to the buffer before the next calculation
            Output  += write_calc(total, precision);
            overall += total; // Reset the number
            total   = 0;
            number  = false;
         }

         s++;
         while (index < int(Output.size())-1) {
            if (in[s] IS '\\') s++; // Skip the \ character and continue so that we can copy the character immediately after it
            else if (in[s] IS '\'') break;

            Output += in[s++];
         }
      }
      else if (in[s] IS 'f') { // Fixed floating point precision adjustment
         s++;
         precision = -strtol(in.c_str() + s, &end, 10);
         s += end - in.c_str();
         continue;
      }
      else if (in[s] IS 'p') { // Floating point precision adjustment
         s++;
         precision = strtol(in.c_str() + s, &end, 10);
         s += end - in.c_str();
         continue;
      }
      else if ((in[s] >= '0') and (in[s] <= '9')) {
         number = true;
         double fvalue = strtod(in.c_str() + s, &end);
         s += end - in.c_str();

         if (sign IS MINUS)         total = total - fvalue;
         else if (sign IS MULTIPLY) total = total * fvalue;
         else if (sign IS MODULO)   total = F2I(total) % F2I(fvalue);
         else if (sign IS DIVIDE) {
            if (fvalue) total = total / fvalue; // NB: Avoid division by zero errors
         }
         else total += fvalue;

         sign = PLUS; // The mathematical sign is reset whenever a number is encountered
         continue;
      }
      else if (in[s] IS '-') {
         if (sign IS MINUS) sign = PLUS; // Handle double-negatives
         else sign = MINUS;
      }
      else if (in[s] IS '+') sign = PLUS;
      else if (in[s] IS '*') sign = MULTIPLY;
      else if (in[s] IS '/') sign = DIVIDE;
      else if (in[s] IS '%') sign = MODULO;

      for (++s; (in[s] & 0xc0) IS 0x80; s++);
   }

   if (number) Output += write_calc(total, precision);
   if (Result) *Result = overall + total;
   return ERR::Okay;
}

/*********************************************************************************************************************

This function is used to translate strings that make object and field references using the standard referencing format.
References are made to objects by enclosing statements within square brackets.  As a result of calling this function,
all references within the Buffer will be translated to their relevant format.  The Buffer needs to be large enough to
accommodate these adjustments as it will be expanded during the translation.  It is recommended that the Buffer is at
least two times the actual length of the string that you are translating.

Valid references can be made to an object by name, ID or relative parameters.  Here are some examples illustrating the
different variations:

<types type="Reference">
<type name="[surface]">Name reference.</>
<type name="[#49302]">ID reference.</>
<type name="[self]">Relative reference to the object that has the current context, or the document.</>
</table>

Field references are a slightly different matter and will be converted to the value of the field that they are
referencing.  A field reference is defined using the object referencing format, but they contain a `.fieldname`
extension.  Here are some examples:

<pre>
[surface.width]
[file.location]
</pre>

A string such as `[mywindow.height] + [mywindow.width]` could be translated to `255 + 120` for instance.

Simple calculations are possible by enclosing a statement within a `[=...]` section.  For example the aforementioned
string can be expanded to `[=[mywindow.height] + [mywindow.width]]`, which would give a result of 375.

The escape character for string translation is `$` and should be used as `[$...]`, which prevents everything within the
square brackets from being translated.  The `[$]` characters will be removed as part of this process unless the
KEEP_ESCAPE flag is used.  To escape a single right or left bracket, use `[rb]` or `[lb]` respectively.

*********************************************************************************************************************/

// Evaluate object references and calculations

ERR parser::tag_xml_content_eval(std::string &Buffer)
{
   pf::Log log(__FUNCTION__);
   int i;

   // Quick check for translation symbols

   if (Buffer.find('[') IS std::string::npos) return ERR::EmptyString;

   log.traceBranch("%.80s", Buffer.c_str());

   ERR error = ERR::Okay;
   ERR majorerror = ERR::Okay;

   // Skip to the end of the buffer (translation occurs 'backwards')

   auto pos = std::ssize(Buffer) - 1;
   while (pos >= 0) {
      if ((Buffer[pos] IS '[') and ((Buffer[pos+1] IS '@') or (Buffer[pos+1] IS '%'))) {
         // Ignore arguments, e.g. [@id] or [%id].  It's also useful for ignoring [@attrib] in xpath.
         pos--;
      }
      else if (Buffer[pos] IS '[') {
         // Make sure that there is a balanced closing bracket

         int end;
         int balance = 0;
         for (end=pos; Buffer[end]; end++) {
            if (Buffer[end] IS '[') balance++;
            else if (Buffer[end] IS ']') {
               balance--;
               if (!balance) break;
            }
         }

         if (Buffer[end] != ']') {
            log.warning("Unbalanced string: %.90s ...", Buffer.c_str());
            return ERR::InvalidData;
         }

         if (Buffer[pos+1] IS '=') { // Perform a calculation
            std::string num;
            num.assign(Buffer, pos+2, end-(pos+2));

            std::string calcbuffer;
            double value;
            calc(num, &value, calcbuffer);
            Buffer.insert(end-pos+1, calcbuffer);
         }
         else if (Buffer[pos+1] IS '$') { // Escape sequence - e.g. translates [$ABC] to ABC.  Note: Use [rb] and [lb] instead for brackets.
            Buffer.erase(end, 1); // ']'
            Buffer.erase(pos, 2); // '[$'
            pos--;
            continue;
         }
         else {
            std::string name;
            name.reserve(64);

            for (i=pos+1; (Buffer[i] != '.') and (i < end); i++) {
               name += std::tolower(Buffer[i]);
            }

            // Check for [lb] and [rb] escape codes

            char code = 0;
            if (name IS "rb") code = ']';
            else if (name IS "lb") code = '[';

            if (code) {
               Buffer[pos] = code;
               Buffer.erase(pos+1, 3);
               pos--;
               continue;
            }
            else {
               OBJECTID objectid = 0;
               if (iequals(name, "self")) objectid = CurrentContext()->UID;
               else FindObject(name.c_str(), CLASSID::NIL, FOF::SMART_NAMES, &objectid);

               if (objectid) {
                  OBJECTPTR object = nullptr;
                  if (Buffer[i] IS '.') {
                     // Get the field from the object
                     i++;

                     std::string field(Buffer, i, end);
                     if (AccessObject(objectid, 2000, &object) IS ERR::Okay) {
                        OBJECTPTR target;
                        const Field *classfield;
                        if (((classfield = find_field(object, field, &target))) and (classfield->Flags & FD_STRING)) {
                           CSTRING str;
                           if (object->get(classfield->FieldID, str) IS ERR::Okay) {
                              Buffer.insert(end-pos+1, str);
                           }
                        }
                        else if (CheckAction(object, AC::GetKey) IS ERR::Okay) {
                           // Get field as an unlisted type and manage any buffer overflow
                           std::string tbuffer;
                           tbuffer.reserve(4096);
repeat:
                           tbuffer[tbuffer.capacity()-1] = 0;
                           struct acGetKey var{ field.c_str(), tbuffer.data(), int(tbuffer.size()) };
                           if (Action(AC::GetKey, object, &var) IS ERR::Okay) {
                              if (tbuffer[tbuffer.capacity()-1]) {
                                 tbuffer.reserve(tbuffer.capacity() * 2);
                                 goto repeat;
                              }
                              Buffer.insert(end-pos+1, tbuffer);
                           }
                        }
                        // NB: For fields, error code is always Okay so that the reference evaluates to NULL

                        ReleaseObject(object);
                     }
                     else error = ERR::AccessObject;
                  }
                  else { // Convert the object reference to an ID
                     Buffer.insert(end-pos+1, std::move(std::string("#") + std::to_string(objectid)));
                  }
               }
               else {
                  error = ERR::NoMatchingObject;
                  log.traceWarning("Failed to find object '%s'", name.c_str());
               }
            }
         }

         if (error != ERR::Okay) {
            pos--;
            majorerror = error;
            error = ERR::Okay;
         }
      }
      else pos--;
   }

   log.trace("Result: %s", Buffer.c_str());

   return majorerror;
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

void parser::tag_object(XTag &Tag)
{
   /*
   pf::Log log(__FUNCTION__);

   // NF::LOCAL is only set when the object is owned by the document

   OBJECTPTR object;
   if (NewObject(class_id, (Self->CurrentObject) ? NF::NIL : NF::LOCAL, (OBJECTPTR *)&object)) {
      log.warning("Failed to create object of class #%d.", class_id);
      return;
   }

   log.branch("Processing %s object from document tag, owner #%d.", object->Class->ClassName, Self->CurrentObject ? Self->CurrentObject->UID : -1);

   // Setup the callback interception so that we can control the order in which objects draw their graphics to the surface.

   if (Self->CurrentObject) {
      SetOwner(object, Self->CurrentObject);
   }
   else if (!pagetarget.empty()) {
      auto field_id = strihash(pagetarget);
      if (Self->BkgdGfx) object->set(field_id, Self->View);
      else object->set(field_id, Self->Page);
   }

   for (unsigned i=1; i < Tag.Attribs.size(); i++) {
      auto argname = Tag.Attribs[i].Name.c_str();
      while (*argname IS '$') argname++;
      if (Tag.Attribs[i].Value.empty()) object->set(strihash(argname), "1");
      else object->set(strihash(argname), Tag.Attribs[i].Value);
   }

   // Check for the 'data' tag which can be used to send data feed information prior to initialisation.
   //
   // <data type="text">Content</data>
   // <data type="xml" template="TemplateName"/>
   // <data type="xml" object="[xmlobj]"/>
   // <data type="xml">Content</data>

   bool customised = false;

   if (!Tag.Children.empty()) {
      STRING src;

      for (auto &scan : Tag.Children) {
         if (!iequals("data", scan.Attribs[0].name)) continue;

         if (*e_revert > *s_revert) {
            while (*e_revert > *s_revert) {
               *e_revert -= 1;
               Self->RestoreAttrib[*e_revert].Attrib[0] = Self->RestoreAttrib[*e_revert].String;
            }
         }
         Self->RestoreAttrib.resize(*s_revert);

         *s_revert = Self->RestoreAttrib.size();
         *e_revert = 0;
         translate_attrib_args(scan.Attribs);
         *e_revert = Self->RestoreAttrib.size();

         const std::string *type = scan.attrib("type");

         if ((!type) or (iequals("text", type))) {
            if (scan.Children.empty()) continue;
            std::string buffer;
            xmlGetContent(scan, buffer);
            if (!buffer.empty()) acDataText(object, buffer.c_str());
         }
         else if (iequals("xml", type)) {
            customised = true;

            if (auto t = scan.attrib("template")) {
               for (auto &tmp : Self->Templates->Tags) {
                  for (unsigned i=1; i < tmp.Attribs.size(); i++) {
                     if ((iequals("name", tmp.Attribs[i].name)) and (iequals(t, tmp.Attribs[i].Value))) {
                        if (!xmlSerialise(Self->Templates, tmp.Children[0].ID, XMF::INCLUDE_SIBLINGS|XMF::STRIP_CDATA, &content)) {
                           acDataXML(object, content.c_str());
                        }

                        break;
                     }
                  }
               }
            }
            else if (auto src = scan.attrib("object")) {
               OBJECTID objectid;
               if (!FindObject(src.c_str(), 0, FOF::SMART_NAMES, &objectid)) {
                  if ((objectid) and (valid_objectid(Self, objectid))) {
                     objXML *objxml;
                     if (!AccessObject(objectid, 3000, &objxml)) {
                        if (objxml->classID() IS CLASSID::XML) {
                           if (!xmlSerialise(objxml, 0, XMF::INCLUDE_SIBLINGS|XMF::STRIP_CDATA, &content)) {
                              acDataXML(object, content.c_str());
                           }
                        }
                        else log.warning("Cannot extract XML data from a non-XML object.");
                        ReleaseObject(objxml);
                     }
                  }
                  else log.warning("Invalid object reference '%s'", src);
               }
               else log.warning("Unable to find object '%s'", src);
            }
            else {
               if (scan.Children.empty()) continue;
               if (!xmlSerialise(XML, scan.Children.ID, XMF::INCLUDE_SIBLINGS|XMF::STRIP_CDATA, &content)) {
                  acDataXML(object, content.c_str());
               }
            }
         }
         else log.warning("Unsupported data type '%s'", type);
      }
   }

   // Feeds are applied to invoked objects, whereby the object's class name matches a feed.

   if ((!customised) and (Template)) {
      STRING content;
      if (!xmlSerialise(Self->Templates, Template->Children[0].ID, XMF::INCLUDE_SIBLINGS|XMF::STRIP_CDATA, &content)) {
         acDataXML(object, content);
         FreeResource(content);
      }
   }

   if (!InitObject(object)) {
      bc_vector escobj;

      if (Self->Invisible) acHide(object); // Hide the object if it's in an invisible section

      // Child tags are processed as normal, but are applied with respect to the object.  Any tags that reflect
      // document content are passed to the object as XML.

      if (!Tag.Children.empty()) {
         pf::Log log(__FUNCTION__);
         log.traceBranch("Processing child tags for object #%d.", object->UID);
         auto prevobject = Self->CurrentObject;
         Self->CurrentObject = object;
         parse_tags(Tag.Children, Flags & (~IPF::FILTER_ALL));
         Self->CurrentObject = prevobject;
      }

      if (&Children != &Tag.Children) {
         pf::Log log(__FUNCTION__);
         log.traceBranch("Processing further child tags for object #%d.", object->UID);
         auto prevobject = Self->CurrentObject;
         Self->CurrentObject = object;
         parse_tags(Children, Flags & (~IPF::FILTER_ALL));
         Self->CurrentObject = prevobject;
      }

      // The object can self-destruct in ClosingTag(), so check that it still exists before inserting it into the text stream.

      if (!CheckObjectExists(object->UID)) {
         if (Self->BkgdGfx) {
            auto &resource = Self->Resources.emplace_back(object->UID, RTD::OBJECT_UNLOAD);
            resource.class_id = class_id;
         }
         else {
            escobj.object_id = object->UID;
            escobj.class_id  = object->classID();
            escobj.in_line = false;
            if (Self->CurrentObject) escobj.owned = true;

            // By default objects are assumed to be in the background (thus not embedded as part of the text stream).
            // This section is intended to confirm the graphical state of the object.

            if (object->classID() IS CLASSID::VECTOR) {
               //if (layout->Layout & (LAYOUT_BACKGROUND|LAYOUT_FOREGROUND));
               //else if (layout->Layout & LAYOUT_EMBEDDED) escobj.Inline = true;
            }
            else escobj.in_line = true; // If the layout object is not present, the object is managing its own graphics and likely is embedded (button, combobox, checkbox etc are like this)

            m_stream->emplace(Index, escobj);

            if (Self->ObjectCache) {
               switch (object->classID()) {
                  // The following class types can be cached
                  case CLASSID::XML:
                  case CLASSID::FILE:
                  case CLASSID::CONFIG:
                  case CLASSID::COMPRESSION:
                  case CLASSID::SCRIPT: {
                     Self->Resources.emplace_back(object->UID, RTD::PERSISTENT_OBJECT);
                     break;
                  }

                  // The following class types use their own internal caching system

                  default:
                     log.warning("Cannot cache object of class type '%s'", object->Class->ClassName);
                  //case CLASSID::IMAGE:
                  //   auto &res = Self->Resources.emplace_back(object->UID, RTD::OBJECT_UNLOAD);
                     break;
               }
            }
            else {
               auto &res = Self->Resources.emplace_back(object->UID, RTD::OBJECT_UNLOAD);
               res.class_id = class_id;
            }

            // If the object is inline, we will allow whitespace to immediately follow the object.

            if (escobj.in_line) Self->NoWhitespace = false;

            // Add the object to the tab-list if it is in our list of classes that support keyboard input.

            static const CLASSID classes[] = { CLASSID::VECTOR };

            for (unsigned i=0; i < std::ssize(classes); i++) {
               if (classes[i] IS class_id) {
                  add_tabfocus(Self, TT::OBJECT, object->UID);
                  break;
               }
            }
         }
      }
      else log.trace("Object %d self-destructed.", object->UID);
   }
   else {
      FreeResource(object);
      log.warning("Failed to initialise object of class $%.8x", class_id);
   }
      */
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
      if ((Tag.Children.empty()) or (!Tag.Children[0].Attribs[0].Name.empty()) or (Tag.Children[0].Attribs[0].Value.empty())) {
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
               if ((!Self->DefaultScript) or (defaultscript)) Self->DefaultScript = script;
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
      if (!name.empty()) SetName(script, name.c_str());

      if (!src.empty()) script->setPath(src);
      else {
         std::string content = xml::GetContent(Tag);
         if (!content.empty()) script->setStatement(content);
      }

      if (!cachefile.empty()) script->setCacheFile(cachefile);

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

            if ((!Self->DefaultScript) or (defaultscript)) {
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

   if ((list->type IS bc_list::CUSTOM) and (!para.value.empty())) {
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
   }

   if (!step) {
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
   else if (!have_end) return;

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
      if (!loop_index_in_range(active_loop->index, active_loop->end, active_loop->step)) break;

      auto result = parse_tags(Tag.Children);
      if (Self->Error != ERR::Okay) break;
      if ((result & TRF::BREAK) != TRF::NIL) break;

      active_loop = loop.frame();
      if (!active_loop) break;

      active_loop->index += active_loop->step;
      active_loop->iteration++;

      if ((result & TRF::CONTINUE) != TRF::NIL) continue;
   }

   log.trace("insert_child:","Repeat loop ends.");
}

//********************************************************************************************************************
// <table columns="10%,90%" width="100" height="100" fill="rgb(128,128,128)">
//  <row><cell>Activate<brk/>This activates the object.</cell></row>
//  <row><cell span="2">Reset</cell></row>
// </table>
//
// <table width="100" height="100" fill="rgb(128,128,128)">
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

   if (!columns.empty()) { // The columns value, if supplied is arranged as a CSV list of column widths
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

   if (!Tag.Children.empty()) {
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

            if (!Self->EditDefs.contains(Tag.Attribs[i].Value)) {
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

   if (!cell.edit_def.empty()) edit_recurse++;

   // Edit sections enforce preformatting, which means that all whitespace entered by the user will be taken
   // into account.  The following check sets FSO::PREFORMAT if it hasn't been set already.

   auto cell_index = m_index;

   if (!Tag.Children.empty()) {
      Self->NoWhitespace = true; // Reset whitespace flag: false allows whitespace at the start of the cell, true prevents whitespace

      if ((!cell.edit_def.empty()) and ((m_style.options & FSO::PREFORMAT) IS FSO::NIL)) {
         new_style.options |= FSO::PREFORMAT;
      }

      // Cell content is managed in an internal stream

      parser parse(Self, cell.stream);

      parse.m_in_template = m_in_template;
      parse.m_inject_tag  = m_inject_tag;
      parse.m_paragraph_depth++;
      parse.parse_tags_with_style(Tag.Children, new_style);
      parse.m_paragraph_depth--;
   }

   auto &stream_cell = m_stream->emplace(m_index, cell);

   m_table_stack.top().row_col += stream_cell.col_span;

   if (!stream_cell.edit_def.empty()) {
      // Links are added to the list of tabbable points

      int tab = add_tabfocus(Self, TT::EDIT, stream_cell.cell_id);
      if (select) Self->FocusIndex = tab;
   }

   if (!stream_cell.edit_def.empty()) edit_recurse--;
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

   if ((!event.empty()) and (!function_name.empty())) {
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
