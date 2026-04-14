//********************************************************************************************************************
// XPath Boolean Functions

XPathVal XPathFunctionLibrary::function_boolean(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(false);
   return XPathVal(Args[0].to_boolean());
}

XPathVal XPathFunctionLibrary::function_not(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(true);
   return XPathVal(!Args[0].to_boolean());
}

XPathVal XPathFunctionLibrary::function_true(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   return XPathVal(true);
}

XPathVal XPathFunctionLibrary::function_false(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   return XPathVal(false);
}

XPathVal XPathFunctionLibrary::function_lang(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 1) return XPathVal(false);

   std::string requested = Args[0].to_string();
   if (requested.empty()) return XPathVal(false);

   XTag *node = Context.context_node;
   if (not node) return XPathVal(false);

   std::string language = find_language_for_node(node, Context.xml);
   if (language.empty()) return XPathVal(false);

   return XPathVal(language_matches(language, requested));
}

XPathVal XPathFunctionLibrary::function_exists(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(false);

   const XPathVal &value = Args[0];

   if (value.Type IS XPVT::NodeSet) {
      if (not value.node_set.empty()) return XPathVal(true);
      if (value.node_set_string_override.has_value()) return XPathVal(true);
      if (not value.node_set_string_values.empty()) return XPathVal(true);
      if (not value.node_set_attributes.empty()) return XPathVal(true);
      return XPathVal(false);
   }

   return XPathVal(true);
}
