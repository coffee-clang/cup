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
cup --version
cup help [command]
cup list [<component>] [--target <target-platform>]
cup install <component> <tool>@<release> [--target <target-platform>] [--format|-f <archive-format>]
cup update <tool|component>
cup remove <component> <tool>@<release> [--target <target-platform>]
cup default <component> <tool>@<release> [--target <target-platform>]
cup info [<component>] [--target <target-platform>]
cup search [<component>] [--target <target-platform>]
cup inspect <component> <tool>@<release> [--target <target-platform>]
cup self-update
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
checksum_url_template
```

Example:

```text
compiler.gcc.linux-x64.windows-x64.stable_version=16.1.0-rev1
compiler.gcc.linux-x64.windows-x64.available_versions=16.1.0-rev1
compiler.gcc.linux-x64.windows-x64.default_format=tar.gz
compiler.gcc.linux-x64.windows-x64.formats=tar.xz,tar.gz,zip
compiler.gcc.linux-x64.windows-x64.url_template=https://github.com/coffee-clang/cup-components/releases/download/gcc-{version}-{host_platform}-{target_platform}/gcc-{version}-{host_platform}-{target_platform}.{format}
compiler.gcc.linux-x64.windows-x64.checksum_url_template=https://github.com/coffee-clang/cup-components/releases/download/gcc-{version}-{host_platform}-{target_platform}/SHA256SUMS
```

`url_template` and `checksum_url_template` support these placeholders:

```text
{tool}
{host_platform}
{target_platform}
{version}
{format}
```

Manifest validation rejects malformed keys, unknown or missing fields, empty values, duplicate keys, invalid component/tool pairs, invalid platforms, invalid archive formats, non-HTTPS URLs and unsupported placeholders. Every package entry must provide a checksum URL so downloaded and cached archives can be verified before extraction.

The manifest is required for package resolution. If `~/.cup/config/packages.cfg` is missing, development builds can fall back to `./config/packages.cfg` from the current repository checkout.

## 5. Network and certificate handling

Package and release downloads are implemented in `src/fetch.c` with libcurl. The downloader requires HTTPS, allows only HTTPS redirects, applies separate connection and total timeouts, enforces size limits for metadata, bootstrap assets and component archives, writes to an exclusive temporary file and removes partial data after failure or interruption. Component archives, including existing cache entries, are verified against the release's `SHA256SUMS` before extraction; a mismatching cache entry is discarded rather than trusted.

The project embeds a CA bundle generated from the versioned source file:

```text
certs/cacert.pem
```

During each configured build, `scripts/certs/generate-ca-bundle.sh` deterministically writes `ca_bundle.h` and `ca_bundle.c` under `build/<platform>/<link-mode>/generated`. These generated files are not tracked. The versioned PEM is updated explicitly with `make update-ca-bundle`, validated before replacement and committed before creating a release tag. Source tests and release builds therefore use the exact PEM stored in the selected commit and do not mutate that commit during CI. When `CUP_USE_EMBEDDED_CA_BUNDLE` is enabled, `fetch.c` passes the generated in-memory bundle to libcurl instead of depending on a distribution-specific certificate file path.

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
  recovery/          # created only when an invalid package is quarantined
  config/
    packages.cfg
    SHA256SUMS.common
    SHA256SUMS.<host_platform>
  scripts/
  state.txt
  cup.lock
  uninstall.pending  # present only while uninstall is active or incomplete
```

The root is always `.cup` in the user home. It is derived from `HOME` or `USERPROFILE`, never from the executable location and never from a configurable `CUP_HOME` environment variable.

Important files:

```text
~/.cup/config/packages.cfg
~/.cup/config/SHA256SUMS.common
~/.cup/config/SHA256SUMS.<host_platform>
~/.cup/state.txt
~/.cup/cup.lock
~/.cup/tmp/transaction.txt
~/.cup/scripts/uninstall.sh
~/.cup/scripts/uninstall.ps1
~/.cup/uninstall.pending
```

The official manifest, bootstrap checksum files, uninstall script and package `info.txt` files are read-only guards against accidental changes. The installed manifest, executable and uninstall script are verified against the published bootstrap checksum files. `doctor` verifies integrity and protection; `repair` downloads and validates replacement bootstrap assets before committing them.

The bootstrap installer initially creates only `bin`, `config` and `scripts`. The first operational command or `repair` creates the full runtime structure. During bootstrap replacement, installers keep a persistent `.bootstrap` staging directory with per-asset backup and commit markers so a later installer run can recover an interrupted update.

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

Staging and removal paths are created under `~/.cup/tmp/`. Their generated names include the operation and complete package identity; a journal is accepted only when its recorded temporary name matches that identity. Mutable operations use a single journal at `~/.cup/tmp/transaction.txt` and an operating-system lock on `~/.cup/cup.lock`. Read commands acquire a shared lock; mutating commands acquire an exclusive non-blocking lock for the complete operation.

