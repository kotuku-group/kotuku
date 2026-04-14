# Documentation Guide for Claude Sessions

This guide describes the comprehensive documentation available in the `docs/` directories to help Claude sessions navigate and utilise Kōtuku documentation effectively.

## Documentation Structure Overview

Kōtuku maintains three parallel documentation systems in the `docs/` directory:

- **`docs/wiki/`** - Community-oriented guides and tutorials (Markdown source)
- **`docs/html/`** - Complete website with API references and galleries (HTML output)
- **`docs/xml/`** - Machine-generated API documentation from source code (XML format)

## 📚 docs/wiki/ - Community Guides and Tutorials

**Location:** `docs/wiki/` (source) and `docs/html/wiki/` (rendered HTML)

The wiki contains practical, community-oriented documentation covering:

### Build and Setup
- **`Linux-Builds.md`** - Linux compilation instructions
- **`Windows-Builds.md`** - Windows compilation instructions
- **`Customising-Your-Build.md`** - Build configuration options

### Core Concepts
- **`Home.md`** - Main wiki landing page with navigation
- **`Kotuku-Objects.md`** - Object system fundamentals
- **`Kotuku-In-Depth.md`** - Advanced framework concepts

### Tiri Scripting Documentation
- **`Tiri-Reference-Manual.md`** - Core Tiri language reference
- **`Tiri-GUI-API.md`** - GUI toolkit APIs and constants
- **`Tiri-JSON-API.md`** - JSON processing utilities
- **`Tiri-VFX-API.md`** - Visual effects and animation APIs
- **`Widgets.md`** - Widget system documentation

### RIPL Document Engine
- **`RIPL-Reference-Manual.md`** - Rich text document formatting system

### Development Tools
- **`Unit-Testing.md`** - Flute test documentation
- **`Origo.md`** - Command-line tool usage
- **`TDL-Tools.md`** - Interface Definition Language tools
- **`TDL-Reference-Manual.md`** - TDL syntax and usage

### Reference Materials
- **`Action-Reference-Manual.md`** - Object action documentation
- **`System-Error-Codes.md`** - Error code reference
- **`Embedded-Document-Formatting.md`** - Documentation markup standards

**Key Usage Notes:**
- These are practical, tutorial-style documents
- Written for developers learning the framework
- Complement the technical API documentation
- Include working examples and best practices

## 🌐 docs/html/ - Complete Website Documentation

**Location:** `docs/html/` (complete self-contained website)

The HTML documentation provides a fully browsable website experience:

### Main Sections
- **`index.html`** - Kōtuku homepage and overview
- **`gallery.html`** - Visual showcase of Kōtuku capabilities
- **`modules/api.html`** - API documentation landing page

### API Documentation Structure
- **`modules/`** - Module-level documentation
  - `core.html`, `vector.html`, `display.html`, `network.html`, etc.
- **`modules/classes/`** - Individual class documentation (70+ classes)
  - `vector.html`, `surface.html`, `file.html`, `bitmap.html`, etc.
  - Each class page includes methods, fields, actions, and examples

### Gallery Assets
- **`gallery/`** - Screenshots and demonstrations

### Web Infrastructure
- **`css/`** - Bootstrap-based styling
- **`js/`** - Interactive functionality
- **`images/`** - Logos and icons

**Key Usage Notes:**
- Complete offline browsable documentation
- Generated from XML source via XSLT transforms
- Includes visual examples and galleries
- Cross-referenced with hyperlinks between classes and modules

## 📄 docs/xml/ - Machine-Generated API Documentation

**Location:** `docs/xml/` (XML source files)

Raw structured documentation extracted directly from C++ source code:

### Module Documentation
- **`modules/core.xml`** - Core system documentation
- **`modules/vector.xml`** - Vector graphics module
- **`modules/display.xml`** - Display management module
- **`modules/network.xml`** - Network communications module
- And others for each Kōtuku module

### Class Documentation
- **`modules/classes/`** - Individual class XML files
  - **`vector.xml`** - Abstract vector graphics base class
  - **`surface.xml`** - Display surface management
  - **`file.xml`** - File system operations
  - **`bitmap.xml`** - Image processing
  - 70+ additional class files covering all Kōtuku functionality

