# Architecture

This page is the map of CUP's implementation. It explains the boundaries between
CLI planning, package handling, state, recovery and native platform code so a
reader can navigate the repository without reconstructing the design from
`main.c`.

User-facing terminology is introduced in [Concepts](../user/CONCEPTS.md).
Persistent formats are documented in [Packages](PACKAGES.md), [State](STATE.md)
and [Transactions](TRANSACTIONS.md).

## Product boundary

CUP manages **prebuilt** C development tools inside one user-managed root. This
repository owns:

```text
CLI and command planning
component/tool/platform domain
catalog and install policy
package download, cache, archive admission and extraction
package metadata/integrity validation
installed state, preferences, defaults and launchers
transactions, recovery, self-update and uninstall
native filesystem/process abstraction
CUP build, tests and release publication
```

Tool packages are produced separately by `cup-components`. CUP intentionally
does not carry the build recipes for GCC, LLVM, GDB, Valgrind or other managed
tools. The repositories meet at the package contract: catalog coordinates,
checksums, archive layout, `info.txt` and `manifest.txt`.

CUP also does not provide a daemon, privileged service, global sysroot or
system-wide package database.

## Runtime model

The important scopes are:

```text
package  = component + tool + host + target + concrete version
default  = component + host + target -> installed package
preference = component + host + target -> preferred tool for future installs
```

`stable` is resolved by the catalog before a package becomes persistent state.
A package may run on one host and target another platform; host and target are
therefore kept separate in paths, state and defaults.

The closed component/tool/platform relationships live in
`include/domain_registry.h`. `packages.cfg` can restrict availability but cannot
extend that compiled domain.

## Command flow

A normal single-package install is representative of the architecture:

```text
argv
  -> parse and normalize public command
  -> select host/target and CUP root
  -> shared preflight state/catalog snapshot
  -> resolve concrete package artifact
  -> exclusive package transaction
  -> verified cache/download stream
  -> archive admission + extraction into private staging
  -> metadata + executable-entry validation
  -> manifest integrity validation
  -> transaction journal
  -> canonical package publication
  -> state.txt commit
  -> launcher reconciliation + cleanup
```

The flow is intentionally staged. Catalog resolution produces a concrete package
specification before mutation. Download verification returns an open
`VerifiedArtifact`; extraction consumes that same stream rather than reopening a
pathname. Package integrity is proved before canonical installation, and
`state.txt` is the package transaction commit point.

Group installs add one layer above this flow: profile/toolchain planning resolves
the complete list first, then installs each package sequentially. Completed
packages remain committed if a later member fails.

## Main layers

### CLI and commands

`main.c` owns public syntax, help/version and dispatch. Argtable3 parses a command
once into normalized arguments before a command opens runtime state.

Each `command_*.c` file owns the policy for one public operation. Shared package
installation stays in `package_install.c`; command modules do not duplicate the
package transaction to implement single installs, groups or updates.

### Command context

`command_context.c` groups the state that must be coherent for one command:

```text
selected root and host/target scope
shared or exclusive runtime lock
state snapshot and state-file identity
package catalog when requested
```

Read-only commands request only what they need. Mutating commands acquire final
exclusive ownership before runtime initialization or persistent changes.

### Package selection and artifacts

The package path from public selector to installed tree is split by responsibility:

```text
registry / install policy / preferences
            ↓
package catalog + selector
            ↓
PackageArtifactSpec
            ↓
package cache / download
            ↓
VerifiedArtifact (open authenticated stream)
            ↓
archive extraction + package validation
```

`PackageArtifactSpec` contains the resolved identity, archive format and remote
coordinates. Cache layout, retry policy and download limits remain with their
own modules instead of becoming generic fields on that value.

### State and launchers

`state.txt` stores installed package identities and defaults. `preferences.txt`
is separate because preferences influence future selection without changing the
meaning of already installed state.

Launchers in `bin/` are derived from defaults. Command code builds the desired
launcher plan from validated packages; `wrappers.c` owns comparison and
publication. `doctor` and `repair` therefore reason from the same derivation used
by normal mutations.

### Transactions and recovery

All root mutations share one runtime lock and one physical `transaction.txt`
path. `runtime_journal.c` owns safe publication/reading/deletion of that file,
while package, self-update and uninstall modules own their schemas and recovery
rules.

The physical file lifecycle is common; operation semantics are not. See
[Transactions](TRANSACTIONS.md) for commit points and helper handoff.

### Platform boundary

Portable code calls `system.h`. The native backends implement operations whose
correctness depends on operating-system semantics:

```text
system_posix.c    descriptor-relative paths, flock, fsync, processes
system_windows.c  UTF-16 paths, handles, reparse checks, LockFileEx/processes
```

`filesystem.c` composes those primitives into project-level operations such as
bounded snapshots, recursive cleanup and atomic text publication. Higher layers
should not grow parallel POSIX/Windows implementations of the same filesystem
contract.

## Native helper model

Self-update and uninstall must continue after the initiating CUP process exits.
Both use a copied native CUP executable rather than a shell implementation of the
transaction.

The parent establishes continuous exclusive handoff before releasing its normal
lock. POSIX can carry lock ownership through inherited file descriptions;
Windows uses a separate named authority because `LockFileEx` ownership cannot be
transferred between processes. The operation-specific helper still validates the
same root and transaction before mutation.

