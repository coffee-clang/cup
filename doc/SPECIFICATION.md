# Specification

This document describes the implemented model of `cup` and the main architectural choices reflected in the current codebase.

This file documents the current system: what it accepts, how it resolves packages, where it writes data, and why the implementation intentionally stays simple.

## 1. Purpose

`cup` manages local user-space installations of C development tools.

The implemented install flow is:

```text
parse command
validate component/tool
resolve host and target platform
resolve symbolic release through the manifest
validate selected version and archive format
build package URL
fetch archive into cache
extract into temporary staging directory
validate extracted layout
commit installation with filesystem rename
update local state
```

The implemented remove flow is:

```text
parse command
validate component/tool
resolve host and target platform
resolve release through the manifest
load local state
check state/disk consistency
move installed directory into temporary staging
remove installed entry from state
remove matching default if needed
save state
clean temporary staged directory
```

The system deliberately avoids privileged writes. It does not install into `/usr`, `Program Files`, or other system locations. All runtime data belongs to the user.

## 2. Current design boundaries

`cup` is not implemented as a general package manager.

The active boundaries are:

- packages are selected from a text manifest;
- the registry accepts only known component/tool pairs;
- packages are downloaded as archives;
- package archives are expected to be self-contained enough to work after extraction;
- the state file stores installed entries and defaults;
- the code detects state/disk inconsistencies but does not perform automatic repair;
- dependency resolution is handled outside `cup`, at package build time.

## 3. Architectural choices

### 3.1 Text manifest instead of embedded package index

The package index is stored in `config/packages.cfg` and installed to `~/.cup/config/packages.cfg` by the bootstrap installers.

The manifest is a simple text file because the current project needs predictable lookup rules more than a flexible metadata database. The code reads exact keys based on component, tool, host platform, target platform, and field name.

This keeps the resolver easy to inspect and easy to update during development.

### 3.2 Registry plus manifest

The manifest alone does not define what the CLI accepts.

The code first validates the component/tool pair through the internal registry, then reads the package metadata from the manifest. This prevents arbitrary manifest keys from silently becoming supported commands.

Current registry pairs:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
linker/lld
```

### 3.3 Self-contained packages instead of install-time dependency solving

A modular dependency solver was considered earlier in the project. The idea was to publish smaller packages and let `cup` install internal dependencies such as runtime files, Binutils, MinGW pieces, or related tools on demand.

That design was not adopted in the current implementation.

The reasons were practical:

- dependency resolution would make the installer significantly more complex;
- cross-target toolchains often need tightly coordinated files, search paths, and target prefixes;
- moving runtime pieces through symlinks or generated target directories is fragile, especially on Windows;
- it is easy to produce installations that are modular on paper but hard to validate;
- the project currently needs a reliable install/remove/state flow more than a minimal package graph.

The current choice is therefore:

```text
Build package archives as self-contained tool distributions.
Let cup install one selected package archive.
Do not resolve package dependencies at install time.
```

This means a package may internally include supporting files required by the selected tool. For example, a GCC package targeting Windows may include Binutils and MinGW-w64 files inside the same archive instead of asking `cup` to install those pieces separately.

### 3.4 User-space installation only

All mutable runtime data is kept under:

```text
~/.cup
```

The project avoids system package manager behavior. It does not write into system directories and does not require `sudo` or administrator privileges for normal operation.

### 3.5 Staged filesystem operations

Install and remove operations are staged through `~/.cup/tmp`.

For install:

```text
extract archive into tmp
validate tmp
rename tmp -> final install path
update state
```

If the state update fails after the filesystem commit, `cup` attempts to roll the install back by moving the final install path back into tmp and cleaning it.

For remove:

```text
rename final install path -> tmp
update state
clean tmp
```

If the state update fails after staging the remove, `cup` attempts to roll the tmp directory back into the final install path.

The goal is not to implement full transactional storage. The goal is to avoid the most common inconsistent states during normal failures.

### 3.6 Concrete version strings include package revisions

Package revisions are represented directly in the version string:

```text
16.1.0-rev1
```

This keeps all version-dependent identifiers homogeneous:

```text
manifest available_versions contains 16.1.0-rev1
state stores gcc@16.1.0-rev1
cache names include 16.1.0-rev1
install paths include 16.1.0-rev1
release tags include 16.1.0-rev1
archive names include 16.1.0-rev1
```

The suffix is `-revN`, not `-rN`, to avoid conflicting with upstream version strings that may already contain `-r`.

Revisions are used when the upstream version is unchanged but the package recipe, bundled runtime, archive contents, or metadata has changed.

## 4. Domain model

### 4.1 Component

A component is a category of C development tools.

Currently implemented components:

```text
compiler
debugger
linker
```

### 4.2 Tool

A tool is an implementation inside a component.

Currently implemented registry pairs:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
linker/lld
```

