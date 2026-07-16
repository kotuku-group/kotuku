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

static ERR extract_item(int &, CSTRING *, CSTRING, objXML::TAGS &, int &);
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

static ERR next_item(int &Line, CSTRING &Input)
{
   while ((*Input) and (*Input <= 0x20)) { if (*Input IS '\n') Line++; Input++; }
   if (*Input IS ',') {
      Input++;
      while ((*Input) and (*Input <= 0x20)) { if (*Input IS '\n') Line++; Input++; }
      return ERR::Okay;
   }
   else return ERR::Failed;
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
// Parse Text string to JSON.  Assumes that the incoming string is null terminated.

static ERR txt_to_json(objXML *Self, std::string_view Text)
{
   kt::Log log;

   if ((!Self) or (Text.empty())) return ERR::NullArgs;

   log.traceBranch();

   CSTRING str = Text.data();
   CSTRING text_end = str + Text.size();
   Self->Tags.clear();
   Self->LineNo = 1;
   for (; (*str) and (*str != '{'); str++) if (*str IS '\n') Self->LineNo++;
   if (*str != '{') return log.warning(ERR::NoData);

   log.trace("Extracting tag information with extract_tag()");
   if (*str IS '{') {
      // XML requires the root tag to be numbered with ID 0
      auto &root = Self->Tags.emplace_back(XTag(0, Self->LineNo));
      root.Attribs.reserve(2);
      root.Attribs.emplace_back("item", "");
      root.Attribs.emplace_back("type", "object");

      str++; // Skip '{'
      while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Self->LineNo++; str++; }

      int tag_id = 1;
      do {
         if (extract_item(Self->LineNo, &str, text_end, root.Children, tag_id) != ERR::Okay) {
            return log.warning(ERR::Syntax);
         }
      } while (!next_item(Self->LineNo, str));
   }

   if (*str != '}') {
      log.warning("Missing expected '}' terminator at line %d.", Self->LineNo);
      return ERR::Syntax;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Called by txt_to_json() to extract the next item from a JSON string.  This function also recurses into itself.

static ERR extract_item(int &Line, CSTRING *Input, CSTRING End, objXML::TAGS &Tags, int &TagID)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Line: %d, %.20s", Line, *Input);

   CSTRING str = Input[0];
   if (*str != '"') {
      log.warning("Malformed JSON statement detected at line %d, expected '\"', got '%c'.", Line, str[0]);
      return ERR::Syntax;
   }

   int line_no = Line;
   str++;
   int i = 0;
   std::string item_name;
   while ((str < End) and (*str) and (*str != '"')) {
      if (*str IS '\\') {
         str++;
         if ((str >= End) or (not *str)) {
            log.warning("Invalid use of back-slash in item name encountered at line %d", Line);
            return ERR::Syntax;
         }

         if (*str IS 'n') item_name += '\n';
         else if (*str IS 'r') item_name += '\r';
         else if (*str IS 't') item_name += '\t';
         else if (*str IS '"') item_name += '"';
         else {
            log.warning("Invalid use of back-slash in item name encountered at line %d", Line);
            return ERR::Syntax;
         }
      }
      else {
         auto length = string_segment_length(str, End);
         auto segment = std::string_view(str, length);
         auto control = std::find_if(segment.begin(), segment.end(), [](unsigned char Char) { return Char < 0x20; });
         if (control != segment.end()) {
            log.warning("Invalid item name encountered at line %d.", Line);
            return ERR::Syntax;
         }

         item_name.append(segment);
         str += length;
      }
   }

   if ((str < End) and (*str IS '"')) str++;
   else return ERR::Syntax;

   while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }

   if (*str != ':') {
      log.warning("Missing separator ':' after item name '%s' at line %d.", item_name.c_str(), Line);
      return ERR::Syntax;
   }

   str++; // Skip ':'
   while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }

   if (*str IS '[') {
      int line_start = Line;

      // Evaluates to:
      //
      //    <item name="array" type="array" subtype="type">
      //      <value>val</value>
      //      ...
      //    </item>
      //
      // Except for JSON arrays:
      //
      //    <item name="array" type="array" subtype="object">
      //      <object>...</object>
      //      ...
      //    </item>

      str++; // Skip '['
      while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }

      // Figure out what type of array this is

      std::string_view subtype;
      if (*str IS '{') subtype = "object";
      else if (*str IS '"') subtype = "string";
      else if ((*str >= '0') and (*str <= '9')) subtype = "integer";
      else if (*str IS ']') subtype = "null";
      else {
         log.warning("Invalid array defined at line %d.", line_start);
         return ERR::Syntax;
      }

      log.trace("Processing %.*s array at line %d.", int(subtype.size()), subtype.data(), Line);

      auto &array_tag = Tags.emplace_back(XTag(TagID++, line_no));
      array_tag.Attribs.reserve(4);
      array_tag.Attribs.emplace_back("item", "");
      array_tag.Attribs.emplace_back("name", std::move(item_name));
      array_tag.Attribs.emplace_back("type", "array");
      array_tag.Attribs.emplace_back("subtype", subtype);

      // Read the array values

      if (*str IS '{') {
         while ((*str) and (*str != ']')) {
            // Evaluates to: <object>...</object>

            auto &object_tag = array_tag.Children.emplace_back(XTag(TagID++, line_no));
            object_tag.Attribs.reserve(2);
            object_tag.Attribs.emplace_back("item", "");
            object_tag.Attribs.emplace_back("type", "object");

            if (*str IS '{') {
               log.trace("Processing new object in array.");

               str++; // Skip '{'
               while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }

               if (*str != '}') { // Don't process content if the object is empty.
                  if (auto error = extract_item(Line, &str, End, object_tag.Children, TagID); error != ERR::Okay) {
                     return error;
                  }

                  while ((*str) and (*str != '}')) { if (*str IS '\n') Line++; str++; } // Skip content/whitespace to get to the next tag.

                  if (*str != '}') {
                     log.warning("Missing '}' character to close an object by the end of line %d.", Line);
                     return ERR::Syntax;
                  }

                  // Go to next value, or end of array

                  log.trace("End of object array reached.");

                  str++; // Skip '}'
                  while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
                  if (*str IS ',') {
                     str++;
                     while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
                  }
               }
               else {
                  log.warning("Invalid array entry encountered at line %d, expected object, encountered character '%c'.", Line, *str);
                  return ERR::Syntax;
               }
            }
         }
      }
      else if (*str IS '"') {
         while ((*str) and (*str != ']')) {
            if (*str != '"') {
               log.warning("Invalid array of strings at line %d.", line_start);
               return ERR::Syntax;
            }

            str++; // Skip '"'

            std::string buffer;
            while ((str < End) and (*str) and (*str != '"')) {
               if (*str IS '\\') {
                  str++;
                  if ((str < End) and (*str)) {
                     if (*str IS 'n') buffer += '\n';
                     else if (*str IS 'r') buffer += '\r';
                     else if (*str IS 't') buffer += '\t';
                     else if (*str IS '"') buffer += '"';
                     else { buffer += '\\'; buffer += *str; }
                     str++;
                  }
               }
               else {
                  auto length = string_segment_length(str, End);
                  buffer.append(str, length);
                  str += length;
               }
            }

            // Create <value>string</value>

            if ((str >= End) or (*str != '"')) {
               log.warning("Unterminated string in array at line %d.", line_start);
               return ERR::Syntax;
            }

            auto &value_tag = array_tag.Children.emplace_back(XTag(TagID++, Line));
            value_tag.Attribs.emplace_back("value", "");
            auto &content_tag = value_tag.Children.emplace_back(XTag(TagID++, Line));
            content_tag.Attribs.emplace_back("", std::move(buffer));

            str++; // Skip terminating '"'

            // Go to next value, or end of array

            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
            if (*str IS ',') str++;
            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
         }
      }
      else if (((End - str) >= 2) and (str[0] IS '0') and (str[1] IS 'x')) {
         // Hexadecimal number.

         while ((*str) and (*str != ']')) {
            if (((End - str) < 2) or (str[0] != '0') or (str[1] != 'x')) {
               log.warning("Invalid array of hexadecimal numbers at line %d.", line_start);
               return ERR::Syntax;
            }

            str += 2;
            CSTRING digit_start = str;
            while ((str < End) and (((*str >= '0') and (*str <= '9')) or
               ((*str >= 'A') and (*str <= 'F')) or ((*str >= 'a') and (*str <= 'f')))) str++;

            if (str IS digit_start) {
               log.warning("Invalid array of hexadecimal numbers at line %d.", line_start);
               return ERR::Syntax;
            }

            std::string numbuf("0x");
            numbuf.append(digit_start, size_t(str - digit_start));

            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }

            // Any character other than ',' or ']' after the number makes the hexadecimal value invalid.

            if ((*str != ',') and (*str != ']')) {
               log.warning("Invalid array of hexadecimal numbers at line %d.", line_start);
               return ERR::Syntax;
            }

            // Create <value>number</value>

            auto &value_tag = array_tag.Children.emplace_back(XTag(TagID++, Line));
            value_tag.Attribs.emplace_back("value", "");
            auto &content_tag = value_tag.Children.emplace_back(XTag(TagID++, Line));
            content_tag.Attribs.emplace_back("", std::move(numbuf));

            // Go to next value, or end of array

            if (*str IS ',') str++;
            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
         }
      }
      else if (((*str >= '0') and (*str <= '9')) or (*str IS '-')) {
         while ((*str) and (*str != ']')) {
            if (((*str < '0') or (*str > '9')) and (*str != '-')) {
               log.warning("Invalid array of integers at line %d.", Line);
               return ERR::Syntax;
            }

            CSTRING number_start = str;
            while ((*str IS '-') or (*str IS '.') or ((*str >= '0') and (*str <= '9'))) str++;
            std::string numbuf(number_start, size_t(str - number_start));

            // Create <value>number</value>

            auto &value_tag = array_tag.Children.emplace_back(XTag(TagID++, Line));
            value_tag.Attribs.emplace_back("value", "");
            auto &content_tag = value_tag.Children.emplace_back(XTag(TagID++, Line));
            content_tag.Attribs.emplace_back("", std::move(numbuf));

            // Go to next value, or end of array

            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
            if (*str IS ',') str++;
            while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; }
         }
      }
      else if (*str IS ']') {

      }
      else {
         log.warning("Invalid array defined at line %d.", line_start);
         return ERR::Syntax;
      }

      if (*str != ']') {
         log.warning("Array at line %d not terminated with expected ']' character.", line_start);
         return ERR::Syntax;
      }
      else str++; // Skip array terminator ']'
   }
   else if (*str IS '{') {
      // Evaluates to: <object>...</object>

      log.trace("Item '%s' is an object.", item_name.c_str());

      auto &object_tag = Tags.emplace_back(XTag(TagID++, Line));
      object_tag.Attribs.reserve(3);
      object_tag.Attribs.emplace_back("item", "");
      object_tag.Attribs.emplace_back("name", std::move(item_name));
      object_tag.Attribs.emplace_back("type", "object");

      str++; // Skip '{'
      while ((*str) and (*str <= 0x20)) { if (*str IS '\n') Line++; str++; } // Skip content/whitespace to get to the next tag.

      if (*str != '}') {

         do {
            if (extract_item(Line, &str, End, object_tag.Children, TagID) != ERR::Okay) {
               log.warning("Aborting parsing of JSON statement.");
               return ERR::Syntax;
            }
         } while (!next_item(Line, str));

         while ((*str) and (*str != '}')) { if (*str IS '\n') Line++; str++; } // Skip content/whitespace to get to the next tag.

         if (*str != '}') {
            log.warning("Missing '}' character to close one of the objects.");
            return ERR::Syntax;
         }
         else str++; // Skip '}'
      }
      else log.trace("The object is empty.");
   }
   else if (*str IS '"') {
      // Evaluates to: <item name="item_name" type="string">string</item>

      log.trace("Item '%s' is a string.", item_name.c_str());

      str++; // Skip '"'

      auto &string_tag = Tags.emplace_back(XTag(TagID++, Line));
      string_tag.Attribs.reserve(3);
      string_tag.Attribs.emplace_back("item", "");
      string_tag.Attribs.emplace_back("name", std::move(item_name));
      string_tag.Attribs.emplace_back("type", "string");

      std::string buffer;
      while ((str < End) and (*str) and (*str != '"')) {
         if (*str IS '\\') {
            str++;
            if ((str < End) and (*str)) {
               if (*str IS 'n') buffer += '\n';
               else if (*str IS 'r') buffer += '\r';
               else if (*str IS 't') buffer += '\t';
               else if (*str IS '"') buffer += '"';
               else { buffer += '\\'; buffer += *str; }
               str++;
            }
         }
         else {
            auto length = string_segment_length(str, End);
            auto segment = std::string_view(str, length);
            Line += int(std::count(segment.begin(), segment.end(), '\n'));
            buffer.append(segment);
            str += length;
         }
      }

      if ((str < End) and (*str IS '"')) {
         auto &content_tag = string_tag.Children.emplace_back(XTag(TagID++, Line));
         content_tag.Attribs.emplace_back("", std::move(buffer));
         str++; // Skip '"'
      }
      else return log.warning(ERR::Syntax);
   }
  else if ((str[0] IS '0') and (str[1] IS 'x')) {
      // Evaluates to: <item name="item_name" type="integer">number</item>

      std::string numbuf("0x");
      while (((*str >= '0') and (*str <= '9')) or
         ((*str >= 'A') and (*str <= 'F')) or
         ((*str >= 'a') and (*str <= 'f'))
      ) numbuf += *str++;

      // Skip whitespace and check that the number was valid.
      while (*str) {
         if (*str IS '\n') Line++;
         else if (*str <= 0x20);
         else if (*str IS ',') break;
         else if (*str IS '}') break;
         else {
            log.warning("Invalid hexadecimal number '%s' at line %d", numbuf.c_str(), Line);
            return ERR::Syntax;
         }
         str++;
      }

      auto &number_tag = Tags.emplace_back(XTag(TagID++, Line));
      number_tag.Attribs.reserve(3);
      number_tag.Attribs.emplace_back("item", "");
      number_tag.Attribs.emplace_back("name", std::move(item_name));
      number_tag.Attribs.emplace_back("type", "number");

      auto &content_tag = number_tag.Children.emplace_back(XTag(TagID++, Line));
      content_tag.Attribs.emplace_back("", std::move(numbuf));
   }
   else if (((*str >= '0') and (*str <= '9')) or
            ((*str IS '-') and (str[1] >= '0') and (str[1] <= '9'))) {
      // Evaluates to: <item name="item_name" type="integer">number</item>

      for (i=0; (str[i] IS '-') or (str[i] IS '.') or ((str[i] >= '0') and (str[i] <= '9')); i++);
      std::string numbuf(str, i);
      str += i;

      // Skip whitespace and check that the number was valid.
      while (*str) {
         if (*str IS '\n') Line++;
         else if (*str <= 0x20);
         else if (*str IS ',') break;
         else if (*str IS '}') break;
         else {
            log.warning("Invalid number at line %d", Line);
            return ERR::Syntax;
         }
         str++;
      }

      auto &number_tag = Tags.emplace_back(XTag(TagID++, Line));
      number_tag.Attribs.reserve(3);
      number_tag.Attribs.emplace_back("item", "");
      number_tag.Attribs.emplace_back("name", std::move(item_name));
      number_tag.Attribs.emplace_back("type", "number");

      auto &content_tag = number_tag.Children.emplace_back(XTag(TagID++, Line));
      content_tag.Attribs.emplace_back("", std::move(numbuf));
   }
   else if (kt::startswith("null", str)) { // Evaluates to <item name="item_name" type="null"/>
      str += 4;

      auto &null_tag = Tags.emplace_back(XTag(TagID++, Line));
      null_tag.Attribs.reserve(3);
      null_tag.Attribs.emplace_back("item", "");
      null_tag.Attribs.emplace_back("name", std::move(item_name));
      null_tag.Attribs.emplace_back("type", "null");
   }
   else {
      log.warning("Invalid value character '%c' encountered for item '%s' at line %d.", *str, item_name.c_str(), Line);
      return ERR::Syntax;
   }

   *Input = str;
   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, nullptr, nullptr)
extern "C" struct ModHeader * register_json_module() { return &ModHeader; }
