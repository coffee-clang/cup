# Specification

This document describes the implemented model of `cup` and the main architectural choices reflected in the current codebase.

`cup` is a user-space toolchain manager for C development tools. It installs prebuilt packages, validates package metadata, records local state and keeps all runtime data under the user's `.cup` directory.

## 1. Scope

The `cup` repository provides:

```text
command-line interface
component/tool registry
platform detection and validation
manifest loading and package URL resolution
archive download and extraction
local installation state
package metadata validation
filesystem layout management
doctor, repair and uninstall operations
bootstrap installers for cup itself
```

The installation model is deliberately user-space only. `cup` does not require `sudo`, administrator privileges, system package managers, system-wide toolchain directories or global PATH edits.

## 2. Command line

The executable supports:

```text
cup help
cup list [--target <target-platform>]
cup install <component> <tool>@<release> [--target <target-platform>] [--format|-f <archive-format>]
cup remove <component> <tool>@<release> [--target <target-platform>]
cup default <component> <tool>@<release> [--target <target-platform>]
cup current <component> [--target <target-platform>]
cup info <component> <tool>@<release> [--target <target-platform>]
cup doctor
cup repair
cup uninstall
```

Commands that refer to a concrete tool release use this entry format:

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

The only global options currently accepted by commands are:

```text
--target <target-platform>
--format <archive-format>
-f <archive-format>
```

`--format` and `-f` are accepted only by `install`.

## 3. Domain model

### 3.1 Component

A component is a category of C development tool. The component name is part of the command line, state keys, manifest keys and installation path.

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

### 3.2 Tool

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

### 3.3 Platform

A platform identifies an operating system and architecture in this form:

```text
<os>-<arch>
```

Recognized platform identifiers are:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Each operation has two platform values:

```text
host_platform
  where the installed tool runs

target_platform
  what the installed tool targets
```

The host platform is detected from the running system. The target platform defaults to the host unless the user passes `--target`.

`cup` installs packages for the current host. A non-host target means that the installed package still runs on the current host, but produces or manages output for the requested target.

### 3.4 Release

A release is the version string requested by the user. `stable` is symbolic and is resolved through the manifest before an installation, removal, default selection or metadata query is performed.

Examples:

```text
stable
16.1.0-rev1
17.1
22.1.5
3.27.0
```

State entries store the resolved concrete release, not the symbolic `stable` input.

### 3.5 Archive format

Package archives are selected through the manifest. Current archive formats are:

```text
tar.xz
tar.gz
zip
```

If the user does not pass `--format` or `-f`, `cup` uses the tuple's `default_format` from the manifest.

## 4. Manifest model

The installed manifest path is:

```text
~/.cup/config/packages.cfg
```

The repository source copy is:

```text
config/packages.cfg
```

The manifest is a line-based key/value file. Keys use this structure:

```text
<component>.<tool>.<host_platform>.<target_platform>.<field>=<value>
```

Supported fields are:

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

`url_template` supports these placeholders:

```text
{tool}
{host_platform}
{target_platform}
{version}
{format}
```

Manifest validation rejects malformed keys, unknown fields, empty values, duplicate keys, invalid component/tool pairs, invalid platforms, invalid archive formats and unsupported placeholders.

The manifest is required for package resolution. If `~/.cup/config/packages.cfg` is missing, development builds can fall back to `./config/packages.cfg` from the current repository checkout.

## 5. Network and certificate handling

Package downloads are implemented in `src/fetch.c` with libcurl. The downloader writes to a process-specific `.part` file, synchronizes and minimally validates it, then moves it into the deterministic cache path. It follows HTTPS release asset redirects, reports HTTP/network failures and cooperates with interrupt handling so partial downloads are removed after failure or interruption.

The project also contains an embedded CA bundle module:

```text
include/ca_bundle.h
src/ca_bundle.c
```

`ca_bundle.c` is generated from `certs/cacert.pem` by `scripts/certs/update-ca-bundle.sh` and exposes the certificate data as `cup_ca_bundle` and `cup_ca_bundle_len`. When `CUP_USE_EMBEDDED_CA_BUNDLE` is enabled, `fetch.c` passes this in-memory bundle to libcurl instead of depending on a distribution-specific certificate file path.

This is part of the executable design, not part of component packaging. It helps the released `cup` binary remain independent from build-machine paths and from platform-specific CA bundle locations where the selected libcurl backend requires an explicit certificate bundle.

