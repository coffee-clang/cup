# cup

`cup` is a small local manager for development tools and toolchains.

The current implementation installs tools from prebuilt archives described by a manifest. It keeps a local state file, resolves symbolic releases such as `stable`, downloads package archives, extracts them into a temporary directory, validates the extracted layout, and commits the installation with a filesystem rename.

The project is still in development. This document describes the current behavior of the codebase.

## 1. Current scope

The current implementation supports:

- command handling for `list`, `install`, `remove`, `default`, and `current`
- supported component/tool validation through the registry module
- manifest-driven versions, archive formats, and package URLs
- user entries written as `<tool>@<release>`
- canonical internal entries such as `gcc@15.2.0`
- cached archive downloads
- archive extraction through `libarchive`
- first-component stripping during extraction
- install/remove staging through temporary directories
- atomic path commits through `rename`
- one default entry per component
- basic `SIGINT` handling
- static dependency bootstrap for building `cup`
- Docker-based GitHub Actions builds for GNU source releases such as GCC and GDB

The current implementation does not yet provide:

- dependency solving
- automatic state repair
- complete multi-architecture selection
- deep component-specific validation of extracted packages

## 2. Build

Before building `cup` statically, prepare the static dependencies:

```sh
bash scripts/bootstrap-static-deps.sh
```

Then build the project:

```sh
make
```

The default dependency prefix used by the `Makefile` is:

```text
~/deps/install
```

The build uses:

```text
-Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L
```

`_POSIX_C_SOURCE` is required because the code uses POSIX functions such as `lstat`, `opendir`, `readdir`, `rmdir`, and `getpid`.

## 3. Basic usage

### List installations

```sh
./cup list
```

Shows installations registered in the local state.

The output may also indicate:

- which entry is the default for a component
- whether an installed entry matches the current `stable` version from the manifest
- basic state/disk inconsistencies detected while listing

### Install a tool

```sh
./cup install <component> <tool>@<release>
```

Example:

```sh
./cup install compiler gcc@stable
```

With an explicit archive format:

```sh
./cup install compiler gcc@stable --format tar.xz
./cup install compiler gcc@stable -f tar.xz
```

If no format is provided, the manifest `default_format` is used.

### Remove a tool

```sh
./cup remove <component> <tool>@<release>
```

Example:

```sh
./cup remove compiler gcc@15.2.0
```

Removal does not directly delete the final installation directory. The directory is first moved to a temporary remove directory, the state is updated, and the temporary directory is cleaned afterwards.

### Set a default

```sh
./cup default <component> <tool>@<release>
```

Example:

```sh
./cup default compiler gcc@15.2.0
```

A default can only be set if the installation exists both in the state and on disk.

### Show the current default

```sh
./cup current <component>
```

Example:

```sh
./cup current compiler
```

If the stored default matches the current `stable` version from the manifest, the output may show it as an annotation while still printing the canonical version:

```text
Current compiler default: gcc@15.2.0 (stable)
```

## 4. Entry model

User input uses entries in this form:

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

`stable` is a symbolic release. It is resolved through the manifest before being stored in the state.

For example, if the manifest contains:

```text
compiler.gcc.stable_version=15.2.0
```

then:

```text
gcc@stable
```

is resolved internally to:

```text
gcc@15.2.0
```

The state always stores canonical entries. It does not store `stable`.

This distinction matters because `stable` may change later in the manifest, while an installed or default entry should continue to refer to the concrete version selected at the time.

## 5. Manifest model

The package manifest is:

```text
config/packages.cfg
```

It defines package metadata using keys in this form:

```text
<component>.<tool>.<field>=<value>
```

Example:

```text
compiler.gcc.stable_version=15.2.0
compiler.gcc.available_versions=15.2.0,15.1.0
compiler.gcc.default_format=tar.gz
compiler.gcc.formats=tar.gz,tar.xz
compiler.gcc.url_template=https://github.com/coffee-clang/cup/releases/download/gcc-{version}-full/gcc-{version}-linux-x64-full.{format}
```

The manifest controls:

- stable release
- available versions
- default archive format
- supported archive formats
- package URL template

The URL template supports:

```text
{version}
{format}
```

The implementation replaces these placeholders after resolving the release and selecting the archive format.

## 6. Supported components and tools

Supported components and tools are defined in the registry module.

Current component categories are:

```text
compiler
debugger
```

Current tools include:

```text
compiler: gcc, clang
debugger: gdb
```

