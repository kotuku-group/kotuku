// XPath Evaluator Context Management
//
// This translation unit manages the XPath evaluation context stack and state for the evaluator.  The context
// includes the current node, position, size, attribute node, and variable bindings that define the environment
// in which XPath expressions are evaluated.
//
// Key responsibilities:
//   - Context stack push/pop operations for nested expression
//   - Variable binding and scope management for FLWOR expressions
//
// The context management system allows the evaluator to properly handle location paths with predicates,
// function calls that change the context node, and nested expressions that require isolated evaluation
// environments.  By maintaining explicit context stacks, the evaluator can traverse complex expressions
// whilst preserving the correct semantics for position() and last() functions.

#include "eval_detail.h"
#include "../../xml/schema/schema_types.h"
#include <utility>

namespace {

class ContextGuard {
   private:
   XPathEvaluator * evaluator{nullptr};
   bool active{false};

   public:
   ContextGuard(XPathEvaluator &Evaluator, XTag *Node, size_t Position, size_t Size, const XMLAttrib *Attribute)
      : evaluator(&Evaluator), active(true) {
      evaluator->push_context(Node, Position, Size, Attribute);
   }

   ContextGuard(ContextGuard &&Other) noexcept
      : evaluator(std::exchange(Other.evaluator, nullptr)), active(std::exchange(Other.active, false)) {}

   ContextGuard & operator=(ContextGuard &&Other) noexcept {
      if (this IS &Other) return *this;

      if (active and evaluator) evaluator->pop_context();

      evaluator = std::exchange(Other.evaluator, nullptr);
      active = std::exchange(Other.active, false);

      return *this;
   }

   ContextGuard(const ContextGuard &) = delete;
   ContextGuard & operator=(const ContextGuard &) = delete;

   ~ContextGuard() {
      if (active and evaluator) evaluator->pop_context();
   }
};

} // namespace

//********************************************************************************************************************
// Extracts STEP nodes; Detects leading ROOT and whether it was a descendant (//); Injects a synthetic
// descendant-or-self::node() step when needed.
//
// Ownership for injected steps is tracked in OwnedSteps so that the raw pointers placed into Steps remain valid for
// the call site lifetime.

static void normalise_location_path(const XPathNode *PathNode, std::vector<const XPathNode *> &Steps,
   std::vector<std::unique_ptr<XPathNode>> &OwnedSteps, bool &HasRoot, bool &RootDescendant)
{
   Steps.clear();
   OwnedSteps.clear();
   HasRoot = false;
   RootDescendant = false;

   if (not PathNode) return;

   for (size_t i = 0; i < PathNode->child_count(); ++i) {
      auto child = PathNode->get_child(i);
      if (not child) continue;

      if ((i IS 0) and (child->type IS XQueryNodeType::ROOT)) {
         HasRoot = true;
         RootDescendant = child->value IS "//";
         continue;
      }

      if (child->type IS XQueryNodeType::STEP) Steps.push_back(child);
   }

   if (RootDescendant) {
      auto descendant_step = std::make_unique<XPathNode>(XQueryNodeType::STEP);
      descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "descendant-or-self"));
      descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, "node"));
      Steps.insert(Steps.begin(), descendant_step.get());
      OwnedSteps.push_back(std::move(descendant_step));
   }
}

//********************************************************************************************************************
// Builds the initial context node set for a path evaluation. For absolute paths this is NULL; for relative paths it
// is the current context node (or NULL if the current context is undefined).

static NODES build_initial_context(bool HasRoot, const XPathContext &ctx)
{
   NODES nodes;
   if (HasRoot) {
      nodes.push_back(nullptr);
   }
   else {
      if (ctx.context_node) nodes.push_back(ctx.context_node);
      else nodes.push_back(nullptr);
   }
   return nodes;
}

//********************************************************************************************************************
// Parsed view of a STEP node.

struct ParsedStep {
   const XPathNode *axis_node = nullptr;
   const XPathNode *node_test = nullptr;
   std::vector<const XPathNode *> predicate_nodes;
};

