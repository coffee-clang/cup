# Security model

`cup` downloads executable packages and mutates a user-managed toolchain root. Its
security model therefore concentrates on remote transport, package admission,
filesystem identity, release provenance and crash recovery. It does not claim to
defend against an attacker that already has arbitrary control of the same user
account and can rewrite `cup` memory while it runs.

## Trust boundaries

The runtime separates several authorities rather than treating one metadata file
as a global source of truth:

- compiled registry and policy define what the running `cup` understands and which
  abbreviated choices it makes;
- `catalog.cfg` defines concrete package availability and artifact digests;
- package `manifest.txt` defines the exact extracted tree;
- `state.txt` defines installed logical identities/defaults;
- installed `release.txt` authenticates the `cup` generation;
- native filesystem identity binds later mutation to the object that was
  actually inspected.

A familiar pathname, cache entry, wrapper or package directory shape is never
sufficient evidence by itself.

## HTTPS and bootstrap trust

Normal product downloads and redirects require HTTPS. Catalog and release URLs
are validated before transfer. Product code permits HTTP only for the explicit
restricted loopback test mode; that path is not usable as a normal remote
endpoint.

The public installer is the first bootstrap code executed before an installed
`cup` generation exists. Its initial trust comes from HTTPS transport. It resolves
one concrete release identity and then uses that versioned release consistently;
it never combines metadata from one moving `latest` lookup with assets from a
later one.

`release.txt` authenticates subsequently downloaded release assets, but it cannot
retroactively authenticate installer code that has already been obtained and
executed. `cup` therefore does not claim that a self-hash removes the initial HTTPS
bootstrap boundary.

## CA bundle and TLS backends

POSIX builds use OpenSSL with an embedded tracked CA bundle. Certificate inputs
under `certs/` are validated and converted to generated C data during the build;
the runtime does not load a CA file from the build machine.

The pinned OpenSSL build disables automatic configuration loading and runtime DSO
modules. Windows uses Schannel through libcurl.

## SHA-256 primitives

`src/third_party/sha256.c` provides incremental SHA-256; `checksum.c` owns
canonical digest validation and hashing helpers used by release and package
authentication.

Where possible hashing operates on the already opened regular-file stream used by
later consumers, avoiding a validate-then-reopen pathname gap.

## `cup` release manifest

Public `cup` releases contain one `release.txt` format 2 manifest. It records:

```text
format=2
version=<version>
commit=<source commit>
root_layout=2
catalog_format=1
asset_count=<N>
asset.0.name=<name>
asset.0.sha256=<sha256>
...
```

Every public release asset except `release.txt` appears exactly once, with names
rendered in canonical lexical order. The manifest has no self-hash.

The public set contains the release-pinned catalog snapshot, installers,
legal/provenance files and all five platform binaries. Candidate assembly creates every other
asset first, generates `release.txt` last from their exact bytes, then validates
the exact set.

An installed generation retains only:

```text
bin/cup[.exe]
release.txt
LICENSE
THIRD_PARTY_NOTICES.txt
```

The live catalog is independent runtime state and can advance after installation.

## Self-update trust precondition

Before `cup update cup` creates a helper or generation journal, the current
installed `release.txt` must be valid and the canonical running `cup` binary must
hash-match its platform entry. A missing/corrupt manifest or binary mismatch
stops before mutation; repair/reinstall owns recovery of that condition.

The updater resolves one concrete target version and then fetches all target
metadata/assets from that versioned release. Equal versions are no-ops and
downgrades are rejected.

## Package catalog and transport

Catalog records contain concrete artifact URLs and SHA-256 digests. `cup` does not
expand producer URL templates at runtime and does not persist another
catalog-side copy of the package manifest digest.

The cache path is content-addressed:

```text
cache/<artifact-sha256>
```

Every cache hit must be a safe regular file and is rehashed before use. Invalid
or unusable cache content is bypassed; cache write failure is non-fatal when `cup`
can continue from verified temporary bytes.

On a miss, download occurs into private temporary storage, the catalog digest is
verified, and only then may those bytes be published opportunistically into the
cache and consumed by extraction.

## Archive admission

Digest-valid archives are still untrusted structured input. Libarchive consumes
the verified stream into fresh private staging while `cup` enforces path/resource
rules, including:

- one safe top-level package root;
- portable relative path grammar and resource bounds;
- no case-fold file/directory aliases;
- no hard-link or special-object entries;
- no traversal outside private staging.

POSIX packages may contain confined relative symbolic links. A link cannot be
used as the parent of a later write and final manifest validation proves that it
resolves within the package to an allowed target. Windows package content rejects
reparse/symlink traversal rather than emulating POSIX links.

