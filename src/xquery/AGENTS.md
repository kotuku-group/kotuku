# XQuery Module - AI Agent Guide

This file provides comprehensive information about the Kōtuku XQuery module for AI agents working with the codebase.

## Overview

The XQuery module provides comprehensive XPath 2.0 and XQuery language support for querying and navigating XML documents. It operates in conjunction with the XML module to deliver a standards-compliant query engine with extensive functionality.

**Key Features:**
- Full XPath 2.0 specification compliance
- XQuery language support with FLWOR expressions
- Comprehensive function library (100+ functions)
- Expression compilation and optimisation
- Thread-safe compiled expressions
- Schema-aware type system integration
- Schema-aware XML Schema type constructors (`xs:double`, `xs:date`, `xs:duration`, `xs:QName`, etc.)
- Type expressions (cast, castable, treat-as, instance-of, typeswitch)
- XQuery module system with import/composition support
- Namespace-aware query processing
- Variable binding and parameterised queries
- Node constructor expressions (direct and computed)

## Module Architecture

### Core Files Structure

```
src/xquery/
├── xquery.tdl                 # Interface definition (XQuery class, method registration)
├── constants.tdl              # XQuery-specific enumerations and flags (XQueryNodeType, XQF, XIF)
├── xquery.cpp                 # Module initialisation and core functions
├── xquery.h                   # Module header with AST, parser, and prolog structures
├── xquery_def.c               # Generated C definitions
├── xquery_class.cpp           # XQuery class implementation
├── xquery_class_def.cpp       # Auto-generated XQuery class definitions
├── unit_tests.cpp             # C++ unit tests for internal components
├── CMakeLists.txt             # Build configuration with 23 registered tests
├── AGENTS.md                  # AI agent guide for this module
├── W3C Error Codes.md         # W3C XPath/XQuery error code documentation
├── QT3_1_0/                   # W3C XQuery Test Suite (optional, extracted from zip)
├── api/                       # Public API implementations
│   ├── xquery_axis.cpp            # XQuery axis evaluation (13 standard axes)
│   ├── xquery_errors.h            # Error code definitions
│   ├── xquery_functions.cpp/h     # Function registry and dispatch
│   └── xquery_prolog.cpp          # XQuery prolog management and module loading
├── parse/                         # Expression parsing and tokenisation
│   ├── xquery_parser.cpp          # XPath/XQuery expression parser
│   └── xquery_tokeniser.cpp       # Lexical analysis and tokenisation
├── eval/                          # Expression evaluation engine
│   ├── AGENTS.md                  # Evaluator architecture guide for developers
│   ├── eval.cpp                   # Main evaluation entry points
│   ├── eval_common.cpp            # Common evaluation utilities
│   ├── eval_context.cpp           # Context and variable management
│   ├── eval_detail.h              # Internal evaluation details
│   ├── eval_expression.cpp        # Expression evaluation with dispatch table
│   ├── eval_flwor.cpp             # FLWOR expression support
│   ├── eval_navigation.cpp        # Node navigation
│   ├── eval_predicates.cpp        # Predicate evaluation
│   ├── eval_values.cpp            # Value operations
│   ├── date_time_utils.cpp/h      # Shared date/time canonicalisation helpers
│   └── checked_arith.h            # Arithmetic overflow checking utilities
├── functions/                     # Function implementations by category
│   ├── function_library.cpp       # Function library initialization
│   ├── accessor_support.cpp/h     # Accessor function utilities
│   ├── func_accessors.cpp         # Accessor functions (base-uri, data, etc.)
│   ├── func_booleans.cpp          # Boolean functions (not, true, false, etc.)
│   ├── func_datetimes.cpp         # Date/time functions (current-date, etc.)
│   ├── func_diagnostics.cpp       # Diagnostic functions (error, trace)
│   ├── func_documents.cpp         # Document functions (doc, collection, etc.)
│   ├── func_nodeset.cpp           # Node-set functions (name, local-name, etc.)
│   ├── func_numbers.cpp           # Numeric functions (sum, round, etc.)
│   ├── func_qnames.cpp            # QName functions
│   ├── func_sequences.cpp         # Sequence functions (distinct-values, etc.)
│   └── func_strings.cpp           # String functions (concat, substring, etc.)
└── tests/                         # Test infrastructure
    ├── test_accessor.tiri            # Accessor function tests
    ├── test_advanced.tiri            # Advanced XQuery queries
    ├── test_advanced_paths.tiri      # Advanced path expression tests
    ├── test_axes.tiri                # XQuery axes tests
    ├── test_constructors.tiri        # Node constructor tests
    ├── test_core.tiri                # Core XQuery functionality tests
    ├── test_datetime.tiri            # Date/time function tests
    ├── test_documents.tiri           # Document function tests
    ├── test_duration.tiri            # Schema duration parsing and validation tests
    ├── test_edge_cases.tiri          # Edge case and regression tests
    ├── test_flwor.tiri               # FLWOR expression tests
    ├── test_func_ext.tiri            # Extended function library tests
    ├── test_module_loading.tiri      # XQuery module import/loading tests
    ├── test_predicates.tiri          # Predicate evaluation tests
    ├── test_prolog.tiri              # XQuery prolog tests
    ├── test_qname.tiri               # QName operation tests
    ├── test_qt_math.tiri             # W3C QT3 math function compliance tests
    ├── test_reserved_words.tiri      # Reserved word handling tests
    ├── test_sequence_cardinality.tiri # Sequence cardinality regression tests
    ├── test_sequences.tiri           # Sequence operation tests
    ├── test_string_uri.tiri          # String and URI function tests
    ├── test_type_constructors.tiri   # Schema type constructor regression tests
    ├── test_type_expr.tiri           # Type expression tests (cast, castable, etc.)
    ├── test_unit_tests.tiri          # Runs the module's internal unit tests (requires ENABLE_UNIT_TESTS)
    └── modules/                       # XQuery module files for testing
        ├── bad_namespace.xq               # Error case: bad namespace declaration
        ├── circular_a.xq                  # Error case: circular module dependencies
        ├── circular_b.xq                  # Error case: circular module dependencies
        ├── composite.xq                   # Composite module usage
        ├── main_module.xq                 # Module entry point
        ├── math_utils.xq                  # Library module with math functions
        ├── self_reference.xq              # Error case: self-referencing module
        └── string_utils.xq                # Library module with string utilities
```

