//********************************************************************************************************************
// XPath Axis Evaluation System Implementation
//
// The axis evaluator encapsulates the traversal rules needed to support XPath's location steps in Kotuku's XML
// engine.  XPath exposes a wide variety of axes—child, ancestor, following, namespace, and so forth—that each
// describe a different relationship between nodes.  Translating those abstract relationships into concrete
// navigation across the engine's tree representation requires a fair amount of bookkeeping: we need to preserve
// document order, honour namespace scoping, emulate axes that are not represented explicitly in the DOM (such as
// attributes or namespaces), and provide deterministic handling for synthetic nodes used by the evaluator.
//
// This file implements the traversal logic in a stand-alone helper so that the tokeniser, parser, and evaluator can
// remain focused on syntactic and semantic concerns.  The AxisEvaluator is responsible for translating an AxisType
// into a set of XTag pointers, normalising the resulting node sets, and providing utility operations that encode
// XPath's ordering semantics.  Splitting these operations out keeps the evaluator readable and makes it easier to
// extend axis support in the future (for example, by adding document order caches or debugging hooks).

struct AxisNameMapping {
   AxisType Type;
   std::string_view Name;
};

constexpr static std::array axis_mappings {
   AxisNameMapping{ AxisType::CHILD, "child" },
   AxisNameMapping{ AxisType::DESCENDANT, "descendant" },
   AxisNameMapping{ AxisType::DESCENDANT_OR_SELF, "descendant-or-self" },
   AxisNameMapping{ AxisType::FOLLOWING, "following" },
   AxisNameMapping{ AxisType::FOLLOWING_SIBLING, "following-sibling" },
   AxisNameMapping{ AxisType::PARENT, "parent" },
   AxisNameMapping{ AxisType::ANCESTOR, "ancestor" },
   AxisNameMapping{ AxisType::ANCESTOR_OR_SELF, "ancestor-or-self" },
   AxisNameMapping{ AxisType::PRECEDING, "preceding" },
   AxisNameMapping{ AxisType::PRECEDING_SIBLING, "preceding-sibling" },
   AxisNameMapping{ AxisType::SELF, "self" },
   AxisNameMapping{ AxisType::ATTRIBUTE, "attribute" },
   AxisNameMapping{ AxisType::NAMESPACE, "namespace" }
};

constexpr static std::array reverse_axes {
   AxisType::ANCESTOR,
   AxisType::ANCESTOR_OR_SELF,
   AxisType::PRECEDING,
   AxisType::PRECEDING_SIBLING
};

//********************************************************************************************************************
// Dispatch helper that selects the concrete traversal routine for a requested axis.

void AxisEvaluator::evaluate_axis(AxisType Axis, XTag *ContextNode, NODES &Output) {
   Output.clear();
   if (ContextNode) {
      size_t hint = estimate_result_size(Axis, ContextNode);
      if (hint > 0) Output.reserve(hint);
   }

   switch (Axis) {
      case AxisType::CHILD:
         if (ContextNode) evaluate_child_axis(ContextNode, Output);
         break;
      case AxisType::DESCENDANT:
         if (ContextNode) evaluate_descendant_axis(ContextNode, Output);
         break;
      case AxisType::PARENT:
         if (ContextNode) evaluate_parent_axis(ContextNode, Output);
         break;
      case AxisType::ANCESTOR:
         if (ContextNode) evaluate_ancestor_axis(ContextNode, Output);
         break;
      case AxisType::FOLLOWING_SIBLING:
         if (ContextNode) evaluate_following_sibling_axis(ContextNode, Output);
         break;
      case AxisType::PRECEDING_SIBLING:
         if (ContextNode) evaluate_preceding_sibling_axis(ContextNode, Output);
         break;
      case AxisType::FOLLOWING:
         if (ContextNode) evaluate_following_axis(ContextNode, Output);
         break;
      case AxisType::PRECEDING:
         if (ContextNode) evaluate_preceding_axis(ContextNode, Output);
         break;
      case AxisType::NAMESPACE:
         if (ContextNode) evaluate_namespace_axis(ContextNode, Output);
         break;
      case AxisType::SELF:
         if (ContextNode) evaluate_self_axis(ContextNode, Output);
         break;
      case AxisType::DESCENDANT_OR_SELF:
         if (ContextNode) evaluate_descendant_or_self_axis(ContextNode, Output);
         break;
      case AxisType::ANCESTOR_OR_SELF:
         if (ContextNode) evaluate_ancestor_or_self_axis(ContextNode, Output);
         break;
      default:
         break;
   }
}
//********************************************************************************************************************
// Clear any synthetic namespace nodes created by namespace axis evaluation.