`~/.cup/recovery/` is not part of the mandatory runtime structure. `repair` creates it lazily when a path has a complete canonical package identity but invalid type or contents. The original object is moved into a unique recovery directory instead of being deleted.

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

State loading parses and validates each line, identifier and duplicate independently from semantic validation. A second phase defensively checks entry identities, duplicate installed records, duplicate default scopes and relationships such as defaults pointing to installed entries. Normal commands require both phases; `doctor` can report semantic inconsistencies and `repair` preserves an invalid state file before reconstructing it when no pending transaction makes recovery ambiguous.

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

`cup inspect <component> <tool>@<release>` prints this metadata for an installed package.

`cup doctor` validates installed package metadata, its correspondence with the canonical path, all declared executable entries and the read-only protection. `cup` never rewrites individual `info.txt` fields; an invalid metadata file requires recovery or replacement of the complete package.

### 8.2 Archive safety

Extraction is performed through libarchive. Every archive must have one common top-level root. Absolute paths, parent traversal, unsafe Windows paths, duplicate paths, device/FIFO/socket entries and links that escape the package are rejected. Relative internal symlinks are accepted, while hardlinks must reference a regular file that has already been extracted. File permissions are normalized, filesystem flags and privileged mode bits are discarded, and limits are enforced for path depth, entry count and total extracted size.

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
reuse a fully validated cached archive or download and atomically cache a replacement
extract one bounded and safe archive root into staging
validate info.txt identity and declared executables
retry once only when a cached archive extracts to an unsafe or inconsistent package
protect info.txt as read-only
move staging into the final install path
add installed entry to state
create the first default for the scope when none exists
save state (commit point)
remove transaction.txt
rebuild managed entry points when a default was created
```

If a normal failure occurs before the commit, `cup` rolls the operation back immediately and clears the journal only after every rollback step succeeds. If the process is terminated or a commit result is uncertain, the journal remains. `doctor` reports it and the transaction module uses `state.txt` as the commit point to complete or roll back the interrupted operation deterministically.

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

If state saving fails before the state commit, `cup` attempts to move the package back to its original install path. Rollback errors preserve the journal and temporary package for deterministic recovery instead of hiding the incomplete operation. Removal remains available for a package recorded in state even when its canonical on-disk object is corrupted.

## 11. Default and info

`cup default` is a mutating command. It selects one installed package as the default for a `component + host + target` scope and rebuilds the managed entry points derived from the resulting state. The first installation in a scope still creates its default automatically; later installations do not replace an existing choice.

```sh
cup default compiler gcc@stable
cup default compiler gcc@stable --target windows-x64
```

`cup info` is the read-only view of those defaults. Without filters it includes every target scope configured for the current host. A component or `--target` restricts the view. Each entry is validated against the installed package and the managed commands currently exposed through `~/.cup/bin`.

```sh
cup info
cup info compiler
cup info --target windows-x64
cup info compiler --target windows-x64
```

A default must point to an installed package whose canonical directory and metadata pass complete validation. Managed wrappers are planned once from the candidate state, validated before the state commit and then applied from that same immutable plan. Native entries keep their declared names; cross-target entries are prefixed with `<target>-`. `doctor` diagnoses missing, altered and stale wrappers, while `repair` rebuilds them from `state.txt`.

## 12. List, search and inspect

`cup list` prints packages installed for the current host and selected target. It can be restricted to one component and annotates defaults, manifest-stable releases, missing packages and invalid packages.

```sh
cup list
cup list compiler
cup list --target windows-x64
```

`cup search` reads the manifest catalog. With no positional arguments it groups all installable tools for the current host by component; with a component it restricts the catalog. `--target` can restrict either form.

```sh
cup search
cup search compiler
cup search compiler --target windows-x64
```

`cup inspect <component> <tool>@<release>` resolves the requested release, requires the package to be installed and valid, then prints the immutable `info.txt` metadata supplied by `cup-components`.

```sh
cup inspect compiler clang@stable
cup inspect compiler gcc@16.1.0-rev1 --target windows-x64
```

## 12.1 Update and self-update

`cup update <tool>` identifies every installed host/target scope of that tool. `cup update <component>` does the same for every installed tool in the component. Each scope is then re-read and updated while holding one exclusive lock: the active manifest `stable` version is installed idempotently when absent, previous versions are retained, and the default moves only when it still belongs to the same tool. This avoids applying stale conclusions from the initial scan. A component-wide update is atomic per scope, not across every tool in the component.

`cup self-update` is available only in official release builds. It first reads the small `release.txt` asset through GitHub's `latest` alias to discover an official `X.Y.Z` version. Bootstrap installers are different: each generated installer already embeds its own `vX.Y.Z` release URL, so a script downloaded from an older tag installs that older tag. It rejects malformed metadata and development builds, treats an equal version as already current, and refuses downgrades. All checksums and replacement assets are then fetched from the immutable `vX.Y.Z` release URL, so an incomplete or changing `latest` alias cannot mix generations. The downloaded metadata, executable and uninstall script must all match the published platform checksum file. The command persists a `self-update` transaction in the existing journal and starts a deferred helper. After the current process exits, the helper reacquires the operating-system lock, backs up all canonical assets, replaces the executable, uninstall script and platform checksum as one recoverable operation, records the commit, clears the journal and removes staging. `repair` deterministically rolls back or completes an interrupted self-update from the same journal.

## 13. Doctor and repair

`cup doctor` is completely read-only. It checks the canonical directory tree, lock and journal, state syntax and semantics, bootstrap checksum files, manifest and executable integrity, read-only protection, state-to-package and package-to-state correspondence, package metadata and executable entries, the uninstall script, uninstall marker and direct leftovers in `tmp`. A failed scan or inspection is reported as an incomplete check and causes a failing result. If installed bootstrap files are unavailable, a repository checkout may be used only as a development fallback and is reported separately.

`cup repair` acquires the exclusive lock and applies deterministic corrections only. It can:

```text
recreate the canonical directory structure
recover or complete an interrupted install, remove or self-update transaction
preserve malformed text files as .invalid.N
restore published bootstrap checksum files
restore checksum-verified manifest, canonical executable and uninstall script
reconstruct installed state entries from fully valid packages
remove stale installed entries and defaults
quarantine canonically identifiable packages with invalid type or contents
restore read-only and executable permissions
clean temporary leftovers after transaction recovery
```

A package in `components` is adopted only when its canonical path, identity metadata and declared contents all agree. A path at the complete version level with a valid package identity but invalid type or contents is moved intact under `~/.cup/recovery/`; ambiguous or unrecognized paths are left unchanged and reported. A truncated scan or a valid-package count that cannot be represented by `state.txt` stops reconciliation before packages or state are modified.

## 14. Uninstall

`cup uninstall` removes `cup` itself and all cup-managed data under the `.cup` root.

Before starting the helper, `cup` creates `~/.cup/uninstall.pending` while holding the exclusive lock and passes its process ID to the helper. The helper verifies that the requested root is exactly the canonical root, waits for that process to exit, atomically moves the root to a unique sibling staging directory, and removes the detached tree. Other commands reject the pending marker, while the atomic detach prevents them from observing a partially deleted canonical root. On POSIX the script is copied to a temporary path; on Windows the installed PowerShell script is copied and invoked with the same parent-process protocol.

The uninstall command intentionally does not remove shell or user PATH entries. A remaining PATH entry is harmless and can be reused by a future installation. A later installer run removes a stale marker only after a valid bootstrap has been committed.

## 15. Main modules

The implementation is split by responsibility:

```text
main.c
  command dispatch and Argtable3 CLI parsing

