---
description: "Update document indexes to include missing links"
---

Update all documentation indexes to include references to missing modules and classes by comparing the actual files in `docs/html/modules/` and `docs/html/modules/classes/` against the indexes.

## Files to Update

1. **docs/html/modules/api.html** - Main API index page (sidebar and main content)
2. **docs/wiki-header.html** - Wiki header template (sidebar only)
3. **docs/xml/modules/module.xsl** - XSL stylesheet for module pages (sidebar only)
4. **docs/xml/modules/classes/class.xsl** - XSL stylesheet for class pages (sidebar only)
6. **docs/html/wiki/Home.html** - Wiki home page (sidebar only)

## Process

For each file:
1. List actual module HTML files: `powershell -Command "Get-ChildItem docs/html/modules/*.html | Select-Object -ExpandProperty Name"`
2. List actual class HTML files: `powershell -Command "Get-ChildItem docs/html/modules/classes/*.html | Select-Object -ExpandProperty Name"`
3. Compare against existing indexes in each file
4. Add any missing modules to the Modules section
5. Add any missing classes to the appropriate class category (Audio, Core, Data, Devices, Effects, Extensions, Graphics, Network, Vectors)

## Notes

- Module entries should be added in alphabetical order
- Class entries should be added in alphabetical order within their category
- Update both sidebar navigation and main content sections where applicable
- Use appropriate relative paths for links based on file location
- **Exception**: Ignore the `XRandR` module, it is treated as private.
