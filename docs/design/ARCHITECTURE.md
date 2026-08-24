# Architecture

This page gives the overall structure of cup. The goal is to show where each
responsibility lives and how the main parts work together, without requiring a
reader to start from `main.c` and follow every call.

More detailed formats are described in [Packages](PACKAGES.md),
[State](STATE.md) and [Transactions](TRANSACTIONS.md).

## Project scope

cup manages prebuilt C development tools. This repository contains:

```text
public CLI and help
component/tool registry
platform detection
package catalog and selectors
HTTPS downloads and checksums
archive validation and extraction
installed package validation
state, preferences and wrappers
transactions and recovery
cup executable update and uninstall
build, test and release scripts
```

cup does not compile GCC, LLVM or the other component packages during
installation. Those archives are built by `cup-components` and consumed through
the package contract documented in [Packages](PACKAGES.md).

The project also does not provide a daemon, privileged service, global sysroot
or system-wide package database.

## Main flow

A normal package install follows this path:

```text
CLI arguments
  -> validated command plan
  -> selected user root and lock
  -> package catalog lookup
  -> concrete package request
  -> download/cache verification
  -> single-pass staged archive validation/extraction
  -> package metadata and executable-entry validation
  -> transaction journal
  -> state commit
  -> wrapper rebuild and cleanup
```

The important point is that each step passes a typed result to the next one.
Later steps do not repeat catalog resolution or reopen an archive by pathname
after it has already been verified.

## Domain model

### Components and tools

A component is a category used by the CLI, state and package layout:

```text
compiler
debugger
linker
formatter
linter
language-server
analyzer
```

A tool is one implementation of a component. The current built-in registry
contains:

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

The registry and catalog have different jobs:

- the registry says which component/tool relationships the executable supports;
- the catalog says which host, target, version and archive combinations are
  available for download.

A catalog entry must match the built-in registry. A downloaded catalog therefore
cannot invent a new component or attach a known tool to the wrong component.

`domain_registry.h` contains the closed component, tool and platform lists used
to derive domain tables and the default-scope capacity. Installed-package
capacity is an independent resource budget because multiple concrete versions
may coexist. `ScopeKey` represents one
component/host/target scope, while `ConcreteRelease` represents a version after
`stable` has been resolved.

### Host and target

Every package has two platform values:

```text
host    platform where the package executables run
target  platform produced or inspected by those tools
```

The target defaults to the host. A cross-target package is still a host-native
package, but it receives its own state scope, path and wrapper names.

### Releases

`stable` is only a catalog selector. Installed paths and `state.txt` always use
concrete versions. If the catalog later changes its stable version, an existing
installation keeps the identity it had when it was installed.

Updating installs the newer package without deleting older versions. The user
can therefore keep more than one version and select the default separately.

## Userspace root

All managed data stays below one root selected from the current user's home:

```text
.cup
.coffee-cup   fallback when .cup is foreign
```

The root is identified by `root.txt`. cup does not use `sudo`, administrator
rights, `/usr`, `/opt`, `Program Files` or an environment-configurable root.

This choice keeps state and recovery local to one user and makes uninstall
possible without a privileged service.

## Main layers

### CLI and command planning

`main.c` uses Argtable3 to parse the complete command once. Parsing produces a
bounded command plan containing the normalized arguments needed for execution.
Only after this succeeds does cup select a root or prepare a mutating command.

Command files contain the policy for one public operation. Reusable package
installation work lives in `package_install.c` instead of being repeated by the
single-package, profile, toolchain and update commands.

### Command context

`command_context.c` owns the common lifetime of:

```text
host and target platform
runtime lock
state and state-file identity
package catalog
```

Read-only commands request only the parts they need. Mutating commands acquire
the lock and check the transaction state before changing files.

### Package request and artifact

Catalog lookup produces a `PackageArtifactSpec`. It contains only the concrete
package identity, selected archive format, package URL and checksum URL. Cache
names, download limits and catalog-snapshot metadata stay with the modules that
own them instead of being copied into the artifact specification.

