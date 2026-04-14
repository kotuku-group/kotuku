// XQuery 3.0 Math Namespace Functions
// Namespace: http://www.w3.org/2005/xpath-functions/math

#include <cmath>
#include <limits>
#include <numbers>

#include "../api/xquery_functions.h"

namespace
{
   constexpr double M_PI_VALUE = std::numbers::pi_v<double>;

   bool math_argument_is_empty_sequence(const std::vector<XPathVal> &Args, size_t index)
   {
      if (index >= Args.size()) return true;

      const XPathVal &value = Args[index];
      if (value.Type IS XPVT::NodeSet) {
         if (not value.node_set.empty()) return false;
         if (value.node_set_string_override.has_value()) return false;
         if (not value.node_set_string_values.empty()) return false;
         if (not value.node_set_attributes.empty()) return false;
         return true;
      }

      return false;
   }

   XPathVal math_nan()
   {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   // Normalises zero-valued results whilst preserving the sign of negative zero for IEEE-754 compliance.
   XPathVal math_numeric(double value)
   {
      if (value IS 0.0) value = std::copysign(0.0, value);
      return XPathVal(value);
   }
}

XPathVal XPathFunctionLibrary::function_math_pi(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   return math_numeric(M_PI_VALUE);
}

XPathVal XPathFunctionLibrary::function_math_sin(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();
   if (std::isinf(value)) return math_nan();

   double result = std::sin(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_cos(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();
   if (std::isinf(value)) return math_nan();

   double result = std::cos(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_tan(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();
   if (std::isinf(value)) return math_nan();

   double result = std::tan(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_asin(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::asin(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_acos(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::acos(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_atan(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::atan(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_atan2(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0) or math_argument_is_empty_sequence(Args, 1)) return XPathVal();

   double y = Args[0].to_number();
   double x = Args[1].to_number();

   if (std::isnan(y) or std::isnan(x)) return math_nan();

   double result = std::atan2(y, x);
   if (result IS 0.0) result = std::copysign(0.0, y);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_exp(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::exp(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_exp10(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::pow(10.0, value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_log(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::log(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_log10(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::log10(value);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_pow(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0) or math_argument_is_empty_sequence(Args, 1)) return XPathVal();

   double base = Args[0].to_number();
   double exponent = Args[1].to_number();

   double result = std::pow(base, exponent);
   return math_numeric(result);
}

XPathVal XPathFunctionLibrary::function_math_sqrt(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (math_argument_is_empty_sequence(Args, 0)) return XPathVal();

   double value = Args[0].to_number();
   if (std::isnan(value)) return math_nan();

   double result = std::sqrt(value);
   return math_numeric(result);
}
