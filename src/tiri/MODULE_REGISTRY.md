# Tiri Module Registry

## Purpose and Ownership

The module registry is the process-wide owner of native modules used by Tiri.  `glModuleRegistry` owns each
`ModuleBinding` exactly once.  A binding owns:

- the loaded `objModule`;
- an immutable compiler signature and its collision-safe function index;
- stable `ModuleCallable` records containing native addresses, argument metadata and prepared `ffi_cif` values; and
- the state and result of publishing the module's constants and structures.

The registry index is non-owning.  It maps case-insensitive hashes to entries that retain the indexed spelling, so
every lookup revalidates the name and cannot mistake a hash collision for a match.  Bindings and callables are
allocated individually and remain at stable addresses until registry expunge.

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
| `glConstantMutex` | Global Tiri constants, global structure publication and each binding's definition state | Use shared access for compiler reads and exclusive access for definition publication. |

The two locks are not nested.  Resolve and publish a binding, release the registry mutex, and only then acquire
`glConstantMutex` to process definitions.  Code holding `glConstantMutex` must not perform registry lookup.  This
separation is the lock order: registry work completes before definition work begins, with no simultaneous ownership.

Lua state access is outside this global lock hierarchy.  A state must be entered according to the normal Tiri runtime
contract before its prototypes, stack or closures are touched.  Registry locks never protect Lua GC objects.

## Prototype and State Boundary

The compiler records only canonical module and function names in each prototype's dependency descriptors.  At
`BC_MODACT`, the runtime resolves one descriptor through the global registry and stores non-owning `ModuleCallable *`
pointers in a sidecar owned by that prototype.  A separate activation byte marks descriptors that have already been
resolved.  Resolution is descriptor-local so failures occur at the source declaration's execution position.

The sidecar is allocated by the owning Lua state and freed with the `GCproto`.  It owns no module data and is never
placed in a process-wide map.  Each activation materialises state-owned C closures in the destination registers; each
closure retains one stable callable pointer and uses the shared `module_call_inner()` marshaller.

This boundary is deliberate:

- native modules, signatures, callable metadata and definition publication are process-wide;
- dependency names are portable prototype metadata;
- resolved slots are prototype-owned, non-owning process pointers; and
- closures, arguments, results, callbacks, errors and temporary marshalling storage are Lua-state-owned.

Serialised bytecode format `0x87` carries names and `BC_MODACT` operands, never native pointers or export-list indices.
Older formats are rejected rather than retaining the removed compiler-private dependency binder.