Windows uninstall also needs a small lifetime carrier for deletion of the
temporary helper executable. That carrier does not implement uninstall: it owns
only the cleanup handle and helper-process lifetime. Root detach, journal handling
and recursive cleanup remain native C in `uninstall_helper.c`/`system_windows.c`.

## Repository scripts

Runtime mutations belong to the C program. Repository automation is split by
workflow responsibility:

| Directory | Responsibility |
|---|---|
| `scripts/build/` | build metadata, binary inspection and finalization |
| `scripts/dependencies/` | pinned dependency prefix build/verification |
| `scripts/certs/` | embedded CA bundle generation/checks |
| `scripts/ci/` | CI preparation and source-build identity verification |
| `scripts/install/` | public transport installers |
| `scripts/release/` | release assembly and publication |
| `scripts/lib/` | small shared shell helpers |

Repository path-safety scripts protect managed build/dependency/release
workspaces from accidental destructive misuse. They are not a second
implementation of the runtime's descriptor/handle identity model.

The `www/` tree and Pages workflow are a separate website surface; they do not
authorize CUP source builds or releases.

## Module map

### Entry point and public commands

| Module | Responsibility |
|---|---|
| `main.c` | CLI parsing, help/version, internal-mode dispatch |
| `command_context.c` | root/lock/state/catalog lifetime for one command |
| `command_search.c` | catalog search |
| `command_list.c` | installed package listing |
| `command_info.c` | defaults and provided commands |
| `command_inspect.c` | installed package metadata |
| `command_config.c` | preference view/set/reset |
| `command_install.c` | install/profile/toolchain planning |
| `command_remove.c` | package removal |
| `command_default.c` | default selection |
| `command_update.c` | tool/component update planning |
| `command_doctor.c` | read-only diagnosis |
| `command_repair.c` | ordered recovery/reconciliation |
| `command_uninstall.c` | uninstall planning and helper launch |
| `bootstrap.c` | hidden verified first-install entry point |

### Packages

| Module | Responsibility |
|---|---|
| `registry.c` | compiled component/tool domain |
| `install_policy.c` | official defaults, profiles, toolchains |
| `tool_preferences.c` | user preference overlay |
| `package_catalog.c` | catalog parser and lookups |
| `package_selector.c` / `package_request.c` | public selector -> concrete request |
| `package_artifact.c` | immutable artifact coordinates and verified stream ownership |
| `package_cache.c` | checksum-verified cache/download lifecycle |
| `package_archive*.c` / `package_extract.c` | format admission and safe extraction |
| `package_metadata.c` | `info.txt` parser |
| `package_manifest.c` | `manifest.txt` integrity verification |
| `package.c` / `installed_package.c` | semantic package validation/scanning |
| `package_install.c` | reusable install transaction |
| `package_transaction.c` | package journal schema and recovery |

### CUP assets and lifecycle

| Module | Responsibility |
|---|---|
| `assets.c` | authenticate installed CUP asset generation |
| `update_assets.c` | generation asset names and paths |
| `self_update.c` | discover/download/stage newer CUP release |
| `update_helper.c` / `update_journal.c` | detached CUP update and recovery |
| `uninstall_helper.c` / `uninstall_journal.c` | root detach/cleanup and recovery |
| `runtime_journal.c` | physical `transaction.txt` operations |
| `release_metadata.c` | `release.txt` parsing |

### State, filesystem and platform support

| Module | Responsibility |
|---|---|
| `layout.c` | root selection and managed paths |
| `state.c` | installed/default state persistence |
| `wrappers.c` | derived launcher plan and reconciliation |
| `filesystem.c` | composite managed-tree operations |
| `path.c` / `text.c` | bounded identifier/path/text parsing |
| `system.c` | platform-neutral system helpers |
| `system_posix.c` / `system_windows.c` | native filesystem/process contracts |
| `platform.c` | host detection and platform validation |
| `interrupt.c` | interrupt observation |
| `exit_status.c` | internal error -> public status mapping |

`include/windows_utf.h` is a private Windows boundary helper for UTF-8/UTF-16
filesystem conversion.

## Error and commit model

Most implementation functions return `CupError`; the CLI maps it to the stable
public status set only at the process boundary.

Filesystem publication also uses `SystemCommitState`:

```text
NOT_APPLIED  destination was not changed
APPLIED      replacement may be visible but durability is uncertain
DURABLE      replacement and required persistence were confirmed
```

This distinction matters to recovery. An error after rename cannot be treated as
if the old destination were certainly still authoritative.

## Design rules visible in the code

The repository generally keeps a separate module when it owns a public command,
persistent format, resource lifecycle, native implementation or reusable
operation with distinct tests. File length alone is not a reason to introduce an
abstraction.

Comments and helpers should preserve the same boundary: share mechanics that are
truly common, but keep operation-specific policy with the module that owns the
invariant. The goal is not minimum line count; it is one clear owner for each
contract.

## Intentional limits

The runtime does not include privilege elevation, local tool compilation,
component dependency solving, a global shared sysroot, a persistent root
environment override, system-wide PATH mutation, automatic VERSION increments or
nightly package selectors.

These are product boundaries rather than partially implemented features.

## Related documents

- [Concepts](../user/CONCEPTS.md)
- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Platforms](PLATFORMS.md)
- [Security](SECURITY.md)
- [Build](../development/BUILD.md)
