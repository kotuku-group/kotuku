// XPath FLWOR Expression Evaluation
//
// FLWOR (For, Let, Where, Order by, Return) expressions provide powerful iteration and transformation
// capabilities in XPath 2.0. This translation unit implements the complete evaluation pipeline for FLWOR
// clauses, including variable binding, filtering, grouping, sorting, and final result construction.
//
// The evaluation strategy uses a tuple-based approach where each tuple represents a binding context
// containing variable assignments and positional information. Clauses are applied sequentially, with
// each clause potentially expanding or filtering the tuple stream. The implementation maintains precise
// control over variable scoping, context node position tracking, and document order semantics to ensure
// correct XPath semantics for complex expressions.
//
// Key responsibilities:
//   - For bindings: iterate sequences and create tuple expansions
//   - Let bindings: introduce immutable variable assignments
//   - Where clauses: filter tuples based on predicate expressions
//   - Group by clauses: partition tuples into groups with aggregate bindings
//   - Order by clauses: sort tuples with collation and empty-value handling
//   - Count clauses: assign position counters to tuples
//   - Return expressions: evaluate results for each tuple and combine into final node-set

static size_t hash_xpath_group_value(const XPathVal &Value);

// Combines two hash values into a single hash using a common mixing technique.
// 0x9e3779b97f4a7c15ull is the 64-bit golden ratio constant commonly used to decorrelate values when mixing hashes.
// Incorporating it here improves the distribution of combined group hashes.

inline size_t combine_group_hash(size_t Seed, size_t Value)
{
   Seed ^= Value + 0x9e3779b97f4a7c15ull + (Seed << 6) + (Seed >> 2);
   return Seed;
}

// Indexed tuple schema maps variable names to stable IDs
struct TupleSchema {
   ankerl::unordered_dense::map<std::string, size_t> variable_indices;
   std::vector<std::string> variable_names;

   size_t register_variable(const std::string &name) {
      auto it = variable_indices.find(name);
      if (it != variable_indices.end()) return it->second;
      size_t id = variable_names.size();
      variable_indices.emplace(name, id);
      variable_names.push_back(name);
      return id;
   }

   size_t get_index(const std::string &name) const {
      auto it = variable_indices.find(name);
      return it != variable_indices.end() ? it->second : size_t(-1);
   }

   size_t size() const { return variable_names.size(); }
};

struct FlworTuple {
   std::vector<XPathVal> bindings;           // Indexed by TupleSchema
   std::vector<uint8_t> binding_set;         // Flags for presence
   XTag *context_node = nullptr;
   const XMLAttrib *context_attribute = nullptr;
   size_t context_position = 1;
   size_t context_size = 1;
   std::vector<XPathVal> order_keys;
   std::vector<bool> order_key_empty;
   size_t original_index = 0;

   inline void set_binding(size_t var_id, XPathVal value) {
      if (var_id == size_t(-1)) return;
      if (var_id >= bindings.size()) {
         bindings.resize(var_id + 1);
         binding_set.resize(var_id + 1, 0);
      }
      bindings[var_id] = std::move(value);
      binding_set[var_id] = 1;
   }

   [[nodiscard]] inline bool has_binding(size_t var_id) const {
      return (var_id < binding_set.size()) and (binding_set[var_id] != 0);
   }
};

struct TupleScope {
   XPathEvaluator & evaluator;
   XPathContext & context_ref;
   std::vector<VariableBindingGuard> guards;
   const TupleSchema & schema;

   TupleScope(XPathEvaluator &Evaluator, XPathContext &ContextRef, const TupleSchema &Schema, const FlworTuple &Tuple)
      : evaluator(Evaluator), context_ref(ContextRef), schema(Schema)
   {
      evaluator.push_context(Tuple.context_node, Tuple.context_position, Tuple.context_size, Tuple.context_attribute);
      guards.reserve(schema.size());
      for (size_t var_id = 0; var_id < schema.size(); ++var_id) {
         if (Tuple.has_binding(var_id)) guards.emplace_back(context_ref, schema.variable_names[var_id], Tuple.bindings[var_id]);
      }
   }

   ~TupleScope() {
      evaluator.pop_context();
   }
};

struct GroupKey {
   pf::vector<XPathVal> values;
};

struct GroupKeyHasher {
   size_t operator()(const GroupKey &Key) const noexcept {
      size_t seed = Key.values.size();
      for (const auto &value : Key.values) seed = combine_group_hash(seed, hash_xpath_group_value(value));
      return seed;
   }
};

struct GroupKeyEqual {
   bool operator()(const GroupKey &Left, const GroupKey &Right) const noexcept {
      if (Left.values.size() != Right.values.size()) return false;
      for (size_t index = 0; index < Left.values.size(); ++index) {
         if (!compare_xpath_values(Left.values[index], Right.values[index])) return false;
      }
      return true;
   }
};

//*********************************************************************************************************************
// Computes a stable hash for a group key value, suitable for use in unordered_map/set.

