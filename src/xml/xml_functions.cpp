//********************************************************************************************************************
// XML Module Functions and Utilities
//
// Provides string utilities, character classification, and helper functions for the XML module.  These functions
// support XML serialisation, attribute value escaping, content formatting, and general-purpose string operations
// used throughout the XML processing pipeline.
//
// Key functionality:
//   - XML character escaping and attribute value formatting
//   - String utilities for XML content manipulation
//   - Character classification for XML syntax validation
//   - URI handling and encoding utilities
//
// This module ensures consistent XML output formatting and provides the building blocks for safe XML generation.

#include <ankerl/unordered_dense.h>
#include <format>
#include <vector>
#include <utility>

#include "unescape.h"
#include "uri_utils.h"

static void output_attribvalue(std::string_view String, std::ostringstream &Output)
{
   size_t last_pos = 0;
   for (size_t i = 0; i < String.size(); ++i) {
      auto escape = xml_escape_table[static_cast<unsigned char>(String[i])];
      if (escape) {
         // Output any unescaped characters before this one
         if (i > last_pos) {
            Output.write(String.data() + last_pos, i - last_pos);
         }
         Output << escape;
         last_pos = i + 1;
      }
   }
   // Output any remaining unescaped characters
   if (last_pos < String.size()) {
      Output.write(String.data() + last_pos, String.size() - last_pos);
   }
}

inline void assign_string(STRING &Target, const std::string_view Value)
{
   if (Target) { FreeResource(Target); Target = nullptr; }
   if (not Value.empty()) Target = pf::strclone(Value);
}

static bool ci_keyword(std::string_view &view, std::string_view keyword) noexcept
{
   if (keyword.empty() or view.size() < keyword.size()) return false;

   for (size_t i = 0; i < keyword.size(); ++i) {
      if (to_lower(view[i]) != to_lower(keyword[i])) return false;
   }

   // Check that we're not matching a partial name
   if ((view.size() > keyword.size()) and
       is_name_char(view[keyword.size()]) and
       (view[keyword.size()] != '[')) return false;

   view.remove_prefix(keyword.size());
   return true;
}

static bool ci_keyword(ParseState &State, std::string_view keyword) noexcept
{
   auto view = State.cursor;
   if (ci_keyword(view, keyword)) {
      State.next(keyword.size());
      return true;
   }
   return false;
}

static std::string_view read_name(std::string_view &view) noexcept
{
   if (view.empty() or not is_name_start(view.front())) {
      return std::string_view();
   }

   size_t length = 1;
   while ((length < view.size()) and is_name_char(view[length])) {
      length++;
   }

   auto result = view.substr(0, length);
   view.remove_prefix(length);
   return result;
}

static void expand_entity_references(extXML *Self, std::string &Value,
   ankerl::unordered_dense::set<std::string> &EntityStack, ankerl::unordered_dense::set<std::string> &ParameterStack);

static ERR resolve_entity_internal(extXML *Self, const std::string &Name, std::string &Value,
   bool Parameter, ankerl::unordered_dense::set<std::string> &EntityStack, ankerl::unordered_dense::set<std::string> &ParameterStack)
{
   pf::Log log(__FUNCTION__);

   auto &stack = Parameter ? ParameterStack : EntityStack;
   if (stack.contains(Name)) return log.warning(ERR::Loop);

   auto &table = Parameter ? Self->ParameterEntities : Self->Entities;
   auto *result = find_in_map(table, Name);
   if (not result) return ERR::Search;

   stack.insert(Name);

   Value = *result;
   expand_entity_references(Self, Value, EntityStack, ParameterStack);

   stack.erase(Name);
   return ERR::Okay;
}

