# Font picker package

This directory implements the `gui/fontdialog` Tiri package, the font dialog shown in the wireframe at
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
| `controls.tiri` | Shared builders: slider rows (a drag's changes are coalesced to one per message cycle), segmented button groups (radio or multi, optionally wrapping), check rows, colour buttons (which open `gui.dialog.colour`), labels and dividers. |
| `fontlist.tiri` | Left column: search, classification and technology filters, the `gui.listView` of families with custom rows (name drawn in its own face, star, meta) and section heading rows, pins, and the preset / Save / Delete row. |
| `preview.tiri` | Centre column: the scrolling Preview panel at the top of the column, with the Paragraph / Waterfall and Compare / Edit text groups in a footer styled as the panel header.  Compare freezes a copy of the settings; clicking the frozen pane swaps it with the live settings.  Edit text makes the live paragraph an editable VectorText (shown without the letter case applied) and shows the sample text buttons in the footer; Waterfall turns editing off. |
| `storage.tiri` | Reads and writes the user's pins and saved presets in `user:config/fontdialog.json`, using the colour picker's storage functions.  A file that can't be read or understood is left alone and turns off saving for that dialog. |
| `sizing.tiri` | Sizing panel: size (snapped to a bitmap family's strikes), line height, tracking, word spacing, kerning, and the px / pt switch in the header. |
| `style.tiri` | Style panel: axis sliders (ranges from the font database) and the italic switch for variable families, style buttons for static and bitmap families, and switches for the common OpenType features that the family provides.  Rebuilt when the family changes.  Also composes the VectorText `fontStyle` for the state. |
| `glyphs.tiri` | Glyph report panel: family, file format, character count and the coverage bars of the font database's character sets.  Its height follows the number of sets shown and asks the picker to restack the column. |
| `fill.tiri`, `stroke.tiri`, `effects.tiri`, `alignment.tiri` | Right column panels.  Effects grows when an effect is enabled and asks the picker to restack the column. |

## Status

Working: the font list (search, filters, pins, selection, keyboard navigation), the preview (paragraph, waterfall,
compare, editing the sample text in place), size, line height, tracking, word spacing, kerning (metrics or off), all variable axes, italic, static and
bitmap styles, fill colour and opacity, centred stroke with its own opacity, drop shadow, outer glow, blend, alignment and justification,
paragraph spacing, upper, lower and title case, presets, and Select / Cancel / Reset.  The paint settings reach every sample through `fontdialog_style.paintDef()` and
the VectorText `textStyle` field.  A blend mode needs a backdrop, so the pane is made `ISOLATED` and repeats the page
background while one is active.  Bitmap families have no outlines, so `fontdialog_style.hasOutlines()` disables the
Stroke and Effects panels for them and leaves those settings out of the preview and the result.  The same applies to
tracking, word spacing and justification, because bitmap text is drawn a line at a time.  The picker's state keeps
these settings, so they return when a scalable family is chosen.

Recorded in the settings without being rendered: OpenType features, because the engine has no shaping.  Installing and removing fonts is not
offered, because the Font module has no function for either.

Known limitations:

- The classification, axis ranges, OpenType features and character set coverage come from the font database (see
  `RefreshFonts()`), through `gui.fontList`.  Script, display, symbol and unclassified families are listed under the
  Other filter.  The weight axis defaults to 400 where the range
  allows, rather than the font's default instance, matching the Regular default of static families.
- `string.upper()` is byte-based, so the preview uses `fontdialog_preview.applyCase()`.  It converts ASCII, Latin-1,
  Latin Extended-A, Greek and Cyrillic; letters in other scripts are unchanged.
- Presets are held as font definition strings, which `fontdialog_style.applyDef()` loads through `ReadTextStyle()`.
- The font database only records Greek, Cyrillic, CJK and emoji among the non-Latin scripts, so under All scripts the
  other scripts' families keep the Latin sample and draw it with missing glyphs.
