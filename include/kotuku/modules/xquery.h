#pragma once

// Name:      xquery.h
// Copyright: Paul Manias © 2025-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_XQUERY (1)

#include <kotuku/modules/xml.h>

#ifdef __cplusplus
#include <functional>
#include <optional>
#include <sstream>
#ifndef STRINGS_HPP
#include <kotuku/strings.hpp>
#endif

#include <string_view>
#include <vector>
#endif

class objXQuery;

enum class XQueryNodeType : int {
   NIL = 0,
   LOCATION_PATH = 0,
   STEP = 1,
   NODE_TEST = 2,
   PREDICATE = 3,
   ROOT = 4,
   EXPRESSION = 5,
   FILTER = 6,
   BINARY_OP = 7,
   UNARY_OP = 8,
   CAST_EXPRESSION = 9,
   CONDITIONAL = 10,
   FOR_EXPRESSION = 11,
   FOR_BINDING = 12,
   LET_EXPRESSION = 13,
   LET_BINDING = 14,
   FLWOR_EXPRESSION = 15,
   WHERE_CLAUSE = 16,
   GROUP_CLAUSE = 17,
   GROUP_KEY = 18,
   ORDER_CLAUSE = 19,
   ORDER_SPEC = 20,
   COUNT_CLAUSE = 21,
   QUANTIFIED_EXPRESSION = 22,
   QUANTIFIED_BINDING = 23,
   FUNCTION_CALL = 24,
   LITERAL = 25,
   VARIABLE_REFERENCE = 26,
   NAME_TEST = 27,
   NODE_TYPE_TEST = 28,
   PROCESSING_INSTRUCTION_TEST = 29,
   WILDCARD = 30,
   AXIS_SPECIFIER = 31,
   UNION = 32,
   NUMBER = 33,
   STRING = 34,
   PATH = 35,
   DIRECT_ELEMENT_CONSTRUCTOR = 36,
   DIRECT_ATTRIBUTE_CONSTRUCTOR = 37,
   DIRECT_TEXT_CONSTRUCTOR = 38,
   COMPUTED_ELEMENT_CONSTRUCTOR = 39,
   COMPUTED_ATTRIBUTE_CONSTRUCTOR = 40,
   TEXT_CONSTRUCTOR = 41,
   COMMENT_CONSTRUCTOR = 42,
   PI_CONSTRUCTOR = 43,
   DOCUMENT_CONSTRUCTOR = 44,
   MAP_CONSTRUCTOR = 45,
   ARRAY_CONSTRUCTOR = 46,
   LOOKUP_EXPRESSION = 47,
   CONSTRUCTOR_CONTENT = 48,
   ATTRIBUTE_VALUE_TEMPLATE = 49,
   EMPTY_SEQUENCE = 50,
   INSTANCE_OF_EXPRESSION = 51,
   TREAT_AS_EXPRESSION = 52,
   CASTABLE_EXPRESSION = 53,
   TYPESWITCH_EXPRESSION = 54,
   TYPESWITCH_CASE = 55,
   TYPESWITCH_DEFAULT_CASE = 56,
};

// Flags indicating the features of a compiled XQuery expression.

enum class XQF : uint32_t {
   NIL = 0,
   XPATH = 0x00000001,
   HAS_PROLOG = 0x00000002,
   LIBRARY_MODULE = 0x00000004,
   MODULE_IMPORTS = 0x00000008,
   DEFAULT_FUNCTION_NS = 0x00000010,
   DEFAULT_ELEMENT_NS = 0x00000020,
   BASE_URI_DECLARED = 0x00000040,
   DEFAULT_COLLATION_DECLARED = 0x00000080,
   BOUNDARY_PRESERVE = 0x00000100,
   CONSTRUCTION_PRESERVE = 0x00000200,
   ORDERING_UNORDERED = 0x00000400,
   HAS_WILDCARD_TESTS = 0x00000800,
};

DEFINE_ENUM_FLAG_OPERATORS(XQF)

// Result flags for InspectFunctions().

enum class XIF : uint32_t {
   NIL = 0,
   AST = 0x00000001,
   NAME = 0x00000002,
   PARAMETERS = 0x00000004,
   RETURN_TYPE = 0x00000008,
   USER_DEFINED = 0x00000010,
   SIGNATURE = 0x00000020,
   ALL = 0x0000003f,
};

DEFINE_ENUM_FLAG_OPERATORS(XIF)

// XQuery class definition

#define VER_XQUERY (1.000000)

// XQuery methods

namespace xq {
struct Evaluate { objXML * XML; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Search { objXML * XML; FUNCTION * Callback; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RegisterFunction { CSTRING FunctionName; FUNCTION * Callback; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InspectFunctions { CSTRING Name; XIF ResultFlags; CSTRING Result; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objXQuery : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::XQUERY;
   static constexpr CSTRING CLASS_NAME = "XQuery";

   using create = pf::Create<objXQuery>;

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR evaluate(objXML * XML) noexcept {
      struct xq::Evaluate args = { XML };
      return(Action(AC(-1), this, &args));
   }
   inline ERR search(objXML * XML, FUNCTION Callback) noexcept {
      struct xq::Search args = { XML, &Callback };
      return(Action(AC(-2), this, &args));
   }
   inline ERR registerFunction(CSTRING FunctionName, FUNCTION Callback) noexcept {
      struct xq::RegisterFunction args = { FunctionName, &Callback };
      return(Action(AC(-3), this, &args));
   }
   inline ERR inspectFunctions(CSTRING Name, XIF ResultFlags, CSTRING * Result) noexcept {
      struct xq::InspectFunctions args = { Name, ResultFlags, (CSTRING)0 };
      ERR error = Action(AC(-4), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }

   // Customised field setting

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setStatement(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

};

namespace xq {

using XQueryFunction = ERR (*)(objXQuery *Query, std::string_view FunctionName, const std::vector<XPathValue> &Input, XPathValue &Result, APTR Meta);

} // namespace xq