void AxisEvaluator::reset_namespace_nodes() {
   recycle_namespace_nodes();
}

//********************************************************************************************************************
// Translate an axis identifier from the query text into the internal AxisType enumeration.

AxisType AxisEvaluator::parse_axis_name(std::string_view AxisName) {
   auto match = std::ranges::find_if(axis_mappings, [AxisName](const AxisNameMapping &Entry) {
      return AxisName IS Entry.Name;
   });

   if (match != axis_mappings.end()) return match->Type;
   return AxisType::CHILD;
}

//********************************************************************************************************************
// Convert an AxisType back into its textual representation for logging and debugging purposes.

std::string_view AxisEvaluator::axis_name_to_string(AxisType Axis) {
   auto match = std::ranges::find_if(axis_mappings, [Axis](const AxisNameMapping &Entry) {
      return Axis IS Entry.Type;
   });

   if (match != axis_mappings.end()) return match->Name;
   return "child";
}

//********************************************************************************************************************
// Determine whether the supplied axis walks the document tree in reverse order.

bool AxisEvaluator::is_reverse_axis(AxisType Axis) {
   return std::ranges::any_of(reverse_axes, [Axis](AxisType Candidate) {
      return Candidate IS Axis;
   });
}

//********************************************************************************************************************
// Perform an ID-based lookup with caching to avoid repeated depth-first scans.
//
// TODO: There *might* be an issue with using find_parent() on dynamically allocated XTag nodes during
// expression evaluation.  Haven't checked to confirm this yet.

XTag * AxisEvaluator::find_parent(XTag *ReferenceNode)
{
   if ((!ReferenceNode) or (!xml)) return nullptr;

   auto parent_id = ReferenceNode->ParentID;
   if (parent_id IS 0) return nullptr;

   auto resolve_in_document = [ReferenceNode, parent_id](extXML *Document) -> XTag * {
      if (!Document) return nullptr;

      auto &map = Document->getMap();
      auto node_iter = map.find(ReferenceNode->ID);
      if (node_iter IS map.end()) return nullptr;
      if (!(node_iter->second IS ReferenceNode)) return nullptr;

      auto parent_iter = map.find(parent_id);
      if (parent_iter IS map.end()) return nullptr;
      return parent_iter->second;
   };

   if (auto *parent = resolve_in_document(xml)) return parent;

   if (!state->XMLCache.empty()) {
      for (auto &entry : state->XMLCache) {
         auto *other_xml = entry.second;
         if (auto *parent = resolve_in_document(other_xml)) return parent;
      }
   }

   return nullptr;
}

//********************************************************************************************************************
// Estimate the likely result size for an axis to enable optimal vector pre-sizing.

size_t AxisEvaluator::estimate_result_size(AxisType Axis, XTag *ContextNode)
{
   if (!ContextNode) return 0;

   switch (Axis) {
      case AxisType::CHILD:
         return ContextNode->Children.size();

      case AxisType::DESCENDANT:
      case AxisType::DESCENDANT_OR_SELF:
         // Conservative estimate: depth * average children per node
         return ContextNode->Children.size() * 4;

      case AxisType::PARENT:
      case AxisType::SELF:
         return 1;

      case AxisType::ANCESTOR:
      case AxisType::ANCESTOR_OR_SELF:
         // Typical XML depth estimate
         return 10;

      case AxisType::FOLLOWING_SIBLING:
      case AxisType::PRECEDING_SIBLING: {
         XTag *parent = find_parent(ContextNode);
         return parent ? parent->Children.size() : 0;
      }

      case AxisType::FOLLOWING:
      case AxisType::PRECEDING:
         // Conservative estimate for document-order traversal
         return 20;

      case AxisType::ATTRIBUTE:
         return ContextNode->Attribs.size();

      case AxisType::NAMESPACE:
         return 5; // Typical namespace count estimate

      default:
         return 4; // Conservative fallback
   }
}

