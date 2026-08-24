# State

This page explains what cup stores below the user root and how those files are
kept consistent. Recovery after interrupted changes is described in
[Transactions](TRANSACTIONS.md).

## Selecting the cup root

The preferred root is:

```text
POSIX   ~/.cup
Windows %USERPROFILE%\.cup
```

If that directory already belongs to another application, cup keeps it intact
and tries:

```text
POSIX   ~/.coffee-cup
Windows %USERPROFILE%\.coffee-cup
```

The chosen root contains `root.txt`:

```text
format=1
product=coffee-clang/cup
layout=1
```

This file is the normal ownership marker. cup does not select the root from the
executable path and does not support a `CUP_HOME` override.

### Roots without `root.txt`

A root without the ownership marker is not adopted from layout clues alone.
Even when it contains a canonical cup executable, the directory is preserved and
reported as an unmarked cup-like root. cup does not add `root.txt`, mutate that
root or silently choose another root. Familiar names such as `state.txt` or
`components/` alone are not ownership proof.

## Filesystem layout

```text
<cup-root>/
  root.txt
  cup.lock
  state.txt
  transaction.txt          present only during recovery work
  bin/
  components/
  cache/
  staging/
  recovery/                created only when quarantine is needed
  config/
    packages.cfg
    install.cfg
    preferences.txt        created after a user preference is stored
    SHA256SUMS.common
    SHA256SUMS.<host>
  helpers/
    update-helper          .exe on Windows
```

The installer downloads files elsewhere first. The hidden C bootstrap creates
or updates this layout while holding `cup.lock` and using `transaction.txt`.

## Package, cache and staging paths

Installed package:

```text
components/<component>/<tool>/<host>/<target>/<version>/
```

Cached archive:

```text
cache/<component>/<tool>/<host>/<target>/<version>/
  <tool>-<version>-<host>-<target>.<format>
```

Staging names include the operation and the complete package identity. A package
journal is accepted only when its `temporary_name` matches the name expected for
that identity. Recovery therefore cannot be redirected to an unrelated path by
editing only the temporary name.

## `state.txt`

The first line is:

```text
format=1
```

Installed entries use:

```text
installed.<component>.<host>.<target>=<tool>@<version>
```

Default entries use:

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

Only concrete versions are stored. `stable` is resolved before a state entry is
created.

## State validation

Loading has two stages:

1. parse each line and reject malformed or duplicated records;
2. validate the complete in-memory result.

A valid state must satisfy these rules:

- every component, platform, tool and version is valid;
- one installed identity appears at most once;
- one default exists at most once for each component/host/target scope;
- every default refers to an installed package in the same scope;
- the configured capacities are not exceeded;
- normal commands do not operate on records belonging to a different host.

Public mutators keep these rules true while they run. For example, a default can
only select an installed package, and a selected package cannot be removed until
its default has been cleared.

The parser does not accept headerless development formats.

## Reading persistent text

Persistent files are read into one bounded snapshot:

1. open a regular file without following a link;
2. record its native identity and size;
3. read its bytes once;
4. detect data beyond the file limit;
5. propagate read and close errors;
6. parse and hash that same snapshot.

This prevents a caller from validating one file and reopening a replacement
through the same pathname.

Most text formats use printable ASCII and LF line endings. NUL and CR bytes are
rejected. Every non-empty file must end with a complete line. A format may allow
other characters only when its own parser says so.

Different files have different size budgets. A state file, journal and package
catalog do not share one arbitrary maximum. Exceeding a limit is an error; data
is never truncated silently.

## Host and target records

One cup process manages packages that run on its current host. Cross compilation
is represented by a different target, not by a foreign host.

`doctor` reports state or package entries for another host. `repair` preserves
them but does not adopt, remove or select them. Normal mutating commands stop
until the mixed-host state has been resolved manually.

## Capacity limits

The in-memory model uses two different bounded capacities:

```text
installed entries  256
default entries    175
```

Installed packages use an explicit resource budget because cup intentionally
keeps multiple concrete versions of the same tool/scope. Default capacity is
derived from the closed component/host/target scope domain because there can be
at most one default per scope. These are hard limits, not truncation points. A
file that contains more valid records returns a capacity error.

Before writing reconstructed state, `repair` also counts preserved foreign-host
records so it does not create a file that the normal loader cannot read.

## Saving state

`state_save` uses the shared atomic publication helpers in `filesystem.c`:

```text
validate the complete model
write a new sibling temporary file
set its required mode
flush and synchronize it
create state.txt without replacement during first initialization
or replace only the exact state.txt identity previously loaded
synchronize the parent directory when required
record the identity of the newly published state
```

