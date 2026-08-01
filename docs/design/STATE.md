# State

This document defines the canonical local layout, persistent state, default
selection, locking and managed wrappers. Interrupted mutations are specified
in [TRANSACTIONS](TRANSACTIONS.md).

## Persistent root

The primary root is:

```text
POSIX   ~/.cup
Windows %USERPROFILE%\.cup
```

It is derived from `HOME` or `USERPROFILE`, is not configurable and is not
derived from the executable path. If the primary name already identifies an
unrelated directory, CUP preserves it and selects:

```text
POSIX   ~/.coffee-cup
Windows %USERPROFILE%\.coffee-cup
```

`root.txt` contains exactly `format=1`, `product=coffee-clang/cup`, and
`layout=1`. A candidate with that valid marker is owned by CUP.

A markerless root is adopted only when its complete installed generation can be
verified without executing the discovered binary. CUP requires the canonical
executable, the matching native update-helper copy, the uninstall helper, the
exact common and platform checksum sets, matching asset digests, strictly
parsed catalog and installation policy, an optional valid `state.txt`, and real
(non-link) runtime directories. Merely finding familiar directory names or a
`format=1` line is never sufficient.

A markerless candidate that contains the canonical CUP executable but fails
that complete verification is classified as a probable damaged legacy
installation. CUP preserves it, stops, and does not select or create the
alternative root. Other familiar-looking files such as `state.txt`, checksum
files or runtime directory names do not establish ownership without the
canonical executable; those markerless directories remain foreign. With an
invalid marker, the broader asset set is still used as evidence that CUP must
stop rather than hide a damaged installation. If both candidates are
recognized, CUP stops instead of choosing silently. A verified legacy root
receives the marker atomically during the next mutating command.

The selected root provides one location for:

- operating-system locking;
- one shared transaction journal;
- cup assets verification;
- package identity paths;
- deterministic repair;
- managed wrappers.

## Filesystem layout

```text
<cup-root>/
  root.txt               persistent CUP ownership marker
  bin/
  components/
  staging/
  cache/
  recovery/               created only when quarantine is required
  config/
    packages.cfg
    install.cfg
    preferences.txt       created only after a local configuration change
    SHA256SUMS.common
    SHA256SUMS.<host>
  helpers/
    cup-update-helper     native helper, .exe on Windows
    uninstall.sh          POSIX
    uninstall.ps1         Windows
  state.txt
  transaction.txt         pending or failed mutation; removed after success/acknowledgement
  cup.lock
```

The cup asset installer initially creates `bin`, `config` and `helpers`,
including the native cup-update helper. The first operational command or
`repair` creates the remaining runtime directories and state/lock files.

## Package and cache paths

Installed packages:

```text
components/<component>/<tool>/<host>/<target>/<version>/
```

Cached archives:

```text
cache/<component>/<tool>/<host>/<target>/<version>/
  <tool>-<version>-<host>-<target>.<format>
```

Staging names include the operation and complete identity. A transaction
journal is accepted only when its recorded temporary basename matches the
identity-derived prefix. This prevents a valid-looking journal from redirecting
recovery to an unrelated path.

## `state.txt`

The first line is mandatory and versioned:

```ini
format=1
```

Installed entries:

```text
installed.<component>.<host>.<target>=<tool>@<version>
```

Default entries:

```text
default.<component>.<host>.<target>=<tool>@<version>
```

Example:

```text
format=1
installed.compiler.linux-x64.linux-x64=gcc@16.1.0-rev1
installed.compiler.linux-x64.windows-x64=gcc@16.1.0-rev1
default.compiler.linux-x64.linux-x64=gcc@16.1.0-rev1
```

State stores concrete versions. It does not store `stable`.

## Loading and validation

State loading has two responsibilities:

1. parse each line, identifier and duplicate rule;
2. validate the complete in-memory model.

The pre-release headerless representation is rejected; there is no
compatibility reader.

Complete validation checks:

- valid component, platform and entry identifiers;
- no duplicate installed identity;
- no duplicate default scope;
- every default refers to an installed entry in the same scope;
- configured counts remain within capacity;
- normal operational contexts contain no records for a host different from the current host.

Normal commands require a fully valid model. `doctor` can report semantic
inconsistency. `repair` preserves an invalid state file before reconstructing it
only when no pending transaction makes the intended commit ambiguous.


## Foreign-host preservation

A single cup process manages only packages executable on its current host;
cross-compilation is represented by `target`, not by a foreign `host`. `doctor`
reports foreign-host state records and package trees. `repair` preserves them
byte-for-byte but does not adopt, quarantine, delete or select them. Operational
commands refuse to proceed until the user resolves that mixed-host state.

## Capacity limits

Current state capacities are:

```text
installed entries  128
default entries     32
```

