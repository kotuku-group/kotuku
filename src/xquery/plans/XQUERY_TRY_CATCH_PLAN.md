# Plan: Try/Catch Support for the XQuery Module

## Objectives
1. Parse and evaluate XQuery 3.0 `try {}` / `catch` expressions so user queries can intercept and recover from dynamic errors per the W3C specification.
2. Ensure caught errors expose the correct `err:code`, `err:description`, `err:value`, and `err:module` metadata to catch clauses.
3. Preserve backwards compatibility with existing error propagation paths and ensure uncaught errors continue to bubble through the evaluator unchanged.
4. Provide unit and integration tests demonstrating successful catch handling, variable binding, and fallback behaviour for both expression and function contexts.

## References and Constraints
- Primary specification: W3C XQuery 3.0 Recommendation, Section 3.15 "Try/Catch Expressions" and Section 2.3 "Errors and Error Handling".
- Existing implementation touch points:
  - Tokeniser: `src/xquery/parse/xquery_tokeniser.cpp`
  - Parser + AST types: `src/xquery/parse/xquery_parser.cpp`, `src/xquery/xquery.h`
  - Evaluator: `src/xquery/eval/eval_expression.cpp`, `src/xquery/eval/eval_values.cpp`, `src/xquery/eval/xquery_evaluator.cpp`
  - Error plumbing: `src/xquery/xquery_errors.h`, `src/xquery/xquery_errors.cpp`, evaluator `record_error()` helpers
  - Tests: `src/xquery/tests/*`
- Follow repository-wide coding and documentation conventions defined in the root `AGENTS.md` (British English, three-space indentation, no `static_cast`, etc.).

## Step-by-step Implementation Plan

### Phase 1 – Specification review and gap audit
1. **Document current error behaviour**
   - Survey `XPathEvaluator::evaluate_expression()` and related helpers to understand how dynamic errors are emitted (return codes, `XPathValue` flags, `XQueryContext::error_state`).
   - Verify how errors are surfaced to Tiri callers so the try/catch implementation can reuse the same plumbing when a catch clause rethrows or returns alternate values.

2. **Extract requirements from W3C spec**
   - Summarise try/catch syntax, binding of `$err:code` / `$err:description` / `$err:value` / `$err:module`, wildcard catches, and the optional `catch *:code` patterns.
   - Note precedence rules (`try/catch` is an expression at the same precedence level as `if` / `typeswitch`).
   - Capture all mandated error codes: `XPTY0018` for wrong catch type, `XQDY0064` for invalid try/catch structure, etc.

### Phase 2 – Lexer and parser groundwork
3. **Add keywords and tokens**
   - Update the tokeniser to emit `TRY`, `CATCH`, and `QNAME_WILDCARD` tokens when appropriate.
   - Ensure `catch` only behaves as keyword when followed by whitespace or `{`, preserving existing identifier usage elsewhere.
   - Extend unit tests that validate reserved words to cover the new keywords.

4. **Extend AST definitions**
   - In `src/xquery/xquery.h`, add a new `XQueryNodeType::TRY_CATCH` value and a struct for holding the try body plus a vector of catch clauses.
   - Each catch clause should store: QName (possibly wildcard), optional `($var)` binding, optional type declaration (`as item()*`), and the handler expression node pointer.

5. **Parse try/catch expressions**
   - Modify `parse_flowr_expression()` or the highest-precedence expression parser that currently handles `if`, `typeswitch`, etc., to recognise `try { Expr } catch ( ... ) { ... }` sequences.
   - Implement grammar per spec:
     - `try { Expr } catch CatchClause+`
     - `CatchClause := (QName | NCName:* | *:NCName | Q{uri}* | * ) ( '(' VarName ( 'as' SequenceType )? ')' )? '{' Expr '}'`
   - Enforce at least one catch clause; emit parser errors for missing braces or malformed names.
   - Normalise wildcard QNames (e.g., store namespace URI + local part flags) for easy matching in the evaluator.

6. **Sequence type parsing for catch bindings**
   - Reuse existing `parse_sequence_type()` helper to parse the optional `as Type`. Ensure this code path can be invoked within catch clauses.
   - Record the parsed type in the AST node for static checking.

### Phase 3 – Static analysis and validation
7. **Static type checks**
   - During parse or early semantic analysis, validate that catch variable names are unique within the try expression and that optional `as` types are valid sequence types.
   - If the implementation performs static typing, extend the relevant pass to infer the resulting type of the try/catch expression as the union of the try body type and all catch handler types.

