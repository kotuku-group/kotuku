
//********************************************************************************************************************
// Determine whether an attribute on a given tag should be treated as an XQuery expression (evaluated as a whole) or
// as an attribute value template (AVT) with embedded {...} expression fragments.
//
//   if@test, elseif@test, while@test       -> XQuery expression attributes
//   print@select, parse@select             -> XQuery expression attributes
//   print@value                            -> AVT

static bool has_avt_markers(std::string_view Value)
{
   return (Value.find('{') != std::string_view::npos) or (Value.find('}') != std::string_view::npos);
}

static bool is_expression_attrib(uint32_t TagHash, std::string_view AttribName)
{
   auto name = AttribName;
   if ((not name.empty()) and (name.front() IS '$')) name.remove_prefix(1);

   if ("test" IS name) {
      return (TagHash IS HASH_if) or (TagHash IS HASH_elseif) or (TagHash IS HASH_while);
   }
   else if ("select" IS name) {
      return (TagHash IS HASH_print) or (TagHash IS HASH_parse) or (TagHash IS HASH_data) or
         (TagHash IS HASH_let) or (TagHash IS HASH_for_each);
   }
   return false;
}

inline bool is_avt_attrib(uint32_t TagHash, std::string_view AttribName, std::string_view AttribValue)
{
   return (AttribValue.find('{') != std::string_view::npos) or (AttribValue.find('}') != std::string_view::npos);
}

//********************************************************************************************************************
// XQuery evaluation helpers
//
// These helpers are intentionally thin wrappers around the XQuery object class so that parser-visible behaviour mirrors
// a standard xml:evaluate() call.  The XML context is optional; when absent, the evaluator still supports literal and
// function-based expressions.  Errors from compilation or evaluation propagate to the caller via the returned ERR and
// the document Self->Error field.

enum class XQEval { STRING, BOOLEAN };

static constexpr std::string_view XQ_KOTUKU_NAMESPACE_URI   = "http://www.kotuku.dev/namespaces/kotuku";
static constexpr std::string_view XQ_OBJECT_EXISTS_FUNCTION = "kt:object-exists";
static constexpr std::string_view XQ_OBJECT_ID_FUNCTION     = "kt:object-id";
static constexpr std::string_view XQ_SELF_ID_FUNCTION       = "kt:self-id";
static constexpr std::string_view XQ_UID_FUNCTION           = "kt:uid";
static constexpr std::string_view XQ_PLATFORM_FUNCTION      = "kt:platform";
static constexpr std::string_view XQ_FIELD_FUNCTION         = "kt:field";
static constexpr std::string_view XQ_KEY_FUNCTION           = "kt:key";
static constexpr std::string_view XQ_TEMPLATE_CONTENT_FUNCTION = "kt:template-content";

static std::string xq_build_eval_expression(std::string_view Expression, XQEval Mode)
{
   std::string eval_expression;
   bool uses_kt_prefix = Expression.find("kt:") != std::string_view::npos;
   bool declares_kt_namespace = Expression.find("declare namespace kt") != std::string_view::npos;

   if ((uses_kt_prefix) and (not declares_kt_namespace)) {
      eval_expression.reserve(Expression.size() + XQ_KOTUKU_NAMESPACE_URI.size() + 40);
      eval_expression += "declare namespace kt = '";
      eval_expression += XQ_KOTUKU_NAMESPACE_URI;
      eval_expression += "'; ";
   }

   if (Mode IS XQEval::BOOLEAN) {
      eval_expression += "boolean(";
      eval_expression += Expression;
      eval_expression += ")";
   }
   else eval_expression += Expression;

   return eval_expression;
}

static inline void map_append_value(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key,
   const XPathValue &Value)
{
   XPathMapEntry entry;
   entry.key.assign(Key);
   entry.value.items.push_back(Value);
   Storage->entries.push_back(std::move(entry));
}

static bool value_is_empty_sequence(const XPathValue &Value)
{
   if (Value.Type != XPVT::NodeSet) return false;

   return Value.node_set.empty() and
      Value.node_set_attributes.empty() and
      Value.node_set_string_values.empty() and
      (not Value.node_set_string_override.has_value()) and
      Value.node_set_composite_values.empty();
}

// A node-set may carry parallel attribute slots, but Stage 6 only treats the value as
// attribute-backed if at least one slot actually points to a live attribute.

static bool nodeset_has_real_attributes(const XPathValue &Value)
{
   for (auto *attribute : Value.node_set_attributes) {
      if (attribute) return true;
   }
   return false;
}

// Likewise for composite entries: null placeholders are tolerated, but real composite
// items mean the sequence is not serialisable as plain XML markup.

static bool nodeset_has_real_composites(const XPathValue &Value)
{
   for (const auto &composite : Value.node_set_composite_values) {
      if (composite) return true;
   }
   return false;
}

static parser::xq_xml_owner xq_adopt_xml(objXML *XML)
{
   return parser::xq_xml_owner(XML, [](objXML *OwnedXML) {
      if (OwnedXML) FreeResource(OwnedXML);
   });
}

static void xq_clone_tag_tree(const XTag &Source, XTag &Target, int ParentID, int &NextID)
{
   Target = XTag(NextID++, Source.LineNo, Source.Attribs);
   Target.ParentID    = ParentID;
   Target.Flags       = Source.Flags;
   Target.NamespaceID = Source.NamespaceID;
   Target.Reserved    = Source.Reserved;
   Target.Children.clear();
   Target.Children.reserve(Source.Children.size());

   for (const auto &child : Source.Children) {
      Target.Children.emplace_back();
      xq_clone_tag_tree(child, Target.Children.back(), Target.ID, NextID);
   }
}

static size_t find_attrib_index(const XTag *Node, const XMLAttrib *Attrib)
{
   if ((not Node) or (not Attrib)) return std::numeric_limits<size_t>::max();

   for (size_t index = 0; index < Node->Attribs.size(); ++index) {
      if (&Node->Attribs[index] IS Attrib) return index;
   }

   return std::numeric_limits<size_t>::max();
}

static void xq_append_node_text(const XTag *Node, std::string &Output)
{
   if (not Node) return;

   if (Node->isContent()) {
      if ((not Node->Attribs.empty()) and Node->Attribs[0].isContent()) {
         Output.append(Node->Attribs[0].Value);
      }

      for (const auto &child : Node->Children) {
         xq_append_node_text(&child, Output);
      }

      return;
   }

   for (const auto &child : Node->Children) {
      if (child.Attribs.empty()) continue;

      if (child.Attribs[0].isContent()) Output.append(child.Attribs[0].Value);
      else xq_append_node_text(&child, Output);
   }
}

static std::string xq_node_string_value(const XTag *Node)
{
   std::string value;
   xq_append_node_text(Node, value);
   return value;
}

// Store a runtime value into a map entry while preserving the XQuery rule that an empty
// sequence is represented by an empty slot rather than a concrete item.

static inline void xq_map_append_runtime_value(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key,
   const XPathValue &Value)
{
   XPathMapEntry entry;
   entry.key.assign(Key);
   if (not value_is_empty_sequence(Value)) entry.value.items.push_back(Value);
   Storage->entries.push_back(std::move(entry));
}

static inline void xq_map_append_string(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key,
   std::string_view Value)
{
   XPathValue result(XPVT::String);
   result.StringValue.assign(Value);
   map_append_value(Storage, Key, result);
}

static inline void xq_map_append_number(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key, double Value)
{
   XPathValue result(XPVT::Number);
   result.NumberValue = Value;
   map_append_value(Storage, Key, result);
}