static ParsedStep parse_step_node(const XPathNode *StepNode)
{
   ParsedStep out;
   if ((not StepNode) or (StepNode->type != XQueryNodeType::STEP)) return out;

   out.predicate_nodes.reserve(StepNode->child_count());

   for (size_t i = 0; i < StepNode->child_count(); ++i) {
      auto child = StepNode->get_child(i);
      if (not child) continue;

      if (child->type IS XQueryNodeType::AXIS_SPECIFIER) out.axis_node = child;
      else if (child->type IS XQueryNodeType::PREDICATE) out.predicate_nodes.push_back(child);
      else if ((not out.node_test) and ((child->type IS XQueryNodeType::NAME_TEST) or
                                     (child->type IS XQueryNodeType::WILDCARD) or
                                     (child->type IS XQueryNodeType::NODE_TYPE_TEST))) {
         out.node_test = child;
      }
   }

   return out;
}

//********************************************************************************************************************

static std::vector<ParsedStep> parse_steps_vector(const std::vector<const XPathNode *> &Steps)
{
   std::vector<ParsedStep> parsed;
   parsed.reserve(Steps.size());
   for (auto *step : Steps) parsed.push_back(parse_step_node(step));
   return parsed;
}

//********************************************************************************************************************
// Save the current context and establish a new context with the provided node, position, size, and optional attribute.

void XPathEvaluator::push_context(XTag *Node, size_t Position, size_t Size, const XMLAttrib *Attribute)
{
   context_stack.push_back(context); // Save frame
   // Set new frame details
   context.context_node = Node;
   context.attribute_node = Attribute;
   context.position = Position;
   context.size = Size;
   // Retain existing contextual XML, otherwise inherit from evaluator
   if (not context.xml) context.xml = xml;
}

//********************************************************************************************************************
// Restore the previous context when unwinding recursive evaluation.

void XPathEvaluator::pop_context()
{
   if (context_stack.empty()) {
      context.context_node = nullptr;
      context.attribute_node = nullptr;
      context.position = 1;
      context.size = 1;
      context.xml = xml;
   }
   else {
      context = context_stack.back();
      context_stack.pop_back();
   }
}

//********************************************************************************************************************
// Dispatch AST nodes to the appropriate evaluation routine based on node type.

ERR XPathEvaluator::evaluate_ast(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (not Node) return ERR::Failed;

   // NOTE: This switch targets top-level AST categories (path traversal versus expression bodies).
   //       The handlers funnel into specialised evaluation entry points rather than the
   //       expression dispatcher, so it intentionally remains separate from the central
   //       NODE_HANDLERS map.

   switch (Node->type) {
      case XQueryNodeType::LOCATION_PATH:
         return evaluate_location_path(Node, CurrentPrefix);

      case XQueryNodeType::STEP:
         return evaluate_step_ast(Node, CurrentPrefix);

      case XQueryNodeType::UNION:
         return evaluate_union(Node, CurrentPrefix);

      case XQueryNodeType::PATH:
         if ((Node->child_count() > 0) and Node->get_child(0) and
             (Node->get_child(0)->type IS XQueryNodeType::LOCATION_PATH)) {
            return evaluate_location_path(Node->get_child(0), CurrentPrefix);
         }
         return evaluate_top_level_expression(Node, CurrentPrefix);

      case XQueryNodeType::EXPRESSION:
      case XQueryNodeType::FILTER:
      case XQueryNodeType::BINARY_OP:
      case XQueryNodeType::UNARY_OP:
      case XQueryNodeType::FUNCTION_CALL:
      case XQueryNodeType::LITERAL:
      case XQueryNodeType::VARIABLE_REFERENCE:
      case XQueryNodeType::NUMBER:
      case XQueryNodeType::STRING:
      case XQueryNodeType::CONDITIONAL:
      case XQueryNodeType::FOR_EXPRESSION:
      case XQueryNodeType::LET_EXPRESSION:
      case XQueryNodeType::FLWOR_EXPRESSION:
      case XQueryNodeType::QUANTIFIED_EXPRESSION:
         return evaluate_top_level_expression(Node, CurrentPrefix);

      default:
         return ERR::Failed;
   }
}

//********************************************************************************************************************
// Execute a full location path expression, managing implicit root handling.
// Returns ERR::Search if no matches were found.

