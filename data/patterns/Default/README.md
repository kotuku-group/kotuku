# Default pattern collection

This read-only collection is mounted as `patterns:` when vector initialises, including headless mode.  Reference an
asset from any fill or stroke with `url(#pattern:Name)`, where `Name` is the file basename without its category or
extension, e.g. `url(#pattern:Canvas)` for `Textiles/Canvas.svg`.

Each asset defines one public paint server and a preview rectangle that uses it.  The paint server's ID is `pid`
followed by the asset's category and name, e.g. `pidGradientsSunset` for `Gradients/Sunset.svg`.  Exported documents
retain `url(#pattern:Name)` references rather than embedding the paint.  By convention, any other IDs used by an
asset are also unique across the collection.

## Copyright

Copyright 2026 Paul Manias.  All files are royalty free and freely distributable under the deed of CC0 1.0 Universal.  https://creativecommons.org/publicdomain/zero/1.0/deed.en
