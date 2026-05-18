---
name: tiri-programming
description: >-
  Use when writing, editing, reviewing, or testing Kotuku Tiri scripts, Flute tests, .tiri files, scripts/gui
  modules, examples, tools, or Tiri-facing API usage. Tiri is not standard Lua; use this skill before changing
  Tiri code or giving Tiri programming guidance.
---

# Tiri Programming

Use this skill before changing `.tiri` files or advising on Tiri code. Tiri is a LuaJIT-based language with
project-specific syntax, runtime behaviour, API bindings, typing, and testing conventions.

## First Steps

1. Read nearby existing `.tiri` files before editing. For new tests, read at least three existing Flute tests first.
2. Prefer current project examples over assumptions from Lua:
   - `examples/*.tiri` for applications and GUI usage
   - `scripts/gui/*.tiri` for widget and layout patterns
   - `scripts/*.tiri` for standard library APIs
   - `tools/*.tiri` for file, process, and utility scripts
   - `tools/idl/idl-c.tiri` for extensive file I/O and API usage
3. Consult `docs/wiki/Tiri-Reference-Manual.md`, `docs/wiki/Tiri-*.md`, or the Tiri LSP for details that are not
   summarised here.
4. Consult `docs/xml/modules` and `docs/xml/modules/classes` for generated class and module API documentation.
5. Deal with uncertainty over language and API behaviour by running `origo` with the `--statement` option to run micro tests.

## Core Rules

- Tiri scripts use `.tiri` and execute top-to-bottom with no entry point function.
- `.lua` files may also be parsed as Tiri; use `-- $TIRI` near the start when a file needs explicit recognition.
- Script named arguments are read with `arg(Name, Default)`. Argument values arrive as strings.
- Variables and functions are local by default and scoped to their statement block. Use `global` before first use only when a symbol must be exported.  Use `local` to manage the scope of local variables.
- Use upper camel-case for function arguments and lower snake-case for local variables.
- Use three spaces for indentation.
- Use zero-based indexing for tables and string functions.
- Prefer arrays for sequential data. Arrays are a distinct, typed, JIT-friendly container.
- The Tiri object interface is case sensitive. Object fields are lower snake-case, for example `netlookup.hostName`.
- Tiri uses `is` instead of `==`. Avoid deprecated `==` and `~=`.
- Use `!=` for not-equal.
- Use `try-except-when`, `check`, and `raise` instead of deprecated `pcall()` and `xpcall()`.
- Use `load()` instead of `loadstring()`, `loadFile()` instead of `dofile()`, and result masks instead of `select()`.
- Lua `package`, `os`, and introspective `debug` library assumptions often do not apply; use Kotuku APIs.

## Syntax Differences From Lua

Recognise and use these Tiri extensions where they match local code style:

- `continue` in loops
- `has` for bit flag checks, equivalent to `flags & mask != 0`
- Compound operators: `+=`, `-=`, `*=`, `/=`, `%=`
- String append: `..=`
- Postfix increment: `++`
- C-style bitwise operators: `&`, `|`, `^`, `~`, `<<`, `>>`
- C-style ternary operator: `condition ? true_val :> false_val`
- Falsey checks and defaults: `??`, `??=`, and `value1 ?? value2`
- Nil assignment shorthand: `?=`
- Safe navigation: `obj?.field`, `obj?.method()`, `obj?[key]`
- Deferred cleanup: `defer ... end`
- To-be-closed variables: `resource <close> = acquire_resource()`
- Constants: `local name <const> = value`
- Anonymous function expressions: `(value => print(value))`
- Ranges: `for i in {0..10} do`
- String interpolation with expressions: `f"My {expression} here"`
- Pipe operator: `value |> transform()` and limited multi-result forwarding with `|N>`
- Result filter operator: `[_*]function_call()` to drop or keep selected return values
- Pattern selection: `choose value from ... end`

Avoid Unicode operator forms in new code unless surrounding code already uses them. ASCII forms are easier for agents,
search tools, and mixed editor environments.

## Typing And Scope