//********************************************************************************************************************
// Standard child-axis traversal: collect direct children in document order.

void AxisEvaluator::evaluate_child_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   Output.reserve(Node->Children.size());

   for (auto &child : Node->Children) {
      Output.push_back(&child);
   }
}

//********************************************************************************************************************
// Depth-first walk that flattens all descendant tags beneath the context node.

void AxisEvaluator::evaluate_descendant_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   size_t estimated_size = Node->Children.size() * 4;
   Output.reserve(estimated_size);

   auto &stack = arena.acquire_node_vector(Node->Children.size());

   for (auto &child : Node->Children) {
      auto *child_ptr = &child;
      Output.push_back(child_ptr);
      if (child.isTag()) stack.push_back(child_ptr);
   }

   while (!stack.empty()) {
      XTag *current = stack.back();
      stack.pop_back();

      for (auto &grandchild : current->Children) {
         auto *grandchild_ptr = &grandchild;
         Output.push_back(grandchild_ptr);
         if (grandchild.isTag()) stack.push_back(grandchild_ptr);
      }
   }

   arena.release_node_vector(stack);
}

//********************************************************************************************************************
// Parent axis resolves a single parent node by ID reference.

void AxisEvaluator::evaluate_parent_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if ((Node) and (Node->ParentID != 0)) {
      if (auto *parent = find_parent(Node)) {
         Output.push_back(parent);
      }
   }
}

//********************************************************************************************************************
// Ascend towards the root, collecting each ancestor encountered along the way.

void AxisEvaluator::evaluate_ancestor_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   Output.reserve(10);

   XTag *parent = find_parent(Node);
   while (parent) {
      Output.push_back(parent);
      parent = find_parent(parent);
   }
}

//********************************************************************************************************************
// Enumerate siblings that appear after the context node under the same parent.

void AxisEvaluator::evaluate_following_sibling_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   XTag *parent = find_parent(Node);
   if (!parent) return;

   Output.reserve(parent->Children.size());

   bool found_self = false;
   for (auto &child : parent->Children) {
      if (found_self) {
         Output.push_back(&child);
      }

      if (&child IS Node) {
         found_self = true;
      }
   }
}

//********************************************************************************************************************
// Enumerate siblings that appear before the context node under the same parent.

void AxisEvaluator::evaluate_preceding_sibling_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   XTag *parent = find_parent(Node);
   if (!parent) return;

   Output.reserve(parent->Children.size());

   for (auto &child : parent->Children) {
      if (&child IS Node) {
         break;
      }

      Output.push_back(&child);
   }
   std::reverse(Output.begin(), Output.end());
}

//********************************************************************************************************************
// Following axis enumerates nodes that appear after the context node in document order.

void AxisEvaluator::evaluate_following_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   auto *parent_for_hint = find_parent(Node);
   auto &siblings = arena.acquire_node_vector(parent_for_hint ? parent_for_hint->Children.size() : 0);
   evaluate_following_sibling_axis(Node, siblings);

   for (auto *sibling : siblings) {
      Output.push_back(sibling);

      if (sibling->isTag()) {
         auto &descendants = arena.acquire_node_vector();
         evaluate_descendant_axis(sibling, descendants);
         Output.insert(Output.end(), descendants.begin(), descendants.end());
         arena.release_node_vector(descendants);
      }
   }

   arena.release_node_vector(siblings);

   if (auto *parent = find_parent(Node)) {
      auto &parent_following = arena.acquire_node_vector(estimate_result_size(AxisType::FOLLOWING, parent));
      evaluate_following_axis(parent, parent_following);
      Output.insert(Output.end(), parent_following.begin(), parent_following.end());
      arena.release_node_vector(parent_following);
   }
}

//********************************************************************************************************************
// Helper that traverses a subtree in reverse document order, used by the preceding axis.