### Structure and Content
Each XML file contains:
- **Class metadata** - Name, module, version, description
- **Actions** - Available operations (Draw, Read, Write, etc.)
- **Methods** - Public method interfaces and parameters
- **Fields** - Object properties and their types
- **Constants** - Enumeration values and flags
- **Source references** - Links to implementation files

**Key Usage Notes:**
- Authoritative technical reference
- Generated directly from C++ source via parsing
- Structured data suitable for programmatic processing
- Used to generate the HTML documentation

## 🤖 AI-Optimized Documentation System

**Location:** `docs/xml/ai/` files and `tools/docgen-ai.tiri`

For Claude Code sessions, a specialized condensed documentation format is available:

### AI Documentation Files
- **`ai-condensed.xsd`** - XML Schema for the condensed format
- **`ai-condense.xsl`** - XSLT transformation from full XML to condensed format
- **`../tools/docgen-ai.tiri`** - Build script for AI documentation

### Condensed Format Features
The AI documentation system creates ultra-compact XML files optimized for AI processing:

- **95%+ size reduction** from original XML files
- **Complete API coverage** - all modules, classes, methods, fields
- **Essential information preserved** - signatures, types, constants, errors
- **Abbreviated element names** - `<m>` for modules, `<cl>` for classes, etc.
- **Short comments only** - verbose descriptions removed
- **Machine-readable structure** - consistent XML format

### Generation and Usage
```bash
# Generate AI-optimized documentation
origo tools/docgen-ai.tiri

# Custom output location
origo tools/docgen-ai.tiri output=my-docs.xml
```

The generated files in `docs/xml/ai/modules/` and `docs/xml/ai/classes/` provide comprehensive API reference for AI assistants while consuming minimal context window space.

## 📖 How to Use This Documentation Effectively

### For Understanding APIs
1. **Start with wiki guides** for conceptual understanding
4. **Load AI documentation** for comprehensive API coverage in AI context
3. **Check XML files** for extended commentary and examples

### For Tiri Scripting
1. **`Tiri-Reference-Manual.md`** for language fundamentals
2. **`Tiri-GUI-API.md`** for user interface development
3. **XML class pages** for specific object APIs
4. **Examples in `examples/`** for practical patterns

### For C++ Development
1. **`Kotuku-Objects.md`** for object system concepts
2. **XML class documentation** for precise API specifications
3. **`TDL-Reference-Manual.md`** for how to write TDL interface definitions
4. **Source code in `src/`** for implementation details

### For Testing and Build
1. **`Unit-Testing.md`** for Flute test framework
2. **Build guide matching your platform** (Linux/Windows)
3. **`Customising-Your-Build.md`** for configuration options

## 🔄 Documentation Generation Process

The documentation follows this generation pipeline:

1. **C++ Source** → **XML** (via TDL parsing)
2. **XML** → **HTML** (via XSLT transformation)
3. **XML** → **AI-optimized XML** (via condensed transformation)
4. **Markdown** → **HTML** (via documentation generator)

Key scripts:

- **`tools/docgen.tiri`** - Main documentation generator
- **`tools/docgen-wiki.tiri`** - Wiki-specific processing
- **`tools/docgen-ai.tiri`** - AI documentation generator

## ✍️ Writing Style Guide

When writing or editing documentation in `docs/wiki/`, follow these conventions to maintain consistency with the established style.

### Voice and Tone

- **Use first-person plural ("we/our")** to create a collaborative, project-insider feel.  Write as an experienced developer
  explaining concepts to a peer.  Avoid second-person instructional voice ("you should...") except where directly
  addressing the reader's actions.
- **Be direct and declarative.**  State facts without hedging or unnecessary qualifiers.  Prefer *"Kōtuku supports
  threads"* over *"It should be noted that Kōtuku has support for threads"*.
- **Maintain a professional register.**  No jokes, exclamation marks, casual asides, or emojis.  The closest to
  conversational should be phrases like *"Note that..."* or *"It is worth considering..."*.