### 4.3 Platform

A platform identifies where a package runs and what it targets.

Current platform identifiers:

```text
linux-x64
windows-x64
```

A command has two platform values:

```text
host_platform
  detected from the executable environment

target_platform
  defaults to host_platform unless --target is passed
```

Examples currently represented by the manifest/build scripts include:

```text
linux-x64 -> linux-x64
linux-x64 -> windows-x64
windows-x64 -> windows-x64
```

The manifest is keyed by both host and target.

### 4.4 Release

A release is the version string requested by the user.

Examples:

```text
stable
17.1
16.1.0
16.1.0-rev1
22.1.5
```

`stable` is symbolic and must be resolved before state changes.

### 4.5 Entry

A user entry has this form:

```text
<tool>@<release>
```

Examples:

```text
gcc@stable
gdb@17.1
clang@22.1.5
lld@22.1.5
gcc@16.1.0-rev1
```

### 4.6 Canonical entry

A canonical entry is the resolved internal form.

Examples:

```text
gdb@stable -> gdb@17.1
gcc@stable -> gcc@16.1.0-rev1
lld@stable -> lld@22.1.5
```

The state stores canonical entries only. It does not store `stable`.

This avoids ambiguity when the manifest changes. An installation made when `stable` resolved to one version must continue to refer to that concrete version.

## 5. Manifest model

### 5.1 Location

Manifest lookup order:

```text
./config/packages.cfg
~/.cup/config/packages.cfg
```

The repository path supports development. The home path supports bootstrap installations.

### 5.2 Key format

Manifest keys are scoped by component, tool, host, and target:

```text
<component>.<tool>.<host_platform>.<target_platform>.<field>=<value>
```

Example:

```text
debugger.gdb.linux-x64.linux-x64.stable_version=17.1
```

### 5.3 Required fields

For an installable tuple, the manifest should provide:

```text
stable_version
available_versions
default_format
formats
url_template
```

Meaning:

```text
stable_version
  concrete version used when the user requests stable

available_versions
  comma-separated allow-list of concrete versions

default_format
  archive format used when --format is not provided

formats
  comma-separated allow-list of accepted archive formats

url_template
  template used to build the package URL
```

### 5.4 URL placeholders

The current URL templates use placeholders such as:

```text
{version}
{host_platform}
{target_platform}
{format}
```

Example:

```text
https://github.com/coffee-clang/cup/releases/download/gdb-{version}-{host_platform}-{target_platform}/gdb-{version}-{host_platform}-{target_platform}.{format}
```

The URL is resolved after the release and archive format have been validated.

## 6. State model

The state file is stored at:

```text
~/.cup/state.txt
```

It stores two kinds of records:

```text
installed.<component>.<host_platform>.<target_platform>=<canonical_entry>
default.<component>.<host_platform>.<target_platform>=<canonical_entry>
```

Examples:

```text
installed.debugger.linux-x64.linux-x64=gdb@17.1
default.compiler.linux-x64.windows-x64=gcc@16.1.0-rev1
```

The state is loaded into fixed-size arrays in memory and written back atomically through a temporary state file followed by a rename.

Installed entries and defaults are scoped by component, host platform, and target platform. This allows separate defaults for different targets.

## 7. Filesystem model

Runtime root:

```text
~/.cup
```

Main subdirectories:

```text
~/.cup/cache
~/.cup/components
~/.cup/tmp
~/.cup/config
```

Installed package layout:

