# Repository Guidelines

This file provides guidance to Agentic programs when working with code in this repository.

### Essential Build Commands

**Configure build:**
- `cmake -S . -B build/agents -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=build/agents-install -DRUN_ANYWHERE=TRUE -DKOTUKU_STATIC=OFF -DUNIT_TESTS=ON -DORIGO_CONSOLE=ON`
- Modular/Static builds: Use `-DKOTUKU_STATIC=OFF` for modular builds and `-DKOTUKU_STATIC=ON` for static.
- Always use Debug builds unless the user requests otherwise.

**Build and install:**
- Build and install: `cmake --build build/agents --config Debug --parallel && cmake --install build/agents --config Debug`
- To build an individual module, append `--target [module]` to the build command, e.g. `--target network`.  In static builds, use `--target [module] origo_cmd` to ensure that the origo executable is rebuilt to include the changes.

**Testing:**
- **ALWAYS** install your latest build before running `ctest`.
- Run all integration tests: `ctest --build-config Debug --test-dir build/agents --output-on-failure`
- Run single integration test: `ctest --build-config Debug --test-dir build/agents --output-on-failure -L TEST_LABEL`
- **ALWAYS** write Tiri tests using Flute unless instructed otherwise.
- Use the `flute-testing` skill before writing, reviewing, planning, or running Flute tests.
- When running the Origo executable for individual tests, **ALWAYS** append `--log-warning` at a minimum for log messages, or `--log-api` if more detail is required.  Log output is directed to stderr.
- Statements can be tested on the commandline with `--statement`, e.g. `origo --statement "print('Hello')"`
- If modifying files in the `scripts` folder, **ALWAYS** append `--set-volume scripts=/absolute/path/to/project/scripts` to ensure your modified files are being loaded over the installed versions.
- If debugging issues involving threads, add `--log-threads` for improved log output.

**Verify:**
- You can inspect the version, git commit hash and build type of the build by running `origo` with `--version`.

### CMake Configuration Options

Key build options (use with `-D` flag):
- `KOTUKU_STATIC=ON/OFF` - Build as static libraries instead of modules
- `BUILD_TESTS=ON/OFF` - Enable/disable Flute tests
- `UNIT_TESTS=ON/OFF` - Enable compiled C++ unit tests (prolongs build)
- `RUN_ANYWHERE=ON/OFF` - Build for local folder execution
- `KOTUKU_VLOG=ON/OFF` - Enables trace level log messages via log.trace() in Debug builds (has no effect on Release builds).

### Development in the Cloud

When working in ephemeral cloud environments:

- Prefer the pre-created build tree at `build/agents` and install tree at `build/agents-install` to avoid the expense of repeated configuration.  If the directory exists you can immediately run `cmake --build build/agents --config Debug --parallel`.
- If you must reconfigure, clean only the affected cache entries with `cmake -S . -B build/agents -DCMAKE_BUILD_TYPE=Debug ...` rather than deleting the entire build tree.
- If `origo` is not already installed at `build/agents-install` then performing the build and install process is essential if intending to run `origo` for Tiri scripts and Flute tests.
- If configuring a build, disabling unnecessary modules like Audio and Graphics features (if they are not relevant) will speed up compilation.  If *certain* that the environment is cloud-based, you can consider including the following with your CMake build configuration: `-DDISABLE_AUDIO=ON -DDISABLE_X11=ON -DDISABLE_DISPLAY=ON -DDISABLE_FONT=ON`

## Architecture Overview

### Core Framework Structure

**Kōtuku** is an application framework with a core focus on building scalable user interfaces and highly efficient JIT compiled applications. The framework automatically handles display resolution and scaling concerns, allowing developers to focus on application logic rather than display technicalities. Key architectural components include:

1. **Core System** (`src/core/`) - Base object system, memory management, filesystem, and module loading
2. **Vector Graphics Engine** (`src/vector/`) - Main graphics rendering system with scene graphs, filters, and painters
3. **SVG Support** (`src/svg/`) - W3C-compliant SVG parsing and rendering with SMIL animation
4. **Display Management** (`src/display/`) - Cross-platform window management, surfaces, and input handling
5. **Tiri Scripting** (`src/tiri/`) - An extensively modified Lua-based scripting environment built on LuaJIT
6. **Document Engine** (`src/document/`) - RIPL text layout engine for rich document rendering
7. **XML Support** (`src/xml/`, `src/xquery/`) - XML and XQuery functionality

### Module System

Kōtuku uses a modular architecture where each major feature is implemented as a separate module:

