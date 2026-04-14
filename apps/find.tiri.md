# Find

A file search tool for Kōtuku that leverages the `io.search()` interface. It provides flexible filtering by name,
type, size, and modification time, with configurable output formats and the ability to execute commands on matched
files.

## Usage

```bash
origo apps/find.tiri path=folder/ [options...]
```

Running without any parameters or with `help=true` will display the built-in help text.

## Options

| Option | Description | Default |
|-----------|-------------------------------------------------------|---------|
| `path` | The folder to search (required) | — |
| `filter` | A regex pattern to filter filenames | — |
| `name` | An exact filename to match | — |
| `type` | Match type: `file`, `dir`, or `all` | `file` |
| `maxdepth` | Maximum directory depth to recurse into | `100` |
| `mindepth` | Minimum depth before results are reported | `0` |
| `size` | Size filter (see [Size Filtering](#size-filtering)) | — |
| `modified` | Modification time filter (see [Time Filtering](#time-filtering)) | — |
| `exclude` | A regex pattern to exclude matching filenames | — |
| `count` | Print only the total match count | `false` |
| `exec` | Execute a command for each match, `{}` is replaced by the file path | — |
| `output` | Output format: `path`, `name`, or `detail` | `path` |
| `verbose` | Print search parameters before searching | `false` |
| `help` | Show built-in help text | — |

The `filter` and `name` options are mutually exclusive. Use `filter` for regex matching and `name` for an exact
filename match.

## Size Filtering

The `size` option accepts a numeric value with an optional prefix and suffix.

**Prefixes** control the comparison direction:

- `+` — minimum size (files larger than the value)
- `-` — maximum size (files smaller than the value)
- No prefix — exact size (both minimum and maximum are set)

**Suffixes** specify the unit:

- `K` — kilobytes (x 1024)
- `M` — megabytes (x 1,048,576)
- `G` — gigabytes (x 1,073,741,824)
- No suffix — bytes

Examples: `+10K` (at least 10 KB), `-1M` (under 1 MB), `500` (exactly 500 bytes).

## Time Filtering

The `modified` option filters files by their modification date relative to the current time.

**Prefixes** are required and control the direction:

- `-` — newer than (modified within the given period)
- `+` — older than (modified before the given period)

**Suffixes** specify the time unit:

- `m` — minutes
- `h` — hours
- `d` — days

Examples: `-7d` (modified in the last 7 days), `+24h` (not modified in the last 24 hours), `-30m` (modified in the
last 30 minutes).

## Output Formats

The `output` option controls how matched files are printed:

- `path` — full path to the file (default)
- `name` — filename only, without the directory path
- `detail` — full path, size in bytes, and modification date separated by tabs

## Examples

### Search for all files in a directory

```bash
origo apps/find.tiri path=src/core/
```

### Find a specific file by name

```bash
origo apps/find.tiri path=src/core/ name=defs.h
```

### Find files matching a regex pattern

Search for all C++ source files:

```bash
origo apps/find.tiri path=src/core/ filter="\.cpp$"
```

### List directories only

```bash
origo apps/find.tiri path=src/ type=dir maxdepth=0
```

### List all entries (files and directories)

```bash
origo apps/find.tiri path=src/ type=all maxdepth=0
```

### Filter by file size

Find files larger than 10 KB:

```bash
origo apps/find.tiri path=src/core/ size=+10K
```

Find files smaller than 1 MB:

```bash
origo apps/find.tiri path=src/ size=-1M
```

### Filter by modification time

Find files modified in the last 7 days:

```bash
origo apps/find.tiri path=src/ modified=-7d
```

Find files not modified in the last 24 hours:

```bash
origo apps/find.tiri path=src/ modified=+24h
```

### Exclude files matching a pattern

Search but exclude any files with "test" in the name:

```bash
origo apps/find.tiri path=src/core/ exclude="test"
```

### Count matches

Print only the number of matching files:

```bash
origo apps/find.tiri path=src/core/ count=true
```

### Output filenames only

```bash
origo apps/find.tiri path=src/core/ maxdepth=0 output=name
```

### Detailed output with size and date

```bash
origo apps/find.tiri path=src/core/ maxdepth=0 output=detail
```

Example output:

```
src/core/lib_memory.cpp     28069   2026-02-22 13:27
src/core/lib_functions.cpp  39246   2026-02-24 08:00
```

### Execute a command on each match

Run `wc -l` on every matched file, with `{}` replaced by the file path:

```bash
origo apps/find.tiri path=src/core/ name=defs.h exec="wc -l {}"
```

### Control recursion depth

Search only the immediate directory (no recursion):

```bash
origo apps/find.tiri path=src/ maxdepth=0
```

Report results only from depth 2 and beyond:

```bash
origo apps/find.tiri path=src/ mindepth=2
```

### Combine multiple options

Find large C++ files modified recently, showing detailed output:

```bash
origo apps/find.tiri path=src/ filter="\.cpp$" size=+20K modified=-7d output=detail
```

### Verbose mode

Print search parameters before starting the search:

```bash
origo apps/find.tiri path=src/core/ size=+10K verbose=true
```
