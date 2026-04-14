# Plan: XQuery 3.0 Windowing Clauses (`window`, `tumbling`, `sliding`, `start`, `end`, `allowing`)

## Objectives
1. Extend the XQuery parser and evaluator so FLWOR expressions can include tumbling and sliding window clauses that conform to W3C XQuery 3.0 section 3.12.4.
2. Support the `start`, `end`, and `allowing empty` modifiers inside window clauses, including positional/previous/following variables and the associated test expressions.
3. Provide robust error handling, variable scoping, and sequence materialisation so windowing interacts correctly with existing `for`/`let` clauses, grouping, and order by clauses.

## References and Constraints
- W3C XQuery 3.0 Recommendation, §3.12.4 "Window Clauses" plus Appendix A grammar productions (Clause 21).
- Current implementation touch points (from previous investigations):
  - Tokeniser: `src/xquery/parse/xquery_tokeniser.cpp`
  - Parser: `src/xquery/parse/xquery_parser.cpp`
  - AST definitions: `src/xquery/xquery.h`
  - Evaluator: `src/xquery/eval/eval_expression.cpp`, `eval_flwor.cpp`, `eval_iterators.cpp`
  - Tests: `src/xquery/tests/test_flwor.tiri` (and related files)
- Follow repository guidelines in `AGENTS.md` (British English, tab width, no `static_cast`, etc.).

## Step-by-step Implementation Plan

### Phase 1 – Lexical support
1. **Introduce windowing keywords**
   - Update the keyword table in `xquery_tokeniser.cpp` to emit dedicated tokens for `window`, `tumbling`, `sliding`, `start`, `end`, `when`, `previous`, `next`, `only`, `allowing`, and `empty` when they appear in FLWOR contexts.
   - Ensure legacy behaviour (these words treated as QNames) remains available when they appear in positions other than the FLWOR grammar, using context-sensitive token types or fallback to identifier tokens if the parser does not expect the keyword.
   - Add lexical regression tests confirming the new tokens are produced and that identifiers such as `windowing` remain unaffected.

### Phase 2 – AST and parser modelling
2. **Define data structures**
   - Extend `enum XQueryNodeType` with entries for `FLWOR_TUMBLING_WINDOW` and `FLWOR_SLIDING_WINDOW` (or a single `FLWOR_WINDOW` node carrying a mode flag).
   - Create a `struct XQueryWindowClause` in `xquery.h` containing:
     - Window type (`Tumbling`/`Sliding`).
     - Window variable name and optional positional variable.
     - Optional `start` and `end` condition blocks (each with variable bindings for `current`, `previous`, `next`, and boolean `only`).
     - Optional `allowing_empty` boolean.
     - Child expression references for `start when`, `end when`, and `window body` expressions.

3. **Parse window clauses**
   - In the FLWOR grammar (likely `parse_flwor_clauses()`), recognise `for $var window <mode>` following an existing `for` clause or as part of the clause loop.
   - Parse `tumbling` or `sliding`, then the mandatory `window` keyword, window variable, optional positional variable (`at $pos`), `in ExprSingle`, and `start` / `end` sections as per grammar:
     - `start startVar at $pos previous $prev next $next when ExprSingle` (each binding optional, but `when` mandatory).
     - `end endVar at $pos previous $prev next $next when ExprSingle`.
   - Support `allowing empty` between the `start` and `end` parts.
   - Record source locations for diagnostic messages.
   - Maintain compatibility with the existing `for` clause AST so downstream evaluator logic can iterate over a single representation for FLWOR clauses.

4. **Update variable scoping**
   - Ensure the parser registers all declared windowing variables (main, positional, start/end, previous, next) in the FLWOR variable environment so later clauses and return expression can resolve them.
   - Guarantee that `start`/`end` variables are only accessible within their respective `when` expressions.

### Phase 3 – Evaluator groundwork
5. **Extend iterator model**
   - Review existing FLWOR evaluation infrastructure (`eval_flwor.cpp`). Determine whether clauses are modelled as iterators or as AST nodes processed by a dispatcher.
   - Introduce a `WindowClauseIterator` (for both tumbling and sliding) that consumes the input sequence from the immediately preceding clause.
   - Define helper structures to maintain the current window state: start/end indices, buffered items, flags for empty windows, and references to `start`/`end` variable bindings.

