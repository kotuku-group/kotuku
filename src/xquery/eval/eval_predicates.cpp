// XPath Predicate Evaluation
//
// This translation unit contains predicate and comparison logic for XPath expressions. It handles:
//
//   - Value comparisons (=, !=, eq, ne)
//   - Relational comparisons (<, >, <=, >=, lt, gt, le, ge)
//   - Node-set to scalar conversions for predicate contexts
//   - Schema-aware type coercion during comparisons
//
// The comparison routines consume shared utilities from xpath_evaluator_detail.h (numeric_equal, numeric_compare,
// schema helpers) to ensure consistent behaviour across the XPath evaluation pipeline.

//********************************************************************************************************************
// Extracts the string value from a node-set at the specified index, with special handling for overrides and attributes.

std::string node_set_string_value(const XPathVal &Value, size_t Index)
{
   if (Value.node_set_string_override.has_value() and (Index IS 0)) {
      return *Value.node_set_string_override;
   }

   if (Index < Value.node_set_string_values.size()) {
      return Value.node_set_string_values[Index];
   }

   if (Index >= Value.node_set.size()) return std::string();

   return XPathVal::node_string_value(Value.node_set[Index]);
}

//********************************************************************************************************************
// Converts the string value at the specified index to a numeric value using XPath conversion rules.

double node_set_number_value(const XPathVal &Value, size_t Index)
{
   std::string str = node_set_string_value(Value, Index);
   if (str.empty()) return std::numeric_limits<double>::quiet_NaN();

   return XPathVal::string_to_number(str);
}

//********************************************************************************************************************
// Promotes a value for comparison context by converting empty node-sets to empty optional or returning the value as-is.

std::optional<XPathVal> promote_value_comparison_operand(const XPathVal &Value)
{
   if (Value.Type IS XPVT::NodeSet) {
      if (Value.node_set.empty()) return std::nullopt;
      auto str = Value.to_string();
      return std::make_optional<XPathVal>(str);
   }

   return Value;
}

//********************************************************************************************************************
// Equality Comparison Logic

bool compare_xpath_values(const XPathVal &LeftValue, const XPathVal &RightValue)
{
   auto left_type = LeftValue.Type;
   auto right_type = RightValue.Type;

   bool schema_boolean = should_compare_as_boolean(LeftValue, RightValue);
   if (schema_boolean) {
      auto left_descriptor = schema_descriptor_for_value(LeftValue);
      auto right_descriptor = schema_descriptor_for_value(RightValue);

      bool left_boolean = LeftValue.to_boolean();
      bool right_boolean = RightValue.to_boolean();

      if (left_descriptor) {
         auto coerced = left_descriptor->coerce_value(LeftValue, xml::schema::SchemaType::XPathBoolean);
         left_boolean = coerced.to_boolean();
      }

      if (right_descriptor) {
         auto coerced = right_descriptor->coerce_value(RightValue, xml::schema::SchemaType::XPathBoolean);
         right_boolean = coerced.to_boolean();
      }

      return left_boolean IS right_boolean;
   }

   bool schema_numeric = should_compare_as_numeric(LeftValue, RightValue);
   if ((left_type IS XPVT::Boolean) or (right_type IS XPVT::Boolean)) {
      bool left_boolean = LeftValue.to_boolean();
      bool right_boolean = RightValue.to_boolean();
      return left_boolean IS right_boolean;
   }

   if ((left_type IS XPVT::Number) or (right_type IS XPVT::Number) or schema_numeric) {
      if ((left_type IS XPVT::NodeSet) or (right_type IS XPVT::NodeSet)) {
         const XPathVal &node_value = (left_type IS XPVT::NodeSet) ? LeftValue : RightValue;
         const XPathVal &number_value = (left_type IS XPVT::NodeSet) ? RightValue : LeftValue;

         double comparison_number = number_value.to_number();
         if (schema_numeric) {
            auto descriptor = schema_descriptor_for_value(number_value);
            if (descriptor) {
               auto coerced = descriptor->coerce_value(number_value, xml::schema::SchemaType::XPathNumber);
               comparison_number = coerced.to_number();
            }
         }

         if (std::isnan(comparison_number)) return false;

         for (size_t index = 0; index < node_value.node_set.size(); ++index) {
            double node_number = node_set_number_value(node_value, index);
            if (std::isnan(node_number)) continue;
            if (numeric_equal(node_number, comparison_number)) return true;
         }

         return false;
      }

      double left_number = LeftValue.to_number();
      double right_number = RightValue.to_number();

      if (schema_numeric) {
         if (auto descriptor = schema_descriptor_for_value(LeftValue)) {
            auto coerced = descriptor->coerce_value(LeftValue, xml::schema::SchemaType::XPathNumber);
            left_number = coerced.to_number();
         }

         if (auto descriptor = schema_descriptor_for_value(RightValue)) {
            auto coerced = descriptor->coerce_value(RightValue, xml::schema::SchemaType::XPathNumber);
            right_number = coerced.to_number();
         }
      }

      return numeric_equal(left_number, right_number);
   }

   if ((left_type IS XPVT::NodeSet) or (right_type IS XPVT::NodeSet)) {
      if ((left_type IS XPVT::NodeSet) and (right_type IS XPVT::NodeSet)) {
         for (size_t left_index = 0; left_index < LeftValue.node_set.size(); ++left_index) {
            std::string left_string = node_set_string_value(LeftValue, left_index);

            for (size_t right_index = 0; right_index < RightValue.node_set.size(); ++right_index) {
               std::string right_string = node_set_string_value(RightValue, right_index);
               if (left_string.compare(right_string) IS 0) return true;
            }
         }

         return false;
      }

      const XPathVal &node_value = (left_type IS XPVT::NodeSet) ? LeftValue : RightValue;
      const XPathVal &string_value = (left_type IS XPVT::NodeSet) ? RightValue : LeftValue;

      std::string comparison_string = string_value.to_string();

      for (size_t index = 0; index < node_value.node_set.size(); ++index) {
         std::string node_string = node_set_string_value(node_value, index);
         if (node_string.compare(comparison_string) IS 0) return true;
      }

      return false;
   }

   std::string left_string = LeftValue.to_string();
   std::string right_string = RightValue.to_string();
   return left_string.compare(right_string) IS 0;
}

