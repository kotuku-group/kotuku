# XSLT Class Specification for Parasol Framework

## Overview

The XSLT class provides W3C-compliant XSLT 1.0 transformation capabilities with selected XSLT 2.0 features. It integrates directly with Parasol's XML class and leverages the existing XPath 2.0 engine for maximum performance and consistency.

A stylesheet source must be provided prior to object initialisation (the stylesheet cannot be changed post-initialisation).
An XML source must be provided prior to object activation.

## Example Usage from Fluid

### Basic Transformation (xsltproc equivalent)

```lua
-- Simple file-to-file transformation
local output_file = obj.new('file', { path = 'result.xml', flags = FL_NEW })
local xslt = obj.new('xslt', {
   stylesheet   = 'transform.xsl',
   source       = 'input.xml',
   output       = output_file,
   outputMethod = XSLT_XML,
   indentLevel  = 2
})

assert(xslt.acActivate() == ERR_Okay, 'Transformation failed: ' .. xslt.errorMessage)

print('Transformation completed in ' .. xslt.processingTime .. 'ms')
```

### Advanced Processing (Saxon-like features)

```lua
-- Complex transformation with parameters and extensions
local xslt = obj.new('xslt', {
   stylesheet     = 'report.xsl',
   outputMethod   = XSLT_HTML,
   outputEncoding = 'UTF-8',
   messageLevel   = XSLT_VERBOSE
})

-- Load source from existing XML object
local xml = obj.new('xml', { path = 'data.xml' })
xslt.xml = xml

-- Set transformation parameters
xslt.setKey('date', mSys.CurrentTime())
xslt.setKey('title', 'Monthly Report')
xslt.setKey('format', 'detailed')

-- Register custom extension function
local function formatCurrency(amount, currency)
   return string.format('%.2f %s', tonumber(amount), currency or 'USD')
end

xslt.mtRegisterFunction('http://mycompany.com/xsl', 'format-currency', formatCurrency)

-- Perform transformation with error handling
local result
local err = xslt.acActivate(result)

if (err == ERR_Okay) then
   -- Save result with custom filename
   local timestamp = os.date('%Y%m%d_%H%M%S')
   local filename = string.format('report_%s.html', timestamp)

   local file = obj.new('file', { path = filename, flags = FL_NEW })
   file.acWrite(result)

   print(string.format('Report generated: %s (%d warnings)', filename, xslt.warningCount))
else
   error(string.format('Transformation failed (Error %d): %s', xslt.errorCode, xslt.errorMessage))
end
```

### Batch Processing Application

```lua
-- Command-line XSLT processor (xsltproc/Saxon clone)
local function processFile(stylesheetPath, inputPath, outputPath, params)
   local xslt = obj.new('xslt', {
      stylesheet = stylesheetPath,
      source = inputPath,
      output = outputPath,
      messageLevel = XSLT_INFO
   })

   -- Apply command-line parameters
   if params then
      for name, value in pairs(params) do
         xslt.setKey(name, value)
      end
   end

   -- Validate inputs first
   local err = xslt.mtValidateStylesheet()
   if (err != ERR_Okay) then
      return err, 'Invalid stylesheet: ' .. xslt.errorMessage
   end

   -- Execute transformation
   local startTime = mSys.PreciseTime()
   err = xslt.acActivate()
   local duration = mSys.PreciseTime() - startTime

   if (err == ERR_Okay) then
      print(string.format('✓ %s → %s (%.2fms)', inputPath, outputPath, duration))
      return ERR_Okay
   else
      return err, xslt.errorMessage
   end
end

-- Process multiple files
local files = {
   { stylesheet = 'html.xsl', input = 'page1.xml', output = 'page1.html' },
   { stylesheet = 'html.xsl', input = 'page2.xml', output = 'page2.html' },
   { stylesheet = 'pdf.xsl', input = 'report.xml', output = 'report.fo' }
}

local globalParams = {
   version = '2.1.0',
   buildDate = os.date('%Y-%m-%d'),
   environment = 'production'
}

for _, file in ipairs(files) do
   local err, msg = processFile(file.stylesheet, file.input, file.output, globalParams)
   if (err != ERR_Okay) then
      print(string.format('✗ Failed: %s (%s)', file.input, msg))
   end
end
```

## Key Features Supporting xsltproc/Saxon Compatibility

1. **Command-line Processing** - Direct file input/output with error codes
2. **Parameter Support** - Full parameter passing and management
3. **Multiple Output Formats** - XML, HTML, and text output methods
4. **Extension Functions** - Custom XPath function registration
5. **Error Handling** - Detailed error reporting and validation
6. **Performance Monitoring** - Timing and statistics collection
7. **Batch Processing** - Efficient multi-file transformation
8. **Memory Management** - Configurable limits and cleanup
9. **Standards Compliance** - W3C XSLT 1.0 with selective 2.0 features
10. **Integration** - Native Parasol XML class compatibility

This design provides a foundation for building both command-line tools and interactive applications that can match the functionality of existing XSLT processors while maintaining tight integration with the Parasol framework.