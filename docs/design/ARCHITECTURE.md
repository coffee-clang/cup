# Architecture

This document defines the responsibilities and design boundaries of `cup`. The
public CLI is documented in [COMMANDS](../user/COMMANDS.md); persistent formats and
recovery have dedicated documents.

## Scope

The `cup` repository provides:

```text
command-line interface
component/tool registry
platform detection and validation
catalog loading and URL resolution
HTTPS transfer and archive extraction
package validation and local cache
installation state and default selection
managed wrappers
transaction journals and recovery
doctor, repair, CUP update and uninstall
CUP asset installers and release workflows
```

It does not build component tools during installation. Complete tool packages
are produced by `cup-components` and consumed through the contract in
[PACKAGES](PACKAGES.md).

## Domain model

### Component

A component is a stable category used in commands, registry entries, catalog
keys, state scopes and paths:

```text
compiler
debugger
linker
formatter
linter
language-server
analyzer
```

### Tool

A tool is one implementation of a component. The current registry accepts:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
linker/lld
linker/ld
formatter/clang-format
linter/clang-tidy
language-server/clangd
analyzer/valgrind
```

The registry and catalog serve different purposes:

```text
registry
  defines supported component/tool relationships in the executable

catalog
  defines available host/target/version/format package tuples
```

A package must satisfy both. This prevents an arbitrary catalog from creating
new semantic categories that the executable does not understand.

### Platform

Every concrete package has a host and a target:

```text
host    where the package executable runs
target  what that package targets
```

The target defaults to the host. Cross-target packages remain host-native
executables and are distinguished in state, paths and entry-point names. See
[PLATFORMS](PLATFORMS.md).

### Release

`stable` is a symbolic catalog pointer. State and package paths use concrete
versions. This keeps an installed identity immutable even when the catalog's
stable pointer later advances.

Older versions are retained after update. Installation and default selection
are separate decisions, which allows rollback or comparison without another
network operation.

## Userspace model

All managed data stays under the canonical user root. `cup` does not require:

```text
administrator privileges
sudo
system package managers
writes to /usr, /opt or Program Files
system-wide toolchain directories
a privileged service
```

This choice makes ownership and recovery local to one user and allows the same
state model on all supported systems.

## PackageCatalog-driven installation

The executable does not contain hard-coded release URLs for component packages.
`packages.cfg` maps one component/tool/host/target tuple to:

```text
stable version
available versions
default archive format
supported formats
archive URL template
checksum URL template
```

The catalog is data, not executable policy. Component/tool validity, path
safety, HTTPS requirements, package identity and transaction behavior remain in
C. See [PACKAGES](PACKAGES.md).

## Installation policy and preferences

Package availability and abbreviated install selection are deliberately
separate:

```text
registry.c
  compiled authority for components, tools and component/tool relationships

packages.cfg
  concrete packages available for host, target, version and format

install.cfg
  checksum-verified scoped defaults, profiles and explicit toolchains

preferences.txt
  mutable scoped preferred tools for future installs
```

For `cup install <component>`, an explicit selector wins first, followed by the
user preference for the exact `component + host + target` scope and then the
official default for that scope. Absence of all three is an error. Profiles apply
that hierarchy to each component. Toolchains are explicit lists and never consult
preferences. A complete group is prevalidated against the registry and catalog
before the first package is installed.

`state.txt` defaults remain independent: they select one already installed
release for execution and never affect which tool an abbreviated install picks.

## Package self-containment

`cup` installs full package directories instead of attempting to merge files
from unrelated packages into one global sysroot. The package can contain its
own runtime libraries, headers, target triples and support directories.

Benefits include:

- removing one package cannot delete files owned by another;
- multiple versions can coexist;
- state maps directly to canonical package directories;
- archive validation can reason about one root;
- defaults can change without moving package contents.

The exact contents remain the responsibility of `cup-components`.

## State and derived data

`state.txt` records installed identities and defaults. Managed wrappers under
`.cup/bin` are derived from defaults; they are not an independent source of
truth. `repair` can therefore rebuild them from valid state and packages.

The canonical filesystem, state rules and entry-point naming are specified in
[STATE](STATE.md).

## PackageTransaction model

Install, remove and CUP update can be interrupted after filesystem changes but
before the caller receives a final result. A persistent journal records the
operation and temporary path before the first irreversible-looking step.

The state replacement is the commit point for package installation and removal.
CUP update has an explicit committed marker because its detached helper
replaces CUP assets after the original process exits.

The complete model is in [TRANSACTIONS](TRANSACTIONS.md).

## Portable and platform-specific layers

Portable C owns domain and state-machine decisions. `system.h` defines the
platform contract for:

```text
home and process information
file and directory operations
exclusive temporary objects
path inspection and permissions
directory traversal
locks
detached process helpers
commit-state reporting
```

`system_posix.c` and `system_windows.c` implement that contract natively. Higher
modules do not branch on shell behavior or call external `tar`, `unzip` or
PowerShell for ordinary runtime operations.

Platform differences are documented in [PLATFORMS](PLATFORMS.md).

## C and script boundary

A rule belongs in C when it is part of runtime semantics or must remain
identical on all platforms. C owns:

- command parsing and domain validation;
- catalog, state, journal and metadata parsing;
- package, path, archive and checksum policy;
- install, remove, update, configuration and repair decisions;
- commit, rollback and recovery decisions;
- selection of canonical paths and assets.

Scripts own operations outside the in-process runtime state machine:

- building pinned third-party dependencies;
- maintaining generated CA sources;
- initial CUP asset installation;
- detached deletion or replacement after `cup` exits;
- CI runner preparation;
- release candidate assembly and publication.

A script may verify data at its own trust boundary, but it must not become a
second implementation of state, catalog or transaction semantics. This boundary
is why detached replacement remains scripted while validation, path selection and
recovery decisions remain in C.

## Module map

### Command layer

```text
main.c
  command dispatch, Argtable3 parsing and help

