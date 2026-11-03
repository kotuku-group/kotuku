# VectorText WrapMode — Implementation Plan

Created 2026-11-02.  Status: implemented 2026-11-03, apart from the optional dialog follow-up (step 9).  Prerequisite
fixes landed 2026-11-02 (see Background).  See Implementation Status at the end of this file.

## Goal

Add a `WrapMode` field to VectorText that controls where `InlineSize` word-wrapping may break a line.  The default
keeps today's behaviour.  Two new modes let long tokens such as file paths and URLs wrap at symbols like `/`, and
break between characters as a last resort, so that no line overflows the wrapping area.

A practical target is `packages/dialog/dialog.tiri`, which carries its own `wrapText()` and `fitLength()` helpers
because VectorText cannot break long words.  With `WrapMode = ANYWHERE` and the `WrappedLines` field, the dialog can
hand wrapping to VectorText.

## Background

- **Wrapping only happens at whitespace.**  Both layout loops (`generate_text()` in
  `src/vector/vectors/text_path.cpp` and `raster_text_to_bitmap()` in `src/vector/vectors/text_bitmap.cpp`) use the
  `WS_NO_WORD` / `WS_NEW_WORD` / `WS_IN_WORD` state machine (`src/vector/vector.h:1749`).  The wrap test runs at the
  first character of each word, and `word_extent()` (`text.cpp`) measures up to the next character `<= 0x20`.
- **Oversized words no longer leave a blank line.**  Fixed 2026-11-02: the wrap test now requires `dx > 0` in both
  loops.  A word wider than `InlineSize` overflows the line it starts on.  Covered by
  `OversizedWordsDoNotLeaveBlankLines` in `src/vector/tests/test_text_wrap.tiri`.
- **`WrappedLines` exists.**  Added 2026-11-02: a read-only field that returns `TotalLines` plus the lines created by
  wrapping.  It regenerates the layout when it is out of date.  The count is held in `txWrappedLines`, which both
  layout loops maintain.  Covered by `WrappedLinesCountsWordWrapping`.
- **Bitmap fonts wrap twice, by two different implementations.**  `raster_text_to_bitmap()` computes caret geometry,
  the line count and the bitmap height with VectorText's own loop.  It then draws each logical line through the Font
  class with `WrapEdge = InlineSize` (`text_bitmap.cpp:120-137`), and the Font class wraps again with its own
  whitespace-only `string_size()` (`src/font/font.cpp:272`).  The two only agree by coincidence.  Any new break rule
  that is added to VectorText alone will make bitmap text draw differently from its carets and boundary.
- **Guide paths do not wrap.**  The guide-path branch of `generate_text()` ignores `InlineSize`, except in
  `VMF::STRETCH` mode where the guide is dropped and the normal loop runs.
- **Carets, hit testing, selection and vertical caret movement follow `TextLine::chars`.**  These are built by the
  same loops that make the wrap decision, so new break points should carry through to them automatically.
  `visual_vertical_position()` and `hit_test()` both rely on the visual rows that the loops produce.
- **Word navigation is separate.**  Ctrl+Left/Right and double-click selection use `word_boundary()` and the
  generated `text_word_ranges.h` tables.  They must not change with the wrap mode.
