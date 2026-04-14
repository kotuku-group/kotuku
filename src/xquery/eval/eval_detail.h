// XPath Evaluator Detail - Shared Utilities

#pragma once

#include "../../xml/xpath_value.h"

#include <memory>
#include <optional>
#include <string>

enum class RelationalOperator {
   LESS,
   LESS_OR_EQUAL,
   GREATER,
   GREATER_OR_EQUAL
};

std::shared_ptr<xml::schema::SchemaTypeDescriptor> schema_descriptor_for_value(const XPathVal &Value);
bool should_compare_as_boolean(const XPathVal &Left, const XPathVal &Right);
bool should_compare_as_numeric(const XPathVal &Left, const XPathVal &Right);

bool numeric_equal(double Left, double Right);
bool numeric_compare(double Left, double Right, RelationalOperator Operation);

struct XPathOrderComparatorOptions {
   bool descending = false;
   bool has_empty_mode = false;
   bool empty_is_greatest = false;
   bool has_collation = false;
   std::string collation_uri;
};

bool xpath_collation_supported(const std::string &Uri);
bool xpath_order_key_is_empty(const XPathVal &Value);
int xpath_compare_order_atomic(const XPathVal &LeftValue, const XPathVal &RightValue, const std::string &CollationUri);
int xpath_compare_order_keys(const XPathVal &LeftValue, bool LeftEmpty, const XPathVal &RightValue,
   bool RightEmpty, const XPathOrderComparatorOptions &Options);

// String and whitespace utilities
bool is_space_character(char Ch) noexcept;
std::string_view trim_view(std::string_view Text);

// Predicate value extraction and comparison (implemented in xpath_evaluator_predicates.cpp)
std::string node_set_string_value(const XPathVal &Value, size_t Index);
double node_set_number_value(const XPathVal &Value, size_t Index);
std::optional<XPathVal> promote_value_comparison_operand(const XPathVal &Value);
bool compare_xpath_values(const XPathVal &LeftValue, const XPathVal &RightValue);
bool compare_xpath_relational(const XPathVal &LeftValue, const XPathVal &RightValue, RelationalOperator Operation);
