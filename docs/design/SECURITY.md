# Security

This document collects the integrity and resource-safety rules applied at the
release, installer and runtime boundaries. It does not claim protection against
a compromised GitHub account or compromised release signing infrastructure;
the current model uses HTTPS and published SHA-256 files.

## Trust boundaries

Validation occurs in three places:

```text
release pipeline
  protects the asset set before publication

CUP asset installer
  protects the initial executable and configuration before installation

cup runtime
  protects metadata, component packages and CUP-update assets before use
```

Some values are checked more than once. This is defense in depth at distinct
boundaries, not duplicate business logic. The publisher validates what it
publishes; each consumer validates what it receives.

## HTTPS policy

Runtime downloads use libcurl and require HTTPS. Redirects are followed only
when the resulting protocol remains HTTPS. Plain HTTP component or checksum URL
templates are rejected during catalog loading.

Transfers use:

- connection and total timeouts;
- a low-speed timeout;
- interrupt-aware progress callbacks;
- explicit response-size limits;
- exclusive temporary files;
- cleanup of partial files after failure.

Network, TLS, timeout and size-limit failures remain distinct `CupError` values.

## Embedded CA bundle

The versioned trust source is:

```text
certs/cacert.pem
```

Every configured build deterministically generates:

```text
ca_bundle.h
ca_bundle.c
```

under the selected build directory. When embedded-bundle support is enabled,
libcurl receives the in-memory CA data instead of a distribution-specific
certificate path.

This avoids embedding a build-machine path such as an OpenSSL configuration or
CA location into a static release. Linux and macOS static builds initialize
OpenSSL without loading external configuration. Windows static builds use the
selected Schannel-based libcurl stack.

The source PEM changes only through `make update-ca-bundle`. The update script
downloads, validates, generates and compiles a temporary representation before
replacing the versioned file. Release workflows never mutate the selected
commit.

## SHA-256 implementation

File hashing is implemented in-tree by `src/sha256.c`, adapted from Brad Conte's
public-domain `crypto-algorithms` implementation. OpenSSL is not used as a direct
checksum API.

SHA-256 is used because release assets publish `SHA256SUMS`. Policy remains
outside the hash primitive:

```text
checksum file parsing
duplicate filename rejection
expected asset selection
hash comparison
cache invalidation
release metadata consistency
```

A digest proves that bytes match the published checksum file. HTTPS is still
required to retrieve the checksum and asset from the intended release boundary.

## Checksum file parsing

Checksum readers reject:

- malformed digest length or characters;
- missing filenames;
- unsafe filenames;
- duplicate records for the selected asset;
- absent expected asset names;
- mismatching content.

Asset names are constructed from validated platform and package identities.
They are not accepted directly from arbitrary network input.

## Package downloads and cache

A package archive is usable only after:

```text
resolve immutable package/checksum URLs from the catalog
download or inspect the cache entry
read the matching SHA256SUMS record
hash the complete archive
compare the digest
```

A mismatching cached archive is removed. If a checksum-valid cached archive
fails extraction or package validation, it is discarded and fetched once from
the network. The retry distinguishes local cache corruption from a consistently
invalid published package.

Maximum download sizes are defined for metadata, CUP asset binaries and package
archives. The write callback refuses to exceed the selected limit even when a
server omits or lies about `Content-Length`.

## Archive preflight

The package archive domain is closed to `tar.xz`, `tar.gz` and `zip`.
`package_archive_is_valid` enables only the corresponding libarchive readers,
traverses the complete stream within the configured resource limits and compares
the detected format/filter stack with the catalog selection. Renaming a ZIP as
`tar.xz`, using an uncompressed TAR or relying on another format understood by
libarchive is rejected.

Preflight is not the complete security boundary; extraction validates every
entry again while creating files.

## Archive extraction

Libarchive is used directly. Extraction rejects:

- absolute POSIX paths;
- drive-qualified or UNC-style Windows paths;
- parent traversal;
- unsafe backslash forms;
- paths deeper than the configured limit;
- more than the configured number of entries;
- extracted content beyond the configured byte limit;
- a missing or non-directory top-level root;
- multiple unrelated top-level roots;
- exact duplicates, ASCII case collisions and file/directory collisions;
- Windows reserved names, trailing dots/spaces and non-ASCII internal names;
- device nodes, FIFOs, sockets and unsupported entry types;
- symbolic links that escape the package root;
- hard links to anything except an already extracted regular file.

Files are extracted below a fresh staging directory opened without following a
link. Privileged mode bits, ownership, timestamps, filesystem flags and unsafe
inherited metadata are discarded. Directories become `0755`; regular files
become `0644` or `0755` according to their executable bits. Final protected
metadata is made read-only through the platform abstraction.

