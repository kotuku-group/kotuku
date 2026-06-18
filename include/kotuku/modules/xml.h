#pragma once

// Name:      xml.h
// Copyright: Paul Manias © 2001-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_XML (1)

#ifdef __cplusplus
#include <functional>
#include <memory>
#include <sstream>
#include <type_traits>
#ifndef STRINGS_HPP
#include <kotuku/strings.hpp>
#endif

#endif

class objXML;

// For SetAttrib()

enum class XMS : int {
   NIL = 0,
   NEW = -1,
   UPDATE_ONLY = -2,
   UPDATE = -3,
   REMOVE = -4,
};

// Options for the Sort method.

enum class XSF : uint32_t {
   NIL = 0,
   DESC = 0x00000001,
   CHECK_SORT = 0x00000002,
};

DEFINE_ENUM_FLAG_OPERATORS(XSF)

// Standard flags for the XML class.

enum class XMF : uint32_t {
   NIL = 0,
   WELL_FORMED = 0x00000001,
   INCLUDE_COMMENTS = 0x00000002,
   STRIP_CONTENT = 0x00000004,
   READABLE = 0x00000008,
   INDENT = 0x00000008,
   LOCK_REMOVE = 0x00000010,
   STRIP_HEADERS = 0x00000020,
   NEW = 0x00000040,
   NO_ESCAPE = 0x00000080,
   INCLUDE_WHITESPACE = 0x00000100,
   PARSE_HTML = 0x00000200,
   STRIP_CDATA = 0x00000400,
   LOG_ALL = 0x00000800,
   PARSE_ENTITY = 0x00001000,
   OMIT_TAGS = 0x00002000,
   NAMESPACE_AWARE = 0x00004000,
   HAS_SCHEMA = 0x00008000,
   STANDALONE = 0x00010000,
   READ_ONLY = 0x00020000,
   INCLUDE_SIBLINGS = 0x80000000,
};

DEFINE_ENUM_FLAG_OPERATORS(XMF)

// Tag insertion options.

enum class XMI : int {
   NIL = 0,
   PREV = 0,
   PREVIOUS = 0,
   CHILD = 1,
   NEXT = 2,
   CHILD_END = 3,
   END = 4,
};

// Standard flags for XTag.

enum class XTF : uint32_t {
   NIL = 0,
   CDATA = 0x00000001,
   INSTRUCTION = 0x00000002,
   NOTATION = 0x00000004,
   COMMENT = 0x00000008,
};

DEFINE_ENUM_FLAG_OPERATORS(XTF)

// Type descriptors for XPathValue

enum class XPVT : int {
   NIL = 0,
   NodeSet = 0,
   Boolean = 1,
   Number = 2,
   String = 3,
   Date = 4,
   Time = 5,
   DateTime = 6,
   Map = 7,
   Array = 8,
};

typedef struct XMLAttrib {
   std::string Name;    // Name of the attribute
   std::string Value;   // Value of the attribute
   inline bool isContent() const { return Name.empty(); }
   inline bool isTag() const { return !Name.empty(); }
   XMLAttrib(std::string_view pName, std::string_view pValue = "") : Name(pName), Value(pValue) { };
   XMLAttrib() = default;
} XMLATTRIB;

typedef struct XTag {
   int      ID;                      // Globally unique ID assigned to the tag on creation.
   int      ParentID;                // UID of the parent tag
   int      LineNo;                  // Line number on which this tag was encountered
   XTF      Flags;                   // Optional flags
   uint32_t NamespaceID;             // Hash of namespace URI or 0 for no namespace
   int      Reserved;                // Private
   kt::vector<XMLAttrib> Attribs;    // Array of attributes for this tag
   kt::vector<XTag> Children;        // Array of child tags
   XTag(int pID, int pLine = 0) :
      ID(pID), ParentID(0), LineNo(pLine), Flags(XTF::NIL), NamespaceID(0), Reserved(0)
      { }

   XTag(int pID, int pLine, kt::vector<XMLAttrib> pAttribs) :
      ID(pID), ParentID(0), LineNo(pLine), Flags(XTF::NIL), NamespaceID(0), Reserved(0), Attribs(pAttribs)
      { }

   XTag() { XTag(0); }

   inline CSTRING name() const { return Attribs[0].Name.c_str(); }
   inline bool hasContent() const { return (!Children.empty()) and (Children[0].Attribs[0].Name.empty()); }
   inline bool isContent() const { return Attribs[0].Name.empty(); }
   inline bool isTag() const { return !Attribs[0].Name.empty(); }

   inline bool hasChildTags() const {
      for (auto &scan : Children) {
         if (!scan.Attribs[0].Name.empty()) return true;
      }
      return false;
   }

   inline const std::string * attrib(std::string_view Name) const {
      for (unsigned a=1; a < Attribs.size(); a++) {
         if (kt::iequals(Attribs[a].Name, Name)) return &Attribs[a].Value;
      }
      return nullptr;
   }

   inline std::string getContent() const {
      if (Children.empty()) return std::string();

      std::string result;
      // Pre-calculate total size to avoid reallocations
      size_t total_size = 0;
      for (auto &scan : Children) {
         if (not scan.Attribs.empty() and scan.Attribs[0].isContent()) {
            total_size += scan.Attribs[0].Value.size();
         }
      }
      result.reserve(total_size);

      for (auto &scan : Children) {
         if (not scan.Attribs.empty() and scan.Attribs[0].isContent()) {
            result += scan.Attribs[0].Value;
         }
      }
      return result;
   }
} XTAG;

