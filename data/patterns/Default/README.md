# Default pattern collection

This read-only collection is mounted as `patterns:` when display initialises, including headless mode.  Reference an
asset from any fill or stroke with `url(#pattern:Name)`, where `Name` is the file basename without its category or
extension, e.g. `url(#pattern:Linen)` for `Textiles/Linen.svg`.

Each asset defines one public paint server and a preview rectangle that uses it.  The paint server's ID is `pid`
followed by the asset's category and name, e.g. `pidGradientsSunset` for `Gradients/Sunset.svg`.  Exported documents
embed imported paints under these IDs, so any other IDs used by an asset must also be unique across the collection.

Categories and asset names use camel case with an initial capital, and asset basenames are unique across the collection regardless of letter case.
