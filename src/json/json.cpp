/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
JSON: Extends the XML class with JSON support.

The JSON class extends the @XML class so JSON data can be loaded into an XML tree and inspected with XML operations
such as XPath queries.

The root JSON object and all named members are represented by `item` tags.  Each named member has `name` and `type`
attributes.  The supported type values are `object`, `array`, `string`, `integer`, `number`, `boolean`, and `null`.
Numbers without a fraction or exponent use `integer`; other standard numbers use `number`.

Arrays are represented by `item` tags with a `subtype` attribute.  A homogeneous array uses its common element type,
a heterogeneous array uses `mixed`, and an empty array uses `null`.  Scalar array elements use `value` tags with a
`type` attribute.  Nested objects and arrays use unnamed `item` tags.  Null values have no content, while booleans
retain the exact lower-case `true` or `false` text from the JSON source.

-EXAMPLE-
The following example illustrates scalar values, nested arrays, and a mixed array:

{ "string":"foo bar",
  "enabled":true,
  "nothing":null,
  "integers":[ 0, 1 ],
  "nested":[ [ "A" ], [ 1, null ] ]
}

It will be translated to the following when loaded into an XML object:

&lt;item type="object"&gt;
  &lt;item name="string" type="string"&gt;foo bar&lt;/item&gt;
  &lt;item name="enabled" type="boolean"&gt;true&lt;/item&gt;
  &lt;item name="nothing" type="null"/&gt;

  &lt;item name="integers" type="array" subtype="integer"&gt;
    &lt;value type="integer"&gt;0&lt;/value&gt;
    &lt;value type="integer"&gt;1&lt;/value&gt;
  &lt;/item&gt;

  &lt;item name="nested" type="array" subtype="array"&gt;
    &lt;item type="array" subtype="string"&gt;
      &lt;value type="string"&gt;A&lt;/value&gt;
    &lt;/item&gt;
    &lt;item type="array" subtype="mixed"&gt;
      &lt;value type="integer"&gt;1&lt;/value&gt;
      &lt;value type="null"/&gt;
    &lt;/item&gt;
  &lt;/item&gt;
&lt;/item&gt;

-END-

*********************************************************************************************************************/

#define PRV_XML
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

JUMPTABLE_CORE

static OBJECTPTR clJSON = nullptr;

static ERR JSON_Init(objXML *);
static ERR JSON_SaveToObject(objXML *, struct acSaveToObject *);

static bool valid_utf8(std::string_view Text) noexcept;

static ActionArray clActions[] = {
   { AC::Init,         JSON_Init },
   { AC::SaveToObject, JSON_SaveToObject },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

struct JSONOutput {
   std::string Text;
   bool Readable;

   explicit JSONOutput(bool ReadableValue) : Readable(ReadableValue) { }

   ERR quoted(std::string_view Value)
   {
      static constexpr char hex[] = "0123456789ABCDEF";

      if (not valid_utf8(Value)) return ERR::InvalidData;

      Text += '"';
      for (auto byte : Value) {
         auto value = uint8_t(byte);
         switch (value) {
            case '"':  Text += "\\\""; break;
            case '\\': Text += "\\\\"; break;
            case '\b': Text += "\\b"; break;
            case '\f': Text += "\\f"; break;
            case '\n': Text += "\\n"; break;
            case '\r': Text += "\\r"; break;
            case '\t': Text += "\\t"; break;
            default:
               if (value < 0x20) {
                  Text += "\\u00";
                  Text += hex[value >> 4];
                  Text += hex[value & 0x0f];
               }
               else Text += char(value);
               break;
         }
      }
      Text += '"';
      return ERR::Okay;
   }

   void line(int Depth)
   {
      if (not Readable) return;
      Text += '\n';
      Text.append(size_t(Depth) * 3, ' ');
   }

   void colon()
   {
      Text += Readable ? ": " : ":";
   }
};

static ERR txt_to_json(objXML *, std::string_view);
static ERR write_native_value(const XTag &Tag, JSONOutput &Output, int Depth);
static ERR write_xml_element(const XTag &Tag, JSONOutput &Output, int Depth);

struct XMLGroup {
   std::string_view Name;
   std::vector<const XTag *> Tags;
};

static size_t string_segment_length(CSTRING Input, CSTRING End) noexcept
{
   if (Input >= End) return 0;

   auto remaining = std::string_view(Input, size_t(End - Input));
   auto length = remaining.find_first_of(std::string_view("\"\\\0", 3));
   return (length IS std::string_view::npos) ? remaining.size() : length;
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   objModule::create xml = { fl::Name("xml") }; // Load our dependency ahead of class registration

   if ((clJSON = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::XML),
      fl::ClassID(CLASSID::JSON),
      fl::Name("JSON"),
      fl::Category(CCF::DATA),
      fl::FileExtension("json"),
      fl::FileDescription("JSON Data"),
      fl::Actions(clActions),
      fl::Path("modules:json")))) return ERR::Okay;

   return ERR::AddClass;
}

