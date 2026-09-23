# Architecture

`cup` is a userspace manager for prebuilt C development tools. Its runtime owns
package selection, verified admission, local state, command wrappers, recovery
and the `cup` generation installed in one managed root. Package compilation and
publication belong to `cup-components`.

The architecture is intentionally split by authority. A file or module may
carry a copy of an identity for validation, but no behavior should have two
independent sources of truth.

## Product boundary

`cup` is not a compiler build system, a system package manager or a global
sysroot manager. It does not decide how GCC, LLVM, GDB, LLDB or Valgrind are
built. It consumes self-contained packages that have already crossed the
producer qualification boundary.

The runtime operates entirely below a user-selected base. The default managed
root is `<home>/.cup`; `.coffee-cup` is only the fallback when `.cup` is a clear
foreign-directory collision. `cup` does not require administrator privileges and
does not write into system toolchain directories.

## Authority graph

The main authorities are deliberately separate:

| Authority | Owns |
| --- | --- |
| compiled registry | known components, tools, platform identifiers and version semantics |
| compiled policy | official abbreviated selections, profiles and toolchains |
| `catalog.cfg` | concrete package availability for published host/target/version tuples |
| `preferences.txt` | user choices for future abbreviated installs |
| `state.txt` | installed package identities and active defaults |
| package `info.txt` | descriptive package identity and producer metadata |
| package `manifest.txt` | exact extracted package tree and content |
| `release.txt` | exact public `cup` release manifest and installed-generation trust metadata |
| wrapper files | derived executable view of current defaults |
| cache objects | optional verified transport reuse; never package or state authority |
| transaction journal | minimum evidence required to recover one interrupted mutation |

A preference is not a default, a wrapper is not state, a cache hit is not
availability and the catalog is not policy. These separations are visible in
the source tree and are part of the product contract.

## Runtime root

A managed root authenticates itself with `root.txt`:

```text
format=2
product=coffee-clang/cup
layout=2
host=<platform>
```

The host belongs to the root. Local package paths, `state.txt`,
`preferences.txt` and package journals therefore do not persist another host
dimension. Objects that cross the root boundary, such as catalog records,
package metadata and release metadata, still carry host explicitly.

The core runtime layout is roughly:

```text
<root>/
  root.txt
  cup.lock
  bin/
    cup[.exe]
    <derived wrappers>
  state.txt
  components/<component>/<tool>/<target>/<version>/
  config/
    catalog.cfg
    preferences.txt        # only when non-empty
  staging/
  release.txt
  LICENSE
  THIRD_PARTY_NOTICES.txt
```

`cache/`, `helpers/` and `recovery/` are lazy. Their absence is not a damaged
installation.

See [State](STATE.md) for the persistent layout and ownership rules.

## Command context and locking

`command_context` binds a command to one authenticated root snapshot, host and
target. Ordinary contexts never create a missing root as a side effect.
Persistent initialization is an explicit capability used only by the commands
that are allowed to create development runtime state: install, preference set
and catalog update.

Read operations use shared authority where possible. Mutations use exclusive
authority around the local commit boundary. Network I/O is not moved under a
global lock merely to simplify reasoning; catalog refresh has its own
snapshot/compare-and-swap lifecycle.

A pending shared transaction is not silently ignored by ordinary mutations.
Doctor reports it, repair owns recovery, and uninstall must first reach a state
where the journal slot can safely be used by the uninstall handoff.

## Package selection and admission

Install resolution happens before package mutation. The planner combines the
request grammar, user preferences, compiled policy and one logical catalog
snapshot. A group/profile/toolchain install preflights every member before the
first package commit; group members then commit sequentially and are not
presented as one atomic transaction.

A package archive is authenticated by the SHA-256 stored in the catalog. The
archive contains `manifest.txt`, which authenticates the extracted tree. `cup`
does not persist another catalog-side copy of the manifest digest.

The high-level admission path is:

```text
request
  -> resolve concrete catalog record
  -> obtain verified archive (cache hit or download)
  -> extract into private staging
  -> validate info.txt + manifest.txt + wrapper namespace
  -> publish package journal
  -> publish canonical package tree
  -> commit state.txt
  -> reconcile derived wrappers
  -> clear journal and disposable staging
```

`state.txt` is the package install/remove commit evidence. Package update is not
a third package transaction type: installing a newer immutable identity reuses
the normal install transaction.

See [Packages](PACKAGES.md) and [Transactions](TRANSACTIONS.md).

## Catalog lifecycle

The local catalog is a versioned snapshot with a monotonic revision and a
stable update URL. Package records contain concrete artifact URLs and SHA-256
digests; `cup` never interprets producer URL templates.

A refresh uses a lifecycle-specific compare-and-swap:

```text
shared lock -> snapshot local catalog -> unlock
network download + validation
exclusive lock -> reload local catalog -> compare -> atomic replace or retry
```

Network I/O never occurs while the runtime lock is held. Retry is bounded.
Lower remote revisions are rejected; equal revision requires byte identity;
higher validated revisions may replace the local snapshot.

`search` uses a best-effort refresh on an existing runtime. Package update uses
one required refresh before planning. Exact local package operations avoid
network when the required concrete identity is already resolvable locally.