### Module Architecture Notes

**Important:** Parser and AST structures are consolidated in `xquery.h` rather than split across separate header files. This design choice improves compilation speed and maintains tight integration between parsing and evaluation components.

The module uses precompiled headers and unity builds for optimised compilation performance. Build configuration enables 23 registered Flute tests (label `xquery`) covering all aspects of XPath and XQuery functionality, including schema type constructors.

### Dependencies

- **unicode** module (PUBLIC) - For text encoding/decoding
- **xml** module - For XML document structures and schema integration
- **regex** module (dynamically loaded) - For pattern matching in XQuery functions

## XQuery Class

The module provides an `XQuery` class for direct XQuery evaluation without requiring an XML document:

**Include:** `<kotuku/modules/xquery.h>`

**Key Methods:**
- `Evaluate(Expression, Result)` - Compile and evaluate XQuery expression, returning typed result
- `Search(Expression, Callback)` - Compile and evaluate XQuery expression with node iteration callback
- `RegisterFunction(FunctionName, Callback)` - Register custom XQuery functions callable from expressions
- `InspectFunctions(Name, ResultFlags, Result)` - Retrieve metadata about compiled XQuery functions

This class is particularly useful for:
- Standalone XQuery processing
- Expression evaluation without XML context
- Direct access to XQuery prolog and module system
- Testing and validation of XQuery expressions
- Extending XQuery with custom function implementations
- Runtime introspection of function definitions

## Core Structures and Types

### XPathValue Structure

Represents the result of XQuery expression evaluation, supporting multiple value types.

**Include:** `<kotuku/modules/xml.h>` (shared type)

```cpp
struct XPathValue {
   XPVT Type;              // Type identifier (NodeSet, Boolean, Number, String, Date, Time, DateTime)
   double NumberValue;     // For Number and Boolean types
   std::string StringValue; // For String type
   pf::vector<XTag *> node_set; // For NodeSet type
}
```

**Type Enumeration (XPVT):**
- `NodeSet` - Collection of XML nodes
- `Boolean` - Boolean value (stored as number)
- `Number` - Numeric value
- `String` - String value
- `Date` - Date value
- `Time` - Time value
- `DateTime` - Date and time value

### XQueryNodeType Enumeration

Describes the type of nodes in the XQuery Abstract Syntax Tree.

**Include:** `<kotuku/modules/xquery.h>`

Key node types include:
- `LOCATION_PATH`, `STEP`, `NODE_TEST`, `PREDICATE`, `ROOT`
- `BINARY_OP`, `UNARY_OP`, `CONDITIONAL`
- `FLWOR_EXPRESSION`, `WHERE_CLAUSE`, `ORDER_CLAUSE`, `GROUP_CLAUSE`, `COUNT_CLAUSE`
- `FUNCTION_CALL`, `LITERAL`, `VARIABLE_REFERENCE`
- `QUANTIFIED_EXPRESSION` - For `some`/`every` expressions
- Constructor types: `DIRECT_ELEMENT_CONSTRUCTOR`, `COMPUTED_ELEMENT_CONSTRUCTOR`, etc.
- Type expressions: `INSTANCE_OF_EXPRESSION`, `TREAT_AS_EXPRESSION`, `CASTABLE_EXPRESSION`, `TYPESWITCH_EXPRESSION`