## 6. Local filesystem layout

The default root is:

```text
~/.cup
```

The canonical per-user structure is:

```text
~/.cup/
  bin/
  components/
  tmp/
    transaction.txt   # present only while an operation is pending
  cache/
  config/
  scripts/
  state.txt
  cup.lock
```

The root is always `.cup` in the user home. It is derived from `HOME` or `USERPROFILE`, never from the executable location and never from a configurable `CUP_HOME` environment variable.

Important files:

```text
~/.cup/config/packages.cfg
~/.cup/state.txt
~/.cup/cup.lock
~/.cup/tmp/transaction.txt
~/.cup/scripts/uninstall.sh
~/.cup/scripts/uninstall.ps1
```

The official manifest, uninstall script and package `info.txt` files are read-only guards against accidental changes. `doctor` verifies the expected protection and `repair` restores permissions when the content remains valid.

Installed component packages use this path shape:

```text
~/.cup/components/<component>/<tool>/<host_platform>/<target_platform>/<version>/
```

Cached package archives use this path shape:

```text
~/.cup/cache/<component>/<tool>/<host_platform>/<target_platform>/<version>/
  <tool>-<version>-<host_platform>-<target_platform>.<format>
```

The cache filename is constructed locally instead of being derived from the URL.

Staging and removal paths are created under `~/.cup/tmp/`. Mutable operations use a single journal at `~/.cup/tmp/transaction.txt` and an operating-system lock on `~/.cup/cup.lock`. Read commands acquire a shared lock; mutating commands acquire an exclusive non-blocking lock for the complete operation.

## 7. State file

The state file is:

```text
~/.cup/state.txt
```

It is a line-based key/value file. Installed entries use:

```text
installed.<component>.<host_platform>.<target_platform>=<tool>@<version>
```

Default entries use:

```text
default.<component>.<host_platform>.<target_platform>=<tool>@<version>
```

Example:

```text
installed.compiler.linux-x64.linux-x64=gcc@16.1.0-rev1
default.compiler.linux-x64.linux-x64=gcc@16.1.0-rev1
```

State loading parses and validates each line, identifier and duplicate independently from semantic validation. A second phase checks global relationships such as defaults pointing to installed entries. Normal commands require both phases; `doctor` can report semantic inconsistencies and `repair` can correct them before saving.

State saving validates the final model, synchronizes a temporary file and atomically replaces `state.txt`. Malformed documents replaced by `repair` are preserved as `state.txt.invalid`, `state.txt.invalid.1` and so on.

## 8. Package contract

A component package archive must extract to a package root that contains:

```text
info.txt
```

The package may also contain:

```text
bin/
lib/
libexec/
include/
share/
<target-triple>/
```

The exact contents depend on the tool. `cup` does not assume that every package has the same internal layout; instead, it validates the metadata and installs the package as a self-contained directory.

### 8.1 info.txt

`info.txt` is a line-based key/value file generated by the component build scripts.

Required package identity fields are:

```text
package.component
package.tool
package.version
platform.host
platform.target
```

The file can also contain grouped metadata:

```text
entry.*
features.*
contents.*
config.*
```

Examples:

```text
entry.gcc=bin/gcc
features.c=true
features.cpp=true
contents.self_contained=true
config.languages=c,c++,lto
```

`cup info` prints this metadata for an installed package.

`cup doctor` validates installed package metadata, its correspondence with the canonical path, all declared executable entries and the read-only protection. `cup` never rewrites individual `info.txt` fields; an invalid metadata file requires recovery or replacement of the complete package.

### 8.2 Archive safety

Extraction is performed through libarchive. Every archive must have one common top-level root. Absolute paths, parent traversal, unsafe Windows paths, device/FIFO/socket entries and links that escape the package are rejected. Relative internal symlinks and hardlinks are accepted.

Packages are extracted into a temporary staging directory first and moved into the final installation path only after validation.

## 9. Install flow

The install command performs these steps:

```text
validate request
load state and manifest
detect host platform
resolve target platform
parse <tool>@<release>
resolve stable release
verify version availability
resolve archive format
check that the package is not already installed
create temporary staging directory
write transaction.txt
build package URL
download through a .part file into the deterministic cache path
extract one safe archive root into staging
validate info.txt identity and declared executables
protect info.txt as read-only
move staging into the final install path
add installed entry to state
save state (commit point)
remove transaction.txt
```

