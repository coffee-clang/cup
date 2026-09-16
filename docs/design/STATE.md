# State

CUP keeps application state, installed packages and derived launchers below one
user-managed root. This page documents that layout and the persistent records
that define an installed configuration.

Interrupted mutations are covered separately in
[Transactions and recovery](TRANSACTIONS.md).

## Root selection and ownership

The default roots are:

```text
POSIX    $HOME/.cup
Windows  %USERPROFILE%\.cup
```

If `.cup` already exists but is not a CUP root, base selection preserves it and
uses `.coffee-cup` as the deterministic fallback. Both leaf names are part of the
product layout; selecting another base does not allow an arbitrary leaf name.

The normal ownership marker is:

```text
root.txt
```

with:

```text
format=1
product=coffee-clang/cup
layout=1
```

An installed executable derives its active root from its own real
`<cup-root>/bin/cup[.exe]` location and then authenticates the marker. CUP does
not use a persistent `CUP_HOME` override or a global root registry.

A markerless directory is never adopted only because it contains familiar files
or a CUP-looking executable. Such a directory is preserved and reported rather
than mutated.

## Filesystem layout

The normal layout is:

```text
<cup-root>/
  root.txt
  cup.lock
  state.txt
  transaction.txt          only while recovery/transaction state exists
  bin/
  components/
  cache/
  staging/
  recovery/                created when an invalid object must be preserved
  config/
    packages.cfg
    install.cfg
    preferences.txt        created only when user preferences exist
    SHA256SUMS.common
    SHA256SUMS.<host>
  helpers/
    update-helper          update-helper.exe on Windows
```

The installer transports verified release files into a private location. Hidden
bootstrap then creates/updates the managed layout through the same locking and
update-transaction machinery used by the installed program.

## Package and cache paths

Installed package:

```text
components/<component>/<tool>/<host>/<target>/<version>/
```

Cached archive:

```text
cache/<component>/<tool>/<host>/<target>/<version>/
  <tool>-<version>-<host>-<target>.<format>
```

Staging names include the operation and complete package identity. Transaction
recovery accepts only the staging name derived for the journal's own identity.

## `state.txt`

State is line-based and begins with:

```text
format=1
```

Installed records:

```text
installed.<component>.<host>.<target>=<tool>@<version>
```

Default records:

```text
default.<component>.<host>.<target>=<tool>@<version>
```

Example:

```text
format=1
installed.compiler.linux-x64.linux-x64=gcc@16.2.0-rev1
installed.compiler.linux-x64.windows-x64=gcc@16.2.0-rev1
default.compiler.linux-x64.linux-x64=gcc@16.2.0-rev1
```

Only concrete versions are stored. `stable` is resolved before state is created.

A valid model requires:

- valid component/tool/platform/version values;
- no duplicate installed identity;
- at most one default per component/host/target scope;
- every default to reference an installed package in the same scope;
- all records to fit the configured capacity;
- normal operation to use records belonging to the current host.

The parser and semantic validator are intentionally separate: malformed text is
never accepted partially, and a syntactically valid document still has to satisfy
the complete state relationships above.

## Persistent-file snapshots

Persistent text that participates in a decision is read as one bounded snapshot:

```text
open expected regular file without following a final link
record native identity and size
read the bounded bytes once
propagate read/close errors
parse/hash that same snapshot
```

This avoids validating one file and later treating a replacement at the same
pathname as if it were the validated object.

Most persistent text uses printable ASCII with LF line endings and one complete
final line. Individual formats define their own size and field limits; CUP does
not silently truncate oversized state, journal or catalog data.

## Host and target records

One running CUP executable manages packages that run on its own host. A cross
compiler is represented by a different **target**, not by a foreign host record.

Foreign-host state/package entries are reported and preserved. Normal mutation
does not adopt or silently delete them, and repair includes them when deciding
whether reconstructed state would exceed its capacity.

## Capacity

The state model has independent hard bounds:

```text
installed entries  256
default entries    175
```

Installed capacity is an explicit resource budget because multiple versions may
coexist. Default capacity is derived from the finite
component × host × target domain because there can be at most one default in each
scope.

