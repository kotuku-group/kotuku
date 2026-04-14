// XPath Evaluator Common Utilities
//
// This translation unit provides shared utility functions used throughout the XPath evaluator for value
// comparisons, type coercion, and schema-aware operations.  These helpers maintain consistent behaviour
// across different parts of the evaluation pipeline.
//
// Key functionality includes:
//   - Schema type descriptor lookup and caching
//   - Comparison type resolution (numeric vs string vs boolean)
//   - Numeric equality testing with epsilon handling for floating-point values
//   - String normalisation and comparison utilities
//   - Type coercion rules for mixed-type comparisons
//
// By centralising these operations, the evaluator ensures that predicates, function calls, and expression
// evaluation all apply the same semantic rules for value comparison and type conversion.

#include "eval_detail.h"
#include "../../xml/schema/schema_types.h"
#include <algorithm>
#include <cmath>
#include <limits>

//********************************************************************************************************************
// Determines if the specified character is whitespace (space, tab, newline, or carriage return).

bool is_space_character(char Ch) noexcept
{
   return (Ch IS ' ') or (Ch IS '\t') or (Ch IS '\n') or (Ch IS '\r');
}

//********************************************************************************************************************
// Trims leading and trailing whitespace from a string view and returns the trimmed view.

std::string_view trim_view(std::string_view Text)
{
   while (!Text.empty() and is_space_character(Text.front())) Text.remove_prefix(1);
   while (!Text.empty() and is_space_character(Text.back())) Text.remove_suffix(1);
   return Text;
}

//********************************************************************************************************************
// Retrieves or looks up the schema type descriptor for a given XPath value. Uses cached type info if available,
// otherwise queries the schema registry for the value's schema type.

std::shared_ptr<xml::schema::SchemaTypeDescriptor> schema_descriptor_for_value(const XPathVal &Value)
{
   if (Value.schema_type_info) return Value.schema_type_info;

   auto &registry = xml::schema::registry();
   return registry.find_descriptor(Value.get_schema_type());
}

//********************************************************************************************************************
// Determines if two XPath values should be compared as booleans based on their types. Returns true if either
// value is already a boolean, or if both values' schema types support coercion to XPath boolean type.

bool should_compare_as_boolean(const XPathVal &Left, const XPathVal &Right)
{
   if ((Left.Type IS XPVT::NodeSet) or (Right.Type IS XPVT::NodeSet)) return false;
   if ((Left.Type IS XPVT::Boolean) or (Right.Type IS XPVT::Boolean)) return true;

   const auto left_descriptor = schema_descriptor_for_value(Left);
   const auto right_descriptor = schema_descriptor_for_value(Right);
   return left_descriptor and right_descriptor and
          left_descriptor->can_coerce_to(xml::schema::SchemaType::XPathBoolean) and
          right_descriptor->can_coerce_to(xml::schema::SchemaType::XPathBoolean);
}

//********************************************************************************************************************
// Determines if two XPath values should be compared as numeric values by checking if both values' schema
// descriptors support coercion to XPath number type.

bool should_compare_as_numeric(const XPathVal &Left, const XPathVal &Right)
{
   const auto left_descriptor = schema_descriptor_for_value(Left);
   const auto right_descriptor = schema_descriptor_for_value(Right);
   return left_descriptor and right_descriptor and
          left_descriptor->can_coerce_to(xml::schema::SchemaType::XPathNumber) and
          right_descriptor->can_coerce_to(xml::schema::SchemaType::XPathNumber);
}

//********************************************************************************************************************
// Compares two floating-point numbers for equality using epsilon-based tolerance to handle floating-point
// precision issues. Special handling for NaN (always unequal) and infinity (equal only if both infinite
// with same sign). Uses relative epsilon for values larger than 1.0 and absolute epsilon otherwise.

bool numeric_equal(double Left, double Right)
{
   if (std::isnan(Left) or std::isnan(Right)) return false;
   if (std::isinf(Left) or std::isinf(Right)) return Left IS Right;

   const double abs_left = std::fabs(Left);
   const double abs_right = std::fabs(Right);
   const double larger = std::max(abs_left, abs_right);

   if (larger <= 1.0) {
      return std::fabs(Left - Right) <= std::numeric_limits<double>::epsilon() * 16;
   }
   else return std::fabs(Left - Right) <= larger * std::numeric_limits<double>::epsilon() * 16;
}

