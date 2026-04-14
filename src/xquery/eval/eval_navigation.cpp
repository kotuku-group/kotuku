// XPath Evaluator Navigation and Location Path Processing
//
// This translation unit implements location path evaluation for XPath, including axis navigation, node test
// matching, predicate application, and step sequencing.  It bridges the abstract syntax tree representation
// of location paths with the concrete traversal operations provided by the axis evaluator.
//
// Key functionality:
//   - Location path evaluation (evaluate_location_path, evaluate_step_ast)
//   - Axis dispatch and candidate filtering (dispatch_axis, expand_axis_candidates)
//   - Node test matching against tag names, wildcards, and node types (match_node_test)
//   - Predicate application to candidate node sets (apply_predicates_to_candidates)
//   - Step sequencing for multi-step location paths (evaluate_step_sequence)
//   - Integration with the callback mechanism for query results (invoke_callback, process_step_matches)
//
// The navigation system maintains document order semantics, applies predicates in the correct context,
// and handles both relative and absolute location paths.  By separating navigation concerns from
// expression evaluation, the code remains modular and testable.

//********************************************************************************************************************
// Axis Navigation Helpers
//
// Dispatches axis evaluation based on axis type, handling all XPath axis types (child, descendant, parent, ancestor,
// sibling, attribute, namespace, and their variants). Manages both element and attribute contexts, and returns a
// vector of axis matches containing node/attribute pairs. The function handles special cases like absolute paths
// (null context node) and attribute context restrictions on certain axes.

std::vector<XPathEvaluator::AxisMatch> XPathEvaluator::dispatch_axis(AxisType Axis,
   XTag *ContextNode, const XMLAttrib *ContextAttribute)
{
   std::vector<AxisMatch> matches;

   size_t estimated_capacity = axis_evaluator.estimate_result_size(Axis, ContextNode);
   matches.reserve(estimated_capacity);

   auto append_nodes = [this, &matches](NODES &nodes) {
      matches.reserve(matches.size() + nodes.size());
      for (auto *node : nodes) {
         matches.push_back({ node, nullptr });
      }
      arena.release_node_vector(nodes);
   };

   bool attribute_context = ContextAttribute != nullptr;

   switch (Axis) {
      case AxisType::CHILD: {
         if (attribute_context) break;

         if (not ContextNode) {
            if (xml) {
               for (auto &tag : xml->Tags) {
                  if (not tag.isTag()) continue;
                  matches.push_back({ &tag, nullptr });
               }
            }
         }
         else {
            auto &child_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::CHILD, ContextNode, child_buffer);
            append_nodes(child_buffer);
         }
         break;
      }

      case AxisType::DESCENDANT: {
         if (attribute_context) break;

         if (not ContextNode) {
            if (xml) {
               for (auto &tag : xml->Tags) {
                  if (not tag.isTag()) continue;
                  matches.push_back({ &tag, nullptr });
                  auto &desc_buffer = arena.acquire_node_vector();
                  axis_evaluator.evaluate_axis(AxisType::DESCENDANT, &tag, desc_buffer);
                  append_nodes(desc_buffer);
               }
            }
         }
         else {
            auto &desc_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::DESCENDANT, ContextNode, desc_buffer);
            append_nodes(desc_buffer);
         }
         break;
      }

      case AxisType::DESCENDANT_OR_SELF: {
         if (attribute_context) {
            matches.push_back({ ContextNode, ContextAttribute });
            break;
         }

         if (not ContextNode) {
            matches.push_back({ nullptr, nullptr });
            if (xml) {
               for (auto &tag : xml->Tags) {
                  if (not tag.isTag()) continue;
                  matches.push_back({ &tag, nullptr });
                  auto &desc_buffer = arena.acquire_node_vector();
                  axis_evaluator.evaluate_axis(AxisType::DESCENDANT, &tag, desc_buffer);
                  append_nodes(desc_buffer);
               }
            }
         }
         else {
            matches.push_back({ ContextNode, nullptr });
            auto &desc_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::DESCENDANT, ContextNode, desc_buffer);
            append_nodes(desc_buffer);
         }
         break;
      }

      case AxisType::SELF: {
         if (attribute_context) {
            matches.push_back({ ContextNode, ContextAttribute });
         }
         else matches.push_back({ ContextNode, nullptr });
         break;
      }

      case AxisType::PARENT: {
         if (attribute_context) {
            if (ContextNode) matches.push_back({ ContextNode, nullptr });
         }
         else if (ContextNode) {
            auto &parent_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::PARENT, ContextNode, parent_buffer);
            append_nodes(parent_buffer);
         }
         break;
      }

      case AxisType::ANCESTOR: {
         if (attribute_context) {
            if (ContextNode) {
               matches.push_back({ ContextNode, nullptr });
               auto &ancestor_buffer = arena.acquire_node_vector();
               axis_evaluator.evaluate_axis(AxisType::ANCESTOR, ContextNode, ancestor_buffer);
               append_nodes(ancestor_buffer);
            }
         }
         else if (ContextNode) {
            auto &ancestor_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::ANCESTOR, ContextNode, ancestor_buffer);
            append_nodes(ancestor_buffer);
         }
         break;
      }

      case AxisType::ANCESTOR_OR_SELF: {
         if (attribute_context) {
            matches.push_back({ ContextNode, ContextAttribute });
            if (ContextNode) {
               matches.push_back({ ContextNode, nullptr });
               auto &ancestor_buffer = arena.acquire_node_vector();
               axis_evaluator.evaluate_axis(AxisType::ANCESTOR, ContextNode, ancestor_buffer);
               append_nodes(ancestor_buffer);
            }
         }
         else if (ContextNode) {
            matches.push_back({ ContextNode, nullptr });
            auto &ancestor_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::ANCESTOR, ContextNode, ancestor_buffer);
            append_nodes(ancestor_buffer);
         }
         else matches.push_back({ nullptr, nullptr });
         break;
      }

      case AxisType::FOLLOWING_SIBLING: {
         if (attribute_context) break;
         if (ContextNode) {
            auto &sibling_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::FOLLOWING_SIBLING, ContextNode, sibling_buffer);
            append_nodes(sibling_buffer);
         }
         break;
      }

      case AxisType::PRECEDING_SIBLING: {
         if (attribute_context) break;
         if (ContextNode) {
            auto &sibling_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::PRECEDING_SIBLING, ContextNode, sibling_buffer);
            append_nodes(sibling_buffer);
         }
         break;
      }

      case AxisType::FOLLOWING: {
         if (attribute_context) break;
         if (ContextNode) {
            auto &following_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::FOLLOWING, ContextNode, following_buffer);
            append_nodes(following_buffer);
         }
         break;
      }

      case AxisType::PRECEDING: {
         if (attribute_context) break;
         if (ContextNode) {
            auto &preceding_buffer = arena.acquire_node_vector();
            axis_evaluator.evaluate_axis(AxisType::PRECEDING, ContextNode, preceding_buffer);
            append_nodes(preceding_buffer);
         }
         break;
      }

      case AxisType::ATTRIBUTE: {
         if (attribute_context) break;
         if (ContextNode and ContextNode->isTag()) {
            for (size_t index = 1; index < ContextNode->Attribs.size(); ++index) {
               matches.push_back({ ContextNode, &ContextNode->Attribs[index] });
            }
         }
         break;
      }

      case AxisType::NAMESPACE: {
         // Namespace axis is not supported. Record an error and return no matches.
         // See XPST0134: The namespace axis is not supported.
         record_error("XPST0134: The namespace axis is not supported.", true);

         // Former code:
         //if (attribute_context) break;
         //if (ContextNode) {
         //   auto &namespace_buffer = arena.acquire_node_vector();
         //   axis_evaluator.evaluate_axis(AxisType::NAMESPACE, ContextNode, namespace_buffer);
         //   append_nodes(namespace_buffer);
         //}
         break;
      }
   }

   return matches;
}

