# Additional Tiri Language Details

Read only the sections relevant to the task. Paths below link to the maintained reference manual source.

## Enums And Structs

Sources: [variables and scoping](../../../../docs/tiri/tiri-reference/ch04_variables_and_scoping.adoc),
[structs](../../../../docs/tiri/tiri-reference/ch10_structs.adoc), and
[type contracts](../../../../docs/tiri/tiri-reference/ch17_type_system.adoc).

- `enum STATE { IDLE, RUNNING, FAILED = 10 }` generates `STATE_IDLE`, `STATE_RUNNING`, and `STATE_FAILED`.
  Prefixes and members use uppercase identifiers; explicit values must be integer literals. Enums are top-level,
  parse-time constants, not runtime tables. `global enum` adds no visibility; `local enum` is invalid.
- Declare struct layouts before use, at top level. Field types use the storage vocabulary, unlike scalar annotations:

  ```tiri
  struct Point
     x: double
     y: double
  end

  point = struct<Point> { x=1, y=2 }
  ```

- Construction without an initializer zero-initializes the fields. `Point` is not a constructor variable; use
  `struct<Point>` or `struct.new('Point', Fields)`. Identical layout redeclarations are accepted; conflicting ones fail.
- Field spelling is exact and case sensitive. Legacy module fields are registered in lower camel-case; consult the
  actual definition rather than converting names from a custom Tiri struct.
- Field writes require the correct value category. Numeric strings need conversion. Numeric fields accept `nil` as
  zero, object/pointer fields accept it as a cleared reference, while boolean and owned string fields reject it.
- `struct.clone()` makes an independent deep copy. `#instance` counts fields; `instance.size()` reports bytes.
  Embedded struct reads are views; dynamic array field reads are copies. Fixed arrays and dynamic arrays have
  different size rules on assignment; consult the struct chapter before passing native buffers.

## Native Arrays And Buffer Views

Sources: [arrays](../../../../docs/tiri/tiri-reference/ch11_arrays.adoc),
[view variables](../../../../docs/tiri/tiri-reference/ch04_variables_and_scoping.adoc), and
[native buffers](../../../../docs/tiri/tiri-reference/ch22_modules.adoc).

- Nested element constraints are supported, for example `array<array<int>>`. A `struct<Name>` array stores its
  elements by value and requires that exact layout; other reference element types retain references.
- `byte`/`char`, `int8`, and `uint8` are distinct element identities despite occupying one byte. String buffer helpers
  and string appends belong to `byte`/`char`, not every one-byte integer type.
- Integer writes truncate toward zero and wrap at the storage width. Integer arrays reject infinity and NaN.
  `float` rejects finite overflow; ordinary numbers remain doubles, so `int64`/`uint64` round-trips beyond 2^53
  can lose precision even though the container holds full-width integers.
- Multi-value mutation validates inputs before writing; a rejected value leaves the original array intact.
  Read-only arrays reject mutation; clone when a writable copy is needed.
- For supported array element types, `is` compares contents and element identity. Equality for `table` and `any`
  arrays is unsupported and can raise; use `rawequal()` for reference identity.
- `local pixels <view> = bitmap.data` binds a live, read-only view of a primitive numeric object array field rather
  than a copy. It binds one local with a plain initializer and cannot combine with `<const>` or `<close>`.
  String/object/pointer/struct fields cannot be viewed. Non-field initializers pass through unchanged.
- Views do not own native storage. Resource-backed views pin their resource; other views become invalid if the owner
  frees or reallocates its buffer. Prefer short-lived field reads to storing views across native mutations.
- Native calls accepting a caller-provided buffer can use `array<byte, Size>` or `string.alloc(Size)`. An omitted
  corresponding size argument is supplied from the buffer length. Check the native signature and returned byte count.

## Metamethods And Entity Designation

Sources: [metamethods](../../../../docs/tiri/tiri-reference/ch27_metatables_and_metamethods.adoc),
[entities](../../../../docs/tiri/tiri-reference/ch09_tables.adoc), and
[context](../../../../docs/tiri/tiri-reference/ch07_functions.adoc).

- Runtime metamethod dispatch installs its receiver as context. Reach it through `&field` or `&&`; do not declare
  Lua's explicit receiver argument. For example `__index = function(Key):any ... end` and
  `__close = function(Error) ... end`. Check each handler's remaining arguments in the manual.
- Metamethod context does not require entity designation. Calling an extracted handler directly is an ordinary
  function call and does not establish its original receiver.
- `entity { ... }` designates only the outer table. Alternatively, direct built-in `setmetatable()` with raw
  `__context = true` designates a table allocated in the designating function. Promoting a borrowed table or calling
  through an extracted/shadowed `setmetatable` is rejected. Designation is permanent and follows aliases.
- `rawget()`/`rawset()` bypass metamethods, not table classification or protected global/type-contract checks.
  User metatables are supported on tables; do not transplant Lua userdata metatable patterns.
- `__contains` receives the candidate, with the right operand as context. `__iter` supplies the bare-loop iterator
  protocol. Read the manual when implementing these; an ordinary `contains` or `iter` field is not a metamethod.

## Lazy Evaluation And Cleanup

Source: [resource management](../../../../docs/tiri/tiri-reference/ch15_resource_management.adoc).