void AxisEvaluator::collect_subtree_reverse(XTag *Node, NODES &Output)
{
   if (!Node) return;

   for (auto child = Node->Children.rbegin(); child != Node->Children.rend(); ++child) {
      collect_subtree_reverse(&(*child), Output);
   }

   Output.push_back(Node);
}

//********************************************************************************************************************
// Collect namespace declarations using optimized flat vector approach

void AxisEvaluator::collect_namespace_declarations(XTag *Node, std::vector<NamespaceDeclaration> &declarations)
{
   if (!Node) return;

   // Reuse storage vectors to avoid allocations
   visited_node_ids.clear();
   visited_node_ids.reserve(10); // Typical tree depth

   declarations.clear();
   declarations.reserve(8); // Typical namespace count

   // Add default xml namespace
   declarations.push_back({"xml", "http://www.w3.org/XML/1998/namespace"});

   XTag *current = Node;
   while (current) {
      // Check if we've already processed this node to avoid cycles
      bool already_visited = false;
      for (int visited_id : visited_node_ids) {
         if (visited_id IS current->ID) {
            already_visited = true;
            break;
         }
      }

      if (!already_visited) {
         visited_node_ids.push_back(current->ID);

         // Scan attributes for namespace declarations
         for (size_t index = 1; index < current->Attribs.size(); ++index) {
            const auto &attrib = current->Attribs[index];

            if (attrib.Name.rfind("xmlns", 0) != 0) continue;

            std::string prefix;
            if (attrib.Name.length() IS 5) {
               prefix.clear(); // Default namespace
            }
            else if ((attrib.Name.length() > 6) and (attrib.Name[5] IS ':')) {
               prefix = attrib.Name.substr(6);
            }
            else continue;

            // Check if prefix already exists (inner scopes override outer)
            bool found_existing = false;
            for (const auto &existing : declarations) {
               if (existing.prefix IS prefix) {
                  found_existing = true;
                  break;
               }
            }

            if (!found_existing) {
               declarations.push_back({std::move(prefix), attrib.Value});
            }
         }
      }

      if (!current->ParentID) break;
      current = find_parent(current);
   }

   // Sort declarations by prefix for consistent ordering and deduplication
   std::sort(declarations.begin(), declarations.end());

   // Remove any duplicates (shouldn't happen but ensures correctness)
   auto new_end = std::unique(declarations.begin(), declarations.end(),
      [](const NamespaceDeclaration &a, const NamespaceDeclaration &b) {
         // Lambda ensures that namespaces with the same prefix collapse to a single declaration.
         return a.prefix IS b.prefix;
      });
   declarations.erase(new_end, declarations.end());
}

//********************************************************************************************************************
// Acquire a namespace node from the pool or create a new one

XTag * AxisEvaluator::acquire_namespace_node()
{
   if (!namespace_node_pool.empty()) {
      auto node = std::move(namespace_node_pool.back());
      namespace_node_pool.pop_back();

      // Reset the node for reuse
      node->Attribs.clear();
      node->Children.clear();

      XTag *raw_ptr = node.get();
      namespace_node_storage.push_back(std::move(node));
      return raw_ptr;
   }

   // Create new node if pool is empty
   auto node = std::make_unique<XTag>(0);
   XTag *raw_ptr = node.get();
   namespace_node_storage.push_back(std::move(node));
   return raw_ptr;
}

//********************************************************************************************************************
// Recycle namespace nodes back to the pool for reuse

void AxisEvaluator::recycle_namespace_nodes()
{
   // Move nodes from storage back to pool for reuse
   for (auto &node : namespace_node_storage) {
      if (node) {
         node->Attribs.clear();
         node->Children.clear();
         namespace_node_pool.push_back(std::move(node));
      }
   }
   namespace_node_storage.clear();
}

//********************************************************************************************************************
// Preceding axis mirrors the following axis but in reverse.

void AxisEvaluator::evaluate_preceding_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   auto &preceding_siblings = arena.acquire_node_vector([&]() -> size_t {
      auto *p = find_parent(Node);
      return p ? p->Children.size() : 0;
   }());
   evaluate_preceding_sibling_axis(Node, preceding_siblings);
   for (auto *sibling : preceding_siblings) {
      collect_subtree_reverse(sibling, Output);
   }
   arena.release_node_vector(preceding_siblings);

   if (auto *parent = find_parent(Node)) {
      auto &parent_preceding = arena.acquire_node_vector(estimate_result_size(AxisType::PRECEDING, parent));
      evaluate_preceding_axis(parent, parent_preceding);
      Output.insert(Output.end(), parent_preceding.begin(), parent_preceding.end());
      arena.release_node_vector(parent_preceding);
   }
}

