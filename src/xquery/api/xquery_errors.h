#pragma once

//********************************************************************************************************************
// W3C XQuery and XPath Error Codes for Module Loading
//
// This file defines error code constants for XQuery module-related static and dynamic errors
// as specified by the W3C XQuery specification.
//
// Reference: https://www.w3.org/TR/xquery/#id-errors
//            https://www.w3.org/2005/xqt-errors/
//********************************************************************************************************************

#include <string_view>

namespace xquery {
namespace errors {

//********************************************************************************************************************
// XQuery Static Errors (XQST) - detected during parsing/compilation
//********************************************************************************************************************

// XQST0047: It is a static error if multiple module imports in the same Prolog specify the same target namespace.
constexpr std::string_view XQST0047 = "XQST0047";

// XQST0048: It is a static error if a function or variable declared in a library module is not in the
// target namespace of the library module.
constexpr std::string_view XQST0048 = "XQST0048";

// XQST0059: It is a static error if an implementation is unable to process a schema or module import
// by finding a schema or module with the specified target namespace.
constexpr std::string_view XQST0059 = "XQST0059";

//********************************************************************************************************************
// XQuery Dynamic Errors (XQDY) - detected during evaluation
//********************************************************************************************************************

// XQDY0054: It is a dynamic error if a cycle is encountered in the definition of a module's dynamic
// context components, for example because of a cycle in variable declarations.
constexpr std::string_view XQDY0054 = "XQDY0054";

// FOAY0001: Array index out of bounds.
constexpr std::string_view FOAY0001 = "FOAY0001";

// FOJS0004: JSON: not schema-aware. Reused for JSON-related map/array diagnostics.
constexpr std::string_view FOJS0004 = "FOJS0004";

//********************************************************************************************************************
// Error Message Formatters
//********************************************************************************************************************

// Formats a complete error message with error code prefix
inline std::string format_error(std::string_view error_code, std::string_view message) {
   std::string result;
   result.reserve(error_code.size() + message.size() + 3U);
   result.append(error_code);
   result.append(": ");
   result.append(message);
   return result;
}

// Error messages for specific module-related errors

inline std::string duplicate_module_import(std::string_view namespace_uri) {
   std::string message = "Duplicate module import for namespace '";
   message.append(namespace_uri);
   message.append("'.");
   return format_error(XQST0047, message);
}

inline std::string export_not_in_namespace(std::string_view component_type, std::string_view qname, std::string_view expected_namespace) {
   std::string message;
   message.append(component_type);
   message.append(" '");
   message.append(qname);
   message.append("' is not in the target namespace '");
   message.append(expected_namespace);
   message.append("' of the library module.");
   return format_error(XQST0048, message);
}

inline std::string module_not_found(std::string_view namespace_uri) {
   std::string message = "Cannot locate module for namespace '";
   message.append(namespace_uri);
   message.append("'.  No valid location hints were provided or all locations failed to load.");
   return format_error(XQST0059, message);
}

inline std::string module_location_not_found(std::string_view location) {
   std::string message = "Cannot access module file at location '";
   message.append(location);
   message.append("'.  File does not exist or is not accessible.");
   return format_error(XQST0059, message);
}

inline std::string circular_module_dependency(std::string_view namespace_uri) {
   std::string message = "Circular module dependency detected when loading namespace '";
   message.append(namespace_uri);
   message.append("'.  Modules form a cycle in their import declarations.");
   return format_error(XQDY0054, message);
}

inline std::string not_library_module(std::string_view namespace_uri) {
   std::string message = "Module at namespace '";
   message.append(namespace_uri);
   message.append("' is not a library module.  Imported modules must begin with a module declaration.");
   return format_error(XQST0059, message);
}

inline std::string namespace_mismatch(std::string_view expected, std::string_view actual) {
   std::string message = "Module namespace mismatch: expected '";
   message.append(expected);
   message.append("' but module declares '");
   message.append(actual);
   message.append("'.  The module's declared namespace must match the import declaration.");
   return format_error(XQST0059, message);
}

inline std::string array_index_out_of_bounds(std::string_view detail) {
   return format_error(FOAY0001, detail);
}

inline std::string json_not_schema_aware(std::string_view detail) {
   return format_error(FOJS0004, detail);
}

} // namespace errors
} // namespace xquery