static void expand_entity_references(extXML *Self, std::string &Value,
   ankerl::unordered_dense::set<std::string> &EntityStack, ankerl::unordered_dense::set<std::string> &ParameterStack)
{
   if (Value.empty()) return;

   // Scan for entities first to avoid allocation if none present

   bool has_entities = false;
   for (size_t i = 0; i < Value.size(); ++i) {
      if ((Value[i] IS '%' or Value[i] IS '&') and (i + 1 < Value.size())) {
         has_entities = true;
         break;
      }
   }

   if (not has_entities) return;

   std::string output;
   output.reserve(Value.size() * 2); // Pre-allocate larger buffer to reduce reallocations

   std::string_view view(Value);

   while (not view.empty()) {
      auto ch = view.front();

      if ((ch IS '%' or ch IS '&') and view.size() > 1) {
         if ((ch IS '&') and (view[1] IS '#')) {
            auto terminator = view.find(';');
            if (!(terminator IS std::string_view::npos)) {
               char unichar[6];
               size_t ulen = 0;
               if (decode_numeric_reference(view.data(), terminator + 1, unichar, ulen)) {
                  output.append(unichar, ulen);
                  view.remove_prefix(terminator + 1);
                  continue;
               }
            }
         }
         bool is_parameter = (ch IS '%');
         view.remove_prefix(1); // Skip the % or &

         // Find the name end using string_view operations
         size_t name_length = 0;
         while (name_length < view.size() and is_name_char(view[name_length])) {
            name_length++;
         }

         if ((name_length > 0) and (name_length < view.size()) and (view[name_length] IS ';')) {
            auto name_view = view.substr(0, name_length);
            std::string resolved;

            // Only create string if resolution succeeds to avoid unnecessary allocation
            if (resolve_entity_internal(Self, std::string(name_view), resolved, is_parameter, EntityStack, ParameterStack) IS ERR::Okay) {
               output.append(resolved);
            }
            else { // Reconstruct the original entity reference
               output.push_back(is_parameter ? '%' : '&');
               output.append(name_view);
               output.push_back(';');
            }

            view.remove_prefix(name_length + 1); // Skip name + ';'
            continue;
         }
         else { // Not a valid entity reference, backtrack
            output.push_back(is_parameter ? '%' : '&');
         }
      }
      else {
         output.push_back(ch);
         view.remove_prefix(1);
      }
   }

   Value = std::move(output);
}

ERR extXML::resolveEntity(const std::string &Name, std::string &Value, bool Parameter)
{
   ankerl::unordered_dense::set<std::string> entity_stack;
   ankerl::unordered_dense::set<std::string> parameter_stack;
   return resolve_entity_internal(this, Name, Value, Parameter, entity_stack, parameter_stack);
}

static bool read_quoted(extXML *Self, ParseState &State, std::string &Result,
   ankerl::unordered_dense::set<std::string> &EntityStack, ankerl::unordered_dense::set<std::string> &ParameterStack)
{
   if (State.done()) return false;

   auto quote = State.current();
   if ((quote != '"') and (quote != '\'')) return false;

   State.next();

   std::string buffer;
   buffer.reserve(State.cursor.size());

   while (not State.done()) {
      auto ch = State.current();

      if (ch IS quote) {
         State.next();
         Result.swap(buffer);
         return true;
      }

      if ((ch IS '%' or ch IS '&') and (State.cursor.size() > 1)) {
         if ((ch IS '&') and (State.cursor[1] IS '#')) {
            auto terminator = State.cursor.find(';');
            if (!(terminator IS std::string_view::npos)) {
               char unichar[6];
               size_t ulen = 0;
               if (decode_numeric_reference(State.cursor.data(), terminator + 1, unichar, ulen)) {
                  buffer.append(unichar, ulen);
                  State.next(terminator + 1);
                  continue;
               }
            }
         }
         bool is_parameter = (ch IS '%');
         State.next(); // Skip the % or &

         size_t name_length = 0;
         while ((name_length < State.cursor.size()) and is_name_char(State.cursor[name_length])) {
            name_length++;
         }

         if ((name_length > 0) and
             (name_length < State.cursor.size()) and
             (State.cursor[name_length] IS ';')) {

            std::string name(State.cursor.data(), name_length);
            std::string resolved;

            if (resolve_entity_internal(Self, name, resolved, is_parameter, EntityStack, ParameterStack) IS ERR::Okay) {
               buffer += resolved;
            }
            else {
               buffer.push_back(is_parameter ? '%' : '&');
               buffer += name;
               buffer.push_back(';');
            }

            State.next(name_length + 1); // Skip name + ';'
            continue;
         }
         else {
            buffer.push_back(is_parameter ? '%' : '&');
            continue;
         }
      }

      if (ch IS '\n') Self->LineNo++;
      buffer.push_back(ch);
      State.next();
   }

   return false;
}

//********************************************************************************************************************
// Support for !DOCTYPE parsing.  Note: DTD and !ATTLIST is not to be implemented as it is surpassed by XML Schema.