//********************************************************************************************************************
// Namespace axis is modelled with transient nodes that expose in-scope prefix mappings.
// Optimized version using flat vector approach and node pooling.

void AxisEvaluator::evaluate_namespace_axis(XTag *Node, NODES &Output) {
   Output.clear();

   if (!Node) return;

   collect_namespace_declarations(Node, namespace_declarations);

   Output.reserve(namespace_declarations.size());

   auto emit_namespace = [&](const std::string &Prefix, const std::string &URI) {
      // Lambda creates a transient namespace node for the specified prefix mapping.
      XTag *node = acquire_namespace_node();

      node->Attribs.emplace_back(Prefix, std::string());

      XTag content_node(0);
      content_node.Attribs.emplace_back(std::string(), URI);
      node->Children.push_back(std::move(content_node));

      node->NamespaceID = xml ? xml->registerNamespace(URI) : 0;

      Output.push_back(node);
   };

   for (const auto &declaration : namespace_declarations) {
      emit_namespace(declaration.prefix, declaration.uri);
   }
}

//********************************************************************************************************************
// Self axis returns the context node itself.

void AxisEvaluator::evaluate_self_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (Node) Output.push_back(Node);
}

//********************************************************************************************************************
// Combine self and descendant traversal for the descendant-or-self axis.

void AxisEvaluator::evaluate_descendant_or_self_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   Output.push_back(Node);

   auto &descendants = arena.acquire_node_vector(Node->Children.size() * 4);
   evaluate_descendant_axis(Node, descendants);
   Output.insert(Output.end(), descendants.begin(), descendants.end());
   arena.release_node_vector(descendants);
}

//********************************************************************************************************************
// Combine self and ancestor traversal for the ancestor-or-self axis.

void AxisEvaluator::evaluate_ancestor_or_self_axis(XTag *Node, NODES &Output)
{
   Output.clear();
   if (!Node) return;

   Output.push_back(Node);

   auto &ancestors = arena.acquire_node_vector();
   evaluate_ancestor_axis(Node, ancestors);
   Output.insert(Output.end(), ancestors.begin(), ancestors.end());
   arena.release_node_vector(ancestors);
}

//********************************************************************************************************************
// Stable ordering is critical for XPath equality semantics; this method enforces document order.

void AxisEvaluator::sort_document_order(NODES &Nodes)
{
   if (Nodes.size() < 2) return;

   std::sort(Nodes.begin(), Nodes.end(), [this](XTag *Left, XTag *Right) {
      // Ordering lambda delegates to the document order helper so XPath stability is preserved.
      if (Left IS Right) return false;
      if (!Left) return false;
      if (!Right) return true;
      return is_before_in_document_order(Left, Right);
   });
}

//********************************************************************************************************************
// Construct the chain of ancestors from the root to the specified node.  The resulting path enables
// relative ordering checks for arbitrarily distant nodes.

AxisEvaluator::AncestorPathView AxisEvaluator::build_ancestor_path(XTag *Node)
{
   if (!Node) return {};

   bool cacheable = !(Node->ID IS 0);

   if (cacheable) {
      auto cached = ancestor_path_cache.find(Node);
      if (!(cached IS ancestor_path_cache.end())) {
         return { std::span<XTag *const>(*cached->second), cached->second, true };
      }
   }

   NODES *storage = nullptr;

   if (cacheable) {
      ancestor_path_storage.push_back(std::make_unique<NODES>());
      storage = ancestor_path_storage.back().get();
   }
   else storage = &arena.acquire_node_vector(10);

   storage->clear();

   XTag *current = Node;
   while (current) {
      storage->push_back(current);
      if (!current->ParentID) break;
      current = find_parent(current);
   }

   std::reverse(storage->begin(), storage->end());
   if (cacheable) ancestor_path_cache[Node] = storage;
   return { std::span<XTag *const>(*storage), storage, cacheable };
}

