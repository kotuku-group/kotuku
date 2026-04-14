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
   XMLAttrib(std::string pName, std::string pValue = "") : Name(pName), Value(pValue) { };
   XMLAttrib() = default;
} XMLATTRIB;

typedef struct XTag {
   int      ID;                      // Globally unique ID assigned to the tag on creation.
   int      ParentID;                // UID of the parent tag
   int      LineNo;                  // Line number on which this tag was encountered
   XTF      Flags;                   // Optional flags
   uint32_t NamespaceID;             // Hash of namespace URI or 0 for no namespace
   int      Reserved;                // Private
   pf::vector<XMLAttrib> Attribs;    // Array of attributes for this tag
   pf::vector<XTag> Children;        // Array of child tags
   XTag(int pID, int pLine = 0) :
      ID(pID), ParentID(0), LineNo(pLine), Flags(XTF::NIL), NamespaceID(0), Reserved(0)
      { }

   XTag(int pID, int pLine, pf::vector<XMLAttrib> pAttribs) :
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

   inline const std::string * attrib(const std::string &Name) const {
      for (unsigned a=1; a < Attribs.size(); a++) {
         if (pf::iequals(Attribs[a].Name, Name)) return &Attribs[a].Value;
      }
      return NULL;
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
struct SetAttrib { int Index; XMS Attrib; CSTRING Name; CSTRING Value; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Serialise { int Index; XMF Flags; STRING Result; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertXML { int Index; XMI Where; CSTRING XML; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetContent { int Index; STRING Buffer; int Length; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Sort { CSTRING XPath; CSTRING Sort; XSF Flags; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveTag { int Index; int Total; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct MoveTags { int Index; int Total; int DestIndex; XMI Where; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetAttrib { int Index; CSTRING Attrib; CSTRING Value; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertXPath { CSTRING XPath; XMI Where; CSTRING XML; int Result; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Search { CSTRING Expression; FUNCTION * Callback; int Result; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Filter { CSTRING XPath; static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Evaluate { CSTRING Statement; CSTRING Result; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ValidateDocument { static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertContent { int Index; XMI Where; CSTRING Content; int Result; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveXPath { CSTRING XPath; int Limit; static const AC id = AC(-15); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetTag { int Index; struct XTag * Result; static const AC id = AC(-16); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RegisterNamespace { CSTRING URI; uint32_t Result; static const AC id = AC(-17); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetNamespaceURI { uint32_t NamespaceID; CSTRING Result; static const AC id = AC(-18); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetTagNamespace { int TagID; int NamespaceID; static const AC id = AC(-19); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ResolvePrefix { CSTRING Prefix; int TagID; uint32_t Result; static const AC id = AC(-20); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetEntity { CSTRING Name; CSTRING Value; static const AC id = AC(-21); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetNotation { CSTRING Name; CSTRING Value; static const AC id = AC(-22); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct LoadSchema { CSTRING Path; static const AC id = AC(-23); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objXML : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::XML;
   static constexpr CSTRING CLASS_NAME = "XML";

   using create = pf::Create<objXML>;

   STRING    Path;    // Set this field if the XML document originates from a file source.
   STRING    DocType; // Root element name from DOCTYPE declaration
   STRING    PublicID; // Public identifier for external DTD
   STRING    SystemID; // System identifier for external DTD
   OBJECTPTR Source;  // Set this field if the XML data is to be sourced from another object.
   XMF       Flags;   // Controls XML parsing behaviour and processing options.
   int       Modified; // A timestamp of when the XML data was last modified.
   ERR       ParseError; // Private
   int       LineNo;  // Private
   public:
   typedef pf::vector<XTag> TAGS;
   TAGS Tags;

   template <class T> inline ERR insertStatement(int Index, XMI Where, T Statement, XTag **Result) {
      int index_result;
      XTag *tag_result;
      if (auto error = insertXML(Index, Where, to_cstring(Statement), &index_result); error IS ERR::Okay) {
         error = getTag(index_result, &tag_result);
         *Result = tag_result;
         return error;
      }
      else return error;
   }

   template <class T> inline ERR setAttribValue(int Tag, int Flags, T &&Attrib, int Value) {
      auto attrib = to_cstring(Attrib);
      auto buffer = std::to_string(Value);
      return setAttrib(Tag, Flags, attrib, buffer.c_str());
   }

   template <class T> inline ERR setAttribValue(int Tag, int Flags, T &&Attrib, double Value) {
      auto attrib = to_cstring(Attrib);
      auto buffer = std::to_string(Value);
      return setAttrib(Tag, Flags, attrib, buffer.c_str());
   }

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR setAttrib(int Index, XMS Attrib, CSTRING Name, CSTRING Value) noexcept {
      struct xml::SetAttrib args = { Index, Attrib, Name, Value };
      return(Action(AC(-1), this, &args));
   }
   inline ERR serialise(int Index, XMF Flags, STRING * Result) noexcept {
      struct xml::Serialise args = { Index, Flags, (STRING)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR insertXML(int Index, XMI Where, CSTRING XML, int * Result) noexcept {
      struct xml::InsertXML args = { Index, Where, XML, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR getContent(int Index, STRING Buffer, int Length) noexcept {
      struct xml::GetContent args = { Index, Buffer, Length };
      return(Action(AC(-4), this, &args));
   }
   inline ERR sort(CSTRING XPath, CSTRING Sort, XSF Flags) noexcept {
      struct xml::Sort args = { XPath, Sort, Flags };
      return(Action(AC(-5), this, &args));
   }
   inline ERR removeTag(int Index, int Total) noexcept {
      struct xml::RemoveTag args = { Index, Total };
      return(Action(AC(-6), this, &args));
   }
   inline ERR moveTags(int Index, int Total, int DestIndex, XMI Where) noexcept {
      struct xml::MoveTags args = { Index, Total, DestIndex, Where };
      return(Action(AC(-7), this, &args));
   }
   inline ERR getAttrib(int Index, CSTRING Attrib, CSTRING * Value) noexcept {
      struct xml::GetAttrib args = { Index, Attrib, (CSTRING)0 };
      ERR error = Action(AC(-8), this, &args);
      if (Value) *Value = args.Value;
      return(error);
   }
   inline ERR insertXPath(CSTRING XPath, XMI Where, CSTRING XML, int * Result) noexcept {
      struct xml::InsertXPath args = { XPath, Where, XML, (int)0 };
      ERR error = Action(AC(-9), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR search(CSTRING Expression, FUNCTION Callback, int * Result) noexcept {
      struct xml::Search args = { Expression, &Callback, (int)0 };
      ERR error = Action(AC(-10), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR filter(CSTRING XPath) noexcept {
      struct xml::Filter args = { XPath };
      return(Action(AC(-11), this, &args));
   }
   inline ERR evaluate(CSTRING Statement, CSTRING * Result) noexcept {
      struct xml::Evaluate args = { Statement, (CSTRING)0 };
      ERR error = Action(AC(-12), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR validateDocument() noexcept {
      return(Action(AC(-13), this, nullptr));
   }
   inline ERR insertContent(int Index, XMI Where, CSTRING Content, int * Result) noexcept {
      struct xml::InsertContent args = { Index, Where, Content, (int)0 };
      ERR error = Action(AC(-14), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR removeXPath(CSTRING XPath, int Limit) noexcept {
      struct xml::RemoveXPath args = { XPath, Limit };
      return(Action(AC(-15), this, &args));
   }
   inline ERR getTag(int Index, struct XTag ** Result) noexcept {
      struct xml::GetTag args = { Index, (struct XTag *)0 };
      ERR error = Action(AC(-16), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR registerNamespace(CSTRING URI, uint32_t * Result) noexcept {
      struct xml::RegisterNamespace args = { URI, (uint32_t)0 };
      ERR error = Action(AC(-17), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR getNamespaceURI(uint32_t NamespaceID, CSTRING * Result) noexcept {
      struct xml::GetNamespaceURI args = { NamespaceID, (CSTRING)0 };
      ERR error = Action(AC(-18), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR setTagNamespace(int TagID, int NamespaceID) noexcept {
      struct xml::SetTagNamespace args = { TagID, NamespaceID };
      return(Action(AC(-19), this, &args));
   }
   inline ERR resolvePrefix(CSTRING Prefix, int TagID, uint32_t * Result) noexcept {
      struct xml::ResolvePrefix args = { Prefix, TagID, (uint32_t)0 };
      ERR error = Action(AC(-20), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }
   inline ERR getEntity(CSTRING Name, CSTRING * Value) noexcept {
      struct xml::GetEntity args = { Name, (CSTRING)0 };
      ERR error = Action(AC(-21), this, &args);
      if (Value) *Value = args.Value;
      return(error);
   }
   inline ERR getNotation(CSTRING Name, CSTRING * Value) noexcept {
      struct xml::GetNotation args = { Name, (CSTRING)0 };
      ERR error = Action(AC(-22), this, &args);
      if (Value) *Value = args.Value;
      return(error);
   }
   inline ERR loadSchema(CSTRING Path) noexcept {
      struct xml::LoadSchema args = { Path };
      return(Action(AC(-23), this, &args));
   }

   // Customised field setting

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setDocType(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setPublic(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setSystem(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setSource(OBJECTPTR Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Source = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const XMF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setReadOnly(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setStatement(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, 0x08800320, to_cstring(Value), 1);
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
   pf::vector<XTag *> node_set; // Defined if the type is NodeSet
   std::optional<std::string> node_set_string_override; // If set, this string is returned for all nodes in the node set
   std::vector<std::string> node_set_string_values; // If set, these strings are returned for all nodes in the node set
   std::vector<const XMLAttrib *> node_set_attributes; // If set, these attributes are returned for all nodes in the node set
   std::vector<std::shared_ptr<XPathValue>> node_set_composite_values; // Stores composite items for general sequences
   bool preserve_node_order = false; // When true, node_set is already in the desired evaluation order
   std::shared_ptr<XPathMapStorage> map_storage; // Defined if the type is Map
   std::shared_ptr<XPathArrayStorage> array_storage; // Defined if the type is Array

   XPathValue(XPVT pType) : Type(pType), NumberValue(0) { }

   explicit XPathValue(const pf::vector<XTag *> &Nodes,
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
      if (pf::iequals(Name, a->Name)) {
         a->Name  = Name;
         a->Value = Value;
         return;
      }
   }

   if (CanCreate) Tag.Attribs.emplace_back(Name, Value);
}

inline void NewAttrib(XTag &Tag, const std::string Name, const std::string Value) {
   Tag.Attribs.emplace_back(Name, Value);
}

inline void NewAttrib(XTag *Tag, const std::string Name, const std::string Value) {
   Tag->Attribs.emplace_back(Name, Value);
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
   ERR (*_XValueNodes)(struct XPathValue *Value, pf::vector<struct XTag *> *Result);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_XML_MODULE)
extern struct XMLBase *XMLBase;
namespace xml {
inline ERR XValueToNumber(struct XPathValue *Value, double *Result) { return XMLBase->_XValueToNumber(Value,Result); }
inline ERR XValueToString(const struct XPathValue *Value, std::string *Result) { return XMLBase->_XValueToString(Value,Result); }
inline ERR XValueNodes(struct XPathValue *Value, pf::vector<struct XTag *> *Result) { return XMLBase->_XValueNodes(Value,Result); }
} // namespace
#else
namespace xml {
extern ERR XValueToNumber(struct XPathValue *Value, double *Result);
extern ERR XValueToString(const struct XPathValue *Value, std::string *Result);
extern ERR XValueNodes(struct XPathValue *Value, pf::vector<struct XTag *> *Result);
} // namespace
#endif // KOTUKU_STATIC

