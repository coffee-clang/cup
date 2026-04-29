# Specification

This document describes the current behavior and internal model of `cup`.

It is a technical reference for the implementation. It does not describe a complete future package manager.

## 1. Goal

`cup` manages local installations of development tools.

The current implementation is based on these ideas:

1. supported tools are declared in code
2. available versions and URLs are declared in a manifest
3. user entries are resolved into canonical entries
4. packages are downloaded as archives
5. archives are extracted into temporary directories
6. validated temporary installs are committed with `rename`
7. local state is stored in a text file

## 2. Core concepts

### 2.1 Component

A component is a category of tools.

Current components:

```text
compiler
debugger
```

Components are validated by the registry module.

### 2.2 Tool

A tool belongs to a component.

Current tools:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
```

A tool is valid only if the registry declares it for the selected component.

### 2.3 Release

A release is the version string requested by the user.

Examples:

```text
stable
15.2.0
17.1
22.1.3
```

`stable` is symbolic and must be resolved through the manifest.

### 2.4 Entry

An entry is the user-facing `<tool>@<release>` form.

Examples:

```text
gcc@stable
gcc@15.2.0
lldb@22.1.3
```

### 2.5 Canonical entry

A canonical entry is the internal stored form.

It always contains a concrete release.

Example:

```text
gcc@15.2.0
```

The state stores canonical entries only.

## 3. Manifest-driven behavior

The manifest is the source of package metadata.

Path:

```text
config/packages.cfg
```

For each component/tool pair, the manifest can define:

```text
stable_version
available_versions
default_format
formats
url_template
```

Example for repository-built GNU packages:

```text
compiler.gcc.stable_version=15.2.0
compiler.gcc.available_versions=15.2.0
compiler.gcc.default_format=tar.gz
compiler.gcc.formats=tar.gz,tar.xz
compiler.gcc.url_template=https://github.com/coffee-clang/cup/releases/download/gcc-{version}-standard/gcc-{version}-linux-x64-standard.{format}
```

Example for repository-built LLVM packages:

```text
debugger.lldb.stable_version=22.1.3
debugger.lldb.available_versions=22.1.3
debugger.lldb.default_format=tar.xz
debugger.lldb.formats=tar.gz,tar.xz
debugger.lldb.url_template=https://github.com/coffee-clang/cup/releases/download/lldb-{version}-linux-x64/lldb-{version}-linux-x64.{format}
```

The registry decides whether a component/tool pair is valid. The manifest decides which versions and formats exist for that pair.

## 4. Release resolution

Release resolution is performed before availability checks.

If the user passes:

```text
gcc@stable
```

and the manifest contains:

```text
compiler.gcc.stable_version=15.2.0
```

then the resolved release is:

```text
15.2.0
```

The canonical entry becomes:

```text
gcc@15.2.0
```

Availability is checked against `available_versions` after resolution.

## 5. Archive format model

Each tool has:

```text
default_format
formats
```

If the user does not pass a format override, `default_format` is used.

If the user passes:

```text
--format tar.xz
```

or:

```text
-f tar.xz
```

then the selected format must be listed in `formats`.

The selected format replaces `{format}` in the URL template.

## 6. Package source model

The current manifest points to packages built and published by this repository.

### 6.1 Repository-built GNU packages

GCC and GDB are built by this repository from upstream source releases.

Their URLs include a build mode, currently `standard`, because the package name is produced by the GNU build workflow.

Example:

```text
gdb-17.1-linux-x64-standard.tar.xz
```

### 6.2 Repository-built LLVM packages

Clang and LLDB are built by this repository from LLVM source releases.

Their URLs are platform-based rather than build-mode-based.

Examples:

```text
clang-22.1.3-linux-x64.tar.xz
lldb-22.1.3-linux-x64.tar.xz
```

For now, the platform `linux-x64` is internally mapped to the LLVM target `X86`.

The Clang package is built with the `clang` LLVM project only. `lld` is not included in that package, so it can remain a possible separate linker tool later.

The LLDB package is built with `clang;lldb`. Clang is included as a technical dependency needed by LLDB, while the package remains classified as `debugger.lldb`.

## 7. State model

The state file is stored at:

```text
~/.cup/state.txt
```

It contains lines in this form:

```text
installed.<component>=<canonical-entry>
default.<component>=<canonical-entry>
```

Example:

```text
installed.compiler=gcc@15.2.0
default.compiler=gcc@15.2.0
```

### 7.1 State invariants

The intended invariants are:

- installed entries are canonical
- default entries are canonical
- `stable` is not stored in state
- default entries should point to installed entries
- installed entries should correspond to directories on disk

The last two invariants are checked by command-level logic, not guaranteed by the state file alone.

### 7.2 State save

State saving uses a temporary file followed by `rename`.

Conceptually:

```text
write ~/.cup/state.txt.tmp
rename ~/.cup/state.txt.tmp -> ~/.cup/state.txt
```

## 8. Filesystem model

Main local root:

```text
~/.cup
```

Main subdirectories:

```text
~/.cup/cache
~/.cup/components
~/.cup/tmp
```

### 8.1 Cache paths

Package archives are cached by component, tool, and release.

Example:

```text
~/.cup/cache/compiler/gcc/15.2.0/gcc-15.2.0.tar.gz
```

### 8.2 Install paths

Installed tools are placed under:

```text
~/.cup/components/<component>/<tool>/<platform>/<release>
```

Example:

```text
~/.cup/components/compiler/gcc/linux/15.2.0
```

The platform component is currently simple. Complete multi-architecture support is not implemented.

### 8.3 Temporary paths

Temporary install and remove directories are placed under:

```text
~/.cup/tmp
```

Examples:

```text
~/.cup/tmp/install-compiler-gcc-15.2.0-12345
~/.cup/tmp/remove-compiler-gcc-15.2.0-12345
```

The process id is used as part of the suffix.

## 9. Install flow

The install command follows this sequence:

```text
1. parse entry
2. validate component
3. validate tool for component
4. resolve release
5. build canonical entry
6. check version availability
7. select archive format
8. load state
9. check whether the installation is already present or inconsistent
10. create temporary install directory
11. fetch package archive
12. extract package archive
13. write install metadata
14. validate temporary installation
15. create final parent directories
16. commit temporary install path to final install path
17. add canonical entry to state
18. save state
```

### 9.1 Commit

The filesystem commit uses `rename`.

Conceptually:

```text
temporary install path -> final install path
```

This is treated as the point where the installation becomes visible on disk.

### 9.2 Install rollback

If the filesystem commit succeeds but the state update or state save fails, the implementation attempts to remove the committed installation.

The rollback uses the same staged removal idea:

```text
final install path -> temporary remove path
cleanup temporary remove path
```

If rollback also fails, the command returns a rollback error.

## 10. Remove flow

The remove command follows this sequence:

```text
1. parse entry
2. validate component/tool
3. resolve release
4. build canonical entry
5. load state
6. check state/disk consistency
7. create temporary remove directory
8. move final install path to temporary remove path
9. remove canonical entry from state
10. remove matching default if present
11. save state
12. clean temporary remove directory
```

### 10.1 Remove rollback

If state saving fails after moving the final install path to the temporary remove path, the implementation can try to restore it:

```text
temporary remove path -> final install path
```

## 11. Commit path

The filesystem layer uses a generic commit operation:

```text
commit_path(source, destination)
```

The operation is a `rename`.

It is used for:

```text
install commit
remove staging
remove rollback
```

The name is intentionally generic because the operation is the same even if the command-level meaning changes.

## 12. Archive extraction model

Extraction is performed through `libarchive`.

For each archive entry, the extraction logic:

1. reads the archive pathname
2. rejects invalid or unsafe paths
3. strips the first path component
4. builds the output path under the temporary directory
5. rewrites hardlink targets when present
6. writes the entry to disk

### 12.1 First-component strip

Packages are expected to contain a top-level directory.

Example:

```text
gcc-15.2.0-linux-x64-standard/bin/gcc
```

The first component is stripped:

```text
bin/gcc
```

This keeps the final installation layout independent from the package root directory name.

### 12.2 Path safety

Extraction rejects paths that are absolute or contain parent-directory references.

Examples rejected:

```text
/etc/passwd
../file
dir/../file
```

Symlink targets are not rewritten. Hardlink targets are rewritten because hardlinks refer to filesystem paths inside the extracted tree.

## 13. Validation model

Validation currently checks the minimal layout expected after extraction and metadata writing.

Required:

```text
tmp_path is a directory
tmp_path/info.txt is a non-empty regular file
tmp_path/bin is a directory
```

This is intentionally not a complete tool validation.

Future validation may check:

```text
bin/<tool>
executable bits
component-specific files
```

## 14. Interrupt model

`SIGINT` handling is flag-based.

The signal handler only records that an interrupt was requested.

Longer operations periodically check the flag and return `CUP_ERR_INTERRUPT` when appropriate.

Cleanup resets the interrupt flag before running. This makes it possible for the first interrupt to stop the main operation and a second interrupt to stop cleanup.

## 15. Module responsibilities

### `main`

CLI entry point and command dispatch.

### `commands`

Command orchestration and command-level consistency handling.

### `state`

State file load/save and in-memory state operations.

### `registry`

Supported component/tool validation.

### `manifest`

Manifest lookup, release resolution, format checks, URL construction, stable checks.

### `fetch`

Package download and cache reuse.

### `extract`

Archive extraction and archive path rewriting.

### `filesystem`

Local paths, directory creation, temporary directories, commit, cleanup, disk existence checks.

### `interrupt`

Signal setup and interrupt flag.

### `util`

Shared helpers.

### `constants`

Project-wide fixed limits.

### `error`

Project-wide error enum.

## 16. GNU source release builds

The project contains build automation for GNU source releases that need to be compiled before `cup` can install them as archives.

The current structure is:

```text
.github/workflows/build-gnu.yml
docker/gnu-builder.Dockerfile
scripts/build-gnu-package.sh
scripts/build-gcc.sh
scripts/build-gdb.sh
```

The workflow is manually started and receives:

```text
tool
version
build_mode
```

The workflow always builds and then uploads release assets. Existing assets for the same tag are overwritten.

This build system is separate from runtime installation. `cup` itself only downloads and installs archives referenced by the manifest.

## 17. LLVM source release builds

The project contains a separate LLVM build workflow for Clang and LLDB archives.

The current structure is:

```text
.github/workflows/build-llvm.yml
docker/llvm-builder.Dockerfile
scripts/build-llvm-package.sh
scripts/build-clang.sh
scripts/build-lldb.sh
```

The workflow is manually started and receives:

```text
tool
version
platform
```

The current platform option is:

```text
linux-x64
```

The scripts map this platform internally to:

```text
LLVM_TARGETS_TO_BUILD=X86
```

`build-clang.sh` builds:

```text
LLVM + Clang
```

`build-lldb.sh` builds:

```text
LLVM + Clang components + LLDB
```

LLDB uses Clang as a technical dependency, while the resulting package is still treated as the `debugger.lldb` package.

## 18. Limitations

Current limitations include:

- no dependency resolution
- no automatic repair command
- no complete multi-architecture selection
- no deep component-specific validation of extracted packages
- release package builds are separate from runtime installation logic

These limitations are part of the current design state and should not be documented as implemented features.