//********************************************************************************************************************
// Performs relational comparisons (less than, greater than, etc.) between two numeric values. Returns false
// if either value is NaN, otherwise applies the specified comparison operator.

bool numeric_compare(double Left, double Right, RelationalOperator Operation)
{
   if (std::isnan(Left) or std::isnan(Right)) return false;

   switch (Operation) {
      case RelationalOperator::LESS: return Left < Right;
      case RelationalOperator::LESS_OR_EQUAL: return Left <= Right;
      case RelationalOperator::GREATER: return Left > Right;
      case RelationalOperator::GREATER_OR_EQUAL: return Left >= Right;
   }

   return false;
}

//********************************************************************************************************************
// Determines if the specified collation URI is supported by the XPath evaluator.

bool xpath_collation_supported(const std::string &Uri)
{
   if (Uri.empty()) return true;
   if (Uri IS "http://www.w3.org/2005/xpath-functions/collation/codepoint") return true;
   // Optionally support HTML ASCII case-insensitive collation if implemented:
   // if (Uri IS "http://www.w3.org/2005/xpath-functions/collation/html-ascii-case-insensitive") return true;
   return false;
}

//********************************************************************************************************************
// Determines if an XPath value is empty in the context of FLWOR order-by clauses (empty or NaN number).

bool xpath_order_key_is_empty(const XPathVal &Value)
{
   if (Value.is_empty()) return true;
   if (Value.Type IS XPVT::Number) {
      double number = Value.to_number();
      return std::isnan(number);
   }

   return false;
}

//********************************************************************************************************************
// Compares two numeric values, returning -1, 0, or 1 with special handling for NaN values.

static int compare_numeric_values(double Left, double Right)
{
   bool left_nan = std::isnan(Left);
   bool right_nan = std::isnan(Right);

   if (left_nan and right_nan) return 0;
   if (left_nan) return -1;
   if (right_nan) return 1;

   if (Left < Right) return -1;
   if (Left > Right) return 1;
   return 0;
}

//********************************************************************************************************************
// Compares two atomic XPath values for ordering, using the specified collation URI for string comparisons.

int xpath_compare_order_atomic(const XPathVal &LeftValue, const XPathVal &RightValue,
   const std::string &CollationUri)
{
   XPVT left_type = LeftValue.Type;
   XPVT right_type = RightValue.Type;
   bool left_numeric = (left_type IS XPVT::Number) or (left_type IS XPVT::Boolean);
   bool right_numeric = (right_type IS XPVT::Number) or (right_type IS XPVT::Boolean);

   if (left_numeric or right_numeric) {
      double left_number = LeftValue.to_number();
      double right_number = RightValue.to_number();
      return compare_numeric_values(left_number, right_number);
   }

   std::string left_string = LeftValue.to_string();
   std::string right_string = RightValue.to_string();

   if ((!CollationUri.empty()) and !(CollationUri IS "http://www.w3.org/2005/xpath-functions/collation/codepoint") and
      !(CollationUri IS "unicode")) {
      return 0;
   }

   if (left_string < right_string) return -1;
   if (left_string > right_string) return 1;
   return 0;
}

//********************************************************************************************************************
// Compares two order keys with options for empty handling, collation, and sort direction (ascending/descending).

int xpath_compare_order_keys(const XPathVal &LeftValue, bool LeftEmpty, const XPathVal &RightValue,
   bool RightEmpty, const XPathOrderComparatorOptions &Options)
{
   const bool empties_greatest = Options.has_empty_mode ? Options.empty_is_greatest : false;

   if (LeftEmpty or RightEmpty) {
      if (LeftEmpty and RightEmpty) return 0;
      return LeftEmpty ? (empties_greatest ? 1 : -1) : (empties_greatest ? -1 : 1);
   }

   const std::string collation = Options.has_collation ? Options.collation_uri : std::string();

   int comparison = xpath_compare_order_atomic(LeftValue, RightValue, collation);
   if (Options.descending) comparison = -comparison;
   return comparison;
}