static std::string xq_format_font_size(const bc_font &Style)
{
   auto append_suffix = [](double Value, std::string_view Suffix) {
      char buffer[32];
      auto [ptr, error] = std::to_chars(buffer, buffer + sizeof(buffer), Value, std::chars_format::fixed, 3);
      if (error != std::errc()) return std::to_string(Value) + std::string(Suffix);

      std::string result(buffer, ptr);
      auto decimal = result.find('.');
      if (decimal != std::string::npos) {
         while ((not result.empty()) and (result.back() IS '0')) result.pop_back();
         if ((not result.empty()) and (result.back() IS '.')) result.pop_back();
      }
      result += Suffix;
      return result;
   };

   switch(Style.req_size.type) {
      case DU::PIXEL:     return append_suffix(int(Style.req_size.value), "px");
      case DU::FONT_SIZE: return append_suffix(Style.req_size.value, "em");
      case DU::SCALED:    return append_suffix(Style.req_size.value * 100.0, "%");
      default:            return std::to_string(DEFAULT_FONTSIZE) + "px";
   }
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

static XPathValue xq_template_args_to_map(const extDocument *Self)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   if (Self->TemplateArgs.empty()) return result;

   auto args = Self->TemplateArgs.back();
   if (not args.Tag) return result;

   auto &attribs = args.Attribs ? *args.Attribs : args.Tag->Attribs;
   for (int i=1; i < std::ssize(attribs); i++) {
      auto name = std::string_view(attribs[i].Name);
      if ((not name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
      if (name.empty()) continue;
      xq_map_append_string(result.map_storage, name, attribs[i].Value);
   }

   return result;
}

static XPathValue xq_meta_to_map(const extDocument *Self)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   if (not Self->Title.empty())       xq_map_append_string(result.map_storage, "title", Self->Title);
   if (not Self->Author.empty())      xq_map_append_string(result.map_storage, "author", Self->Author);
   if (not Self->Description.empty()) xq_map_append_string(result.map_storage, "description", Self->Description);
   if (not Self->Copyright.empty())   xq_map_append_string(result.map_storage, "copyright", Self->Copyright);
   if (not Self->Keywords.empty())    xq_map_append_string(result.map_storage, "keywords", Self->Keywords);
   if (not Self->Path.empty()) xq_map_append_string(result.map_storage, "path", Self->Path);
   xq_map_append_number(result.map_storage, "line-no", double(Self->Segments.size()));

   if (Self->PageTag) {
      if (auto current_page = Self->PageTag->attrib("name")) {
         xq_map_append_string(result.map_storage, "current-page", *current_page);
      }

      if (auto next_page = Self->PageTag->attrib("next-page")) {
         // Returns the name of the next page to link to (user defined, not the next page in the document)
         xq_map_append_string(result.map_storage, "next-page", *next_page);
      }

      if (auto prev_page = Self->PageTag->attrib("prev-page")) {
         // Returns the name of the previous page (user defined, not the previous page in the document)
         xq_map_append_string(result.map_storage, "prev-page", *prev_page);
      }
   }

   return result;
}

static XPathValue xq_layout_to_map(const extDocument *Self)
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

static XPathValue xq_loop_to_map(const parser *Parser)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   auto frame = Parser->active_loop();
   if (not frame) return result;

   xq_map_append_number(result.map_storage, "index", frame->index);
   xq_map_append_number(result.map_storage, "iteration", frame->iteration);
   xq_map_append_number(result.map_storage, "start", frame->start);
   xq_map_append_number(result.map_storage, "step", frame->step);
   if (frame->count_known) xq_map_append_number(result.map_storage, "count", frame->count);
   if (frame->end_known) xq_map_append_number(result.map_storage, "end", frame->end);
   return result;
}

static XPathValue xq_style_to_map(const parser *Parser)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();

   xq_map_append_string(result.map_storage, "font-face", Parser->m_style.face);
   xq_map_append_string(result.map_storage, "font-style", Parser->m_style.style);
   xq_map_append_string(result.map_storage, "font-fill", Parser->m_style.fill);
   xq_map_append_string(result.map_storage, "font-size", xq_format_font_size(Parser->m_style));
   return result;
}

//********************************************************************************************************************
// Convert the top-level roots of an XML tree into the node-sequence form expected by the XQuery runtime.  This is
// used for $doc.

static XPathValue xq_xml_roots_to_nodeset(objXML *XML)
{
   XPathValue result(XPVT::NodeSet);
   if (not XML) return result;

   for (auto &tag : XML->Tags) {
      if (tag.isTag()) result.node_set.push_back(&tag);
   }

   return result;
}

//********************************************************************************************************************
// $page always refers to the active source <page> node, regardless of which child tag is currently being parsed.

static XPathValue xq_page_to_nodeset(parser *Parser)
{
   XPathValue result(XPVT::NodeSet);
   if ((not Parser) or (not Parser->Self) or (not Parser->Self->PageTag)) return result;

   result.node_set.push_back((XTag *)Parser->Self->PageTag);
   return result;
}

//********************************************************************************************************************
// Expose the current template caller content as a concrete node sequence so template attributes can consume the same
// injected content that <inject/> would parse directly.

static XPathValue xq_template_content_to_nodeset(parser *Parser)
{
   XPathValue result(XPVT::NodeSet);
   if ((not Parser) or (not Parser->m_inject_tag)) return result;

   result.preserve_node_order = true;
   result.node_set.reserve(Parser->m_inject_tag[0].size());
   for (auto &tag : Parser->m_inject_tag[0]) {
      result.node_set.push_back((XTag *)&tag);
   }

   return result;
}

//********************************************************************************************************************
// Materialise the parser's lexical bindings as an XQuery map so $state?name can read the exact raw values produced
// by <let>, including node sequences.

static XPathValue xq_state_to_map(const parser *Parser)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();
   if (not Parser) return result;

   for (const auto &entry : Parser->m_state_values) {
      xq_map_append_runtime_value(result.map_storage, entry.first, entry.second.value);
   }

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

//********************************************************************************************************************