## Package update

Update plans one `(component, tool, target)` family at a time from one refreshed
catalog snapshot. The reference is the active default when that default uses the
same tool; otherwise it is the semantic maximum installed release for the tool.

No usable stable means warning and skip. A lower stable never causes a
downgrade. An equal stable is a no-op only after the package being reused passes
the normal installed-integrity boundary. A newer stable is installed or adopted
as another immutable identity. A terminal `-revN` package revision participates
in this same semantic ordering.

The planner freezes the target catalog record, then `package_install` revalidates
the mutable local reference under exclusive authority immediately before a
mutation. Stable is not silently re-resolved mid-command.

## `cup` generation

The installed `cup` generation has four managed objects:

```text
bin/cup[.exe]
release.txt
LICENSE
THIRD_PARTY_NOTICES.txt
```

The live package catalog is runtime state, not a generation asset.

`release.txt` is format 2 and authenticates every other public release asset.
For an installed generation, the manifest entries for the legal files and the
platform binary authenticate the canonical local bytes.

Fresh installation and existing-root generation replacement are deliberately
different lifecycles. A fresh install is assembled completely in a private
sibling root and published with one no-clobber directory move. Existing-root
reinstall and self-update preserve packages, state, preferences and a valid
live catalog while replacing only the generation.

Self-update first proves that the currently running canonical binary matches the
current installed `release.txt`. It then resolves one concrete target release,
verifies target assets from that versioned release and hands the binary-last
commit to a detached verified helper.

## Recovery model

Recovery follows evidence, not desired outcome. The main repair order is:

```text
pending transaction
package scan / quarantine
state
preferences
wrappers
staging
installed generation
catalog
```

Package repair never invents defaults. Wrappers are always reconstructed from
state. Invalid or ambiguous bytes are preserved rather than adopted by shape.
A well-formed catalog from a future unsupported format is preserved/refused,
not mislabeled as corruption and replaced with an older seed.

Generation recovery uses the minimal journal plus `new/` and `old/` byte
evidence. The `cup` binary is committed last. Before that binary commit, recovery
can roll back non-binary generation assets when the old binary is still proven.
After a complete target generation is present, recovery finalizes. A third or
ambiguous binary preserves the workspace and stops.

Uninstall keeps its separate, proven detach/delete handoff because its process
and commit boundary differ from package and generation replacement.

## Platform boundary

The product model is shared across Linux x64/ARM64, macOS x64/ARM64 and Windows
x64. Divergence is kept where the operating system requires it:

- POSIX wrappers are executable scripts; Windows wrappers are `.cmd` files.
- Windows executable lifetime may require detached helper behavior where POSIX
  can unlink or replace executing files.
- Linux releases are fully static; macOS keeps Apple system libraries/frameworks
  dynamic while third-party dependencies are static; Windows keeps qualified
  system DLL imports while third-party/compiler runtimes are static.
- installer PATH integration uses shell startup files on POSIX/macOS and User
  PATH on Windows.

`cup` does not mirror the changing `cup-components` host/target build matrix in
its registry. A concrete known-platform catalog record can become explicitly
installable without adding another runtime support table.

## Main implementation owners

The source layout follows the lifecycle split rather than one generic manager:

| Area | Main owners |
| --- | --- |
| CLI dispatch | `main.c`, `command_*.c` |
| root/context | `layout.c`, `command_context.c`, `filesystem.c` |
| registry and policy | `registry.c`, `install_policy.c`, `tool_preferences.c` |
| version/request resolution | `package_selector.c`, `package_request.c` |
| catalog | `package_catalog.c`, `catalog_refresh.c` |
| package transport | `package_artifact.c`, `package_cache.c`, `download.c` |
| package admission | `package.c`, `package_manifest.c`, `package_extract.c`, `package_install.c` |
| state/wrappers | `state.c`, `installed_package.c`, `wrappers.c` |
| package recovery | `package_transaction.c`, `runtime_journal.c` |
| `cup` generation | `release_metadata.c`, `generation.c`, `bootstrap.c` |
| self-update | `self_update.c`, `update_journal.c`, `update_helper.c` |
| uninstall | `command_uninstall.c`, `uninstall_journal.c`, `uninstall_helper.c` |
| platform implementation | `system_posix.c`, `system_windows.c`, `platform.c` |

## Design rules visible in the code

`cup` prefers small lifecycle-specific owners over universal transaction, asset or
configuration frameworks. Persistent files use strict schemas; the package
catalog is more forward-tolerant because older `cup` versions must be able to
ignore structurally safe future records they cannot operate.

Filesystem replacement uses identity-aware/no-follow primitives at destructive
boundaries. Writes are staged and committed atomically where the filesystem
contract permits it. Errors are propagated as `CupError`; low-level owners
report concrete causes while command owners add only useful context.

The implementation deliberately does not add a database, background refresh,
cache eviction policy, general plugin system, system-wide PATH ownership or a
network-filesystem transaction layer.

## Related documents

- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Security](SECURITY.md)
- [Platforms](PLATFORMS.md)
- [Release process](../development/RELEASES.md)
