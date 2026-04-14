# Plan: Switch Expression Support for the XQuery Module

## Objectives
1. Allow XQuery queries to use the W3C 3.0 `switch` expression syntax (`switch ($value) case "a" return 1 default return 0`) by extending the tokenizer, parser, and AST representations.
2. Execute switch expressions according to specification semantics, including sequential `case` evaluation, empty sequence behaviour, and dynamic type comparisons (using `eq`/`deep-equal` rules where applicable).
3. Provide complete error reporting and test coverage so regression and conformance tests clearly signal invalid syntax, arity mismatches, or unsupported combinations.

## References and Constraints
- Primary specification: **W3C XQuery 3.0 Recommendation**, Section 3.9.2 *Switch Expressions* and related static typing rules in Section 2.3.
- Supporting reference: **XQuery and XPath Functions & Operators 3.0**, Section 1.5 for comparison semantics (`fn:deep-equal`, value comparisons, `collation` attributes).
- Implementation entry points (per previous plans and repo layout):
  - Tokeniser: `src/xquery/parse/xquery_tokeniser.cpp`
  - Parser / AST: `src/xquery/parse/xquery_parser.cpp`, `src/xquery/xquery.h`
  - Evaluator: `src/xquery/eval/eval_expression.cpp`, `src/xquery/eval/eval_values.cpp`
  - Error definitions: `src/xquery/xquery_errors.h`
  - Tests: `src/xquery/tests/`
- Adhere to repo-wide guidelines (British English, tab width, no `static_cast`, etc.) from root `AGENTS.md`.

## Step-by-step Implementation Plan

### Phase 1 – Baseline review
1. **Catalogue current conditional constructs**
   - Audit parser functions that handle `typeswitch`, `if`, and `case`-like keywords to understand how existing tokens are consumed.
   - Note whether `case`/`default` tokens are already defined for `typeswitch`; reuse if practical to keep keyword handling consistent.
   - Inspect evaluator handlers for `typeswitch` or conditional nodes to determine reuse opportunities (e.g., result caching, context item evaluation helpers).

2. **Identify AST representation gaps**
   - In `src/xquery/xquery.h`, confirm whether `XQueryNodeType` already contains entries for `TYPESWITCH` and what data structures store the branch list.
   - Decide if switch expressions can reuse the same struct with a flag, or if a dedicated node type (e.g., `XQNT_SWITCH`) is clearer; document trade-offs in code comments.

### Phase 2 – Lexical support
3. **Tokenise `switch`, `case`, `default`, `return` combinations**
   - Verify `case`/`default` tokens currently exist (likely for `typeswitch`); ensure they remain reserved when `switch` is enabled.
   - If `return` already tokenised for FLWOR, confirm it is legal to reuse; otherwise add a token.
   - Update keyword table (and any perfect hash) to map `switch` to a distinct token and add coverage in lexical unit tests.

### Phase 3 – Parser extensions
4. **Grammar integration point**
   - Extend `parse_primary_expression()` or whichever method handles conditional expressions so that when `switch` token is encountered, the parser constructs a new AST node.
   - Recognise syntax: `switch` `(` Expr `)` (`case` SwitchCaseClause)+ `default` `return` ExprSingle.

5. **Parsing clauses**
   - Implement helper `parse_switch_case_clause()` returning a structure with `case_exprs` (vector) and `return_expr` pointer.
   - Support comma-separated `case` values per spec (`case 1, 2 return ...`). Each `case` should hold a vector of child expression nodes.
   - Ensure at least one `case` is required and `default return` is mandatory; emit `XPST0003` (static error) when missing.

6. **AST structure**
   - Add node type `XQNT_SWITCH` with fields:
     - `XPathNode *test_expr;`
     - `std::vector<SwitchCase>` where `SwitchCase` contains `std::vector<XPathNode*> case_tests` and `XPathNode *return_expr`.
     - `XPathNode *default_expr;`
   - Provide constructors / helper functions for allocation consistent with the rest of the parser (likely using `XPathNodeAllocator`).

7. **Static analysis hooks**
   - Wire switch nodes into existing static typing / variable binding checks (if parser currently records variables or namespaces per node).
   - Update `resolve_sequence_types()` (if present) so each branch is type-checked and the resulting static type is the union of branch result types.

