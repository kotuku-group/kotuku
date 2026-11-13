# Pattern Collection

This read-only collection is mounted under the `patterns:` volume.  Reference an asset from any fill or stroke
with `url(#pattern:Category/Name)`, where `Name` is the file basename with or without its `Category` or extension, e.g. `url(#pattern:Canvas)` for `Textiles/Canvas.svg`.

Each asset defines one public paint server and a preview rectangle that uses it.  The paint server's ID is `pid`
followed by the asset's category and name, e.g. `pidGradientsSunset` for `Gradients/Sunset.svg`.  Exported documents
embed the paint's definitions rather than retaining `url(#pattern:Name)` references.  By convention, any other IDs used by an asset are also unique across the collection.

## Copyright

All SVG patterns are created for distribution as part of the Kotuku Framework.  Patterns are royalty free, can be modified and are freely distributable under the deed of CC0 1.0 Universal.  https://creativecommons.org/publicdomain/zero/1.0/deed.en
