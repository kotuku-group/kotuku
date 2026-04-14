# XML Module - AI Agent Guide

This file provides comprehensive information about the Kōtuku XML module for AI agents working with the codebase.

## Overview

The XML module provides robust functionality for creating, parsing, and maintaining XML data structures. It serves as Kōtuku's general-purpose structured data handler, supporting XML, JSON, YAML, and other structured formats with cross-format conversion capabilities.

**Key Features:**
- Full XML parsing and serialisation with flexible parsing modes
- XML Schema (XSD) validation and type system integration
- Namespace processing and management
- Document validation and well-formedness checking
- In-memory XML tree manipulation
- Cross-format structured data conversion
- Thread-safe operations with C++20 features
- Integration with the XPath module for querying and navigation

## Module Architecture

### Core Files Structure

```
src/xml/
├── xml.tdl              # Interface definition (classes, enums, structs)
├── xml.cpp              # Main XML module implementation
├── xml.h                # Internal header definitions
├── xml_def.c            # Generated C module definitions
├── xml_class.cpp        # XML class implementation
├── xml_class_def.c      # Generated C class definitions
├── xml_functions.cpp    # XML manipulation functions
├── unescape.cpp         # HTML/XML entity handling
├── unescape.h           # Entity handling header
├── uri_utils.h          # URI utility functions
├── xpath_value.cpp      # XPath value type implementation (shared with XPath module)
├── xpath_value.h        # XPath value type definitions
├── schema/              # XML Schema validation system
│   ├── schema_parser.cpp/h       # XSD schema parsing
│   ├── schema_types.cpp/h        # Schema type system and validation
│   └── type_checker.cpp/h        # Type validation logic
└── tests/               # XML-specific test suite
    ├── test_basic.tiri                # Basic XML operations
    ├── test_advanced_features.tiri    # Complex parsing scenarios
    ├── test_schema_validation.tiri    # XML Schema validation tests
    ├── test_xml_parsing.tiri          # Parser robustness tests
    ├── test_xml_manipulation.tiri     # Content modification tests
    ├── test_data_sources.tiri         # Multiple input source handling
    ├── test_namespaces.tiri           # Namespace processing
    ├── test_setvariable.tiri          # Variable binding
    ├── test_error_handling.tiri       # Error condition handling
    └── benchmark.tiri                 # Performance measurement
```

### Dependencies

- **unicode** module (PUBLIC) - For text encoding/decoding
- **xpath** module (for XPath query functionality) - Used via XML class methods

## Core Classes and Structures

### XML Class (`objXML`)

Primary class for XML document handling with comprehensive parsing and manipulation capabilities.

**Include:** `<kotuku/modules/xml.h>`

**Key Fields:**
- `Path` (str) - File system location of XML data
- `Source` (obj) - Alternative object-based data source
- `Flags` (XMF) - Parsing and behavior flags
- `Start` (int) - Starting cursor position for operations
- `Modified` (int) - Modification timestamp
- `Tags` (TAGS) - Hierarchical array of XTag structures

**Critical Implementation Notes:**
- C++ developers get direct access to `Tags` field as `pf::vector<XTag>`
- Tiri developers should cache `Tags` reads as they create full copies
- Thread-safe due to object locking principles.

### XTag Structure

Represents complete XML elements with attributes, content, and hierarchy.

**Include:** `<kotuku/modules/xml.h>`

```cpp
struct XTag {
   int ID;                             // Unique tag identifier
   int ParentID;                       // Parent tag ID (0 for root)
   int LineNo;                         // Source line number
   XTF Flags;                          // Tag flags (CDATA, INSTRUCTION, etc.)
   uint NamespaceID;                   // Namespace URI hash
   pf::vector<XMLAttrib> Attribs;      // Attributes array
   pf::vector<XTag> Children;        // Child elements array
}
```

**Important Methods:**
- `name()` - Returns tag name (first attribute)
- `hasContent()` - Checks for text content
- `isContent()` - Determines if this is content node
- `getContent()` - Extracts all text content
- `attrib(name)` - Gets attribute value by name

