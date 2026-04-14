# CUP README

`cup` is a prototype toolchain manager.

The current implementation is focused on compiler installation and local state management.

## Supported commands

```text
list
install <component> <tool>@<release>
remove <component> <tool>@<release>
default <component> <tool>@<release>
current <component>
```

## Examples

```bash
./cup install compiler gcc@stable
./cup install compiler clang@22.1.3
./cup remove compiler gcc@stable
./cup default compiler clang@stable
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
clang@nightly
clang@22.1.3
```

## Installation flow

The installation process is staged:

1. resolve the requested release
2. fetch the package archive
3. download the archive into the local cache if needed
4. extract the archive into a temporary directory
5. write installation metadata
6. validate the staged installation
7. move it to the final location
8. update the local state

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
│               └── package.tar.xz
└── tmp/
```

## State file

The state file is:

```text
~/.cup/state.txt
```

Installed entries are stored as:

```text
installed.<component>=<tool>@<release>
```

Default entries are stored as:

```text
default.<component>=<tool>@<release>
```

## Package manifest

Package sources are configured through:

```text
config/packages.cfg
```

The manifest contains:

- stable version mappings
- nightly version mappings
- URL templates

## Build

```bash
make
```

## Cleaning

```bash
make clean
make dev-clean
```