static void parse_doctype(extXML *Self, ParseState &State)
{
   State.skipWhitespace(Self->LineNo);

   auto view = State.cursor;
   auto type_view = read_name(view);
   if (type_view.empty()) return;

   assign_string(Self->DocType, type_view);
   if (Self->PublicID) { FreeResource(Self->PublicID); Self->PublicID = nullptr; }
   if (Self->SystemID) { FreeResource(Self->SystemID); Self->SystemID = nullptr; }
   Self->Entities.clear();
   Self->ParameterEntities.clear();
   Self->Notations.clear();

   bool standalone = ((Self->Flags & XMF::STANDALONE) != XMF::NIL);

   State.next(type_view.size());
   State.skipWhitespace(Self->LineNo);

   ankerl::unordered_dense::set<std::string> entity_stack;
   ankerl::unordered_dense::set<std::string> parameter_stack;

   if (ci_keyword(State, "PUBLIC")) {
      State.skipWhitespace(Self->LineNo);
      std::string public_id;
      if (read_quoted(Self, State, public_id, entity_stack, parameter_stack)) {
         if (not standalone) assign_string(Self->PublicID, public_id);
      }
      State.skipWhitespace(Self->LineNo);
      std::string system_id;
      if (read_quoted(Self, State, system_id, entity_stack, parameter_stack)) {
         if (not standalone) assign_string(Self->SystemID, system_id);
      }
   }
   else if (ci_keyword(State, "SYSTEM")) {
      State.skipWhitespace(Self->LineNo);
      std::string system_id;
      if (read_quoted(Self, State, system_id, entity_stack, parameter_stack)) {
         if (not standalone) assign_string(Self->SystemID, system_id);
      }
   }

   State.skipWhitespace(Self->LineNo);

   if (State.current() IS '[') {
      State.next();

      while (not State.done()) {
         State.skipWhitespace(Self->LineNo);
         if (State.done()) break;
         if (State.current() IS ']') { State.next(); break; }

         if ((State.current() IS '<') and (State.cursor.size() > 1) and (State.cursor[1] IS '!')) {
            State.next(2);

            if (ci_keyword(State, "ENTITY")) {
               State.skipWhitespace(Self->LineNo);
               bool parameter = false;
               if (State.current() IS '%') {
                  parameter = true;
                  State.next();
                  State.skipWhitespace(Self->LineNo);
               }

               auto name_view = State.cursor;
               auto entity_name = read_name(name_view);
               if (entity_name.empty()) {
                  State.skipTo('>', Self->LineNo);
                  if (State.current() IS '>') State.next();
                  continue;
               }

               std::string name(entity_name);
               State.next(entity_name.size());
               State.skipWhitespace(Self->LineNo);

               if (ci_keyword(State, "SYSTEM")) {
                  State.skipWhitespace(Self->LineNo);
                  std::string system;
                  if (read_quoted(Self, State, system, entity_stack, parameter_stack)) {
                     if (not standalone) {
                        if (parameter) Self->ParameterEntities[name] = system;
                        else Self->Entities[name] = system;
                     }
                  }
               }
               else {
                  std::string value;
                  if ((State.current() IS '"') or (State.current() IS '\'')) {
                     if (read_quoted(Self, State, value, entity_stack, parameter_stack)) {
                        if (parameter) Self->ParameterEntities[name] = value;
                        else Self->Entities[name] = value;
                     }
                  }
               }

               State.skipTo('>', Self->LineNo);
               if (State.current() IS '>') State.next();
            }
            else if (ci_keyword(State, "NOTATION")) {
               State.skipWhitespace(Self->LineNo);
               auto name_view = State.cursor;
               auto notation_name = read_name(name_view);
               if (notation_name.empty()) {
                  State.skipTo('>', Self->LineNo);
                  if (State.current() IS '>') State.next();
                  continue;
               }

               std::string name(notation_name);
               State.next(notation_name.size());
               State.skipWhitespace(Self->LineNo);

               std::string notation_value;
               if (ci_keyword(State, "PUBLIC")) {
                  State.skipWhitespace(Self->LineNo);
                  std::string public_id;
                  std::string system_id;
                  if (read_quoted(Self, State, public_id, entity_stack, parameter_stack)) {
                     State.skipWhitespace(Self->LineNo);
                     if (read_quoted(Self, State, system_id, entity_stack, parameter_stack)) {
                        notation_value = std::format("{} {}", public_id, system_id);
                     }
                     else notation_value = public_id;
                  }
               }
               else if (ci_keyword(State, "SYSTEM")) {
                  State.skipWhitespace(Self->LineNo);
                  read_quoted(Self, State, notation_value, entity_stack, parameter_stack);
               }

               if ((not notation_value.empty()) and (not standalone)) Self->Notations[name] = notation_value;

               State.skipTo('>', Self->LineNo);
               if (State.current() IS '>') State.next();
            }
            else {
               State.skipTo('>', Self->LineNo);
               if (State.current() IS '>') State.next();
            }
         }
         else {
            if (State.current() IS '\n') Self->LineNo++;
            State.next();
         }
      }
   }

   State.skipWhitespace(Self->LineNo);
}