Initial creation is create-only, so a concurrently existing state file is never
adopted or overwritten. Advancing an existing state is tied to the native identity
of the snapshot that the command loaded. A failure before publication means the
previous file is still selected. A failure after publication may mean the new file
is already visible but its durability or new identity could not be confirmed. The
transaction remains available so recovery can inspect the actual state instead of
guessing.

## Defaults

A default belongs to:

```text
component + host + target
```

The selected tool and version are the value.

The first valid package installed in an empty scope becomes the default. Later
installs leave the current default unchanged. `cup default` changes it
explicitly. `cup update` moves it only when it selected the same tool at an older
version.

This allows, for example, a native Linux compiler and a Windows cross compiler
to have different defaults on the same machine.

## User preferences

`preferences.txt` stores choices used by abbreviated installs. Its persisted
document begins with the schema marker:

```text
format=1
preferred.<host>.<target>.<component>=<tool>
```

Preferences do not change installed state or current defaults. They only affect
future component and profile installs.

The file is removed after the last preference is reset.

## Managed wrappers

`bin/` contains the cup executable and wrappers derived from defaults.

Names are:

```text
native target       <entry>
cross target        <target>-<entry>
```

Examples:

```text
gcc
clang
windows-x64-gcc
```

Before committing a new state, cup prepares the full wrapper plan and checks:

- that every default package is valid;
- that each declared entry exists;
- that two packages do not expose the same name;
- that no package tries to expose `cup`;
- that the wrapper representation is valid for the platform.

After state commit the same plan is published. cup does not validate one set of
defaults and then rebuild wrappers from another state snapshot.

Wrappers are derived data. `doctor` reports missing, changed, wrong-type,
wrong-mode and stale wrappers. `repair` rebuilds the expected set from valid
defaults.

### POSIX and Windows representation

POSIX uses executable shell wrappers and Windows uses `.cmd` files. Wrapper-name
collisions use the case semantics observed in the selected cup root, so two
spellings are distinct only when that filesystem namespace keeps them distinct.
The same rule protects the reserved `cup` executable name.

Windows wrappers start with `setlocal DisableDelayedExpansion` so arguments and
paths containing `!` are preserved.

## Locking

The runtime lock path is:

```text
<cup-root>/cup.lock
```

Read-only commands use a shared lock when they need the root. Mutating commands
use an exclusive non-blocking lock.

The lock coordinates running processes. It is not a recovery record: the
operating system releases a lock when a process dies, while partially committed
files may remain. `transaction.txt` records what needs to happen next.

Detached update/uninstall children use a temporary operation handoff so authority
remains continuous while ownership moves between processes. On Windows this
handoff is also checked during root admission because the canonical lock lives
inside the root that uninstall must eventually detach.

## cup assets

The release verification set is larger than the generation retained in the cup
root. `release.txt`, the installer scripts and checksum documents authenticate
the release during bootstrap/update, but are not all persistent runtime assets.

The retained installed generation consists of the main executable,
`packages.cfg`, `install.cfg` and the two checksum documents needed by the
installed asset contract. `SHA256SUMS.common` authenticates catalog/policy and
installer bytes in the release set; the platform checksum authenticates the
executable, `release.txt` and the exact common checksum document used for that
release.

The native `update-helper` is different. It is derived by copying the current
installed executable and may therefore still contain the previous version after
a successful self-update. It is refreshed before `cup update cup` and can also
be rebuilt by `repair`. It does not prove root ownership and is not part of the
release checksum set.

`preferences.txt` is also outside the official generation because it is user
state.

## Invalid state preservation

When reconstruction is safe, `repair` moves the invalid state file to a free
name:

```text
state.txt.invalid
state.txt.invalid.1
state.txt.invalid.2
```

The original content is kept for diagnosis. Preservation moves only the exact
native file or directory identity that repair diagnosed; a pathname replacement
is not adopted. A protected file is not made more writable during preservation.

The new `state.txt` is built only from fully validated packages.

## Recovery directory

`recovery/` is created only when cup has a package object that can be identified
safely but cannot remain in the normal component tree.

The object is moved intact to a unique name. Unknown paths are reported and left
where they are; cup does not guess their identity.

## Consistency rules

The normal state is:

```text
one installed state entry
  <=> one valid installed package directory

a default entry
  => a matching installed package

a managed wrapper
  <=> an entry declared by a valid default package
```

An interrupted operation may temporarily break the first relationship. The
transaction journal and the committed `state.txt` decide whether recovery should
finish or undo the filesystem change.

## Implementation and tests

The responsible modules are listed in [Architecture](ARCHITECTURE.md). Recovery
is explained in [Transactions](TRANSACTIONS.md), and test coverage is described
in [Testing](../development/TESTING.md).

## Related documents

- [Packages](PACKAGES.md)
- [Transactions](TRANSACTIONS.md)
- [Commands](../user/COMMANDS.md)
- [Security](SECURITY.md)
