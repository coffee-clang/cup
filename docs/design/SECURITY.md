# Security model

CUP downloads executable packages and mutates a user-managed toolchain root. Its
security model therefore concentrates on four boundaries: remote transport,
package admission, filesystem identity and release provenance.

It does **not** claim to defend against a process that already has arbitrary
control of the same user account and can alter CUP's memory while it runs.

## Trust boundaries

CUP starts from these trusted inputs:

- the running CUP code and its compiled component/tool/platform domain;
- official endpoint/configuration data only after the installed release assets
  that contain it have passed their checksum chain;
- a filesystem object only while the native identity retained by the operation
  still refers to that object;
- source-build identity only when it comes from the selected successful Tests run
  attempt and matches the official candidate's build identity.

Everything else is parsed/validated before use. In particular CUP does not trust
catalog values, response filenames, cached archives, archive paths/types,
familiar directory names or a pathname that may now refer to another object.

## HTTPS and TLS

Normal product downloads and redirects require HTTPS. Package/archive URLs come
from `packages.cfg`, but the catalog parser validates the scheme and template
placeholders before a request is built.

Network operations are bounded by connection/overall/low-speed limits, response
size limits and interrupt checks. Product code permits insecure HTTP only for the
explicit loopback test mode and only under its restricted loopback host/port
rules; this path is not usable for normal remote package URLs.

The public installers implement the same normal HTTPS requirement before CUP is
installed and keep their own explicit loopback-only test override.

### CA bundle and TLS backends

POSIX builds use OpenSSL with an embedded tracked CA bundle. The certificate
inputs in `certs/` are validated and converted to generated C data during the
build; the runtime does not load a CA file from the build machine.

The pinned OpenSSL build disables automatic configuration loading and runtime DSO
modules. Windows uses Schannel rather than OpenSSL as libcurl's TLS backend.

## SHA-256 and checksum documents

CUP uses the incremental SHA-256 implementation in
`src/third_party/sha256.c`. Higher-level checksum policy stays in
`checksum.c`/the package and release consumers.

Checksum documents are parsed as data. Accepted records require safe expected
filenames, canonical lowercase 64-hex digests, no duplicate expected name and no
unknown member in the exact set being validated.

Where possible, digest calculation operates on an already opened regular-file
stream so later consumers can continue with the same object instead of reopening
a pathname.

## CUP release checksum chain

Official bootstrap/self-update uses two checksum documents:

```text
SHA256SUMS.common
SHA256SUMS.<platform>
```

The common checksum covers:

```text
packages.cfg
install.cfg
install.sh
install.ps1
```

The platform checksum covers:

```text
cup or cup.exe
release.txt
SHA256SUMS.common
```

Because the platform checksum authenticates the exact common checksum document,
assets from different CUP release generations cannot be mixed into one accepted
installation.

The public installer validates this chain before invoking bootstrap; hidden
bootstrap validates the relevant generation again before changing the managed
root.

## Package download and cache

Package URLs are expanded from validated catalog templates and locally validated
identity values. The local cache path is constructed by CUP; remote response
names never choose a filesystem destination.

A cached archive is reusable only after its digest matches the package
`SHA256SUMS`. The resulting `VerifiedArtifact` owns the open file used for digest
calculation and archive extraction.

If a bad cached artifact must be removed, CUP first proves that the cache path
still refers to the object it opened. It then performs at most one fresh network
attempt; repeated invalid data is surfaced as a failure rather than retried
without bound.

## Archive admission

A verified digest is not sufficient to make an archive safe to extract.
Libarchive is configured for CUP's supported formats, then the extraction pass
checks structure while consuming the already verified stream.

Before writing an entry CUP enforces, among other limits:

- one safe top-level package root;
- portable relative path grammar and depth/resource bounds;
- no case-fold/file-directory aliases;
- no hard-link or special-object archive entries;
- creation beneath fresh private staging rather than reuse of unexpected objects.

POSIX package archives may contain confined relative symbolic links. During
construction a link cannot escape lexically or become the parent of a later
write. Final manifest validation then proves that each link resolves physically
inside the package to a regular file. Windows package content rejects symbolic
links/reparse objects.

Archive admission and package integrity are separate boundaries. After extraction
CUP validates `info.txt`, executable entries and the complete staged tree against
`manifest.txt` before canonical publication.

See [Packages](PACKAGES.md) for the full package format.

## Package metadata and manifest

`info.txt` is bounded line-based metadata. Consumer-owned identity fields must
match the selected component/tool/host/target/version and the canonical package
path. Declared entry paths must remain within the package and resolve to
executable regular files.