- Local variables use sticky types: the first non-`nil` assignment fixes the variable's type.
- Assigning `nil` clears a value but does not clear its fixed type.
- Use `:any` only when a variable or result genuinely needs mixed types.
- Use parameter and return annotations for public/library functions and recursive functions.
- Supported type names include `any`, `nil`, `num`, `str`, `bool`, `table`, `array`, `func`, `thread`, and `obj`.
- Multi-result return annotations use angle brackets, for example `function split(Line: str):<str, str>`.
- Recursive and mutually recursive functions require explicit return types.
- Use `_` as the blank identifier to intentionally discard assignment, return, or loop values. It cannot be read.

## Control Flow And Errors

- Prefer `try ... except ... when ... success ... end` for structured exception handling.
- Put filtered `except e when ERR_Name` handlers before any catch-all `except e` handler.
- Use `try<trace>` only when stack traces are needed; normal `try` avoids trace overhead.
- Use `check` with API calls returning `ERR` codes, and `raise ERR_Name` for explicit error-code exceptions.
- Use `error(Message)` for generic script exceptions and `error(e)` to rethrow an exception table.
- There is no `finally`; use `defer`, `<close>`, or object lifetime management for cleanup.
- `defer` executes on normal scope exit, `return`, `break`, and `continue`. `<close>` handlers run before defers.
- `??` treats `nil`, `false`, `0`, and `""` as empty. Standard `or` only treats `nil` and `false` as falsey.
- `??` can guard control flow, for example `value ?? return ERR_InvalidInput`.

## Ranges And Collections

- Range literals use `{Start..Stop}` for exclusive stop and `{Start...Stop}` for inclusive stop.
- Range literals do not support step expressions; use `range(Start, Stop, Inclusive, Step)` for stepped ranges.
- Range operands can be variables but not arbitrary expressions.
- Negative indices in slicing count from the end and preserve `..` exclusive or `...` inclusive stop semantics.
- Ranges can iterate, slice strings/tables/arrays, test membership with `in`, and provide functional methods such as
  `each`, `map`, `filter`, `reduce`, `take`, `any`, `all`, and `find`.
- Use native arrays for sequential data and buffers:
  - `array<type>`, `array<type, size>`, `array<type> { values... }`
  - common element types include `byte`, `int16`, `int`, `int64`, `float`, `double`, `string`, `object`, `struct`,
    `table`, `array`, and `any`
  - useful methods include `push`, `pop`, `clear`, `resize`, `fill`, `insert`, `remove`, `reverse`, `sort`,
    `contains`, `first`, `last`, `find`, `copy`, `getString`, `setString`, `slice`, `concat`, `join`, `clone`,
    `each`, `map`, `filter`, `reduce`, `any`, and `all`

## Strings And Regex

- Use `string.substr()` instead of `string.sub()`.
- Use the `regex.*` API instead of obsolete Lua pattern helpers such as `string.gsub()`, `string.match()`, and
  `string.gmatch()`.
- Useful string helpers include `alloc`, `cap`, `count`, `decap`, `escXML`, `hash`, `join`, `pop`, `replace`,
  `trim`, `rtrim`, `split`, `startsWith`, `substr`, `endsWith`, and `unescapeXML`.
- Use f-strings for readable formatting. Add `??` fallbacks when interpolating externally sourced values.

Regex uses compiled PCRE-compatible objects. Compile once and reuse; wrap untrusted patterns in `try`.

Available regex signatures:

```lua
rx = regex.new(Pattern, [Flags])
escaped = regex.escape(Text)
result = rx.test(Text, [RMATCH])
start, stop, captures = rx.findFirst(Text, [Offset], [RMATCH])
iter = rx.findAll(Text, [Offset], [RMATCH])
capture, ... = rx.extract(Text, Offset, [RMATCH])
matches = rx.match(Text, [RMATCH])
all_matches = rx.search(Text, [RMATCH])
result = rx.replace(Text, Replacement, [RMATCH])
parts = rx.split(Text, [RMATCH])
```

Regex compile flags: `regex.ICASE`, `regex.MULTILINE`, `regex.DOT_ALL`.

Regex match/replace flags: `regex.NOT_BEGIN_OF_LINE`, `regex.NOT_END_OF_LINE`, `regex.NOT_BEGIN_OF_WORD`,
`regex.NOT_END_OF_WORD`, `regex.NOT_NULL`, `regex.CONTINUOUS`, `regex.PREV_AVAILABLE`,
`regex.REPLACE_NO_COPY`, `regex.REPLACE_FIRST_ONLY`.