No archive entry is written directly into the final package path.

## Path validation

Domain identifiers use safe single path segments. Relative package paths reject
empty segments, `.` and `..`, absolute roots and unsafe separators. Canonical
paths are always assembled locally from validated values.

Filesystem inspection does not follow links when deciding an object's type.
POSIX recursive removal is descriptor-relative (`openat`, `fstatat`, `unlinkat`)
and refuses linked parents. Windows uses wide APIs, explicit reparse-point
classification and long-path prefixes. This prevents a canonical package-level
link from redirecting validation or cleanup outside `.cup`.

The CUP root is private to its owner. POSIX enforces owner-only permissions;
Windows uses a protected DACL limited to the current user, Local System and
Administrators. `doctor` reports ownership or permission drift.

## Package metadata

`info.txt` is validated after extraction and whenever a package is used for a
default, inspection, diagnosis or repair. Required identity fields must match
the command/catalog identity and canonical path.

Declared package commands must be safe relative paths to regular executable
files within the package. Metadata is protected read-only after installation.
`cup` does not repair individual metadata fields; an invalid package is replaced
or quarantined as a whole.

## CUP assets

The installed CUP asset generation is checked against:

```text
SHA256SUMS.common
SHA256SUMS.<host>
```

`SHA256SUMS.common` contains exactly `packages.cfg`, `install.cfg`, `install.sh`
and `install.ps1`. The installers verify their delegated counterpart before
execution, while CUP consumes the configuration records required for its asset
generation. The platform checksum file
contains the executable, uninstall helper and `release.txt` records for that
release generation. The CUP asset inspection verifies the catalog, installation policy, executable
and uninstall helper against those sets; `release.txt` is consumed during
installer and CUP-update staging rather than retained as mutable local state.
`doctor` reports missing or altered files and permissions. An official `repair`
can refresh checksum files, the catalog and the uninstall helper from its own
immutable release. On POSIX it can also restore the canonical executable; on
Windows a missing or altered running executable must be replaced by the
official installer. A development build cannot restore official CUP assets
assets.

## CUP-update assets

The `latest` alias is used only to discover `release.txt`. After a concrete
version is known, every replacement file comes from the immutable `vX.Y.Z`
release URL.

The latest and versioned metadata must agree. The platform set verifies the
executable, uninstall helper and release metadata. The common set verifies
`packages.cfg` and `install.cfg`. Both text assets are parsed before staging is
accepted, so checksum-valid but structurally invalid policy cannot become the
active generation.

The detached helper replaces all six persistent CUP assets as one
recoverable generation. A mixed or partially published set is rejected before
a journal is created. The transactional replacement is described in
[TRANSACTIONS](TRANSACTIONS.md#cup-update-transaction).

## Release publication

The release pipeline verifies:

- version, tag and commit agreement;
- the exact expected asset set;
- checksum files with one record per expected asset;
- native execution of each platform candidate;
- installer consumption of the candidate assets;
- the bytes uploaded to a draft or existing release.

A resumed draft is accepted only when it belongs to the expected tag/commit;
unexpected or partial assets are replaced and the final remote set is compared
with the verified candidate before publication.

`THIRD_PARTY_NOTICES.txt` accompanies the release with the notices and license
texts for the pinned build. The copy maintained beside the dependency source
lock also identifies the corresponding archives; integrity still comes from the
tested candidate, published checksums and exact remote byte comparison.

See [RELEASES](../development/RELEASES.md).

## Read-only protection

Read-only attributes protect against accidental local modification of:

```text
packages.cfg
SHA256SUMS files
uninstall helper
installed package info.txt
```

They are not treated as a cryptographic trust mechanism. Integrity still comes
from published checksums and package validation.

## Current boundary

The project currently does not implement a separate signature or key-rotation
infrastructure beyond GitHub HTTPS delivery and published SHA-256 files. Adding
signatures would require a documented trust root, key distribution, rotation,
revocation and recovery model; it should not be represented as a small checksum
extension.

## Implementation and verification

Security-boundary ownership is listed in [ARCHITECTURE](ARCHITECTURE.md).
Runtime, integration and release-contract verification are described in
[TESTING](../development/TESTING.md).

## Related documents

- [PACKAGES](PACKAGES.md) — catalog and metadata contract;
- [TRANSACTIONS](TRANSACTIONS.md) — staged commit and recovery;
- [BUILD](../development/BUILD.md) — CA generation and linked libraries;
- [RELEASES](../development/RELEASES.md) — publication gates.
