//********************************************************************************************************************
// XPath Node Set Functions

XPathVal XPathFunctionLibrary::function_last(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   return XPathVal((double)Context.size);
}

XPathVal XPathFunctionLibrary::function_position(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   return XPathVal((double)Context.position);
}

XPathVal XPathFunctionLibrary::function_count(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   if (Args.size() != 1) return XPathVal(0.0);
   if (Args[0].Type != XPVT::NodeSet) return XPathVal(0.0);
   return XPathVal((double)Args[0].node_set.size());
}

XPathVal XPathFunctionLibrary::function_id(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   pf::vector<XTag *> results;

   if (Args.empty()) return XPathVal(results);

   std::unordered_set<std::string> requested_ids;

   auto add_tokens = [&](const std::string &Value) {
      size_t start = Value.find_first_not_of(" \t\r\n");
      while (start != std::string::npos) {
         size_t end = Value.find_first_of(" \t\r\n", start);
         std::string token = Value.substr(start, (end IS std::string::npos) ? std::string::npos : end - start);
         if (not token.empty()) requested_ids.insert(token);
         if (end IS std::string::npos) break;
         start = Value.find_first_not_of(" \t\r\n", end);
      }
   };

   auto collect_from_value = [&](const XPathVal &Value) {
      switch (Value.Type) {
         case XPVT::NodeSet: {
            if (not Value.node_set_string_values.empty()) {
               for (const auto &entry : Value.node_set_string_values) add_tokens(entry);
            }
            else if (Value.node_set_string_override.has_value()) add_tokens(*Value.node_set_string_override);
            else {
               for (auto *node : Value.node_set) {
                  if (not node) continue;
                  add_tokens(node->getContent());
               }
            }
            break;
         }

         case XPVT::String:
         case XPVT::Date:
         case XPVT::Time:
         case XPVT::DateTime:
            add_tokens(Value.StringValue);
            break;

         case XPVT::Boolean:
            add_tokens(Value.to_string());
            break;

         case XPVT::Number:
            if (not std::isnan(Value.NumberValue)) add_tokens(Value.to_string());
            break;

         case XPVT::Map:
         case XPVT::Array:
            // Maps and arrays cannot contribute ID tokens.
            break;
      }
   };

   for (const auto &arg : Args) collect_from_value(arg);

   if (requested_ids.empty()) return XPathVal(results);
   if (not Context.xml) return XPathVal(results);

   std::unordered_set<int> seen_tags;

   std::function<void(XTag &)> visit = [&](XTag &Tag) {
      if (Tag.isTag()) {
         for (size_t index = 1; index < Tag.Attribs.size(); ++index) {
            const auto &attrib = Tag.Attribs[index];
            if (not (pf::iequals(attrib.Name, "id") or pf::iequals(attrib.Name, "xml:id"))) continue;

            size_t start = attrib.Value.find_first_not_of(" \t\r\n");
            while (start != std::string::npos) {
               size_t end = attrib.Value.find_first_of(" \t\r\n", start);
               std::string token = attrib.Value.substr(start, (end IS std::string::npos) ? std::string::npos : end - start);
               if (not token.empty() and (requested_ids.find(token) != requested_ids.end())) {
                  if (seen_tags.insert(Tag.ID).second) results.push_back(&Tag);
                  break;
               }
               if (end IS std::string::npos) break;
               start = attrib.Value.find_first_not_of(" \t\r\n", end);
            }
         }
      }

      for (auto &child : Tag.Children) visit(child);
   };

   for (auto &root : Context.xml->Tags) visit(root);

   return XPathVal(results);
}