### XMLAttrib Structure

Simple name-value pair for XML attributes and content.

**Include:** `<kotuku/modules/xml.h>`

```cpp
struct XMLAttrib {
   std::string Name;   // Attribute name (empty for content)
   std::string Value;  // Attribute/content value
}
```

### Key Enumerations and Flags

**XMF Flags** - XML parsing and behavior options
**Include:** `<kotuku/modules/xml.h>`
- `WELL_FORMED` - Require well-formed XML structure
- `INCLUDE_COMMENTS` - Preserve XML comments in parsing
- `STRIP_CONTENT` - Remove all text content during parsing
- `READABLE/INDENT` - Format output with indentation
- `NAMESPACE_AWARE` - Enable namespace processing
- `PARSE_HTML` - Handle HTML escape codes
- `INCLUDE_WHITESPACE` - Retain whitespace between tags

**XTF Flags** - XTag type indicators
**Include:** `<kotuku/modules/xml.h>`
- `CDATA` - Tag represents CDATA section
- `INSTRUCTION` - Processing instruction (<?xml?>)
- `NOTATION` - Notation declaration (<!XML>)
- `COMMENT` - Comment section (<!-- -->)

**XMI Enum** - Tag insertion positions
**Include:** `<kotuku/modules/xml.h>`
- `PREV/PREVIOUS` - Insert before target tag
- `CHILD` - Insert as first child
- `NEXT` - Insert after target tag
- `CHILD_END` - Insert as last child

## Core Functionality

### Document Loading and Parsing

**Multiple Input Methods:**
- `Path` field - File system sources with automatic caching
- `Statement` field - Direct XML string parsing
- `Source` field - Object-based input (any object with Read action)

**Parsing Behaviors:**
- Default: Accepts loosely structured XML
- `XMF::WELL_FORMED` - Requires well-formed XML structure
- `XMF::INCLUDE_COMMENTS` - Preserves XML comments
- `XMF::STRIP_CONTENT` - Removes all text content
- `XMF::NAMESPACE_AWARE` - Enables namespace processing

### XML Schema Validation

The XML module includes comprehensive XML Schema (XSD) validation support for ensuring document conformance to schema definitions.

**Include:** `src/xml/schema/schema_types.h` (internal), access via XML class methods

**Supported Features:**
- Schema parsing from XSD files
- Type validation (built-in and derived types)
- Element and attribute validation
- Namespace-aware schema processing
- Detailed validation error diagnostics
- Schema-aware XPath type comparisons (via XPath module integration)

**Key Methods (objXML):**
- `LoadSchema()` - Load and parse XSD schema definition
- `ValidateDocument()` - Validate XML document against loaded schema
- Schema validation integrates with namespace processing

**Validation Capabilities:**
- Built-in schema types (string, integer, boolean, date, etc.)
- Complex type definitions with sequences and choices
- Attribute presence and type validation
- Element cardinality validation (minOccurs, maxOccurs)
- Namespace-qualified element validation

### XPath Integration

The XML module provides seamless integration with the XPath module for querying and navigating XML documents. XPath functionality is accessed through XML class methods that delegate to the XPath module.

**Key Methods (objXML):**
- `FindTag()` - Find tags matching an XPath expression with callback support
- `InsertXPath()` - Insert XML content at positions specified by XPath
- `RemoveXPath()` - Remove tags matching XPath expressions
- `SetVariable()` - Set XPath variables for parameterised queries

**XPath Value Type:**
The `XPathValue` structure is defined in the XML module and shared with the XPath module to represent query results (node sets, strings, numbers, booleans, dates, etc.).

For detailed XPath functionality, see the XPath module documentation at `src/xpath/AGENTS.md`.

### Content Manipulation

**Core Methods (all in objXML):**
- `InsertXML()` - Insert XML content at specific positions
- `InsertXPath()` - Insert based on XPath expressions
- `RemoveTag()` - Remove specific tags
- `RemoveXPath()` - Remove tags matching XPath
- `SetAttrib()` - Modify/create attributes
- `GetAttrib()` - Retrieve attribute values
- `MoveTags()` - Relocate tags within document
- `Sort()` - Sort elements by attributes