command_context.c
  platform, root, lock, state and catalog lifetime

command_install.c / package_install.c
  CLI planning and reusable one-scope installation operation

command_config.c / install_policy.c / tool_preferences.c
  preference commands, official policy and mutable user choices

command_remove.c / command_update.c
  package removal and stable-update planning

command_search.c / command_list.c / command_default.c
command_info.c / command_inspect.c
  focused query and default handlers

cup_update.c
  official release discovery, verification and staging

command_doctor.c / command_repair.c / command_uninstall.c
  diagnosis, deterministic reconciliation and self-removal
```

### Persistent model

```text
layout.c / filesystem.c
  canonical paths and portable tree operations

state.c
  installed and active package identities

package_transaction.c / cup_update_journal.c / runtime_journal.c
  separate package and CUP-update journals over the shared physical boundary

wrappers.c
  deterministic wrapper plans derived from active package state
```

### Package and transfer layer

```text
package_catalog.c / registry.c / package_selector.c / package_request.c
  package availability, selectors and resolved requests

install_policy.c / tool_preferences.c
  verified official policy and mutable user preference overlay

package.c / installed_package.c / package_metadata.c
  package identity, installed-package preconditions and metadata

download.c / package_cache.c / checksum.c / sha256.c
  HTTPS transfer, cache ownership, verification and SHA-256

package_archive.c / package_extract.c
  archive preflight and safe extraction
```

### Foundation

```text
system_posix.c / system_windows.c
  policy-neutral native operating-system primitives

cup_update_helper.c
  platform-specific detached replacement of CUP assets

path.c / text.c / platform.c / interrupt.c
  focused parsing, path, platform and interrupt helpers

cup_assets.c
  installed CUP asset inspection and checksum verification
```

## Module granularity

Files are divided by stable contracts rather than by a target line count. A
separate module is useful when it owns a lifecycle, persistence rule, platform
boundary or independently reusable API. Splitting private helpers merely to
shorten a file would instead expose state and increase coupling.

The larger modules are intentionally cohesive:

- `system_posix.c` and `system_windows.c` each implement the complete native
  side of `system.h`;
- `package_install.c` owns the reusable transactional installation operation;
- `command_repair.c` owns one ordered reconciliation process and its private context;
- the small `command_search.c`, `command_list.c`, `command_default.c`,
  `command_info.c` and `command_inspect.c` handlers each own one public query.

Conversely, `package_selector.c`, `platform.c` and `interrupt.c` remain small because each
has a narrow contract used across otherwise unrelated flows. Combining them
would create a miscellaneous utility layer rather than simplify the design.

## Error model

Modules return `CupError`; they do not terminate the process directly except for
platform child helpers that must finish independently. Error families preserve
meaning across layers so callers can distinguish input, availability, state,
filesystem, transfer, validation, commit and rollback failures.

`SystemCommitState` adds information that a normal error code cannot express:

```text
NOT_APPLIED  destination is known not to contain the replacement
APPLIED      replacement may be visible but durability is uncertain
DURABLE      replacement and required parent metadata were synchronized
```

Callers use this state to decide whether rollback is valid. See
[TRANSACTIONS](TRANSACTIONS.md#commit-state).

## Design boundaries

The current design intentionally excludes:

```text
administrator or service-based installation
local component builds during cup install
cross-package dependency solving
one merged global sysroot
a configurable cup root
automatic PATH cleanup
nightly channels
automatic VERSION increments
signature infrastructure beyond HTTPS and published SHA-256 files
```

These are not accidental omissions. They keep the installation model small,
inspectable and recoverable. A future extension should be added only when its
ownership and persistent contract are clear.

## Related documents

- [PACKAGES](PACKAGES.md) — package and catalog contracts;
- [STATE](STATE.md) — local state and derived wrappers;
- [TRANSACTIONS](TRANSACTIONS.md) — recovery model;
- [BUILD](../development/BUILD.md) — dependencies and generated build inputs.