### XQF Flags (XQuery Feature Flags)

Flags indicating the features of a compiled XQuery expression.

**Include:** `<kotuku/modules/xquery.h>`

- `XQF::XPATH` - The expression is an XPath location string
- `XQF::HAS_PROLOG` - The XQuery declares a prolog
- `XQF::LIBRARY_MODULE` - Prolog declares a module namespace (library module)
- `XQF::MODULE_IMPORTS` - One or more module imports are declared
- `XQF::DEFAULT_FUNCTION_NS` - Default function namespace declared
- `XQF::DEFAULT_ELEMENT_NS` - Default element namespace declared
- `XQF::BASE_URI_DECLARED` - Static base URI declared
- `XQF::DEFAULT_COLLATION_DECLARED` - Default collation declared
- `XQF::BOUNDARY_PRESERVE` - Boundary-space preserve mode
- `XQF::CONSTRUCTION_PRESERVE` - Construction preserve mode
- `XQF::ORDERING_UNORDERED` - Ordering mode is unordered
- `XQF::HAS_WILDCARD_TESTS` - Wildcard name tests present

### XIF Flags (Inspect Functions Result Flags)

Result flags for the `InspectFunctions()` method controlling which metadata is returned.

**Include:** `<kotuku/modules/xquery.h>`

- `XIF::AST` - Include the compiled function body in the inspection result
- `XIF::NAME` - Include function name in the inspection result
- `XIF::PARAMETERS` - Include function parameters in the inspection result
- `XIF::RETURN_TYPE` - Include function return type in the inspection result
- `XIF::USER_DEFINED` - Include user-defined status in the inspection result
- `XIF::SIGNATURE` - Include function signature in the inspection result
- `XIF::ALL` - Include all available information (default if no flags specified)

## XPath 2.0 Language Support

### Path Expressions

Full support for XPath 2.0 path expressions including:

**Axes (all 13 standard axes):**
- `child::` - Child elements (default axis)
- `descendant::` - All descendants
- `descendant-or-self::` - Self and all descendants
- `parent::` - Parent element
- `ancestor::` - All ancestors
- `ancestor-or-self::` - Self and all ancestors
- `following::` - All following nodes
- `following-sibling::` - Following siblings only
- `preceding::` - All preceding nodes
- `preceding-sibling::` - Preceding siblings only
- `self::` - Current node
- `attribute::` - Attributes (short form: `@`)
- `namespace::` - Namespace nodes

**Node Tests:**
- Element names: `/root/element`
- Wildcards: `/root/*`
- Attribute selectors: `@attr`, `@*`
- Node type tests: `node()`, `text()`, `comment()`

**Path Types:**
- Absolute paths: `/root/element`
- Relative paths: `element/subelement`
- Recursive descent: `//element`

### Predicates

Complex predicate expressions with full boolean logic:

**Position Predicates:**
- `[1]` - First element
- `[last()]` - Last element
- `[position() < 5]` - First 4 elements

**Comparison Predicates:**
- `[@price < 10]` - Numeric comparison
- `[@category='fiction']` - String equality
- `[@*='value']` - Any attribute matching (Kōtuku extension)

**Boolean Predicates:**
- `[not(@deprecated)]` - Negation
- `[@a and @b]` - Logical AND
- `[@a or @b]` - Logical OR

**Content Matching:**
- `[=pattern]` - Match on text content (Kōtuku extension)

## XQuery Language Support

### FLWOR Expressions

Full support for XQuery FLWOR (For, Let, Where, Order by, Return) expressions:

**For Clause:**
```xquery
for $book in //book return $book/title
```

**Let Clause:**
```xquery
let $total := sum(//book/@price) return $total
```

**Where Clause:**
```xquery
for $book in //book where $book/@price < 20 return $book/title
```

**Order By Clause:**
```xquery
for $book in //book order by $book/@price return $book
```

**Return Clause:**
```xquery
for $x in 1 to 10 return $x * $x
```

**Combined Example:**
```xquery
for $book in //book
let $discount := $book/@price * 0.1
where $book/@category = 'fiction'
order by $book/@price descending
return $book/title
```

### Additional XQuery Features

**Group By Clause:**
```xquery
for $book in //book
group by $category := $book/@category
return $category
```

**Count Clause:**
```xquery
for $book in //book
count $index
return concat($index, ': ', $book/title)
```