### Namespace Support

**Namespace Management (objXML methods):**
- `RegisterNamespace()` - Define namespace prefixes
- `GetNamespaceURI()` - Resolve namespace URIs
- `SetTagNamespace()` - Assign namespaces to tags
- `ResolvePrefix()` - Convert prefixes to URIs

### Utility Functions

**XML Namespace Functions**
**Include:** `<kotuku/modules/xml.h>` (xml namespace)

```cpp
// Direct tag attribute manipulation
void UpdateAttrib(XTag &Tag, const std::string Name, const std::string Value, bool CanCreate = false)
void NewAttrib(XTag &Tag, const std::string Name, const std::string Value)
std::string GetContent(const XTag &Tag)

// Process all attributes in XML tree
void ForEachAttrib(objXML::TAGS &Tags, std::function<void(XMLAttrib &)> &Function)
```

## Testing Framework

### Test Organization

Comprehensive test suite using Flute test runner covering all module features:

**Core XML Tests (src/xml/tests/):**
- `test_basic.tiri` - Core XML operations and tag access
- `test_advanced_features.tiri` - Complex parsing scenarios
- `test_xml_parsing.tiri` - Parser robustness tests
- `test_xml_manipulation.tiri` - Content modification
- `test_data_sources.tiri` - Multiple input source handling
- `test_namespaces.tiri` - Namespace processing
- `test_setvariable.tiri` - Variable binding for XPath queries
- `test_error_handling.tiri` - Error condition handling
- `test_schema_validation.tiri` - XML Schema validation and type checking
- `benchmark.tiri` - Performance measurement and optimization

**XPath Integration Tests (src/xpath/tests/):**
XPath tests have been moved to the separate XPath module. See `src/xpath/AGENTS.md` for details on XPath-specific tests including:
- `test_core.tiri` - XPath integration with XML class
- `test_predicates.tiri` - Predicate evaluation
- `test_axes.tiri` - XPath axes navigation
- `test_advanced.tiri` - Complex XPath queries
- `test_flwor.tiri` / `test_flwor_clauses.tiri` - FLWOR expressions
- `test_func_ext.tiri` - Extended function library
- And many more XPath-specific test files

### Running Tests

**Individual Test:**
```bash
cd src/xml/tests && ../../../install/agents/origo.exe ../../../tools/flute.tiri file=E:/parasol/src/xml/tests/test_basic.tiri --gfx-driver=headless --log-warning
```

**All XML Tests via CMake:**
```bash
ctest --build-config [BuildType] --test-dir build/agents -R xml_
```

## Common Usage Patterns

### Basic XML Parsing

```tiri
local xml = obj.new('xml', { path = 'document.xml' })
local tags = xml.tags
for i, tag in ipairs(tags) do
   if tag.name == 'target_element' then
      local content = tag.content
      -- Process content
   end
end
```

### XPath Queries - Multiple Matches

```tiri
-- XPath queries are handled through the XML class methods
local err, index = xml.mtFindTag('//book[@category="fiction"]',  function(XML, TagID, Attrib)
   local err, tag = XML.mtGetTag(TagID)
   -- Process matching tags
end)
```

**Note:** For detailed XPath usage patterns and advanced querying, see the XPath module documentation.

### XML Schema Validation

```tiri
-- Load schema definition
local xml = obj.new('xml', { path = 'document.xml', flags = '!NAMESPACE_AWARE' })
local err = xml.mtLoadSchema('schema.xsd')
if err == ERR_Okay then
   -- Validate document against schema
   err = xml.mtValidateDocument()
   if err == ERR_Okay then
      print('Document is valid')
   else
      print('Validation failed: ' .. mSys.GetErrorMsg(err))
   end
end
```

### Content Manipulation