Regex object properties: `pattern`, `flags`, and `error`.

## Scripts, Modules, And Objects

- Use `include 'core','xml','display'` to load API definitions when needed.
- Use top-level `import 'name' [as namespace]` for parse-time inlined libraries. Do not include `.tiri` in imports.
- Use `import './local_name'` for application-specific libraries in the local folder.
- Use `@if(imported=true)` and `@if(imported=false)` to separate library-import behaviour from direct execution.
- Use `loadFile(Path)` only when runtime loading is required; it does not get parse-time inlining benefits.
- Use `exec(Statement)` for dynamic statements and expect parse/runtime failures to raise exceptions.
- Load modules with `mName ?= mod.load('module')`; common globals are `mAudio`, `mSys`, `mGfx`, `mFont`,
  `mNet`, `mVec`, and `mXML`.
- Module and object API calls often return `ERR` as the first result; use `check`, result filters, or explicit
  handling rather than ignoring error codes accidentally.
- Create objects with `obj.new('Class', { field=value })` when fields are known up front; this initialises the
  object automatically and raises on error.
- If fields must be set in stages, call `obj.new('Class')`, assign fields, then call `object.init()`.
- Use `obj.find(NameOrUID)` to access existing objects; it returns `nil` if not found.
- Use `with object do ... end` to lock Kotuku objects for thread-safe and faster repeated field access.
- Child objects created through `parent.new(...)` follow parent lifetime and are weakly referenced.
- Prefer clearing object references to `nil` over manual `free()` unless weak references or immediate termination
  require `free()`.
- Actions use the `ac` prefix, methods use the `mt` prefix, and fields are direct properties. Consult generated API
  docs for class-specific names.
- Use `subscribe()`/`unsubscribe()` for action and method subscriptions and clean up subscriptions when finished.

## Async, Processing, And Events

- `async.script(Statement, Callback)` runs code in a separate script state. It cannot see caller variables directly;
  find shared objects with `obj.find()`.
- `async.action(Object, Action, Callback, Key, Args...)` and `async.method(...)` run object calls in a thread. Keep
  the target object alive until the callback runs.
- Async callbacks run on the next message processing cycle, so scripts commonly call `processing.sleep()`.
- Use `processing.sleep([Timeout], [WakeOnSignal=true])` for passive wait loops and event-driven examples.
- Use `processing.new({ timeout=..., signals={...} })` when coordinating multiple signal objects.
- Use `processing.delayedCall(Function)` to schedule work for the next message processing cycle.
- Use `subscribeEvent(EventName, Function)` and `unsubscribeEvent(Handle)` for system-wide events.

## Annotations And Tests

- Function annotations use `@Name(args...)` immediately before a function declaration.
- Use `@Doc(text=[[...]])` when writing library functions that should surface documentation through tooling.
- Compile-time preprocessing uses `@if(condition=value) ... @end`; common conditions are `imported`, `debug`,
  `platform`, and `exists`.
- Flute recognises `@Test`, `@BeforeEach`, `@AfterEach`, `@BeforeAll`, `@AfterAll`, `@Disabled`, and `@Requires`.
- `@Requires` can declare runtime needs such as `display`, `network`, `audio`, `font`, and `ssl`.

## Running Scripts

Run scripts with the installed `origo` executable. When invoking `origo` directly, include logging:

```powershell
build/agents-install/origo --statement "print('Hello')" --log-warning
```

If changing files under `scripts/`, make sure the modified source tree overrides installed scripts:

```powershell
build/agents-install/origo path/to/script.tiri --set-volume scripts=E:/parasol/scripts --log-warning
```

Use `--log-api` for more detail and `--log-threads` when debugging thread-related issues.

## Flute Tests

Always write Tiri tests using Flute unless instructed otherwise. Test files are typically named `test_*.tiri`.

Run a focused test from the repository root with:

```powershell
build/agents-install/origo tools/flute.tiri file=src/network/tests/test_bind_address.tiri --log-warning
```

Use `--gfx-driver=headless` for CI or automated display tests. If running outside the repository root, use an
absolute path for the `file=` parameter.

Register new tests through the existing CMake `flute_test()` pattern in the relevant module.