**Quantified Expressions:**
```xquery
some $book in //book satisfies $book/@price < 10
every $book in //book satisfies $book/@isbn
```

### Node Constructors

XQuery supports both direct and computed constructors for creating new XML nodes:

**Direct Element Constructor:**
```xquery
<result>{//book/title}</result>
```

**Computed Element Constructor:**
```xquery
element book { attribute isbn {'123'}, 'Title' }
```

**Attribute Constructor:**
```xquery
attribute price { $book/@price * 0.9 }
```

**Text, Comment, and PI Constructors:**
```xquery
text { 'Hello' }
comment { 'This is a comment' }
processing-instruction target { 'data' }
```

### Type Expressions

XQuery provides comprehensive type system support for runtime type checking and conversion:

**Cast Expression:**
```xquery
$value cast as xs:decimal
"123" cast as xs:integer
current-date() cast as xs:string
```

**Castable Expression:**
```xquery
$value castable as xs:decimal  (: returns true/false :)
"abc" castable as xs:integer   (: returns false :)
```

**Treat As Expression:**
```xquery
$value treat as xs:string  (: raises error if not a string :)
```

**Instance Of Expression:**
```xquery
$value instance of xs:decimal
$sequence instance of element()*
```

**Typeswitch Expression:**
```xquery
typeswitch($value)
   case xs:integer return $value * 2
   case xs:string return upper-case($value)
   default return $value
```

**To Range Operator:**
```xquery
1 to 10                    (: sequence 1, 2, 3, ..., 10 :)
for $i in 1 to 5 return $i (: iterate over range :)
```

### XQuery Module System

XQuery supports modular programming through library module imports:

**Module Declaration:**
```xquery
module namespace math = "http://example.com/math";
declare function math:add($a, $b) { $a + $b };
```

**Module Import:**
```xquery
import module namespace math = "http://example.com/math" at "math_utils.xq";
math:add(1, 2)
```

**Multiple Function Imports:**
```xquery
import module namespace str = "http://example.com/strings" at "string_utils.xq";
import module namespace math = "http://example.com/math" at "math_utils.xq";
str:uppercase(math:add(1, 2))
```

**Module Composition:**
```xquery
(: composite.xq can import and re-export multiple modules :)
import module namespace comp = "http://example.com/composite" at "composite.xq";
```

## Function Library

The XQuery module provides an extensive function library organised by category.

### Node Functions

**Include:** `xquery_functions.h` (internal)

- `position()` - Current node position in context
- `last()` - Size of context node set
- `count(node-set)` - Count nodes in set
- `id(string)` - Find element by ID attribute
- `idref(string)` - Find elements by IDREF
- `name([node])` - Qualified name of node
- `local-name([node])` - Local part of node name
- `namespace-uri([node])` - Namespace URI of node
- `root([node])` - Root node of document
- `node-name([node])` - QName of node
- `base-uri([node])` - Base URI of node

### String Functions

**Include:** `functions/func_strings.cpp`

- `string(value)` - Convert to string
- `concat(string, ...)` - Concatenate strings
- `substring(string, start, [length])` - Extract substring
- `substring-before(string, delimiter)` - Text before delimiter
- `substring-after(string, delimiter)` - Text after delimiter
- `string-length([string])` - Length of string
- `normalize-space([string])` - Normalise whitespace
- `normalize-unicode(string, [form])` - Unicode normalisation
- `translate(string, from, to)` - Character translation
- `upper-case(string)` - Convert to uppercase
- `lower-case(string)` - Convert to lowercase
- `contains(string, substring)` - Substring test
- `starts-with(string, prefix)` - Prefix test
- `ends-with(string, suffix)` - Suffix test
- `string-join(sequence, separator)` - Join strings
- `encode-for-uri(string)` - URL encoding
- `iri-to-uri(string)` - IRI to URI conversion
- `escape-html-uri(string)` - HTML URI escaping

### Numeric Functions

**Include:** `functions/func_numbers.cpp`

- `number([value])` - Convert to number
- `sum(node-set)` - Sum numeric values
- `floor(number)` - Round down
- `ceiling(number)` - Round up
- `round(number)` - Round to nearest
- `round-half-to-even(number)` - Banker's rounding
- `abs(number)` - Absolute value
- `min(sequence)` - Minimum value
- `max(sequence)` - Maximum value
- `avg(sequence)` - Average value

### Boolean Functions

**Include:** `functions/func_booleans.cpp`

- `boolean(value)` - Convert to boolean
- `not(boolean)` - Logical negation
- `true()` - Boolean true
- `false()` - Boolean false
- `lang(string)` - Language test

### Sequence Functions

**Include:** `functions/func_sequences.cpp`