They are deliberately bounded in-memory limits, not silent truncation. Loading or
constructing a larger model fails. `repair` also stops before reconciling a
package scan that cannot be represented completely.

## Atomic save

`state_save`:

```text
validates the complete candidate model
writes an exclusive temporary file beside state.txt
flushes and synchronizes the file
atomically replaces state.txt
synchronizes required parent metadata
reports whether replacement was not applied, applied, or durable
```

A failure after replacement may mean that the new state is already visible. It
is reported as a commit uncertainty rather than a normal save failure. The
journal remains so recovery can inspect the actual persistent state.

## Default scope

A default is selected for:

```text
component + host + target
```

The tool is the selected value, not part of the scope key. Therefore one
compiler package can be default for native Linux while another compiler package
is default for a Windows target on the same host.

The first installation in an empty scope becomes the default automatically.
Later installations do not replace the existing default. `cup default` changes
it explicitly. `cup update` moves it only when it still belongs to the tool being
updated.

## Managed package commands

The `bin` directory contains the `cup` executable and wrappers derived from
active packages.

Naming:

```text
native active package       <command>
cross-target active package <target>-<command>
```

Examples:

```text
gcc
clang
windows-x64-gcc
```

The complete wrapper set is planned before a state change. Planning validates:

- the default package and its metadata;
- declared entry names and paths;
- collisions between defaults;
- the reserved name `cup`;
- platform-specific wrapper representation.

The same immutable plan is applied after state commit. This avoids validating
one state and writing wrappers for another.

Wrappers are derived data. `doctor` checks missing, altered and stale wrappers;
`repair` rebuilds the exact set from valid defaults.

## Wrapper representation

On POSIX, wrappers are executable shell wrappers pointing at canonical package
entries. On Windows, `.cmd` wrappers use native path and quoting rules. The
public naming model is the same; implementation differences are documented in
[PLATFORMS](PLATFORMS.md).

## Locking

The canonical lock file is:

```text
<cup-root>/cup.lock
```

Read commands acquire a shared lock. Mutating commands acquire an exclusive
non-blocking lock. The lock is held for the complete one-scope operation unless
a command explicitly creates a new per-scope update operation.

The lock coordinates processes; it does not replace the transaction journal.
A process can terminate while holding a lock, causing the operating system to
release the lock while persistent filesystem changes remain. The journal
records what must be recovered next.

## cup assets state

The package catalog, official installation policy, common checksums, platform
checksums, uninstall helper, native cup-update helper and canonical executable are inspected as one cup
asset generation. `SHA256SUMS.common` covers both `packages.cfg` and
`install.cfg`; their published checksums and file protections are checked by
`doctor`. An official `repair` restores only assets that can be replaced safely
from its immutable release; a Windows executable that is itself missing or
altered requires the official installer. Development builds report this boundary
instead of downloading an official generation.

`preferences.txt` is deliberately outside that verified generation. It is a
locally mutable overlay written atomically by `cup config`, parsed strictly and
removed when the last scoped preference is reset. It never changes installed-package
state or the selected default version of an installed component.

cup assets metadata and installation preferences are not stored in `state.txt`.
Package state, cup assets integrity and local selection policy have different
lifecycles and recovery mechanisms. Detached update and uninstall outcomes do
not create separate result or pending files: `transaction.txt` remains their
single authoritative durable record until success or explicit recovery.

## Invalid state preservation

When deterministic reconstruction is safe, `repair` moves an invalid state file
to a unique name:

```text
state.txt.invalid
state.txt.invalid.1
state.txt.invalid.2
```

Preservation is preferred to deletion because the original content can still be
used for diagnosis. The replacement state is derived only from fully valid
canonical packages.

## Recovery directory

`recovery/` is created lazily. Invalid package objects are moved there only when
their complete canonical path identifies the package unambiguously. The original
object is kept intact under a unique destination.

Unrecognized paths are not guessed, renamed or deleted. They are reported for
manual inspection.

## Consistency model

The intended invariant is:

```text
state installed entry
  <=> one valid canonical package directory

default entry
  => matching installed entry and valid package

managed package command
  <=> declared entry of a valid default package
```

A crash can temporarily violate the first relationship. The journal plus
persistent state determines whether recovery completes or rolls back the
operation. See [TRANSACTIONS](TRANSACTIONS.md).

## Implementation and verification

State-module responsibilities are listed in [ARCHITECTURE](ARCHITECTURE.md). Persistence,
recovery and process-level verification are described in
[TRANSACTIONS](TRANSACTIONS.md) and [TESTING](../development/TESTING.md).

## Related documents

- [PACKAGES](PACKAGES.md) — package identities and metadata;
- [TRANSACTIONS](TRANSACTIONS.md) — interrupted mutation recovery;
- [COMMANDS](../user/COMMANDS.md) — list, info and default behavior;
- [SECURITY](SECURITY.md) — read-only and integrity protections.