command_context.c
  shared platform, state, manifest and lock context

commands_install.c / commands_remove.c / commands_update.c
  install, remove and stable update orchestration

commands_state.c / entrypoints.c
  search, list, default, info, inspect and managed command wrappers

self_update.c
  checksum-verified replacement of canonical platform bootstrap assets

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
  transaction.txt persistence, temporary-path binding and deterministic recovery

state.c / manifest.c / text.c
  state model, structured manifest model and common key/value parsing

fetch.c / package_archive.c / extract.c
  HTTPS transfer, cache checks and safe archive extraction

entry.c / path.c / registry.c / platform.c / interrupt.c
  focused domain and platform helpers
```

## 16. Build version model

The root `VERSION` file is the single manually maintained official version. The build generates `version.h`, `release.txt` and the Windows version resource from that value. A build is official only when release mode is requested explicitly, the working tree is clean, and `HEAD` is exactly tagged `vX.Y.Z` with the same `X.Y.Z` stored in `VERSION`. Other Git builds use `X.Y.Z-dev.N+commit` with an optional `.dirty` suffix; a source archive without Git metadata always uses `X.Y.Z-dev+archive`.

The release cycle is deliberate and driven only by `VERSION`: update `VERSION`, finish the development commits, push them, then start `build-release.yml` manually. The build workflow reads `VERSION`, refuses to build if tag `vX.Y.Z` already exists, creates the static release candidate assets and stores them as Actions artifacts with `candidate.env`. `test-release.yml` then tests those exact artifacts, either automatically after the build workflow or manually from a chosen build run id, and only after successful native tests creates tag `vX.Y.Z` and publishes the complete release. There is no commit-count or automatic patch increment. `BUILD_MODE=release` controls optimization only; it does not by itself turn an arbitrary commit into an official release.

## 17. Design boundaries

`cup` is a C toolchain installer, not a system package manager.

The current design intentionally keeps these boundaries:

```text
no administrator privileges
no writes to system toolchain directories
no local builds of component tools during install
no dependency solving between independently installed packages
no automatic PATH cleanup during uninstall
no signature infrastructure beyond HTTPS and published SHA-256 release checksums
```

Package completeness is the responsibility of `cup-components`. Local installation correctness, metadata validation and state consistency are the responsibility of `cup`.
