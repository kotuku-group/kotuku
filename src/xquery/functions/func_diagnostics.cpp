//********************************************************************************************************************
// XPath Diagnostics Functions

#include <format>

XPathVal XPathFunctionLibrary::function_error(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string error_code = "err:FOER0000";
   std::string description = "User-defined error";
   std::string detail;

   if (not Args.empty()) {
      const XPathVal &code_value = Args[0];
      if (not code_value.is_empty()) error_code = code_value.to_string();
   }

   if (Args.size() > 1) {
      const XPathVal &description_value = Args[1];
      if (not description_value.is_empty()) description = description_value.to_string();
   }

   if (Args.size() > 2) {
      const XPathVal &detail_value = Args[2];
      if (not detail_value.is_empty()) detail = describe_xpath_value(detail_value);
   }

   pf::Log log(__FUNCTION__);

   if (detail.empty()) {
      log.warning("XPath error (%s): %s", error_code.c_str(), description.c_str());
   }
   else {
      log.warning("XPath error (%s): %s [%s]", error_code.c_str(), description.c_str(), detail.c_str());
   }

   if (Context.expression_unsupported) {
      *Context.expression_unsupported = true;
   }

   if (Context.xml) {
      if (not Context.xml->ErrorMsg.empty()) Context.xml->ErrorMsg.append("\n");
      if (detail.empty()) {
         Context.xml->ErrorMsg.append(std::format("XPath error {}: {}", error_code, description));
      }
      else {
         Context.xml->ErrorMsg.append(std::format("XPath error {}: {} [{}]", error_code, description, detail));
      }
   }

   return XPathVal();
}

XPathVal XPathFunctionLibrary::function_trace(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal();

   const XPathVal &value = Args[0];
   std::string label = "trace";

   if (Args.size() > 1) {
      const XPathVal &label_value = Args[1];
      if (not label_value.is_empty()) label = label_value.to_string();
   }

   if (label.empty()) label = "trace";

   std::string summary = describe_xpath_value(value);
   if (summary.empty()) summary = std::string("()");

   pf::Log log(__FUNCTION__);
   log.msg("XPath trace [%s]: %s", label.c_str(), summary.c_str());

   (void)Context;

   return value;
}
