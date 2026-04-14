# CUP SPECIFICATION

## 1. Purpose

`cup` is a prototype toolchain manager for installing software components.
The current implementation is centered on compilers.

The project focuses on installation, local state persistence, cache handling, and default selection.

## 2. Objectives

The implementation is based on the following objectives:

- maintain consistency between filesystem and internal state
- avoid administrator privileges
- support multiple tools for the same component
- support multiple installed releases for the same tool
- support one default entry per component
- keep the filesystem layout simple and extendable

## 3. Terminology

### Component
A logical category of toolchain element.

Example:

```text
compiler
```

### Tool
A specific implementation belonging to a component.

Examples:

```text
gcc
clang
```

### Release
A user-requested or resolved release identifier.

Examples:

```text
stable
nightly
22.1.3
```

### Entry
A user-facing identifier in the form:

```text
<tool>@<release>
```

Example:

```text
clang@stable
```

## 4. Commands

The current CLI supports:

```text
list
install <component> <tool>@<release>
remove <component> <tool>@<release>
default <component> <tool>@<release>
current <component>
```

### list
Shows installed entries and marks defaults.

### install
Installs the requested entry through the staged installation flow.

### remove
Removes an installed entry from the filesystem and from the local state.

### default
Sets the default entry for a component.

### current
Shows the current default entry for a component.

## 5. Filesystem layout

All runtime data is stored under:

```text
~/.cup/
```

### Root structure

```text
~/.cup/
├── state.txt
├── components/
├── cache/
└── tmp/
```

### Installed components

```text
~/.cup/components/<component>/<tool>/<platform>/<release>
```

Example:

```text
~/.cup/components/compiler/clang/linux/22.1.3
```

### Cache

```text
~/.cup/cache/<component>/<tool>/<release>/package.tar.xz
```

### Temporary directories

```text
~/.cup/tmp/<component>-<tool>-<release>-<pid>
```

## 6. State model

Persistent state is stored in:

```text
~/.cup/state.txt
```

Two kinds of records are used.

### Installed entries

```text
installed.<component>=<tool>@<release>
```

### Default entries

```text
default.<component>=<tool>@<release>
```

At most one default entry is stored for each component.

## 7. Installation process

The installation logic is staged.

### 7.1 Entry validation

Validation is divided into two parts:

- syntactic validation
- semantic validation

Syntactic validation checks that the entry contains exactly one `@` and that both parts are non-empty.

Semantic validation checks:

- component support
- tool support for the component
- release validity

### 7.2 Release resolution

Requested releases are resolved as follows:

- `stable` is resolved through the manifest
- `nightly` is resolved through the manifest
- explicit versions are used directly

### 7.3 Fetch

The package archive is searched in the local cache.

If the archive is not present, it is downloaded using the URL template defined in the manifest.

### 7.4 Temporary installation

The archive is extracted into a temporary directory.

Then `info.txt` metadata is written for the staged installation.

### 7.5 Validation

The current validation checks:

- temporary directory existence
- `info.txt` existence
- `info.txt` readability
- `info.txt` not empty

### 7.6 Commit

After validation, the temporary installation is moved to the final destination through `rename`.

### 7.7 State update

After a successful commit:

- the installed entry is added to state
- the state file is saved

If state saving fails after commit, a rollback attempt is performed.

## 8. Package manifest

The implementation uses a local manifest file:

```text
config/packages.cfg
```

The manifest is used to store:

- stable release mappings
- nightly release mappings
- package URL templates

Example structure:

```text
compiler.gcc.stable_version=15.2.0
compiler.gcc.nightly_version=15.2.0
compiler.gcc.url=https://gcc.gnu.org/pub/gcc/releases/gcc-{version}/gcc-{version}.tar.xz

compiler.clang.stable_version=22.1.3
compiler.clang.nightly_version=22.1.3
compiler.clang.url=https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/LLVM-{version}-Linux-X64.tar.xz
```

The placeholder `{version}` is replaced with the resolved release string.

## 9. Error handling

The project uses explicit error codes through the `CupError` enum.

The current implementation distinguishes errors related to:

- invalid input
- unsupported component or tool
- invalid release
- fetch failures
- installation failures
- validation failures
- filesystem failures
- state load/save failures
- interruption handling
- rollback failures
- state/filesystem inconsistencies

## 10. Cleanup and rollback

Temporary directories are cleaned:

- after installation failures
- after validation failures
- after interruptions
- at startup through temporary cleanup

If commit succeeds but state saving fails, the implementation attempts to remove the installed directory.

## 11. Interruption handling

The install flow handles `SIGINT` through a global `sig_atomic_t` flag.

The signal handler only sets the flag.
Cleanup is performed later by normal control flow.

## 12. Source code structure

The source code is divided into four modules.

### main.c
Responsible for command dispatch and argument checking.

### component.c
Responsible for command logic, entry parsing, semantic validation, and command orchestration.

### state.c
Responsible for state initialization, load/save operations, and state manipulation.

### fs.c
Responsible for path construction, directory management, cache handling, archive download, extraction, validation helpers, commit, and cleanup.

## 13. Current limitations

The current prototype has the following limitations:

- Linux-oriented implementation
- minimal post-extraction validation
- no full distinction yet between binary and source archives
- archive layout normalization is not yet fully handled

## 14. Planned extensions

Possible future extensions include:

- explicit separation between binary and source packages
- richer validation of extracted contents
- support for additional components
- cross-platform support
- stronger package provider abstraction
- improved handling of extracted archive roots