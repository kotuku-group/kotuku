//********************************************************************************************************************
// XPath Number Functions

XPathVal XPathFunctionLibrary::function_number(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   if (Args.empty()) {
      if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_set_value(nodes);
         return XPathVal(node_set_value.to_number());
      }
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }
   return XPathVal(Args[0].to_number());
}

XPathVal XPathFunctionLibrary::function_sum(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(0.0);
   if (Args[0].Type != XPVT::NodeSet) {
      return XPathVal(0.0);
   }

   const auto &nodeset = Args[0];
   double sum = 0.0;

   size_t length = nodeset.node_set.size();
   if (length < nodeset.node_set_attributes.size()) length = nodeset.node_set_attributes.size();
   if (length < nodeset.node_set_string_values.size()) length = nodeset.node_set_string_values.size();
   if ((length IS 0) and nodeset.node_set_string_override.has_value()) length = 1;

   bool use_override = nodeset.node_set_string_override.has_value() and nodeset.node_set_string_values.empty();

   for (size_t index = 0; index < length; ++index) {
      const XMLAttrib *attribute = nullptr;
      if (index < nodeset.node_set_attributes.size()) attribute = nodeset.node_set_attributes[index];

      std::string value_string;
      if (attribute) value_string = attribute->Value;
      else if (index < nodeset.node_set_string_values.size()) value_string = nodeset.node_set_string_values[index];
      else if (use_override and (index IS 0)) value_string = *nodeset.node_set_string_override;
      else if ((index < nodeset.node_set.size()) and nodeset.node_set[index]) {
         value_string = XPathVal::node_string_value(nodeset.node_set[index]);
      }
      else continue;

      double value = XPathVal::string_to_number(value_string);
      if (std::isnan(value)) continue;
      sum += value;
   }

   return XPathVal(sum);
}

XPathVal XPathFunctionLibrary::function_floor(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(std::numeric_limits<double>::quiet_NaN());
   double value = Args[0].to_number();
   if (std::isnan(value) or std::isinf(value)) return XPathVal(value);
   return XPathVal(std::floor(value));
}

XPathVal XPathFunctionLibrary::function_ceiling(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(std::numeric_limits<double>::quiet_NaN());
   double value = Args[0].to_number();
   if (std::isnan(value) or std::isinf(value)) return XPathVal(value);
   return XPathVal(std::ceil(value));
}

XPathVal XPathFunctionLibrary::function_round(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(std::numeric_limits<double>::quiet_NaN());
   double value = Args[0].to_number();
   if (std::isnan(value) or std::isinf(value)) return XPathVal(value);
   return XPathVal(std::round(value));
}

XPathVal XPathFunctionLibrary::function_round_half_to_even(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args.size() > 2) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = Args[0].to_number();
   if (std::isnan(value) or std::isinf(value)) return XPathVal(value);

   int precision = 0;
   if (Args.size() > 1) precision = (int)Args[1].to_number();

   double scaled = value;
   double factor = 1.0;
   bool negative_precision = false;

   if (precision > 0) {
      factor = std::pow(10.0, precision);
      if (std::isnan(factor) or std::isinf(factor) or factor IS 0.0) return XPathVal(value);
      scaled = value * factor;
      if (std::isnan(scaled) or std::isinf(scaled)) return XPathVal(value);
   }
   else if (precision < 0) {
      negative_precision = true;
      factor = std::pow(10.0, -precision);
      if (std::isnan(factor) or std::isinf(factor) or factor IS 0.0) return XPathVal(value);
      scaled = value / factor;
   }

   double rounded_scaled = std::nearbyint(scaled);

   if (std::isnan(rounded_scaled) or std::isinf(rounded_scaled)) return XPathVal(rounded_scaled);

   double result = rounded_scaled;
   if (precision > 0) {
      result = rounded_scaled / factor;
   }
   else if (negative_precision) {
      result = rounded_scaled * factor;
   }

   return XPathVal(result);
}

XPathVal XPathFunctionLibrary::function_abs(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = Args[0].to_number();
   if (std::isnan(value) or std::isinf(value)) return XPathVal(value);

   return XPathVal(std::fabs(value));
}

XPathVal XPathFunctionLibrary::function_min(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   std::vector<double> numbers;
   numbers.reserve(Args.size());

   for (const auto &arg : Args) append_numbers_from_value(arg, numbers);

   if (numbers.empty()) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double minimum = numbers[0];
   for (size_t index = 1; index < numbers.size(); ++index) {
      if (numbers[index] < minimum) minimum = numbers[index];
   }

   return XPathVal(minimum);
}

XPathVal XPathFunctionLibrary::function_max(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   std::vector<double> numbers;
   numbers.reserve(Args.size());

   for (const auto &arg : Args) append_numbers_from_value(arg, numbers);

   if (numbers.empty()) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double maximum = numbers[0];
   for (size_t index = 1; index < numbers.size(); ++index) {
      if (numbers[index] > maximum) maximum = numbers[index];
   }

   return XPathVal(maximum);
}

XPathVal XPathFunctionLibrary::function_avg(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   std::vector<double> numbers;
   numbers.reserve(Args.size());

   for (const auto &arg : Args) append_numbers_from_value(arg, numbers);

   if (numbers.empty()) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double total = 0.0;
   for (double value : numbers) total += value;

   return XPathVal(total / (double)numbers.size());
}

