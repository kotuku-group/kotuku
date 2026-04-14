# XQuery Expression Evaluator Architecture

## Overview
The XPath/XQuery expression evaluator traverses the AST produced by the parser and computes the runtime value for each
node. The implementation lives primarily in `eval_expression.cpp` with supporting routines in adjacent files.

## Dispatch Mechanism
Expression evaluation uses a hybrid dispatch strategy in `eval_expression.cpp`:

1. **Fast path switch**: The leading switch inside `XPathEvaluator::evaluate_expression()` handles the hottest node
   kinds (binary and unary operators, variable references, function calls, and literals) without hashing overhead.
2. **Dispatch table**: A static `NODE_HANDLERS` hash map maps every remaining node type to its handler method.
3. **Handler methods**: Each node type exposes a dedicated `handle_<type>()` method that contains the evaluation logic.

When adding new node types, follow this sequence:

1. Declare the enum in `src/xquery/xquery.tdl` and regenerate headers if necessary.
2. Add a handler declaration to `XPathEvaluator` in `src/xquery/xquery.h`.
3. Implement the handler in `src/xquery/eval/eval_expression.cpp` (or a related evaluation file when appropriate).
4. Register the handler in the `NODE_HANDLERS` table.
5. Extend the fast-path switch if the new node is performance critical.
6. Add Flute coverage under `src/xquery/tests/` and run the `xquery` CTest label.

## Handler Method Conventions
All handlers use the signature:

```cpp
XPathVal handle_<type>(const XPathNode *Node, uint32_t CurrentPrefix);
```

* **Node**: Non-null AST node provided by the dispatcher.
* **CurrentPrefix**: Namespace prefix index for QName resolution during evaluation.
* **Return**: `XPathVal` representing the computed result (empty with `expression_unsupported` flagged on failure).

Handlers must:

* Validate structural preconditions and record user-facing diagnostics with `record_error()`.
* Set `expression_unsupported = true` on unrecoverable errors and propagate child flags.
* Reuse helper utilities (e.g., `evaluate_predicate()`, `numeric_promote()`) to enforce consistent semantics.
* Use accessor helpers on `XPathNode` instead of manual child/field indexing.

## Performance Considerations
* The fast-path switch covers node kinds that accounted for the majority of dispatch hits during LR-3 profiling.
* Handler functions are intentionally small to encourage inlining and instruction cache locality.
* Binary operator handling is split into logical, set, sequence, arithmetic, and comparison helpers for clarity and
  CPU-friendly branching.
* Avoid heap allocations in hot paths; prefer arena-backed storage provided by the evaluator.
* Operator nodes produced by the parser populate cached enum hints (`BinaryOperationKind`, `UnaryOperationKind`). The
  handlers consult `get_cached_binary_kind()` / `get_cached_unary_kind()` first and increment fallback counters when the
  hints are absent. Trace logging highlights unexpected cache misses so helper factories can be corrected quickly.

## Testing
After modifying dispatch logic or handlers, run the module build and labelled tests:

```bash
cmake --build build/agents --config Release --target xquery --parallel
cmake --install build/agents
ctest --build-config Release --test-dir build/agents -L xquery
```

These steps rebuild the evaluator, install updated artefacts, and execute the dedicated XQuery regression suite.
