# Regex module

## Valgrind cannot run the SIMD search path

On x86-64, Valgrind stops any process that performs a regex search with `SIGILL`.  The repository builds with `-msse4.2`, so `srell/srell.hpp` defines `SRELL_HAS_SSE42` and `do_search()` uses `pcmpestri` with immediate `0x04`.  Valgrind 3.24 cannot emulate that form of the instruction.  The instruction runs correctly on real hardware, so the `SIGILL` is not a defect in this module.

The failure looks like this:

- Kōtuku reports `Process terminated, signal 04: SIGILL` with `srell::re_detail::re_object::do_search` at the top of the execution path.
- The fault is often followed by `Invalid write` reports from `object_free` during exit.  These are a consequence of the crash handler's exit path, not of the regex code.

Regex searches are common outside this module, because importing the `gui` packages performs them.  **A Valgrind run that ends in this `SIGILL` has not executed the code after the failing search, so a report without errors from that code proves nothing.**  Confirm that the script reached its own output before trusting the result.

To use Valgrind on code that needs regex searches, compile this module with `SRELL_NO_SIMD` defined, which selects the non-SIMD search path.  Do not leave the definition in a committed build, as it slows searches.
