# `cup` documentation

`cup` is a userspace manager for prebuilt C development tools. This manual is
organized around what a reader needs to understand rather than around the order
of files in the repository.

If you are new to `cup`, read **Getting started** and **Concepts** first. The design
pages document the persistent formats and internal contracts; the development
pages describe how this repository is built, tested and released.

## Use `cup`

- [Getting started](user/GETTING_STARTED.md) — install `cup` and complete the first
  package/default workflow.
- [Concepts](user/CONCEPTS.md) — components, tools, packages, host/target scopes,
  versions, preferences, defaults, profiles, toolchains and wrappers.
- [Installation](user/INSTALLATION.md) — supported hosts, installation root,
  PATH behavior, relocation, update, reinstall and uninstall.
- [Commands](user/COMMANDS.md) — public CLI syntax, effects and exit statuses.

These pages are sufficient for normal use. `cup help` and `cup help <command>`
remain the version-specific CLI reference shipped by the executable.

## Understand the design

- [Architecture](design/ARCHITECTURE.md) — repository boundary, runtime layers,
  command flow and module ownership.
- [Packages](design/PACKAGES.md) — catalog, package identity, metadata, manifest,
  archive admission, cache and the contract with `cup-components`.
- [State](design/STATE.md) — managed root, `state.txt`, preferences, defaults,
  wrappers and installed assets.
- [Transactions](design/TRANSACTIONS.md) — locking, commit points, package
  transactions, self-update, uninstall and recovery.
- [Platforms](design/PLATFORMS.md) — Linux/macOS/Windows differences and native
  filesystem/process behavior.
- [Security](design/SECURITY.md) — trust boundaries, transport, checksums,
  archive/filesystem validation and supply-chain limits.

The design pages describe contracts that matter across modules. Function-level
implementation details stay in the source unless they are required to explain a
persistent format, platform boundary or recovery rule.

## Develop `cup`

- [Build](development/BUILD.md) — configurations, native toolchains, pinned
  dependencies, linking policy and public Make targets.
- [Testing](development/TESTING.md) — unit, integration, repository, coverage,
  sanitizer, portability and release tests.
- [Releases](development/RELEASES.md) — versioning, source-tested build identity,
  candidate construction, native validation and publication.

## Scope

`cup` intentionally does not require privilege elevation, build component tools
from source during `cup install`, maintain a system-wide package database, manage
a global sysroot or rewrite the system PATH. Package availability is limited to
the built-in component/tool domain and the installed catalog. `stable` is the
only symbolic package release selector.
