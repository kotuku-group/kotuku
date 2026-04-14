# XQuery 3.0 Compliance Requirements

This document records missing W3C XQuery 3.0 features discovered during the audit of Kōtuku's XQuery module.

## Summary of unsupported 3.0 features

| Feature | Current status | Evidence | Impact |
| --- | --- | --- | --- |
| Try/Catch expressions | Not tokenised or parsed | Keyword table in `src/xquery/parse/xquery_tokeniser.cpp` lacks `try`/`catch`; parser never branches on a try expression | Error recovery specified by 3.0 cannot be expressed |
| Switch expressions | Not tokenised or parsed | `switch` is absent from the tokenizer's reserved keywords, so expressions like `switch($x)` cannot even lex | Users must fall back to verbose `typeswitch` cases |
| FLWOR windowing (`tumbling`/`sliding` window clauses and `allowing empty`) | Not tokenised or parsed | No `window`, `tumbling`, `sliding`, `start`, `end`, or `allowing` keywords in the tokenizer; FLWOR parser only handles `for/let/where/group/order/count` clauses | Prevents 3.0 streaming and grouping patterns |
| Higher-order functions (inline `function(){}` expressions, function items, dynamic invocation) | Parser only supports prolog `declare function` and runtime function calls; no inline literals, simple map operator, arrow operator, or lookup operator | `parse_primary_expr` never handles inline `function` nodes; binary operator table lacks `!` and tokenizer lacks `=>` | Blocks arrow chains, partial application, and dynamic function APIs added in 3.0 |

## Detailed gap notes

### Try/Catch expressions
- The tokenizer enumerates every reserved word it recognises, but `try` and `catch` are missing, so a `try { ... } catch` sequence is lexed as generic identifiers and rejected during parsing.
- Add tokens, AST nodes, and evaluator handlers so queries can handle dynamic errors as required by §3.0 "Try/Catch Expressions".

### Switch expressions
- XQuery 3.0 adds the `switch` expression that simplifies `typeswitch`; only `typeswitch` exists today, so queries must emulate switch semantics.
- Implement keyword recognition, parser production, and evaluation analogous to the existing `typeswitch` handler.

### FLWOR windowing and allowing empty
- 3.0 FLWOR clauses introduce `window` (with `tumbling`/`sliding`) and `allowing empty`. The parser enforces a strict `for/let/where/group/order/count/return` ordering and never recognises window keywords, preventing these new clause forms.
- Extend the tokenizer and `parse_flwor_expr` to cover the 3.0 clause grammar, then add evaluator support for tuple windows and empty-sequence handling.

### Higher-order functions & arrow/simple map operators
- Inline `function($x){ ... }` expressions, the simple map operator `!`, and the arrow operator `=>` are the primary entry points to function items in 3.0, but the parser only permits prolog `declare function` definitions and call expressions.
- Adding these constructs requires AST additions, call-site bindings for closures, dynamic function invocation, and operator parsing for `!` and `=>`.
