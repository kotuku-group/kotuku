---
name: code-cleanup-auditor
description: Audit Kotuku C++ modules and related TDL/CMake integration for cleanup opportunities, including dead private functions, stale helpers, duplicated implementation patterns, consolidation candidates, avoidable complexity, and structural anti-patterns. Use when the agent is asked to inspect existing Kotuku module code for maintainability cleanup, refactoring candidates, simplification opportunities, or removal of unused code. This skill is not for primary bug hunting.
argument-hint: "<module-or-file-path>"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "TaskCreate", "TaskUpdate"]
---

# Code Cleanup Auditor

Audit existing Kotuku C++ code for low-disruption cleanup work. Prefer evidence-backed, incremental improvements over
large rewrites. Treat public API, generated API hooks, module registration, callbacks, and platform-specific entry points
as live until proven otherwise.

## Audit Boundary

Stay within the requested module, sub-project, or file set. Include nearby files only when needed to prove reachability or
understand an abstraction:

- `src/<module>/CMakeLists.txt`, source files, private headers, platform subdirectories, and tests.
- Relevant `.tdl` files, generated API expectations, `MOD_IDL` strings, exported class/action/method names, and public
  headers.
- Scripts, examples, and Flute tests only when they may call into the audited API.

Do not turn the audit into a bug hunt. If a possible defect appears, record it briefly under "Bug-Sweep Handoffs" and
keep the cleanup audit moving.

## Workflow

1. Confirm the target exists and list the files or directories in scope.
2. Build an entry-point map before declaring anything dead:
   - Module initialisation, registration, and unload functions.
   - TDL-declared classes, fields, actions, methods, constants, and generated callbacks.
   - Public headers, exported symbols, factory functions, callback tables, and platform hooks.
   - CMake source registration, tests, examples, scripts, and documented usage.
3. Search references, favouring exact symbol searches plus nearby naming variants. Check both
   declarations and call sites, and search across all caller surfaces — not just C++:
   - C++ sources and headers in `src/` and `include/`.
   - `.tdl` files for declared classes, fields, actions, methods, and constants.
   - `.tiri` files anywhere under `scripts/`, `examples/`, `tools/`, and module `tests/` directories.
4. Classify cleanup opportunities by type and confidence:
   - **Dead-code candidate**: private/static function, local helper, branch, include, declaration, or data structure with
     no reachable caller after entry-point checks. Before flagging, rule out removal-risk holders that keep a symbol live
     without an obvious call site: macro expansions, `friend` declarations, explicit template instantiations, virtual
     overrides, `extern "C"` exports, weak/visibility-attributed symbols, ODR-fragile inline definitions, and
     registration tables populated at static-init time.
   - **Consolidation candidate**: repeated validation, conversion, allocation, cleanup, logging, option parsing, or
     platform branching that can use an existing helper or a small new helper.
   - **Anti-pattern candidate**: avoidable global coupling, mixed responsibilities, long procedural flows, unclear
     ownership handoffs, repeated manual resource release, duplicated state machines, macro-heavy control flow, or
     abstraction mismatches with neighbouring Kotuku modules.
   - **Stale integration candidate**: CMake entries, comments, documentation markers, tests, or examples that no longer
     match current code shape.
5. Rank findings by practical cleanup value:
   - **High**: high confidence, small blast radius, clearly removes code or collapses repeated logic.
   - **Medium**: useful cleanup but needs focused validation or touches shared internal contracts.
   - **Low**: plausible improvement with limited payoff, style-adjacent cleanup, or uncertain usage.
6. Provide a validation plan for any proposed removal or consolidation. For C++ changes this normally includes building
   the affected module and installing before running relevant tests.

## Kotuku-Specific Cautions

- Do not mark TDL-declared APIs dead solely because C++ references are absent; they may be invoked from Tiri, examples,
  tests, documentation, or dynamic dispatch.
- Do not mark functions dead if they are used by platform loaders, module registration, callback tables, generated code,
  macro expansion, virtual dispatch, or external linkage.
- Generated files and installed build outputs are not cleanup targets unless the user explicitly asks about generated
  artefacts.
- Grep hits inside `include/kotuku/`, `build/`, or any installed output directory are likely generated stubs, not real
  callers. Verify the hit lives in hand-written source before counting it as a live reference.
- Prefer existing local helpers and project idioms before proposing new abstractions.
- Keep mandatory project style in mind when suggesting changes: no `static_cast`, use `and`/`or`, use `IS` instead of
  `==` except where allowed, no C++ exceptions, three-space indentation, British English, and 120-column default width.

## Output Format

Use this report shape unless the user requests direct edits. Cap the detailed write-up at roughly **10–15 of the
highest-value findings**; list any remaining items in a compact "Additional Findings" section as one-line entries
(`file:line - category - one-line rationale`) so the report stays actionable for large modules.

```markdown
# Code Cleanup Audit - <scope>

## Scope
Files/directories inspected, plus notable entry points checked.

## Summary
| Category | High value | Medium value | Low value |
|----------|------------|--------------|-----------|
| Dead-code candidates | N | N | N |
| Consolidation candidates | N | N | N |
| Anti-pattern candidates | N | N | N |
| Stale integration candidates | N | N | N |

## High-Value Cleanup
1. `file:line` - Category; Confidence: High/Medium/Low; evidence; suggested cleanup; expected benefit;
   validation required.

## Medium-Value Cleanup
...

## Low-Value Cleanup
...

## Additional Findings
Brief one-line entries for items not expanded above, when the full report would otherwise exceed ~15 detailed findings.

## Bug-Sweep Handoffs
Possible defects noticed incidentally. Keep brief, or state "None".

## Validation Plan
Builds, installs, focused tests, or manual checks needed before merging cleanup changes.

## Non-Findings
Important symbols checked and intentionally not flagged because they appear to be public API, generated hooks, callbacks,
platform entry points, or otherwise reachable.
```

When no meaningful cleanup opportunities are found, say so directly and list the evidence checked. Avoid filling the
report with speculative style preferences.