//********************************************************************************************************************
// Extract content and add it to the end of Tags

static void extract_content(extXML *Self, TAGS &Tags, ParseState &State)
{
   if ((Self->Flags & XMF::STRIP_CONTENT) != XMF::NIL) {
      State.skipTo('<', Self->LineNo);
      return;
   }

   if ((Self->Flags & XMF::INCLUDE_WHITESPACE) IS XMF::NIL) {
      // Skip initial whitespace to find content.  We only drop content strings when they are pure whitespace.

      auto original = State.cursor;
      auto ch = State.skipWhitespace(Self->LineNo); // Find first non-whitespace character

      // If we found content (not '<'), reset to original position to preserve leading whitespace
      if ((not State.done()) and (ch != '<')) State.cursor = original;
   }

   if ((not State.done()) and (State.current() != '<')) {
      auto content = State;
      State.skipTo('<', Self->LineNo);

      std::string str;
      str.reserve(std::max(size_t(State - content), size_t(64))); // Reserve at least 64 bytes

      auto end_ptr = State.cursor.data();
      auto ptr = content.cursor.data();

      while (ptr < end_ptr) {
         char ch = *ptr++;
         if (ch == '\n') ++Self->LineNo;
         if (ch != '\r') str.push_back(ch);
      }

      auto &content_tag = Tags.emplace_back(XTag(glTagID++, 0, { { "", std::move(str) } }));
      Self->BaseURIMap[content_tag.ID] = State.CurrentBase;
   }
}

//********************************************************************************************************************
// Called by txt_to_xml() to extract the next tag from an XML string.  This function also recurses into itself.

class NamespaceScopeGuard {
   private:
   struct PrefixEntry {
      std::string Prefix;
      bool Restore;
      uint32_t PreviousValue;
   };

   ParseState &state;
   uint32_t default_namespace;
   std::vector<PrefixEntry> entries;

   public:
   explicit NamespaceScopeGuard(ParseState &StateRef)
      : state(StateRef), default_namespace(StateRef.DefaultNamespace)
   {
   }

   ~NamespaceScopeGuard()
   {
      for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
         if (it->Restore) {
            state.PrefixMap[it->Prefix] = it->PreviousValue;
         }
         else {
            state.PrefixMap.erase(it->Prefix);
         }
      }
      state.DefaultNamespace = default_namespace;
   }

   void registerInsertion(const std::string &Prefix, bool Inserted, uint32_t PreviousValue = 0)
   {
      entries.push_back(PrefixEntry{Prefix, not Inserted, PreviousValue});
   }
};

class BaseScopeGuard {
   private:
   ParseState &state;
   std::string previous_base;

   public:
   explicit BaseScopeGuard(ParseState &StateRef)
      : state(StateRef), previous_base(StateRef.CurrentBase)
   {
   }

   ~BaseScopeGuard()
   {
      state.CurrentBase = previous_base;
   }
};