The registry validates whether a component/tool pair is supported. It does not know which versions exist and does not build URLs. Version and URL data come from the manifest.

## 7. Local filesystem layout

`cup` stores its local data under:

```text
~/.cup
```

Expected layout:

```text
~/.cup/
├── cache/
├── components/
├── tmp/
└── state.txt
```

### Cache

Downloaded archives are stored under `cache`.

Example:

```text
~/.cup/cache/compiler/gcc/15.2.0/gcc-15.2.0.tar.gz
```

### Components

Installed tools are stored under `components`.

Example:

```text
~/.cup/components/compiler/gcc/linux/15.2.0/
```

The current platform name is simple and fixed by the implementation. Complete multi-architecture support is not implemented yet.

### Temporary directories

Install and remove operations use temporary directories.

Examples:

```text
~/.cup/tmp/install-compiler-gcc-15.2.0-12345
~/.cup/tmp/remove-compiler-gcc-15.2.0-12345
```

## 8. State file

The state file is:

```text
~/.cup/state.txt
```

Example:

```text
installed.compiler=gcc@15.2.0
default.compiler=gcc@15.2.0
```

The state contains:

- installed canonical entries
- default canonical entries per component

The state is saved through a temporary file and then replaced with `rename`.

## 9. Install behavior

Installation follows this high-level flow:

```text
resolve entry
check manifest availability
choose archive format
load state
check state/disk consistency
create temporary install directory
fetch package archive
extract archive
write package metadata
validate temporary installation
create final component directories
commit temporary directory to final path
update state
save state
```

The final filesystem commit is a rename:

```text
temporary install path -> final install path
```

If the final filesystem commit succeeds but updating the state fails, the code attempts to roll back the committed installation.

## 10. Remove behavior

Removal uses a staged model:

```text
load state
check state/disk consistency
create temporary remove directory
move final install path to temporary remove path
update state in memory
save state
clean temporary remove directory
```

This keeps the destructive part separated from the state update. If state saving fails after the install path has been moved, the code can attempt to move it back.

## 11. Archive extraction

Archives are extracted with `libarchive`.

The extraction code:

- rejects absolute paths
- rejects unsafe parent references
- rewrites archive paths under the temporary install directory
- rewrites hardlink targets under the same temporary directory
- does not rewrite symlink targets
- strips the first path component

Because of first-component stripping, packages are expected to contain one top-level directory.

Example archive layout:

```text
gcc-15.2.0-linux-x64-full/bin/gcc
gcc-15.2.0-linux-x64-full/lib/...
```

Extracted layout:

```text
tmp_path/bin/gcc
tmp_path/lib/...
```

## 12. Installation validation

After extraction and metadata writing, the temporary installation is validated.

The current validation is intentionally minimal. It checks that:

```text
tmp_path exists and is a directory
tmp_path/info.txt exists, is a regular file, and is not empty
tmp_path/bin exists and is a directory
```

More specific checks, such as verifying `bin/<tool>` or other component-specific files, can be added later.

## 13. Source layout

The main source modules are:

```text
main.c
commands.c / commands.h
state.c / state.h
registry.c / registry.h
manifest.c / manifest.h
fetch.c / fetch.h
extract.c / extract.h
filesystem.c / filesystem.h
interrupt.c / interrupt.h
util.c / util.h
constants.h
error.h
```

Module roles:

- `main`: CLI dispatch
- `commands`: command-level orchestration
- `state`: local state file handling
- `registry`: supported components and tools
- `manifest`: package metadata lookup
- `fetch`: download and cache handling
- `extract`: archive extraction
- `filesystem`: paths, directories, temporary storage, commit, cleanup
- `interrupt`: signal flag handling
- `util`: shared utility functions
- `constants`: shared limits
- `error`: project error enum

## 14. Building GNU release packages

The repository includes automation for building GNU source releases into archives installable by `cup`.

The current build structure is:

```text
.github/workflows/build-gnu.yml
docker/gnu-builder.Dockerfile
scripts/build-gnu-package.sh
scripts/build-gcc.sh
scripts/build-gdb.sh
```

GitHub Actions orchestrates the build. Docker provides the controlled build environment.

The workflow is manual and takes:

```text
tool
version
build_mode
```

Example release tags:

```text
gcc-15.2.0-full
gdb-17.1-full
```

Example assets:

```text
gcc-15.2.0-linux-x64-full.tar.xz
gdb-17.1-linux-x64-full.tar.xz
```

The current policy is simple: each manual run builds the package and uploads the assets, overwriting existing assets for the same release tag.