- **Unicode line breaking (UAX #14) is out of scope.**  A full implementation needs ICU or large generated tables.
  The fixed rule set below covers paths, URLs, identifiers and hyphenated words, which is the need today.

## Design

### The `WrapMode` field

A new enum in `src/vector/vector.tdl`:

```lua
  enums("VTW", { type="int", start=0, comment="VectorText wrapping modes, used when InlineSize is set." },
    "WORD: Lines break at whitespace only.  A word that is wider than the wrapping area overflows it.  This is the default.",
    "SYMBOL: As for WORD, and lines may also break after the symbols listed in the WrapMode documentation, such as the / in a file path.",
    "ANYWHERE: As for SYMBOL, and a segment that is too wide for a line of its own is broken between characters.")
```

Field table entry in `text.cpp`, next to `InlineSize`:

```cpp
   { "WrapMode", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, TEXT_GET_WrapMode, TEXT_SET_WrapMode, &clVectorTextVTW },
```

- Add `'VTW'` to the `references` list of `klass('VectorText', ...)` so the lookup table is generated.
- Store the mode in `VTW txWrapMode = VTW::WORD;`.  The setter validates the range, stores the value and calls
  `reset_path()`, as `InlineSize` does.
- `WrapMode` has no effect when `InlineSize` is 0.  The documentation must say so, and list the break symbols.

The ANYWHERE fallback matches CSS `overflow-wrap: anywhere`: characters are only split when a segment cannot fit on
an empty line.  It is not `word-break: break-all`, which splits every word that reaches the edge.

### Break rules

A **segment** is the unit that the wrap test measures and moves to a new line.  In WORD mode, a segment is a word,
exactly as today.  In SYMBOL and ANYWHERE modes, a word is split into segments after each break symbol:

| Symbol | Example | Breaks as |
|--------|---------|-----------|
| `/` `\` | `/usr/share/kotuku` | `/usr/` · `share/` · `kotuku` |
| `-` | `well-known` | `well-` · `known` |
| `_` | `field_name_here` | `field_` · `name_` · `here` |
| `?` `&` `=` | `page?id=4&x=2` | `page?` · `id=` · `4&` · `x=` · `2` |
| `.` `,` `:` `;` | `example.com` | Not break symbols.  Splitting `3.14`, `12:30` and file extensions is worse than overflowing. |

A break after a symbol is suppressed when:

1. The next character is the end of the word.  A trailing symbol stays with its word.
2. The next character is also a break symbol, so runs like `//`, `--`, `\\` and `-->` stay together.
3. The symbol is a `-` at the start of the word, as in `-5` and `--log-warning`.  (The `-` between `log` and
   `warning` can still break.)
4. The symbol is a `-` followed by a digit, so `x-1` stays together.

The symbol always stays at the end of the first line.  No hyphen is inserted at a break.

### Shared segment helper

Both layout loops currently duplicate the wrap logic.  Replace `word_extent()` at the wrap-test call sites with a
single helper in `text.cpp` (both loops are `#include`d into it):

```cpp
// Returns the byte and character length of the segment starting at String, following the break rules for Mode.
static WordExtent segment_extent(const char *String, VTW Mode, bool WordStart);
```

- In WORD mode it returns the same result as `word_extent()`.
- `WordStart` is true when the previous character was whitespace or the start of the line.  It is needed for rule 3.
- Keep `word_extent()` for any callers that are about words rather than wrapping.

The state machine changes from "new word" to "new segment": a character starts a segment if the previous character
was whitespace, or if the previous character ended a segment under the break rules.  The cleanest way is for each
loop to track `segment_end`, the byte offset where the current segment ends (from `segment_extent()`).  When the loop
reaches it, the next non-space character is a segment start and the wrap test runs.

The wrap test is unchanged apart from the measured width: `(dx > 0) and (dx + segment_width >= InlineSize)`.

### Character fallback (ANYWHERE)

After the segment test, if the mode is ANYWHERE and the segment is still wider than `InlineSize` from the start of
the line, the loop switches to per-character checks until the segment ends:

- Before drawing each glyph, wrap if `dx > 0` and `dx + char_width > InlineSize`.
- At least one character is always placed on a line, so a single glyph wider than the area cannot loop forever.
- Break between UTF-8 characters, never inside one.  Combining marks are not handled specially (a known limitation,
  to be recorded in the field documentation).

Each wrap increments `wrapped_lines` (and `txWrappedLines` for bitmap fonts), so `WrappedLines` and the boundary
height stay correct without further changes.

### Kerning

In `generate_text()`, reset `prev_glyph_index` to 0 when a wrap occurs.  Today kerning is only reset on whitespace,
which was enough when wraps only happened after whitespace.  A symbol or character break would otherwise apply the
kerning pair from the end of one line to the start of the next.

### Bitmap fonts draw the lines VectorText computed

Stop using the Font class's `WrapEdge` in `raster_text_to_bitmap()`.  Instead:

1. During the layout loop, record the byte offset of each wrap in the `TextLine`, e.g.
   `std::vector<int> wrapOffsets;`, cleared in `invalidate_text()` along with `chars`.
2. When drawing, split each logical line at its `wrapOffsets` and draw each part with `WrapEdge = 0` at its own `y`,
   advancing by `LineSpacing`.
3. Trailing whitespace before a wrap stays with the earlier part, matching the caret layout.

This gives a single wrapping implementation for both font types, and removes the existing reliance on the two
implementations agreeing.  The Font class itself is not changed.

The scalable-font loop can record `wrapOffsets` too.  It doesn't need them for drawing, but they would make the
wrapping information cacheable, which the TODO comments in both loops ask for.  Recording them in both loops is
cheap, so do it for consistency.

### SECRET text

Confirm how `VTXF::SECRET` text is laid out before implementing.  If the layout loops see the real characters, a
symbol break would reveal where the `/` and `-` characters are.  If so, treat SECRET text as WORD mode whatever
`WrapMode` is, and document this.

## Implementation Steps

1. **TDL and field.**  Add the `VTW` enum, the `references` entry, `txWrapMode`, the getter and setter, the field
   table entry and the `-FIELD-` documentation (including the symbol table and the no-effect-without-`InlineSize`
   note).  Cross-reference `WrapMode` from the `InlineSize` documentation.
2. **Regenerate headers.**  Field-table changes shift the dictionary indexes, so follow the full sequence: build and
   install, touch `src/vector/vector.tdl`, run `cmake --build --preset agents-headers`, then rebuild and install.
   Revert `include/kotuku/modules/core.h` if only `CORE_BUILD_DATE` changed.
3. **Segment helper.**  Add `segment_extent()` with the break rules.  Switch both layout loops to segment-based wrap
   tests.  WORD mode must give exactly the same layout as today; run the existing vector tests to confirm before going
   further.
4. **Kerning reset** at wraps in `generate_text()`.
5. **ANYWHERE fallback** in both loops.
6. **Bitmap drawing.**  Add `TextLine::wrapOffsets`, record them in both loops, and draw bitmap text per visual line
   with `WrapEdge = 0`.
7. **SECRET check** and the resulting rule.
8. **Tests** (see below).
9. **Dialog follow-up** (separate change, optional): replace `wrapText()` and `fitLength()` in
   `packages/dialog/dialog.tiri` with `inlineSize = text_width`, `wrapMode = VTW_ANYWHERE`, and `#lines` with
   `&messageText.wrappedLines`.  `widest_line` is still measured with `mVec.StringWidth()` to choose the column width,
   so the font handle stays.  Run the dialog's GUI tests (`packages/tests/test_gui_dialog.tiri`).

Use a `Release` build only if the wrap loops need profiling; the extra checks are per segment and per character in
ANYWHERE mode only, so no measurable cost is expected for WORD mode.

## Tests

Add to `src/vector/tests/test_text_wrap.tiri`.  Every test runs with both `Noto Sans` and the `Clean` bitmap face,
following the pattern of `OversizedWordsDoNotLeaveBlankLines`.  Measure with `wrappedLines`, `mtGetCaretBounds()`
and `mtGetBoundary()`.

| Test | Checks |
|------|--------|
| `WrapModeDefaultsToWord` | Default is `VTW_WORD`; a path that doesn't fit overflows; layout identical to today's. |
| `WrapModeIsIgnoredWithoutInlineSize` | SYMBOL and ANYWHERE have no effect when `inlineSize` is 0. |
| `SymbolModeBreaksAfterSlash` | `/usr/share/kotuku/fonts` wraps after a `/`; the `/` ends the first line; the next line starts at x = 5. |
| `SymbolModeKeepsSymbolRunsTogether` | `//` and `--` never split. |
| `SymbolModeRespectsLeadingHyphenAndDigits` | `--log-warning` doesn't break after the leading `--`; `x-1` doesn't break. |
| `SymbolModeLeavesPeriodsAlone` | `example.com` and `3.14` don't break. |
| `SymbolModeStillOverflowsOversizedSegments` | A segment with no break symbol that is wider than the area overflows. |
| `AnywhereModeBreaksBetweenCharacters` | The same segment is split, no line is wider than `inlineSize`, and `wrappedLines` is correct. |
| `AnywhereModeHandlesNarrowAreas` | `inlineSize` narrower than one glyph places one glyph per line without hanging. |
| `AnywhereModeKeepsUTF8Whole` | A run of `é` splits only between characters (check caret columns). |
| `WrapModeChangeRefreshesLayout` | Changing `wrapMode` after drawing updates `wrappedLines` and the boundary. |
| `BitmapDrawingMatchesCaretLayout` | For `Clean`, draw to a bitmap and check that pixels exist on each visual line at the caret positions, and not past `inlineSize`. |

Also extend the editing tests in `test_text_selection_keyboard.tiri` and `test_text_selection_mouse.tiri` with one
case each for a SYMBOL-mode path: Up/Down caret movement across the symbol break, and a click on the continuation line
placing the caret correctly.

Run `ctest --preset agents -L vector` after each implementation step.  The GUI package tests use VectorText heavily,
so run `ctest --preset agents` before finishing.

## Risks and Open Questions

- **Bitmap drawing change (step 6) is the largest risk.**  It changes how all wrapped bitmap text is drawn, in every
  mode, including WORD.  The pixel test in the table above is the guard.  If the Font class's per-line output differs
  in ways the test doesn't catch (alignment, `Leading`, `EndY`), compare captures of wrapped `Clean` text before and
  after the change.
- **Alignment.**  `ALIGN::RIGHT` and `ALIGN::HORIZONTAL` currently offset the whole text by `txWidth`, not each line.
  This plan doesn't change that, but new break points make lines more uneven, so the limitation will be more visible.
- **The break symbol list is a judgement call.**  It is defined in one place (`segment_extent()`), so it is cheap to
  adjust.  Confirm the list, especially `_`, `=` and `&`, before implementing.
- **Should `WrapMode` also affect Font's `WrapEdge`?**  Not in this plan.  The Font class keeps its whitespace-only
  wrapping; VectorText simply stops relying on it.

## Implementation Status (2026-11-03)

Steps 1-8 are complete.  Step 9 (the dialog follow-up) was not started; it remains a separate, optional change.

- **Field and TDL.**  `VTW` enum (`WORD`, `SYMBOL`, `ANYWHERE`) added to `vector.tdl` and referenced by VectorText.
  `WrapMode` is stored in `txWrapMode`; the setter rejects out-of-range values with `ERR::OutOfRange` and only resets
  the path when the value changes.  Full `-FIELD-` documentation, including the symbol table, is in `text.cpp`, and
  `InlineSize` cross-references it.  Headers and `vectortext.xml` were regenerated.
- **Segment helper.**  `segment_extent()` and `is_break_symbol()` in `text.cpp` implement the break rules as listed.
  Both layout loops track `segment_end` instead of the old `WS_*` state machine, which was removed from `vector.h`.
  WORD mode is identical to the old behaviour by construction: `segment_extent()` returns `word_extent()`, so segment
  starts only occur at word starts.
- **Kerning.**  `prev_glyph_index` is reset at every wrap in `generate_text()`.
- **ANYWHERE.**  Per-character fallback implemented in both loops as designed.
- **Bitmap drawing.**  The prerequisite change had already stopped the Font class from wrapping (`WrapEdge = 0`), using
  a local list of visual lines.  This was replaced with `TextLine::wrapOffsets`, recorded by both loops and cleared in
  `invalidate_text()`; the bitmap path draws each logical line split at those offsets.
- **SECRET.**  The layout loops see the real characters (SECRET only affects copying), so SECRET text always uses WORD
  mode through `extVectorText::effective_wrap_mode()`.  Documented in the field.
- **Bug found during testing.**  `ft_point::get_glyph()` returns a reference into a cache that `string_width()` can
  grow, so the glyph reference must be fetched after the wrap test.  The per-character check uses a separate lookup.
- **Tests.**  `src/vector/tests/test_text_wrap.tiri` (registered as `vector_text_wrap`) has 17 tests.  The
  `test_text_wrap.tiri`, `test_text_selection_keyboard.tiri` and `test_text_selection_mouse.tiri` files referenced
  above did not exist in the repository, so the new file was created from scratch, and the Up/Down caret and click
  cases were added to it (`VerticalCaretMovementCrossesSymbolBreaks`, `ClickOnContinuationRowPlacesCaret`) instead.
  An extra `SymbolModeBreaksURLQueries` test covers `?`, `=`, `&` and `_`, and `SecretTextWrapsAtWhitespaceOnly` and
  `WrapModeRejectsInvalidValues` cover the SECRET rule and setter validation.
- **Results.**  `ctest --preset agents -L vector`: 36/36 passed.  Full `ctest --preset agents`: 357/365 passed; the 8
  failures are network and WebRTC tests that need IPv6 and DNS, which the cloud container lacks.

Open points unchanged from above: `-->` stays together only as far as rule 2 allows (`--` is never split, but a break
may follow the run before `>`), and per-line alignment remains out of scope.
