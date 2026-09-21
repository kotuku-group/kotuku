# Tiri Version Policy

## Promises

- 1.x source that matches documented behaviour continues to work throughout 1.x if not affected by bug fixes.
- Bytecode and internals are excluded.
- Breaking changes are saved for the next major release except in cases illustrated under "Bug Fixing Policy"

## Syntax Additions

Within Tiri 1.x:

- New syntax may be added in a minor release only when every valid earlier program retains the same parse and meaning.
- New globally reserved words should normally wait for the next major release. Prefer contextual keywords where they are unambiguous.
- Changes to precedence, tokenisation, scoping, evaluation order, or an existing construct’s meaning are breaking changes.
- Removed syntax may remain temporarily recognised solely to produce a targeted migration diagnostic.

## Standard-library Changes

Tiri libraries are distinct from Kōtuku module APIs.  For the Tiri standard library:

- Adding a new function, optional parameter, or result field is normally a minor change.
- Existing argument handling, return values, side effects, errors, and ordering are part of the compatibility contract when documented.
- Removing or incompatibly changing an operation requires a new major language version.
- Names may be deprecated during 1.x, but must continue to work until 2.0 unless retaining them creates a security or correctness hazard.

The definitive standard library documentation is found in the Tiri Reference Manual.

Tiri libraries installed through the indexed `packages:` volume should establish their contracts using document annotations, with expanded details in their associated Wiki page.

## Deprecation Policy

- Every deprecation names its replacement, introduction release, and earliest possible removal release.
- Deprecated facilities remain functional throughout the current major version.
- Warnings are enabled in validation and tooling, but do not make an otherwise valid program fail.
- Suppression should be available for intentionally retained compatibility code.
- Removal normally happens only in the next major version.
- An exception requires a documented security, data-loss, or unmaintainable-runtime justification.

## Bug Fixing Policy

- **Violation of explicit documentation:** fixable in a patch release. The documentation was authoritative, although release notes should call out likely migration impact.
- **Crash, memory corruption, security issue, or data loss:** fixable immediately, even if observable behaviour changes.
- **Unspecified or ambiguous behaviour:** may be defined and changed in a minor release, with release notes and regression tests.
- **Documented behaviour that is merely undesirable:** preserve it during 1.x; replace it through a new API or wait for 2.0.
- **Widely relied-upon accidental behaviour:** provide a transition period or compatibility option even if the strict policy permits an immediate fix.

## Bytecode Policy

Bytecode is tied to the Kōtuku release, not the Tiri version number.  Bytecode is therefore exempt from Tiri policy restrictions.

Users are instructed to regenerate bytecode from source when breaking changes occur.

The private dump format includes semantic `@Package` and transitive `@Dependencies` metadata even when debug information
is stripped.  A loader checks dependency requirements before committing prototypes.  Format and imported-module cache
schema changes intentionally invalidate older generated artefacts; they are rebuilt from source.

Programs can be distributed as compiled bytecode, but this should normally be done in conjunction with a compiled Kōtuku build.

## Release Model

The Tiri version number is exposed in the `_VERSION` string as `major.minor`.

- **Tiri 1.0:** Frozen baseline contract.
- **Tiri 1.x minor:** Compatible syntax and library additions; newly defined formerly-unspecified behaviour.
- **Tiri 1.x patch:** Conformance fixes, safety fixes, and changes with no intended documented semantic impact.
- **Tiri 2.0:** Removals and intentional incompatible semantic changes.
- **Kōtuku release number:** states which Tiri version it implements, but does not replace the independent language version.

## Dependency Declarations

An unprefixed version in the `tiri` field of `@Dependencies` names the language contract that the source expects.  A
runtime satisfies that declaration when it implements the same major language version and its minor version is at least
the declared minor version.  Consequently, `tiri="1.0"` accepts compatible Tiri 1.1 and later 1.x runtimes, but rejects
Tiri 0.x and 2.x runtimes.  A Tiri 1.0 runtime rejects `tiri="1.1"`.

The comparison operators `<`, `<=`, `>` and `>=` retain their numeric meanings and may be used to state explicit bounds.
This compatibility interpretation is specific to the Tiri language domain.  An unprefixed Kōtuku or package version is
an exact requirement because those version domains do not inherit Tiri's same-major compatibility promise.

## Summary

A conforming Tiri 1.x implementation shall continue to accept valid Tiri 1.0 source and preserve its documented observable behaviour. Compatible additions may be made in minor releases; incompatible changes and removal of deprecated facilities require a new major language version.

Private bytecode, undocumented behaviour, implementation details, and separately versioned Kōtuku APIs are outside this guarantee.