Producer-owned `features.*`, `contents.*`, `bundle.*`, `requires.*` and
`config.*` fields remain descriptive; CUP does not invent a second tool-specific
validation layer for them.

`manifest.txt` `format=2` is the package-byte integrity inventory. CUP verifies
every regular-file digest/mode, every directory and the exact target text of POSIX
links, rejects undeclared objects and verifies final link confinement. Raw hard
links are outside the consumer contract.

Full manifest hashing is used when package integrity is being admitted or
diagnosed (installation/scanning/`doctor`/repair). Lightweight metadata queries
do not re-hash entire installed toolchains.

## Filesystem identity and path mutation

When a later destructive operation relies on an earlier observation, CUP retains
native identity and revalidates the object at the mutation boundary. A different
object that appears under the same pathname is not automatically accepted.

Typical shape:

```text
enumerate/open object
validate object and retain identity
...
reopen/mutate only if identity still matches
```

Native recursive cleanup never follows links/reparse points and refuses to cross
the starting device/volume boundary. No-replace publication uses a real native
primitive rather than “check that destination is absent, then rename”.

These mechanisms reduce accidental/racing mutation of unrelated objects within
the supported threat model. They are not a kernel-level pathname compare-and-
swap guarantee against a hostile process with complete control of the same user.

## Root ownership

`root.txt` is the normal proof that CUP owns a managed root. A directory is not
adopted merely because it is named `.cup` or contains recognizable state files.
Markerless CUP-like roots are preserved for explicit recovery/reinstallation.

Detached uninstall residue has a different ownership proof because ordinary root
files may already have been removed: the reserved token-bound sibling name and a
strict matching uninstall transaction must agree. Installers do not adopt or
remove such detached directories automatically.

## Persistent state and transactions

Persistent state/journal/config text is read from one bounded regular-file
snapshot. Atomic writers publish through sibling temporary files and tie
replacement/deletion to the expected existing identity where applicable.

`transaction.txt` is deliberately preserved when recovery is ambiguous. CUP does
not discard recovery evidence only to make subsequent commands runnable.

Self-update and uninstall carry exclusive mutation ownership from parent to a
native helper before the parent releases its normal lock. The helper validates
the root/token/journal before mutation. See [Transactions](TRANSACTIONS.md) for
commit ordering and [Platforms](PLATFORMS.md) for POSIX/Windows handoff details.

## Self-update and uninstall

For `cup update cup`, all five installed generation assets are downloaded and
verified before the journal is scheduled. The helper records complete rollback
evidence, commits support assets, writes the commit marker and replaces the main
executable last.

Uninstall does not recursively delete the running root in place. A copied native
helper validates and detaches the exact root, then performs no-follow cleanup.
Temporary-helper deletion is independently bound to that helper executable:
POSIX can unlink the verified running path; Windows uses an exact
`DELETE_ON_CLOSE` handle kept alive until helper-process termination.

The Windows lifetime carrier has no root path, transaction token or mutation
authority, so it cannot become a second implementation of uninstall.

## Build and release provenance

CUP's CI/release chain separates source verification from candidate publication:

1. dependencies are restored/built and verified against the canonical prefix
   metadata/source/toolchain identity;
2. Tests performs repository checks, native source tests, coverage and
   sanitizers;
3. each release platform publishes its source-tested `build-config.txt` for that
   exact Tests run attempt;
4. Release selects one successful Tests run for the source commit and verifies
   candidate build identity against those source-tested configs;
5. each official candidate is tested natively without rebuilding it;
6. publication validates the complete expected asset set/checksums/provenance and
   remote tag/release state before making a release public.

Resolved runner paths and harmless vendor wording are diagnostic data, not
cross-runner equality keys. Dependency prefix format/profile/build revision,
source lock, toolchain identity and compiler identity are equality keys; Windows
also verifies the resource compiler.

Only the publication job receives release write permission. Website/Pages
workflow data is outside the application release authorization chain.

## Read-only operations

These public operations do not modify the managed root:

```text
help / help options / --version
search
list
info
inspect
doctor
```

`doctor` may inspect a pending transaction but never acknowledges it. `repair`
is the public recovery command and mutates only where the recorded state yields a
safe result.

## Limits of the model

CUP currently relies on HTTPS plus published SHA-256 checksum chains. It does not
implement a separate package-signing key infrastructure or transparency log.

Checksums, identity checks and recovery ordering are protections within the
project's stated trust model; they are not presented as a complete answer to all
software supply-chain threats.

## Related documents

- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Platforms](PLATFORMS.md)
- [Releases](../development/RELEASES.md)