- `distinct-values(sequence)` - Remove duplicates
- `index-of(sequence, value)` - Find value positions
- `insert-before(sequence, position, values)` - Insert items
- `remove(sequence, position)` - Remove item
- `reverse(sequence)` - Reverse order
- `subsequence(sequence, start, [length])` - Extract subsequence
- `unordered(sequence)` - Remove ordering
- `exists(sequence)` - Test non-empty
- `empty(sequence)` - Test empty
- `deep-equal(sequence1, sequence2)` - Deep comparison
- `zero-or-one(sequence)` - Cardinality check
- `one-or-more(sequence)` - Cardinality check
- `exactly-one(sequence)` - Cardinality check

### Regular Expression Functions

**Include:** `functions/func_strings.cpp`

- `matches(string, pattern, [flags])` - Pattern matching
- `replace(string, pattern, replacement, [flags])` - Pattern replacement
- `tokenize(string, pattern, [flags])` - Split by pattern
- `analyze-string(string, pattern, [flags])` - Detailed analysis

### Date and Time Functions

**Include:** `functions/func_datetimes.cpp`

**Current Date/Time:**
- `current-date()` - Current date
- `current-time()` - Current time
- `current-dateTime()` - Current date and time
- `implicit-timezone()` - System timezone

**Date/Time Components:**
- `year-from-dateTime(dateTime)` - Extract year
- `month-from-dateTime(dateTime)` - Extract month
- `day-from-dateTime(dateTime)` - Extract day
- `hours-from-dateTime(dateTime)` - Extract hours
- `minutes-from-dateTime(dateTime)` - Extract minutes
- `seconds-from-dateTime(dateTime)` - Extract seconds
- `timezone-from-dateTime(dateTime)` - Extract timezone

**Similar extractors for date and time types**

**Timezone Adjustments:**
- `adjust-dateTime-to-timezone(dateTime, [timezone])` - Adjust timezone
- `adjust-date-to-timezone(date, [timezone])` - Adjust date timezone
- `adjust-time-to-timezone(time, [timezone])` - Adjust time timezone

**Duration Functions:**
- `years-from-duration(duration)` - Extract years
- `months-from-duration(duration)` - Extract months
- `days-from-duration(duration)` - Extract days
- `hours-from-duration(duration)` - Extract hours
- `minutes-from-duration(duration)` - Extract minutes
- `seconds-from-duration(duration)` - Extract seconds

### QName Functions

**Include:** `functions/func_qnames.cpp`

- `QName(namespace, local-name)` - Create QName
- `resolve-QName(string, element)` - Resolve QName from string
- `prefix-from-QName(qname)` - Extract prefix
- `local-name-from-QName(qname)` - Extract local name
- `namespace-uri-from-QName(qname)` - Extract namespace URI
- `namespace-uri-for-prefix(prefix, element)` - Resolve prefix to URI
- `in-scope-prefixes(element)` - Get all prefixes in scope

### Document Functions

**Include:** `functions/func_documents.cpp`

- `doc(uri)` - Load external XML document
- `doc-available(uri)` - Test document availability
- `collection([uri])` - Access document collection
- `uri-collection([uri])` - Get URIs in collection
- `document-uri([node])` - Get document URI
- `unparsed-text(uri, [encoding])` - Read text file
- `unparsed-text-available(uri, [encoding])` - Test text availability
- `unparsed-text-lines(uri, [encoding])` - Read text as line sequence

### Accessor Functions

**Include:** `functions/func_accessors.cpp`

- `data([sequence])` - Extract typed values
- `nilled([node])` - Test for nilled element
- `static-base-uri()` - Get static base URI
- `default-collation()` - Get default collation

### Utility Functions

**Include:** `functions/func_diagnostics.cpp`

- `error([code], [description], [object])` - Raise error
- `trace(value, label)` - Trace value with label

## Variable Binding

The XQuery module supports variable binding for parameterised queries.

**Setting Variables (via XML class):**
```cpp
// C++ example
xml->setVariable("threshold", 10.0);
xml->setVariable("category", "fiction");
```

```tiri
-- Tiri example
xml.mtSetVariable('threshold', 10.0)
xml.mtSetVariable('category', 'fiction')
```

**Using Variables in Expressions:**
```xquery
//book[@price < $threshold and @category = $category]
```

**FLWOR Variables:**
Variables defined in `for` and `let` clauses are automatically bound:
```xquery
for $book in //book
let $discount := $book/@price * 0.1
return $discount
```

## Schema-Aware Type System

When an XML document has a loaded schema, the XQuery module can leverage schema type information for type-aware operations.

**Schema Integration:**
- Type-aware comparisons
- Type casting and validation
- Schema type information in predicates
- Enhanced type checking in functions