ERR XPathEvaluator::evaluate_location_path(const XPathNode *PathNode, uint32_t CurrentPrefix)
{
   pf::Log log(__FUNCTION__);

   if ((not PathNode) or (PathNode->type != XQueryNodeType::LOCATION_PATH)) return log.warning(ERR::Failed);

   std::vector<const XPathNode *> steps;
   std::vector<std::unique_ptr<XPathNode>> owned_steps;
   bool has_root = false;
   bool root_descendant = false;

   normalise_location_path(PathNode, steps, owned_steps, has_root, root_descendant);

   if (steps.empty()) return ERR::Search;

   auto initial_context = build_initial_context(has_root, context);

   bool matched = false;
   auto result = evaluate_step_sequence(initial_context, steps, 0, CurrentPrefix, matched);

   if ((result IS ERR::Okay) or (result IS ERR::Search)) {
      if (query->Callback.defined()) return ERR::Okay; // Search (not found) is not relevant with a callback
      return matched ? ERR::Okay : ERR::Search; // At least one match == Okay, otherwise Search
   }
   else return result;
}

//********************************************************************************************************************
// Evaluate a union expression by computing each branch and combining results with deduplication.

ERR XPathEvaluator::evaluate_union(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::UNION)) return ERR::Failed;

   auto saved_context = context;
   auto saved_context_stack = context_stack;
   bool saved_expression_unsupported = expression_unsupported;

   auto last_error = ERR::Search;

   std::unordered_set<std::string> evaluated_branches;
   evaluated_branches.reserve(Node->child_count());

   for (size_t index = 0; index < Node->child_count(); ++index) {
      auto branch = Node->get_child(index);
      if (not branch) continue;

      auto branch_signature = build_ast_signature(branch);
      if (not branch_signature.empty()) {
         auto insert_result = evaluated_branches.insert(branch_signature);
         if (not insert_result.second) continue;
      }

      context = saved_context;
      context_stack = saved_context_stack;
      expression_unsupported = saved_expression_unsupported;

      auto result = evaluate_ast(branch, CurrentPrefix);
      if ((result IS ERR::Okay) or (result IS ERR::Terminate)) return result;

      if (result != ERR::Search) {
         last_error = result;
         break;
      }
   }

   context = saved_context;
   context_stack = saved_context_stack;
   expression_unsupported = saved_expression_unsupported;

   return last_error;
}

//********************************************************************************************************************
// Evaluate a single location path step against the current evaluation context.

ERR XPathEvaluator::evaluate_step_ast(const XPathNode *StepNode, uint32_t CurrentPrefix)
{
   pf::Log log(__FUNCTION__);

   if (not StepNode) return log.warning(ERR::NullArgs);

   std::vector<const XPathNode *> steps;
   steps.push_back(StepNode);

   auto context_nodes = build_initial_context(false, context);

   bool matched = false;
   auto result = evaluate_step_sequence(context_nodes, steps, 0, CurrentPrefix, matched);

   if ((result IS ERR::Okay) or (result IS ERR::Search)) {
      if (query->Callback.defined()) return ERR::Okay; // Search (not found) is not relevant with a callback
      return matched ? ERR::Okay : ERR::Search; // At least one match == Okay, otherwise Search
   }
   else return result;
}

//********************************************************************************************************************
// Advance one step for the step-sequencing evaluator by expanding axis candidates, applying predicates and either
// invoking callbacks (for last steps) or preparing the next context for subsequent steps.