The cache returns a `VerifiedArtifact`. This object owns the regular file that was
size-checked and hashed. Extraction consumes that same open stream, so the code
does not verify one pathname and later open a different file with the same name.
Archive format, structural and resource admission are performed during extraction.

### State and transactions

`state.txt` records installed packages and defaults. Preferences are stored
separately because they affect future installs, not the identity of packages
already installed.

All persistent mutations share the runtime lock and the single
`transaction.txt` path. `runtime_journal.c` owns the physical read, publication
and identity checks for that file. The package, update and uninstall journal
modules keep their own schemas and recovery rules.

This is deliberate: the file lifecycle is shared, but the meaning of each
operation is not.

### Platform layer

Portable modules call `system.h`. `system_posix.c` and `system_windows.c`
implement the native parts such as no-follow path operations, locks, process
creation, atomic replacement and directory synchronization.

`filesystem.c` combines those primitives into project-level operations such as
bounded snapshots and atomic text-file publication.

The command and package layers should not contain separate POSIX/Windows
implementations of the same filesystem rule.

## C and script responsibilities

The C program owns package/state mutations and the authenticated handoff for
root-level install, update and uninstall work. Public installers only download
and verify a release generation before calling the hidden C bootstrap entry
point. Detached update and uninstall continuation is performed by native copies
of the cup executable, so platform filesystem identity and cleanup rules remain
inside the C system layer.

Uninstall deliberately separates two lifetimes. `uninstall_helper.c` owns the
managed-root transaction: validate the journal, detach the exact root and remove
its payload. The platform backend owns the temporary helper executable itself.
POSIX can unlink the running helper pathname; Windows cannot rely on that
property, so `system_windows.c` prepares deferred deletion before launch and
keeps the final handle lifetime outside the helper process. This difference does
not create two uninstall transaction models.

Repository scripts own development tasks:

```text
scripts/build/          build metadata, binary inspection and finalization
scripts/dependencies/   pinned dependency prefixes
scripts/certs/          embedded CA bundle generation and checks
scripts/ci/             CI preparation and evidence files
scripts/install/        public transport installers
scripts/release/        candidate assembly and publication
scripts/lib/            safe repository-path frontend used by scripts
```

`scripts/lib/path-ops.c` is the native path frontend used by repository scripts.
It links the required filesystem modules from `src/` instead of keeping another
copy of the no-follow and identity logic. POSIX hosts use the POSIX backend;
MSYS2 crosses explicitly to the native Windows backend. It also owns
repository-only policy, such as build-root markers, build locks and the
publication modes required by shell callers. Its dispatch is kept in one place
so those operations do not become a set of unrelated helper programs.

Dependency scripts are split by responsibility: `environment.sh` prepares the
controlled build environment, `root-transaction.sh` owns the managed prefix
transaction, `prefix-metadata.sh` validates and records the prefix, and
`source-build.sh` contains shared source download/build operations.

The files under `www/` and the Pages workflow form a separate website surface
and do not participate in cup application, dependency or release responsibilities.

## Module map

The C source files are grouped below by responsibility.

### Entry point and commands

| Module | Responsibility |
|---|---|
| `main.c` | one-pass CLI parsing, help and dispatch |
| `command_context.c` | shared runtime objects used by commands |
| `command_search.c` | available-package search |
| `command_list.c` | installed-package listing |
| `command_info.c` | defaults and exposed commands |
| `command_inspect.c` | installed package metadata |
| `command_config.c` | user preference display and changes |
| `command_install.c` | CLI install planning |
| `command_remove.c` | package removal planning |
| `command_default.c` | default selection |
| `command_update.c` | component/tool update planning |
| `command_doctor.c` | read-only diagnosis |
| `command_repair.c` | ordered recovery and reconstruction |
| `command_uninstall.c` | uninstall planning and helper launch |
| `bootstrap.c` | hidden initial installation entry point |

### `cup update cup` and installed assets

