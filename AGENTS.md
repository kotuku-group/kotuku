# Repository Guidelines

This file provides guidance to Agentic programs when working with code in this repository.

### Essential Build Commands

Prefer the shared presets in `CMakePresets.json` (CMake 3.28 or newer).  Run commands from the repository root.
Use `cmake --list-presets=all` to list the available presets.  For a complete modular Debug configure, build, install,
and test cycle, use `cmake --workflow --preset agents`; this installs the build before running tests.

**Configure build:**

- Preferred: `cmake --preset agents`.  This uses `build/agents` and installs to `build/agents-install`.
- Static build: `cmake --preset agents -DKOTUKU_STATIC=ON`.  Then use the separate build/install and test commands below;
  the `agents` workflow reconfigures with the modular defaults.
- Explicit alternative: `cmake -S . -B build/agents -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=build/agents-install -DRUN_ANYWHERE=TRUE -DKOTUKU_STATIC=OFF -DUNIT_TESTS=ON -DORIGO_CONSOLE=ON -DLINKED_PACKAGES=ON`
- Modular/Static builds: Use `-DKOTUKU_STATIC=OFF` for modular builds and `-DKOTUKU_STATIC=ON` for static.
- Always use Debug builds unless the user requests otherwise, or if the task requires use of performant Release binaries.
- `UNIT_TESTS=ON` enables compiled C++ unit tests and prolongs the build.  The agent presets enable `BUILD_TESTING`,
  `BUILD_TESTS`, and `UNIT_TESTS` for integration and unit tests.

If `build/agents` is already configured for a different platform, use `cmake --preset agents-platform`.
This selects `build/agents-${hostSystemName}` (for example, `build/agents-Linux`) and retains `build/agents-install`.
Use the corresponding `agents-platform`, `agents-platform-install`, and `agents-platform-headers` build presets,
and the `agents-platform` test or workflow preset for that tree.

**Build and install:**

- Preferred: `cmake --build --preset agents-install` (parallel Debug build and install).
- Build only: `cmake --build --preset agents`.
- Explicit alternative: `cmake --build build/agents --config Debug --parallel && cmake --install build/agents --config Debug`
- To build an individual module, append `--target [module]` to the build command, e.g. `--target network`.  In static builds, use `--target [module] origo_cmd` to ensure that the origo executable is rebuilt to include the changes.

**Testing:**

- Always install your latest build before running `ctest`.
- Always run `ctest` through a test preset.  Run the standard test suite with `ctest --preset agents`.
- Filter by label with `-L TEST_LABEL` or by test name with `-R TEST_NAME`, e.g. `ctest --preset agents -L xquery`.
- Audio module tests (any label beginning with `audio`) are excluded from the standard presets.  Run them
  explicitly with `ctest --preset agents-audio`, optionally adding `-R audio_reverb` for a single test.  The
  `debug-audio` and `debug-static-audio` presets do the same for the `debug` and `debug-static` trees.
- For the `agents-platform` tree, use the `agents-platform` and `agents-platform-audio` test presets.
- Do not invoke `ctest --test-dir` directly.  It bypasses the preset filters and runs every registered test.
- Tiri tests are always written for the Flute tool.
- Use the `flute-testing` skill before writing, reviewing, planning, or running Flute tests.
- When running the Origo executable for individual tests, append `--log-warning` at a minimum for log messages, or `--log-api` if more detail is required.  Log output is directed to stderr.
- For ad-hoc testing, statements can be tested on the commandline with `--statement`, e.g. `origo --statement "print('Hello')"`
- If modifying files in the `packages` folder, ensure the build option `LINKED_PACKAGES=ON` is active so that script changes are always visible to origo.
- If debugging issues involving threads, add `--log-threads` for improved log output.

**Verify:**
- You can inspect the version, git commit hash and build type of the build by running `origo` with `--version`.

## Architecture Overview

### Module System

- Module definitions are stored in `.tdl` files which generate C++ headers and module `MOD_IDL` strings

### Object System and TDL Files

Kōtuku uses Interface Definition Language (IDL) files with `.tdl` extension to generate documentation, include files and C++ stubs:

- TDL files define classes, methods, fields, and constants
- The `build_headers` target generates C/C++ headers and XML documentation from TDL using tools in `tools/idl/`
- Normal builds consume the existing generated headers.  After changing `.tdl` files, C++ blueprints or embedded documentation, run `build_headers` to regenerate headers and XML documentation.  Use `touch` on any `.tdl` file beforehand if needing to force regeneration of its downstream files.

### Scripting Integration

**Tiri** is the integrated scripting language:

- Tiri is a LuaJIT-based language that provides high-level access to Kōtuku APIs, but it is not standard Lua.
- GUI toolkit available through the indexed `gui/*` packages (modular widget system)
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
  run `cmake --build --preset agents-headers` after configuring the agent build.  The explicit alternative is
  `cmake --build build/agents --config Debug --target build_headers --parallel`.
- The `build_headers` target requires a working installed `origo` executable so that the IDL tools can run.

### Multi-Platform Considerations

- Core code is cross-platform (Windows/Linux/MacOS/Android) and is specifically optimised for 64-bit CPUs.  32-bit compatibility is redundant.
- Windows builds support the MSVC toolchain.  MinGW support is deprecated

**Windows-Specific Notes:**
- In Bash commands, quote paths with spaces: `"path with spaces"`
- For Flute tests, use relative paths for executables to avoid path separator issues

## Development Guidelines

- Use the `cpp-programming` skill before writing, editing, reviewing, or testing C++ code, or giving Kōtuku C++
  programming guidance.
- The default column width is 120 characters for all programming languages. Markdown and Asciidoc files are exempt.
- Use LF line endings on all platforms; CRLF is not permitted.

## File Organization

- `docs/plans/` - For storing and retrieving your plan files.

Lower snake-case is the preferred string format for new file names.

## Agentic Behaviour

- Always give an honest, balanced opinion in your responses
- Encourage testing and validation of changes.  Analysis should be presented alongside evidence.
- If you are asked to do work that relates to an existing plan file, update the plan at the end of the session to indicate what was achieved.
- Read AGENTS.local.md if present