```text
~/.cup/components/<component>/<tool>/<host_platform>/<target_platform>/<version>/
```

Examples:

```text
~/.cup/components/compiler/gcc/linux-x64/linux-x64/16.1.0/
~/.cup/components/compiler/gcc/linux-x64/windows-x64/16.1.0-rev1/
~/.cup/components/linker/lld/windows-x64/windows-x64/22.1.5/
```

Cache layout is built from the requested package information and archive format. The cache stores downloaded archives so repeated installs do not always need to download again.

Temporary install/remove directories are created under `~/.cup/tmp` and are cleaned on normal command startup and after staged operations.

## 8. Archive model

Supported package formats are described by the manifest. The current packages use:

```text
tar.gz
tar.xz
zip
```

Archives are extracted with `libarchive`; external `tar` or `unzip` commands are not used by `cup` itself.

During extraction, the implementation normalizes the common top-level archive directory. Package archives are expected to include:

```text
info.txt
bin/
```

The current generic install validation requires a `bin` directory in the extracted root.

## 9. Command model

Implemented commands:

```text
cup list [--target <target-platform>]
cup install <component> <tool>@<release> [--target <target-platform>] [--format|-f <archive-format>]
cup remove <component> <tool>@<release> [--target <target-platform>]
cup default <component> <tool>@<release> [--target <target-platform>]
cup current <component> [--target <target-platform>]
```

Supported options:

```text
--target <target-platform>
--format <archive-format>
-f <archive-format>
```

`--format` is valid only for `install`.

## 10. Error handling and consistency

The implementation checks both state and disk before install/remove/default operations.

Examples of detected inconsistencies:

```text
state says an entry is installed, but the installation directory is missing
installation directory exists, but the state does not list it
```

The current implementation reports these cases and stops. It does not attempt automatic repair.

Install rollback:

```text
if tmp -> install succeeds but state update/save fails:
  install -> tmp
  cleanup tmp
```

Remove rollback:

```text
if install -> tmp succeeds but state update/save fails:
  tmp -> install
```

Windows-specific remove behavior includes avoiding directory rename over an already-created destination directory during remove staging.

## 11. Interrupt behavior

Interrupt handling is implemented for long-running operations.

Current behavior:

- downloads are interrupted through the libcurl progress callback;
- interrupted downloads remove the partial archive;
- extraction checks the interrupt flag during header and data loops;
- interrupted extraction returns an interrupt error;
- the install command cleans the temporary installation directory after an interrupted extraction;
- remove avoids voluntarily stopping after the critical staging step has begun.

The cleanup after interrupted extraction may take time for large packages because many partially extracted files may need to be deleted.

## 12. Windows filesystem behavior

The Windows system layer implements platform-specific filesystem operations.

Current Windows-specific points:

- file removal uses a platform function instead of C `remove()`;
- read-only file attributes are cleared before deletion;
- directory removal uses the Windows directory API;
- rename/commit uses Windows rename behavior through the system layer;
- reparse points are identified so recursive removal does not traverse junctions or symlink-like directory entries.

Wide-character Windows APIs are not yet implemented. The current Windows implementation still uses narrow WinAPI calls.

## 13. Build and package scripts

The repository contains two categories of build scripts.

Scripts for the `cup` executable:

```text
scripts/bootstrap-linux-deps.sh
scripts/bootstrap-windows-deps.sh
Makefile
```

Scripts for tool packages:

```text
scripts/package-common.sh
scripts/build-gcc.sh
scripts/build-gdb.sh
scripts/build-gnu-package.sh
scripts/build-llvm-tool.sh
```

Tool package scripts publish archives whose names match the manifest URL templates. `cup` itself does not build these tools during install.

## 14. Current limitations

Current known limitations:

- supported platforms are limited to `linux-x64` and `windows-x64`;
- package metadata is read from a local text manifest, not from a remote index service;
- package validation is generic and currently requires only a `bin` directory;
- automatic repair of inconsistent state/disk situations is not implemented;
- install-time dependency solving is not implemented;
- Windows wide-character path support is not implemented yet;
- LLVM package recipes currently support native host/target combinations only.