static ERR MODExpunge(void)
{
   if (clJSON) { FreeResource(clJSON); clJSON = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR load_file(objXML *Self, std::string_view Path)
{
   CacheFile *filecache;

   if (!(Self->ParseError = LoadFile(Path, LDF::NIL, &filecache))) { // loaded content is null terminated
      Self->ParseError = txt_to_json(Self, std::string_view((CSTRING)filecache->Data, size_t(filecache->Size)));
      UnloadFile(filecache);
      return Self->ParseError;
   }
   else return Self->ParseError;
}

//********************************************************************************************************************

static ERR JSON_Init(objXML *Self)
{
   kt::Log log;

   log.trace("Attempting JSON interpretation of source data.");

   std::string_view statement;
   if (!Self->getStatementView(statement)) {
      if ((Self->ParseError = txt_to_json(Self, statement)) != ERR::Okay) {
         log.warning("JSON Parsing Error: %s", GetErrorMsg(Self->ParseError));
      }

      return Self->ParseError;
   }

   std::string_view location;
   Self->getPath(location);
   if (location.empty() or ((Self->Flags & XMF::NEW) != XMF::NIL)) {
      // If no location has been specified, assume that the JSON source is being
      // created from scratch (e.g. to save to disk).

      return ERR::Okay;
   }
   else if ((Self->ParseError = load_file(Self, location)) != ERR::Okay) {
      log.warning("Parsing Error: %s [File: %.*s]", GetErrorMsg(Self->ParseError), int(location.size()), location.data());
      return Self->ParseError;
   }
   else return ERR::Okay;

   return ERR::NoSupport;
}

//********************************************************************************************************************

enum class JSONValueKind {
   Object,
   Array,
   String,
   Integer,
   Number,
   Boolean,
   Null
};

static std::string_view json_kind_name(JSONValueKind Kind) noexcept
{
   switch (Kind) {
      case JSONValueKind::Object:  return "object";
      case JSONValueKind::Array:   return "array";
      case JSONValueKind::String:  return "string";
      case JSONValueKind::Integer: return "integer";
      case JSONValueKind::Number:  return "number";
      case JSONValueKind::Boolean: return "boolean";
      case JSONValueKind::Null:    return "null";
   }

   return "null";
}

struct JSONParser {
   CSTRING Current;
   CSTRING End;
   int Line;
   int NextTagID;

   JSONParser(std::string_view Text) noexcept :
      Current(Text.data()), End(Text.data() + Text.size()), Line(1), NextTagID(1) { }

   bool at_end() const noexcept { return Current >= End; }

   bool has(size_t Count) const noexcept { return size_t(End - Current) >= Count; }

   bool at_value_boundary() const noexcept
   {
      if (at_end()) return true;
      return (*Current IS ',') or (*Current IS ']') or (*Current IS '}') or (*Current IS ' ') or
         (*Current IS '\t') or (*Current IS '\r') or (*Current IS '\n');
   }

   static int hex_digit(char Value) noexcept
   {
      if ((Value >= '0') and (Value <= '9')) return Value - '0';
      if ((Value >= 'A') and (Value <= 'F')) return Value - 'A' + 10;
      if ((Value >= 'a') and (Value <= 'f')) return Value - 'a' + 10;
      return -1;
   }

   ERR parse_hex_quad(uint32_t &Result) noexcept
   {
      if (not has(4)) return ERR::Syntax;

      uint32_t value = 0;
      for (unsigned i=0; i < 4; i++) {
         int digit = hex_digit(Current[i]);
         if (digit < 0) return ERR::Syntax;
         value = (value << 4) | uint32_t(digit);
      }

      Current += 4;
      Result = value;
      return ERR::Okay;
   }

   static ERR append_utf8(uint32_t CodePoint, std::string &Result)
   {
      bool valid_xml = (CodePoint IS 0x09) or (CodePoint IS 0x0a) or (CodePoint IS 0x0d) or
         ((CodePoint >= 0x20) and (CodePoint <= 0xd7ff)) or
         ((CodePoint >= 0xe000) and (CodePoint <= 0xfffd)) or
         ((CodePoint >= 0x10000) and (CodePoint <= 0x10ffff));
      if (not valid_xml) return ERR::Syntax;

      if (CodePoint <= 0x7f) Result += char(CodePoint);
      else if (CodePoint <= 0x7ff) {
         Result += char(0xc0 | (CodePoint >> 6));
         Result += char(0x80 | (CodePoint & 0x3f));
      }
      else if (CodePoint <= 0xffff) {
         Result += char(0xe0 | (CodePoint >> 12));
         Result += char(0x80 | ((CodePoint >> 6) & 0x3f));
         Result += char(0x80 | (CodePoint & 0x3f));
      }
      else {
         Result += char(0xf0 | (CodePoint >> 18));
         Result += char(0x80 | ((CodePoint >> 12) & 0x3f));
         Result += char(0x80 | ((CodePoint >> 6) & 0x3f));
         Result += char(0x80 | (CodePoint & 0x3f));
      }

      return ERR::Okay;
   }

   void skip_whitespace() noexcept
   {
      while (Current < End) {
         if ((*Current IS ' ') or (*Current IS '\t') or (*Current IS '\r')) Current++;
         else if (*Current IS '\n') { Line++; Current++; }
         else break;
      }
   }

   ERR parse_string(std::string &Result)
   {
      if (at_end() or (*Current != '"')) return ERR::Syntax;
      Current++;

      while (Current < End) {
         if (*Current IS '"') {
            Current++;
            return ERR::Okay;
         }

         if (*Current IS '\\') {
            Current++;
            if (at_end() or (not *Current)) return ERR::Syntax;

            if (*Current IS '"') Result += '"';
            else if (*Current IS '\\') Result += '\\';
            else if (*Current IS '/') Result += '/';
            else if ((*Current IS 'b') or (*Current IS 'f')) return ERR::Syntax;
            else if (*Current IS 'n') Result += '\n';
            else if (*Current IS 'r') Result += '\r';
            else if (*Current IS 't') Result += '\t';
            else if (*Current IS 'u') {
               Current++;
               uint32_t code_point;
               if (parse_hex_quad(code_point) != ERR::Okay) return ERR::Syntax;

               if ((code_point >= 0xd800) and (code_point <= 0xdbff)) {
                  if ((not has(2)) or (Current[0] != '\\') or (Current[1] != 'u')) return ERR::Syntax;
                  Current += 2;

                  uint32_t low_surrogate;
                  if (parse_hex_quad(low_surrogate) != ERR::Okay) return ERR::Syntax;
                  if ((low_surrogate < 0xdc00) or (low_surrogate > 0xdfff)) return ERR::Syntax;
                  code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low_surrogate - 0xdc00);
               }
               else if ((code_point >= 0xdc00) and (code_point <= 0xdfff)) return ERR::Syntax;

               if (append_utf8(code_point, Result) != ERR::Okay) return ERR::Syntax;
               continue;
            }
            else return ERR::Syntax;

            Current++;
         }
         else {
            auto length = string_segment_length(Current, End);
            if (not length) return ERR::Syntax;

            auto segment = std::string_view(Current, length);
            auto control = std::find_if(segment.begin(), segment.end(), [](unsigned char Char) { return Char < 0x20; });
            if (control != segment.end()) return ERR::Syntax;

            Result.append(segment);
            Current += length;
         }
      }

      return ERR::Syntax;
   }

   ERR parse_hex_number(std::string &Result, JSONValueKind &Kind)
   {
      auto number_start = Current;
      Kind = JSONValueKind::Integer;

      Current += 2;
      auto digit_start = Current;
      while ((Current < End) and (((*Current >= '0') and (*Current <= '9')) or
         ((*Current >= 'A') and (*Current <= 'F')) or ((*Current >= 'a') and (*Current <= 'f')))) Current++;
      if ((Current IS digit_start) or (not at_value_boundary())) return ERR::Syntax;

      Result.assign(number_start, size_t(Current - number_start));
      return ERR::Okay;
   }

   ERR parse_standard_number(std::string &Result, JSONValueKind &Kind)
   {
      auto number_start = Current;
      Kind = JSONValueKind::Integer;

      if (*Current IS '-') {
         Current++;
         if (at_end()) return ERR::Syntax;
      }

      if (*Current IS '0') {
         Current++;
         if ((Current < End) and (*Current >= '0') and (*Current <= '9')) return ERR::Syntax;
      }
      else {
         if ((*Current < '1') or (*Current > '9')) return ERR::Syntax;
         while ((Current < End) and (*Current >= '0') and (*Current <= '9')) Current++;
      }

      if ((Current < End) and (*Current IS '.')) {
         Kind = JSONValueKind::Number;
         Current++;
         auto fraction_start = Current;
         while ((Current < End) and (*Current >= '0') and (*Current <= '9')) Current++;
         if (Current IS fraction_start) return ERR::Syntax;
      }

      if ((Current < End) and ((*Current IS 'e') or (*Current IS 'E'))) {
         Kind = JSONValueKind::Number;
         Current++;
         if ((Current < End) and ((*Current IS '+') or (*Current IS '-'))) Current++;

         auto exponent_start = Current;
         while ((Current < End) and (*Current >= '0') and (*Current <= '9')) Current++;
         if (Current IS exponent_start) return ERR::Syntax;
      }

      if (not at_value_boundary()) return ERR::Syntax;
      Result.assign(number_start, size_t(Current - number_start));
      return ERR::Okay;
   }

   ERR parse_number(std::string &Result, JSONValueKind &Kind)
   {
      if (has(2) and (Current[0] IS '0') and (Current[1] IS 'x')) return parse_hex_number(Result, Kind);
      return parse_standard_number(Result, Kind);
   }

   ERR parse_literal(std::string &Result, JSONValueKind &Kind)
   {
      std::string_view literal;
      if (*Current IS 't') {
         literal = "true";
         Kind = JSONValueKind::Boolean;
      }
      else if (*Current IS 'f') {
         literal = "false";
         Kind = JSONValueKind::Boolean;
      }
      else if (*Current IS 'n') {
         literal = "null";
         Kind = JSONValueKind::Null;
      }
      else return ERR::Syntax;

      if ((size_t(End - Current) < literal.size()) or
          (std::string_view(Current, literal.size()) != literal)) return ERR::Syntax;

      Current += literal.size();
      if (not at_value_boundary()) return ERR::Syntax;
      Result.assign(literal);
      return ERR::Okay;
   }

   ERR parse_member(objXML::TAGS &Tags)
   {
      std::string item_name;
      if (parse_string(item_name) != ERR::Okay) return ERR::Syntax;

      skip_whitespace();
      if (at_end() or (*Current != ':')) return ERR::Syntax;
      Current++;
      skip_whitespace();

      JSONValueKind kind;
      return parse_value(Tags, &item_name, kind);
   }

   ERR parse_object(objXML::TAGS &Children)
   {
      if (at_end() or (*Current != '{')) return ERR::Syntax;
      Current++;
      skip_whitespace();

      if ((Current < End) and (*Current IS '}')) {
         Current++;
         return ERR::Okay;
      }

      while (Current < End) {
         if (parse_member(Children) != ERR::Okay) return ERR::Syntax;
         skip_whitespace();
         if (at_end()) return ERR::Syntax;

         if (*Current IS '}') {
            Current++;
            return ERR::Okay;
         }

         if (*Current != ',') return ERR::Syntax;
         Current++;
         skip_whitespace();
         if (at_end() or (*Current IS '}')) return ERR::Syntax;
      }

      return ERR::Syntax;
   }

   ERR parse_array(objXML::TAGS &Children, std::string_view &Subtype)
   {
      if (at_end() or (*Current != '[')) return ERR::Syntax;
      Current++;
      skip_whitespace();

      if ((Current < End) and (*Current IS ']')) {
         Current++;
         Subtype = "null";
         return ERR::Okay;
      }

      bool first_value = true;
      JSONValueKind common_kind = JSONValueKind::Null;
      bool mixed = false;

      while (Current < End) {
         JSONValueKind value_kind;
         if (parse_value(Children, nullptr, value_kind) != ERR::Okay) return ERR::Syntax;

         if (first_value) {
            common_kind = value_kind;
            first_value = false;
         }
         else if (value_kind != common_kind) mixed = true;

         skip_whitespace();
         if (at_end()) return ERR::Syntax;

         if (*Current IS ']') {
            Current++;
            Subtype = mixed ? std::string_view("mixed") : json_kind_name(common_kind);
            return ERR::Okay;
         }

         if (*Current != ',') return ERR::Syntax;
         Current++;
         skip_whitespace();
         if (at_end() or (*Current IS ']')) return ERR::Syntax;
      }

      return ERR::Syntax;
   }

   ERR parse_value(objXML::TAGS &Tags, const std::string *Name, JSONValueKind &Kind)
   {
      if (at_end() or (not *Current)) return ERR::Syntax;

      int line_no = Line;
      bool array_element = not Name;
      if (*Current IS '{') {
         Kind = JSONValueKind::Object;
         auto &object_tag = Tags.emplace_back(XTag(NextTagID++, line_no));
         object_tag.Attribs.reserve(Name ? 3 : 2);
         object_tag.Attribs.emplace_back("item", "");
         if (Name) object_tag.Attribs.emplace_back("name", *Name);
         object_tag.Attribs.emplace_back("type", "object");
         return parse_object(object_tag.Children);
      }

      if (*Current IS '[') {
         Kind = JSONValueKind::Array;
         auto &array_tag = Tags.emplace_back(XTag(NextTagID++, line_no));
         array_tag.Attribs.reserve(Name ? 4 : 3);
         array_tag.Attribs.emplace_back("item", "");
         if (Name) array_tag.Attribs.emplace_back("name", *Name);
         array_tag.Attribs.emplace_back("type", "array");

         std::string_view subtype;
         auto error = parse_array(array_tag.Children, subtype);
         if (error != ERR::Okay) return error;
         array_tag.Attribs.emplace_back("subtype", subtype);
         return ERR::Okay;
      }

      std::string content;
      if (*Current IS '"') {
         Kind = JSONValueKind::String;
         if (parse_string(content) != ERR::Okay) return ERR::Syntax;
      }
      else if ((*Current IS '-') or ((*Current >= '0') and (*Current <= '9'))) {
         if (parse_number(content, Kind) != ERR::Okay) return ERR::Syntax;
      }
      else if ((*Current IS 't') or (*Current IS 'f') or (*Current IS 'n')) {
         if (parse_literal(content, Kind) != ERR::Okay) return ERR::Syntax;
      }
      else return ERR::Syntax;

      auto &value_tag = Tags.emplace_back(XTag(NextTagID++, line_no));
      if (array_element) {
         value_tag.Attribs.reserve(2);
         value_tag.Attribs.emplace_back("value", "");
         value_tag.Attribs.emplace_back("type", json_kind_name(Kind));
      }
      else {
         value_tag.Attribs.reserve(3);
         value_tag.Attribs.emplace_back("item", "");
         value_tag.Attribs.emplace_back("name", *Name);
         value_tag.Attribs.emplace_back("type", json_kind_name(Kind));
      }

      if (Kind != JSONValueKind::Null) {
         auto &content_tag = value_tag.Children.emplace_back(XTag(NextTagID++, Line));
         content_tag.Attribs.emplace_back("", std::move(content));
      }

      return ERR::Okay;
   }
};

//********************************************************************************************************************

static bool valid_utf8(std::string_view Text) noexcept
{
   for (size_t i=0; i < Text.size();) {
      auto first = uint8_t(Text[i]);
      if (first <= 0x7f) {
         i++;
         continue;
      }

      size_t count;
      if ((first >= 0xc2) and (first <= 0xdf)) count = 2;
      else if ((first >= 0xe0) and (first <= 0xef)) count = 3;
      else if ((first >= 0xf0) and (first <= 0xf4)) count = 4;
      else return false;

      if (i + count > Text.size()) return false;
      for (size_t j=1; j < count; j++) {
         auto byte = uint8_t(Text[i + j]);
         if ((byte < 0x80) or (byte > 0xbf)) return false;
      }

      auto second = uint8_t(Text[i + 1]);
      if ((first IS 0xe0) and (second < 0xa0)) return false;
      if ((first IS 0xed) and (second > 0x9f)) return false;
      if ((first IS 0xf0) and (second < 0x90)) return false;
      if ((first IS 0xf4) and (second > 0x8f)) return false;
      i += count;
   }

   return true;
}

//********************************************************************************************************************

static ERR find_attribute(const XTag &Tag, std::string_view Name, const std::string *&Result, bool &Present)
{
   Result = nullptr;
   Present = false;
   if (Tag.Attribs.empty()) return ERR::InvalidData;

   for (size_t i=1; i < Tag.Attribs.size(); i++) {
      if (Tag.Attribs[i].Name IS Name) {
         if (Present) return ERR::InvalidData;
         Result = &Tag.Attribs[i].Value;
         Present = true;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR required_attribute(const XTag &Tag, std::string_view Name, const std::string *&Result)
{
   bool present;
   if (auto error = find_attribute(Tag, Name, Result, present); error != ERR::Okay) return error;
   return present ? ERR::Okay : ERR::InvalidData;
}

//********************************************************************************************************************

static ERR scalar_content(const XTag &Tag, std::string &Result, bool &HasElements)
{
   Result.clear();
   HasElements = false;

   for (auto &child : Tag.Children) {
      if (child.Attribs.empty()) return ERR::InvalidData;
      if (child.Attribs[0].Name.empty()) {
         if ((child.Attribs.size() != 1) or (not child.Children.empty())) return ERR::InvalidData;
         Result += child.Attribs[0].Value;
      }
      else HasElements = true;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR json_kind(std::string_view Name, JSONValueKind &Result)
{
   if (Name IS "object") Result = JSONValueKind::Object;
   else if (Name IS "array") Result = JSONValueKind::Array;
   else if (Name IS "string") Result = JSONValueKind::String;
   else if (Name IS "integer") Result = JSONValueKind::Integer;
   else if (Name IS "number") Result = JSONValueKind::Number;
   else if (Name IS "boolean") Result = JSONValueKind::Boolean;
   else if (Name IS "null") Result = JSONValueKind::Null;
   else return ERR::InvalidData;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR validate_number(std::string_view Value, JSONValueKind Expected)
{
   if (Value.empty()) return ERR::InvalidData;

   JSONParser parser(Value);
   JSONValueKind kind;
   std::string parsed;
   if ((parser.parse_number(parsed, kind) != ERR::Okay) or (not parser.at_end()) or (kind != Expected)) {
      return ERR::InvalidData;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR write_native_object(const XTag &Tag, JSONOutput &Output, int Depth)
{
   Output.Text += '{';
   if (Tag.Children.empty()) {
      Output.Text += '}';
      return ERR::Okay;
   }

   for (size_t i=0; i < Tag.Children.size(); i++) {
      auto &child = Tag.Children[i];
      if (child.Attribs.empty() or (child.Attribs[0].Name != "item")) return ERR::InvalidData;

      const std::string *name;
      if (required_attribute(child, "name", name) != ERR::Okay or name->empty()) return ERR::InvalidData;

      if (i) Output.Text += ',';
      Output.line(Depth + 1);
      if (auto error = Output.quoted(*name); error != ERR::Okay) return error;
      Output.colon();
      if (auto error = write_native_value(child, Output, Depth + 1); error != ERR::Okay) return error;
   }

   Output.line(Depth);
   Output.Text += '}';
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR write_native_array(const XTag &Tag, JSONOutput &Output, int Depth)
{
   Output.Text += '[';
   if (Tag.Children.empty()) {
      Output.Text += ']';
      return ERR::Okay;
   }

   for (size_t i=0; i < Tag.Children.size(); i++) {
      auto &child = Tag.Children[i];
      if (child.Attribs.empty()) return ERR::InvalidData;

      const std::string *type;
      if (required_attribute(child, "type", type) != ERR::Okay) return ERR::InvalidData;
      JSONValueKind kind;
      if (json_kind(*type, kind) != ERR::Okay) return ERR::InvalidData;

      bool compound = (kind IS JSONValueKind::Object) or (kind IS JSONValueKind::Array);
      if ((compound and (child.Attribs[0].Name != "item")) or
          ((not compound) and (child.Attribs[0].Name != "value"))) return ERR::InvalidData;

      const std::string *name;
      bool has_name;
      if (find_attribute(child, "name", name, has_name) != ERR::Okay or has_name) return ERR::InvalidData;

      if (i) Output.Text += ',';
      Output.line(Depth + 1);
      if (auto error = write_native_value(child, Output, Depth + 1); error != ERR::Okay) return error;
   }

   Output.line(Depth);
   Output.Text += ']';
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR write_native_value(const XTag &Tag, JSONOutput &Output, int Depth)
{
   const std::string *type;
   if (required_attribute(Tag, "type", type) != ERR::Okay) return ERR::InvalidData;

   JSONValueKind kind;
   if (json_kind(*type, kind) != ERR::Okay) return ERR::InvalidData;

   if (kind IS JSONValueKind::Object) return write_native_object(Tag, Output, Depth);
   if (kind IS JSONValueKind::Array) return write_native_array(Tag, Output, Depth);

   std::string content;
   bool has_elements;
   if (scalar_content(Tag, content, has_elements) != ERR::Okay or has_elements) return ERR::InvalidData;

   switch (kind) {
      case JSONValueKind::String:
         return Output.quoted(content);
      case JSONValueKind::Integer:
      case JSONValueKind::Number:
         if (validate_number(content, kind) != ERR::Okay) return ERR::InvalidData;
         Output.Text += content;
         return ERR::Okay;
      case JSONValueKind::Boolean:
         if ((content != "true") and (content != "false")) return ERR::InvalidData;
         Output.Text += content;
         return ERR::Okay;
      case JSONValueKind::Null:
         if (not content.empty()) return ERR::InvalidData;
         Output.Text += "null";
         return ERR::Okay;
      case JSONValueKind::Object:
      case JSONValueKind::Array:
         return ERR::InvalidData;
   }

   return ERR::InvalidData;
}

//********************************************************************************************************************

static bool xml_metadata(const XTag &Tag) noexcept
{
   if (Tag.Attribs.empty()) return false;
   if ((Tag.Flags & XTF::CDATA) != XTF::NIL) return false;
   if ((Tag.Flags & XTF::COMMENT) != XTF::NIL) return true;
   if ((Tag.Flags & XTF::INSTRUCTION) != XTF::NIL) return true;
   if ((Tag.Flags & XTF::NOTATION) != XTF::NIL) return true;
   if (Tag.Attribs[0].Name.empty()) return false;
   return (Tag.Attribs[0].Name[0] IS '?') or (Tag.Attribs[0].Name[0] IS '!');
}

//********************************************************************************************************************

static bool whitespace_only(std::string_view Text) noexcept
{
   return std::all_of(Text.begin(), Text.end(), [](unsigned char Char) {
      return (Char IS ' ') or (Char IS '\t') or (Char IS '\r') or (Char IS '\n');
   });
}

//********************************************************************************************************************

static ERR collect_xml_element(const XTag &Tag, std::string &Text, std::vector<XMLGroup> &Groups)
{
   Text.clear();
   Groups.clear();

   for (auto &child : Tag.Children) {
      if (child.Attribs.empty()) return ERR::InvalidData;
      if (xml_metadata(child)) continue;
      if (child.Attribs[0].Name.empty()) {
         if (not child.Children.empty()) return ERR::InvalidData;
         Text += child.Attribs[0].Value;
         continue;
      }

      auto group = std::find_if(Groups.begin(), Groups.end(), [&child](const XMLGroup &Group) {
         return Group.Name IS child.Attribs[0].Name;
      });
      if (group IS Groups.end()) Groups.push_back({ child.Attribs[0].Name, { &child } });
      else group->Tags.push_back(&child);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR write_xml_group(const XMLGroup &Group, JSONOutput &Output, int Depth)
{
   if (Group.Tags.size() IS 1) return write_xml_element(*Group.Tags[0], Output, Depth);

   Output.Text += '[';
   for (size_t i=0; i < Group.Tags.size(); i++) {
      if (i) Output.Text += ',';
      Output.line(Depth + 1);
      if (auto error = write_xml_element(*Group.Tags[i], Output, Depth + 1); error != ERR::Okay) return error;
   }
   Output.line(Depth);
   Output.Text += ']';
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR write_xml_element(const XTag &Tag, JSONOutput &Output, int Depth)
{
   if (Tag.Attribs.empty() or Tag.Attribs[0].Name.empty()) return ERR::InvalidData;

   std::string content;
   std::vector<XMLGroup> groups;
   if (auto error = collect_xml_element(Tag, content, groups); error != ERR::Okay) return error;

   if ((Tag.Attribs.size() IS 1) and groups.empty()) return Output.quoted(content);

   std::vector<std::string> property_names;
   property_names.reserve(Tag.Attribs.size() + groups.size());
   for (size_t i=1; i < Tag.Attribs.size(); i++) {
      if (Tag.Attribs[i].Name.empty()) return ERR::InvalidData;
      std::string property = "@" + Tag.Attribs[i].Name;
      if (std::find(property_names.begin(), property_names.end(), property) != property_names.end()) {
         return ERR::InvalidData;
      }
      property_names.push_back(std::move(property));
   }

   bool has_text = (not content.empty()) and (groups.empty() or (not whitespace_only(content)));
   if (has_text) property_names.emplace_back("#text");

   for (auto &group : groups) {
      if (std::find(property_names.begin(), property_names.end(), group.Name) != property_names.end()) {
         return ERR::InvalidData;
      }
      property_names.emplace_back(group.Name);
   }

   Output.Text += '{';
   size_t member = 0;
   auto begin_member = [&Output, &member, Depth](std::string_view Name) -> ERR {
      if (member++) Output.Text += ',';
      Output.line(Depth + 1);
      if (auto error = Output.quoted(Name); error != ERR::Okay) return error;
      Output.colon();
      return ERR::Okay;
   };

   for (size_t i=1; i < Tag.Attribs.size(); i++) {
      std::string name = "@" + Tag.Attribs[i].Name;
      if (auto error = begin_member(name); error != ERR::Okay) return error;
      if (auto error = Output.quoted(Tag.Attribs[i].Value); error != ERR::Okay) return error;
   }

   if (has_text) {
      if (auto error = begin_member("#text"); error != ERR::Okay) return error;
      if (auto error = Output.quoted(content); error != ERR::Okay) return error;
   }

   for (auto &group : groups) {
      if (auto error = begin_member(group.Name); error != ERR::Okay) return error;
      if (auto error = write_xml_group(group, Output, Depth + 1); error != ERR::Okay) return error;
   }

   Output.line(Depth);
   Output.Text += '}';
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR write_xml_document(const objXML::TAGS &Tags, JSONOutput &Output)
{
   std::vector<XMLGroup> groups;
   for (auto &tag : Tags) {
      if (tag.Attribs.empty()) return ERR::InvalidData;
      if (xml_metadata(tag) or tag.Attribs[0].Name.empty()) continue;

      auto group = std::find_if(groups.begin(), groups.end(), [&tag](const XMLGroup &Group) {
         return Group.Name IS tag.Attribs[0].Name;
      });
      if (group IS groups.end()) groups.push_back({ tag.Attribs[0].Name, { &tag } });
      else group->Tags.push_back(&tag);
   }

   Output.Text += '{';
   for (size_t i=0; i < groups.size(); i++) {
      if (i) Output.Text += ',';
      Output.line(1);
      if (auto error = Output.quoted(groups[i].Name); error != ERR::Okay) return error;
      Output.colon();
      if (auto error = write_xml_group(groups[i], Output, 1); error != ERR::Okay) return error;
   }
   if (not groups.empty()) Output.line(0);
   Output.Text += '}';
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
SaveToObject: Serialises JSON or XML-backed data as JSON and writes it to another object.

Native JSON objects preserve their parsed JSON types and source number lexemes.  A base @XML object is transcribed to
JSON using element names as properties, `@`-prefixed attribute properties, `#text` for mixed content, and arrays for
repeated sibling names.  Attribute values and XML text remain strings.

Set #Flags to `XMF::READABLE` for three-space indentation and a final line-feed.  Otherwise compact JSON is written.
The complete tree is validated before one write is attempted, so invalid source data does not partially modify the
destination.

-ERRORS-
Okay: The JSON data was written successfully, or the source tree contained no data.
NullArgs: The action parameters or destination object were not supplied.
InvalidData: The source tree is structurally invalid or contains invalid UTF-8 or scalar data.
BufferOverflow: The serialised output is too large for the destination action.
Write: The destination rejected the output or performed a short write.

-END-

*********************************************************************************************************************/

static ERR JSON_SaveToObject(objXML *Self, struct acSaveToObject *Args)
{
   if ((not Args) or (not Args->Dest)) return ERR::NullArgs;
   if (Self->Tags.empty()) return ERR::Okay;

   JSONOutput output((Self->Flags & XMF::READABLE) != XMF::NIL);
   ERR error;
   if (Self->isDerived()) {
      if (Self->Tags.size() != 1) return ERR::InvalidData;

      auto &root = Self->Tags[0];
      if (root.Attribs.empty() or (root.Attribs[0].Name != "item")) return ERR::InvalidData;

      const std::string *name;
      bool has_name;
      if (find_attribute(root, "name", name, has_name) != ERR::Okay or has_name) return ERR::InvalidData;

      const std::string *type;
      if ((required_attribute(root, "type", type) != ERR::Okay) or (*type != "object")) return ERR::InvalidData;
      error = write_native_value(root, output, 0);
   }
   else error = write_xml_document(Self->Tags, output);

   if (error != ERR::Okay) return error;
   if (output.Readable) output.Text += '\n';
   if (output.Text.size() > size_t(std::numeric_limits<int>::max())) return ERR::BufferOverflow;

   int written = 0;
   auto bytes = std::span<const int8_t>((const int8_t *)output.Text.data(), output.Text.size());
   if ((acWrite(Args->Dest, bytes, &written) != ERR::Okay) or (written != int(output.Text.size()))) return ERR::Write;
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse a bounded text string into the JSON class's XML representation.

static ERR txt_to_json(objXML *Self, std::string_view Text)
{
   kt::Log log;

   if ((!Self) or (Text.empty())) return ERR::NullArgs;

   log.traceBranch();

   Self->Tags.clear();
   Self->LineNo = 1;
   JSONParser parser(Text);
   parser.skip_whitespace();
   if (parser.at_end()) return log.warning(ERR::NoData);
   if (*parser.Current != '{') return log.warning((*parser.Current IS 0) ? ERR::Syntax : ERR::NoData);

   auto &root = Self->Tags.emplace_back(XTag(0, parser.Line));
   root.Attribs.reserve(2);
   root.Attribs.emplace_back("item", "");
   root.Attribs.emplace_back("type", "object");

   auto error = parser.parse_object(root.Children);
   parser.skip_whitespace();
   Self->LineNo = parser.Line;
   if ((error != ERR::Okay) or (not parser.at_end())) {
      Self->Tags.clear();
      log.warning("Malformed JSON statement detected at line %d.", parser.Line);
      return ERR::Syntax;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, nullptr, nullptr)
extern "C" struct ModHeader * register_json_module() { return &ModHeader; }