// XML class definition

#define VER_XML (1.000000)

// XML methods

namespace xml {
struct SetAttrib { int Index; XMS Attrib; std::string_view Name; std::string_view Value; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Serialise { int Index; XMF Flags; std::string *Result; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertXML { int Index; XMI Where; std::string_view XML; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetContent { int Index; std::string *Buffer; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Sort { std::string_view XPath; std::string_view Sort; XSF Flags; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveTag { int Index; int Total; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct MoveTags { int Index; int Total; int DestIndex; XMI Where; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetAttrib { int Index; std::string_view Attrib; std::string *Value; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertXPath { std::string_view XPath; XMI Where; std::string_view XML; int Result; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Search { std::string_view Expression; FUNCTION *Callback; int Result; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Filter { std::string_view XPath; static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Evaluate { std::string_view Statement; std::string *Result; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ValidateDocument { static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertContent { int Index; XMI Where; std::string_view Content; int Result; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveXPath { std::string_view XPath; int Limit; static const AC id = AC(-15); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetTag { int Index; struct XTag *Result; static const AC id = AC(-16); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RegisterNamespace { std::string_view URI; uint32_t Result; static const AC id = AC(-17); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetNamespaceURI { uint32_t NamespaceID; std::string *Result; static const AC id = AC(-18); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetTagNamespace { int TagID; int NamespaceID; static const AC id = AC(-19); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ResolvePrefix { std::string_view Prefix; int TagID; uint32_t Result; static const AC id = AC(-20); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetEntity { std::string_view Name; std::string *Value; static const AC id = AC(-21); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetNotation { std::string_view Name; std::string *Value; static const AC id = AC(-22); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct LoadSchema { std::string_view Path; static const AC id = AC(-23); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objXML : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::XML;
   static constexpr CSTRING CLASS_NAME = "XML";

   using create = kt::Create<objXML>;

   std::string Path;       // Set this field if the XML document originates from a file source.
   std::string DocType;    // Root element name from a parsed DOCTYPE declaration.
   std::string PublicID;   // Public identifier from a parsed DOCTYPE declaration.
   std::string SystemID;   // System identifier from a parsed DOCTYPE declaration.
   std::string ErrorMsg;   // A textual description of the last parse error.
   OBJECTPTR Source;       // Set this field if the XML data is to be sourced from another object.
   XMF       Flags;        // Controls XML parsing behaviour and processing options.
   int       Modified;     // A timestamp of when the XML data was last modified.
   ERR       ParseError;   // Private
   int       LineNo;       // Private
   public:
   typedef kt::vector<XTag> TAGS;
   TAGS Tags;

   template <class T> inline ERR insertStatement(int Index, XMI Where, T Statement, XTag **Result) {
      int index_result;
      XTag *tag_result;
      if (auto error = insertXML(Index, Where, std::string_view(Statement), &index_result); !error) {
         error = getTag(index_result, &tag_result);
         *Result = tag_result;
         return error;
      }
      else return error;
   }

   template <class T> inline ERR setAttribValue(int Tag, XMS AttribID, T &&Attrib, int Value) {
      auto buffer = std::to_string(Value);
      return setAttrib(Tag, AttribID, std::string_view(Attrib), buffer);
   }