Exceeding either capacity is an error rather than a truncation point.

## Publishing state

`state_save` publishes the complete validated model atomically:

```text
serialize to a sibling temporary file
apply required mode
flush/synchronize file content
create state.txt without replacement for first initialization
or replace only the exact state.txt identity previously loaded
synchronize the parent directory where supported/required
return the new state-file identity
```

The identity precondition prevents a command from overwriting a different
`state.txt` that appeared after its snapshot was loaded.

Publication can also fail after a rename/replacement has become visible but
before durability is fully confirmed. In that case the surrounding transaction
is preserved so recovery can inspect the real filesystem instead of assuming the
old state is still selected.

## Defaults

A default is keyed by:

```text
component + host + target
```

and points to one installed tool/version in that scope.

The first valid package installed in an empty scope becomes the default. Later
installs leave it unchanged. `cup default` changes it explicitly. `cup update`
advances a default only when that default already selected the same tool at an
older release.

Different targets therefore keep independent defaults on the same host.

## Preferences

User preferences live in:

```text
config/preferences.txt
```

with:

```text
format=1
preferred.<host>.<target>.<component>=<tool>
```

Preferences affect future abbreviated component/profile installs. They do not
modify installed package records or current defaults. The file is removed when
the last stored preference is reset.

## Managed launchers

`bin/` contains the CUP executable plus launchers derived from defaults.

Naming is:

```text
native target  <entry>
cross target   <target>-<entry>
```

For example:

```text
gcc
clang
windows-x64-gcc
```

CUP builds a complete launcher plan from validated defaults before committing a
state change that needs it. Planning checks the declared package entries,
reserved CUP name and collisions. After state commit the prepared plan is
published.

Launchers are derived state. `doctor` reports missing, invalid, conflicting or
stale launchers; `repair` rebuilds the desired set from valid defaults.

POSIX launchers are executable shell wrappers. Windows launchers are `.cmd`
files and disable delayed expansion so arguments/paths containing `!` survive.
Name collision rules follow the case behavior observed in the selected root.

## Runtime lock

The root lock is:

```text
<cup-root>/cup.lock
```

Read-only commands that need the root use shared access. Mutating commands use
exclusive non-blocking access.

The lock coordinates **live processes**; it is not crash recovery. The OS drops a
lock when a process terminates, while a half-completed filesystem transition may
remain. `transaction.txt` records the information needed across process lifetime.

Self-update/uninstall can transfer exclusive ownership to a detached helper; the
platform-specific handoff is described in [Transactions](TRANSACTIONS.md) and
[Platforms](PLATFORMS.md).

## Installed CUP generation

The files retained for the installed CUP generation are the main executable,
`packages.cfg`, `install.cfg`, `SHA256SUMS.common` and the platform checksum.
Release metadata/installers participate in bootstrap/update verification but are
not all retained as runtime assets.

`helpers/update-helper[.exe]` is operational data copied from the currently
installed executable before self-update. It may therefore lag the main executable
between updates; it is not release identity or root ownership evidence.

`preferences.txt` is user state and is likewise outside the official asset
generation.

## Preservation and recovery storage

When repair can safely reconstruct state, an invalid state object is preserved
under the first free name:

```text
state.txt.invalid
state.txt.invalid.1
state.txt.invalid.2
...
```

Preservation is tied to the exact object repair diagnosed. A replacement that
appears under the same pathname is not adopted automatically.

`recovery/` is created only for package objects whose identity can be established
safely but that cannot remain in the normal component tree. Unknown/ambiguous
objects are reported and left in place.

## Consistency model

In a healthy installation:

```text
one installed state entry
  <=> one valid canonical installed package

a default
  => one matching installed package

a managed launcher
  <=> an entry provided by one valid default package
```

A transaction may temporarily break these relationships. The committed state
and transaction journal determine whether recovery finishes or rolls back the
filesystem side of the operation.

## Related documents

- [Concepts](../user/CONCEPTS.md)
- [Packages](PACKAGES.md)
- [Transactions and recovery](TRANSACTIONS.md)
- [Commands](../user/COMMANDS.md)
- [Security](SECURITY.md)
