/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
JSON: Extends the XML class with JSON support.

The JSON class is an extension for the @XML class.  It allows JSON data to be loaded into an XML tree, where
it can be manipulated and scanned using XML based functions.  This approach is advantageous in that the simplicity of
the JSON is maintained, yet advanced features such as XPath lookups can be used to inspect the data.

It is important to understand how JSON data is converted to the XML tree structure.  All JSON values will be
represented as 'item' tags that describe the name and type of value that is being represented.  Each value will be
stored as content in the corresponding item tag.  Arrays are stored as items that contain a series of value tags, in
the case of strings and numbers, or object tags.

-EXAMPLE-
The following example illustrates a JSON structure containing the common datatypes:

{ "string":"foo bar",
  "array":[ 0, 1, 2 ],
  "array2":[ { "ABC":"XYZ" },
             { "DEF":"XYZ" } ]
}

It will be translated to the following when loaded into an XML object:

&lt;item type="object"&gt;
  &lt;item name="string" type="string"&gt;foo bar&lt;/item&gt;

  &lt;item name="array" type="array" subtype="integer"&gt;
    &lt;value&gt;0&lt;/value&gt;
    &lt;value&gt;1&lt;/value&gt;
    &lt;value&gt;2&lt;/value&gt;
  &lt;/item&gt;

  &lt;item name="array2" type="array" subtype="object"&gt;
    &lt;item type="object"&gt;&lt;item name="ABC" type="string" value="XYZ"/&gt;&lt;/item&gt;
    &lt;item type="object"&gt;&lt;item name="DEF" type="string" value="XYZ"/&gt;&lt;/item&gt;
  &lt;/item&gt;
&lt;item&gt;

-END-

*********************************************************************************************************************/

#undef DEBUG
//#define DEBUG

#define PRV_XML
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>
#include <algorithm>
#include <utility>

JUMPTABLE_CORE

static OBJECTPTR clJSON = nullptr;

static ERR JSON_Init(objXML *);
static ERR JSON_SaveToObject(objXML *, struct acSaveToObject *);

static ActionArray clActions[] = {
   { AC::Init,         JSON_Init },
   { AC::SaveToObject, JSON_SaveToObject },
   { AC::NIL, nullptr }
};

static ERR txt_to_json(objXML *, std::string_view);

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
// Debug routines.

#if defined(DEBUG)

static void debug_tree(objXML *Self)
{
   kt::Log log("Tree");
   int i, j;
   char buffer[1000];

   for (int index=0; index < int(Tags.size()); index++) {
      XTag &Tag = Tags[index];

      //for (i=0; i < Tag.Branch; i++) buffer[i] = ' '; // Indenting
      //buffer[i] = 0;

      if (Tag.Attrib) {
         if (Tag.Attrib->Name) {
            log.msg("%.3d/%.3d: %p<-%p->%p Child %p %s%s", index, Tag.Index, Tag.Prev, Tag, Tag.Next, Tag.Child, buffer, Tag.Attrib->Name ? Tag.Attrib->Name : "Content");
         }
         else {
            // Extract a limited amount of content
            for (j=0; (Tag.Attrib->Value[j]) and (j < 16) and ((size_t)i < sizeof(buffer)); j++) {
               if (Tag.Attrib->Value[j] IS '\n') buffer[i++] = '.';
               else buffer[i++] = Tag.Attrib->Value[j];
            }
            if (i) buffer[i] = 0;
            else StrCopy("<Empty Content>", buffer, sizeof(buffer));
            log.msg("%.3d/%.3d: %p<-%p->%p Child %p %s", index, Tag.Index, Tag.Prev, Tag, Tag.Next, Tag.Child, buffer);
            //log.msg("%.3d: %s", index, buffer);
         }
      }
   }
}

#endif

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

      #ifdef DEBUG
      debug_tree(Self);
      #endif

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

static ERR JSON_SaveToObject(objXML *Self, struct acSaveToObject *Args)
{
   if (!Args) return ERR::NullArgs;

   return ERR::Okay;
}

//********************************************************************************************************************
enum class JSONValueKind {
   Object,
   Array,
   String,
   Integer,
   Number,
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

            if (*Current IS 'n') Result += '\n';
            else if (*Current IS 'r') Result += '\r';
            else if (*Current IS 't') Result += '\t';
            else if (*Current IS '"') Result += '"';
            else { Result += '\\'; Result += *Current; }
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

   ERR parse_number(std::string &Result, JSONValueKind &Kind)
   {
      auto number_start = Current;
      Kind = JSONValueKind::Integer;

      if (has(2) and (Current[0] IS '0') and (Current[1] IS 'x')) {
         Current += 2;
         auto digit_start = Current;
         while ((Current < End) and (((*Current >= '0') and (*Current <= '9')) or
            ((*Current >= 'A') and (*Current <= 'F')) or ((*Current >= 'a') and (*Current <= 'f')))) Current++;
         if (Current IS digit_start) return ERR::Syntax;
      }
      else {
         if ((Current < End) and (*Current IS '-')) Current++;
         auto digit_start = Current;
         while ((Current < End) and (*Current >= '0') and (*Current <= '9')) Current++;
         if (Current IS digit_start) return ERR::Syntax;

         if ((Current < End) and (*Current IS '.')) {
            Kind = JSONValueKind::Number;
            Current++;
            auto fraction_start = Current;
            while ((Current < End) and (*Current >= '0') and (*Current <= '9')) Current++;
            if (Current IS fraction_start) return ERR::Syntax;
         }
      }

      Result.assign(number_start, size_t(Current - number_start));
      return ERR::Okay;
   }

   ERR parse_literal(std::string &Result, JSONValueKind &Kind)
   {
      constexpr std::string_view null_literal = "null";
      if ((size_t(End - Current) < null_literal.size()) or
          (std::string_view(Current, null_literal.size()) != null_literal)) return ERR::Syntax;

      Current += null_literal.size();
      Result.assign(null_literal);
      Kind = JSONValueKind::Null;
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
      else if (*Current IS 'n') {
         if (parse_literal(content, Kind) != ERR::Okay) return ERR::Syntax;
      }
      else return ERR::Syntax;

      auto &value_tag = Tags.emplace_back(XTag(NextTagID++, line_no));
      if (array_element) value_tag.Attribs.emplace_back("value", "");
      else {
         value_tag.Attribs.reserve(3);
         value_tag.Attribs.emplace_back("item", "");
         value_tag.Attribs.emplace_back("name", *Name);
         value_tag.Attribs.emplace_back("type", Kind IS JSONValueKind::Integer ? "number" : json_kind_name(Kind));
      }

      if (Kind != JSONValueKind::Null) {
         auto &content_tag = value_tag.Children.emplace_back(XTag(NextTagID++, Line));
         content_tag.Attribs.emplace_back("", std::move(content));
      }

      return ERR::Okay;
   }
};

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