   template <class T> inline ERR setAttribValue(int Tag, XMS AttribID, T &&Attrib, double Value) {
      auto buffer = std::to_string(Value);
      return setAttrib(Tag, AttribID, std::string_view(Attrib), buffer);
   }

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR setAttrib(int Index, XMS Attrib, const std::string_view &Name, const std::string_view &Value) noexcept {
      struct xml::SetAttrib args = { Index, Attrib, Name, Value };
      return Action(AC(-1), this, &args);
   }
   inline ERR serialise(int Index, XMF Flags, std::string &Result) noexcept {
      struct xml::Serialise args = { Index, Flags, &Result };
      ERR error = Action(AC(-2), this, &args);
      return error;
   }
   inline ERR insertXML(int Index, XMI Where, const std::string_view &XML, int * Result) noexcept {
      struct xml::InsertXML args = { Index, Where, XML, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR getContent(int Index, std::string &Buffer) noexcept {
      struct xml::GetContent args = { Index, &Buffer };
      ERR error = Action(AC(-4), this, &args);
      return error;
   }
   inline ERR sort(const std::string_view &XPath, const std::string_view &Sort, XSF Flags) noexcept {
      struct xml::Sort args = { XPath, Sort, Flags };
      return Action(AC(-5), this, &args);
   }
   inline ERR removeTag(int Index, int Total) noexcept {
      struct xml::RemoveTag args = { Index, Total };
      return Action(AC(-6), this, &args);
   }
   inline ERR moveTags(int Index, int Total, int DestIndex, XMI Where) noexcept {
      struct xml::MoveTags args = { Index, Total, DestIndex, Where };
      return Action(AC(-7), this, &args);
   }
   inline ERR getAttrib(int Index, const std::string_view &Attrib, std::string &Value) noexcept {
      struct xml::GetAttrib args = { Index, Attrib, &Value };
      ERR error = Action(AC(-8), this, &args);
      return error;
   }
   inline ERR insertXPath(const std::string_view &XPath, XMI Where, const std::string_view &XML, int * Result) noexcept {
      struct xml::InsertXPath args = { XPath, Where, XML, (int)0 };
      ERR error = Action(AC(-9), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR search(const std::string_view &Expression, FUNCTION Callback, int * Result) noexcept {
      struct xml::Search args = { Expression, &Callback, (int)0 };
      ERR error = Action(AC(-10), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR filter(const std::string_view &XPath) noexcept {
      struct xml::Filter args = { XPath };
      return Action(AC(-11), this, &args);
   }
   inline ERR evaluate(const std::string_view &Statement, std::string &Result) noexcept {
      struct xml::Evaluate args = { Statement, &Result };
      ERR error = Action(AC(-12), this, &args);
      return error;
   }
   inline ERR validateDocument() noexcept {
      return Action(AC(-13), this, nullptr);
   }
   inline ERR insertContent(int Index, XMI Where, const std::string_view &Content, int * Result) noexcept {
      struct xml::InsertContent args = { Index, Where, Content, (int)0 };
      ERR error = Action(AC(-14), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR removeXPath(const std::string_view &XPath, int Limit) noexcept {
      struct xml::RemoveXPath args = { XPath, Limit };
      return Action(AC(-15), this, &args);
   }
   inline ERR getTag(int Index, struct XTag ** Result) noexcept {
      struct xml::GetTag args = { Index, (struct XTag *)0 };
      ERR error = Action(AC(-16), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR registerNamespace(const std::string_view &URI, uint32_t * Result) noexcept {
      struct xml::RegisterNamespace args = { URI, (uint32_t)0 };
      ERR error = Action(AC(-17), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR getNamespaceURI(uint32_t NamespaceID, std::string &Result) noexcept {
      struct xml::GetNamespaceURI args = { NamespaceID, &Result };
      ERR error = Action(AC(-18), this, &args);
      return error;
   }
   inline ERR setTagNamespace(int TagID, int NamespaceID) noexcept {
      struct xml::SetTagNamespace args = { TagID, NamespaceID };
      return Action(AC(-19), this, &args);
   }
   inline ERR resolvePrefix(const std::string_view &Prefix, int TagID, uint32_t * Result) noexcept {
      struct xml::ResolvePrefix args = { Prefix, TagID, (uint32_t)0 };
      ERR error = Action(AC(-20), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR getEntity(const std::string_view &Name, std::string &Value) noexcept {
      struct xml::GetEntity args = { Name, &Value };
      ERR error = Action(AC(-21), this, &args);
      return error;
   }
   inline ERR getNotation(const std::string_view &Name, std::string &Value) noexcept {
      struct xml::GetNotation args = { Name, &Value };
      ERR error = Action(AC(-22), this, &args);
      return error;
   }
   inline ERR loadSchema(const std::string_view &Path) noexcept {
      struct xml::LoadSchema args = { Path };
      return Action(AC(-23), this, &args);
   }

   // Customised field getting

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getDocType(std::string_view &Value) noexcept {
      Value = this->DocType;
      return ERR::Okay;
   }

   inline ERR getPublic(std::string_view &Value) noexcept {
      Value = this->PublicID;
      return ERR::Okay;
   }

   inline ERR getSystem(std::string_view &Value) noexcept {
      Value = this->SystemID;
      return ERR::Okay;
   }

   inline ERR getErrorMsg(std::string_view &Value) noexcept {
      Value = this->ErrorMsg;
      return ERR::Okay;
   }

   inline ERR getSource(OBJECTPTR &Value) noexcept {
      Value = this->Source;
      return ERR::Okay;
   }

   inline ERR getFlags(XMF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getModified(int &Value) noexcept {
      Value = this->Modified;
      return ERR::Okay;
   }

   inline ERR getTags(std::span<struct XTag> &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      auto get_field = (ERR (*)(APTR, std::span<struct XTag> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getStatement(std::string &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      std::string_view view;
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, view);
      if (error IS ERR::Okay) {
         Value.assign(view);
         if (view.data()) FreeResource(GetMemoryID(view.data()));
      }
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setDocType(const std::string_view &Value) noexcept {
      this->DocType = Value;
      return ERR::Okay;
   }

   inline ERR setPublic(const std::string_view &Value) noexcept {
      this->PublicID = Value;
      return ERR::Okay;
   }

   inline ERR setSystem(const std::string_view &Value) noexcept {
      this->SystemID = Value;
      return ERR::Okay;
   }

   inline ERR setSource(OBJECTPTR Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Source = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const XMF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setStatement(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, 0x00804328, &Value);
   }

};

struct XPathValueSequence;
struct XPathMapEntry;
struct XPathMapStorage;
struct XPathArrayStorage;
typedef struct XPathValue {
   XPVT   Type;                // Identifies the type of value stored
   double NumberValue;         // Defined if the type is Number or Boolean
   std::string StringValue;    // Defined if the type is String
   kt::vector<XTag *> node_set; // Defined if the type is NodeSet
   std::optional<std::string> node_set_string_override; // If set, this string is returned for all nodes in the node set
   std::vector<std::string> node_set_string_values; // If set, these strings are returned for all nodes in the node set
   std::vector<const XMLAttrib *> node_set_attributes; // If set, these attributes are returned for all nodes in the node set
   std::vector<std::shared_ptr<XPathValue>> node_set_composite_values; // Stores composite items for general sequences
   bool preserve_node_order = false; // When true, node_set is already in the desired evaluation order
   std::shared_ptr<XPathMapStorage> map_storage; // Defined if the type is Map
   std::shared_ptr<XPathArrayStorage> array_storage; // Defined if the type is Array

   XPathValue(XPVT pType) : Type(pType), NumberValue(0) { }

   explicit XPathValue(const kt::vector<XTag *> &Nodes,
      std::optional<std::string> NodeSetString = std::nullopt,
      std::vector<std::string> NodeSetStrings = {},
      std::vector<const XMLAttrib *> NodeSetAttributes = {})
      : Type(XPVT::NodeSet),
        node_set(Nodes),
        node_set_string_override(std::move(NodeSetString)),
        node_set_string_values(std::move(NodeSetStrings)),
        node_set_attributes(std::move(NodeSetAttributes)),
        preserve_node_order(false) {}

   void reset();
} XPATHVALUE;

struct XPathValueSequence
{
   std::vector<XPathValue> items;

   inline void reset() { items.clear(); }
   inline bool empty() const noexcept { return items.empty(); }
   inline size_t size() const noexcept { return items.size(); }
};

struct XPathMapEntry
{
   std::string key;
   XPathValueSequence value;

   inline void reset() { value.reset(); }
};

struct XPathMapStorage
{
   std::vector<XPathMapEntry> entries;

   inline void reset() {
      for (auto &entry : entries) entry.reset();
      entries.clear();
   }

   inline bool empty() const noexcept { return entries.empty(); }
   inline size_t size() const noexcept { return entries.size(); }
};

struct XPathArrayStorage
{
   std::vector<XPathValueSequence> members;

   void reset() {
      for (auto &member : members) member.reset();
      members.clear();
   }

   inline bool empty() const noexcept { return members.empty(); }
   inline size_t size() const noexcept { return members.size(); }
};

inline void XPathValue::reset() {
   Type = XPVT::Boolean;
   NumberValue = 0.0;
   StringValue.clear();
   node_set.clear();
   node_set_string_override.reset();
   node_set_string_values.clear();
   node_set_attributes.clear();
   node_set_composite_values.clear();
   preserve_node_order = false;
   if (map_storage) map_storage->reset();
   map_storage.reset();
   if (array_storage) array_storage->reset();
   array_storage.reset();
}

//********************************************************************************************************************

namespace xml {

inline void UpdateAttrib(XTag &Tag, const std::string Name, const std::string Value, bool CanCreate = false)
{
   for (auto a = Tag.Attribs.begin(); a != Tag.Attribs.end(); a++) {
      if (kt::iequals(Name, a->Name)) {
         a->Name  = Name;
         a->Value = Value;
         return;
      }
   }

   if (CanCreate) Tag.Attribs.emplace_back(Name, Value);
}

inline void NewAttrib(XTag &Tag, const std::string_view Name, const std::string_view Value) {
   Tag.Attribs.emplace_back(Name, Value);
}

inline void NewAttrib(XTag *Tag, const std::string_view Name, const std::string_view Value) {
   Tag->Attribs.emplace_back(Name, Value);
}

// Convert an arithmetic value to its attribute string representation.  Floating point values are formatted via a
// stream so that insignificant trailing zeros are dropped (e.g. "10" rather than "10.000000") while retaining
// sufficient precision; integral values use the more direct std::to_string().

template <class T> requires std::is_arithmetic_v<T>
inline std::string attrib_to_string(const T Value) {
   if constexpr (std::is_floating_point_v<T>) {
      std::ostringstream buffer;
      buffer << Value;
      return buffer.str();
   }
   else return std::to_string(Value);
}

template <class T> requires std::is_arithmetic_v<T>
inline void NewAttrib(XTag &Tag, const std::string_view Name, const T Value) {
   Tag.Attribs.emplace_back(Name, attrib_to_string(Value));
}

template <class T> requires std::is_arithmetic_v<T>
inline void NewAttrib(XTag *Tag, const std::string_view Name, const T Value) {
   Tag->Attribs.emplace_back(Name, attrib_to_string(Value));
}

inline std::string GetContent(const XTag &Tag) {
   std::string value;
   for (auto &scan : Tag.Children) {
      if (scan.Attribs.empty()) continue;
      if (scan.Attribs[0].isContent()) value.append(scan.Attribs[0].Value);
   }
   return value;
}

//********************************************************************************************************************
// Call a Function for every attribute in the XML tree.  Allows you to modify attributes quite easily, e.g. to convert
// all attribute names to uppercase:
//
// std::transform(attrib.Name.begin(), attrib.Name.end(), attrib.Name.begin(),
//   [](UBYTE c){ return std::toupper(c); });

inline void ForEachAttrib(objXML::TAGS &Tags, std::function<void(XMLAttrib &)> &Function)
{
   for (auto &tag : Tags) {
      for (auto &attrib : tag.Attribs) {
         Function(attrib);
      }
      if (!tag.Children.empty()) ForEachAttrib(tag.Children, Function);
   }
}

} // namespace
#ifdef KOTUKU_STATIC
#define JUMPTABLE_XML [[maybe_unused]] static struct XMLBase *XMLBase = nullptr;
#else
#define JUMPTABLE_XML struct XMLBase *XMLBase = nullptr;
#endif

struct XMLBase {
#ifndef KOTUKU_STATIC
   ERR (*_XValueToNumber)(struct XPathValue *Value, double *Result);
   ERR (*_XValueToString)(const struct XPathValue *Value, std::string *Result);
   ERR (*_XValueNodes)(struct XPathValue *Value, kt::vector<XTag *> *Result);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_XML_MODULE)
extern struct XMLBase *XMLBase;
namespace xml {
inline ERR XValueToNumber(struct XPathValue *Value, double *Result) { return XMLBase->_XValueToNumber(Value,Result); }
inline ERR XValueToString(const struct XPathValue *Value, std::string *Result) { return XMLBase->_XValueToString(Value,Result); }
inline ERR XValueNodes(struct XPathValue *Value, kt::vector<XTag *> *Result) { return XMLBase->_XValueNodes(Value,Result); }
} // namespace
#else
namespace xml {
extern ERR XValueToNumber(struct XPathValue *Value, double *Result);
extern ERR XValueToString(const struct XPathValue *Value, std::string *Result);
extern ERR XValueNodes(struct XPathValue *Value, kt::vector<XTag *> *Result);
} // namespace
#endif // KOTUKU_STATIC