//********************************************************************************************************************
// Relational Comparison Logic

bool compare_xpath_relational(const XPathVal &LeftValue, const XPathVal &RightValue, RelationalOperator Operation)
{
   auto left_type = LeftValue.Type;
   auto right_type = RightValue.Type;
   bool schema_numeric = should_compare_as_numeric(LeftValue, RightValue);

   if ((left_type IS XPVT::NodeSet) or (right_type IS XPVT::NodeSet)) {
      if ((left_type IS XPVT::NodeSet) and (right_type IS XPVT::NodeSet)) {
         for (size_t left_index = 0; left_index < LeftValue.node_set.size(); ++left_index) {
            double left_number = node_set_number_value(LeftValue, left_index);
            if (std::isnan(left_number)) continue;

            for (size_t right_index = 0; right_index < RightValue.node_set.size(); ++right_index) {
               double right_number = node_set_number_value(RightValue, right_index);
               if (std::isnan(right_number)) continue;
               if (numeric_compare(left_number, right_number, Operation)) return true;
            }
         }

         return false;
      }

      const XPathVal &node_value = (left_type IS XPVT::NodeSet) ? LeftValue : RightValue;
      const XPathVal &other_value = (left_type IS XPVT::NodeSet) ? RightValue : LeftValue;

      if (other_value.Type IS XPVT::Boolean) {
         bool node_boolean = node_value.to_boolean();
         bool other_boolean = other_value.to_boolean();
         double node_number = node_boolean ? 1.0 : 0.0;
         double other_number = other_boolean ? 1.0 : 0.0;
         return numeric_compare(node_number, other_number, Operation);
      }

      double other_number = other_value.to_number();
      if (schema_numeric) {
         if (auto descriptor = schema_descriptor_for_value(other_value)) {
            auto coerced = descriptor->coerce_value(other_value, xml::schema::SchemaType::XPathNumber);
            other_number = coerced.to_number();
         }
      }
      if (std::isnan(other_number)) return false;

      for (size_t index = 0; index < node_value.node_set.size(); ++index) {
         double node_number = node_set_number_value(node_value, index);
         if (std::isnan(node_number)) continue;
         if (numeric_compare(node_number, other_number, Operation)) return true;
      }

      return false;
   }

   double left_number = LeftValue.to_number();
   double right_number = RightValue.to_number();
   if (schema_numeric) {
      if (auto descriptor = schema_descriptor_for_value(LeftValue)) {
         auto coerced = descriptor->coerce_value(LeftValue, xml::schema::SchemaType::XPathNumber);
         left_number = coerced.to_number();
      }

      if (auto descriptor = schema_descriptor_for_value(RightValue)) {
         auto coerced = descriptor->coerce_value(RightValue, xml::schema::SchemaType::XPathNumber);
         right_number = coerced.to_number();
      }
   }
   return numeric_compare(left_number, right_number, Operation);
}