static ERR advance_step_context(XPathEvaluator &Eval, const std::vector<XPathEvaluator::AxisMatch> &CurrentContext,
   AxisType Axis, const XPathNode *NodeTest, const std::vector<const XPathNode *> &PredicateNodes,
   bool IsLastStep, uint32_t CurrentPrefix, bool &Matched, std::vector<XPathEvaluator::AxisMatch> &NextContext,
   std::vector<XPathEvaluator::AxisMatch> &AxisCandidates, std::vector<XPathEvaluator::AxisMatch> &PredicateBuffer,
   bool &ShouldTerminate)
{
   ShouldTerminate = false;
   NextContext.clear();

   for (auto &context_entry : CurrentContext) {
      Eval.expand_axis_candidates(context_entry, Axis, NodeTest, CurrentPrefix, AxisCandidates);
      if (AxisCandidates.empty()) continue;

      auto predicate_error = Eval.apply_predicates_to_candidates(PredicateNodes, CurrentPrefix, AxisCandidates, PredicateBuffer);
      if (predicate_error != ERR::Okay) return predicate_error;
      if (AxisCandidates.empty()) continue;

      auto step_error = Eval.process_step_matches(AxisCandidates, Axis, IsLastStep, Matched, NextContext, ShouldTerminate);
      if (step_error != ERR::Okay) return step_error;
      if (ShouldTerminate) return ERR::Okay;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Filters matches for collect_step_results (no callbacks) with special-case handling for foreign-document child-axis
// roots identical to the original implementation.

static ERR filter_step_matches_for_collect(XPathEvaluator &Eval, const std::vector<XPathEvaluator::AxisMatch> &ContextNodes,
   AxisType Axis, const XPathNode *NodeTest, const std::vector<const XPathNode *> &PredicateNodes, uint32_t CurrentPrefix,
   std::vector<XPathEvaluator::AxisMatch> &Out, std::vector<XPathEvaluator::AxisMatch> &AxisBuffer,
   std::vector<XPathEvaluator::AxisMatch> &PredicateBuffer, bool &Unsupported)
{
   for (auto &context_entry : ContextNodes) {
      Eval.expand_axis_candidates(context_entry, Axis, NodeTest, CurrentPrefix, AxisBuffer);

      // Foreign-document child-axis fallback: include the context node itself if it is a root of a foreign
      // document and matches the node test.

      if (AxisBuffer.empty()) {
         if ((Axis IS AxisType::CHILD) and context_entry.node and (context_entry.node->ParentID IS 0) and
             Eval.is_foreign_document_node(context_entry.node)) {
            if (Eval.match_node_test(NodeTest, Axis, context_entry.node, context_entry.attribute, CurrentPrefix)) {
               AxisBuffer.push_back(context_entry);
            }
         }
      }

      if (AxisBuffer.empty()) continue;

      auto predicate_error = Eval.apply_predicates_to_candidates(PredicateNodes, CurrentPrefix, AxisBuffer, PredicateBuffer);
      if (predicate_error != ERR::Okay) {
         Unsupported = true;
         return ERR::Failed;
      }
      if (AxisBuffer.empty()) continue;

      // Append to the output in document order
      Out.insert(Out.end(), AxisBuffer.begin(), AxisBuffer.end());
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Expand axis candidates by applying the axis traversal and filtering by the node test.

void XPathEvaluator::expand_axis_candidates(const AxisMatch &ContextEntry, AxisType Axis,
   const XPathNode *NodeTest, uint32_t CurrentPrefix, std::vector<AxisMatch> &FilteredMatches)
{
   FilteredMatches.clear();

   auto *context_node = ContextEntry.node;
   const XMLAttrib *context_attribute = ContextEntry.attribute;

   if ((not context_attribute) and context_node and context.attribute_node and (context_node IS context.context_node)) {
      context_attribute = context.attribute_node;
   }

   auto axis_matches = dispatch_axis(Axis, context_node, context_attribute);
   if (FilteredMatches.capacity() < axis_matches.size()) {
      FilteredMatches.reserve(axis_matches.size());
   }

   for (auto &match : axis_matches) {
      if (not match_node_test(NodeTest, Axis, match.node, match.attribute, CurrentPrefix)) continue;
      FilteredMatches.push_back(match);
   }
}

//********************************************************************************************************************
// Apply predicate expressions sequentially to filter axis candidates.

ERR XPathEvaluator::apply_predicates_to_candidates(const std::vector<const XPathNode *> &PredicateNodes,
   uint32_t CurrentPrefix, std::vector<AxisMatch> &Candidates, std::vector<AxisMatch> &ScratchBuffer)
{
   for (auto *predicate_node : PredicateNodes) {
      ScratchBuffer.clear();
      if (ScratchBuffer.capacity() < Candidates.size()) {
         ScratchBuffer.reserve(Candidates.size());
      }

      for (size_t index = 0; index < Candidates.size(); ++index) {
         auto &match = Candidates[index];
         ContextGuard context_guard(*this, match.node, index + 1, Candidates.size(), match.attribute);

         auto predicate_result = evaluate_predicate(predicate_node, CurrentPrefix);
         if (predicate_result IS PredicateResult::UNSUPPORTED) return ERR::Failed;
         if (predicate_result IS PredicateResult::MATCH) ScratchBuffer.push_back(match);
      }

      Candidates.swap(ScratchBuffer);
      if (Candidates.empty()) break;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Invoke the registered callback for a matched node, handling both C and script callbacks.

ERR XPathEvaluator::invoke_callback(XTag *Node, const XMLAttrib *Attribute, bool &Matched, bool &ShouldTerminate)
{
   pf::Log log(__FUNCTION__);

   ShouldTerminate = false;
   if (not Node) return ERR::Okay;

   TAGS * tags = nullptr;
   bool is_constructed = (Node->ID <= 0);

   if (is_constructed) {
      // Node was constructed on-the-fly and has no representation in the xml object.
      // Temporarily append it to xml->Tags so the callback can access it.

      xml->appendTags(*Node);
      tags = &xml->Tags;
   }
   else if (tags = xml->getTags(Node); not tags) {
      log.warning("Unable to locate tag list for callback on node ID %d.", Node->ID);
      return ERR::Search;
   }

   // Use defer to ensure constructed nodes are removed when we exit
   auto cleanup = pf::Defer([&, is_constructed]() {
      if (is_constructed and (not xml->Tags.empty())) {
         xml->nullifyMap(xml->Tags.back());
         xml->Tags.pop_back();
      }
   });

   Matched = true;

   if (not query->Callback.defined()) {
      ShouldTerminate = true;
      return ERR::Okay;
   }

   if (query->Callback.isC()) {
      auto routine = (ERR (*)(extXML *, int, CSTRING, APTR))query->Callback.Routine;
      return routine(xml, Node->ID, Attribute ? Attribute->Name.c_str() : nullptr, query->Callback.Meta);
   }
   else if (query->Callback.isScript()) {
      ERR callback_error = ERR::Okay;
      if (sc::Call(query->Callback, std::to_array<ScriptArg>({
         { "XML",  xml, FD_OBJECTPTR },
         { "Tag",  Node->ID },
         { "Attrib", Attribute ? Attribute->Name.c_str() : nullptr }
      }), callback_error) != ERR::Okay) return ERR::Terminate;

      return callback_error;
   }
   else return ERR::InvalidValue;
}

//********************************************************************************************************************
// Process matched axis nodes by invoking callbacks or passing to the next step.

ERR XPathEvaluator::process_step_matches(const std::vector<AxisMatch> &Matches, AxisType Axis, bool IsLastStep,
   bool &Matched, std::vector<AxisMatch> &NextContext, bool &ShouldTerminate)
{
   ShouldTerminate = false;

   for (size_t index = 0; index < Matches.size(); ++index) {
      auto &match = Matches[index];
      auto *candidate = match.node;

      ContextGuard context_guard(*this, candidate, index + 1, Matches.size(), match.attribute);

      if (Axis IS AxisType::ATTRIBUTE) {
         AxisMatch next_match{};
         next_match.node = candidate;
         next_match.attribute = match.attribute;

         if (not next_match.node or not next_match.attribute) continue;

         if (IsLastStep) {
            ShouldTerminate = false;
            auto callback_error = invoke_callback(next_match.node, next_match.attribute, Matched, ShouldTerminate);

            if (callback_error IS ERR::Terminate) return ERR::Terminate;
            if (callback_error != ERR::Okay) return callback_error;
            if (ShouldTerminate) return ERR::Okay;

            continue;
         }

         NextContext.push_back(next_match);
         continue;
      }

      if (IsLastStep) {
         if (not candidate) continue;

         ShouldTerminate = false;
         auto callback_error = invoke_callback(candidate, nullptr, Matched, ShouldTerminate);

         if (callback_error IS ERR::Terminate) return ERR::Terminate;
         if (callback_error != ERR::Okay) return callback_error;
         if (ShouldTerminate) return ERR::Okay;

         continue;
      }

      if (not candidate) {
         if (not IsLastStep) {
            bool propagate_document_node = (Axis IS AxisType::DESCENDANT_OR_SELF) or
               (Axis IS AxisType::ANCESTOR_OR_SELF) or (Axis IS AxisType::SELF);
            if (propagate_document_node) NextContext.push_back({ nullptr, match.attribute });
         }
         continue;
      }

      NextContext.push_back({ candidate, nullptr });
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Recursively evaluate a sequence of location path steps against the context nodes.

ERR XPathEvaluator::evaluate_step_sequence(const NODES &ContextNodes, const std::vector<const XPathNode *> &Steps,
   size_t StepIndex, uint32_t CurrentPrefix, bool &Matched)
{
   if (StepIndex >= Steps.size()) return Matched ? ERR::Okay : ERR::Search;

   auto parsed_steps = parse_steps_vector(Steps);

   std::vector<AxisMatch> current_context;
   current_context.reserve(ContextNodes.size());

   for (auto *candidate : ContextNodes) {
      const XMLAttrib *attribute = nullptr;
      if ((candidate) and context.attribute_node and (candidate IS context.context_node)) attribute = context.attribute_node;
      current_context.push_back({ candidate, attribute });
   }

   std::vector<AxisMatch> next_context;
   next_context.reserve(current_context.size());

   std::vector<AxisMatch> axis_candidates;
   axis_candidates.reserve(current_context.size());
   std::vector<AxisMatch> predicate_buffer;
   predicate_buffer.reserve(current_context.size());

   for (size_t step_index = StepIndex; step_index < Steps.size(); ++step_index) {
      if (current_context.empty()) break;

      auto step_node = Steps[step_index];
      if ((not step_node) or (step_node->type != XQueryNodeType::STEP)) return ERR::Failed;
      auto &parsed = parsed_steps[step_index];
      AxisType axis = AxisType::CHILD;
      if (parsed.axis_node) axis = AxisEvaluator::parse_axis_name(parsed.axis_node->value);

      bool is_last_step = (step_index + 1 >= Steps.size());

      bool should_terminate = false;
      auto step_error = advance_step_context(*this, current_context, axis, parsed.node_test, parsed.predicate_nodes,
         is_last_step, CurrentPrefix, Matched, next_context, axis_candidates, predicate_buffer, should_terminate);
      if (step_error != ERR::Okay) return step_error;
      if (should_terminate) return ERR::Okay;

      current_context.swap(next_context);
   }

   return Matched ? ERR::Okay : ERR::Search;
}

//********************************************************************************************************************
// Returns the static registry mapping predicate operation names to their handler methods.

const std::unordered_map<std::string_view, XPathEvaluator::PredicateHandler> &XPathEvaluator::predicate_handler_map() const
{
   static const std::unordered_map<std::string_view, PredicateHandler> handlers = {
      { "attribute-exists", &XPathEvaluator::handle_attribute_exists_predicate },
      { "attribute-equals", &XPathEvaluator::handle_attribute_equals_predicate },
      { "content-equals", &XPathEvaluator::handle_content_equals_predicate }
   };
   return handlers;
}

//********************************************************************************************************************
// Dispatch a named predicate operation to its registered handler function.

XPathEvaluator::PredicateResult XPathEvaluator::dispatch_predicate_operation(std::string_view OperationName, const XPathNode *Expression,
   uint32_t CurrentPrefix)
{
   auto &handlers = predicate_handler_map();
   auto it = handlers.find(OperationName);
   if (it IS handlers.end()) return PredicateResult::UNSUPPORTED;

   auto handler = it->second;
   return (this->*handler)(Expression, CurrentPrefix);
}

//********************************************************************************************************************
// Predicate handler for the attribute-exists operation.

XPathEvaluator::PredicateResult XPathEvaluator::handle_attribute_exists_predicate(const XPathNode *Expression, uint32_t /*CurrentPrefix*/)
{
   auto *candidate = context.context_node;
   if (not candidate) return PredicateResult::NO_MATCH;
   if ((not Expression) or (Expression->child_count() IS 0)) return PredicateResult::UNSUPPORTED;

   const XPathNode *name_node = Expression->get_child(0);
   if (not name_node) return PredicateResult::UNSUPPORTED;

   if (name_node->value IS "*") {
      return (candidate->Attribs.size() > 1) ? PredicateResult::MATCH : PredicateResult::NO_MATCH;
   }

   for (int index = 1; index < std::ssize(candidate->Attribs); ++index) {
      auto &attrib = candidate->Attribs[index];
      if (pf::iequals(attrib.Name, name_node->value)) return PredicateResult::MATCH;
   }

   return PredicateResult::NO_MATCH;
}

//********************************************************************************************************************
// Predicate handler for the attribute-equals operation with wildcard support.

XPathEvaluator::PredicateResult XPathEvaluator::handle_attribute_equals_predicate(const XPathNode *Expression, uint32_t CurrentPrefix)
{
   auto *candidate = context.context_node;
   if (not candidate) return PredicateResult::NO_MATCH;
   if ((not Expression) or (Expression->child_count() < 2)) return PredicateResult::UNSUPPORTED;

   const XPathNode *name_node = Expression->get_child(0);
   const XPathNode *value_node = Expression->get_child(1);
   if ((not name_node) or (not value_node)) return PredicateResult::UNSUPPORTED;

   const std::string &attribute_name = name_node->value;
   std::string attribute_value;
   bool wildcard_value = false;

   if (value_node->type IS XQueryNodeType::LITERAL) {
      attribute_value = value_node->value;
      wildcard_value = attribute_value.find('*') != std::string::npos;
   }
   else {
      bool saved_expression_unsupported = expression_unsupported;
      auto evaluated_value = evaluate_expression(value_node, CurrentPrefix);
      bool evaluation_failed = expression_unsupported;
      expression_unsupported = saved_expression_unsupported;
      if (evaluation_failed) return PredicateResult::NO_MATCH;

      attribute_value = evaluated_value.to_string();
      wildcard_value = attribute_value.find('*') != std::string::npos;
   }

   bool wildcard_name = attribute_name.find('*') != std::string::npos;

   for (int index = 1; index < std::ssize(candidate->Attribs); ++index) {
      auto &attrib = candidate->Attribs[index];

      bool name_matches;
      if (attribute_name IS "*") name_matches = true;
      else if (wildcard_name) name_matches = pf::wildcmp(attribute_name, attrib.Name);
      else name_matches = pf::iequals(attrib.Name, attribute_name);

      if (not name_matches) continue;

      bool value_matches;
      if (wildcard_value) value_matches = pf::wildcmp(attribute_value, attrib.Value);
      else value_matches = pf::iequals(attrib.Value, attribute_value);

      if (value_matches) return PredicateResult::MATCH;
   }

   return PredicateResult::NO_MATCH;
}

//********************************************************************************************************************
// Predicate handler for the content-equals operation with wildcard support.

XPathEvaluator::PredicateResult XPathEvaluator::handle_content_equals_predicate(const XPathNode *Expression, uint32_t CurrentPrefix)
{
   auto *candidate = context.context_node;
   if (not candidate) return PredicateResult::NO_MATCH;
   if ((not Expression) or (Expression->child_count() IS 0)) return PredicateResult::UNSUPPORTED;

   const XPathNode *value_node = Expression->get_child(0);
   if (not value_node) return PredicateResult::UNSUPPORTED;

   std::string expected;
   bool wildcard_value = false;

   if (value_node->type IS XQueryNodeType::LITERAL) {
      expected = value_node->value;
      wildcard_value = expected.find('*') != std::string::npos;
   }
   else {
      bool saved_expression_unsupported = expression_unsupported;
      auto evaluated_value = evaluate_expression(value_node, CurrentPrefix);
      bool evaluation_failed = expression_unsupported;
      expression_unsupported = saved_expression_unsupported;
      if (evaluation_failed) return PredicateResult::NO_MATCH;

      expected = evaluated_value.to_string();
      wildcard_value = expected.find('*') != std::string::npos;
   }

   if (not candidate->Children.empty()) {
      auto &first_child = candidate->Children[0];
      if ((not first_child.Attribs.empty()) and (first_child.Attribs[0].isContent())) {
         const std::string &content = first_child.Attribs[0].Value;
         if (wildcard_value) {
            return pf::wildcmp(expected, content) ? PredicateResult::MATCH : PredicateResult::NO_MATCH;
         }
         return pf::iequals(content, expected) ? PredicateResult::MATCH : PredicateResult::NO_MATCH;
      }
   }

   return PredicateResult::NO_MATCH;
}

//********************************************************************************************************************
// Evaluate a predicate expression, applying XPath predicate coercion rules.

XPathEvaluator::PredicateResult XPathEvaluator::evaluate_predicate(const XPathNode *PredicateNode, uint32_t CurrentPrefix)
{
   if ((not PredicateNode) or (PredicateNode->type != XQueryNodeType::PREDICATE)) {
      return PredicateResult::UNSUPPORTED;
   }

   if (PredicateNode->child_count() IS 0) return PredicateResult::UNSUPPORTED;

   const XPathNode *expression = PredicateNode->get_child(0);
   if (not expression) return PredicateResult::UNSUPPORTED;

   if (expression->type IS XQueryNodeType::BINARY_OP) {
      auto *candidate = context.context_node;
      if (not candidate) return PredicateResult::NO_MATCH;

      auto dispatched = dispatch_predicate_operation(expression->value, expression, CurrentPrefix);
      if (dispatched != PredicateResult::UNSUPPORTED) return dispatched;
   }

   auto result_value = evaluate_expression(expression, CurrentPrefix);

   if (expression_unsupported) {
      expression_unsupported = false;
      return PredicateResult::UNSUPPORTED;
   }

   if (result_value.Type IS XPVT::NodeSet) {
      return result_value.node_set.empty() ? PredicateResult::NO_MATCH : PredicateResult::MATCH;
   }

   if (result_value.Type IS XPVT::Boolean) {
      return result_value.to_boolean() ? PredicateResult::MATCH : PredicateResult::NO_MATCH;
   }

   if (result_value.Type IS XPVT::String) {
      return result_value.to_string().empty() ? PredicateResult::NO_MATCH : PredicateResult::MATCH;
   }

   if (result_value.Type IS XPVT::Number) {
      double expected = result_value.to_number();
      if (std::isnan(expected)) return PredicateResult::NO_MATCH;

      double integral_part = 0.0;
      double fractional = std::modf(expected, &integral_part);
      if (fractional != 0.0) return PredicateResult::NO_MATCH;
      if (integral_part < 1.0) return PredicateResult::NO_MATCH;

      return (context.position IS size_t(integral_part)) ? PredicateResult::MATCH : PredicateResult::NO_MATCH;
   }

   return PredicateResult::UNSUPPORTED;
}

//********************************************************************************************************************
// Resolve which XML document owns a given node by checking ID maps and registrations.

extXML * XPathEvaluator::resolve_document_for_node(XTag *Node) const
{
   if ((not Node) or (not xml)) return nullptr;

   auto &map = xml->getMap();
   auto base = map.find(Node->ID);
   if ((base != map.end()) and (base->second IS Node)) return xml;

   for (auto &imp : parse_context->XMLCache) {
      auto &imp_map = imp.second->getMap();
      auto imported = imp_map.find(Node->ID);
      if ((imported != imp_map.end()) and (imported->second IS Node)) return imp.second;
   }

   return nullptr;
}

//********************************************************************************************************************
// Determines if a node belongs to a different XML document than the evaluator's primary document.

bool XPathEvaluator::is_foreign_document_node(XTag *Node) const
{
   auto *document = resolve_document_for_node(Node);
   return (document) and (document != xml);
}

//********************************************************************************************************************
// Collect all nodes resulting from evaluating a step sequence without callback invocation.

NODES XPathEvaluator::collect_step_results(const std::vector<AxisMatch> &ContextNodes,
   const std::vector<const XPathNode *> &Steps, size_t StepIndex, uint32_t CurrentPrefix,
   bool &Unsupported)
{
   NODES results;

   if (Unsupported) return results;

   if (StepIndex >= Steps.size()) {
      for (auto &entry : ContextNodes) results.push_back(entry.node);
      return results;
   }

   auto step_node = Steps[StepIndex];
   if ((not step_node) or (step_node->type != XQueryNodeType::STEP)) {
      Unsupported = true;
      return results;
   }

   auto parsed = parse_step_node(step_node);
   AxisType axis = AxisType::CHILD;
   if (parsed.axis_node) axis = AxisEvaluator::parse_axis_name(parsed.axis_node->value);

   bool is_last_step = (StepIndex + 1 >= Steps.size());

   std::vector<AxisMatch> filtered_all;
   filtered_all.reserve(ContextNodes.size());

   std::vector<AxisMatch> axis_buffer;
   axis_buffer.reserve(ContextNodes.size());
   std::vector<AxisMatch> predicate_buffer;
   predicate_buffer.reserve(ContextNodes.size());

   auto f_err = filter_step_matches_for_collect(*this, ContextNodes, axis, parsed.node_test, parsed.predicate_nodes,
      CurrentPrefix, filtered_all, axis_buffer, predicate_buffer, Unsupported);
   if (f_err != ERR::Okay) return {};

   if (filtered_all.empty()) return results;

   if (is_last_step) {
      for (auto &match : filtered_all) results.push_back(match.node);
      return results;
   }

   auto child_results = collect_step_results(filtered_all, Steps, StepIndex + 1, CurrentPrefix, Unsupported);
   if (Unsupported) return {};
   results.insert(results.end(), child_results.begin(), child_results.end());

   return results;
}