After extraction `cup` validates `info.txt`, entry executables and the complete tree
against `manifest.txt` before any canonical package commit.

## Package metadata and manifest

Consumer-owned identity fields in `info.txt` must match the selected
component/tool/host/target/version. A revision-bearing package requires a
revision reason and its `source.primary.version` must be the unsuffixed upstream
base version.

`manifest.txt` format 2 is the exact package-tree authority. `cup` verifies every
managed regular-file digest/mode, directory and permitted link and rejects
undeclared objects. Full manifest verification is used at admission, package
scanning and integrity diagnosis; lightweight descriptive queries do not rehash
an entire toolchain unnecessarily.

## Filesystem identity and mutation

When a destructive or replacing operation depends on an earlier observation,
`cup` retains native identity and revalidates the object at the mutation boundary.
A different object that appears under the same pathname is not accepted merely
because its text path matches.

Recursive cleanup does not follow links/reparse points and remains bound to the
starting filesystem identity/device rules. No-replace publication uses native
primitives rather than a check-then-rename approximation.

These mechanisms protect against realistic races and accidental mutation within
the product threat model; they are not a promise of kernel-level CAS against a
hostile same-user process with arbitrary execution.

## Root ownership

`root.txt` is the normal managed-root proof:

```text
format=2
product=coffee-clang/cup
layout=2
host=<platform>
```

A directory is not adopted merely because it is named `.cup` or contains
recognizable state. Markerless `cup`-like roots are preserved for explicit
recovery/reinstallation. A clear foreign `.cup` collision may select
`.coffee-cup`; a corrupt recognized root does not.

Fresh installer siblings use a separate private-name/permission contract and are
published only by a final no-clobber move. Detached uninstall residue has its own
strict token-bound ownership proof and is never auto-adopted by an installer.

## Persistent state and transactions

Persistent state/journal/catalog/config text is read from bounded regular-file
snapshots. Atomic writers publish through sibling temporaries and tie
replacement/deletion to expected native identities where required.

Package journals contain only install/remove identity and staging ownership. `cup`
generation journals contain only target `release.txt` digest and workspace name.
The generation binary is committed last; recovery reasons from `new/old` bytes
and the actual canonical binary rather than trusting a stored progress phase.

Ambiguous transaction evidence is preserved. `cup` does not clear it merely to let
normal commands continue.

## Self-update and uninstall handoff

Self-update derives a lazy helper copy from the already authenticated canonical
binary. The parent establishes continuous handoff authority before exiting; the
helper then reacquires the canonical root lock and performs the binary-last
commit. The live catalog is not replaced.

Uninstall uses a separate proven helper/detach protocol. The canonical root move
to its token-bound sibling is the uninstall commit. POSIX can unlink the verified
running helper path; Windows binds deferred deletion to the exact helper/process
lifetime.

## Build and release provenance

CI separates source qualification from publication:

1. pinned dependency prefixes are built/verified;
2. Tests owns repository checks, native source tests, coverage and sanitizers;
3. source-tested `build-config.txt` evidence is retained for the exact run
   attempt;
4. Release builds official candidates for the same source commit and verifies
   build identity against that Tests evidence;
5. native candidate tests run without rebuilding candidate bytes;
6. final assembly generates `release.txt` from the exact public asset set;
7. publication compares the immutable local snapshot with remote state before a
   draft becomes public.

Only the publication job receives release write permission. A published `cup`
release is immutable by project policy; same-version success is idempotent only
when the complete bytes already match.

`provenance.txt` ties public bytes to source repository/commit and workflow
evidence. It is itself authenticated by `release.txt`; there is no recursive
hash-back into the evidence it describes.

## Read/query operations

Help/version never need the managed root. `list`, `info`, concrete `inspect` and
`doctor` are local-state reads. `search` is the deliberate discovery exception:
on an existing runtime it may perform one best-effort catalog refresh before
showing a frozen local snapshot. That refresh may replace the runtime `config/catalog.cfg`,
but it does not mutate installed packages/state/defaults.

`repair` is the public recovery command and mutates only where evidence yields a
safe result.

## Limits of the model

`cup` relies on HTTPS plus SHA-256 authenticated manifests/artifacts. It does not
currently introduce an independent package-signing key infrastructure or
transparency log. Local integrity metadata is not a signature against a malicious
administrator who can rewrite both managed data and the running process.

## Related documents

- [Architecture](ARCHITECTURE.md)
- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Platforms](PLATFORMS.md)
- [Releases](../development/RELEASES.md)