- Each module is in `src/[module_name]/` with its own `CMakeLists.txt`
- Static builds link all modules into the core, while modular builds load them dynamically
- Module definitions are stored in `.tdl` files which generate C++ headers and module `MOD_IDL` strings

### Object System and TDL Files

Kōtuku uses Interface Definition Language (IDL) files with `.tdl` extension to generate documentation, include files and C++ stubs:

- TDL files define classes, methods, fields, and constants
- The `build_headers` target generates C/C++ headers and XML documentation from TDL using tools in `tools/idl/`
- Class implementations are in `class_*.cpp` files
- Generated headers go to `include/kotuku/` directory
- Normal builds consume the existing generated headers.  After changing `.tdl` files, C++ blueprints or embedded documentation, run `build_headers` to regenerate headers and XML documentation.

### Scripting Integration

**Tiri** is the integrated Lua-based scripting language:

- Tiri is a LuaJIT-based language that provides high-level access to Kōtuku APIs, but it is not standard Lua.
- GUI toolkit available through `scripts/gui/` modules (modular widget system)
- All Tiri scripts use `.tiri` extension
- Tiri scripts are executed with the `origo` executable, which has a dependency on the project being built and installed
- Tiri scripts execute top-to-bottom with NO entry point function
- When writing, editing, reviewing, or testing Tiri code, use the `tiri-programming` skill first
- Tiri APIs and reference manuals are available in multiple files at `docs/wiki/Tiri-*.md`
- General API framework documentation in `docs/xml/modules` and `docs/xml/modules/classes` can be utilised to understand class and module interfaces in detail.

## Key Development Patterns

### Flute Testing

Use the `flute-testing` skill for Flute-specific test design, implementation, CMake registration, and runner
commands.  Key repository facts:

- Flute tests are Tiri `.tiri` files, normally named `test_*.tiri`.
- Register new tests with the local module's existing `flute_test()` CMake pattern.
- Tests run post-install against the installed framework.
- Use `--gfx-driver=headless` for CI and automated display tests.
- A focused test can be run from the repository root with:

```bash
build/agents-install/origo tools/flute.tiri file=src/network/tests/test_bind_address.tiri --log-warning
```

### Code Generation

The build system heavily uses code generation:
- TDL files are processed by `tools/idl/idl-c.tiri` to generate C headers
- `tools/idl/idl-compile.tiri` generates IDL definition strings
- Normal agent builds consume the existing generated files.  When headers or XML documentation require regeneration,
  run `cmake --build build/agents --config Debug --target build_headers --parallel` from a configured build tree.
- The `build_headers` target requires a working installed `origo` executable so that the IDL tools can run.

### Multi-Platform Considerations

- Core code is cross-platform (Windows/Linux/MacOS/Android) and is specifically optimised for 64-bit CPUs.  32-bit compatibility is redundant.
- Platform-specific code is in subdirectories (`win32/`, `x11/`, etc.)
- Build system auto-detects platform capabilities (X11, SSL, etc.)
- Windows builds support the MSVC toolchain.  MinGW support is deprecated

**Windows-Specific Notes:**
- In Bash commands, quote paths with spaces: `"path with spaces"`
- For Flute tests, use relative paths for executables to avoid path separator issues

## Working with Vector Graphics

The vector graphics system is the core of Kōtuku and provides unique capabilities:
- **API-accessible scene graphs** - Hierarchical scene graphs specifically for vector graphics with full programmatic access
- **SVG-to-scene graph integration** - SVG files are parsed directly into manipulable scene graphs for real-time modification
- **Resolution independence** - All graphics scale automatically across display resolutions and DPI settings
- Filters provide effects like blur, lighting, and color manipulation
- Painters handle gradients, patterns, and image fills
- Animation support includes SMIL (SVG animation) and custom VFX
- Real-time manipulation of individual scene graph nodes for dynamic graphics

**Distinctive Features:**
Kōtuku maintains retained scene graphs that can be modified at runtime. This enables dynamic, scalable graphics where individual elements can be manipulated programmatically.

## Development Guidelines

### ⚠️ CRITICAL PROJECT REQUIREMENTS (Override Standard C++ Practices)

**These rules MUST be followed and override common C++ conventions:**

- **NEVER use `static_cast`** - Use C-style casting instead, e.g. `int(variable)` NOT `static_cast<int>(variable)`
- **NEVER use the `&&` boolean operator** - Use `and`
- **NEVER use the `||` boolean operator** - Use `or`
- **NEVER use `==` for equality** - Use the `IS` macro (exceptions made for operator overloading).  Use of `!=` is still permitted.
- **NEVER use C++ exceptions** - Error management relies on checking function results

