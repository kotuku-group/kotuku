---
name: flute-testing
description: >-
  Use when writing, reviewing, extending, running, or planning coverage for Kotuku Flute tests. This includes creating
  test_*.tiri files, adding flute_test() CMake registrations, analysing missing test coverage, debugging Flute failures,
  and validating Tiri-facing module behaviour through the Flute runner.
---

# Flute Testing

Use this skill for Flute-specific testing work. Flute tests are Tiri scripts, so also follow the `tiri-programming`
skill for Tiri syntax, naming, indentation, object API usage, and runtime conventions.

## Workflow

1. Read the implementation under test and nearby tests before writing anything. For a new test style or unfamiliar
   module, read at least three existing Flute tests.
2. Identify the behavioural risks first: normal operation, edge cases, error handling, boundary values, module
   interactions, platform-sensitive paths, and lifecycle or cleanup behaviour.
3. Write focused tests that are deterministic, fast, and meaningful when they fail.
4. Register new test files with the nearest existing `flute_test()` CMake pattern.
5. Build and install before running `ctest`; run the focused Flute file directly while iterating.

## Test File Pattern

- Name test files `test_*.tiri` unless surrounding module tests use a different established convention.
- Keep test files in the module's `tests/` directory unless the module already has a more specific pattern.
- Use top-to-bottom Tiri execution. Do not add an entry point function.
- Use `@Test` for test functions and `@BeforeAll`, `@AfterAll`, `@BeforeEach`, and `@AfterEach` for fixtures.
- Use `@Requires` for optional runtime dependencies such as `display`, `network`, `audio`, `font`, or `ssl`.
- Use `assert(Condition, Message)` for expected test conditions. Prefer assertions over `error()` for conditional test
  failures so the failing expectation is explicit.
- Use `try ... except ... success ... end` when testing expected failures.
- Clean up temporary files, objects, subscriptions, sockets, and servers in `@AfterAll` or `@AfterEach`.

Minimal structure:

```
   include 'module'

@BeforeAll
function init(State)
end

@AfterAll
function cleanup()
end

@Test function BehaviourUnderTest()
   assert(true, 'Describe the expected behaviour clearly')
end
```

## Coverage Planning

When asked what tests are missing, produce a prioritised test plan before editing. Cover:

- Public behaviour and common use cases.
- Boundary values and malformed inputs.
- Error codes, exceptions, and recovery paths.
- Cross-module integration points.
- Platform-sensitive behaviour.
- Resource ownership, cleanup, and object lifecycle.
- Regression cases tied directly to the code change or bug report.

Prefer a small number of high-value tests over broad but brittle coverage. Call out risks that are deliberately left
untested and explain why.

## Running Tests

Run focused Flute files from the repository root:

```powershell
build/agents-install/origo tools/flute.tiri file=src/network/tests/test_bind_address.tiri --log-warning
```

Use `--log-api` when runner or API detail is needed, and `--log-threads` for thread-related failures. Use
`--gfx-driver=headless` for CI, automated display tests, and other non-interactive graphics checks.

If running outside the repository root, use an absolute path for the `file=` parameter. If testing changes under
`scripts/`, add the source override so the installed runtime loads the edited scripts:

```powershell
build/agents-install/origo tools/flute.tiri file=src/example/tests/test_example.tiri `
   --set-volume scripts=E:/parasol/scripts --log-warning
```

After C++ or build-system changes, build and install before running `ctest`:

```powershell
cmake --build build/agents --config Debug --parallel
cmake --install build/agents --config Debug
ctest --build-config Debug --test-dir build/agents --output-on-failure -L TEST_LABEL
```

## CMake Registration

Register new tests with the local module's existing style. Common patterns include:

```cmake
flute_test(vector_readpainter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_readpainter.tiri")
```

or loop-based registrations:

```cmake
foreach(TEST_NAME item_one item_two)
   flute_test(module_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach()
```

Keep labels and names consistent with neighbouring tests so focused `ctest -L ...` runs remain predictable.