static size_t group_nodeset_length(const XPathVal &Value)
{
   size_t length = Value.node_set.size();
   if (length < Value.node_set_attributes.size()) length = Value.node_set_attributes.size();
   if (length < Value.node_set_string_values.size()) length = Value.node_set_string_values.size();
   if ((length IS 0) and Value.node_set_string_override.has_value()) length = 1;
   return length;
}

//*********************************************************************************************************************
// Retrieves the string value for a node in a group key value at the specified index.

static std::string group_nodeset_string(const XPathVal &Value, size_t Index)
{
   if (Index < Value.node_set_string_values.size()) return Value.node_set_string_values[Index];

   bool use_override = Value.node_set_string_override.has_value() and Value.node_set_string_values.empty() and (Index IS 0);
   if (use_override) return *Value.node_set_string_override;

   if (Index < Value.node_set_attributes.size()) {
      const XMLAttrib *attribute = Value.node_set_attributes[Index];
      if (attribute) return attribute->Value;
   }

   if (Index < Value.node_set.size()) {
      XTag *node = Value.node_set[Index];
      if (node) return XPathVal::node_string_value(node);
   }

   return std::string();
}

//*********************************************************************************************************************
// Computes a stable hash for an XPath value, suitable for use in unordered_map/set.

static size_t hash_xpath_group_value(const XPathVal &Value)
{
   size_t seed = size_t(Value.Type);

   switch (Value.Type) {
      case XPVT::Boolean:
      case XPVT::Number: {
         double number = Value.to_number();
         if (std::isnan(number)) return combine_group_hash(seed, 0x7ff8000000000000ull);
         size_t hashed = std::hash<double>{}(number);
         return combine_group_hash(seed, hashed);
      }

      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime: {
         std::string string_value = Value.to_string();
         size_t hashed = std::hash<std::string>{}(string_value);
         return combine_group_hash(seed, hashed);
      }

      case XPVT::NodeSet: {
         size_t combined = seed;
         size_t length = group_nodeset_length(Value);
         for (size_t index = 0; index < length; ++index) {
            std::string string_value = group_nodeset_string(Value, index);
            size_t hashed = std::hash<std::string>{}(string_value);
            combined = combine_group_hash(combined, hashed);
         }

         return combined;
      }

      case XPVT::Map:
      case XPVT::Array: {
         std::string summary = Value.to_string();
         size_t hashed = std::hash<std::string>{}(summary);
         return combine_group_hash(seed, hashed);
      }
   }

   return seed;
}

//********************************************************************************************************************