**Example:**
```cpp
// Load schema into XML object
xml->loadSchema("schema.xsd");

// XQuery can use schema types
xp::Compile(xml, "//book[@price cast as xs:decimal > 10.0]", &query);
```

### Schema Type Constructors

Type constructors (`xs:double("INF")`, `xs:date(fn:current-dateTime())`, etc.) are handled directly in the evaluator via
`evaluate_type_constructor()` so operands are atomised, validated, and canonicalised according to the W3C XQuery 3.0 rules.
Constructor EQNames are resolved through the namespace-aware schema registry (including user-defined simple types) and return
precise static errors:

- `XPST0081` when the constructor prefix is unbound
- `XPST0051` when the referenced type is unknown
- `FORG0001`/`FODT0001`/`FONS0004` when the lexical form is invalid for the requested type

Helper routines in `eval_values.cpp` and `date_time_utils.cpp` share IEEE 754 parsing, timezone canonicalisation, duration
normalisation, and QName namespace binding logic with the public XPath API, guaranteeing consistent results between constructors
and subsequent `cast as`/`treat as` expressions. Regression coverage lives in `src/xquery/tests/test_type_constructors.tiri`.

## Namespace Support

The XQuery module is fully namespace-aware and integrates with the XML module's namespace system.

**Namespace Prefixes:**
```cpp
// Register namespace prefix in XML object
xml->registerNamespace("bk", "http://example.com/books");

// Use in XQuery
xp::Compile(xml, "//bk:book/bk:title", &query);
```

**Default Namespace:**
```xquery
// Access elements in default namespace
//*[local-name()='book']
```

## Performance Considerations

### Expression Compilation

- **Compile Once, Use Many**: Compiled expressions are reusable and thread-safe
- **Compilation Cost**: Upfront cost for complex expressions
- **Memory Management**: Compiled expressions are managed resources

### Memory Management

- **Arena Allocation**: XQuery evaluation uses arena-based memory allocation
- **Node Set Efficiency**: Node sets store pointers, not copies
- **String Operations**: Pre-calculated sizes prevent reallocations

### Optimization Strategies

- Compile expressions outside loops for reuse
- Use specific paths rather than `//` when possible
- Leverage predicates to filter early in evaluation
- Consider using `xp::Query()` with callbacks for large result sets
- Cache compiled expressions for frequently used queries

**Recent Performance Improvements:**
- Centralised dispatch table for expression evaluation with fast-path switch for hot node types
- Flattened arithmetic chains for improved evaluation performance
- Cached operator metadata to reduce runtime lookups
- Tokeniser improvements for faster lexical analysis
- Improved wildcard handling in path expressions

### Function Performance

- String functions pre-calculate buffer sizes
- Numeric functions use optimised algorithms
- Sequence functions minimise allocations
- Regular expression compilation is cached

## Testing Framework

### XQuery Integration Tests

XQuery tests are located in the XQuery module's test directory and exercise XQuery functionality through the XML class interface:

**XQuery Integration Tests (`src/xquery/tests/`):**
- `test_accessor.tiri` - Accessor functions
- `test_advanced.tiri` - Complex XQuery queries
- `test_advanced_paths.tiri` - Advanced path expressions
- `test_axes.tiri` - All 13 XQuery axes
- `test_constructors.tiri` - Node constructors
- `test_core.tiri` - Core XQuery expressions and operators
- `test_datetime.tiri` - Date/time functions
- `test_documents.tiri` - Document functions
- `test_duration.tiri` - Schema duration parsing and validation
- `test_edge_cases.tiri` - Edge case handling and regressions
- `test_flwor.tiri` - FLWOR expressions (for, let, where, order, group, count clauses)
- `test_func_ext.tiri` - Extended function library
- `test_module_loading.tiri` - Module import regression tests
- `test_predicates.tiri` - Predicate evaluation (comprehensive)
- `test_prolog.tiri` - XQuery prolog functionality
- `test_qname.tiri` - QName operations
- `test_qt_math.tiri` - W3C QT3 math function compliance tests
- `test_reserved_words.tiri` - Reserved word handling
- `test_sequence_cardinality.tiri` - Sequence cardinality regression tests
- `test_sequences.tiri` - Sequence operations
- `test_string_uri.tiri` - String and URI functions
- `test_type_constructors.tiri` - Schema type constructors (xs:double, xs:dateTime, xs:duration, xs:QName, etc.)
- `test_type_expr.tiri` - Type expressions (cast, castable, treat-as, instance-of, typeswitch, to-range)
- *(Optional)* `test_unit_tests.tiri` - Runs compiled-in internal unit tests when `-DENABLE_UNIT_TESTS=ON`