//********************************************************************************************************************
// Release storage acquired for an ancestor path view, returning pooled vectors when appropriate.

void AxisEvaluator::release_ancestor_path(AncestorPathView &View)
{
   if (!View.Storage) return;

   if (!View.Cached) arena.release_node_vector(*View.Storage);

   View.Storage = nullptr;
   View.Path = {};
   View.Cached = false;
}

//********************************************************************************************************************
// Generate a deterministic cache key for the relative document order of two nodes.

uint64_t AxisEvaluator::make_document_order_key(XTag *Left, XTag *Right)
{
   if ((!Left) or (!Right)) return 0;

   auto encode_node = [](XTag *Node) -> uint32_t {
      // Lambda compresses a node pointer or identifier into a 32-bit hash for cache storage.
      if (!Node) return 0;
      if (!(Node->ID IS 0)) return uint32_t(Node->ID);

      uintptr_t pointer_value = uintptr_t(Node);
      std::string_view pointer_bytes((char *)&pointer_value, sizeof(pointer_value));
      return pf::strhash(pointer_bytes);
   };

   uint32_t left_key = encode_node(Left);
   uint32_t right_key = encode_node(Right);

   return (uint64_t(left_key) << 32) | uint64_t(right_key);
}

//********************************************************************************************************************
// Evaluate whether Node1 precedes Node2 in document order, handling synthetic nodes gracefully.

bool AxisEvaluator::is_before_in_document_order(XTag *Node1, XTag *Node2) {
   if ((!Node1) or (!Node2) or (Node1 IS Node2)) return false;

   if ((Node1->ID IS 0) or (Node2->ID IS 0)) {
      if (Node1->ID IS Node2->ID) return Node1 < Node2;
      return Node1->ID < Node2->ID;
   }

   uint64_t cache_key = make_document_order_key(Node1, Node2);
   auto cached = document_order_cache.find(cache_key);
   if (!(cached IS document_order_cache.end())) return cached->second;

   auto path1_view = build_ancestor_path(Node1);
   auto path2_view = build_ancestor_path(Node2);

   auto path1 = path1_view.Path;
   auto path2 = path2_view.Path;

   bool result = false;

   if (path1.empty() or path2.empty()) {
      result = Node1 < Node2;
   } else {
      size_t max_common = std::min(path1.size(), path2.size());
      size_t index = 0;

      while ((index < max_common) and (path1[index] IS path2[index])) index++;

      if (index IS max_common) {
         result = path1.size() < path2.size();
      }
      else if (index IS 0) {
         result = path1[index]->ID < path2[index]->ID;
      }
      else {
         XTag *parent = path1[index - 1];
         XTag *branch1 = path1[index];
         XTag *branch2 = path2[index];

         bool resolved = false;
         for (auto &child : parent->Children) {
            if (&child IS branch1) {
               result = true;
               resolved = true;
               break;
            }
            else if (&child IS branch2) {
               result = false;
               resolved = true;
               break;
            }
         }

         if (!resolved) result = branch1->ID < branch2->ID;
      }
   }

   release_ancestor_path(path1_view);
   release_ancestor_path(path2_view);

   document_order_cache[cache_key] = result;
   document_order_cache[make_document_order_key(Node2, Node1)] = !result;

   return result;
}

//********************************************************************************************************************
// Remove null entries, enforce document order, and deduplicate the node-set to satisfy XPath rules.

void AxisEvaluator::normalise_node_set(NODES &Nodes)
{
   // Remove placeholder entries so that ordering and deduplication operate on valid nodes only.
   auto new_end = std::remove_if(Nodes.begin(), Nodes.end(), [](XTag *Node) {
      return !Node;
   });
   Nodes.erase(new_end, Nodes.end());

   if (Nodes.size() < 2) return;

   sort_document_order(Nodes);

   new_end = std::unique(Nodes.begin(), Nodes.end(), [](XTag *Left, XTag *Right) {
      // Treat identical pointers as duplicates so the resulting node-set is canonical.
      return Left IS Right;
   });

   Nodes.erase(new_end, Nodes.end());
}