If a normal failure occurs before the commit, `cup` rolls the operation back immediately. If the process is terminated in a way that cannot be handled, the journal remains. `doctor` reports it and `repair` uses `state.txt` as the commit point to roll the operation back or finish its cleanup.

## 10. Remove flow

The remove command:

```text
validates the request
resolves the concrete release
checks state and disk consistency
moves the installed package into a temporary removal path
removes the installed entry from state
removes the default entry if it pointed to that package
saves state
removes temporary files
```

If state saving fails, `cup` attempts to move the package back to its original install path. An unhandled termination leaves the journal and temporary package available for deterministic recovery.

## 11. Default and current

`cup default` sets the default installed package for a component, host and target tuple.

```sh
cup default compiler gcc@stable
```

`cup current` prints the current default for a component on the current host and selected target.

```sh
cup current compiler
cup current compiler --target windows-x64
```

A default must point to an installed package. `cup current` reports an inconsistent state if the default points to a missing package.

## 12. List and info

`cup list` prints installed packages for the current host and selected target. It annotates entries that are defaults and entries that match the current manifest stable version.

```sh
cup list
cup list --target windows-x64
```

`cup info` reads the installed package's `info.txt` and prints identity, entry points, feature metadata, content metadata and build configuration metadata.

```sh
cup info compiler clang@stable
```

## 13. Doctor and repair

`cup doctor` is completely read-only. It checks the canonical directory tree, lock and journal, state syntax and semantics, installed manifest and read-only protection, state-to-package and package-to-state correspondence, package metadata and executable entries, the canonical binary and uninstall script, and direct leftovers in `tmp`. If the installed manifest is missing, a repository checkout may be used only as a development fallback and is reported separately.

`cup repair` acquires the exclusive lock and applies deterministic corrections only. It can:

```text
recreate the canonical directory structure
recover or complete an interrupted install/remove transaction
preserve malformed text files as .invalid.N
restore the official manifest, canonical executable and uninstall script
reconstruct installed state entries from fully valid packages
remove stale installed entries and defaults
restore read-only and executable permissions
clean temporary leftovers after transaction recovery
```

A package in `components` is adopted only when its canonical path, identity metadata and declared contents all agree. Ambiguous or unrecognized package paths are left unchanged and reported; `repair` does not choose between conflicting valid alternatives.

## 14. Uninstall

`cup uninstall` removes `cup` itself and all cup-managed data under the `.cup` root.

On POSIX systems, `cup` copies the uninstall script to a temporary path and starts it after the current process exits. On Windows, it uses the PowerShell uninstall script installed under `.cup/scripts`.

The uninstall command intentionally does not remove shell or user PATH entries. A remaining PATH entry is harmless and can be reused by a future installation.

## 15. Main modules

The implementation is split by responsibility:

```text
main.c / options.c
  command dispatch and CLI option parsing

command_context.c
  shared platform, state, manifest and lock context

commands_install.c / commands_remove.c
  install and remove command orchestration

commands_state.c
  list, default, current and info commands

doctor.c / repair.c / uninstall.c
  diagnostics, deterministic recovery and self-removal

layout.c
  canonical ~/.cup paths and directory creation

filesystem.c
  portable tree operations and invalid-file preservation

system_posix.c / system_windows.c
  operating-system file, permission, lock and process primitives

package.c / info.c
  package identity, component scanning and immutable metadata validation

transaction.c
  transaction.txt persistence and identity reconstruction

state.c / manifest.c / text.c
  state model, structured manifest model and common key/value parsing

fetch.c / package_archive.c / extract.c
  HTTPS transfer, cache checks and safe archive extraction

entry.c / path.c / registry.c / platform.c / interrupt.c
  focused domain and platform helpers
```

## 16. Design boundaries

`cup` is a C toolchain installer, not a system package manager.

The current design intentionally keeps these boundaries:

```text
no administrator privileges
no writes to system toolchain directories
no local builds of component tools during install
no dependency solving between independently installed packages
no automatic PATH cleanup during uninstall
no package trust model beyond configured release URLs and archive validation
```

Package completeness is the responsibility of `cup-components`. Local installation correctness, metadata validation and state consistency are the responsibility of `cup`.
