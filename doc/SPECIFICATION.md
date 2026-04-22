# CUP SPECIFICATION

## 1. Purpose

`cup` is a prototype toolchain manager for installing software components under a user-owned directory structure.

The current implementation is centered on:

- installation of components from package archives
- local state persistence
- cache reuse
- per-component default selection
- support for multiple archive formats
- manifest-driven version resolution

## 2. Current scope

The current implementation supports multiple components.

Examples currently configured in the project include:

- `compiler`
- `debugger`

Examples of supported tools include:

- `gcc`
- `clang`
- `gdb`

The supported component/tool mapping is defined in source code and validated explicitly.

## 3. Objectives

The implementation follows these objectives:

- keep consistency between persistent state and filesystem layout
- avoid administrator privileges
- support multiple tools under the same component
- support multiple installed releases
- support one default entry per component
- keep runtime data under a predictable user directory
- allow package source configuration through a manifest
- keep the installation flow staged and recoverable

## 4. Terminology

### Component
A logical category of installable software.

Examples:

```text
compiler
debugger
```

### Tool
A concrete installable implementation inside a component.

Examples:

```text
gcc
clang
gdb
```

### Release
A user-requested or resolved version identifier.

Examples:

```text
stable
15.2.0
22.1.3
17.1
```

### Entry
A user-facing identifier in the form:

```text
<tool>@<release>
```

Example:

```text
gcc@stable
```

### Canonical entry
The internal normalized form used in persistent state:

```text
<tool>@<resolved_version>
```

Example:

```text
gcc@15.2.0
```

### Archive format
The package compression/container format selected for download.

Examples:

```text
tar.gz
tar.xz
```

## 5. Commands

The CLI currently supports:

```text
list
install <component> <tool>@<release> [--format <archive-format>]
remove <component> <tool>@<release>
default <component> <tool>@<release>
current <component>
```

### list
Prints installed entries and marks defaults.

### install
Installs a requested tool release for a selected component.

### remove
Removes an installed entry from both filesystem and local state.

### default
Sets the default canonical entry for a component.

### current
Prints the current default canonical entry for a component.

## 6. Entry resolution model

The implementation separates user input from internal state.

### 6.1 User input
The user provides:

```text
<tool>@<release>
```

### 6.2 Release resolution
The release is resolved as follows:

- `stable` is resolved through the manifest
- explicit versions are used directly

### 6.3 Canonicalization
After resolution, a canonical entry is built:

```text
<tool>@<resolved_version>
```

This canonical entry is used for:

- persistent state
- installed entry lookup
- default handling
- consistency checks

## 7. Version availability

After release resolution, the resolved version is checked against the tool’s declared available versions in the manifest.

If the version is not listed as available, installation fails before any download begins.

This prevents invalid or unsupported version requests from reaching the fetch phase.

## 8. Archive format model

The archive format is selected as follows:

- if the user does not specify a format, the tool’s `default_format` is used
- if the user specifies a format, it must appear in the tool’s supported `formats` list

The selected format is then used to:

- determine the archive filename in cache
- build the final package URL
- select the downloaded asset

Archive extraction is performed by the archive layer and is independent of the originally requested alias.

## 9. Filesystem layout

All runtime data is stored under:

```text
~/.cup/
```

### 9.1 Root layout

```text
~/.cup/
├── state.txt
├── components/
├── cache/
└── tmp/
```

### 9.2 Installed components

Installed directories follow:

```text
~/.cup/components/<component>/<tool>/<platform>/<release>
```

Example:

```text
~/.cup/components/compiler/gcc/linux/15.2.0
```

### 9.3 Cache layout

Cached packages follow:

```text
~/.cup/cache/<component>/<tool>/<release>/package.<format>
```

Examples:

```text
~/.cup/cache/compiler/gcc/15.2.0/package.tar.gz
~/.cup/cache/compiler/gcc/15.2.0/package.tar.xz
```

### 9.4 Temporary staging layout

Temporary directories follow:

```text
~/.cup/tmp/<component>-<tool>-<release>-<pid>
```

These are created during installation and cleaned on failure or startup cleanup.

## 10. State model

Persistent state is stored in:

```text
~/.cup/state.txt
```

The state file contains two record families.

### Installed entries

```text
installed.<component>=<tool>@<resolved_version>
```

### Default entries

```text
default.<component>=<tool>@<resolved_version>
```

Only one default entry is stored for each component.

## 11. Installation process

The installation flow is staged.

### 11.1 Entry context resolution
The implementation resolves an internal entry context that contains:

- tool
- requested release
- resolved release
- canonical entry

This context is reused across install, remove, and default operations.

### 11.2 Validation phase
The install flow validates:

- entry syntax
- component support
- tool support for the selected component
- release syntax
- version availability
- requested archive format support

### 11.3 Fetch phase
The package archive is looked up in the local cache.

If the archive is missing, the implementation:

- builds the package URL from the manifest
- downloads the archive into the cache

### 11.4 Extraction phase
The archive is extracted into a temporary staging directory.

The current implementation uses a dedicated archive module for extraction.

### 11.5 Metadata phase
After extraction, installation metadata is written to:

```text
info.txt
```

inside the temporary installation directory.

### 11.6 Validation phase
The staged installation is checked for:

- temporary directory existence
- metadata existence
- metadata readability
- metadata non-emptiness

### 11.7 Commit phase
If staging validation succeeds, the installation is moved to the final destination using `rename`.

### 11.8 State update
After a successful commit:

- the canonical entry is added to state
- the state file is saved

If state persistence fails after commit, rollback is attempted.

## 12. Error handling

The project uses explicit error codes through the `CupError` enum.

The current implementation distinguishes errors related to:

- invalid input
- unsupported component
- invalid tool for component
- invalid release
- unavailable version
- fetch failures
- archive extraction failures
- filesystem failures
- validation failures
- state load/save failures
- interruption handling
- rollback failures
- state/filesystem inconsistencies

## 13. Interruption handling

The install flow handles `SIGINT` through a `sig_atomic_t` flag.

The signal handler only records interruption state.
Cleanup is performed later by normal control flow.

## 14. Source code organization

### `main.c`
Command-line parsing and dispatch.

### `component.c`
Command orchestration, entry context resolution, and top-level install/remove/default logic.

### `state.c`
State initialization, parsing, saving, and mutation.

### `fs.c`
Filesystem paths, directory preparation, cache layout, commit, cleanup, and install metadata writing.

### `manifest.c`
Manifest reading, release resolution, version checks, format checks, and URL generation.

### `support.c`
Supported components and their tools.

### `fetch.c`
Archive download and cache acquisition.

### `archive.c`
Archive extraction.

### `util.c`
Reusable general-purpose helpers such as safe formatted string writing.

### `constants.h`
Shared project-wide size limits.

## 15. Current implementation characteristics

The current implementation is:

- Linux-oriented
- user-space only
- manifest-driven
- cache-aware
- alias-aware through canonical entries
- format-aware at package selection time

## 16. Current limitations

The current implementation still has limitations:

- Linux-centric runtime assumptions
- support tables for components and tools are still code-defined
- manifest parsing is simple and line-oriented
- no checksum or signature verification
- validation of extracted contents is still minimal
- archive and fetch behavior are implemented around current supported formats and package layout assumptions

## 17. Planned extension areas

Likely future directions include:

- richer post-extraction validation
- automatic manifest generation/update
- additional components and tools
- support for more package providers
- stronger package integrity verification
- broader platform support