static ERR parse_tag(extXML *Self, TAGS &Tags, ParseState &State)
{
   enum { RAW_NONE=0, RAW_CDATA, RAW_NDATA };

   pf::Log log(__FUNCTION__);

   log.traceBranch("%.*s", int(State.cursor.size()), State.cursor.data());

   NamespaceScopeGuard namespace_guard(State);
   BaseScopeGuard base_guard(State);

   if (State.current() != '<') {
      log.warning("Malformed XML statement detected.");
      return ERR::InvalidData;
   }

   State.next(); // Skip '<'

   auto line_no = Self->LineNo;

   if (State.startsWith("!--")) {
      if ((Self->Flags & XMF::INCLUDE_COMMENTS) IS XMF::NIL) {
         if (auto i = State.cursor.find("-->"); i != State.cursor.npos) {
            State.next(i + 3);
            return ERR::NothingDone;
         }
         else {
            log.warning("Detected malformed comment (missing --> terminator).");
            return ERR::InvalidData;
         }
      }

      State.next(3);
      auto comment = State;
      comment.skipTo("-->", Self->LineNo);

      if (comment.done()) {
         log.warning("Detected malformed comment (missing --> terminator).");
         return ERR::InvalidData;
      }

      std::string comment_text(State.cursor.data(), comment - State);
      auto &comment_tag = Tags.emplace_back(XTag(glTagID++, line_no, { { "", comment_text } }));
      comment_tag.Flags |= XTF::COMMENT;
      Self->BaseURIMap[comment_tag.ID] = State.CurrentBase;
      State = comment;
      State.next(3);
      return ERR::Okay;
   }

   line_no = Self->LineNo;
   int8_t raw_content;

   if (State.startsWith("![CDATA[")) { raw_content = RAW_CDATA; State.next(8); }
   else if (State.startsWith("![NDATA[")) { raw_content = RAW_NDATA; State.next(8); }
   else raw_content = RAW_NONE;

   if (raw_content) {
      // CDATA handler

      auto content = State; // Save start of content
      if (raw_content IS RAW_CDATA) {
         State.skipTo("]]>", Self->LineNo);
      }
      else if (raw_content IS RAW_NDATA) {
         int nest = 1;
         while (not State.done()) {
            if (State.startsWith("]]>")) {
               nest--;
               if (not nest) break;
            }
            else if ((State.startsWith("![CDATA[")) or (State.startsWith("![NDATA[")))  {
               nest++;
               State.next(8);
            }
            else if (State.current() IS '\n') Self->LineNo++;
         }
      }

      // CDATA counts as content and therefore can be stripped out if desired

      if (((Self->Flags & XMF::STRIP_CONTENT) != XMF::NIL) or (State - content IS 0)) {
         State.next(3);
         return ERR::NothingDone;
      }

      if (State.done()) {
         log.warning("Malformed XML:  A CDATA section is missing its closing string.");
         return ERR::InvalidData;
      }

      // CDATA sections are assimilated into the parent tag as content

      auto &cdata_tag = Tags.emplace_back(XTag(glTagID++, line_no, {
         { "", std::string(content.cursor.data(), State - content) }
      }));
      cdata_tag.Flags |= XTF::CDATA;
      Self->BaseURIMap[cdata_tag.ID] = State.CurrentBase;
      State.next(3); // Skip "]]>"
      return ERR::Okay;
   }

   if (State.startsWith("!DOCTYPE")) {
      State.next(8);

      if ((Self->Flags & XMF::PARSE_ENTITY) != XMF::NIL) {
         parse_doctype(Self, State);
      }
      else {
         int depth = 0;
         while (not State.done()) {
            char ch = State.current();
            if (ch IS '[') depth++;
            else if ((ch IS ']') and depth) depth--;
            else if ((ch IS '>') and (depth IS 0)) break;
            if (ch IS '\n') Self->LineNo++;
            State.next();
         }
      }

      if (State.current() IS '>') State.next();
      return ERR::NothingDone;
   }

   if ((State.current() IS '?') or (State.current() IS '!')) {
      if ((Self->Flags & XMF::STRIP_HEADERS) != XMF::NIL) {
         State.skipTo('>', Self->LineNo);
         if (State.current() IS '>') State.next();
         return ERR::NothingDone;
      }
   }

   State.Balance++;

   auto &tag = Tags.emplace_back(XTag(glTagID++, line_no));
   std::string node_base = State.CurrentBase;
   std::string inherited_base = State.CurrentBase;

   if (State.current() IS '?') tag.Flags |= XTF::INSTRUCTION; // Detect <?xml ?> style instruction elements.
   else if ((State.current() IS '!') and (State.cursor.size() > 1) and (State.cursor[1] >= 'A') and (State.cursor[1] <= 'Z')) tag.Flags |= XTF::NOTATION;

   bool xml_instruction = ((tag.Flags & XTF::INSTRUCTION) != XTF::NIL);
   bool is_xml_declaration = false;

   // Extract all attributes within the tag
   tag.Attribs.reserve(8); // Reserve space for typical attribute count

   State.skipWhitespace(Self->LineNo);
   while ((not State.done()) and (State.current() != '>')) {
      if (State.startsWith("/>")) break; // Termination checks
      if (State.startsWith("?>")) break;

      if (State.current() IS '=') return log.warning(ERR::InvalidData);

      std::string_view name;

      if (State.current() IS '"') {
         // Quoted notation attributes are parsed as content values
      }
      else {
         size_t length = 0;
         while ((length < State.cursor.size()) and (State.cursor[length] > 0x20) and (State.cursor[length] != '>') and (State.cursor[length] != '=')) {
            if ((State.cursor[length] IS '/') and (length + 1 < State.cursor.size()) and (State.cursor[length+1] IS '>')) break;
            if ((State.cursor[length] IS '?') and (length + 1 < State.cursor.size()) and (State.cursor[length+1] IS '>')) break;
            length++;
         }
         name = State.cursor.substr(0, length);
         State.next(length);
      }

      State.skipWhitespace(Self->LineNo);

      std::string name_string(name);
      bool has_value = false;
      std::string value_string;

      if (State.current() IS '=') {
         State.next();
         State.skipWhitespace(Self->LineNo);

         std::string val;
         val.reserve(64); // Reserve space for typical attribute value

         if (State.current() IS '"') {
            State.next();
            auto value_start = State.cursor.data();
            while ((not State.done()) and (State.current() != '"')) {
               if (State.current() IS '\n') Self->LineNo++;
               State.next();
            }
            val.assign(value_start, State.cursor.data() - value_start);
            if (State.current() IS '"') State.next();
         }
         else if (State.current() IS '\'') {
            State.next();
            auto value_start = State.cursor.data();
            while ((not State.done()) and (State.current() != '\'')) {
               if (State.current() IS '\n') Self->LineNo++;
               State.next();
            }
            val.assign(value_start, State.cursor.data() - value_start);
            if (State.current() IS '\'') State.next();
         }
         else {
            auto value_start = State.cursor.data();
            while ((not State.done()) and (State.current() > 0x20) and (State.current() != '>')) {
               if ((State.current() IS '/') and (State.cursor.size() > 1) and (State.cursor[1] IS '>')) break;
               if ((State.current() IS '?') and (State.cursor.size() > 1) and (State.cursor[1] IS '>')) break;
               State.next();
            }
            val.assign(value_start, State.cursor.data() - value_start);
         }

         value_string = val;
         has_value = true;
         tag.Attribs.emplace_back(name_string, val);

         if ((Self->Flags & XMF::NAMESPACE_AWARE) != XMF::NIL) {
            if (name.starts_with("xmlns")) {
               auto ns_hash = Self->registerNamespace(val);

               if (name IS "xmlns") {
                  State.DefaultNamespace = ns_hash;
               }
               else if ((name.size() > 6) and (name[5] IS ':')) {
                  std::string prefix(name.substr(6));
                  Self->Prefixes[prefix] = ns_hash;
                  auto result = State.PrefixMap.try_emplace(prefix, ns_hash);
                  if (result.second) {
                     namespace_guard.registerInsertion(prefix, true);
                  }
                  else {
                     auto &existing = result.first->second;
                     auto previous = existing;
                     namespace_guard.registerInsertion(prefix, false, previous);
                     existing = ns_hash;
                  }
               }
            }
         }
      }
      else if ((name.empty()) and (State.current() IS '"')) {
         State.next();
         auto value_state = State;
         while ((not State.done()) and (State.current() != '"')) {
            if (State.current() IS '\n') Self->LineNo++;
            State.next();
         }
         tag.Attribs.emplace_back(name_string, std::string(value_state.cursor.data(), State - value_state));
         if (State.current() IS '"') State.next();
      }
      else tag.Attribs.emplace_back(name_string, std::string{});

      if (has_value and pf::iequals(name_string, "xml:base")) {
         std::string resolved;

         if (value_string.empty()) {
            resolved = inherited_base;
         }
         else if (inherited_base.empty()) {
            resolved = value_string;
         }
         else {
            resolved = xml::uri::resolve_relative_uri(value_string, inherited_base);
         }

         resolved = xml::uri::normalise_uri_separators(std::move(resolved));
         node_base = resolved;
         State.CurrentBase = resolved;
         inherited_base = resolved;
     }

      if (xml_instruction) {
         if (tag.Attribs.size() IS 1) {
            is_xml_declaration = pf::iequals(tag.Attribs[0].Name, "?xml");
         }
         else if (is_xml_declaration and has_value and pf::iequals(name_string, "standalone")) {
            if (pf::iequals(value_string, "yes")) Self->Flags |= XMF::STANDALONE;
            else Self->Flags &= ~XMF::STANDALONE;
         }
      }

      State.skipWhitespace(Self->LineNo);
   }

   Self->BaseURIMap[tag.ID] = node_base;

   if (tag.Attribs.empty()) {
      log.warning("No attributes parsed for tag at line %d", Self->LineNo);
      return ERR::Syntax;
   }

   // Resolve prefixed tag names to namespace IDs

   if ((Self->Flags & XMF::NAMESPACE_AWARE) != XMF::NIL) {
      if (not tag.Attribs[0].Name.empty()) {
         auto &tag_name = tag.Attribs[0].Name;
         if (auto colon = tag_name.find(':'); colon != std::string::npos) {
            std::string prefix = tag_name.substr(0, colon);
            if (auto it = State.PrefixMap.find(prefix); it != State.PrefixMap.end()) {
               tag.NamespaceID = it->second;
            }
         }
         else if (State.DefaultNamespace) { // Apply default namespace if no prefix
            tag.NamespaceID = State.DefaultNamespace;
         }
      }
   }

   if ((State.current() IS '>') and (not tag.Attribs.empty()) and
       (tag.Attribs[0].Name[0] != '!') and (tag.Attribs[0].Name[0] != '?')) {
      // We reached the end of an open tag.  Extract the content within it and handle any child tags.

      State.next();
      extract_content(Self, Tags.back().Children, State);

      // Each cycle of the loop needs to start with an open tag

      while ((not State.done()) and (State.current() IS '<') and
             (State.cursor.size() > 1) and (State.cursor[1] != '/')) {
         auto error = parse_tag(Self, Tags.back().Children, State);

         if (error IS ERR::NothingDone) { // Extract any additional content trapped between tags
            extract_content(Self, Tags.back().Children, State);
         }
         else if (error IS ERR::Okay) { // Extract any new content caught in-between tags
            extract_content(Self, Tags.back().Children, State);
         }
         else return ERR::Failed;
      }

      // There should be a closing tag - skip past it

      if (State.cursor.starts_with("</")) {
         State.Balance--;
         while ((not State.done()) and (State.current() != '>')) { if (State.current() IS '\n') Self->LineNo++; State.next(); }
      }

      if (State.current() IS '>') State.next();
   }
   else {
      if (State.cursor.starts_with("/>")) State.next(2);
      State.Balance--;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Parse a text string into XML tags.

static ERR txt_to_xml(extXML *Self, TAGS &Tags, std::string_view Text)
{
   pf::Log log(__FUNCTION__);

   if (&Tags IS &Self->Tags) {
      if (Self->DocType)  { FreeResource(Self->DocType); Self->DocType = nullptr; }
      if (Self->PublicID) { FreeResource(Self->PublicID); Self->PublicID = nullptr; }
      if (Self->SystemID) { FreeResource(Self->SystemID); Self->SystemID = nullptr; }
      Self->Entities.clear();
      Self->ParameterEntities.clear();
      Self->Notations.clear();
      Self->BaseURIMap.clear();
      Self->Flags &= ~XMF::STANDALONE;
   }

   // Extract the tag information.  This loop will extract the top-level tags.  The parse_tag() function is recursive
   // to extract the child tags.

   log.msg(VLF::BRANCH|VLF::DETAIL, "Parsing %" PRId64 " bytes...", Text.size());

   // Estimate tag count and reserve space (heuristic: 1 tag per 100 characters)
   Tags.reserve(std::max(size_t(Text.size() / 100), size_t(16)));

   ParseState state(Text);

   if (Self->Path) state.CurrentBase = xml::uri::normalise_uri_separators(std::string(Self->Path));
   else state.CurrentBase.clear();

   state.skipTo('<', Self->LineNo); // Skip any leading whitespace or content
   if (state.done()) {
      Self->ParseError = log.warning(ERR::InvalidData);
      return Self->ParseError;
   }

   // Each cycle of the loop needs to start with an open tag

   while ((not state.done()) and (state.current() IS '<') and
          (state.cursor.size() > 1) and (state.cursor[1] != '/')) {
      ERR error = parse_tag(Self, Tags, state);

      if ((error != ERR::Okay) and (error != ERR::NothingDone)) {
         return log.warning(error);
      }

      // Skip content/whitespace to get to the next tag.  NB: We are working on the basis that
      // we are at the root level of the document and Kotuku permits multiple root tags.

      state.skipTo('<', Self->LineNo);
   }

   // If the WELL_FORMED flag is used, the tags must balance.

   if ((Self->Flags & XMF::WELL_FORMED) != XMF::NIL) {
      if (state.Balance != 0) return log.warning(ERR::UnbalancedXML);
   }

   if ((Self->Flags & XMF::NO_ESCAPE) IS XMF::NIL) {
      log.trace("Unescaping XML.");
      unescape_all(Self, Tags);
   }

   Self->modified();

   log.trace("XML parsing complete.");
   return ERR::Okay;
}

//********************************************************************************************************************
// Serialise XML data into string form.

static void serialise_xml(XTag &Tag, std::ostringstream &Buffer, XMF Flags)
{
   if (Tag.Attribs[0].isContent()) {
      if (not Tag.Attribs[0].Value.empty()) {
         if ((Tag.Flags & XTF::CDATA) != XTF::NIL) {
            if ((Flags & XMF::STRIP_CDATA) IS XMF::NIL) Buffer << "<![CDATA[";
            Buffer << Tag.Attribs[0].Value;
            if ((Flags & XMF::STRIP_CDATA) IS XMF::NIL) Buffer << "]]>";
         }
         else {
            // Use lookup table for efficient escaping (matching output_attribvalue)
            const auto &str = Tag.Attribs[0].Value;
            size_t last_pos = 0;
            for (size_t j = 0; j < str.size(); ++j) {
               if (auto escape = xml_escape_table[uint8_t(str[j])]; escape) {
                  if (j > last_pos) Buffer.write(str.data() + last_pos, j - last_pos);
                  Buffer << escape;
                  last_pos = j + 1;
               }
            }
            if (last_pos < str.size()) Buffer.write(str.data() + last_pos, str.size() - last_pos);
         }
      }
   }
   else if ((Flags & XMF::OMIT_TAGS) != XMF::NIL) {
      if (not Tag.Children.empty()) {
         for (auto &child : Tag.Children) {
            serialise_xml(child, Buffer, Flags);
         }
         if ((Flags & XMF::READABLE) != XMF::NIL) Buffer << '\n';
      }
   }
   else {
      Buffer << '<';

      bool insert_space = false;
      for (auto &scan : Tag.Attribs) {
         if (insert_space) Buffer << ' ';
         if (not scan.Name.empty()) output_attribvalue(scan.Name, Buffer);

         if (not scan.Value.empty()) {
            if (not scan.Name.empty()) Buffer << '=';
            Buffer << '"';
            output_attribvalue(scan.Value, Buffer);
            Buffer << '"';
         }
         insert_space = true;
      }

      if ((Tag.Flags & XTF::INSTRUCTION) != XTF::NIL) {
         Buffer << "?>";
         if ((Flags & XMF::READABLE) != XMF::NIL) Buffer << '\n';
      }
      else if ((Tag.Flags & XTF::NOTATION) != XTF::NIL) {
         Buffer << '>';
         if ((Flags & XMF::READABLE) != XMF::NIL) Buffer << '\n';
      }
      else if (not Tag.Children.empty()) {
         Buffer << '>';
         if (not Tag.Children[0].Attribs[0].isContent()) Buffer << '\n';

         for (auto &child : Tag.Children) {
            serialise_xml(child, Buffer, Flags);
         }

         Buffer << "</";
         output_attribvalue(Tag.Attribs[0].Name, Buffer);
         Buffer << '>';
         if ((Flags & XMF::READABLE) != XMF::NIL) Buffer << '\n';
      }
      else {
         Buffer << "/>";
         if ((Flags & XMF::READABLE) != XMF::NIL) Buffer << '\n';
      }
   }
}

//********************************************************************************************************************

static ERR parse_source(extXML *Self)
{
   pf::Log log(__FUNCTION__);
   CacheFile *filecache;

   log.traceBranch();

   Self->Tags.clear();
   Self->LineNo = 1;

   // Although the file will be uncached as soon it is loaded, the developer can pre-cache XML files with his own
   // call to LoadFile(), which can lead our use of LoadFile() to being quite effective.

   if (Self->Source) {
      constexpr size_t initial_size = 64 * 1024;
      constexpr size_t growth_threshold = 1024;

      std::string buffer;
      buffer.reserve(initial_size);
      Self->ParseError = ERR::Okay;
      acSeekStart(Self->Source, 0);

      while (true) {
         // Ensure we have space for the next read
         size_t current_size = buffer.size();
         if (buffer.capacity() - current_size < growth_threshold) {
            buffer.reserve(buffer.capacity() * 2);
         }

         // Read directly into the buffer
         buffer.resize(current_size + growth_threshold);
         int result;
         if (acRead(Self->Source, buffer.data() + current_size, growth_threshold, &result) != ERR::Okay) {
            Self->ParseError = ERR::Read;
            break;
         }
         else if (result <= 0) {
            buffer.resize(current_size); // Trim unused space
            break;
         }

         buffer.resize(current_size + result); // Adjust to actual read size
      }

      if (Self->ParseError IS ERR::Okay) {
         Self->ParseError = txt_to_xml(Self, Self->Tags, std::string_view(buffer));
      }
   }
   else if (LoadFile(Self->Path, LDF::NIL, &filecache) IS ERR::Okay) {
      Self->ParseError = txt_to_xml(Self, Self->Tags, std::string_view((char *)filecache->Data, filecache->Size));
      UnloadFile(filecache);
   }
   else Self->ParseError = ERR::File;

   return Self->ParseError;
}
