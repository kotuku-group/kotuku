# Tiri Programming Language Manual

# Manual Writing Style Guide

This document defines the official style guide for writing the Tiri Programming Language manual. Its purpose is to ensure the manual is  precise, consistent, unambiguous, and suitable as a long-term technical reference.

The produced manual will target AsciiDoc as the official formatting standard.

The manual is a **specification and contract**, not a tutorial.

When writing new sections in the manual, refer to the @docs/wiki/Tiri-Reference-Manual.md first to check if pre-existing information is available.

Refer to the actual implementation of the Tiri module and the LuaJIT compiler as necessary at `src/tiri/`.

There is no word-wrapping requirement when writing AsciiDoc files.  Column width restrictions only apply to pre-formatted text sections.

## Version Maintenance

It is the responsibility of the project maintainer to add a `VERIFIED` comment with a date to the top of each `*.adoc` file once reviewed and considered fit to publish.  If updating an adoc file that has been previously verified, add a comment to the top of file to indicate your changes.  This will help the maintainer review your changes for resubmission, and they can then update the verification date.

## Document Generation Process

From the root folder, generating the PDF manual is achieved as follows:

```
asciidoctor-pdf -a source-highlighter=rouge -o docs/html/manuals/tiri-manual.pdf \
-r .\docs\manuals\tiri\tiri_lexer.rb docs/manuals/tiri/book.adoc
```

------------------------------------------------------------------------

# Function Documentation Template

Functions are documented as simply as possible, with minimal verbosity.  Consider the following example:

```
### io.open()

`file = io.open(Path, [Mode])`

Opens the file at `Path`, requesting access permissions specified by `Mode`.  If `Mode` is not set, the file is opened in read-only mode.  If an error occurs, an exception is thrown with a descriptive error message.

`file = io.open('temp:myfile.txt', 'w')`

Available options for `Mode` are as follows:

|Option|Description|
|:-|:-|
|r|Read only.|
|w|Create new file in read/write mode.|
|a|Append to existing file in read/write mode.  Automatically creates file if it does not exist.|
|+|Open existing file in read/write mode.|
```

The title `io.open()` declares both the namespace and name of the function.  This is followed by a declaration that includes the results and parameters.  Parameters are always declared in upper camel-case.  Optional parameters are encased in square brackets.  Parameter types are not specified.  Always reference parameter names by enclosing them with backticks.

The next paragraph describes the behaviour of the function, and explicitly calls out the available parameters and their effects.  Notice that it is mentioned that the function can throw exceptions, but further detail is avoided.  It is sufficient to mention to the reader that such behaviour is possible without being overly descriptive.  The parameter types are not declared as it is possible for the reader to infer the type from a combination of the parameter names and the example code.  It is only necessary to make reference to types if there could be ambiguity for the reader, or a parameter supports multiple types.  For example "the `Output` parameter accepts table and array values".

The opening paragraph can be followed by a basic example if warranted.  The behaviour of a simple function such as `string.ltrim()` may not warrant an example, because if the description is sufficient then the example is adding unnecessary verbosity.  If a complex function needs more than one example, add it to additional lines in the same code block.

If necessary, the example can be followed with more detail on function behaviour and parameter effects.  In this case, a table is provided to make the available Mode options clear to the reader.

## AsciiDoc Formatting

Precede function prototype declarations with `[.api]`, e.g.

```
[.api]`str = string.substr(String, Start, [End])`
```

Format Tiri source code examples with the Tiri lexer, e.g.

```
[source,tiri]
----
'hello world':substr(0, 5)    -- 'hello'
'hello world':substr(6)       -- 'world'
----
```

------------------------------------------------------------------------

# Writing Rules

## Use Imperative Voice

Correct:

> Returns the length of the string.

Incorrect:

> The length of the string is returned.

## Use Present Tense Only

Correct:

> Raises an error if index is invalid.

Incorrect:

> Will raise an error

## Avoid Ambiguous Words

Do not use:

-   normally
-   usually
-   typically
-   might
-   sometimes
-   should

Replace with precise guarantees.

## Define Error Behaviour Explicitly

Every operation must clearly define:

-   error conditions
-   return values
-   failure behaviour

## Use Minimal Examples

Examples must demonstrate behaviour only.

Do not combine unrelated concepts.

## Define Terminology Once

Define clearly:

-   value
-   reference
-   object
-   mutable
-   immutable
-   identity

Use consistently.

## Separate Guarantees from Implementation

Guarantee:

> Array indexing is O(1)

Implementation detail:

> Arrays use contiguous memory

Only document implementation details when necessary.

------------------------------------------------------------------------

# Grammar and Syntax Definitions

Use EBNF (Extended Backus-Naur Form) notation for formal grammar definitions.  EBNF is restricted to Part I (Language Fundamentals) where it defines the core language syntax: statements, expressions, operators, control flow, and function declarations.  Library and API chapters do not use EBNF; function signatures and behaviour descriptions are sufficient.

Example:

```
if_statement = "if" expression "then" block
               { "elseif" expression "then" block }
               [ "else" block ]
               "end" ;
```

------------------------------------------------------------------------

# Cross-Reference Conventions

Use AsciiDoc `<<anchor>>` syntax for all cross-references.  Every chapter and numbered section defines an anchor in the form `ch-slug` or `sec-slug`.  Reference by anchor, not by chapter number, so that references remain valid if chapters are reordered.

Correct:

> See <<sec-tables>>.

> As defined in <<ch-error-handling>>.

Incorrect:

> See Chapter 9.

> See the Tables section.

------------------------------------------------------------------------

# Standard Structure for Each Language Feature

Each feature should include:

1.  Overview
2.  Syntax
3.  Semantics
4.  Errors
5.  Examples
6.  Notes (optional)
7.  See Also (optional)

------------------------------------------------------------------------

# Container and Data Structure Documentation Requirements

Each container must define:

-   mutation behaviour
-   ordering guarantees
-   iteration guarantees
-   identity guarantees
-   equality semantics
-   performance guarantees

------------------------------------------------------------------------

# Style Principles Summary

The manual must be:

-   Precise
-   Consistent
-   Deterministic
-   Unambiguous
-   Concise
-   Complete

The manual is a specification, not prose.

Every sentence in normative sections must define behaviour, constrain semantics, or describe error conditions.  Structural navigation ("See <<sec-tables>>") and cross-references are exempt.

------------------------------------------------------------------------

# Reference Implementation Neutrality

The manual defines the language, not the implementation.  Chapters that document the core language (Parts I–V) must avoid implementation-specific behaviour.

Chapters that are inherently implementation-specific (e.g. <<ch-the-debug-library>>, <<ch-performance-and-the-jit>>, <<ch-embedding-fluid-in-cpp>>) must be marked with the following admonition at the start of the chapter:

> **Implementation-specific.** This chapter documents behaviour specific to the Kōtuku/LuaJIT implementation and is not part of the language specification.

Avoid:

-   platform‑specific behaviour (unless explicitly documented as such)

------------------------------------------------------------------------

# Example Formatting Rules

Examples must:

-   use consistent indentation (3 spaces)
-   use minimal code
-   avoid unrelated features
-   avoid unnecessary explanation
-   use single quotes as the default when declaring strings, or `[[ ... ]]` for complex encoded strings such as regex
-   word-wrapping of paragraphs is unnecessary outside of pre-formatted areas (assume the reader has word-wrapping enabled in the editor).  For code, use a maximum column width of 80 characters.

------------------------------------------------------------------------

# Manual Is a Contract

The manual defines:

-   behaviour guarantees
-   programmer expectations
-   implementation requirements

Ambiguity is a defect.
