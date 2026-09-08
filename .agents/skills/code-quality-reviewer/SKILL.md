---
name: code-quality-reviewer
description: >-
  Review Kōtuku code changes for correctness, maintainability, compile risk, and strict project coding standards. Use
  when Codex is asked to review C++, TDL, Tiri, Flute tests, CMake, or module integration changes; after implementing
  or modifying code; before committing; or when the user asks for a quality, compliance, regression, or bug-focused
  review of existing code.
---

# Code Quality Reviewer

Review code as a strict project maintainer. Prioritise bugs, behavioural regressions, compile failures, data races,
resource lifetime problems, and mandatory style violations. Keep praise brief or omit it unless the user asks for a
balanced critique.

## Context Gathering

Before reviewing, inspect the changed files and enough surrounding implementation to understand the contract:

- Use `git diff`, `git status`, and targeted `rg` searches when reviewing recent changes.
- Check nearby call sites, TDL definitions, generated API expectations, module `CMakeLists.txt`, and tests when relevant.
- For `.tiri` files, scripts, examples, or Tiri-facing API usage, use the `tiri-programming` skill before giving Tiri
  guidance.
- For Flute tests or test runner changes, use the `flute-testing` skill before writing, reviewing, planning, or running
  Flute tests.
- If C++ changed, verify whether the affected module has been built. If it has not, call out compile validation as a
  remaining risk.

Focus on the requested scope or recently modified code. Do not expand into a whole-repository audit unless asked.

## Mandatory Compliance Checks

For C++ changes, fail the review if any mandatory project rule is violated:

- Do not use `static_cast`; use C-style casts such as `int(value)`.
- Do not use `&&`; use `and`.
- Do not use `||`; use `or`.
- Do not use `==`; use the `IS` macro, except in operator overload definitions where applicable.
- Do not use C++ exceptions; check and propagate function results instead.
- Use upper camel-case for function arguments.
- Use lower snake_case for local variables.
- Use three spaces for indentation and no tabs.
- Do not leave trailing whitespace.
- Keep line length near the project default of 120 columns.
- Use British English in comments and documentation.

For Tiri changes, also check:

- Do not use Lua's `~=` inequality operator; use `!=`.
- Use top-to-bottom script execution patterns, not synthetic entry point functions unless existing code requires it.
- Use upper camel-case for function arguments and lower snake_case for local variables.
- Use three spaces for indentation and no trailing whitespace.

## Review Passes

Perform the review in this order:

1. Check mandatory style and syntax constraints first because any violation is a blocker.
2. Analyse correctness, edge cases, error handling, memory/resource ownership, and algorithmic complexity.
3. Check framework integration: object lifecycle, module dependencies, TDL/API contracts, includes, CMake registration,
   Tiri bindings, and vector/document/display architecture assumptions where relevant.
4. Check concurrency risks, especially global variables prefixed with `gl`, callbacks, worker threads, and shared caches.
5. Check tests: affected behaviour should have focused coverage, and new Tiri-facing behaviour should normally be covered
   by Flute tests.
6. Check embedded documentation markers such as `-FUNCTION-`, `-CLASS-`, `-ACTION-`, `-METHOD-`, `-FIELD-`, and `-END-`
   when documentation changed.

## Severity Model

- **Critical**: Compile failure, mandatory rule violation, crash, memory corruption, data race, security issue, or broken
  public API contract.
- **High**: Logic error, missing error propagation, leak, significant behavioural regression, or missing required test for
  risky behaviour.
- **Medium**: Maintainability issue, inefficient implementation, weak validation, unclear ownership, or incomplete but
  non-blocking coverage.
- **Low**: Minor clarity, formatting, naming, or documentation polish that does not affect behaviour.

Mark the overall review as failed when any Critical issue exists, or when High issues make the change unsafe to merge.

## Output Format

Use the repository's normal code-review format:

1. Lead with findings, ordered by severity.
2. Include precise file and line references whenever possible.
3. Explain the concrete risk and provide an actionable correction. Include short corrected snippets only when they make
   the fix unambiguous.
4. Add open questions only when source context is insufficient.
5. End with a brief summary and any validation gaps, such as tests or builds not run.

If no issues are found, say that clearly and mention residual risk. Avoid long compliance checklists in the final answer
unless the user explicitly asks for one.