### 📋 MANDATORY CODING CHECKLIST

Before considering ANY C++ code changes complete, verify:

- [ ] Code compiles successfully
- [ ] Follows formatting standards below

### Additional Code Style Standards

- Always use upper camel-case for the names of function arguments in C++ and Tiri code.
- Always use lower snake_case for the names of variables inside C++ and Tiri functions.
- Use three spaces for tabulation in C++ and Tiri code.
- C++ functions that use global variables must be written with thread safety in mind.
- New and refactored code must target modern C++20 conventions and functionality.
- C++ global variable names are prefixed with `gl` and written in upper camel-case, e.g. `glSomeVariable`
- The default column width is 120 characters for all programming languages.  Markdown and Asciidoc files are exempt.
- Always default to British English spelling in code and comments.
- For C++ `if`, `while`, `else`, `for`, `switch` and `struct` keywords, the opening curly brace must be on the same line if no word-wrapping has occurred.
- Whilst the spelling of Kōtuku applies to all documentation for branding purposes, when writing code the simplified `Kotuku` spelling is used throughout.
- The only acceptable line-feed character on all platforms is LF and the use of CRLF is not permitted.

### Testing

**MANDATORY: Always compile after making C++ changes**
- After making changes to C++ source files, you MUST verify compilation by building the affected module(s)
- This is required before considering any code changes complete
- There is a dependency on `origo_cmd` being built by cmake in order to make the `origo` executable available to run tests.

**Full Build Commands:**
```bash
# Build everything
cmake --build build/agents --config Debug --parallel

# Install after successful build
cmake --install build/agents --config Debug
```

**Module Build Commands:**
```bash
# Build specific module (e.g., network, vector, svg, etc.)
cmake --build build/agents --config Debug --target [module_name] --parallel

# Examples:
cmake --build build/agents --config Debug --target network --parallel    # For network changes
```

### Documentation

- API documentation is embedded within comment sections of the C++ source files.
- Embedded documentation is identified with markers in the format `-MARKER-` whereby `MARKER` describes a section for the document generator to parse.
- Marked document sections are terminated at the end of their comment or when `-END-` is encountered.
- Embedded documentation for each function is identified by the `-FUNCTION-` marker.
- Embedded documentation for each class is identified by the `-CLASS-` marker.
- Classes can export actions and methods, which are documented with `-ACTION-` and `-METHOD-` markers.
- Embedded documentation for class fields are identified by the `-FIELD-` marker.
- Always use British English spelling in documentation, comments and variable names.

## File Organization

- `src/` - All source code organized by module
- `include/kotuku/` - Public API headers (many auto-generated)
- `scripts/` - Tiri standard library and GUI toolkit
- `tools/` - Build tools and utilities (IDL processors, test runner)
- `examples/` - Example applications and demonstrations (examine git-tracked .tiri files for current examples)
- `data/` - Icons, fonts, styles, and configuration files
- `docs/wiki/` - Markdown files for the GitHub Wiki, includes practical tutorials and guides on how to use Kōtuku.
- `docs/html/` - Contains the entire Kōtuku website for offline viewing.
- `docs/xml/` - Auto-generated API documentation in XML format.  This content is sourced from the Kōtuku C++ files.
- `docs/plans/` - For storing and retrieving your plan files.

Lower snake-case is the preferred string format for new file names.

### Key Examples for Learning

- **`apps/find.tiri`** - Commandline application that demonstrates how to effectively search for files and content with `io.search()`
- **`apps/paintbox.tiri`** - Commandline application with colour manipulation features
- **`apps/tuku.tiri`** - HTTP client application, mimics cURL functionality
- **`apps/tuku_server.tiri`** - HTTP server implementation with NetSocket usage patterns
- **`apps/vuepoint.tiri`** - File viewer supporting SVG, RIPL, JPEG, PNG - shows document and graphics integration
- **`examples/widgets.tiri`** - Primary showcase of Kōtuku's GUI capabilities, demonstrates standard widgets and UI patterns
- **`examples/gradients.tiri`** - Interactive gradient editor demonstrating real-time vector graphics manipulation
- **`tools/idl/idl-c.tiri`** - Extensive file I/O and general API usage

## Agentic Behaviour

- Always give an honest, balanced opinion in your responses
- Encourage testing and validation of changes.  Analysis should be presented alongside evidence.
- If you are asked to do work that relates to an existing plan file, update the plan at the end of the session to indicate what was achieved.
