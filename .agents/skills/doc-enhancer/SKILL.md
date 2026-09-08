---
name: doc-enhancer
description: >-
  Use when improving, rewriting, or reviewing Kotuku technical documentation, including embedded C++ API
  documentation sections marked with -FUNCTION-, -CLASS-, -ACTION-, -METHOD-, or -FIELD-, and markdown technical
  guides. Use this skill for clearer API references, class and method descriptions, field documentation, examples,
  and professional documentation edits that must remain technically accurate.
---

# Documentation Enhancer

Use this skill to improve documentation while preserving technical accuracy and local project conventions. Apply edits
directly to the relevant files unless the user explicitly asks for review-only feedback.

## Workflow

1. Read the target documentation and the surrounding code or neighbouring documents before editing.  Scan for the markers
   `-FUNCTION-`, `-CLASS-`, `-ACTION-`, `-METHOD-` and `-FIELD-` to discover existing documentation.
2. Verify factual claims against the implementation, generated API documentation, or established project docs.
3. Improve clarity, precision, structure, and consistency without expanding the documentation into tutorial material unless
   the surrounding document is explicitly tutorial-oriented.
4. Preserve existing documentation markers, public API names, and documented behaviour. Do not invent behaviour from
   intent or naming alone.
5. After editing, summarise the meaningful documentation improvements and call out any areas where accuracy depended on
   an assumption.

## Embedded C++ Documentation

- Read `docs/wiki/Embedded-Document-Formatting.md` before editing embedded C++ documentation.
- Edit only existing documentation sections marked with `-FUNCTION-`, `-MODULE-`, `-CLASS-`, `-ACTION-`, `-METHOD-`, or `-FIELD-`.
  Ignore undocumented functions unless the user explicitly asks for new documentation sections.
- Do not add new marked sections unless the user explicitly asks for them.
- Use only the XML formatting features allowed by the embedded document formatting guide. Do not use markdown formatting
  in embedded C++ documentation.
- Ensure source code has priority over existing prose when they conflict.
- Document parameters, return values, and error behaviour clearly when the existing section includes those concerns.
- If a function returns error codes, ensure the `-ERRORS-` section references only `ERR` values that are explicitly
  returnable by the main function body.  Verify that the list is complete and adjust as necessary.  Do not associate a short description with an error unless you can add sufficient value beyond the default error description for that code.
- Use abstract API type names where possible because the generated API documentation is relevant to languages beyond C++:
  `INT`, `INT64`, `STRING`, `NULL`, and `BYTE` instead of C++-specific type spelling.
- Use backticks for named coding constants, enums, and flags that are defined in include headers.
- Avoid exposing implementation details that do not affect how callers use the public interface.
- Do not use headings except for important warnings or dark patterns.
- Do not use headings inside bullet points. Use `<b>` or `<i>` for emphasis when needed.
- For `-FIELD-` sections whose field is a lookup or flag type, write only a brief purpose summary. The documentation
  generator inserts the value breakdown.
- For `-METHOD-` or `-FUNCTION-` sections that refer to a struct, lookup, or flag type in parameters, use the `!` token
  to inject the generated value table instead of writing the value breakdown manually.
- For existing `-TAGS-` sections, ensure that the existing tags are not stale.
- If a documentation section is associated with a function, scan the function for characteristics that are relevant to the `-TAGS-` section and add relevant tags if not already present.  For documented functions, methods and actions, a new `-TAGS-` section can be added if one is not already present.
- For any given header (e.g. `-FUNCTION-`), the accepted order for the body of document sections is `-INPUT-`, `-RESULT-`, `-ERRORS-`, `-TAGS-`, `-END-`

## Markdown Documentation

- Structure content with clear heading levels and a logical flow from general concepts to specific details.
- Orient the reader quickly at the start of a document or section.
- Keep code blocks, lists, emphasis, and terminology consistent with nearby documentation.
- Verify examples for syntax and API accuracy where practical.
- Include practical examples only when they clarify usage or match the document's established style.
- Prefer concise connecting text over verbose transitions.

## Style Standards

- Use British English spelling.
- Use professional, direct technical language.
- Prefer active voice where it improves clarity.
- Be concise. Assume the reader is a competent software engineer who needs a reliable reference, not broad education.
- Avoid inflated wording. For example, prefer "Adds a sample stream to an Audio object for playback." over marketing-style
  phrasing about intelligent streaming or minimal memory footprints unless those claims are documented and useful.
- Use two spaces between sentences in edited documentation.
- Remove trailing whitespace.
- Wrap paragraphs at or before column 119 when editing wrapped documentation in code files.  In Markdown and AsciiDoc files, word-wrapping is not required except for pre-formatted text sections.
