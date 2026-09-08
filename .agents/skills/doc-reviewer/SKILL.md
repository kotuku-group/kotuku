---
name: doc-reviewer
description: >-
  Review Kōtuku technical documentation for developer usability, clarity, completeness, technical accuracy, and
  consistency with project documentation standards. Use when Codex is asked to review API docs, module docs, embedded
  C++ documentation comments, markdown guides, Tiri documentation, release-facing feature docs, or drafts that should
  let developers implement Kōtuku functionality without reverse-engineering examples or source code.
---

# Kōtuku Doc Reviewer

## Review Goal

Review documentation from the perspective of a competent software engineer trying to implement a Kōtuku feature using
only the documentation provided.

Prioritise gaps that would block implementation, then issues that raise cognitive load, then polish and consistency
improvements. Feedback must be specific, actionable, and tied to the developer workflow impact.

## Context Gathering

Before reviewing, inspect the documentation being reviewed and enough nearby context to judge accuracy:

- For embedded API docs, check the surrounding C++ implementation, TDL definitions, and generated or adjacent API
  references when relevant.
- For Tiri-facing docs or examples, use the `tiri-programming` skill before giving Tiri guidance or changing `.tiri`
  content.
- For Flute test documentation or examples, use the `flute-testing` skill before writing, reviewing, planning, or
  running Flute tests.
- For markdown guides, compare terminology and structure with nearby `docs/wiki/` or module documentation.
- Verify claims against source when the documentation describes signatures, fields, return values, error behaviour,
  threading, ownership, performance, or module interactions.

Do not assume examples are correct just because they are present. Validate whether examples match current APIs and
project style.

## Evaluation Criteria

Check clarity and parseability:

- Logical organisation and scan-friendly headings.
- Clear, unambiguous explanations of technical concepts.
- Consistent terminology and naming.
- Code examples that are formatted, syntactically plausible, and aligned with Kōtuku/Tiri style.
- Procedures broken into steps when multiple actions or concepts are involved.

Check completeness and self-sufficiency:

- Required parameters, return values, data types, fields, and constants are documented.
- Prerequisites, dependencies, setup requirements, and module interactions are stated.
- Error conditions, failure modes, return codes, and recovery expectations are described where applicable.
- Common workflows, edge cases, limitations, and performance considerations are covered.
- Developers would not need to inspect unrelated examples or source code to implement the documented task.
- For embedded documentation, tags are accurately defined in each `-TAGS-` section.

Check developer usefulness:

- The documentation answers how, why, and when to use the API or feature.
- Integration patterns with other Kōtuku modules are explained where relevant.
- Best practices, ownership rules, threading considerations, and lifecycle expectations are stated when they affect
  correct usage.
- Troubleshooting guidance exists for likely mistakes or ambiguous outcomes.

Check Kōtuku documentation standards:

- Use British English spelling.
- Use the simplified `Kotuku` spelling in code identifiers and examples.
- Use the branded spelling only in prose when matching existing project documentation.
- For embedded documentation, preserve marker conventions such as `-FUNCTION-`, `-CLASS-`, `-ACTION-`, `-METHOD-`,
  `-FIELD-`, `-TAGS-` and `-END-`.
- In embedded documentation, literal `<` and `>` characters must be XML-escaped as `&lt;` and `&gt;` (the generator
  parses these comments as XML). Flag any unescaped `<` or `>` in prose, ranges, or comparisons, e.g. a valid range
  written as `.01 < Multiplier < 10` must appear as `.01 &lt; Multiplier &lt; 10`.
- Keep line length and formatting consistent with surrounding files.

## Review Output

Use a code-review style response:

1. Lead with findings, ordered by severity.
2. Include file and line references whenever possible.
3. Explain why each issue matters to a developer trying to use the documentation.
4. Separate blockers from improvements.
5. Include open questions only when the documentation or source does not provide enough evidence.
6. Keep summary secondary and brief.

Use this severity model:

- **Critical**: Documentation is technically wrong or would lead developers to broken code, unsafe usage, or a false
  API contract.
- **Major**: Missing or unclear information would force developers to inspect source, examples, or generated files
  before implementing.
- **Minor**: Structure, terminology, formatting, or examples add avoidable friction but do not block implementation.

When no issues are found, say so directly and mention any remaining validation gaps, such as examples not being
executed or source not being available.
