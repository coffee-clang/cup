# CUP README

`cup` is a prototype toolchain manager for installing software components in user space.

The current implementation supports:

- compiler tools
- debugger tools

The current supported tools are:

- `gcc`
- `clang`
- `gdb`

## Supported commands

```text
list
install <component> <tool>@<release> [--format <archive-format>]
install <component> <tool>@<release> [-f <archive-format>]
remove <component> <tool>@<release>
default <component> <tool>@<release>
current <component>
```

## Examples

```bash
./cup install compiler gcc@stable
./cup install compiler gcc@15.2.0 --format tar.xz
./cup install compiler clang@22.1.3
./cup install debugger gdb@stable
./cup remove compiler gcc@15.2.0
./cup default compiler clang@22.1.3
./cup current compiler
./cup list
```

## Entry format

Entries are expressed as:

```text
<tool>@<release>
```

Examples:

```text
gcc@stable
gcc@15.2.0
clang@22.1.3
gdb@17.1
```

## Release model

The current implementation supports:

- `stable`
- explicit versions

`stable` is resolved through the package manifest.

The implementation no longer relies on a `nightly` release alias.

## Archive format selection

If no archive format is specified, the default format configured for the selected tool is used.

If a format is specified with `--format` or `-f`, `cup` verifies that the tool supports that format before continuing.

Current archive formats are manifest-driven and currently include:

- `tar.gz`
- `tar.xz`

Examples:

```bash
./cup install compiler gcc@15.2.0 --format tar.gz
./cup install compiler gcc@15.2.0 --format tar.xz
```

## Installation flow

The installation process is staged:

1. parse and validate the entry
2. validate the component
3. validate the tool for the selected component
4. resolve the requested release
5. build the canonical entry
6. check that the resolved version is available
7. select the archive format
8. fetch the package archive from cache or remote source
9. extract the archive into a temporary directory
10. write installation metadata
11. validate the staged installation
12. move the staged installation to the final location
13. update the local state

## Canonical entries

User input is treated as a request.

Internal state is stored using canonical entries in the form:

```text
<tool>@<resolved_version>
```

Example:

```text
gcc@stable
```

may be resolved and stored internally as:

```text
gcc@15.2.0
```

This keeps the filesystem layout and the persistent state consistent.

## Local storage

Runtime data is stored under:

```text
~/.cup/
```

Structure:

```text
~/.cup/
├── state.txt
├── components/
│   └── <component>/
│       └── <tool>/
│           └── <platform>/
│               └── <release>/
├── cache/
│   └── <component>/
│       └── <tool>/
│           └── <release>/
│               └── package.<format>
└── tmp/
```

Examples:

```text
~/.cup/components/compiler/gcc/linux/15.2.0
~/.cup/components/debugger/gdb/linux/17.1
~/.cup/cache/compiler/gcc/15.2.0/package.tar.gz
~/.cup/cache/compiler/gcc/15.2.0/package.tar.xz
```

## State file

The state file is:

```text
~/.cup/state.txt
```

Installed entries are stored as:

```text
installed.<component>=<tool>@<resolved_version>
```

Default entries are stored as:

```text
default.<component>=<tool>@<resolved_version>
```

## Package manifest

Package metadata is configured through:

```text
config/packages.cfg
```

The manifest currently stores:

- stable version mappings
- available versions
- default archive formats
- supported archive formats
- URL templates

Example structure:

```text
compiler.gcc.stable_version=15.2.0
compiler.gcc.available_versions=15.2.0,15.1.0
compiler.gcc.default_format=tar.gz
compiler.gcc.formats=tar.gz,tar.xz
compiler.gcc.url_template=https://.../gcc-{version}-linux-x64-full.{format}
```

## Source code structure

The current source tree is divided into the following modules:

- `main.c` — command parsing and dispatch
- `component.c` — command orchestration
- `state.c` — persistent state handling
- `fs.c` — filesystem paths, tmp/cache/commit/cleanup logic
- `manifest.c` — manifest reading, release resolution, version and format checks
- `support.c` — supported components and tools
- `fetch.c` — package download and cache fetch logic
- `archive.c` — archive extraction
- `util.c` — shared low-level utilities
- `constants.h` — shared size limits and common constants

## Build

```bash
make
```

## Cleaning

```bash
make clean
make dev-clean
```