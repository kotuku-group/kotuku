# Font picker package

This directory implements the `gui/fontdialog` Tiri package, a prototype of the font dialog shown in the wireframe at
`docs/plans/Kotuku Font Dialog Mockups/FontDialog.dc.html`.  `package.json` declares `fontdialog.tiri` as the entry
point.  The picker is used through `import 'gui/fontdialog'` and `gui.dialog.fontdialog(...)`;
`examples/fontdialog.tiri` opens it.

## Design

The dialog must look like the colour picker.  It does not copy the colour picker's styling: it imports
`gui/colourpicker/theme` and `gui/colourpicker/parts`, which the colour picker exports for this purpose.

- `colourpicker_theme.apply(Scene, 'fontdialog')` derives the window theme and registers it as `fontdialog-<Base>`,
  so that a colour picker opened from the Fill or Stroke panel keeps its own `colourpicker-<Base>` theme.  Each
  dialog removes only its own entry when its window is freed.
- Panels are `gui.panel()` widgets with `colourpicker_parts.PANEL_PADDING`, and use the picker tokens
  (`picker.tokens`) for text and borders.  Sliders use `colourpicker_theme.channelSlider()` and
  `colourpicker_parts.thumbRings()`, and the footer uses `colourpicker_parts.hints()`.
- The layout constants follow the colour picker: 14 px at the window edges, 12 px between panels, a 42 px footer.

## Sizing

The layout is `CLIENT_WIDTH` units wide.  The client viewport's view box is scaled to the width of the window, and
its height follows the window, so a vertical resize reflows the layout rather than scaling it: `layout()` gives the
extra height to the font list, the Preview panel and the right column's scrolling stack, and keeps the Sizing and
Style panels and the footer at the bottom.  The layout does not expand horizontally; a change of width changes the
scale.  Below `MIN_CLIENT_HEIGHT` the layout stops reflowing and scales down to fit.

The window has no fixed aspect ratio.  Its zoom keys are redirected to `setScale()`, which resizes the window to the
new scale while keeping the layout's height, within the monitor.  The window's own view box zoom is not used.

## Source layout

| File | Responsibility |
| --- | --- |
| `fontdialog.tiri` | Dialog entry point.  Owns the picker state, the window and three-column layout and its vertical reflow, the right column's scrolling stack, the footer, the dialog keys and Select/Cancel/Reset.  Panels change the state through `update(Patch, Source)`; `refresh(Source)` then calls every panel's `update(Source)`, so a panel can skip the control that made the change. |
| `catalogue.tiri` | The installed families, read from `gui.fontList`, with the font database's classification (serif, sans, mono, script, display, symbol), axis ranges, features and coverage, plus technology (scalable, bitmap) and list meta.  The only file that calls the Font module (`SelectFont()` for the Glyph report's format). |
| `controls.tiri` | Shared builders: slider rows, segmented button groups (radio or multi, optionally wrapping), check rows, colour buttons (which open `gui.dialog.colour`), labels and dividers. |
| `fontlist.tiri` | Left column: search, classification and technology filters, the `gui.listView` of families with custom rows (name drawn in its own face, star, meta) and section heading rows, pins, and the preset / Save / Install / Remove row. |
| `preview.tiri` | Centre column: the scrolling Preview panel at the top of the column, with the Paragraph / Waterfall and Compare / Edit text groups in a footer styled as the panel header.  Compare freezes a copy of the settings; clicking the frozen pane swaps it with the live settings.  Edit text makes the live paragraph an editable VectorText (shown without the letter case applied) and shows the sample text buttons in the footer; Waterfall turns editing off. |
| `sizing.tiri` | Sizing panel: size (snapped to a bitmap family's strikes), line height, tracking, word spacing, kerning, and the px / pt switch in the header. |
| `style.tiri` | Style panel: axis sliders (ranges from the font database) and the italic switch for variable families, style buttons for static and bitmap families, and switches for the common OpenType features that the family provides.  Rebuilt when the family changes.  Also composes the VectorText `fontStyle` for the state. |
| `glyphs.tiri` | Glyph report panel: family, file format, character count and the coverage bars of the font database's character sets.  Its height follows the number of sets shown and asks the picker to restack the column. |
| `fill.tiri`, `stroke.tiri`, `effects.tiri`, `alignment.tiri` | Right column panels.  Effects grows when an effect is enabled and asks the picker to restack the column. |

## Prototype status

Working: the font list (search, filters, pins, selection, keyboard navigation), the preview (paragraph, waterfall,
compare, editing the sample text in place), size, the variable axes weight and width, italic, static and bitmap styles, fill
colour, upper and lower case, presets, and Select / Cancel / Reset.

Mockups that record their settings without rendering them: line height, tracking, word spacing, kerning, other
variable axes, OpenType features, opacity, stroke, effects, alignment, title case and paragraph spacing.  Save,
Install and Remove flash a message.

Known gaps found while building the prototype:

- VectorText has no settable line spacing or letter spacing, and it aligns a wrapped block as a whole rather than
  line by line, so line height, tracking and alignment can't be previewed yet.
- The classification, axis ranges, OpenType features and character set coverage come from the font database (see
  `RefreshFonts()`), through `gui.fontList`.  A family that the database cannot classify has no class and is hidden
  by the classification filters.  The filters offer serif, sans and mono only, so script, display and symbol families
  are also only listed when no classification filter is active.  The weight axis defaults to 400 where the range
  allows, rather than the font's default instance, matching the Regular default of static families.
- `string.upper()` is byte-based, so upper case does not convert non-ASCII letters such as `ō`.
- Pins and presets are not saved.