//********************************************************************************************************************
// Matches a candidate node or attribute against a node test expression. Handles wildcards, name tests (including
// namespace-aware matching with prefix resolution), node type tests (node(), text(), comment()), and processing
// instruction tests. Supports both attribute and element matching based on the axis type, with full wildcard
// support for both prefixes and local names.

bool XPathEvaluator::match_node_test(const XPathNode *NodeTest, AxisType Axis, XTag *Candidate, const XMLAttrib *Attribute, uint32_t CurrentPrefix)
{
   bool attribute_axis = (Axis IS AxisType::ATTRIBUTE) or ((Axis IS AxisType::SELF) and (Attribute != nullptr));

   auto resolve_namespace = [&](std::string_view Prefix, XTag *Scope) -> std::optional<uint32_t> {
      if (not xml) return std::nullopt;

      std::string prefix_string(Prefix);
      uint32_t namespace_hash = 0;
      XTag *lookup_scope = Scope ? Scope : context.context_node;
      int tag_id = lookup_scope ? lookup_scope->ID : 0;

      if (xml->resolvePrefix(prefix_string, tag_id, namespace_hash) IS ERR::Okay) {
         return namespace_hash;
      }

      if (lookup_scope and context.context_node and (lookup_scope != context.context_node)) {
         if (xml->resolvePrefix(prefix_string, context.context_node->ID, namespace_hash) IS ERR::Okay) {
            return namespace_hash;
         }
      }

      if (not prefix_string.empty()) {
         auto it = xml->Prefixes.find(prefix_string);
         if (it != xml->Prefixes.end()) return it->second;
      }

      return std::nullopt;
   };

   if (not NodeTest) {
      if (attribute_axis) return Attribute != nullptr;
      return Candidate != nullptr;
   }

   if (attribute_axis) {
      if (not Attribute) return false;
      if (NodeTest->type IS XQueryNodeType::NODE_TYPE_TEST) return NodeTest->value IS "node";
      if (NodeTest->type IS XQueryNodeType::WILDCARD) return true;

      if (NodeTest->type IS XQueryNodeType::NAME_TEST) {
         std::string_view test_name = NodeTest->value;
         if (test_name.empty()) return false;

         std::string_view attribute_name = Attribute->Name;

         std::string_view expected_prefix;
         std::string_view expected_local = test_name;

         if (auto colon = test_name.find(':'); colon != std::string::npos) {
            expected_prefix = test_name.substr(0, colon);
            expected_local = test_name.substr(colon + 1);
         }

         std::string_view candidate_prefix;
         std::string_view candidate_local = attribute_name;

         if (auto colon = attribute_name.find(':'); colon != std::string::npos) {
            candidate_prefix = attribute_name.substr(0, colon);
            candidate_local = attribute_name.substr(colon + 1);
         }

         bool wildcard_local = expected_local.find('*') != std::string::npos;
         bool local_matches = wildcard_local ? pf::wildcmp(expected_local, candidate_local) : pf::iequals(expected_local, candidate_local);
         if (not local_matches) return false;

         if ((not xml) or ((xml->Flags & XMF::NAMESPACE_AWARE) != XMF::NIL)) {
            bool wildcard_prefix = (not expected_prefix.empty()) and (expected_prefix IS "*");
            if (wildcard_prefix) return true;

            if (not expected_prefix.empty()) {
               auto expected_hash = resolve_namespace(expected_prefix, Candidate);
               if (not expected_hash) return false;
               if (candidate_prefix.empty()) return false;
               auto candidate_hash = resolve_namespace(candidate_prefix, Candidate);
               if (not candidate_hash) return false;
               return *candidate_hash IS *expected_hash;
            }

            return candidate_prefix.empty();
         }

         return pf::iequals(test_name, attribute_name);
      }

      return false;
   }

   if (NodeTest->type IS XQueryNodeType::NODE_TYPE_TEST) {
      if (NodeTest->value IS "node") return true;
      if (not Candidate) return false;

      if (NodeTest->value IS "text") {
         if (not Candidate->isContent()) return false;
         return ((Candidate->Flags & (XTF::COMMENT | XTF::INSTRUCTION | XTF::NOTATION)) IS XTF::NIL);
      }

      if (NodeTest->value IS "comment") {
         bool has_comment_flag = Candidate ? (((Candidate->Flags & XTF::COMMENT) IS XTF::NIL) ? false : true) : false;
         return Candidate and has_comment_flag;
      }

      return false;
   }

   if (NodeTest->type IS XQueryNodeType::PROCESSING_INSTRUCTION_TEST) {
      if (not Candidate) return false;
      if ((Candidate->Flags & XTF::INSTRUCTION) IS XTF::NIL) return false;

      if (NodeTest->value.empty()) return true;

      std::string_view candidate_name;
      if (not Candidate->Attribs.empty()) candidate_name = Candidate->Attribs[0].Name;

      if (not candidate_name.empty() and (candidate_name.front() IS '?')) candidate_name.remove_prefix(1);
      if (candidate_name.empty()) return false;

      std::string candidate_target(candidate_name);
      return pf::iequals(candidate_target, NodeTest->value);
   }

   if (not Candidate) return false;

   if (NodeTest->type IS XQueryNodeType::WILDCARD) return Candidate->isTag();

   if (NodeTest->type IS XQueryNodeType::NAME_TEST) {
      std::string_view test_name = NodeTest->value;
      if (test_name.empty()) return false;

      std::string_view candidate_name = Candidate->name();

      if ((not xml) or ((xml->Flags & XMF::NAMESPACE_AWARE) != XMF::NIL)) {
         std::string_view expected_prefix;
         std::string_view expected_local = test_name;

         if (auto colon = test_name.find(':'); colon != std::string::npos) {
            expected_prefix = test_name.substr(0, colon);
            expected_local = test_name.substr(colon + 1);
         }

         std::string_view candidate_prefix;
         std::string_view candidate_local = candidate_name;

         if (auto colon = candidate_name.find(':'); colon != std::string::npos) {
            candidate_prefix = candidate_name.substr(0, colon);
            candidate_local = candidate_name.substr(colon + 1);
         }

         bool wildcard_local = expected_local.find('*') != std::string::npos;
         bool name_matches = wildcard_local ? pf::wildcmp(expected_local, candidate_local) : pf::iequals(expected_local, candidate_local);
         if (not name_matches) return false;

         if (not expected_prefix.empty()) {
            bool wildcard_prefix = expected_prefix IS "*";
            if (wildcard_prefix) return Candidate->isTag();

            auto expected_hash = resolve_namespace(expected_prefix, Candidate);
            if (not expected_hash) return false;
            return Candidate->NamespaceID IS *expected_hash;
         }

         auto default_hash = resolve_namespace(std::string_view(), Candidate);
         uint32_t expected_namespace = default_hash ? *default_hash : 0u;
         return Candidate->NamespaceID IS expected_namespace;
      }

      if (test_name.find('*') != std::string::npos) return pf::wildcmp(test_name, candidate_name);

      return pf::iequals(test_name, candidate_name);
   }

   return false;
}