static ERR xq_resolve_runtime_scope(objXQuery *Query, std::string_view Name, XPathValue *Result, APTR Meta)
{
   auto Self = (extDocument *)CurrentContext();

   auto name_hash = strhash(Name);
   if (name_hash IS HASH_params) {
      *Result = xq_keyvalue_to_map(Self->Params);
   }
   else if (name_hash IS HASH_args) {
      *Result = xq_template_args_to_map(Self);
   }
   else if (name_hash IS HASH_vars) {
      *Result = xq_keyvalue_to_map(Self->Vars);
   }
   else if (name_hash IS HASH_meta) {
      *Result = xq_meta_to_map(Self);
   }
   else if (name_hash IS HASH_layout) {
      *Result = xq_layout_to_map(Self);
   }
   else if (name_hash IS HASH_loop) {
      *Result = xq_loop_to_map((parser *)Meta);
   }
   else if (name_hash IS HASH_doc) {
      auto p = (parser *)Meta;
      *Result = p ? xq_xml_roots_to_nodeset(p->m_doc_xml) : XPathValue(XPVT::NodeSet);
   }
   else if (name_hash IS HASH_page) {
      *Result = xq_page_to_nodeset((parser *)Meta);
   }
   else if (name_hash IS HASH_state) {
      *Result = xq_state_to_map((parser *)Meta);
   }
   else if (name_hash IS HASH_style) {
      *Result = xq_style_to_map((parser *)Meta);
   }
   else return ERR::Search;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR xq_document_object_exists(objXQuery *, std::string_view, const std::vector<XPathValue> &Input,
   XPathValue &Result, APTR Meta)
{
   kt::Log log(__FUNCTION__);

   auto Parser = (parser *)Meta;
   Result = XPathValue(XPVT::Boolean);
   Result.reset();

   if ((not Parser) or Input.empty()) return ERR::Args;

   std::string object_name;
   auto &value = Input[0];
   if (value.Type IS XPVT::String) object_name = value.StringValue;
   else if ((value.Type IS XPVT::Number) or (value.Type IS XPVT::Boolean)) object_name = std::to_string(value.NumberValue);
   else if (value.node_set_string_override.has_value()) object_name = *value.node_set_string_override;
   else if (not value.node_set_string_values.empty()) object_name = value.node_set_string_values[0];
   else return log.warning(ERR::Args);

   OBJECTID object_id = 0;
   bool exists = false;
   if ((not object_name.empty()) and
      (FindObject(object_name.c_str(), CLASSID::NIL, &object_id) IS ERR::Okay)) {
      exists = valid_objectid(Parser->Self, object_id) ? true : false;
   }

   Result.Type = XPVT::Boolean;
   Result.NumberValue = exists ? 1.0 : 0.0;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR xq_value_to_string(const XPathValue &Value, std::string &Result)
{
   Result.clear();

   if (Value.Type IS XPVT::NodeSet) {
      if (value_is_empty_sequence(Value)) return ERR::Okay;

      if (Value.node_set_string_override.has_value()) {
         Result = *Value.node_set_string_override;
         return ERR::Okay;
      }

      if (not Value.node_set_string_values.empty()) {
         if (Value.node_set_string_values.size() IS 1) {
            Result = Value.node_set_string_values[0];
            return ERR::Okay;
         }

         for (size_t index = 0; index < Value.node_set_string_values.size(); ++index) {
            if (index > 0) Result.push_back(':');
            Result += Value.node_set_string_values[index];
         }
         return ERR::Okay;
      }

      if (not Value.node_set_attributes.empty()) {
         auto count = std::max(Value.node_set_attributes.size(), Value.node_set.size());
         for (size_t index = 0; index < count; ++index) {
            if (index > 0) Result.push_back(':');
            if ((index < Value.node_set_attributes.size()) and Value.node_set_attributes[index]) {
               Result += Value.node_set_attributes[index]->Value;
            }
            else if ((index < Value.node_set.size()) and Value.node_set[index]) {
               Result += xq_node_string_value(Value.node_set[index]);
            }
         }
         return ERR::Okay;
      }

      if (not Value.node_set.empty()) {
         if (Value.node_set.size() IS 1) {
            Result = xq_node_string_value(Value.node_set[0]);
            return ERR::Okay;
         }

         for (size_t index = 0; index < Value.node_set.size(); ++index) {
            if (index > 0) Result.push_back(':');
            Result += xq_node_string_value(Value.node_set[index]);
         }
         return ERR::Okay;
      }
   }

   if (Value.Type IS XPVT::String) {
      Result = Value.StringValue;
      return ERR::Okay;
   }
   else if (Value.Type IS XPVT::Number) {
      Result = std::to_string(Value.NumberValue);
      while ((not Result.empty()) and (Result.back() IS '0')) Result.pop_back();
      if ((not Result.empty()) and (Result.back() IS '.')) Result.pop_back();
      return ERR::Okay;
   }
   else if (Value.Type IS XPVT::Boolean) {
      Result = (Value.NumberValue != 0.0) ? "true" : "false";
      return ERR::Okay;
   }
   else if (Value.node_set_string_override.has_value()) {
      Result = *Value.node_set_string_override;
      return ERR::Okay;
   }
   else if (not Value.node_set_string_values.empty()) {
      Result = Value.node_set_string_values[0];
      return ERR::Okay;
   }

   return ERR::Args;
}

static std::string xq_format_object_id(OBJECTID ObjectID)
{
   return "#" + std::to_string(ObjectID);
}

static ERR xq_resolve_document_object(const parser *Parser, std::string_view ObjectName, OBJECTID &ObjectID)
{
   ObjectID = 0;
   if ((not Parser) or ObjectName.empty()) return ERR::Args;

   if (iequals(ObjectName, "self")) {
      if (auto context = CurrentContext()) ObjectID = context->UID;
      else ObjectID = Parser->Self->UID;
   }
   else if (ObjectName.front() IS '#') {
      long long object_id = 0;
      auto numeric_id = ObjectName.substr(1);
      auto parsed = std::from_chars(numeric_id.data(), numeric_id.data() + numeric_id.size(), object_id);
      if ((parsed.ec != std::errc()) or (parsed.ptr != numeric_id.data() + numeric_id.size())) return ERR::InvalidData;
      ObjectID = OBJECTID(object_id);
   }
   else {
      auto object_name = std::string(ObjectName);
      if (FindObject(object_name.c_str(), CLASSID::NIL, &ObjectID) != ERR::Okay) {
         return ERR::NoMatchingObject;
      }
   }

   if (not ObjectID) return ERR::NoMatchingObject;
   if (ObjectID IS Parser->Self->UID) return ERR::Okay;
   if (auto context = CurrentContext(); (context) and (ObjectID IS context->UID)) return ERR::Okay;
   if (not valid_objectid(Parser->Self, ObjectID)) return ERR::AccessObject;
   return ERR::Okay;
}

static ERR xq_get_object_key(OBJECTPTR Object, std::string_view Key, std::string &Result)
{
   if (CheckAction(Object, AC::GetKey) != ERR::Okay) return ERR::Search;

   auto key = std::string(Key);
   std::string buffer(4096, 0);
   while (true) {
      buffer.back() = 0;
      struct acGetKey var = { key, &buffer };
      if (Action(AC::GetKey, Object, &var) != ERR::Okay) return ERR::Search;
      if (buffer.back()) {
         buffer.resize(buffer.size() * 2);
         continue;
      }

      Result.assign(buffer.c_str());
      return ERR::Okay;
   }
}

static ERR xq_get_object_field(OBJECTID ObjectID, std::string_view FieldName, std::string &Result)
{
   OBJECTPTR object = nullptr;
   auto access = AccessObject(ObjectID, 2000, &object);
   if (access != ERR::Okay) return access;

   auto cleanup = kt::Defer([&]() {
      if (object) ReleaseObject(object);
   });

   OBJECTPTR target = nullptr;
   if (auto classfield = FindField(object, strihash(FieldName), &target)) {
      return target->get(classfield->FieldID, Result);
   }

   return xq_get_object_key(object, FieldName, Result);
}

static ERR xq_document_object_id(objXQuery *, std::string_view, const std::vector<XPathValue> &Input,
   XPathValue &Result, APTR Meta)
{
   auto Parser = (parser *)Meta;
   if (Input.empty()) return ERR::Args;

   std::string object_name;
   if (auto err = xq_value_to_string(Input[0], object_name); err != ERR::Okay) return err;

   OBJECTID object_id = 0;
   if (auto err = xq_resolve_document_object(Parser, object_name, object_id); err != ERR::Okay) return err;

   Result = XPathValue(XPVT::String);
   Result.StringValue = xq_format_object_id(object_id);
   return ERR::Okay;
}

static ERR xq_document_self_id(objXQuery *, std::string_view, const std::vector<XPathValue> &,
   XPathValue &Result, APTR Meta)
{
   auto Parser = (parser *)Meta;

   OBJECTID object_id = 0;
   if (auto context = CurrentContext()) object_id = context->UID;
   else object_id = Parser->Self->UID;

   Result = XPathValue(XPVT::String);
   Result.StringValue = xq_format_object_id(object_id);
   return ERR::Okay;
}

static ERR xq_document_uid(objXQuery *, std::string_view, const std::vector<XPathValue> &,
   XPathValue &Result, APTR Meta)
{
   auto Parser = (parser *)Meta;

   if (Parser->Self->RuntimeUID.empty()) {
      Parser->Self->RuntimeUID = std::to_string(AllocateID(IDTYPE::GLOBAL));
   }

   Result = XPathValue(XPVT::String);
   Result.StringValue = Parser->Self->RuntimeUID;
   return ERR::Okay;
}

static ERR xq_document_platform(objXQuery *, std::string_view, const std::vector<XPathValue> &,
   XPathValue &Result, APTR)
{
   Result = XPathValue(XPVT::String);
   Result.StringValue = GetSystemState()->Platform;
   return ERR::Okay;
}

static ERR xq_document_field(objXQuery *, std::string_view, const std::vector<XPathValue> &Input,
   XPathValue &Result, APTR Meta)
{
   auto Parser = (parser *)Meta;
   if (Input.size() < 2) return ERR::Args;

   std::string object_name, field_name, field_value;
   if (auto err = xq_value_to_string(Input[0], object_name); err != ERR::Okay) return err;
   if (auto err = xq_value_to_string(Input[1], field_name); err != ERR::Okay) return err;

   OBJECTID object_id = 0;
   if (auto err = xq_resolve_document_object(Parser, object_name, object_id); err != ERR::Okay) return err;
   if (auto err = xq_get_object_field(object_id, field_name, field_value); err != ERR::Okay) return err;

   Result = XPathValue(XPVT::String);
   Result.StringValue = field_value;
   return ERR::Okay;
}

static ERR xq_document_key(objXQuery *, std::string_view, const std::vector<XPathValue> &Input,
   XPathValue &Result, APTR Meta)
{
   auto Parser = (parser *)Meta;
   if (Input.size() < 2) return ERR::Args;

   std::string object_name, key_name, key_value;
   if (auto err = xq_value_to_string(Input[0], object_name); err != ERR::Okay) return err;
   if (auto err = xq_value_to_string(Input[1], key_name); err != ERR::Okay) return err;

   OBJECTID object_id = 0;
   if (auto err = xq_resolve_document_object(Parser, object_name, object_id); err != ERR::Okay) return err;

   OBJECTPTR object = nullptr;
   auto access = AccessObject(object_id, 2000, &object);
   if (access != ERR::Okay) return access;

   auto cleanup = kt::Defer([&]() {
      if (object) ReleaseObject(object);
   });

   if (auto err = xq_get_object_key(object, key_name, key_value); err != ERR::Okay) return err;

   Result = XPathValue(XPVT::String);
   Result.StringValue = key_value;
   return ERR::Okay;
}

static ERR xq_document_template_content(objXQuery *, std::string_view, const std::vector<XPathValue> &Input,
   XPathValue &Result, APTR Meta)
{
   if (not Input.empty()) return ERR::Args;

   Result = xq_template_content_to_nodeset((parser *)Meta);
   return ERR::Okay;
}

//********************************************************************************************************************
// Shared setup for all document-side XQuery surfaces.  Compiled XQuery objects are cached for the lifetime of the
// current parser so repeated AVT/test/select expressions can reuse the compiled AST rather than rebuilding it on
// every evaluation.

static ERR xq_prepare_query(parser *Parser, std::string_view Expression, XQEval Mode, objXQuery *&Query)
{
   auto Self = Parser->Self;
   Query = nullptr;

   auto eval_expression = xq_build_eval_expression(Expression, Mode);

   if (auto cached = Parser->m_xq_query_cache.find(eval_expression); cached != Parser->m_xq_query_cache.end()) {
      Query = cached->second;
      return ERR::Okay;
   }

   objXQuery *query = nullptr;
   if (NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&query) != ERR::Okay) {
      Self->Error = ERR::NewObject;
      return ERR::NewObject;
   }

   if (query->init() != ERR::Okay) {
      Self->Error = ERR::Init;
      FreeResource(query);
      return ERR::Init;
   }

   query->setResolveVariable(C_FUNCTION(xq_resolve_runtime_scope, Parser));
   query->registerFunction(XQ_OBJECT_EXISTS_FUNCTION.data(), C_FUNCTION(xq_document_object_exists, Parser));
   query->registerFunction(XQ_OBJECT_ID_FUNCTION.data(), C_FUNCTION(xq_document_object_id, Parser));
   query->registerFunction(XQ_SELF_ID_FUNCTION.data(), C_FUNCTION(xq_document_self_id, Parser));
   query->registerFunction(XQ_UID_FUNCTION.data(), C_FUNCTION(xq_document_uid, Parser));
   query->registerFunction(XQ_PLATFORM_FUNCTION.data(), C_FUNCTION(xq_document_platform, Parser));
   query->registerFunction(XQ_FIELD_FUNCTION.data(), C_FUNCTION(xq_document_field, Parser));
   query->registerFunction(XQ_KEY_FUNCTION.data(), C_FUNCTION(xq_document_key, Parser));
   query->registerFunction(XQ_TEMPLATE_CONTENT_FUNCTION.data(), C_FUNCTION(xq_document_template_content, Parser));

   if (query->setStatement(eval_expression) != ERR::Okay) {
      Self->Error = ERR::SetField;
      FreeResource(query);
      return ERR::SetField;
   }

   Parser->m_xq_query_cache.emplace(std::move(eval_expression), query);
   Query = query;
   return ERR::Okay;
}

//********************************************************************************************************************
// Execute a prepared XQuery expression against the effective parser context.  The XML and tag defaults come from the
// caller, but <for-each> may temporarily override them via m_xq_context_stack so relative paths evaluate against
// the current item.

static ERR xq_execute_query(parser *Parser, objXML *XMLContext, const XTag *ContextTag, std::string_view Expression,
   XQEval Mode, std::string *OutString, bool *OutBoolean, XPathValue *OutValue, bool *OutHasValue)
{
   kt::Log log(__FUNCTION__);

   objXQuery *query = nullptr;
   XPathValue raw_value(XPVT::String);
   bool have_raw_value = false;

   if (OutString) OutString->clear();
   if (OutBoolean) *OutBoolean = false;
   if (OutHasValue) *OutHasValue = false;

   if (Expression.empty()) return ERR::Okay;

   if (auto err = xq_prepare_query(Parser, Expression, Mode, query); err != ERR::Okay) {
      Parser->log_error(ContextTag, err, "doc.xquery-prepare-failed",
         "Failed to prepare XQuery expression \"{}\".", Expression);
      return err;
   }

   objXML *effective_xml = XMLContext;
   const XTag *effective_tag = ContextTag;
   if (auto xq_context = Parser->active_xq_context()) {
      if (xq_context->xml) effective_xml = xq_context->xml;
      if (xq_context->node) effective_tag = xq_context->node;
   }

   if (auto err = query->evaluate(effective_xml, effective_tag ? effective_tag->ID : 0, XEF::NIL); err != ERR::Okay) {
      std::string_view msg;
      if (query->get(FID_ErrorMsg, msg) IS ERR::Okay) {
         Parser->log_error(effective_tag, err, "doc.xquery-evaluation-failed",
            "XQuery error evaluating \"{}\": {}.", Expression, msg.empty() ? std::string_view("(none)") : msg);
      }
      else Parser->log_error(effective_tag, err, "doc.xquery-evaluation-failed",
         "XQuery error evaluating \"{}\".", Expression);
      return err;
   }

   XPathValue *query_value = nullptr;
   if ((query->get(FID_Result, query_value) IS ERR::Okay) and query_value) {
      raw_value = *query_value;
      have_raw_value = true;
      if (OutValue) *OutValue = raw_value;
      if (OutHasValue) *OutHasValue = true;
   }

   if (have_raw_value) {
      if (OutString) {
         (void)xq_value_to_string(raw_value, *OutString);
      }

      if (OutBoolean) {
         if (raw_value.Type IS XPVT::Boolean) *OutBoolean = raw_value.NumberValue != 0.0;
         else {
            std::string boolean_text;
            if ((xq_value_to_string(raw_value, boolean_text) IS ERR::Okay) and (iequals("true", boolean_text))) {
               *OutBoolean = true;
            }
         }
      }
   }
   else {
      std::string_view result;
      if (query->get(FID_ResultString, result) IS ERR::Okay) {
         if ((OutString) and not result.empty()) OutString->assign(result);
         if ((OutBoolean) and not result.empty() and iequals("true", result)) *OutBoolean = true;
      }
   }

   return ERR::Okay;
}

static ERR xq_eval_helper(parser *Parser, objXML *XMLContext, const XTag *ContextTag, std::string_view Expression,
   XQEval Mode, std::string &OutString, bool &OutBoolean)
{
   return xq_execute_query(Parser, XMLContext, ContextTag, Expression, Mode, &OutString, &OutBoolean, nullptr, nullptr);
}

//********************************************************************************************************************
// Variant of the helper above that preserves the raw XPathValue result for features such as <data>, <let>,
// <for-each>, and parse@select.  If the query only exposes a string value, callers still receive a string-typed
// XPathValue.

static ERR xq_eval_value_helper(parser *Parser, objXML *XMLContext, const XTag *ContextTag, std::string_view Expression,
   XPathValue &OutValue, std::string &OutString, bool &OutHasValue)
{
   bool unused_boolean = false;
   OutValue = XPathValue(XPVT::String);
   OutValue.StringValue.clear();

   auto err = xq_execute_query(Parser, XMLContext, ContextTag, Expression, XQEval::STRING, &OutString,
      &unused_boolean, &OutValue, &OutHasValue);
   if (err != ERR::Okay) return err;

   if (not OutHasValue) {
      OutValue = XPathValue(XPVT::String);
      OutValue.StringValue = OutString;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Attribute lookup.  Attribute names may still carry a leading $ after preprocessing, so the lookup normalises
// that away.

static const std::string * find_attrib(const tag_view &Tag, std::string_view Name)
{
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto attrib_name = std::string_view(Tag.Attribs[i].Name);
      if ((not attrib_name.empty()) and (attrib_name.front() IS '$')) attrib_name.remove_prefix(1);
      if (attrib_name IS Name) return &Tag.Attribs[i].Value;
   }
   return nullptr;
}

//********************************************************************************************************************
// Minimal XML escaping helper for serialising XQuery node sequences back into markup.

static void append_xml_escaped(std::ostringstream &Buffer, std::string_view Text, bool AttributeMode)
{
   for (char ch : Text) {
      switch (ch) {
         case '&': Buffer << "&amp;"; break;
         case '<': Buffer << "&lt;"; break;
         case '>': Buffer << "&gt;"; break;
         case '"':
            if (AttributeMode) Buffer << "&quot;";
            else Buffer << '"';
            break;
         case '\'':
            if (AttributeMode) Buffer << "&apos;";
            else Buffer << '\'';
            break;
         default:
            Buffer << ch;
            break;
      }
   }
}

//********************************************************************************************************************
// Serialise an XTag subtree into XML fragment form.  This is used when <data> or <parse> receives a
// node-sequence result and needs reparsing through the document parser rather than ordinary XQuery string-value rules.

static void serialise_tag_fragment(const XTag &Tag, std::ostringstream &Buffer)
{
   if (Tag.Attribs.empty()) return;

   if (Tag.Attribs[0].isContent()) {
      if (not Tag.Attribs[0].Value.empty()) append_xml_escaped(Buffer, Tag.Attribs[0].Value, false);
      return;
   }

   Buffer << '<';
   append_xml_escaped(Buffer, Tag.Attribs[0].Name, false);

   for (size_t index = 1; index < Tag.Attribs.size(); ++index) {
      auto &attrib = Tag.Attribs[index];
      Buffer << ' ';
      append_xml_escaped(Buffer, attrib.Name, false);
      Buffer << "=\"";
      append_xml_escaped(Buffer, attrib.Value, true);
      Buffer << '"';
   }

   if ((Tag.Flags & XTF::INSTRUCTION) != XTF::NIL) {
      Buffer << "?>";
      return;
   }

   if ((Tag.Flags & XTF::NOTATION) != XTF::NIL) {
      Buffer << '>';
      return;
   }

   if (Tag.Children.empty()) {
      Buffer << "/>";
      return;
   }

   Buffer << '>';
   for (auto &child : Tag.Children) {
      serialise_tag_fragment(child, Buffer);
   }

   Buffer << "</";
   append_xml_escaped(Buffer, Tag.Attribs[0].Name, false);
   Buffer << '>';
}

//********************************************************************************************************************
// Serialise a tag view using the original child tree but the currently active attribute set, so AVT-expanded
// attributes can be emitted without constructing a duplicate XTag subtree.

static void serialise_tag_fragment(const tag_view &Tag, std::ostringstream &Buffer)
{
   if (Tag.Attribs.empty()) return;

   if (Tag.Attribs[0].isContent()) {
      if (not Tag.Attribs[0].Value.empty()) append_xml_escaped(Buffer, Tag.Attribs[0].Value, false);
      return;
   }

   Buffer << '<';
   append_xml_escaped(Buffer, Tag.Attribs[0].Name, false);

   for (size_t index = 1; index < Tag.Attribs.size(); ++index) {
      auto &attrib = Tag.Attribs[index];
      Buffer << ' ';
      append_xml_escaped(Buffer, attrib.Name, false);
      Buffer << "=\"";
      append_xml_escaped(Buffer, attrib.Value, true);
      Buffer << '"';
   }

   if ((Tag.Flags & XTF::INSTRUCTION) != XTF::NIL) {
      Buffer << "?>";
      return;
   }

   if ((Tag.Flags & XTF::NOTATION) != XTF::NIL) {
      Buffer << '>';
      return;
   }

   if (Tag.Children.empty()) {
      Buffer << "/>";
      return;
   }

   Buffer << '>';

   for (auto &child : Tag.Children) {
      serialise_tag_fragment(child, Buffer);
   }

   Buffer << "</";
   append_xml_escaped(Buffer, Tag.Attribs[0].Name, false);
   Buffer << '>';
}

//********************************************************************************************************************
// Convert a pure node sequence into reparsable XML fragment text.  Attribute-backed or composite sequences are
// rejected here because we only accept concrete markup.

static ERR xq_value_to_xml_fragment(const XPathValue &Value, std::string &OutXml)
{
   OutXml.clear();

   if (Value.Type != XPVT::NodeSet) return ERR::Args;
   if (Value.node_set.empty()) return ERR::Args;
   if (nodeset_has_real_attributes(Value) or nodeset_has_real_composites(Value)) {
      return ERR::Args;
   }

   std::ostringstream buffer;
   for (auto *node : Value.node_set) {
      if (not node) return ERR::Args;
      serialise_tag_fragment(*node, buffer);
   }

   OutXml = buffer.str();
   if (OutXml.empty()) return ERR::Args;
   return ERR::Okay;
}

//********************************************************************************************************************
// <data> requires at least one real top-level tag in the parsed fragment, while parse@select is allowed to accept
// more permissive fragments.

static bool xq_has_top_level_tag(const objXML *XML)
{
   if (not XML) return false;

   for (const auto &tag : XML->Tags) {
      if (tag.isTag()) return true;
   }

   return false;
}

//********************************************************************************************************************
// Centralised fragment parser for XML-emitting surfaces.  RequireTag lets callers distinguish between "must load a
// document tree" and "may parse loose markup".

static ERR xq_parse_xml_fragment(std::string_view Fragment, objXML *&OutXML, bool RequireTag)
{
   OutXML = nullptr;

   auto xml = objXML::create::local(fl::Statement(std::string(Fragment)), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS));
   if (not xml) return ERR::Syntax;

   if (RequireTag and (not xq_has_top_level_tag(xml))) {
      FreeResource(xml);
      return ERR::Syntax;
   }

   OutXML = xml;
   return ERR::Okay;
}

//********************************************************************************************************************
// Best-effort ownership test for XTag pointers drawn from mixed XML sources such as the original RIPL, $doc,
// included XML, or templates.

static bool xq_xml_owns_node(objXML *XML, const XTag *Node)
{
   if ((not XML) or (not Node)) return false;

   XTag *tag = nullptr;
   if (XML->getTag(Node->ID, &tag) != ERR::Okay) return false;
   return tag IS Node;
}

//********************************************************************************************************************
// Resolve which XML object currently owns a node so <for-each> can rebind the XQuery context against the correct
// tree when evaluating relative paths.

static objXML * xq_find_known_node_owner(const parser *Parser, const XTag *Node)
{
   if ((not Parser) or (not Node)) return nullptr;

   if (xq_xml_owns_node(Parser->m_xml, Node)) return Parser->m_xml;
   if (xq_xml_owns_node(Parser->m_doc_xml, Node)) return Parser->m_doc_xml;
   for (int i = std::ssize(Parser->m_doc_xml_history) - 1; i >= 0; i--) {
      auto *xml = Parser->m_doc_xml_history[i];
      if (xq_xml_owns_node(xml, Node)) return xml;
   }
   if (xq_xml_owns_node(Parser->m_source_xml, Node)) return Parser->m_source_xml;
   if (xq_xml_owns_node(Parser->m_inject_xml, Node)) return Parser->m_inject_xml;
   if ((Parser->Self) and xq_xml_owns_node(Parser->Self->Templates, Node)) return Parser->Self->Templates;
   return nullptr;
}

static objXML * xq_resolve_binding_node_owner(const parser *Parser, const parser::xq_value_binding &Binding,
   size_t Index, const XTag *Node)
{
   if (Index < Binding.node_owners.size()) {
      if (auto owner = Binding.node_owners[Index]) return owner;
   }

   if (Binding.owned_xml and Node and xq_xml_owns_node(Binding.owned_xml.get(), Node)) {
      return Binding.owned_xml.get();
   }

   return xq_find_known_node_owner(Parser, Node);
}

static ERR xq_make_stored_value(parser *Parser, const XPathValue &Source, parser::xq_value_binding &Result,
   objXML **OwnedXML = nullptr)
{
   Result = parser::xq_value_binding(Source);
   if (OwnedXML) *OwnedXML = nullptr;
   if ((not Parser) or (Source.Type != XPVT::NodeSet)) return ERR::Okay;

   bool requires_owned_xml = false;
   size_t owned_node_count = 0;
   for (size_t index = 0; index < Source.node_set.size(); ++index) {
      auto *node = Source.node_set[index];
      if (not node) continue;

      auto owner = xq_find_known_node_owner(Parser, node);
      Result.node_owners[index] = owner;

      if (not owner) {
         requires_owned_xml = true;
         owned_node_count++;
      }
   }

   if (not requires_owned_xml) return ERR::Okay;

   auto xml = objXML::create::local(fl::Flags(XMF::NEW|XMF::READABLE));
   if (not xml) return ERR::NewObject;

   xml->Tags.clear();
   xml->Tags.reserve(owned_node_count);
   int next_id = 1;

   for (size_t index = 0; index < Source.node_set.size(); ++index) {
      auto *node = Source.node_set[index];
      if ((not node) or Result.node_owners[index]) continue;

      xml->Tags.emplace_back();
      auto &clone = xml->Tags.back();
      xq_clone_tag_tree(*node, clone, 0, next_id);
      Result.value.node_set[index] = &clone;
      Result.node_owners[index] = xml;

      if ((index < Source.node_set_attributes.size()) and Source.node_set_attributes[index]) {
         auto attrib_index = find_attrib_index(node, Source.node_set_attributes[index]);
         if ((attrib_index != std::numeric_limits<size_t>::max()) and (attrib_index < clone.Attribs.size())) {
            Result.value.node_set_attributes[index] = &clone.Attribs[attrib_index];
         }
         else Result.value.node_set_attributes[index] = nullptr;
      }
   }

   if (OwnedXML) *OwnedXML = xml;
   else Result.owned_xml = xq_adopt_xml(xml);
   return ERR::Okay;
}

//********************************************************************************************************************
// Clone a concrete node sequence directly into a new XML tree so <data select> can replace $doc without paying a
// serialise/reparse round-trip.  RequireTag preserves the existing distinction between document roots and loose
// fragments.

static ERR xq_clone_nodes_to_xml(parser *Parser, const XPathValue &Value, objXML *&OutXML, bool RequireTag)
{
   OutXML = nullptr;

   if ((not Parser) or (Value.Type != XPVT::NodeSet)) return ERR::Args;
   if (nodeset_has_real_attributes(Value) or nodeset_has_real_composites(Value)) return ERR::InvalidData;
   if (Value.node_set.empty()) return RequireTag ? ERR::Syntax : ERR::Args;

   parser::xq_value_binding stored_value;
   if (auto err = xq_make_stored_value(Parser, Value, stored_value); err != ERR::Okay) return err;

   auto xml = objXML::create::local(fl::Flags(XMF::NEW|XMF::READABLE));
   if (not xml) return ERR::NewObject;

   xml->Tags.clear();
   xml->Tags.reserve(stored_value.value.node_set.size());

   int next_id = 1;
   bool has_top_level_tag = false;

   for (auto *node : stored_value.value.node_set) {
      if (not node) {
         FreeResource(xml);
         return ERR::InvalidData;
      }

      xml->Tags.emplace_back();
      auto &clone = xml->Tags.back();
      xq_clone_tag_tree(*node, clone, 0, next_id);
      if (clone.isTag()) has_top_level_tag = true;
   }

   if (xml->Tags.empty() or (RequireTag and (not has_top_level_tag))) {
      FreeResource(xml);
      return ERR::Syntax;
   }

   OutXML = xml;
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse a concrete node sequence in-place by rebinding the parser's active XML to the owning tree of each selected
// top-level node.  This preserves direct node access for parse@select without reparsing generated markup text.

static ERR xq_parse_selected_nodes(parser *Parser, const XPathValue &Value)
{
   if ((not Parser) or (Value.Type != XPVT::NodeSet)) return ERR::Args;
   if (nodeset_has_real_attributes(Value) or nodeset_has_real_composites(Value)) return ERR::InvalidData;
   if (Value.node_set.empty()) return ERR::Args;

   parser::xq_value_binding stored_value;
   objXML *owned_xml = nullptr;
   if (auto err = xq_make_stored_value(Parser, Value, stored_value, &owned_xml); err != ERR::Okay) return err;

   if (owned_xml) Parser->Self->Resources.emplace_back(owned_xml->UID, RTD::OBJECT_TEMP);

   IPF flags = IPF::NIL;

   for (size_t node_index = 0; node_index < stored_value.value.node_set.size(); ++node_index) {
      auto *node = stored_value.value.node_set[node_index];
      if (not node) return ERR::InvalidData;

      auto node_xml = xq_resolve_binding_node_owner(Parser, stored_value, node_index, node);
      if (not node_xml) return ERR::InvalidData;

      auto old_xml = Parser->change_xml(node_xml);
      auto restore_xml = kt::Defer([&]() {
         Parser->change_xml(old_xml);
      });

      auto result = Parser->parse_tag(*node, flags);
      if (Parser->Self->Error != ERR::Okay) return Parser->Self->Error;
      if ((result & (TRF::CONTINUE|TRF::BREAK)) != TRF::NIL) break;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Convert a select result into plain text using XQuery string-value semantics.  This keeps print@select text-only,
// even when the expression yields nodes.

static ERR xq_stringify_select_value(const XPathValue &Value, std::string &Result)
{
   Result.clear();

   if ((Value.Type IS XPVT::NodeSet) and value_is_empty_sequence(Value)) return ERR::Okay;
   return xq_value_to_string(Value, Result);
}

//********************************************************************************************************************
// print@select always inserts text.  Callers may fall back to the exposed FID_ResultString when the raw value is not
// available from the XQuery object.

static ERR xq_select_to_print_text(const XPathValue &Value, std::string_view Fallback, bool HasValue,
   std::string &OutText)
{
   OutText.clear();

   if (HasValue) {
      if (auto err = xq_stringify_select_value(Value, OutText); err IS ERR::Okay) return ERR::Okay;
   }

   OutText.assign(Fallback);
   return ERR::Okay;
}

//********************************************************************************************************************
// Fallback used by XML/RIPL injection surfaces when the select result has to be treated as markup text.  Concrete
// node sequences are handled directly by the caller to avoid reparsing serialised XML.

static ERR xq_select_to_xml_fragment(parser *Parser, const XPathValue &Value, std::string_view Fallback,
   bool HasValue, std::string &OutXml)
{
   OutXml.clear();

   if (not HasValue) {
      OutXml.assign(Fallback);
      return ERR::Okay;
   }

   if (Value.Type != XPVT::NodeSet) return xq_stringify_select_value(Value, OutXml);

   if (nodeset_has_real_attributes(Value) or nodeset_has_real_composites(Value)) return ERR::InvalidData;

   if (Value.node_set.empty()) {
      if (value_is_empty_sequence(Value)) return ERR::Okay;
      return xq_stringify_select_value(Value, OutXml);
   }

   parser::xq_value_binding stored_value;
   if (auto err = xq_make_stored_value(Parser, Value, stored_value); err != ERR::Okay) return err;
   return xq_value_to_xml_fragment(stored_value.value, OutXml);
}

static ERR xq_expand_avt(parser *Parser, objXML *XMLContext, const XTag *ContextTag, std::string_view Input,
   std::string &Output);

static ERR xq_prepare_tag(parser *Parser, const XTag &Tag, kt::vector<XMLAttrib> &PreparedAttribs,
   const kt::vector<XMLAttrib> *&ActiveAttribs)
{
   ActiveAttribs = &Tag.Attribs;

   if (Tag.Attribs.size() < 2) return ERR::Okay;

   auto tag_name = std::string_view(Tag.Attribs[0].Name);
   if ((not tag_name.empty()) and (tag_name.front() IS '$')) return ERR::Okay;
   auto tag_hash = strhash(tag_name);

   bool requires_prepare = false;
   for (int i=1; i < std::ssize(Tag.Attribs); i++) {
      auto name = std::string_view(Tag.Attribs[i].Name);
      if ((not name.empty()) and (name.front() IS '$')) continue;
      if (name.empty()) continue;

      if ((not is_expression_attrib(tag_hash, name)) and has_avt_markers(Tag.Attribs[i].Value)) {
         requires_prepare = true;
         break;
      }
   }

   if (not requires_prepare) return ERR::Okay;

   PreparedAttribs = Tag.Attribs;

   for (int i=1; i < std::ssize(PreparedAttribs); i++) {
      auto name = std::string_view(PreparedAttribs[i].Name);
      if ((not name.empty()) and (name.front() IS '$')) continue;
      if (name.empty()) continue;

      auto &value = PreparedAttribs[i].Value;

      if ((not is_expression_attrib(tag_hash, name)) and has_avt_markers(value)) {
         std::string expanded;
         if (auto err = xq_expand_avt(Parser, Parser->m_xml, &Tag, value, expanded); err != ERR::Okay) return err;

         if (expanded != value) value = std::move(expanded);
      }
   }

   ActiveAttribs = &PreparedAttribs;
   return ERR::Okay;
}

// Expand a string containing attribute value template fragments {...}, evaluating each embedded XQuery expression
// and concatenating the result with literal parts.  Follows the same escaping rules as the XQuery tokeniser so that
// {{ and }} are preserved as literal { and }.

static ERR xq_expand_avt(parser *Parser, objXML *XMLContext, const XTag *ContextTag, std::string_view Input,
   std::string &Output)
{
   kt::Log log(__FUNCTION__);
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
            Parser->log_error(ContextTag, ERR::Syntax, "doc.avt-unterminated", "Unterminated attribute value template: {}.", Input);
            return ERR::Syntax;
         }

         auto expr = Input.substr(expr_start, scan - expr_start);
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
// Used by if, elseif, while statements to check the satisfaction of conditions.
//
// XQuery integration: the test attribute is recognised as a standalone XQuery expression.  The result of the
// expression is coerced via XQuery's effective boolean value rules (boolean(...)).

static bool check_tag_conditions(parser *Parser, const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   for (unsigned i=1; i < Tag.Attribs.size(); i++) {
      auto name = std::string_view(Tag.Attribs[i].Name);
      if ((not name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
   }

   for (unsigned i=1; i < Tag.Attribs.size(); i++) {
      auto name = std::string_view(Tag.Attribs[i].Name);
      if ((not name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
      if ("test" IS name) {
         std::string result;
         bool boolean_result = false;
        auto err = xq_eval_helper(Parser, Parser->m_xml, Tag.Source, Tag.Attribs[i].Value,
            XQEval::BOOLEAN, result, boolean_result);
         if (err != ERR::Okay) {
            Parser->log_error(&Tag, err, "doc.xquery-test-failed", attrib_name("test"),
               "XQuery test failed: {}.", Tag.Attribs[i].Value);
            return false;
         }
         log.trace("Test: %s -> %s", Tag.Attribs[i].Value.c_str(), boolean_result ? "true" : "false");
         return boolean_result;
      }
   }

   return false;
}

//********************************************************************************************************************
// Load or replace the parser-local document data scope.

void parser::tag_data(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   auto select = find_attrib(Tag, "select");
   if (not select) {
      log_warning(&Tag, "doc.data-missing-select", "<data> requires a select attribute.");
      return;
   }

   XPathValue value(XPVT::String);
   std::string result;
   bool has_value = false;
   auto err = xq_eval_value_helper(this, m_xml, Tag.Source, *select, value, result, has_value);
   if (err != ERR::Okay) {
      log_error(&Tag, err, "doc.data-select-failed", attrib_name("select"), "<data select=\"{}\"> failed.", *select);
      return;
   }

   if (has_value and (value.Type IS XPVT::NodeSet)) {
      if (nodeset_has_real_attributes(value) or nodeset_has_real_composites(value)) {
         log_error(&Tag, ERR::InvalidData, "doc.data-select-invalid-result",
            "<data select> requires parseable XML/RIPL markup or a concrete XML node sequence.");
         return;
      }

      if (not value.node_set.empty()) {
         objXML *new_doc = nullptr;
         err = xq_clone_nodes_to_xml(this, value, new_doc, true);
         if (err != ERR::Okay) {
            log_error(&Tag, err, "doc.data-select-invalid-result",
               "<data select> requires parseable XML/RIPL markup or a concrete XML node sequence.");
            return;
         }

         replace_doc_xml(new_doc);
         return;
      }
   }

   std::string xml_fragment;
   err = xq_select_to_xml_fragment(this, value, result, has_value, xml_fragment);
   if (err != ERR::Okay) {
      log_error(&Tag, err, "doc.data-select-invalid-result",
         "<data select> requires parseable XML/RIPL markup or a concrete XML node sequence.");
      return;
   }

   objXML *new_doc = nullptr;
   err = xq_parse_xml_fragment(xml_fragment, new_doc, true);
   if (err != ERR::Okay) {
      log_error(&Tag, err, "doc.data-select-xml-parse-failed",
         "<data select> requires parseable XML/RIPL markup or an XML node sequence, got: {}.", xml_fragment);
      return;
   }

   replace_doc_xml(new_doc);
}

//********************************************************************************************************************
// Parse a string value as XML

void parser::tag_parse(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   // The value attribute will contain XML.  We will parse the XML as if it were part of the document source.  This feature
   // is typically used when pulling XML information out of an object field.
   //
   //   select="..."  evaluates an XQuery expression and parses the resulting string as XML/RIPL content.

   if (std::ssize(Tag.Attribs) > 1) {
      // Prefer select over value when both are present.
      for (int i=1; i < std::ssize(Tag.Attribs); i++) {
         auto name = std::string_view(Tag.Attribs[i].Name);
         if ((not name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
         if ("select" IS name) {
            XPathValue value(XPVT::String);
            std::string result;
            bool has_value = false;
            auto err = xq_eval_value_helper(this, m_xml, Tag.Source, Tag.Attribs[i].Value, value, result, has_value);
            if (err != ERR::Okay) {
               log_error(&Tag, err, "doc.parse-select-failed", attrib_name("select"),
                  "<parse select=\"{}\"> failed.", Tag.Attribs[i].Value);
               return;
            }

            if (has_value and (value.Type IS XPVT::NodeSet)) {
               if (nodeset_has_real_attributes(value) or nodeset_has_real_composites(value)) {
                  log_error(&Tag, ERR::InvalidData, "doc.parse-select-invalid-result",
                     "<parse select> requires parseable XML/RIPL markup or a concrete XML node sequence.");
                  return;
               }

               if (not value.node_set.empty()) {
                  log.traceBranch("Parsing XQuery node sequence directly...");
                  err = xq_parse_selected_nodes(this, value);
                  if (err != ERR::Okay) {
                     log_error(&Tag, err, "doc.parse-select-invalid-result",
                        "<parse select> requires parseable XML/RIPL markup or a concrete XML node sequence.");
                  }
                  return;
               }
            }

            std::string xml_fragment;
            err = xq_select_to_xml_fragment(this, value, result, has_value, xml_fragment);
            if (err != ERR::Okay) {
               log_error(&Tag, err, "doc.parse-select-invalid-result",
                  "<parse select> requires parseable XML/RIPL markup or a concrete XML node sequence.");
               return;
            }

            log.traceBranch("Parsing XQuery result as XML...");
            objXML *xmlinc = nullptr;
            if (xq_parse_xml_fragment(xml_fragment, xmlinc, false) != ERR::Okay) {
               log_error(&Tag, ERR::Syntax, "doc.parse-select-xml-parse-failed",
                  "<parse select> requires parseable XML/RIPL markup or an XML node sequence, got: {}.", xml_fragment);
               return;
            }
            auto old_xml = change_xml(xmlinc);
            parse_tags(xmlinc->Tags);
            change_xml(old_xml);
            Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
            return;
         }
      }

      if (("value" IS Tag.Attribs[1].Name) or ("$value" IS Tag.Attribs[1].Name)) {
         log.traceBranch("Parsing string value as XML...");

         if (auto xmlinc = objXML::create::local(fl::Statement(Tag.Attribs[1].Value), fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS))) {
            auto old_xml = change_xml(xmlinc);
            parse_tags(xmlinc->Tags);
            change_xml(old_xml);

            // Add the created XML object to the document rather than destroying it

            Self->Resources.emplace_back(xmlinc->UID, RTD::OBJECT_TEMP);
         }
         else log_error(&Tag, ERR::Syntax, "doc.parse-value-xml-parse-failed", attrib_name("value"),
            "<parse value=\"{}\"> could not be parsed as XML/RIPL.", Tag.Attribs[1].Value);
      }
      else log_hint(&Tag, "doc.parse-noop",
         "<parse/> requires either a select or value attribute.");
   }
   else {
      log_hint(&Tag, "doc.parse-noop",
         "<parse/> requires either a select or value attribute.");
   }
}

//********************************************************************************************************************

void parser::tag_print(const tag_view &Tag)
{
   kt::Log log(__FUNCTION__);

   // Copy the content from the value attribute into the document stream.  If used inside an object, the data is sent
   // to that object as XML.
   //
   //   select="..."  evaluates an XQuery expression and inserts its string value
   //   value="..."   may contain {...} attribute value template fragments

   if (Tag.Attribs.size() > 1) {
      // Prefer select over value when both are present.
      for (int i=1; i < std::ssize(Tag.Attribs); i++) {
         auto name = std::string_view(Tag.Attribs[i].Name);
         if ((not name.empty()) and (name.front() IS '$')) name.remove_prefix(1);
         if ("select" IS name) {
            XPathValue value(XPVT::String);
            std::string result;
            bool has_value = false;
            auto err = xq_eval_value_helper(this, m_xml, Tag.Source, Tag.Attribs[i].Value, value, result, has_value);
            if (err != ERR::Okay) {
               log_error(&Tag, err, "doc.print-select-failed", attrib_name("select"),
                  "<print select=\"{}\"> failed.", Tag.Attribs[i].Value);
               return;
            }
            err = xq_select_to_print_text(value, result, has_value, result);
            if (err != ERR::Okay) {
               log_error(&Tag, err, "doc.print-select-text-conversion-failed", attrib_name("select"),
                  "<print select=\"{}\"> could not be converted into text.", Tag.Attribs[i].Value);
               return;
            }
            insert_text(Self, m_stream, m_index, result, (m_style.options & FSO::PREFORMAT) != FSO::NIL);
            return;
         }
      }

      auto tagname = std::string_view(Tag.Attribs[1].Name);
      if (tagname.starts_with('$')) tagname.remove_prefix(1);

      if ("value" IS tagname) {
         insert_text(Self, m_stream, m_index, Tag.Attribs[1].Value, (m_style.options & FSO::PREFORMAT) != FSO::NIL);
      }
      else if ("src" IS tagname) {
         // This option is only supported in unrestricted mode
         if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) {
            CacheFile *cache;
            if (LoadFile(Tag.Attribs[1].Value, LDF::NIL, &cache) IS ERR::Okay) {
               insert_text(Self, m_stream, m_index, std::string((CSTRING)cache->Data), (m_style.options & FSO::PREFORMAT) != FSO::NIL);
               UnloadFile(cache);
            }
         }
         else log_warning(&Tag, "doc.print-src-restricted", attrib_name("src"),
            "Cannot <print src.../> unless in unrestricted mode.");
      }
      else log_hint(&Tag, "doc.print-noop", "<print/> element does nothing.");
   }
}

//********************************************************************************************************************
// <let> introduces a lexical $state binding that is visible only to the element's children.  The state_guard above
// restores any previous binding automatically.

TRF parser::tag_let(const tag_view &Tag, IPF &Flags)
{
   kt::Log log(__FUNCTION__);

   if (Tag.Children.empty()) {
      log_error(&Tag, ERR::InvalidData, "doc.let-missing-content", "<let> requires child content.");
      return TRF::NIL;
   }

   auto name = find_attrib(Tag, "name");
   auto select = find_attrib(Tag, "select");
   if ((not name) or name->empty() or (not select)) {
      log_error(&Tag, ERR::InvalidData, "doc.let-missing-attrib", "<let> requires both name and select attributes.");
      return TRF::NIL;
   }

   std::string binding_name = *name;
   if ((not binding_name.empty()) and (binding_name.front() IS '$')) binding_name.erase(0, 1);
   if (binding_name.empty()) {
      log_error(&Tag, ERR::InvalidData, "doc.let-empty-name", attrib_name("name"),
         "<let name=\"...\"> resolved to an empty binding name.");
      return TRF::NIL;
   }

   XPathValue value(XPVT::String);
   std::string result;
   bool has_value = false;
   auto err = xq_eval_value_helper(this, m_xml, Tag.Source, *select, value, result, has_value);
   if (err != ERR::Okay) {
      log_error(&Tag, err, "doc.let-select-failed", attrib_name("select"), "<let select=\"{}\"> failed.", *select);
      return TRF::NIL;
   }

   if (not has_value) {
      value = XPathValue(XPVT::String);
      value.StringValue = result;
   }

   xq_value_binding binding_value;
   if ((err = xq_make_stored_value(this, value, binding_value)) != ERR::Okay) {
      log_error(&Tag, err, "doc.let-preserve-failed", attrib_name("select"),
         "<let select=\"{}\"> could not preserve the selected value.", *select);
      return TRF::NIL;
   }

   state_guard binding(this, std::move(binding_name), binding_value);
   return parse_tags(Tag.Children, Flags);
}

//********************************************************************************************************************
// <for-each> evaluates its select expression once, requires a node sequence, and then parses its children once for
// each item with both $loop and the XQuery context item rebound to that node.

TRF parser::tag_for_each(const tag_view &Tag, IPF &Flags)
{
   kt::Log log(__FUNCTION__);

   auto select = find_attrib(Tag, "select");
   if (not select) {
      log_error(&Tag, ERR::InvalidData, "doc.for-each-missing-select", "<for-each> requires a select attribute.");
      return TRF::NIL;
   }

   XPathValue value(XPVT::String);
   std::string result;
   bool has_value = false;
   auto err = xq_eval_value_helper(this, m_xml, Tag.Source, *select, value, result, has_value);
   if (err != ERR::Okay) {
      log_error(&Tag, err, "doc.for-each-select-failed", attrib_name("select"), "<for-each select=\"{}\"> failed.", *select);
      return TRF::NIL;
   }

   if ((not has_value) and result.empty()) return TRF::NIL;

   xq_value_binding sequence_value;
   if ((err = xq_make_stored_value(this, value, sequence_value)) != ERR::Okay) {
      log_error(&Tag, err, "doc.for-each-preserve-failed", attrib_name("select"),
         "<for-each select=\"{}\"> could not preserve the selected sequence.", *select);
      return TRF::NIL;
   }

   auto &stored_value = sequence_value.value;

   if ((not has_value) or (not (stored_value.Type IS XPVT::NodeSet)) or
      nodeset_has_real_attributes(stored_value) or nodeset_has_real_composites(stored_value)) {
      log_error(&Tag, ERR::InvalidData, "doc.for-each-invalid-result", "<for-each> currently requires a node sequence result.");
      return TRF::NIL;
   }

   if (stored_value.node_set.empty()) {
      if (value_is_empty_sequence(stored_value)) return TRF::NIL;
      log_error(&Tag, ERR::InvalidData, "doc.for-each-invalid-result", "<for-each> currently requires a node sequence result.");
      return TRF::NIL;
   }

   for (auto *node : stored_value.node_set) {
      if (not node) {
         log_error(&Tag, ERR::InvalidData, "doc.for-each-invalid-result", "<for-each> currently requires a node sequence result.");
         return TRF::NIL;
      }
   }

   loop_frame frame;
   frame.index = 0;
   frame.iteration = 0;
   frame.start = 0;
   frame.step = 1;
   frame.count = int(stored_value.node_set.size());
   frame.count_known = true;

   loop_guard loop(this, frame);
   TRF result_flags = TRF::NIL;

   for (size_t item_index = 0; item_index < stored_value.node_set.size(); ++item_index) {
      XTag *item = stored_value.node_set[item_index];

      auto active_loop = loop.frame();
      if (not active_loop) break;

      active_loop->index = int(item_index);
      active_loop->iteration = int(item_index);

      auto item_xml = xq_resolve_binding_node_owner(this, sequence_value, item_index, item);
      if (not item_xml) {
         log_error(&Tag, ERR::InvalidData, "doc.for-each-owner-missing",
            "<for-each> could not resolve the owning XML document for the current item.");
         return TRF::NIL;
      }

      xq_context_guard item_context(this, item_xml, item);

      result_flags = parse_tags(Tag.Children, Flags);
      if (Self->Error != ERR::Okay) break;

      if ((result_flags & TRF::BREAK) != TRF::NIL) {
         result_flags = TRF::NIL;
         break;
      }

      if ((result_flags & TRF::CONTINUE) != TRF::NIL) {
         result_flags = TRF::NIL;
         continue;
      }
   }

   return result_flags;
}
