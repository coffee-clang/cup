# Security model

cup downloads executables and changes a user-managed toolchain, so it treats
remote data, archive paths and existing filesystem objects as untrusted until
they have been checked.

This page explains the protections used by cup. It does not claim protection
against a user or process that already has full control of the same account and
can modify cup memory while it runs.

## Trust boundaries

cup trusts:

- its own running code and compiled registry;
- official HTTPS endpoints configured in the installed assets;
- release checksum files after their own checksum chain has been verified;
- filesystem objects only while their recorded identity still matches.

cup does not trust:

- package catalog values before parsing;
- URL path segments or response filenames;
- cached archives before hashing;
- archive entry names or types;
- a familiar directory name as proof of ownership;
- a pathname after another operation may have replaced the object behind it;
- CI artifacts unless their repository, commit, run and digest metadata match.

## HTTPS policy

Normal remote URLs and every redirect must use HTTPS.

Component archive and checksum URLs come from `packages.cfg`, but the parser
checks the scheme and required placeholders before a request is created. The
public installers apply the same transport rule, impose an overall timeout and
low-speed limit, and stop if a binary exceeds 256 MiB or a text asset exceeds
16 MiB.

The cup release base URL is fixed for official builds. Tests may use loopback
HTTP only when both conditions are true:

```text
explicit insecure-test flag
host is 127.0.0.1 or localhost
```

A non-loopback HTTP URL is rejected even when the test flag is present.

## Embedded certificate authority (CA) bundle

cup links libcurl statically and embeds a CA bundle generated from the repository
certificate input.

The build scripts:

1. read the source certificate bundle and metadata;
2. check size, text format and age policy;
3. generate the C header used by the application;
4. compare the generated output during repository tests;
5. reject stale or unexpected generated data.

The release binary therefore does not depend on a certificate file from the
build machine.

OpenSSL is configured without automatic configuration loading and without
runtime DSO modules. Windows uses Schannel instead of the bundled OpenSSL TLS
backend.

## SHA-256

Application SHA-256 uses the incremental implementation in `src/third_party/sha256.c`.
Checksum helpers accept an already opened stream where possible so callers can
keep the file identity fixed from digest calculation to later use.

Hash text must contain 64 lowercase hexadecimal characters.

Checksum files are parsed as data, not passed to a shell. cup checks:

- one expected filename per record;
- no duplicate expected names;
- no unknown entry in the accepted set;
- lowercase SHA-256 values;
- LF-terminated bounded text;
- a safe basename rather than an arbitrary path.

## cup installer checksum chain

An official installer verifies two checksum files:

```text
SHA256SUMS.common
SHA256SUMS.<platform>
```

The common file covers:

```text
packages.cfg
install.cfg
install.sh
install.ps1
```

The platform file covers:

```text
cup or cup.exe
release.txt
SHA256SUMS.common
```

Because the platform file also hashes the common file, files from two different
release generations cannot be mixed into one accepted install.

The hidden bootstrap repeats the relevant checks before changing the managed
root.

## Package downloads and cache

Package URLs are built from validated catalog values and locally validated
identity fields. The cache filename is constructed locally; a server cannot
choose a path through `Content-Disposition` or a URL basename.

Downloads have:

```text
connection timeout
overall timeout
low-speed timeout
maximum response size
interrupt checks
```

A cached archive is accepted only after its digest matches the release checksum
file.

The cache returns `VerifiedArtifact`, which owns the open archive stream. Archive
preflight and extraction use that same stream. This removes the gap where a
program verifies a pathname and later opens whatever file happens to be there.

A bad cached archive is removed only when the pathname still identifies the
same file object that was opened. cup then performs one fresh download. A second
failure is returned to the user.

## Archive preflight

Before extraction `package_archive` performs one complete bounded decoder pass
over the already-open verified stream. It checks:

- the detected compression/archive format matches the selected format;
- the entry-count limit is not exceeded;
- at least one non-directory payload entry exists;
- declared regular-file sizes stay within per-entry and total limits;
- decoded data blocks stay within their declared sizes and the total read limit;
- the complete archive stream can be decoded successfully.

Preflight does not extract files.

## Archive extraction

Extraction writes only inside a private staging directory created for the
selected package identity.

The extraction pass owns structural admission. It requires one safe top-level
directory, accepts only directories and regular files, rejects symbolic links,
hard links and special objects, validates the portable relative-path grammar,
detects ASCII case-fold and file/directory collisions, enforces path depth and
size/resource bounds, and creates files without following links. Existing
unexpected objects cause a failure instead of being reused. Size and format
checks are repeated here because this is a new decoder/read/write pass, not a
second validation of an unchanged in-memory result.

The extracted package is not installed immediately. cup first validates the
package root, the semantic identity in `info.txt` and every declared executable
entry. The package directory is moved to its installed path only after those
checks succeed.

## Filesystem identity

For managed trees cup records native file or directory identity and passes it to
later copy, move or removal operations.

This matters in a sequence such as:

```text
enumerate child
validate child
open or remove child
```

The final operation rechecks that it is still acting on the object seen during
enumeration. A replacement with the same name is not accepted automatically. These checks occur
immediately before the native mutation; they are not a kernel-level pathname compare-and-swap
against a hostile process controlling the same user account, which is outside the threat model
described above.

Native recursive operations never traverse links/reparse points, keep the identity of
observed entries and refuse to cross a device or volume boundary. Enumeration reports such
entries to policy callers; recursive removal may unlink the link entry itself without following it.