| Module | Responsibility |
|---|---|
| `assets.c` | validate the installed asset generation |
| `self_update.c` | discover, compare, download and stage a cup release |
| `update_helper.c` | detached replacement of installed assets |
| `update_journal.c` | executable-update schema and recovery |
| `uninstall_helper.c` | native root detach, cleanup and recovery ordering |
| `uninstall_journal.c` | uninstall schema and recovery |
| `runtime_journal.c` | shared `transaction.txt` file operations |
| `release_metadata.c` | `release.txt` parsing and validation |

### Package selection and installation

| Module | Responsibility |
|---|---|
| `registry.c` | built-in component/tool relationships |
| `package_catalog.c` | catalog parsing and lookup |
| `package_selector.c` | `<tool>@<release>` parsing |
| `package_request.c` | resolved package request construction |
| `install_policy.c` | official defaults, profiles and toolchains |
| `tool_preferences.c` | local preference overlay |
| `package_install.c` | reusable one-package install transaction |
| `package_transaction.c` | package journal schema and recovery |

### Package files, cache and archives

| Module | Responsibility |
|---|---|
| `package.c` | package identity, semantic validation and installed-tree scanning |
| `installed_package.c` | validation of installed package roots |
| `package_metadata.c` | `info.txt` parsing |
| `package_artifact.c` | artifact coordinates and verified stream ownership |
| `package_cache.c` | cache lookup, refresh and verified download |
| `checksum.c` | checksum-document parsing |
| `third_party/sha256.c` | adapted third-party incremental SHA-256 implementation |
| `download.c` | HTTPS transfer and size/time limits |
| `download_url.c` | CURLU parsing and loopback-only insecure test URL policy |
| `package_archive_format.c` | archive format names and extensions |
| `package_archive.c` | supported archive reader setup and detected-format checks |
| `package_extract.c` | safe extraction of a verified stream |

### State, paths and wrappers

| Module | Responsibility |
|---|---|
| `layout.c` | root selection and managed path construction |
| `state.c` | installed identities and defaults in `state.txt` |
| `wrappers.c` | commands derived from defaults |
| `filesystem.c` | snapshots and atomic managed-file publication |
| `path.c` | path and identifier validation helpers |
| `text.c` | bounded text parsing helpers |

### Platform and process support

| Module | Responsibility |
|---|---|
| `system.c` | portable filesystem queries and shared traversal |
| `system_posix.c` | POSIX filesystem, lock and process primitives |
| `system_windows.c` | Windows filesystem, locks, processes and deferred helper cleanup |
| `platform.c` | host detection and platform validation |
| `interrupt.c` | Ctrl+C and signal state |
| `exit_status.c` | mapping `CupError` to public exit status |

`include/windows_utf.h` is a private Windows helper shared by the Windows backend
and archive extraction for UTF-8/UTF-16 path conversion.

## Why modules are split this way

A separate module is useful when it owns one of these responsibilities:

- a public command;
- a persistent format;
- a resource lifecycle;
- a platform implementation;
- a reusable operation with its own tests.

A file is not split only because it is long. For example, `command_repair.c`
contains one ordered recovery process and keeps its private context together.
On the other hand, archive format parsing and artifact ownership are separate
because they are reused by more than one caller and have different failure
rules.

The same rule is used when sharing code: common mechanics are reused, while
operation-specific policy stays in the owner that understands it.

## Error model

Most functions return `CupError`. They do not call `exit()` themselves. The
entry point converts the final error to the public process status.

Filesystem replacement also reports `SystemCommitState`:

```text
NOT_APPLIED  the destination was not replaced
APPLIED      the replacement may be visible, but durability is uncertain
DURABLE      replacement and required directory metadata were synchronized
```

This extra state matters because an I/O error after a rename is different from
an error before the rename. Recovery must not roll back an operation that may
already be visible without first checking the saved state.

## Deliberate limits

The current design does not include:

```text
administrator or service-based installation
local component compilation during cup install
a dependency solver between component packages
a global shared sysroot
an environment variable for the cup root
automatic PATH modification or cleanup
automatic VERSION increments
nightly package selectors
```

These limits keep the state model and recovery rules small enough to inspect and
test.

## Related documents

- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Platforms](PLATFORMS.md)
- [Security](SECURITY.md)
- [Build](../development/BUILD.md)