### Language and Spelling

- **British English** throughout: *initialised*, *behaviour*, *colour*, *customised*, *utilise*, *recognised*,
  *centre*, *modelling*.
- **Two spaces** after full stops (sentence-ending periods), consistent with the existing convention.
- **Active voice** is strongly preferred.  Write *"The Core API loads a module"* rather than *"A module is loaded by
  the Core API"*.

### Sentence Structure

- Keep sentences moderate in length, typically 15-30 words.
- Break complex technical concepts across multiple shorter sentences rather than packing them into long compound
  constructions.
- Use parenthetical asides sparingly and keep them brief.

### Document Structure

- **Opening paragraph:** One to three sentences summarising what the page covers, optionally setting expectations for
  assumed knowledge.
- **Table of Contents:** Include for longer documents, using numbered or bulleted lists with anchor links.
- **Horizontal rules (`---`):** Use to separate major sections.
- **Heading hierarchy:** H2 (`##`) for major sections, H3 (`###`) for subsections, H4 (`####`) for sub-subsections.
  Reserve H1 (`#`) for the page title or top-level concept only.
- **Maximum line width:** 120 characters, consistent with the project's code formatting standard.

### Formatting Conventions

- **Italics** for introducing new terminology on first use, e.g. *"local"*, *"sub-classing"*, *"base-class"*.
- **Bold** sparingly for emphasis on key points.  Do not overuse.
- **Inline code** (backticks) for function names, field names, type names, file names, and code fragments within prose.
- **Code blocks** should immediately follow a brief prose introduction.  Show, then explain.
- **Tables** for structured reference data such as parameters, options, flags, and field definitions.  Prefer tables
  over prose lists for this type of content.

### API Reference Sections

When documenting functions or methods, follow this consistent pattern:

1. **Heading:** `### functionName()` or `### module.functionName()`
2. **Signature:** As inline code on its own line, e.g. `` `result = module.functionName(Param1, Param2)` ``
3. **Prose description:** One or more paragraphs explaining behaviour.
4. **Code example:** A practical demonstration in a fenced code block.
5. **Parameter table:** If the function accepts options or has multiple parameters, present them in a markdown table.

### Cross-Referencing

- Link to other wiki pages using relative markdown links: `[Page Title](./Page-Name.md)`.
- Introduce links naturally in prose, e.g. *"further discussed in the [TDL Tools](./TDL-Tools.md) manual"*.
- Avoid bare URLs in body text.

### Code Examples

- Introduce concepts briefly in prose, then show the code, then add follow-up notes on specifics.
- Use fenced code blocks with appropriate language hints (e.g. ` ```cpp `, ` ```lua `).
- Examples should be realistic and functional, not abstract pseudocode.
- Keep examples concise; include only what is necessary to demonstrate the concept.

## 💡 Best Practices for Claude Sessions

### When Researching Kōtuku Concepts
- Start with relevant wiki pages for conceptual understanding
- Load AI documentation for complete API coverage
- Reference XML for detailed technical specifications
- **NEVER** read documentation from `docs/html` when writing code, use the XML documentation instead.

### When Writing Code
- Check existing examples in `examples/` directory first
- Use API documentation to understand object interfaces
- Follow patterns established in wiki tutorials
- Reference condensed AI docs for quick API lookup
- If there are discrepencies between documentation and code, bring it to the user's attention

### When Debugging Issues
- Consult error codes in `docs/wiki/System-Error-Codes.md`
- Review action documentation for correct usage patterns
- Check class field documentation for property requirements

### When Working with Tiri Scripts
- Always study existing `.tiri` files for patterns
- Use GUI API documentation for interface constants
- Reference class XML pages for object method signatures
- Use AI documentation for complete method/field reference

### For AI Documentation Maintenance
The AI documentation should be regenerated when:
- New classes or modules are added
- API signatures change in existing code
- Field definitions or access patterns are modified
- Constants or enumerations are updated

This documentation ecosystem provides comprehensive coverage from high-level concepts to low-level implementation details, with specialized AI-optimized formats for efficient context usage, enabling effective development with the Kōtuku framework.