**Test Modules (`src/xquery/tests/modules/`):**
The `modules/` subdirectory contains XQuery library modules used for testing module loading functionality:
- `math_utils.xq` - Library module with math functions
- `string_utils.xq` - Library module with string utilities
- `composite.xq` - Composite module that imports other modules
- `main_module.xq` - Module entry point for testing
- `bad_namespace.xq` - Error case: invalid namespace declaration
- `circular_a.xq`, `circular_b.xq` - Error case: circular module dependencies
- `self_reference.xq` - Error case: module importing itself

### Running XQuery Tests

**Individual Test:**
```bash
cd src/xquery/tests && ../../../build/agents-install/origo.exe ../../../tools/flute.tiri file=/[path]/src/xquery/tests/test_core.tiri --log-warning
```

**All XQuery Tests via CMake:**
```bash
ctest --build-config [BuildType] --test-dir build/agents -L xquery --output-on-failure
```

### C++ Unit Testing for Internal Components

The XQuery module includes a compiled-in unit testing framework for testing internal components that are not easily accessible through the Tiri interface. This is particularly useful for debugging low-level functionality like XQuery prolog integration, parser internals, and data structure integrity.

Unit tests will only be compiled in the module if ENABLE_UNIT_TESTS is enabled in the module's CMakeLists.txt file.

**Unit Test Infrastructure:**

The module exposes a `xq::UnitTest()` function that can be called to run compiled-in unit tests. This function is defined in:
- **Implementation**: `src/xquery/unit_tests.cpp` - Contains all unit test suites
- **Test Runner**: `src/xquery/tests/test_unit_tests.tiri` - Calls the unit test function

**Creating Unit Tests:**

To add new C++ unit tests for internal XQuery components:

1. Add test functions to `unit_tests.cpp`
2. Register the test suite in `UnitTest()`
3. Build and run the tests

**Example Unit Test Structure:**

The current implementation includes tests for XQuery prolog functionality:

```cpp
// Test XQueryProlog API
static void test_prolog_api() {
   // Test function declaration
   XQueryProlog prolog;
   XQueryFunction func;
   func.qname = "local:test";
   func.parameter_names.push_back("x");
   prolog.declare_function(std::move(func));

   auto found = prolog.find_function("local:test", 1);
   test_assert(found not_eq nullptr, "Function declaration",
      "Declared function should be findable");
}
```

**Best Practices for C++ Unit Tests:**

- Test internal data structures and APIs not exposed to Tiri
- Use unit tests for debugging complex integration issues
- Keep tests focused on specific functionality
- Use descriptive test names for easy identification
- Include both positive and negative test cases
- Verify edge cases and boundary conditions
- Test error handling and invalid inputs

**When to Use C++ Unit Tests vs Tiri Tests:**

- **C++ Unit Tests**: Internal APIs, data structures, parser internals, performance-critical code, debugging integration issues
- **Tiri Tests**: End-to-end functionality, user-facing features, XQuery expression evaluation, integration with XML module

This dual testing approach ensures comprehensive coverage at both the internal implementation level and the user-facing API level.

## Common Usage Patterns

### Basic Query with Compilation

```cpp
// C++ example
#include <kotuku/modules/xml.h>
#include <kotuku/modules/xquery.h>

if (auto xml = objXML::create { fl::Path("document.xml") }; xml.ok()) {
   XPathParseResult *query;
   if (xp::Compile(*xml, "//book[@price < 20]/title", &query) IS ERR::Okay) {
      XPathValue *result;
      if (xp::Evaluate(*xml, query, &result) IS ERR::Okay) {
         // Process result node set
         for (auto *node : result->node_set) {
            log.msg("Found: %s", node->name());
         }
         FreeResource(result);
      }
      FreeResource(query);
   }
}
```

### Callback-Based Node Iteration

```cpp
// C++ example - process each matching node
static ERR process_book(objXML *XML, int TagID, CSTRING Attrib) {
   XTag *tag;
   if (XML->getTag(TagID, &tag) IS ERR::Okay) {
      log.msg("Processing book: %s", tag->getContent().c_str());
   }
   return ERR::Okay;
}

XPathParseResult *query;
if (xp::Compile(xml, "//book[@category='fiction']", &query) IS ERR::Okay) {
   FUNCTION callback = C_FUNCTION(process_book);
   xp::Query(xml, query, &callback);
   FreeResource(query);
}
```

### FLWOR Query

```tiri
-- Tiri example
local xml = obj.new('xml', { path = 'books.xml' })

-- Compile FLWOR expression
local query = 'for $book in //book ' ..
              'where $book/@price < 20 ' ..
              'order by $book/@price ' ..
              'return $book/title'

local err, matches = xml.mtSearch(query, function(XML, TagID)
   local err, tag = XML.mtGetTag(TagID)
   print('Title: ' .. tag.content)
end)
```

