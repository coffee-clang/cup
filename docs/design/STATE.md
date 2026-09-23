# State

`cup` keeps installed identities, user preferences, the live catalog and the
installed `cup` generation below one authenticated userspace root. Persistent
objects are deliberately split by authority: package state is not inferred from
wrappers, preferences are not defaults, and the catalog is not product policy.

Interrupted mutations are described in [Transactions and recovery](TRANSACTIONS.md).

## Root selection and ownership

The default managed root is:

```text
POSIX    $HOME/.cup
Windows  %USERPROFILE%\.cup
```

If `.cup` is a clear foreign-directory collision, `cup` preserves it and may use
`.coffee-cup`. The fallback is not a second installation slot and is never used
to bypass a damaged recognized `cup` root.

A managed root authenticates itself with:

```text
format=2
product=coffee-clang/cup
layout=2
host=<platform>
```

The host belongs to the root. Local state, preferences, package paths and package
journals therefore do not persist a second host dimension. Package/catalog
objects that can cross roots still carry host explicitly.

An installed executable binds itself to the root containing its real
`bin/cup[.exe]` and authenticates that root. Markerless `cup`-like roots are
preserved and reported rather than adopted heuristically.

## Filesystem layout

A normal installed root is:

```text
<cup-root>/
  root.txt
  cup.lock
  state.txt
  transaction.txt              # only while a durable operation is pending
  release.txt
  LICENSE
  THIRD_PARTY_NOTICES.txt
  bin/
    cup[.exe]
    <derived wrappers>
  components/
  staging/
  config/
    catalog.cfg
    preferences.txt            # only when non-empty
```

`cache/`, `helpers/` and `recovery/` are lazy and may be absent in a healthy
installation. `components/` and `staging/` are runtime-core directories.

Fresh installation is assembled first in a private sibling such as
`.cup-install-<token>` and published by one no-clobber directory move. Ordinary
commands never treat such a sibling as an installed root.

## Package and cache paths

Installed packages use the root host implicitly:

```text
components/<component>/<tool>/<target>/<version>/
```

The cache is content-addressed:

```text
cache/<artifact-sha256>
```

There is no package identity sidecar, archive extension, TTL or LRU database.
Every cache hit is rehashed before use. Cache failure is non-fatal when `cup` can
continue from a verified temporary artifact.

## `state.txt`

State is a strict line-based format:

```text
format=2
installed.<component>.<target>=<tool>@<version>
default.<component>.<target>=<tool>@<version>
```

Example:

```text
format=2
installed.compiler.linux-x64=clang@23.1.0
installed.compiler.linux-x64=clang@23.1.0-rev1
default.compiler.linux-x64=clang@23.1.0-rev1
```

Only concrete package versions are stored. `stable` is a catalog selector and
never becomes persistent state.

When a state file is loaded, `cup` obtains the host from the authenticated root and
reconstructs complete in-memory package identities. The parser first builds a
private candidate, validates all global relationships and only then publishes
that candidate to the caller.

A valid state requires:

- canonical component/tool/target/version values;
- no duplicate installed identity;
- at most one default for each `(component, target)` scope;
- every default to reference an installed package in the same scope;
- the complete canonical document to fit the persistent state budget.

Installed entries are dynamically allocated. There is no historical-version
record-count cap such as 256. The canonical `state.txt` byte budget is 4 MiB.
Defaults remain naturally bounded by the finite component × target scope set.

## Publishing state

`state_save` validates and measures the complete canonical document before
publication. It then writes a sibling temporary file, applies the required mode,
flushes the file and replaces only the exact `state.txt` identity previously
observed. The caller receives the new identity.

A replacement may become visible before durability can be fully confirmed. Such
an outcome is not rewritten into a false pre-commit failure; the surrounding
transaction remains available so recovery can inspect the actual filesystem.

## Defaults

A default is keyed by:

```text
component + target
```

The root supplies host implicitly.

The first package installed in a genuinely empty scope may become the default.
A scope that already contains package history but has no default stays without a
default until the user chooses one. Installing another version does not invent a
replacement default.

`cup default` changes only the default and never installs. Removing the active
package clears the default; `cup` does not choose another installed version
implicitly. Package update advances a default only when it already selected the
same tool at an older semantic version.

## Preferences and compiled policy

User preferences live in:

```text
config/preferences.txt
```

with:

```text
format=2
preferred.<target>.<component>=<tool>
```

The file owns only user choices for future abbreviated installs. It does not
contain fallback policy, installed state or defaults, and it is removed when the
last preference is reset.

Official selections, profiles and toolchains are compiled policy belonging to
the `cup` binary generation. Install planning combines a user preference with
compiled policy explicitly; persistence does not perform that fallback itself.

## Live catalog

The live package snapshot is:

```text
config/catalog.cfg
```

It is managed runtime state and can advance independently from the installed `cup`
generation. `cup` source does not track a seed; an official `cup` release contains
the published catalog snapshot acquired before that candidate's qualification.
Once a valid runtime catalog exists, reinstall and self-update preserve it.

Catalog refresh has its own revision/CAS lifecycle. A well-formed catalog in a
future unsupported format is preserved/refused rather than mislabeled as
corruption and replaced with an older seed.

## Managed wrappers

`bin/` contains the `cup` executable plus wrappers derived from recorded defaults.
Wrapper naming is:

```text
native target  <entry-basename>
cross target   <target>-<entry-basename>
```

The metadata key after `entry.` is not the public alias; the basename of the
entry path supplies the public command name. Windows adds the `.cmd` wrapper
convention.

Before a default-changing commit, `cup` builds the prospective full wrapper
namespace and rejects reserved-name or case-semantic collisions. After state
commit, wrapper reconciliation may replace stale derived files. Wrappers never
become authority for state or defaults.

## Installed `cup` generation

The immutable installed generation consists of:

```text
bin/cup[.exe]
release.txt
LICENSE
THIRD_PARTY_NOTICES.txt
```

`release.txt` authenticates the other three objects through the platform binary
release name and SHA-256 digests. The live catalog, package state, preferences,
cache and wrappers are outside the generation.

`helpers/update-helper[.exe]` is a lazy byte-identical handoff copy of the
trusted running binary. It is disposable transaction machinery, not a generation
asset or health requirement.

## Persistent-file snapshots

Persistent text used for a decision is read as one bounded regular-file
snapshot: `cup` records native identity/size, reads the bounded bytes once and
parses/hashes that same snapshot. A later pathname replacement is not silently
accepted as the validated object.

Atomic writers use identity-bound create/replace/delete operations where the
lifecycle requires them. Invalid or ambiguous bytes are preserved instead of
being truncated or partly accepted.

## Recovery storage

When repair can prove a safe reconstruction, invalid state or preferences may be
preserved under collision-safe `.invalid` names before the canonical object is
recreated. `recovery/` is created lazily for identifiable invalid package trees
that can be moved safely out of the managed component hierarchy.

Unknown higher-level structures are reported and left in place. Repair never
adopts an object merely because its pathname resembles a managed one.

## Consistency model

In a healthy installation:

```text
one installed state identity
  <=> one valid canonical package tree

a default
  => one matching installed identity

a managed wrapper
  <=> one command provided by a valid default package

release.txt + legal files + canonical binary
  => one valid `cup` generation
```

Transactions may temporarily break these relationships. State plus the relevant
journal/workspace evidence determines what recovery may safely finish or roll
back.

## Related documents

- [Architecture](ARCHITECTURE.md)
- [Packages](PACKAGES.md)
- [Transactions and recovery](TRANSACTIONS.md)
- [Commands](../user/COMMANDS.md)
- [Security](SECURITY.md)
