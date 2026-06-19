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

// Options for XQuery evaluation flags.

enum class XEF : uint32_t {
   NIL = 0,
   LIMIT_SCOPE = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(XEF)

// XQuery class definition

#define VER_XQUERY (1.000000)

// XQuery methods

namespace xq {
struct Evaluate { objXML *XML; int Index; XEF Flags; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Search { objXML *XML; FUNCTION *Callback; int Index; XEF Flags; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RegisterFunction { std::string_view FunctionName; FUNCTION *Callback; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InspectFunctions { std::string_view Name; XIF ResultFlags; std::string *Result; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objXQuery : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::XQUERY;
   static constexpr CSTRING CLASS_NAME = "XQuery";

   using create = kt::Create<objXQuery>;

   std::string ErrorMsg;    // A readable description of the last parse or execution error.
   std::string Path;        // Base path for resolving relative references.
   std::string Statement;   // XQuery statements are specified here.
   int64_t MemoryUsage;     // The total amount of memory allocated by the last compilation or evaluation.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR evaluate(objXML * XML, int Index, XEF Flags) noexcept {
      struct xq::Evaluate args = { XML, Index, Flags };
      return Action(AC(-1), this, &args);
   }
   inline ERR search(objXML * XML, FUNCTION Callback, int Index, XEF Flags) noexcept {
      struct xq::Search args = { XML, &Callback, Index, Flags };
      return Action(AC(-2), this, &args);
   }
   inline ERR registerFunction(const std::string_view &FunctionName, FUNCTION Callback) noexcept {
      struct xq::RegisterFunction args = { FunctionName, &Callback };
      return Action(AC(-3), this, &args);
   }
   inline ERR inspectFunctions(const std::string_view &Name, XIF ResultFlags, std::string &Result) noexcept {
      struct xq::InspectFunctions args = { Name, ResultFlags, &Result };
      ERR error = Action(AC(-4), this, &args);
      return error;
   }

   // Customised field getting

   inline ERR getErrorMsg(std::string_view &Value) noexcept {
      Value = this->ErrorMsg;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getStatement(std::string_view &Value) noexcept {
      Value = this->Statement;
      return ERR::Okay;
   }

   inline ERR getMemoryUsage(int64_t &Value) noexcept {
      Value = this->MemoryUsage;
      return ERR::Okay;
   }

   inline ERR getResult(struct XPathValue * &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->GetValue(this, &Value);
   }

   inline ERR getResultString(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getFeatureFlags(int &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->GetValue(this, &Value);
   }

   inline ERR getResultType(int &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->GetValue(this, &Value);
   }

   inline ERR getResolveVariable(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getFunctions(std::span<std::string> &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<std::string> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getVariables(std::span<std::string> &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<std::string> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setStatement(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setResolveVariable(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

namespace xq {

using XQueryFunction = ERR (*)(objXQuery *Query, std::string_view FunctionName, const std::vector<XPathValue> &Input, XPathValue &Result, APTR Meta);
using XQueryResolveVariable = ERR (*)(objXQuery *Query, std::string_view Name, XPathValue *Result, APTR Meta);

} // namespace xq
