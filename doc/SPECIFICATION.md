# Specification

This document describes the implemented model of `cup` and the main architectural choices reflected in the current codebase.

`cup` is a user-space toolchain manager for C development tools. It installs prebuilt archives, records local state, and keeps runtime data under the user's `.cup` directory.

## 1. Purpose

`cup` is designed as a toolchain manager for C. Its purpose is to provide a small command-line interface for installing, tracking, selecting, checking and removing prebuilt C development tools without writing into system locations.

The current implementation manages toolchain components such as compilers, debuggers, linkers, formatters, linters, language servers and analyzers. Package archives are produced by the project build infrastructure, published as component release assets, and then installed locally by `cup` according to the manifest.

The basic install flow is:

```text
parse command
validate component and tool
resolve host and target platform
resolve symbolic release through the manifest
validate selected version and archive format
build package URL
fetch archive into cache
extract into temporary staging directory
validate extracted package layout
commit installation with filesystem rename
update local state
```

The basic remove flow is:

```text
parse command
validate component and tool
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

The system deliberately avoids privileged writes. It does not install into `/usr`, `Program Files`, or other system locations. All mutable runtime data belongs to the user.

## 2. Implemented scope

The current implementation is intentionally centered on the concrete toolchain-management flow implemented in the codebase:

- supported component/tool pairs are defined by the internal registry;
- available packages are described by a simple text manifest;
- installable packages are downloaded as release archives;
- package archives are validated after extraction before being committed into the local `.cup` tree;
- installed entries and defaults are recorded in local state;
- `doctor` checks the local installation and reports inconsistencies;
- `repair` performs safe state and filesystem repairs;
- `uninstall` removes cup-managed data through the installed uninstall script.

Component build workflows produce the archives that `cup` installs. At runtime, `cup` consumes the published assets and the manifest; it does not build GCC, LLVM, GDB, Valgrind or other component packages on the user's machine.

The current package model uses self-contained component archives where practical. More advanced dependency graph management between component packages is considered a possible future extension, but the implemented model prioritizes a reliable install/remove/state flow.

## 3. Supported command line

The executable supports:

```text
cup help
cup list [--target <target-platform>]
cup install <component> <tool>@<release> [--target <target-platform>] [--format|-f <archive-format>]
cup remove <component> <tool>@<release> [--target <target-platform>]
cup default <component> <tool>@<release> [--target <target-platform>]
cup current <component> [--target <target-platform>]
cup doctor
cup repair
cup uninstall
```

The entry format accepted by commands that take a tool release is:

```text
<tool>@<release>
```

Examples:

```text
gcc@stable
gcc@16.1.0-rev1
gdb@17.1
clang@22.1.5
valgrind@3.27.0
```

## 4. Domain model

### 4.1 Component

A component is a category of C development tool.

Current components are:

```text
compiler
debugger
linker
formatter
linter
language-server
analyzer
```

The component name is part of the CLI, state model, manifest lookup, and install path.

### 4.2 Tool

A tool is an implementation inside a component.

Current registry pairs are:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
linker/lld
formatter/clang-format
linter/clang-tidy
language-server/clangd
analyzer/valgrind
```

The manifest alone does not define supported tools. A component/tool pair must also be accepted by the internal registry.

### 4.3 Platform

A platform identifies where a package runs and what it targets.

Current platform identifiers are:

```text
linux-x64
windows-x64
```

A command has two platform values:

```text
host_platform
  where the installed tool runs

target_platform
  what the installed tool targets
```

The host platform is detected by `cup`. The target platform defaults to the host unless `--target` is passed.

Examples represented by the current manifest and build scripts include:

```text
linux-x64 -> linux-x64
linux-x64 -> windows-x64
windows-x64 -> windows-x64
```

Cross-target support is meaningful mainly for compilers and toolchains. Other tools are normally published as native host packages only.

### 4.4 Release

A release is the version string requested by the user.

Examples:

```text
stable
17.1
16.1.0
16.1.0-rev1
22.1.5
3.27.0
```

`stable` is symbolic and must be resolved before any state change is written.

### 4.5 Entry and canonical entry

A user entry has this form:

```text
<tool>@<release>
```

A canonical entry stores the resolved concrete release:

```text
gcc@stable       -> gcc@16.1.0-rev1
gdb@stable       -> gdb@17.1
clang@stable     -> clang@22.1.5
valgrind@stable  -> valgrind@3.27.0
```

State uses canonical entries. User-facing output may still indicate when the canonical version is the manifest's current stable version.

### 4.6 Archive format

The manifest declares available archive formats and a default format for each component/tool/host/target tuple.

Current formats are:

```text
tar.xz
tar.gz
zip
```

