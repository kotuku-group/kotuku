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
[Tiri] Added bulk TValue operations with AVX2 acceleration for arrays and tables

* Introduced lj_bulk module providing vectorised nil-fill, copy and memmove for TValue arrays
* [Doc] Updated AGENTS.md with information on how to optimise for AVX2
```

Rules - these take precedence over prior statements:

* Never apply word-wrapping.
* If listing a series of changes, use asterisk based bullet points, one on each line.
* Never add credit or authorship attributions for yourself or others.  For example, a `Co-Authored-By` statement is not permitted.
* Low-value changes do not need to be mentioned.  For example: comments, decorative changes, file renaming, minor refactoring, and anything that does not make a fundamental difference to programming logic.
* If the Sub-Label would match the parent Label, the Sub-Label should not be applied.

Note: If currently in the `master` or `main` branch, create a new branch under `test/[name]` with a relevant name related to the changes and commit to that target.

PowerShell caveat: Passing a multi-line message via `git commit -m @'...'@` only works when the opening `@'` and closing `'@` each sit alone on their own line at column 0.  Writing `-m @'...` on the same line as `git commit` causes the leading `@` to leak into the commit subject.  To avoid this, write the message to a temporary file and commit with `git commit -F <file>` (then delete the file).

The `Label` is the most appropriate single-word label that categorises the most valuable changes being submitted.  For instance, if the most valuable changes are in the `Core` module, then `Core` is the appropriate label.  This rule is true of all module-based changes.  The following labels may be appropriate in other circumstances:

* Script: For changes to Tiri scripts, such as in the `scripts/` and `examples/` folders.
* Build: For changes to CMake files
* AI: For updates to agent configuration files
* Doc: For document specific updates

If no label seems appropriate, do not include a label.

Push to remote after completing your commit.

## Note

Do not compile, test or perform any verification of the staged files.
If Git returns a pull-request URL after the commit, always report it back to the user as a clickable link.
Changes to files in the workspace are not permitted during a review.