Uninstall detach and cleanup use the native C filesystem layer on every supported
platform. The copied helper receives continuous handoff authority, validates the
exact root and token-bound journal, performs an identity-bound namespace move,
and removes the detached tree without following links or reparse points or
crossing a filesystem/volume boundary.

Atomic publication inside cup uses native no-replace or replace operations. cup
does not simulate no-replace with a separate existence check followed by a
move.

## Root ownership

A valid `root.txt` is the normal proof that cup owns a directory.

A markerless cup-like root is never adopted automatically because familiar
filenames or a `state.txt` header do not prove ownership. Such a root is
preserved for explicit recovery or reinstallation. Recovery first moves the
unknown root outside the `.cup` / `.coffee-cup` candidate names, then installs a
clean current generation and restores only data accepted by current parsers.
`cup repair` does not make the ownership decision and `root.txt` must not be
manufactured manually.

A detached uninstall directory is recognized only when its reserved sibling name
and strict token-bound `detaching/detach` transaction journal agree. Managed
payload may already have been partially removed before a cleanup failure, so
`root.txt` and the executable are not required to survive together.

These checks reduce the risk of deleting an unrelated directory that happens to
look similar to cup state.

## Package metadata

`info.txt` is parsed with bounded file-size, line, key and value limits. Its identity
must match the component/tool/host/target/version path selected by the command.
Declared executable entries must remain inside the package root and point to
regular executable files.

Commands use one `ValidatedPackage` result instead of implementing separate
metadata rules.

## State and journal files

Persistent text files are read from one bounded snapshot. The snapshot records
the opened object's identity and contains all bytes used by parsing and hashing.

Writers create a sibling temporary file, set its mode, synchronize it and publish
it atomically. Replacement and deletion are tied to the expected destination
identity when an existing file is being advanced.

`transaction.txt` remains in place when recovery cannot decide safely. cup does
not clear transaction data only to make normal commands available again.

## `cup update cup`

cup downloads the installed release assets from one official release, verifies them
and stages them before writing the update journal.

The update helper is regenerated from the current executable before each update
and its digest is checked. The parent establishes continuous handoff authority
before it can release the canonical lock. After parent exit, the helper returns
to `cup.lock` under that authority and replaces the main executable last.

Before the commit marker, the old complete generation can be restored. Mixed or
incomplete rollback data is preserved for manual recovery instead of being
combined.

## Uninstall

Uninstall does not delete the running root in place. The parent creates a temporary
native helper outside the managed root and starts it while still holding the canonical
exclusive lock. Handoff authority is established before that lock can disappear. The
child waits for parent exit, validates the exact root and journal, publishes
`detaching/detach`, moves the root to its unique sibling and performs native no-follow
cleanup.

The temporary uninstall child removes its own reserved pathname before it can detach the root.
If that operation fails, no root mutation occurs and the canonical journal retains ownership of
the token-bound helper residue. `repair`, while holding canonical exclusive authority and only
after the active handoff has disappeared, removes that exact regular file by filesystem identity
before it can clear the stale journal.

On POSIX the handoff authority is the original flock open-file description. On
Windows it is a named per-user kernel object outside the managed root, allowing
`cup.lock` to close before the root is moved without admitting a competing process.

Cleanup preserves `transaction.txt` until every other managed entry is gone. If
cleanup fails while managed payload remains, that strict journal remains as ownership
evidence; `root.txt` and the executable may already have been removed. A later
installer does not automatically adopt or delete the detached sibling.

## CI and release data

Project-owned workflows use readable numeric GitHub Action version references. Version
changes are reviewed explicitly in workflow diffs, and the repository does not use an
automatic dependency-update bot. The separate Pages workflow belongs to the protected
website surface and does not participate in the cup build, test or release trust chain.

Checkout credentials are disabled in the cup build, test and release
workflows.

The Tests workflow produces source and dependency evidence containing:

```text
repository
commit
run ID
run attempt
artifact name or ID
platform/profile
compiler command, normalized target and numeric version
dependency source lock and toolchain fingerprint
hashes of the checked files
```

The release workflow selects one successful Tests run and verifies those fields
before using an artifact. Raw targets, compiler/resource-compiler paths and full
vendor strings remain diagnostic fields rather than cross-runner equality keys. Artifacts from
different run attempts are not mixed.

Candidate assembly accepts the complete expected asset set. Publication checks
tag/commit identity, draft provenance and existing asset bytes before making a
release public.

## Workflow permissions

Workflows declare permissions explicitly. `contents: write` is limited to the
release publication job. Other jobs use read access plus only the artifact,
cache, Pages or identity-token permissions they require.

The Pages workflow and files under `www/` are maintained separately from cup
implementation changes.

## Read-only behavior

These operations do not modify the managed root:

```text
help
--version
search
list
info
inspect
doctor
```

A read-only command does not acknowledge or remove a transaction file. `doctor`
reports the bytes and state it observes.

`repair` is the only public recovery command and changes files only when the
saved state gives one safe result.

## Current limits

The project currently relies on HTTPS and published SHA-256 files. It does not
implement a separate package-signing or transparency-log system.

This is a deliberate project limit, not a claim that checksums alone solve every
software-supply-chain problem.

## Related documents

- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Platforms](PLATFORMS.md)
- [Releases](../development/RELEASES.md)
