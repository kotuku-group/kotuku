# Tiri Module Registry

## Purpose and Ownership

The module registry is the process-wide owner of native modules used by Tiri.  `glModuleRegistry` owns each
`ModuleBinding` exactly once.  A binding owns:

- the loaded `objModule`;
- an immutable signature and its collision-safe function index;
- stable `ModuleCallable` records containing native addresses and precomputed dispatch and marshalling metadata,
  including a prepared `ffi_cif` for each typed signature; and
- the state and result of publishing the module's constants and structures.

The registry index is non-owning.  It maps case-insensitive hashes to entries that retain the indexed spelling, so
every lookup revalidates the name and cannot mistake a hash collision for a match.  Bindings and callables are
allocated individually and remain at stable addresses until registry expunge.

## One Representation of Function Metadata

The immutable signature is the sole owner of each function's canonical name and copied `FunctionField` metadata.
Compile-time queries read those copies, and so does every `ModuleCallable`: a callable's `Name`, and its `Fields` when
present, point into its binding's signature rather than into the loaded module's original `Function` list (`Fields`
remains null for an untyped no-argument export).  That list is consulted only while the binding is being constructed,
as the source of the native addresses the signature deliberately does not retain.  The metadata therefore does not
depend on the loaded module's descriptor storage, and a name or field descriptor cannot drift between the compiler's
view and the runtime's.

Function lookup is likewise single.  `ModuleBinding::Callables` is parallel to `static_module_signature::Functions`:
element *i* describes the same export in both, because both are built in the module's export order.  Runtime lookup
calls `find_ordinal()` on the signature — the same index the compiler queries — and then addresses `Callables`
directly.  There is no second runtime hash map, so the two paths cannot disagree about which export a name selects.
The `module_registry` unit test asserts that agreement, and that a callable's name and fields are the signature's own
storage rather than a copy.

## Lifecycle and Publication

Resolution follows a construct-then-publish sequence:

1. Look up the requested name while holding `ModuleRegistry::Mutex`.
2. Release the mutex before `objModule::create`, signature copying and callable preparation.  Module initialisation is
   external code and may re-enter Tiri.
3. Reacquire the mutex and repeat the lookup.  If another thread published the module first, discard the newly built
   candidate after releasing the mutex.  If expunge began while construction was in progress, reject publication and
   discard the candidate instead.
4. Otherwise publish the complete binding, then index both its canonical name and requested spelling.

No partially prepared binding is observable.  Failed module creation is not published or cached, so a later request
may retry.  Once a binding exists, definition loading stages its constants and structures before committing the batch
under `glConstantMutex`.  A failed commit rolls back entries from that batch and records the error on the binding;
later requests return the same failure rather than exposing a partial definition set.

`MODExpunge()` first prevents Tiri states from executing and releases the Tiri class.  `expunge_modules()` then marks
the registry permanently expunging, clears the non-owning index and moves the owned bindings out while holding the
registry mutex.  The flag prevents an in-flight resolver from publishing after teardown begins.  The bindings are
destroyed after the mutex is released because `FreeResource()` invokes external module teardown code that may re-enter
registry-facing paths; such resolution is rejected with `ERR::InvalidState`.

## Locking

| Lock | Protects | Rules |
|---|---|---|
| `ModuleRegistry::Mutex` | Binding ownership, name index and publication | Hold only for lookup or publication.  Never call module creation or destruction while held. |
| `glConstantMutex` | Global Tiri constants, module definition batches and each binding's definition state | Use shared access for compiler reads and exclusive access for definition publication. |
| `glStructMutex` | The global structure registry | Definition commit and rollback hold it while `glConstantMutex` is held, preventing readers from observing structures that may be withdrawn. |

The registry mutex is not nested with either definition lock.  Resolve and publish a binding, release the registry
mutex, and only then acquire `glConstantMutex` to process definitions.  A definition commit acquires `glStructMutex`
inside `glConstantMutex`; code holding either definition lock must not perform registry lookup.  Registry work therefore
completes before definition work begins, with no simultaneous ownership.

Lua state access is outside this global lock hierarchy.  A state must be entered according to the normal Tiri runtime
contract before its prototypes, stack or closures are touched.  Registry locks never protect Lua GC objects.

## Prototype and State Boundary

The compiler records only canonical module and function names in each prototype's dependency descriptors.  At
`BC_MODACT`, the runtime resolves one descriptor through the global registry and stores non-owning `ModuleCallable *`
pointers in a sidecar owned by that prototype.  A separate activation byte marks descriptors that have already been
resolved.  Resolution is descriptor-local so failures occur at the source declaration's execution position.

The sidecar is allocated by the owning Lua state and freed with the `GCproto`.  It owns no module data and is never
placed in a process-wide map.  Each activation materialises state-owned C closures in the destination registers; each
closure retains one stable callable pointer and uses the shared `module_call_inner()` dispatcher.

This boundary is deliberate:

- native modules, signatures, callable metadata and definition publication are process-wide;
- dependency names are portable prototype metadata;
- resolved slots are prototype-owned, non-owning process pointers; and
- closures, arguments, results, callbacks, errors and temporary marshalling storage are Lua-state-owned.

Native-module dependency descriptors in the current serialised bytecode format (`0xa9`) carry canonical names and
`BC_MODACT` operands, never native pointers or export-list indices.  Older formats are rejected rather than retaining
compatibility shims, including one for the removed compiler-private dependency binder.

## Invocation Storage and Output Ownership

Published callable metadata contains no invocation storage.  Eligible void and scalar signatures use preselected
direct dispatch: untyped no-argument functions, typed zero-argument functions and typed functions with one to four
exact `FD_INT`, `FD_INT64` or `FD_DOUBLE` inputs.  Other supported typed signatures use the cached-CIF bridge.  The
bridge uses aligned eight-byte argument/result slots inside its 256-byte buffer, plus exact-type bounded stores for
C++ temporaries.  Preparation and result traversal use the same padding rules.

The bridge holds native owners outside `protected_tiri_call()`, which uses a protected runtime C frame without adding
another Lua call frame or changing the closure upvalue.  Caller try handlers are saved and suspended while the bridge
runs; checkall's immediate-scope identity remains intact.  On an error, the runtime returns to this boundary before
native storage is released, then the original error is rethrown.  Ownership is cleared after a copied allocation is
freed or a GC wrapper successfully adopts it.  The same protection releases structure conversion's temporary registry
references.  This does not depend on platform-specific C++ destructor unwinding.

For a non-void function, the native return value is the first Tiri result.  Each `FD_RESULT` parameter then contributes
one result in descriptor order.  Result slots begin zero/null, so an error path that leaves an output untouched exposes
that initial value.  Unchecked calls retain native error results and any outputs produced by the native function; `check`
and immediate-scope `checkall` promote qualifying native errors without invoking the native function again.

Output conversion preserves established representations rather than normalising them to native-return semantics:
unsigned native scalar returns are pushed as numbers, whereas `FD_INT` output slots use the legacy integer conversion;
64-bit values retain the runtime's numeric precision limits.  A null plain-pointer native return is `nil`, while a null
plain-pointer output slot remains light userdata.  Strings and structures are copied unless their descriptor specifies
resource ownership.  An allocated object or resource transfers ownership to its GC wrapper only after that wrapper is
successfully pushed; every allocation not transferred is released exactly once, including after a native error or a
later conversion failure.

Signatures with `FD_RESULT` parameters remain on cached CIF; direct dispatch is limited to calls without output
parameters.  A callable contains no Lua-state pointer, whichever dispatch path it uses.