### Variable Binding

```cpp
// C++ example with variables
xml->setVariable("minPrice", 10.0);
xml->setVariable("maxPrice", 50.0);
xml->setVariable("category", "fiction");

XPathParseResult *query;
xp::Compile(xml, "//book[@price >= $minPrice and @price <= $maxPrice "
                 "and @category = $category]", &query);

XPathValue *result;
xp::Evaluate(xml, query, &result);
// Process results...
FreeResource(result);
FreeResource(query);
```

### Custom Function Registration

The `RegisterFunction()` method allows extending XQuery with custom function implementations:

```cpp
// C++ example - Register a custom function
static ERR custom_double(objXQuery *Query, std::string_view FunctionName,
                        const std::vector<XPathValue> &Input,
                        XPathValue &Result, APTR Meta) {
   if (Input.empty()) return ERR::Args;

   Result.Type = XPVT::Number;
   Result.NumberValue = Input[0].NumberValue * 2.0;
   return ERR::Okay;
}

if (auto xquery = objXQuery::create()) {
   FUNCTION callback = C_FUNCTION(custom_double);
   xquery->registerFunction("custom-double", &callback);

   // Use the custom function in XQuery
   XPathValue *result;
   xquery->evaluate("custom-double(21)", &result);
   // result->NumberValue will be 42.0
   FreeResource(result);
}
```

```tiri
-- Tiri example - Register and use custom function
local xquery = obj.new('xquery')

xquery.mtRegisterFunction('custom-sum', function(query, name, input, result)
   local sum = 0
   for _, val in ipairs(input) do
      sum = sum + val.number
   end
   result.type = 'number'
   result.number = sum
   return ERR_Okay
end)

local err, result = xquery.mtEvaluate('custom-sum(1, 2, 3, 4, 5)')
print('Sum:', result.number) -- Prints: Sum: 15
```

## Error Handling

### Common Error Conditions

- `ERR::Syntax` - XQuery expression syntax error
- `ERR::Search` - XQuery query matched no nodes (Query function only)
- `ERR::NoData` - XML document is empty
- `ERR::NullArgs` - Required parameter is NULL
- `ERR::NoSupport` - Unsupported XQuery feature
- `ERR::AllocMemory` - Memory allocation failure

### Best Practices

- Always check return codes from XQuery functions
- Use `xml->ErrorMsg` field for detailed error diagnostics after compilation failures
- Enable appropriate logging levels (`--log-warning` or `--log-api`)
- Validate XQuery expressions during development
- Handle `ERR::Search` appropriately (it indicates no matches, not a failure)

## Development Guidelines

### Build Integration

**Module Target:**
```bash
cmake --build build/agents --config [BuildType] --target xquery --parallel
```

**Dependency Note:**
The XQuery module depends on the XML module for document structures and the schema system. Always build XML before XQuery in dependency order.

## Integration Points

### With XML Module

The XQuery module has a tight integration with the XML module:
- Operates on `XTag` structures from XML documents
- Uses XML namespace resolution
- Leverages XML schema type system
- Accesses XML document metadata

### With Other Modules

- **Core Module**: Uses Core's object system and memory management
- **Unicode Module**: For text encoding and string operations
- **Regex Module**: Dynamically loaded for pattern matching functions

## Advanced Features

### Custom Function Registration

The XQuery function library is extensible through the `RegisterFunction()` method, allowing custom function registration for specialised processing needs. Custom functions can be implemented in C++ or Tiri and are callable from XQuery expressions just like built-in functions.

### Expression Caching

Compiled expressions are expensive to create but cheap to reuse. Consider caching strategies for frequently used expressions.

### Streaming Evaluation

Use the `xp::Query()` function with callbacks for streaming evaluation of large result sets, avoiding memory overhead of materialising all results.

### Evaluator Architecture

For developers modifying or extending the XQuery evaluator, see `src/xquery/eval/AGENTS.md` for detailed documentation on:
- Expression evaluation dispatch mechanism
- Handler method conventions
- Performance considerations for hot paths
- Testing procedures for evaluator changes

## Related Documentation

- **XML Module**: See `src/xml/AGENTS.md` for XML document handling
- **XQuery 2.0 Specification**: W3C XQuery 2.0 Recommendation
- **XQuery Specification**: W3C XQuery 1.0 Recommendation
- **API Reference**: See `docs/xml/modules/xquery.xml` for complete API documentation

This guide provides the essential information needed for AI agents to work effectively with the Kōtuku XQuery module, covering architecture, language support, function library, and integration patterns.

Last Updated: 2025-11-16