XPathVal XPathEvaluator::evaluate_flwor_pipeline(const XPathNode *Node, uint32_t CurrentPrefix)
{
   pf::Log log("eval_flwor");

   if (!Node) {
      record_error("FLWOR expression is missing its AST node.", Node, true);
      return XPathVal();
   }

   if (Node->child_count() < 2) {
      record_error("FLWOR expression requires at least one clause and a return expression.", Node, true);
      return XPathVal();
   }

   const XPathNode *return_node = Node->get_child(Node->child_count() - 1);
   if (!return_node) {
      record_error("FLWOR expression is missing its return clause.", Node, true);
      return XPathVal();
   }

   bool tracing_flwor = is_trace_enabled();

   auto nodeset_length = [](const XPathVal &value) -> size_t
   {
      size_t length = value.node_set.size();
      if (length < value.node_set_attributes.size()) length = value.node_set_attributes.size();
      if (length < value.node_set_string_values.size()) length = value.node_set_string_values.size();
      if ((length IS 0) and value.node_set_string_override.has_value()) length = 1;
      return length;
   };

   auto nodeset_string_at = [](const XPathVal &value, size_t index) -> std::string {
      if (index < value.node_set_string_values.size()) return value.node_set_string_values[index];

      bool use_override = value.node_set_string_override.has_value() and value.node_set_string_values.empty() and (index IS 0);
      if (use_override) return *value.node_set_string_override;

      if (index < value.node_set_attributes.size()) {
         const XMLAttrib *attribute = value.node_set_attributes[index];
         if (attribute) return attribute->Value;
      }

      if (index < value.node_set.size()) {
         XTag *node = value.node_set[index];
         if (node) return XPathVal::node_string_value(node);
      }

      return std::string();
   };

   // Note these log messages will only be compiled in debug builds.  You can include them temporarily in a release build
   // by switching to pf::Log.msg(VLF::TRACE, ...) in the lines below.

   auto trace_detail = [&](const char *Format, auto ...Args) {
      if (tracing_flwor) pf::Log("XPath").trace(Format, Args...);
   };

   auto trace_verbose = [&](const char *Format, auto ...Args) {
      if (tracing_flwor) pf::Log("XPath").trace(Format, Args...);
   };

   auto describe_value_for_trace = [&](const XPathVal &value) -> std::string {
      switch (value.Type) {
         case XPVT::Boolean:
            return value.to_boolean() ? std::string("true") : std::string("false");

         case XPVT::Number:
            return value.to_string();

         case XPVT::String:
         case XPVT::Date:
         case XPVT::Time:
         case XPVT::DateTime:
            return value.to_string();

         case XPVT::NodeSet: {
            size_t length = nodeset_length(value);
            std::string preview;
            if (length > 0) preview = nodeset_string_at(value, 0);
            if (preview.empty()) return std::format("node-set[{}]", length);
            else return std::format("node-set[{}]: {}", length, preview);
         }

         default:
            break;
      }

      return value.to_string();
   };

   auto describe_tuple_bindings = [&](const TupleSchema &schema, const FlworTuple &tuple) -> std::string {
      if (schema.size() IS 0) return std::string();

      std::vector<std::string> entries;
      entries.reserve(schema.size());

      for (size_t var_id = 0; var_id < schema.size(); ++var_id) {
         if (!tuple.has_binding(var_id)) continue;
         entries.push_back(std::format("{}={}", schema.variable_names[var_id], describe_value_for_trace(tuple.bindings[var_id])));
      }

      if (entries.empty()) return std::string();
      std::sort(entries.begin(), entries.end());
      std::string summary = entries[0];
      for (size_t index = 1; index < entries.size(); ++index) { summary.append(", "); summary.append(entries[index]); }
      return summary;
   };

   auto describe_value_sequence = [&](const auto &values) -> std::string {
      if (values.empty()) return std::string();

      std::string summary = describe_value_for_trace(values[0]);
      for (size_t index = 1; index < values.size(); ++index) {
         summary.append(" | ");
         summary.append(describe_value_for_trace(values[index]));
      }

      return summary;
   };

   auto ensure_nodeset_binding = [&](XPathVal &value) {
      if (value.Type IS XPVT::NodeSet) return;

      bool has_existing = !value.is_empty();
      std::string preserved_string;
      if (has_existing) preserved_string = value.to_string();

      value.Type = XPVT::NodeSet;
      value.NumberValue = 0.0;
      value.StringValue.clear();
      value.node_set.clear();
      value.node_set_attributes.clear();
      value.node_set_string_values.clear();
      value.node_set_string_override.reset();
      value.preserve_node_order = false;
      value.schema_type_info.reset();
      value.schema_validated = false;

      if (has_existing) {
         value.node_set.push_back(nullptr);
         value.node_set_attributes.push_back(nullptr);
         value.node_set_string_values.push_back(std::move(preserved_string));
         value.node_set_string_override = value.node_set_string_values.back();
      }
   };

   auto append_binding_value = [&](XPathVal &target_nodeset, const XPathVal &source_value) {
      target_nodeset.preserve_node_order = false;
      if (source_value.Type IS XPVT::NodeSet) {
         size_t length = nodeset_length(source_value);
         for (size_t value_index = 0; value_index < length; ++value_index) {
            XTag *node = value_index < source_value.node_set.size() ? source_value.node_set[value_index] : nullptr;
            target_nodeset.node_set.push_back(node);

            const XMLAttrib *attribute = nullptr;
            if (value_index < source_value.node_set_attributes.size()) attribute = source_value.node_set_attributes[value_index];
            target_nodeset.node_set_attributes.push_back(attribute);

            std::string node_string = nodeset_string_at(source_value, value_index);
            target_nodeset.node_set_string_values.push_back(std::move(node_string));
         }

         if (!target_nodeset.node_set_string_values.empty()) target_nodeset.node_set_string_override.reset();
         return;
      }

      if (source_value.is_empty()) return;

      std::string atomic_string = source_value.to_string();
      target_nodeset.node_set.push_back(nullptr);
      target_nodeset.node_set_attributes.push_back(nullptr);
      target_nodeset.node_set_string_values.push_back(std::move(atomic_string));

      if (!target_nodeset.node_set_string_values.empty()) target_nodeset.node_set_string_override.reset();
   };

   auto merge_binding_values = [&](XPathVal &target, const XPathVal &source) {
      ensure_nodeset_binding(target);
      append_binding_value(target, source);
   };

   auto merge_binding_vectors = [&](FlworTuple &target_tuple, const FlworTuple &source_tuple) {
      size_t max_size = source_tuple.bindings.size();
      if (max_size > target_tuple.bindings.size()) {
         target_tuple.bindings.resize(max_size);
         target_tuple.binding_set.resize(max_size, 0);
      }
      for (size_t var_id = 0; var_id < max_size; ++var_id) {
         if (!source_tuple.has_binding(var_id)) continue;
         if (!target_tuple.has_binding(var_id)) {
            target_tuple.bindings[var_id] = source_tuple.bindings[var_id];
            target_tuple.binding_set[var_id] = 1;
         }
         else {
            merge_binding_values(target_tuple.bindings[var_id], source_tuple.bindings[var_id]);
         }
      }

      if (target_tuple.original_index > source_tuple.original_index) {
         target_tuple.original_index = source_tuple.original_index;
      }
   };

   std::vector<const XPathNode *> binding_nodes;
   binding_nodes.reserve(Node->child_count());

   const XPathNode *where_clause = nullptr;
   const XPathNode *group_clause = nullptr;
   const XPathNode *order_clause = nullptr;
   const XPathNode *count_clause = nullptr;

   for (size_t index = 0; index + 1 < Node->child_count(); ++index) {
      const XPathNode *child = Node->get_child(index);
      if (!child) {
         record_error("FLWOR expression contains an invalid clause.", Node, true);
         return XPathVal();
      }
      else if ((child->type IS XQueryNodeType::FOR_BINDING) or (child->type IS XQueryNodeType::LET_BINDING)) {
         binding_nodes.push_back(child);
      }
      else if (child->type IS XQueryNodeType::WHERE_CLAUSE) where_clause = child;
      else if (child->type IS XQueryNodeType::GROUP_CLAUSE) group_clause = child;
      else if (child->type IS XQueryNodeType::ORDER_CLAUSE) order_clause = child;
      else if (child->type IS XQueryNodeType::COUNT_CLAUSE) count_clause = child;
      else {
         record_error("FLWOR expression contains an unsupported clause type.", child, true);
         return XPathVal();
      }
   }

   if (binding_nodes.empty()) {
      record_error("FLWOR expression is missing binding clauses.", Node, true);
      return XPathVal();
   }

   TupleSchema schema;
   // Pre-register variables from FOR/LET bindings
   for (const auto *bn : binding_nodes) if (bn && !bn->value.empty()) schema.register_variable(bn->value);

   std::vector<FlworTuple> tuples;
   tuples.reserve(8);

   FlworTuple initial_tuple;
   initial_tuple.context_node = context.context_node;
   initial_tuple.context_attribute = context.attribute_node;
   initial_tuple.context_position = context.position;
   initial_tuple.context_size = context.size;
   initial_tuple.original_index = 0;
   tuples.push_back(std::move(initial_tuple));

   for (const XPathNode *binding_node : binding_nodes) {
      if (!binding_node) continue;

      if (binding_node->type IS XQueryNodeType::LET_BINDING) {
         if ((binding_node->value.empty()) or (binding_node->child_count() IS 0)) {
            record_error("Let binding requires a variable name and expression.", binding_node, true);
            return XPathVal();
         }

         const XPathNode *binding_expr = binding_node->get_child(0);
         if (!binding_expr) {
            record_error("Let binding requires an expression node.", binding_node, true);
            return XPathVal();
         }

         std::vector<FlworTuple> next_tuples;
         next_tuples.reserve(tuples.size());

         for (const auto &tuple : tuples) {
            TupleScope scope(*this, context, schema, tuple);
            XPathVal bound_value = evaluate_expression(binding_expr, CurrentPrefix);
            if (expression_unsupported) {
               record_error("Let binding expression could not be evaluated.", binding_expr);
               return XPathVal();
            }

            FlworTuple updated_tuple = tuple;
            size_t var_id = schema.register_variable(binding_node->value);
            updated_tuple.set_binding(var_id, std::move(bound_value));
            next_tuples.push_back(std::move(updated_tuple));
         }

         tuples = std::move(next_tuples);
         for (size_t tuple_index = 0; tuple_index < tuples.size(); ++tuple_index) {
            tuples[tuple_index].original_index = tuple_index;
         }
         continue;
      }

      if (binding_node->type IS XQueryNodeType::FOR_BINDING) {
         if ((binding_node->value.empty()) or (binding_node->child_count() IS 0)) {
            record_error("For binding requires a variable name and sequence.", binding_node, true);
            return XPathVal();
         }

         const XPathNode *sequence_expr = binding_node->get_child(0);
         if (!sequence_expr) {
            record_error("For binding requires a sequence expression.", binding_node, true);
            return XPathVal();
         }

         std::vector<FlworTuple> next_tuples;
         next_tuples.reserve(tuples.size());

         for (const auto &tuple : tuples) {
            TupleScope scope(*this, context, schema, tuple);
            XPathVal sequence_value = evaluate_expression(sequence_expr, CurrentPrefix);
            if (expression_unsupported) {
               record_error("For binding sequence could not be evaluated.", sequence_expr);
               return XPathVal();
            }

            if (sequence_value.Type != XPVT::NodeSet) {
               record_error("For binding sequences must evaluate to node-sets.", sequence_expr, true);
               return XPathVal();
            }

            size_t sequence_size = sequence_value.node_set.size();
            if (sequence_size IS 0) continue;

            // Pre-size the output expansion for this tuple to avoid repeated reallocations
            size_t required = next_tuples.size() + sequence_size;
            if (required > next_tuples.capacity()) next_tuples.reserve(required);

            bool use_override = sequence_value.node_set_string_override.has_value() and
               sequence_value.node_set_string_values.empty();

            for (size_t item_index = 0; item_index < sequence_size; ++item_index) {
               FlworTuple next_tuple = tuple;

               XTag *item_node = sequence_value.node_set[item_index];
               const XMLAttrib *item_attribute = nullptr;
               if (item_index < sequence_value.node_set_attributes.size()) {
                  item_attribute = sequence_value.node_set_attributes[item_index];
               }

               std::string item_string;
               if (item_index < sequence_value.node_set_string_values.size()) {
                  item_string = sequence_value.node_set_string_values[item_index];
               }
               else if (use_override) item_string = *sequence_value.node_set_string_override;
               else if (item_attribute) item_string = item_attribute->Value;
               else if (item_node) item_string = XPathVal::node_string_value(item_node);

               XPathVal bound_value = xpath_nodeset_singleton(item_node, item_attribute, item_string);
               size_t var_id = schema.register_variable(binding_node->value);
               next_tuple.set_binding(var_id, std::move(bound_value));
               next_tuple.context_node = item_node;
               next_tuple.context_attribute = item_attribute;
               next_tuple.context_position = item_index + 1;
               next_tuple.context_size = sequence_size;

               next_tuples.push_back(std::move(next_tuple));
            }
         }

         tuples = std::move(next_tuples);
         for (size_t tuple_index = 0; tuple_index < tuples.size(); ++tuple_index) {
            tuples[tuple_index].original_index = tuple_index;
         }
         continue;
      }

      record_error("FLWOR expression contains an unsupported binding clause.", binding_node, true);
      return XPathVal();
   }

   if (tuples.empty()) {
      NODES empty_nodes;
      return XPathVal(empty_nodes);
   }

   if (where_clause) {
      if (where_clause->child_count() IS 0) {
         record_error("Where clause requires a predicate expression.", where_clause, true);
         return XPathVal();
      }

      const XPathNode *predicate_node = where_clause->get_child(0);
      if (!predicate_node) {
         record_error("Where clause requires a predicate expression.", where_clause, true);
         return XPathVal();
      }

      std::vector<FlworTuple> filtered;
      filtered.reserve(tuples.size());

      for (const auto &tuple : tuples) {
         TupleScope scope(*this, context, schema, tuple);
         XPathVal predicate_value = evaluate_expression(predicate_node, CurrentPrefix);
         if (expression_unsupported) {
            record_error("Where clause expression could not be evaluated.", predicate_node);
            return XPathVal();
         }

         if (predicate_value.to_boolean()) filtered.push_back(tuple);
      }

      tuples = std::move(filtered);
      for (size_t tuple_index = 0; tuple_index < tuples.size(); ++tuple_index) {
         tuples[tuple_index].original_index = tuple_index;
      }

      if (tuples.empty()) {
         NODES empty_nodes;
         return XPathVal(empty_nodes);
      }
   }

   if (group_clause) {
      if (group_clause->child_count() IS 0) {
         record_error("Group clause requires at least one key definition.", group_clause, true);
         return XPathVal();
      }

      ankerl::unordered_dense::map<GroupKey, size_t, GroupKeyHasher, GroupKeyEqual> group_lookup;
      group_lookup.reserve(tuples.size());
      std::vector<FlworTuple> grouped;
      grouped.reserve(tuples.size());

      if (tracing_flwor) {
         trace_detail("FLWOR group-by: tuple-count=%d, key-count=%d", int(std::ssize(tuples)), int(group_clause->child_count()));
      }

      const auto tuples_size = int(std::ssize(tuples));
      for (int tuple_index = 0; tuple_index < tuples_size; ++tuple_index) {
         const FlworTuple &tuple = tuples[tuple_index];
         GroupKey key;
         key.values.reserve(group_clause->child_count());

         std::string tuple_binding_summary;
         if (tracing_flwor) tuple_binding_summary = describe_tuple_bindings(schema, tuple);

         {
            TupleScope scope(*this, context, schema, tuple);
            for (int key_index = 0; key_index < int(group_clause->child_count()); ++key_index) {
               const XPathNode *key_node = group_clause->get_child(key_index);
               if (!key_node) {
                  record_error("Group clause contains an invalid key.", group_clause, true);
                  return XPathVal();
               }

               const XPathNode *key_expr = key_node->get_child(0);
               if (!key_expr) {
                  record_error("Group key requires an expression.", key_node, true);
                  return XPathVal();
               }

               XPathVal key_value = evaluate_expression(key_expr, CurrentPrefix);
               if (expression_unsupported) {
                  record_error("Group key expression could not be evaluated.", key_expr);
                  return XPathVal();
               }

               key.values.push_back(std::move(key_value));

               if (tracing_flwor) {
                  const XPathVal &evaluated_value = key.values.back();
                  std::string value_summary = describe_value_for_trace(evaluated_value);
                  trace_verbose("FLWOR group key[%d,%d]: %s", tuple_index, key_index, value_summary.c_str());
               }
            }
         }

         std::string key_summary;
         if (tracing_flwor) key_summary = describe_value_sequence(key.values);

         auto lookup = group_lookup.find(key);
         if (lookup == group_lookup.end()) {
            size_t group_index = grouped.size();

            FlworTuple grouped_tuple = tuple;
            grouped_tuple.original_index = tuple.original_index;

            for (size_t key_index = 0; key_index < group_clause->child_count(); ++key_index) {
               const XPathNode *key_node = group_clause->get_child(key_index);
               if (!key_node) continue;
               const auto *info = key_node->get_group_key_info();
               if ((info) and info->has_variable()) {
                  size_t var_id = schema.register_variable(info->variable_name);
                  grouped_tuple.set_binding(var_id, key.values[key_index]);
               }
            }

            grouped.push_back(std::move(grouped_tuple));

            if (tracing_flwor) {
               if (tuple_binding_summary.empty()) {
                  trace_detail("FLWOR group create tuple[%d] -> group %zu, keys: %s", tuple_index, group_index,
                     key_summary.c_str());
               }
               else {
                  trace_detail("FLWOR group create tuple[%d] -> group %zu, keys: %s, bindings: %s", tuple_index,
                     group_index, key_summary.c_str(), tuple_binding_summary.c_str());
               }
            }

            group_lookup.emplace(std::move(key), group_index);
            continue;
         }

         FlworTuple &existing_group = grouped[lookup->second];
         merge_binding_vectors(existing_group, tuple);

         for (size_t key_index = 0; key_index < group_clause->child_count(); ++key_index) {
            const XPathNode *key_node = group_clause->get_child(key_index);
            if (!key_node) continue;
            const auto *info = key_node->get_group_key_info();
            if ((info) and info->has_variable()) {
               size_t var_id = schema.register_variable(info->variable_name);
               existing_group.set_binding(var_id, key.values[key_index]);
            }
         }

         if (tracing_flwor) {
            std::string merged_summary = describe_tuple_bindings(schema, existing_group);
            trace_detail("FLWOR group merge tuple[%zu] into group %zu, keys: %s", tuple_index, lookup->second,
               key_summary.c_str());
            if (!merged_summary.empty()) {
               trace_verbose("FLWOR group[%zu] bindings: %s", lookup->second, merged_summary.c_str());
            }
         }
      }

      tuples = std::move(grouped);

      if (tuples.empty()) {
         NODES empty_nodes;
         return XPathVal(empty_nodes);
      }
   }

   if (order_clause) {
      if (order_clause->child_count() IS 0) {
         record_error("Order by clause requires at least one sort specification.", order_clause, true);
         return XPathVal();
      }

      struct OrderSpecMetadata {
         const XPathNode *node = nullptr;
         XPathOrderSpecOptions options{};
         bool has_options = false;
         XPathOrderComparatorOptions comparator_options{};
      };

      std::vector<OrderSpecMetadata> order_specs;
      order_specs.reserve(order_clause->child_count());

      for (size_t spec_index = 0; spec_index < order_clause->child_count(); ++spec_index) {
         const XPathNode *spec_node = order_clause->get_child(spec_index);
         if (!spec_node) {
            record_error("Order by clause contains an invalid specification.", order_clause, true);
            return XPathVal();
         }

         OrderSpecMetadata metadata;
         metadata.node = spec_node;
         if (spec_node->has_order_spec_options()) {
            metadata.options = *spec_node->get_order_spec_options();
            metadata.has_options = true;
         }

         if (metadata.has_options and metadata.options.has_collation()) {
            const std::string &uri = metadata.options.collation_uri;
            if (!xpath_collation_supported(uri)) {
               record_error("FLWOR order by clause collation '" + uri + "' is not supported.", spec_node, true);
               return XPathVal();
            }
            metadata.comparator_options.has_collation = true;
            metadata.comparator_options.collation_uri = uri;
         }

         if (metadata.has_options) {
            metadata.comparator_options.descending = metadata.options.is_descending;
            metadata.comparator_options.has_empty_mode = metadata.options.has_empty_mode;
            metadata.comparator_options.empty_is_greatest = metadata.options.empty_is_greatest;
         }

         if (not metadata.comparator_options.has_empty_mode) {
            metadata.comparator_options.has_empty_mode = true;
            metadata.comparator_options.empty_is_greatest = prolog_empty_is_greatest();
         }

         order_specs.push_back(metadata);
      }

      if (tracing_flwor) {
         trace_detail("FLWOR order-by: tuple-count=%d, spec-count=%d", int(std::ssize(tuples)), int(std::ssize(order_specs)));
         for (int spec_index = 0; spec_index < std::ssize(order_specs); ++spec_index) {
            const auto &spec = order_specs[spec_index];
            const XPathNode *spec_expr = spec.node ? spec.node->get_child(0) : nullptr;
            std::string expression_signature;
            if (spec_expr) expression_signature = build_ast_signature(spec_expr);
            else expression_signature = std::string("<missing>");

            std::string collation = spec.comparator_options.has_collation ? spec.comparator_options.collation_uri :
               std::string("(default)");
            const char *direction = spec.comparator_options.descending ? "descending" : "ascending";
            const char *empty_mode = "no-empty-order";
            if (spec.comparator_options.has_empty_mode) {
               empty_mode = spec.comparator_options.empty_is_greatest ? "empty-greatest" : "empty-least";
            }

            trace_detail("FLWOR order spec[%d]: expr=%s, collation=%s, direction=%s, empty=%s", spec_index,
               expression_signature.c_str(), collation.c_str(), direction, empty_mode);
         }
      }

      for (size_t tuple_index = 0; tuple_index < tuples.size(); ++tuple_index) {
         FlworTuple &tuple = tuples[tuple_index];
         tuple.order_keys.clear();
         tuple.order_key_empty.clear();
         tuple.order_keys.reserve(order_specs.size());
         tuple.order_key_empty.reserve(order_specs.size());

         if (tracing_flwor) {
            std::string binding_summary = describe_tuple_bindings(schema, tuple);
            if (binding_summary.empty()) {
               trace_detail("FLWOR order tuple[%zu] original=%zu has no bindings", tuple_index, tuple.original_index);
            }
            else {
               trace_detail("FLWOR order tuple[%zu] original=%zu bindings: %s", tuple_index, tuple.original_index,
                  binding_summary.c_str());
            }
         }

         TupleScope scope(*this, context, schema, tuple);
         for (size_t spec_index = 0; spec_index < order_specs.size(); ++spec_index) {
            const auto &spec = order_specs[spec_index];
            const XPathNode *spec_expr = spec.node ? spec.node->get_child(0) : nullptr;
            if (!spec_expr) {
               record_error("Order by clause requires an expression.", spec.node ? spec.node : order_clause, true);
               return XPathVal();
            }

            XPathVal key_value = evaluate_expression(spec_expr, CurrentPrefix);
            if (expression_unsupported) {
               record_error("Order by expression could not be evaluated.", spec_expr);
               return XPathVal();
            }

            bool is_empty_key = xpath_order_key_is_empty(key_value);

            tuple.order_keys.push_back(std::move(key_value));
            tuple.order_key_empty.push_back(is_empty_key);

            if (tracing_flwor) {
               const XPathVal &stored_value = tuple.order_keys.back();
               std::string value_summary = describe_value_for_trace(stored_value);
               trace_verbose("FLWOR order key[%zu,%zu]: %s%s", tuple_index, spec_index,
                  value_summary.c_str(), is_empty_key ? " (empty)" : "");
            }
         }

         if (tracing_flwor) {
            std::string key_summary = describe_value_sequence(tuple.order_keys);
            trace_detail("FLWOR order tuple[%zu] generated %zu key(s): %s", tuple_index, tuple.order_keys.size(),
               key_summary.c_str());
         }
      }

      auto comparator = [&](const FlworTuple &lhs, const FlworTuple &rhs) -> bool
      {
         for (size_t spec_index = 0; spec_index < order_specs.size(); ++spec_index) {
            const auto &spec = order_specs[spec_index];
            const XPathVal *left_ptr = spec_index < lhs.order_keys.size() ? &lhs.order_keys[spec_index] : nullptr;
            const XPathVal *right_ptr = spec_index < rhs.order_keys.size() ? &rhs.order_keys[spec_index] : nullptr;

            XPathVal left_placeholder;
            XPathVal right_placeholder;

            const XPathVal &left_value = left_ptr ? *left_ptr : left_placeholder;
            const XPathVal &right_value = right_ptr ? *right_ptr : right_placeholder;

            bool left_empty = left_ptr ? (spec_index < lhs.order_key_empty.size() ? lhs.order_key_empty[spec_index] :
               xpath_order_key_is_empty(left_value)) : true;
            bool right_empty = right_ptr ? (spec_index < rhs.order_key_empty.size() ? rhs.order_key_empty[spec_index] :
               xpath_order_key_is_empty(right_value)) : true;

            int comparison = xpath_compare_order_keys(left_value, left_empty, right_value, right_empty,
               spec.comparator_options);
            if (comparison != 0) return comparison < 0;
         }

         return lhs.original_index < rhs.original_index;
      };

      if (order_clause->order_clause_is_stable) std::stable_sort(tuples.begin(), tuples.end(), comparator);
      else std::sort(tuples.begin(), tuples.end(), comparator);

      if (tracing_flwor) {
         std::string index_summary;
         if (!tuples.empty()) {
            index_summary = std::to_string(tuples[0].original_index);
            for (size_t tuple_index = 1; tuple_index < tuples.size(); ++tuple_index) {
               index_summary.append(", ");
               index_summary.append(std::to_string(tuples[tuple_index].original_index));
            }
         }

         const char *sort_mode = order_clause->order_clause_is_stable ? "stable" : "unstable";
         trace_detail("FLWOR order-by sorted (%s), original indices: %s", sort_mode, index_summary.c_str());
      }
   }

   if (count_clause) {
      if (count_clause->value.empty()) {
         record_error("Count clause requires a variable name.", count_clause, true);
         return XPathVal();
      }

      if (tracing_flwor) {
         trace_detail("FLWOR count clause applying to %zu sorted tuple(s)", tuples.size());
      }

      for (size_t tuple_index = 0; tuple_index < tuples.size(); ++tuple_index) {
         XPathVal counter(double(tuple_index + 1));
         size_t var_id = schema.register_variable(count_clause->value);
         tuples[tuple_index].set_binding(var_id, std::move(counter));

         if (tracing_flwor) {
            trace_verbose("FLWOR count tuple[%zu] original=%zu -> %zu", tuple_index,
               tuples[tuple_index].original_index, tuple_index + 1);
         }
      }
   }

   size_t tuple_count = tuples.size();
   for (size_t tuple_index = 0; tuple_index < tuple_count; ++tuple_index) {
      tuples[tuple_index].context_position = tuple_index + 1;
      tuples[tuple_index].context_size = tuple_count;
   }

   NODES combined_nodes;
   std::vector<const XMLAttrib *> combined_attributes;
   std::vector<std::string> combined_strings;
   std::optional<std::string> combined_override;

   for (size_t tuple_index = 0; tuple_index < tuples.size(); ++tuple_index) {
      const auto &tuple = tuples[tuple_index];
      if (tracing_flwor) {
         std::string binding_summary = describe_tuple_bindings(schema, tuple);
         if (binding_summary.empty()) {
            trace_detail("FLWOR return tuple[%zu] original=%zu context=%zu/%zu evaluating", tuple_index,
               tuple.original_index, tuple.context_position, tuple.context_size);
         }
         else {
            trace_detail("FLWOR return tuple[%zu] original=%zu context=%zu/%zu bindings: %s", tuple_index,
               tuple.original_index, tuple.context_position, tuple.context_size, binding_summary.c_str());
         }
      }

      TupleScope scope(*this, context, schema, tuple);
      XPathVal iteration_value = evaluate_expression(return_node, CurrentPrefix);

      if (expression_unsupported) {
         if (tracing_flwor) {
            const char *error_msg = xml ? xml->ErrorMsg.c_str() : "<no-xml>";
            trace_detail("FLWOR return tuple[%zu] evaluation failed: %s", tuple_index, error_msg);
         }
         record_error("FLWOR return expression could not be evaluated.", return_node);
         if (xml and xml->ErrorMsg.empty()) xml->ErrorMsg.assign("FLWOR return expression could not be evaluated.");
         return XPathVal();
      }

      if (iteration_value.Type IS XPVT::NodeSet) {
         size_t length = nodeset_length(iteration_value);
         if (tracing_flwor) {
            trace_detail("FLWOR return tuple[%zu] produced node-set length=%zu", tuple_index, length);
            if (length > 0) {
               for (size_t value_index = 0; value_index < length; ++value_index) {
                  XTag *node = value_index < iteration_value.node_set.size() ? iteration_value.node_set[value_index] : nullptr;
                  int node_id = node ? node->ID : -1;
                  const XMLAttrib *attribute = value_index < iteration_value.node_set_attributes.size() ?
                     iteration_value.node_set_attributes[value_index] : nullptr;
                  const char *attribute_name = (attribute and !attribute->Name.empty()) ?
                     attribute->Name.c_str() : "<node>";
                  trace_verbose("FLWOR return tuple[%zu] value[%zu]: node-id=%d attribute=%s", tuple_index,
                     value_index, node_id, attribute_name);
               }
            }
            else {
               trace_verbose("FLWOR return tuple[%zu] produced empty node-set", tuple_index);
            }
         }
         if (length IS 0) continue;

         for (size_t value_index = 0; value_index < length; ++value_index) {
            XTag *node = value_index < iteration_value.node_set.size() ? iteration_value.node_set[value_index] : nullptr;
            combined_nodes.push_back(node);

            const XMLAttrib *attribute = nullptr;
            if (value_index < iteration_value.node_set_attributes.size()) {
               attribute = iteration_value.node_set_attributes[value_index];
            }
            combined_attributes.push_back(attribute);

            std::string node_string = nodeset_string_at(iteration_value, value_index);
            combined_strings.push_back(node_string);
            if (!combined_override.has_value()) combined_override = node_string;
         }

         if (iteration_value.node_set_string_override.has_value() and iteration_value.node_set_string_values.empty()) {
            if (!combined_override.has_value()) combined_override = iteration_value.node_set_string_override;
         }
         continue;
      }

      if (iteration_value.is_empty()) continue;

      // Wrap atomic values (strings, numbers, booleans) in a node-set
      if (tracing_flwor) trace_detail("FLWOR return tuple[%zu] produced atomic type %d, wrapping in node-set",
         tuple_index, int(iteration_value.Type));

      std::string atomic_string = iteration_value.to_string();
      combined_nodes.push_back(nullptr);
      combined_attributes.push_back(nullptr);
      combined_strings.push_back(std::move(atomic_string));
      if (!combined_override.has_value()) combined_override = combined_strings.back();
   }

   XPathVal result;
   result.Type = XPVT::NodeSet;
   result.node_set = std::move(combined_nodes);
   result.node_set_attributes = std::move(combined_attributes);
   result.node_set_string_values = std::move(combined_strings);
   if (combined_override.has_value()) result.node_set_string_override = combined_override;
   else result.node_set_string_override.reset();
   bool preserve_order = (order_clause != nullptr);
   if (not prolog_ordering_is_ordered()) preserve_order = true;
   result.preserve_node_order = preserve_order;
   return result;
}