6. **Implement tumbling windows**
   - Evaluate the input sequence eagerly enough to determine window boundaries, but avoid materialising the entire sequence unless necessary.
   - For each window:
     - Bind the main window variable to a sequence representing the items in the window.
     - Populate the positional variable with a 1-based counter if requested.
     - Evaluate the `start when` expression to determine the start boundary (first item that satisfies the `start` condition). For tumbling windows, each new window begins immediately after the previous window ends.
     - Evaluate the `end when` expression. If `end` is omitted, treat the window as running until the next `start` satisfies or the input is exhausted.
     - Honour `allowing empty` by creating windows even if no items satisfied the `start` condition.

7. **Implement sliding windows**
   - Allow overlapping windows where the window starts at each item that satisfies the `start when` expression while previous windows may still be active until their `end when` condition is met.
   - Maintain a deque of active windows with metadata (start index, bound variables, state) and update them as each input item is processed.
   - Close windows when their `end when` expression succeeds; emit them in order, binding the window variable accordingly.
   - Handle `only end` (start omitted) and `only start` (end omitted) per the spec rules.

8. **Variable binding semantics**
   - Within `start when` and `end when` expressions, bind the `start/end` variables to the item being examined, the `previous` variable to the preceding item, and the `next` variable to the following item if available (empty sequence otherwise).
   - Ensure these bindings are updated per iteration without leaking into other windows.
   - Provide error handling for `only start` / `only end` forms (e.g., `start only when ...`).

9. **Implement `allowing empty`**
   - Track when a window has no items between its start and end. If `allowing empty` is not present, skip such windows; otherwise, emit an empty sequence for the window variable.
   - Validate spec-mandated error `XQDY0054` for `allowing empty` used outside sliding/tumbling contexts or incorrectly placed.

10. **Integrate with downstream clauses**
    - Ensure the `WindowClauseIterator` feeds its output into subsequent `where`, `group by`, `order by`, `return`, etc., identical to other clause iterators.
    - Reuse existing tuple/variable frame infrastructure so `window` behaves like `for`/`let` in evaluation order.

### Phase 4 – Error handling and diagnostics
11. **Static errors**
    - During parsing, raise `XPST0003` for malformed window syntax (missing `start`, duplicate variable names, `allowing empty` outside clause boundaries).
    - Add checks for reusing variable names already declared earlier in the FLWOR expression and emit `XQST0039` where appropriate.

12. **Dynamic errors**
    - Implement runtime checks for non-deterministic `start when` or `end when` expressions that return non-boolean values (raise `XPTY0004`).
    - Validate that the input sequence for windowing is not updated during iteration and raise an error if the evaluator detects unsupported types (e.g., function items if not yet implemented).

### Phase 5 – Testing
13. **Unit tests**
    - Extend `src/xquery/tests/test_flwor.tiri` (or create `test_windowing.tiri`) with coverage for:
      - Basic tumbling windows with `start`/`end` and `allowing empty` variations.
      - Sliding windows with overlapping boundaries.
      - `start only`, `end only`, and `start ... end` combos using `previous`/`next` variables.
      - Error scenarios (duplicate variable names, invalid `allowing empty` placement, non-boolean `when` results).
    - Include sequences of nodes and atomics to ensure both are supported.

14. **Interop tests**
    - Import relevant QT3 windowing test sets (e.g., `prod-WINDOW`, `prod-WINDOWClause`) if licensing permits. Document any skipped tests with TODOs.
    - Add regression tests ensuring legacy FLWOR expressions still evaluate identically (no performance regressions).

### Phase 6 – Documentation and follow-up
15. **Update planning documents**
    - Amend `XQUERY_30_REQUIREMENTS.md` to mark the windowing gap as targeted once this implementation begins.
    - Cross-reference this plan from future higher-level XQuery roadmap documents.

17. **Future considerations**
    - Evaluate whether higher-order windowing (interaction with `group by`, `count`, or streaming) requires additional optimisations.
    - Consider how maps/arrays and lookup operator support integrate with windowing variables once both feature sets land.