8. **Name resolution for error variables**
   - Ensure catch variable bindings are pushed to the lexical environment so they are visible inside handler expressions.
   - `err:code` style implicit variables can be implemented as automatically bound values (even if the user does not declare a variable). Decide whether to expose them via reserved variable names or via the explicit `catch ($code)` binding per spec.

### Phase 4 – Evaluator implementation
9. **Implement try execution**
   - In the evaluator, add a handler for `TRY_CATCH` nodes that:
     1. Evaluates the try body in a context that can trap dynamic errors (e.g., capture non-`ERR::OK` status plus metadata).
     2. If no error occurs, return the resulting sequence immediately.
     3. If an error occurs, package error information into a lightweight structure (code QName, description string, value sequence, module URI) for matching.

10. **Catch clause matching**
    - Implement matching logic that compares the raised error QName against each clause in order:
      - Exact QName match (consider namespace resolution with statically known prefixes).
      - Namespace wildcards (`*:local`), local-name wildcards (`prefix:*`), and full wildcard (`*`).
    - Respect optional `as` type filters by verifying the `err:value` sequence type; raise `XPTY0018` if the type test fails.
    - If no clause matches, rethrow the original error (propagate the stored status and metadata).

11. **Variable binding inside handlers**
    - When a clause matches:
      - Bind any explicit variable declared in the clause to the dynamic error object; spec requires binding to the caught error object (i.e., `map` with `code`, `description`, etc.) or to the `err:value`. Choose representation consistent with existing variable binding semantics (likely extend `XPathVal` with an `ErrorObject` kind or reuse `XPathMap`).
      - Provide helper functions such as `bind_error_variables()` to populate implicit variables (`$err:code`, `$err:description`, `$err:value`, `$err:module`) if the implementation uses the implicit variable model.
      - Execute the handler expression with the augmented variable scope; ensure bindings are popped afterwards to prevent leakage across handlers.

12. **Error propagation and rethrowing**
    - Allow handler expressions to raise new errors. The evaluator should propagate these normally.
    - Provide a `rethrow()` helper or allow handlers to call a `fn:error()` equivalent so they can re-emit the original error if needed.
    - Confirm that `catch {}` blocks returning empty sequences behave identically to `try` expressions whose errors are suppressed.

13. **Interaction with tail calls and optimisations**
    - Review existing optimisation paths (tail-call elimination, expression memoisation) to ensure they respect the new node type. If necessary, update switch statements or visitor patterns to include `TRY_CATCH` so the evaluator does not fall through to default error cases.

### Phase 5 – Error metadata infrastructure
14. **Extend error representation**
    - If current errors only track a code and message, extend `XQueryError` (or equivalent) to carry optional value sequences and module URIs.
    - Provide constructors/utilities so existing error sites can populate the extra fields even before try/catch uses them, ensuring backwards compatibility.

15. **Expose metadata to catch clauses**
    - Implement helper functions (`create_error_value_sequence()`, `convert_error_to_xpath_value()`) that produce the values bound to handler variables.
    - Document how `err:value` behaves when no value is supplied (should be empty sequence) and ensure descriptions fallback to default strings.

### Phase 6 – Testing and validation
16. **Parser tests**
    - Add unit tests covering successful parsing of simple try/catch expressions, wildcard catches, multiple handlers, optional `as` types, and syntax errors.
    - Verify that `try`/`catch` keywords remain usable as NCNames when not followed by braces (per spec requirements).

17. **Evaluator tests**
    - Create Tiri tests (e.g., `src/xquery/tests/test_try_catch.tiri`) that:
      - Force dynamic errors (`fn:error()`, division by zero, missing function) and demonstrate handler recovery.
      - Confirm clause ordering (first matching clause wins).
      - Validate variable bindings by inspecting returned sequences.
      - Test wildcard namespace matches and default catch-all behaviour.
      - Ensure uncaught errors propagate with original metadata.

18. **Regression tests**
    - Run the broader XQuery test suite to confirm existing expressions still pass.
    - Integrate applicable QT3 tests for try/catch if available, noting any skipped cases.

### Phase 7 – Documentation and follow-up tasks
19. **Update plan and requirement files**
    - Once implementation completes, amend `XQUERY_30_REQUIREMENTS.md` to mark try/catch as addressed and add references to new tests.

21. **Future enhancements**
    - Track follow-up work for:
      - Integration with higher-order functions if `catch` blocks need to accept inline functions.
      - Serialisation of error objects for Tiri callers.
      - Performance optimisation (e.g., caching resolved catch clause QNames).
