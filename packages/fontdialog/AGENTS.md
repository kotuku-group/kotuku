# Font picker package

## Source layout

| File | Responsibility |
| --- | --- |
| `fontdialog.tiri` | Dialog entry point.  Owns the picker state, the window and three-column layout and its vertical reflow, the right column's scrolling stack, the footer, the dialog keys and Select/Cancel/Reset.  Panels change the state through `update(Patch, Source)`; `refresh(Source)` then calls every panel's `update(Source)`, so a panel can skip the control that made the change.  `planLayout()` turns the `features` option into the columns of the layout; panels that weren't requested are left as `nil`, so code outside `planLayout()` that uses a panel must allow for its absence. |
| `catalogue.tiri` | The installed families, read from `gui.fontList`, with the font database's classification (serif, sans, mono, script, display, symbol), axis ranges, features and coverage, plus technology (scalable, bitmap) and list meta.  The only file that calls the Font module (`SelectFont()` for the Glyph report's format). |
| `controls.tiri` | Shared builders: slider rows (a drag's changes are coalesced to one per message cycle), segmented button groups (radio or multi, optionally wrapping), check rows, colour buttons (which open `gui.dialog.colour`), labels and dividers. |
| `fontlist.tiri` | Left column: search, classification and technology filters, the `gui.listView` of families with custom rows (name drawn in its own face, star, meta) and section heading rows, and pins. |
| `presets.tiri` | Presets panel, below the font list in the left column, or first in the right column's stack if there is no font list: a `gui.listView` of the built-in and saved presets with Save and Delete buttons in the panel header.  A row stays selected until the settings no longer match its preset, so a repeat click restores them.  The panel is `presetPanel` on the picker, because `presets` holds the preset data. |
| `preview.tiri` | Centre column: the scrolling Preview panel at the top of the column, with the Paragraph / Waterfall and Compare / Edit text groups in a footer styled as the panel header.  Compare freezes a copy of the settings; clicking the frozen pane swaps it with the live settings.  Edit text makes the live paragraph an editable VectorText (shown without the letter case applied) and shows the sample text buttons in the footer; Waterfall turns editing off. |
| `storage.tiri` | Reads and writes the user's pins and saved presets in `user:config/fontdialog.json`, using the colour picker's storage functions.  A file that can't be read or understood is left alone and turns off saving for that dialog. |
| `sizing.tiri` | Sizing panel: size (snapped to a bitmap family's strikes), line height, tracking, word spacing, kerning, and the px / pt switch in the header. |
| `style.tiri` | Style panel: axis sliders (ranges from the font database) and the italic switch for variable families, style buttons for static and bitmap families, and switches for the common OpenType features that the family provides.  Rebuilt when the family changes.  Also composes the VectorText `fontStyle` for the state. |
| `glyphs.tiri` | Glyph report panel: family, file format, character count and the coverage bars of the font database's character sets.  Its height follows the number of sets shown and asks the picker to restack the column. |
| `fill.tiri`, `stroke.tiri`, `effects.tiri`, `alignment.tiri` | Right column panels.  Effects grows when an effect is enabled and asks the picker to restack the column. |

## Status

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