If `--format` or `-f` is not provided, `cup` uses the tuple's `default_format`.

## 5. Manifest model

The manifest is installed as:

```text
~/.cup/config/packages.cfg
```

The repository source copy is:

```text
config/packages.cfg
```

It is a text file made of keys in this form:

```text
<component>.<tool>.<host_platform>.<target_platform>.<field>=<value>
```

Common fields are:

```text
stable_version
available_versions
default_format
formats
url_template
```

Example:

```text
compiler.gcc.linux-x64.windows-x64.stable_version=16.1.0-rev1
compiler.gcc.linux-x64.windows-x64.available_versions=16.1.0-rev1
compiler.gcc.linux-x64.windows-x64.default_format=tar.gz
compiler.gcc.linux-x64.windows-x64.formats=tar.xz,tar.gz,zip
compiler.gcc.linux-x64.windows-x64.url_template=https://github.com/coffee-clang/cup-components/releases/download/gcc-{version}-{host_platform}-{target_platform}/gcc-{version}-{host_platform}-{target_platform}.{format}
```

The URL template supports these placeholders:

```text
{version}
{host_platform}
{target_platform}
{format}
```

The manifest is intentionally simple. The resolver performs exact key lookup rather than general package-query behavior.

## 6. Registry plus manifest

The registry and manifest serve different purposes.

The registry defines what commands are accepted:

```text
compiler/gcc
compiler/clang
...
```

The manifest defines which archives exist for each accepted tuple.

A tool present only in the manifest is not automatically accepted by the CLI. This avoids silently enabling arbitrary component names through manifest edits.

## 7. Package release model

`cup` itself and the installable tool packages are released separately.

`cup` bootstrap assets are released from:

```text
coffee-clang/cup
```

Component package assets are released from:

```text
coffee-clang/cup-components
```

The manifest points to the component repository for tool archives.

This separation keeps the `cup` repository focused on the toolchain manager itself while component assets are published from a dedicated repository.

## 8. Package version revisions

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

## 9. Filesystem layout

All runtime data is kept below:

```text
~/.cup
```

Current structure:

```text
~/.cup/
  bin/
    cup
    cup.exe
    uninstall.sh
    uninstall.ps1
  cache/
  components/
  config/
    packages.cfg
  tmp/
  state.txt
```

Installed packages are stored under:

```text
~/.cup/components/<component>/<tool>/<host_platform>/<target_platform>/<version>/
```

Example:

```text
~/.cup/components/compiler/gcc/linux-x64/windows-x64/16.1.0-rev1/
```

The package archive itself contains a top-level package directory. Extraction normalizes the layout so validation can check the extracted root consistently.

## 10. Package archive layout

A package archive is expected to contain a usable extracted tree with at least:

```text
bin/
info.txt
```

Typical packages may also contain:

```text
lib/
libexec/
include/
share/
```

`info.txt` describes the package that was built. It is produced by the build scripts and is useful for inspection and tests. It does not replace the manifest.

Windows packages include required runtime DLLs in `bin/` when the tool depends on MSYS2/UCRT64 or CLANG64 libraries. This makes the package usable outside the build environment.

Valgrind packages use a small generated launcher script for `bin/valgrind`. The real executable is `bin/valgrind.bin`. The launcher sets `VALGRIND_LIB` to the package's installed Valgrind runtime directory and then executes the real binary. This is used because Valgrind needs to locate internal tools such as Memcheck after the package has been moved under `.cup`.

## 11. State model

The state file is:

```text
~/.cup/state.txt
```

It records:

```text
installed entries
default entries per component/host/target
```

State stores canonical entries. For example, after installing `gcc@stable`, the state stores the resolved concrete version.

Defaults are scoped by:

```text
component
host_platform
target_platform
```

This allows one default compiler for `linux-x64 -> linux-x64` and a different default compiler for `linux-x64 -> windows-x64`.

## 12. State and disk consistency

`cup` checks consistency between state and disk before operations that depend on installed entries.

Examples of inconsistent states:

```text
state says an entry is installed but the install directory is missing
a default points to an entry that is no longer installed
a manifest no longer contains a version recorded in state
```

The current behavior is conservative:

- `doctor` reports inconsistencies without modifying files;
- `repair` removes stale state entries/defaults only when it can do so safely;
- invalid state syntax is not automatically repaired.

Unknown, malformed, or duplicate state/manifest content is treated as an error rather than being silently ignored.

## 13. Install flow

Install uses these major steps:

```text
validate input
load manifest
resolve host and target
resolve release
resolve archive format
check existing installation
fetch archive into cache
extract archive into temporary install directory
validate extracted package
rename temporary install directory to final install path
load/update/save state
cleanup temporary files
```

