# Documentation Guide for Agents

This guide describes the comprehensive documentation available in the `docs/` directories to help AI agents navigate and utilise Kōtuku documentation effectively.

## Documentation Structure Overview

Kōtuku maintains four parallel documentation systems in the `docs/` directory:

- **`docs/wiki/`** - Community-oriented guides and tutorials (Markdown source)
- **`docs/manuals/`** - Long-form AsciiDoc manuals and their generated PDFs
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
- **`_Sidebar.md`** - Wiki navigation sidebar
- **`Kotuku-Objects.md`** - Object system fundamentals
- **`Kotuku-In-Depth.md`** - Advanced framework concepts
- **`Kotuku-Design-Patterns.md`** - Recurring design patterns across the framework
- **`Coding-With-AI.md`** - Guidance for AI-assisted development against Kōtuku

### Tiri Scripting Documentation
- **`Tiri-Reference-Manual.md`** - Core Tiri language reference
- **`Tiri-Defer-Syntax.md`** - Deferred execution syntax
- **`Tiri-GUI-API.md`** - GUI toolkit APIs and constants
- **`Tiri-Widgets.md`** - Widget system documentation
- **`Tiri-VFX-API.md`** - Visual effects and animation APIs
- **`Tiri-IO-API.md`** - File and stream I/O
- **`Tiri-JSON-API.md`** - JSON processing utilities
- **`Tiri-URL-API.md`** - URL parsing and construction
- **`Tiri-HTTP-Server-API.md`** - HTTP server interfaces
- **`Tiri-Proxy-Server-API.md`** - Proxy server interfaces
- **`Tiri-OAuth-API.md`** - OAuth authentication flows
- **`Tiri-Config-API.md`** - Configuration file handling
- **`Tiri-Options-API.md`** - Command-line option parsing
- **`Tiri-Tempus-API.md`** - Date and time handling

### Document and Text Engines
- **`RIPL-Reference-Manual.md`** - Rich text document formatting system
- **`Regex-Manual.md`** - Regular expression support
- **`XML-Comparisons.md`** - Comparison of the XML processing approaches

### Development Tools
- **`Unit-Testing.md`** - Flute test documentation
- **`Origo.md`** - Command-line tool usage
- **`Tuku.md`** - Tuku tool usage
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

## 📘 docs/manuals/ - Long-Form AsciiDoc Manuals

**Location:** `docs/manuals/` (AsciiDoc source and generated PDFs)

Book-length manuals maintained as AsciiDoc and published as PDF.  Unlike the wiki, these are written as
specifications and contracts rather than tutorials.

- **`tiri-reference/`** - The Tiri Programming Language manual.  Carries its own `AGENTS.md` with the manual's
  style guide, versioning rules, and function documentation template.  Read that file before editing the manual.
- **`fonts/`** - Noto font files embedded into the generated PDFs.

**Key Usage Notes:**
- Each manual folder holds its own `book.adoc` master document and a `tiri_lexer.rb` syntax highlighter.
- PDF generation uses `asciidoctor-pdf`; see the manual's own `AGENTS.md` for the exact invocation.
- These manuals are authored by hand.  They are *not* regenerated from source code, so changes here will not be
  overwritten by the documentation pipeline.

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
- **`modules/classes/`** - Individual class documentation
  - `vector.html`, `surface.html`, `file.html`, `bitmap.html`, etc.
  - Each class page includes methods, fields, actions, and examples
- **`wiki/`** - Rendered HTML of the `docs/wiki/` Markdown sources
- **`manuals/`** - Published PDF manuals

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
- **`modules/`** - One XML file per module (16 in total): `audio`, `config`, `core`, `display`, `document`,
  `font`, `http`, `network`, `regex`, `scintilla`, `svg`, `tiri`, `vector`, `xml`, `xquery`, `xrandr`.

### Class Documentation
- **`modules/classes/`** - Individual class XML files (82 in total)
  - **`vector.xml`** - Abstract vector graphics base class
  - **`surface.xml`** - Display surface management
  - **`file.xml`** - File system operations
  - **`bitmap.xml`** - Image processing

### Transformation Assets
- **`css/`, `js/`, `images/`** - Assets consumed when generating the HTML website

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

## 📖 How to Use This Documentation Effectively

### For Understanding APIs
1. **Start with wiki guides** for conceptual understanding
2. **Check XML files** in `docs/xml/modules/` for precise interfaces, extended commentary and examples

### For Tiri Scripting
1. **`Tiri-Reference-Manual.md`** for language fundamentals
2. **The topic-specific `Tiri-*-API.md` page** for the area in question (GUI, IO, JSON, HTTP server, OAuth, and so on)
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
3. **Markdown** → **HTML** (via documentation generator)

Key scripts:

- **`tools/docgen.tiri`** - Main documentation generator
- **`tools/docgen_wiki.tiri`** - Wiki-specific processing

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
- Reference `docs/xml/` for detailed technical specifications and complete API coverage
- **NEVER** read documentation from `docs/html` when writing code, use the XML documentation instead.

### When Writing Code
- Check existing examples in `examples/` directory first
- Use the XML API documentation to understand object interfaces
- Follow patterns established in wiki tutorials
- If there are discrepancies between documentation and code, bring it to the user's attention

### When Debugging Issues
- Consult error codes in `docs/wiki/System-Error-Codes.md`
- Review action documentation for correct usage patterns
- Check class field documentation for property requirements

### When Working with Tiri Scripts
- Always study existing `.tiri` files for patterns
- Use `Tiri-GUI-API.md` for interface constants
- Reference class XML pages for object method signatures

This documentation ecosystem provides comprehensive coverage from high-level concepts to low-level implementation details, enabling effective development with the Kōtuku framework.