### Phase 4 – Evaluator implementation
8. **Entry point registration**
   - Add case to evaluator dispatch table to handle `XQNT_SWITCH` nodes (mirroring `typeswitch`).

9. **Execution semantics**
   - Evaluate the switch operand expression once, caching the resulting sequence.
   - For each `case` clause in order:
     1. Evaluate each case expression.
     2. For each value in the case expression result, compare against the switch operand sequence according to spec:
        - If operand is empty and clause is `case empty-sequence()`, treat as match (requires type check).
        - Otherwise, atomise the operand sequence to singletons if needed; raise `XPTY0004` when operand or case value returns more than one item where spec requires singletons.
        - Use `fn:deep-equal` or general comparison? (Per spec, `switch` uses `eq` for atomic values with implicit casting; follow Section 3.9.2). Document chosen helper, likely reuse existing general comparison utilities.
     3. On first match, evaluate and return the clause `return` expression; stop evaluating further clauses.
   - If no case matches, evaluate and return the `default` expression.

10. **Collation handling**
    - Support optional `collation` attribute on `case` clauses? (Spec allows `case "a" collation "..."`). Determine support level:
      - If collation support exists for `typeswitch`, reuse the same parsing/evaluation code.
      - If not, plan fallback: parse `case` optional `collation` URI and pass to comparison helper; for now, support at least the default collation and emit `XQST0038` on unknown URIs.

11. **Error handling**
    - Emit spec-aligned error codes:
      - `XPST0003` for syntax errors (missing `default`, empty case list, missing `return`).
      - `XPTY0004` when operand/case expression cardinalities mismatch.
      - `XQST0038` or other codes for duplicate `default`/invalid collation.
    - Update `xquery_errors.h` if any codes are missing and add descriptive text per repo conventions.

### Phase 5 – Testing
12. **Parser tests**
    - Add lexical/parser regression cases (maybe in `src/xquery/tests/test_parser.tiri`):
      - Basic switch with string literal cases.
      - Multiple case expressions and whitespace variations.
      - Error cases: missing parenthesis, missing default, duplicate `case` keywords without `return`.

13. **Evaluator tests**
    - Extend runtime tests (Tiri or QT3 harness) with scenarios:
      - Numeric comparisons with implicit casting (e.g., `switch (3) case 3 return "match"`).
      - Empty sequence operand hitting `default`.
      - Sequence of nodes requiring atomisation (e.g., `switch ($node/@type)` case `'foo'` ...).
      - Collation test if supported.
      - Ensure evaluation short-circuits after first match (cases with side effects or expensive computations).

14. **QT3 compatibility**
    - Identify relevant QT3 tests under `src/xquery/tests/qt3/` (if repository mirrors them). Enable the subset covering switch expressions and log unsupported tests.

### Phase 6 – Documentation and tracking
15. **Update gap tracking**
    - Once implementation is complete, amend `docs/plans/XQUERY_30_REQUIREMENTS.md` to mark `switch` as implemented (or reclassify under "in progress").

16. **Developer documentation**
    - Add comments in `xquery_parser.cpp` describing the grammar addition for future maintainers.
    - If there is a dedicated README for the XQuery module (`src/xquery/AGENTS.md`), add a note referencing the new plan/implementation steps for switch expressions.

17. **Rollout checklist**
    - Build target: `cmake --build build/agents --config Release --target xquery --parallel`.
    - Install: `cmake --install build/agents`.
    - Run targeted tests: `ctest --build-config Release --test-dir build/agents -L xquery`.
    - Execute Tiri-based parser tests if they live outside `ctest` harness (e.g., `origo src/xquery/tests/test_switch.tiri --log-warning`).

## Expected follow-ups
- After switch expression support, revisit the evaluator to share logic with `typeswitch` to avoid duplicate comparison utilities.
- Consider future enhancements for XQuery 3.1 `switch` expression features (e.g., `case typeswitch` style pattern matching) and note them in `XQUERY_30_REQUIREMENTS.md` if still outstanding.
- Monitor performance regressions in queries heavy on conditional branching; profile if necessary and optimise comparison caching.
