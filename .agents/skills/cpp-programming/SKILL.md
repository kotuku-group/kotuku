---
name: cpp-programming
description: >-
  Use when writing, editing, reviewing, or testing Kōtuku C++ source or headers, or when giving guidance about
  Kōtuku C++ code. Applies the project's non-standard language rules, formatting, validation, and embedded API
  documentation conventions.
---

# C++ Programming

Use this skill before changing Kōtuku C++ files or advising on Kōtuku C++ code. These project requirements override
ordinary C++ conventions.

## Language Rules

- Never use `static_cast`; use the project's C-style cast notation, for example `int(value)`.
- Never use `&&`; use `and`.
- Never use `||`; use `or`.
- Never use `==` for equality; use the `IS` macro. Operator overloads are exempt. `!=` remains permitted.
- Never use C++ exceptions. Check function results and propagate errors through the project's error-handling patterns.
- Target modern C++20 conventions and functionality where they do not conflict with these project rules.

## Naming And Formatting

- Use upper camel-case for function arguments.
- Use lower snake-case for variables inside functions.
- Prefix global variables with `gl` and use upper camel-case, for example `glSomeVariable`.
- Use three spaces for indentation.
- Keep opening braces on the same line for `if`, `while`, `else`, `for`, `switch`, and `struct` when the declaration
  has not wrapped.
- Use British English in code, comments, and documentation.
- Use `Kotuku` in code and `Kōtuku` in documentation.
- Keep lines within 120 columns.
- Use LF line endings only.
- Consider thread safety whenever a function uses global variables.

## Validation

- After changing C++ source or headers, build the affected module before considering the work complete.
- Use the Debug build and relevant commands documented in the repository `AGENTS.md`.
- In a static build, rebuild both the affected module and `origo_cmd` so the executable includes the change.
- Install the latest build before running `ctest`.
- Confirm both successful compilation and compliance with this skill's formatting rules.

## Embedded API Documentation

Kōtuku API documentation is embedded in C++ comment sections and parsed by the documentation generator.

- Use `-FUNCTION-` for functions, `-CLASS-` for classes, `-ACTION-` for actions, `-METHOD-` for methods, and
  `-FIELD-` for fields.
- A marked section ends with its comment or an explicit `-END-` marker.
- Preserve technically significant markers when editing surrounding comments.
- Use British English throughout embedded documentation.