```cpp
for (auto &tag : xml->Tags) {
   if (tag.name() IS "target") {
      xml::UpdateAttrib(tag, "modified", "true", true);
   }
}
```

## Development Guidelines

### Build Integration

**Module Target:**
```bash
cmake --build build/agents --config [BuildType] --target xml --parallel
```


### Schema System Architecture

The XML Schema validation system is integrated into the module with these components:

**Schema Parser** (`schema/schema_parser.cpp/h`):
- Parses XSD schema definitions
- Builds internal schema type registry
- Handles namespace-qualified schema elements

**Type System** (`schema/schema_types.cpp/h`):
- Defines built-in and derived schema types
- Provides type validation interface
- Supports complex type definitions

**Type Checker** (`schema/type_checker.cpp/h`):
- Validates element and attribute values against schema types
- Checks element cardinality constraints
- Reports detailed validation errors

## Performance Considerations

### Memory Management

- **Tags Array**: Direct C++ access avoids copying, Tiri access creates copies
- **Content Extraction**: Pre-calculated string sizes prevent reallocations
- **Namespace Hashing**: URI hashes for fast namespace lookups
- **Schema Validation**: Schema definitions cached after initial load

### Optimization Strategies

- Cache `Tags` reads in Tiri scripts
- Use specific XPath expressions rather than broad queries (see XPath module for optimization)
- Enable namespace processing only when required (`XMF::NAMESPACE_AWARE`)
- Consider `XMF::STRIP_CONTENT` for metadata-only operations
- Load schemas once and reuse XML objects for multiple validations
- Schema validation requires namespace awareness for qualified elements

## Error Handling

### Common Error Conditions

- `ERR::BadData` - Malformed XML when `XMF::WELL_FORMED` enabled
- `ERR::Search` - XPath expression matches no nodes
- `ERR::InvalidPath` - Invalid file path in `Path` field
- `ERR::NoSupport` - Unsupported XPath feature
- `ERR::Args` - Invalid arguments to XML methods
- `ERR::Failed` - Schema validation failure (invalid document structure or types)
- `ERR::FileNotFound` - Schema file not found during `LoadSchema()`

### Best Practices

- Always check return codes from XML methods
- Use `xml->ParseError` field for detailed parsing diagnostics
- Enable appropriate logging levels (`--log-warning` minimum)
- Validate XPath expressions before batch processing
- Enable `XMF::NAMESPACE_AWARE` when using schema validation
- Schema validation errors are logged with detailed diagnostics
- Use `ValidateDocument()` after successful `LoadSchema()`

## Advanced Features

### Schema-Aware XPath

The XPath module integrates with the XML module's schema validation system to provide type-aware comparisons and operations. When a schema is loaded into an XML object, XPath expressions can leverage schema type information for more accurate evaluations.

### Variable Binding

XPath variable binding is supported through the XML class `SetVariable()` method, enabling parameterised XPath queries. Variables can be referenced in XPath expressions using the `$variable` syntax. See the XPath module documentation for details.

### Cross-Format Support

While primarily XML-focused, the module architecture supports extension to other structured data formats (JSON, YAML) through the same tag-based representation.

## Integration Points

### With Other Modules

- **XPath Module**: Provides XPath 2.0 and XQuery functionality for XML querying and navigation
- **Document Module**: XML provides structured data for RIPL document processing
- **SVG Module**: XML parsing serves as foundation for SVG document handling
- **Core Module**: Utilizes Core's file system and object management
- **Tiri Module**: Provides scripted access to all XML functionality

### External Dependencies

- Unicode module for text encoding/decoding operations
- Standard C++ libraries (C++20 features utilized throughout)

## Related Documentation

- **XPath Module**: See `src/xpath/AGENTS.md` for detailed XPath 2.0 and XQuery functionality
- **Schema Validation**: Detailed schema validation information in this document
- **API Reference**: See `docs/xml/modules/classes/xml.xml` for complete API documentation

This guide provides the essential information needed for AI agents to work effectively with the Kōtuku XML module, covering architecture, functionality, testing, and integration patterns.