---
description: "Commit to the Kōtuku repo using the correct message format"
---

# Commit to Kōtuku

Write a commit message for the currently staged changes in Git and commit.  If there are no changes staged, inform the user and stop.

Use this template in constructing your Git message:

```
[Label] Single line summary

* [Sub-Label] Describe isolated change A
* [Sub-Label] Describe isolated change B
```

For example:

```
[Tiri] Add bulk TValue operations with AVX2 acceleration for arrays and tables

* [Tiri] Introduce lj_bulk module providing vectorised nil-fill, copy and memmove for TValue arrays
* [Tiri] Runtime CPUID detection selects the AVX2 path on capable hardware and falls back to scalar otherwise
```

Rules:

* Do not apply word-wrapping.
* If listing a series of changes, use asterisk based bullet points, one on each line.
* Do not add credit or authorship attributions for yourself or others.
* Low-value changes do not need to be mentioned.  For example: comments, decorative changes, file renaming, minor refactoring, and anything that does not make a fundamental difference to programming logic.

Note: If currently in the `master` or `main` branch, create a new branch under `test/[name]` with a relevant name related to the changes and commit to that target.

The `Label` is the most appropriate single-word label that categorises the most valuable changes being submitted.  For instance, if the most valuable changes are in the `Core` module, then `Core` is the appropriate label.  This rule is true of all module-based changes.  The following labels may be appropriate in other circumstances:

* Script: For changes to Tiri scripts, such as in the `scripts/` and `examples/` folders.
* Build: For changes to CMake files
* AI: For updates to agent configuration files
* Doc: For document specific updates

If no label seems appropriate, do not include a label.

Sub-Labels are specific to the change being documented.  If all Sub-Label entries are identical and share the same name as the Label, the Sub-Labels can be omitted as they do not provide additional value.

Push to remote after completing your commit.