XPathVal XPathFunctionLibrary::function_local_name(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   XTag *target_node = nullptr;
   const XMLAttrib *target_attribute = nullptr;

   if (Args.empty()) {
      target_node = Context.context_node;
      target_attribute = Context.attribute_node;
   }
   else if (Args[0].Type IS XPVT::NodeSet) target_node = Args[0].node_set.empty() ? nullptr : Args[0].node_set[0];
   else return XPathVal(std::string());

   if (target_attribute) {
      std::string_view name = target_attribute->Name;
      if (!name.empty() and name.front() IS '?') return XPathVal(std::string(name.substr(1)));
      auto colon = name.find(':');
      if (colon IS std::string::npos) return XPathVal(std::string(name));
      return XPathVal(std::string(name.substr(colon + 1)));
   }

   if (not target_node) return XPathVal(std::string());
   if (target_node->Attribs.empty()) return XPathVal(std::string());

   std::string_view node_name = target_node->Attribs[0].Name;
   if (!node_name.empty() and node_name.front() IS '?') return XPathVal(std::string(node_name.substr(1)));
   if (node_name.empty()) return XPathVal(std::string());

   auto colon = node_name.find(':');
   if (colon IS std::string::npos) return XPathVal(std::string(node_name));
   return XPathVal(std::string(node_name.substr(colon + 1)));
}

XPathVal XPathFunctionLibrary::function_namespace_uri(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   XTag *target_node = nullptr;
   const XMLAttrib *target_attribute = nullptr;

   if (Args.empty()) {
      target_node = Context.context_node;
      target_attribute = Context.attribute_node;
   }
   else if (Args[0].Type IS XPVT::NodeSet) target_node = Args[0].node_set.empty() ? nullptr : Args[0].node_set[0];
   else return XPathVal(std::string());

   if (target_attribute) {
      std::string_view name = target_attribute->Name;
      auto colon = name.find(':');
      if (colon IS std::string::npos) return XPathVal(std::string());

      std::string prefix(name.substr(0, colon));
      if (pf::iequals(prefix, "xml")) return XPathVal("http://www.w3.org/XML/1998/namespace");
      if (pf::iequals(prefix, "xmlns")) return XPathVal("http://www.w3.org/2000/xmlns/");

      XTag *scope_node = target_node ? target_node : Context.context_node;
      if (not scope_node) return XPathVal(std::string());

      if (Context.xml) {
         std::string uri = find_in_scope_namespace(scope_node, Context.xml, prefix);
         return XPathVal(uri);
      }

      return XPathVal(std::string());
   }

   if (not target_node) return XPathVal(std::string());

   std::string prefix;
   if (not target_node->Attribs.empty()) {
      std::string_view node_name = target_node->Attribs[0].Name;
      auto colon = node_name.find(':');
      if (colon != std::string::npos) prefix = std::string(node_name.substr(0, colon));
   }

   if (not prefix.empty()) {
      if (pf::iequals(prefix, "xml")) return XPathVal("http://www.w3.org/XML/1998/namespace");
      if (pf::iequals(prefix, "xmlns")) return XPathVal("http://www.w3.org/2000/xmlns/");
   }

   if ((target_node->NamespaceID != 0) and Context.xml) {
      if (auto uri = Context.xml->getNamespaceURI(target_node->NamespaceID)) return XPathVal(*uri);
   }

   if (Context.xml) {
      std::string uri;
      if (not prefix.empty()) uri = find_in_scope_namespace(target_node, Context.xml, prefix);
      else uri = find_in_scope_namespace(target_node, Context.xml, std::string());
      return XPathVal(uri);
   }

   return XPathVal(std::string());
}

XPathVal XPathFunctionLibrary::function_name(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   XTag *target_node = nullptr;
   const XMLAttrib *target_attribute = nullptr;

   if (Args.empty()) {
      target_node = Context.context_node;
      target_attribute = Context.attribute_node;
   }
   else if (Args[0].Type IS XPVT::NodeSet) target_node = Args[0].node_set.empty() ? nullptr : Args[0].node_set[0];
   else return XPathVal(std::string());

   if (target_attribute) return XPathVal(target_attribute->Name);

   if (not target_node) return XPathVal(std::string());
   if (target_node->Attribs.empty()) return XPathVal(std::string());

   std::string name = target_node->Attribs[0].Name;
   if (!name.empty() and name.front() IS '?') name.erase(0, 1);
   return XPathVal(std::move(name));
}
