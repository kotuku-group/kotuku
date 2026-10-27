# Colour picker package

This directory implements the `gui/colourpicker` Tiri package.  `package.json` declares `colourpicker.tiri` as the
entry point and exports the colour maths, palette model and export formatter as separate import paths.  The picker is
used through `import 'gui/colourpicker'` and `gui.dialog.colour(...)`.

## Source layout

| File | Responsibility |
| --- | --- |
| `package.json` | Package metadata, dependencies, file manifest and public export paths. |
| `colourpicker.tiri` | Dialog entry point.  Owns the picker state, window and three-column layout, input handling, scratch row, palette list (a `gui.listView` with custom rows) and editing, readouts, contrast and export panels, and Apply/Cancel behaviour.  Hosts the Tonal Ramp panel and applies its picks through `setColour()`. |
| `colour.tiri` | Display-independent colour maths: OKLCH/OKLab and RGB conversions, gamut handling, CSS colour parsing and formatting, contrast calculations, colour vision deficiency simulation and named colours.  Exported as `gui/colourpicker/colour`. |
| `palette.tiri` | Display-independent palette model: swatches, linked colour relationships, palette modes and their reserved role swatches, reordering, serialisation and persistence.  Exported as `gui/colourpicker/palette`. |
| `export.tiri` | Display-independent generation of CSS, design token JSON, Tiri and Android palette output.  Produces text segments for `codeview.tiri` and clipboard text.  Exported as `gui/colourpicker/export`. |
| `theme.tiri` | Derives the picker theme from the window's gui theme (its scheme selects the light or dark design and its palette supplies the accent colours) and supplies colour tokens and widget overrides. |
| `parts.tiri` | Shared vector UI builders, text/layout helpers, the small combobox caret and timer helpers used by the picker components, including the palette row content drawn in the list's row viewports. |
| `wheel.tiri` | Hue/chroma wheel bitmap, interaction markers, palette relationship markers, gamut edge outlines and magnified loupe. |
| `eyedropper.tiri` | Samples the loupe area at `GetCursorPos()`: Kōtuku surface bitmaps first (resolving shared bitmap origins and converting linear RGB), then the screen through `CaptureDisplay()` where the driver supports it.  Also manages the global sampling button subscription. |
| `preview.tiri` | Per-mode palette previews: the Interface application mockup, a Web Site page, a Code Theme snippet, a Data Vis. chart and a Terminal session.  Each preview has a builder and a colour function that maps the palette mode's roles to its paint roles, and optionally a meta function for the panel meta (the Data Vis. distinctness readout). |
| `ramp.tiri` | Tonal Ramp panel: a row of step buttons at the edited colour's hue, plus/minus buttons for the step count, the selection ring, and the L and C curves with their preset comboboxes.  The L curve shapes the lightness from 0.98 to 0.18 and the C curve scales the base chroma (gamut-limited).  Keeps its base while the edited colour is a picked step.  Step drags are detected by an extra input subscription on each step button's viewport and handed to the picker's `startScratchDrag()` through `onDrag`. |
| `curve.tiri` | Curve editor used by the Tonal Ramp: a monotone cubic (PCHIP) curve through its end points and up to two handles, which are dragged or moved with the arrow keys.  The handles are either the points between the ends, or the end points themselves (`movableEnds`, used by Linear), which move vertically only. |
| `codeview.tiri` | Tabbed, syntax-coloured and scrollable display for generated export text. |

The dialog imports `colour.tiri`, `palette.tiri` and `export.tiri` through their package paths.  It imports the UI helpers
through relative paths.  Keep native Core timer calls in `parts.tiri`: this package's compile-time interface rejects
references to the same native module from more than one source file.

## Related files

`examples/colourpicker.tiri` shows how to open the dialog and receive its result.  The model and export Flute tests live
in `packages/tests/test_colourpicker_colour.tiri`, `test_colourpicker_palette.tiri` and
`test_colourpicker_export.tiri`; the dialog/surface tests are in `test_colourpicker_eyedropper.tiri` and `test_colourpicker_rows.tiri`, which also covers the Tonal Ramp.  Their registrations are in `packages/CMakeLists.txt`.