The final install path is:

```text
~/.cup/components/<component>/<tool>/<host>/<target>/<version>
```

If state saving fails after the filesystem commit, `cup` attempts to roll the filesystem change back.

## 14. Remove flow

Remove uses staging rather than directly deleting the final install directory.

The main steps are:

```text
validate input
resolve release
load state
check installed entry
rename final install path into tmp
remove installed entry from state
remove matching default if needed
save state
clean tmp staged directory
```

If the state update fails after the install directory has been moved to tmp, `cup` attempts to move it back.

## 15. Default and current

`cup default` sets the default installed tool for a component/host/target tuple.

Example:

```sh
cup default compiler gcc@stable --target windows-x64
```

The selected entry must exist both in state and on disk.

`cup current` prints the default for a component and selected target:

```sh
cup current compiler
cup current compiler --target windows-x64
```

If the stored canonical entry matches the manifest's stable version, the output may annotate it as stable.

## 16. List

`cup list` lists installed entries for the current host and selected target:

```sh
cup list
cup list --target windows-x64
```

If nothing is installed, it prints an explicit empty-state message.

## 17. Doctor

`cup doctor` checks the local installation without modifying files.

It checks:

```text
.cup directory structure
state file validity
installed entries recorded in state
manifest availability for installed entries
temporary directory leftovers
```

Warnings are used for non-blocking issues such as missing directories or temporary leftovers. Blocking inconsistencies return an error and suggest running `cup repair` after review.

## 18. Repair

`cup repair` performs safe repairs only.

It currently:

```text
creates missing cup directories
cleans temporary files
loads state
removes stale installed entries whose directories are missing
removes stale defaults that point to missing installations
saves state only if it changed
```

It does not try to infer a correct state from arbitrary invalid content. If the state file is malformed, repair stops and reports that automatic repair is not available.

## 19. Uninstall

`cup uninstall` removes `cup` itself and all cup-managed data under `.cup`.

The uninstall command does not directly delete the running executable. Instead, it starts the installed uninstall script and exits. The uninstall script removes the `.cup` tree externally.

The PATH entry is not removed. This is intentional: it is safe to leave in place and will be reused if `cup` is installed again.

## 20. Cache and temporary data

Downloaded archives are stored in:

```text
~/.cup/cache
```

Temporary install/remove/extraction data is stored in:

```text
~/.cup/tmp
```

Interrupted downloads and extractions are cleaned up where possible. Temporary leftovers can also be removed with:

```sh
cup repair
```

## 21. Interrupt handling

The implementation has interrupt-aware paths for long operations such as download, extraction, install staging, and cleanup.

The goal is to avoid leaving partial installs in the final component directory. Temporary data may remain after some failures, but it is isolated under `.cup/tmp` and can be cleaned by `cup repair`.

## 22. Package self-containment

A modular dependency solver was considered earlier in the project. The idea was to publish smaller packages and let `cup` install internal dependencies such as runtime files, Binutils, MinGW pieces, or related LLVM runtime files on demand.

That design is not part of the current implementation.

The current choice is:

```text
build archives as self-contained tool distributions
let cup install one selected archive
do not resolve package dependencies during install
```

This means a package may internally include supporting files required by the selected tool. For example:

- GCC packages can include Binutils;
- Windows-target GCC packages include MinGW-w64 runtime pieces;
- Windows packages include runtime DLLs next to executables;
- Valgrind includes its internal runtime and uses a launcher for relocation.

This keeps the runtime installer simple and makes package correctness a build/test responsibility.

## 23. Build infrastructure in the repository

The repository contains scripts and workflows that build component packages. They are grouped by purpose:

```text
scripts/build/
scripts/package/
scripts/test/
scripts/install/
scripts/bootstrap/
```

The build infrastructure is not part of the runtime package resolver. It exists to produce and validate archives referenced by the manifest.

Current package tests extract the produced archive and run tool-specific checks before publish. Component workflow publish mode uploads release assets to `coffee-clang/cup-components`. Non-publish mode uploads workflow artifacts for inspection.

## 24. Current limitations and planned extensions

Current limitations:

- supported platforms are only `linux-x64` and `windows-x64`;
- 32-bit platforms are not implemented yet;
- macOS support is not implemented yet;
- install-time dependency solving is intentionally not implemented;
- state repair is conservative and does not repair arbitrary malformed files;
- `cup uninstall` does not remove PATH entries;
- only known registry pairs are accepted.

Possible future extensions:

- macOS host packages;
- 32-bit platform support;
- richer package metadata;
- optional dependency/runtime packages;
- generated environment or symlink handling for shared runtimes;
- additional C development tools;
- stronger package verification.