- `<{ Expression }>` creates a deferred value; `<num{ Expression }>` supplies an explicit native type. The expression
  captures upvalues and evaluates on first required access, caching its result. `resolve(Value)` forces it and passes
  ordinary values through unchanged. A `thunk Name(Parameters):Type ... end` returns a new deferred value per call.
- Declared thunk types let `type()` and unconstrained native type tests answer without evaluating the body.
  Constrained tests such as `<array int>` may force evaluation. `rawtype()`/`isthunk()` identify unresolved thunks.
- `defer(Saved) ... end(Value)` snapshots the argument value, not a deep copy of a referenced table or object.
  Plain `defer` reads the latest upvalues. Return expressions are evaluated before cleanup.
- `<close>` destroys native objects and closes `io` file handles. Custom `__close` handlers receive an error argument
  during exception unwinding and use the closed value as context. Values without a close handler are safely ignored.

## Choose Patterns And Result Selection

Sources: [choose](../../../../docs/tiri/tiri-reference/ch16_pattern_matching.adoc) and
[result management](../../../../docs/tiri/tiri-reference/ch19_result_management.adoc).

```tiri
label = choose status from
   200 -> 'OK'
   <num> when status >= 500 -> 'Server error'
   else -> 'Other'
end
```

- The scrutinee is evaluated once; the first matching pattern with a passing guard wins. `else` must be last.
  No match yields `nil` in an expression, or no action in a statement.
- `{ key=Value }` patterns are shallow open-record tests, ignoring extra fields. `{}` specifically matches an empty
  table, not every table. Use `<table>` for a general type test.
- Tuple patterns match multiple results: `choose (x, y) from (0, _) -> ... end`. Tuple arm arity must agree;
  a call scrutinee can take its arity from the first tuple arm. `_` is a wildcard pattern, never a readable binding.
- In result filters, `*` keeps a position and `_` discards it. The final character repeats for remaining results:
  `[_*]call()` drops the first result and keeps the rest; `[_*_]call()` keeps only the second. `[]call()` drops all.
  Filters shape results; use `check` or explicit handling before discarding an API error code.
- `|N>` limits forwarded pipe values. For example `source() |1> forEach(Callback)` is appropriate when only the first
  result is the collection; `forEach()` takes exactly the target and callback.

## Packages And Runtime Loading

Sources: [script organisation](../../../../docs/tiri/tiri-reference/ch20_script_organisation.adoc) and
[metadata](../../../../docs/tiri/tiri-reference/ch28_annotations_and_preprocessing.adoc).

- `namespace Name { ... }` creates a constant namespace binding with mutable members. `namespace Name` joins an
  existing registry entry; import its creator first. A source file may declare one namespace at library level.
  `import ... as Alias` requires a declared namespace in the imported library.
- `@Package(name="net/url", version="2026.9.19")` identifies a unit and must be its first non-comment construct.
  Its version must match the selecting package-index entry. Names use lower-case ASCII letters/digits, dashes, and
  slashes; versions are integer components without leading zeroes.
- `@Dependencies(tiri="1.0", kotuku=">=2026.2.23")` follows `@Package`, or comes first when there is no package
  declaration. A bare Tiri version requires compatible major and equal/newer minor; a bare framework version is
  exact. Local `./`/`../` imports inherit unit metadata and cannot declare their own package or dependencies.
- These declarations are compilation metadata, not function annotations or `_ANNO` entries. Ordinary annotations
  attach to the immediately following function declaration; they are registered when that declaration executes.
- `loadFile('./file.tiri')` and `../` paths resolve from the calling script; other relative paths use the process
  working directory. `exec()` and `loadFile()` execute immediately, forward returned values, and raise on failure.

## File I/O And Async Coordination

Sources: [file I/O](../../../../docs/tiri/tiri-reference/ch24_file_io.adoc),
[processing](../../../../docs/tiri/tiri-reference/ch23_processing_and_events.adoc), and
[threading](../../../../docs/tiri/tiri-reference/ch26_threading.adoc).

- `io.*` failures raise exceptions. Use `local file <close> = io.open(Path, 'r')` for deterministic closure.
  `file.read('*a')` reads all remaining data; `io.readAll()`/`io.writeAll()` handle whole files. Dot calls pass the
  file receiver automatically.
- Async work on the same object runs in FIFO order; different objects can run in parallel. Completion callbacks
  require message processing and run on the main thread. Action/method callbacks receive
  `ActionID, Object, Error, Key`, unlike the single-object callback for `async.script()`.
- `async.wait(ObjectOrArray, [Seconds])` pumps messages until queued and active work completes; check its `ERR`
  result. `async.pending(Object)` counts both, and `async.cancel(ObjectOrArray)` interrupts/drains work.
- `async.pool` shares string-keyed numbers, strings, booleans, and object IDs between worker states. `nil` deletes;
  tables/functions/arrays require serialization. Stored object IDs do not pin objects. Individual reads/writes are
  synchronized; do not assume a compound read-modify-write is atomic.
- `processing.new()` requires `signals=array<obj> { ... }`, with `mode='all'` or `'any'`. Reuse requires clearing
  signal flags as appropriate; `proc.signal()` signals the script and cannot wake a custom list excluding it.
- `processing.collect()` replaces Lua's `collectgarbage()`. Entering nonzero `processing.sleep()` can collect;
  retain object ownership needed across event processing instead of relying on delayed garbage collection.
