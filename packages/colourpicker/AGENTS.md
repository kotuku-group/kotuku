# Colour picker package

This directory implements the `gui/colourpicker` Tiri package.  `package.json` declares `colourpicker.tiri` as the
entry point and exports the colour maths, palette model and export formatter as separate import paths.  The picker is
used through `import 'gui/colourpicker'` and `gui.dialog.colour(...)`.

## Source layout

| File | Responsibility |
| --- | --- |
| `package.json` | Package metadata, dependencies, file manifest and public export paths. |
| `colourpicker.tiri` | Dialog entry point.  Owns the picker state, window and three-column layout, input handling, scratch row, palette list (a `gui.listView` with custom rows) and editing, readouts, contrast and export panels, and Apply/Cancel behaviour.  Hosts the Tonal Ramp panel and applies its picks through `setColour()`.  Switches the body between the Picker and the palette manager (`selectTab()`, Ctrl+Tab, Ctrl+1/2; a swatch rename in progress is committed first), owns the palette database (`db`, stable `dbID`, derived `dbIndex`) and opens database palettes for editing (`openPalette()`). |
| `colour.tiri` | Display-independent colour maths: OKLCH/OKLab and RGB conversions, gamut handling, CSS colour parsing and formatting, contrast calculations, colour vision deficiency simulation and named colours.  Exported as `gui/colourpicker/colour`. |
| `palette.tiri` | Display-independent palette model: swatches, linked colour relationships, palette modes and their reserved role swatches, reordering, serialisation and persistence, including the palette database (`user:config/palettes.json`) used by the palette manager, and the starter palettes (`loadStarters()`, copied into the database with `insertCopy()`).  Exported as `gui/colourpicker/palette`. |
| `palettes.json` | The read-only starter palettes (`colourpalette.STARTERS`), in the palette database format: Winter, Spring, Summer and Autumn.  Each holds five themes (Christmas, Northern Lights and so on), and a theme is a base colour followed by its companion colours, which are linked to the base by L, C and H deltas so that they follow edits of it. |
| `export.tiri` | Display-independent generation of CSS, design token JSON, Tiri and Android palette output.  Produces text segments for `codeview.tiri` and clipboard text.  Exported as `gui/colourpicker/export`. |
| `theme.tiri` | Derives the picker theme from the window's gui theme (its scheme selects the light or dark design and its palette supplies the accent colours) and supplies colour tokens and widget overrides.  Its `panel` overlay pins the design's panel sizes (12 px title, 14 px mono meta, 31 px header) and removes the base theme's header fill; `PANEL_HEADER_H` is the header height including the divider, for layout constants computed before a panel exists. |
| `parts.tiri` | Shared vector UI builders, text/layout helpers, the small combobox caret and timer helpers used by the picker components, including the palette row content drawn in the list's row viewports.  `PANEL_PADDING` is the zero body padding that every picker panel uses. |
| `wheel.tiri` | Hue/chroma wheel bitmap, interaction markers, palette relationship markers, gamut edge outlines and magnified loupe. |
| `eyedropper.tiri` | Samples the loupe area at `GetCursorPos()`: Kōtuku surface bitmaps first (resolving shared bitmap origins and converting linear RGB), then the screen through `CaptureDisplay()` where the driver supports it.  Also manages the global sampling button subscription. |
| `preview.tiri` | Per-mode palette previews: the Interface application mockup, a Web Site page, a Code Theme snippet, a Data Vis. chart and a Terminal session.  Each preview has a builder and a colour function that maps the palette mode's roles to its paint roles, and optionally a meta function for the panel meta (the Data Vis. distinctness readout). |
| `ramp.tiri` | Tonal Ramp panel: a row of step buttons at the edited colour's hue, plus/minus buttons for the step count, the selection ring, and the L and C curves with their preset comboboxes.  The L curve shapes the lightness from 0.98 to 0.18 and the C curve scales the base chroma (gamut-limited).  Keeps its base while the edited colour is a picked step.  Step drags are detected by an extra input subscription on each step button's viewport and handed to the picker's `startScratchDrag()` through `onDrag`. |
| `curve.tiri` | Curve editor used by the Tonal Ramp: a monotone cubic (PCHIP) curve through its end points and up to two handles, which are dragged or moved with the arrow keys.  The handles are either the points between the ends, or the end points themselves (`movableEnds`, used by Linear), which move vertically only. |
| `manager.tiri` | Palette manager (Palette tab): the searchable palette list with icon buttons for New, Duplicate, Delete (confirmed; the picker's `removePalette()` switches the picker if it edits that palette), Import (merges, then reports the counts or the refusal in a dialog) and Export (of the whole database through the file dialog), swatch cards for the selected palette, and the palette and swatch details with editable descriptions.  Double-clicking a palette or card calls the picker's `openPalette()`.  The card grid is one tab stop (arrow keys, Home and End select, Enter edits), and `keyPress()` handles the manager shortcuts that the dialog's key handler passes on (Ctrl+F/K, Delete, F2).  Cards show reserved, linked, translucent and out-of-sRGB swatches; the search has a clear button and a match count, and keeps a filtered-out selection listed (the list has a row per palette, and the search hides rows rather than rebuilding the list); description areas scroll.  The Starter Palettes panel under the palette list shows the read-only starter palettes: one can be viewed (its name, descriptions, Delete and Edit in Picker are disabled), and Duplicate or a drag to the palette list adds an editable copy. |
| `codeview.tiri` | Export panel: a scrolling `gui.panel` with format tabs above its body, which shows a syntax-coloured display of the generated export text on a background that meets the panel border. |

The dialog imports `colour.tiri`, `palette.tiri` and `export.tiri` through their package paths.  It imports the UI helpers
through relative paths.  Keep native Core timer calls in `parts.tiri`: this package's compile-time interface rejects
references to the same native module from more than one source file.

## Panels

Every panel is a `gui.panel()` (the `gui/panel` package), with `padding = colourpicker_parts.PANEL_PADDING` so that
the body meets the panel edges and its content applies its own insets.  Retain each panel table for the life of the
dialog: collecting it frees the panel's viewport.  Body heights come from `bodySize()`, which is valid as soon as the
panel's height is set.

- **Palette**: the mode combobox is the `lead` header item and the palette name field is the stretched `meta` item.
  The panel's `headerPad` is `LIST_PAD_X`, so that the mode box lines up with the list rows, and its `metaGap` allows
  for the field's `FIELD_PAD` overhang.  `flashPaletteMeta()` calls `flash()`, which hides the name field for the
  message.
- **Eyedropper** and **Relationship**: the pick and Swap buttons are `trail` items.  The Relationship controls are
  registered with the panel (`addWidget()`), so `hide()` disables them and takes them out of the tab order.  A value
  field has a `disabled` field so that it can be registered.
- **Contrast**: `updateContrast()` sets `events.metaActivate` only while a dropped colour is the target, which makes
  the meta clickable, and passes the `vs ` prefix and ` on panel` suffix to `setMeta()`.
- **Export** and the manager's list panels flash messages with `flash()`.  The manager's centre panel uses
  `truncate = 'title'` and `scroll = 'vertical'`; its body is the card grid's tab stop.

## Related files

`examples/colourpicker.tiri` shows how to open the dialog and receive its result.  The model and export Flute tests live
in `packages/tests/test_colourpicker_colour.tiri`, `test_colourpicker_palette.tiri` and
`test_colourpicker_export.tiri`.  The palette manager's dialog tests are in `test_colourpicker_manager.tiri` and
`test_colourpicker_persistence.tiri` (storage, starter palettes and drags).  The eyedropper, Tonal Ramp and palette row
tests (`test_colourpicker_eyedropper.tiri`, `test_colourpicker_rows.tiri`) are not in every checkout.  Registrations
are in `packages/CMakeLists.txt`.  User documentation is the Colour Picker section of `docs/wiki/Tiri-Widgets.md`,
including the palette database format.

## Palette storage

`user:config/palettes.json` is the sole palette store.  The starter palettes are loaded from the package's
`palettes.json` each time the manager is built and are never saved; a copy made from one is an ordinary database
palette with a new ID.  Each palette has a stable string `id`; database `active`
contains that ID.  Legacy numeric active indices and palettes without IDs are migrated on load.  The picker settings
file, `user:config/colourpicker.json`, stores `activePalette`, display preferences and `paletteFolder` (the folder
last used by Import and Export), with no palette data.  Legacy
settings palettes migrate once; an existing database entry takes precedence over the settings copy.

Committed palette edits save immediately from both tabs, including reset and clipboard replacement.  Cancel cancels
the colour result and discards preference changes; it keeps saved palette edits.  Reset restores the palette snapshot
captured when it was opened and saves that restoration.  Apply and window close save preferences and the active ID.

Database saves compare the current file text with the snapshot from load or the last successful save.  A mismatch
blocks further saves in that picker and displays a notice; reopen the picker to load the current database.  Explicit
export can save the in-memory copy elsewhere without clearing the conflict.  This detects stale writers; it does not
provide file locking or automatic merging for simultaneous writes from separate processes.

Palette names are unique within the database.  New, Duplicate and Import number a clashing name
(`uniquePaletteName()`); renaming in either tab refuses another palette's name.  The database always keeps at least
one palette.  Deleting the edited palette makes the picker edit the palette that takes its place in the list.

Damaged databases are preserved under an unused `.bak`, `.bak.1`, etc. name.  A recovery dialog reports that path.
Failed reads or backups leave the original file intact and prevent replacement.  Storage errors are printed to stderr and shown in the
picker's Palette Storage dialog.  Once a database error is latched, an edit that can't be saved flashes `not saved`
in the meta of the shown tab's palette panel.  Description limits count Unicode
characters, with the same limit enforced by the manager's text editors.
