# Releases

A CUP release is one tested source commit turned into five native candidates and
one immutable public generation. The release workflow does not rebuild or edit a
candidate after it has been tested: the bytes accepted by native release tests
are the bytes considered for publication.

## Release model

The release path is:

```text
source commit on main
        ↓
complete Tests workflow for that commit/attempt
        ↓
source-tested build identity for each platform
        ↓
five native official candidates
        ↓
native candidate tests
        ↓
one flat public release
```

The supported release platforms are:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

## Version and tag

`VERSION` contains the manually selected public version:

```text
MAJOR.MINOR.PATCH
```

The matching release tag is:

```text
vMAJOR.MINOR.PATCH
```

There is no automatic patch increment or nightly release channel. An official
release requires the version, tag and selected source commit to agree.

Development builds may include Git/archive identity in `cup --version`.
Official candidates expose exactly the public `VERSION` and record the full
source commit in release metadata.

## Source authorization

`.github/workflows/tests.yml` owns source qualification. Before an official
candidate can be built, Release resolves a successful Tests run for the exact
source commit and fixes both its run ID and run attempt.

Each native source-test job publishes its canonical `build-config.txt`. Release
downloads the file from that selected attempt and compares the relevant identity
with the official candidate build.

The comparison includes the properties that must remain stable across source and
release builds, such as:

- platform/toolchain identity;
- dependency-prefix format/profile/build revision;
- dependency source-lock/toolchain digests;
- compiler command, target and numeric version;
- Windows resource compiler identity where applicable.

Runner-specific paths and diagnostic version strings remain in `build-config.txt`
for diagnosis but are not treated as cross-runner equality keys.

If the selected Tests attempt does not contain its own expected source-build
identity, Release fails rather than borrowing one from another attempt.

## Release metadata

Every build generation includes `release.txt`:

```ini
format=1
version=X.Y.Z
commit=<full-source-commit>
```

Official releases require a real source commit. Development archive builds use
the reserved all-zero commit while identifying themselves as `dev+archive`.

The assembled public generation also contains `provenance.txt`:

```ini
format=4
version=X.Y.Z
source_repository=owner/repository
source_commit=<full-source-commit>
tests_run_id=<tests-workflow-run-id>
tests_run_attempt=<tests-workflow-attempt>
release_run_id=<release-workflow-run-id>
```

This connects published bytes to the source repository/commit, the exact Tests
attempt that authorized the release and the Release workflow that produced it.
A retry of jobs inside the same Release run does not change that public
provenance merely because the GitHub `run_attempt` number changed.

## Common assets

`scripts/release/common-assets.sh` creates the files shared by every platform:

```text
packages.cfg
install.cfg
release.txt
provenance.txt
THIRD_PARTY_NOTICES.txt
install.sh
install.ps1
SHA256SUMS.common
```

`packages.cfg` is the package catalog consumed by CUP. `install.cfg` carries the
host/target component selection policy, including defaults, profiles and curated
toolchains. These files cover the complete component model: compiler, debugger,
linker, formatter, linter, language server and analyzer where packages are
available.

The installers are stamped with the exact version, tag and source commit before
the common asset set is accepted.

## Native candidates

Each release-matrix job uses the verified dependency prefix for its platform,
builds an official release configuration and runs binary inspection.

`scripts/build/finalize-release.sh` prepares the platform bundle in staging,
separates native debug symbols, validates the finalized executable/metadata and
only then replaces the previous finalized directory.

The public platform contribution is:

```text
cup-<platform>[.exe]
SHA256SUMS.<platform>
```

Native symbol files (`cup.debug` or `cup.dSYM`) remain workflow artifacts for
diagnostics and are not public release assets.

`scripts/release/build-platform.sh` combines the finalized native output with the
common metadata required by native release tests. Generic flat merging is owned
by `scripts/release/assemble-candidate.sh`, which rejects collisions instead of
silently choosing one input.

GitHub artifact transport does not preserve all POSIX modes, so release assembly
restores the expected file modes before candidate testing and publication.

## Public asset set

A published release contains exactly:

```text
packages.cfg
install.cfg
release.txt
provenance.txt
THIRD_PARTY_NOTICES.txt
install.sh
install.ps1
cup-linux-x64
cup-linux-arm64
cup-macos-x64
cup-macos-arm64
cup-windows-x64.exe
SHA256SUMS.common
SHA256SUMS.linux-x64
SHA256SUMS.linux-arm64
SHA256SUMS.macos-x64
SHA256SUMS.macos-arm64
SHA256SUMS.windows-x64
```

The common checksum file covers the versioned installer/configuration inputs.
Each platform checksum file binds its binary to the common release metadata.
Publication validates both membership and values; unexpected files are not
silently accepted as part of the generation.

## Native candidate qualification

Candidate jobs test the assembled files on their matching native runners without
rebuilding or modifying the candidate.

The release suite checks the properties that matter specifically for published
bytes, including:

- exact file membership and checksums;
- release/provenance identity;
- executable version and startup;
- default and custom-root installation;
- relocation and reinstall;
- `cup doctor` on the installed generation;
- relevant repair/preservation behavior;
- uninstall.

A private newer-version fixture is built separately when self-update needs to
exercise a genuine version transition. The candidate under test is never patched
into a different version.

Source unit/integration/coverage/sanitizer tests are not repeated wholesale in
Release; the selected Tests workflow already owns them. Candidate-specific tests
remain mandatory because only they observe the official finalized bytes.

## Platform binary policy

Release inspection enforces the linkage/format rules documented in
[Platforms](../design/PLATFORMS.md):

- Linux release executables satisfy the static-runtime contract;
- macOS candidates contain statically linked third-party dependencies and only
  approved dynamic system libraries/frameworks, with no runtime search path;
- Windows candidates import only the approved system DLL set and contain the
  expected resource/mitigation properties.

Final public executables are stripped after symbol separation. Path-leak checks
apply to the publishable binary; rich source/debug metadata belongs in the
separate debug/symbol artifacts.

## Publication

`scripts/release/publish.sh` is the only script that mutates the remote GitHub
release. Before doing so it snapshots the completed local candidate and validates
that snapshot. Hashing, comparison and upload then use that immutable local copy.

Only the publication job receives `contents: write`; build/test jobs remain
read-only.

The publisher handles four states deliberately:

### No release exists

A draft is created for the tested commit, the exact snapshot is uploaded and the
remote asset set is downloaded/compared before publication.

### Matching draft exists

A draft is resumed only when its provenance identifies the same candidate
generation. Expected missing/stale assets may then be corrected.

### Release appears concurrently

Remote state is read again. A concurrently created generation is accepted only
when tag, asset set and bytes match the local snapshot.

### Release is already published

Published generations are read-only. Success means the existing remote assets
already match exactly; the publisher does not edit them.

Ambiguous network/API results are failures, not “not found”. An unrecognized or
ambiguous draft is preserved for inspection instead of being adopted or deleted.

## Workflow responsibilities

The release-related workflows have intentionally separate roles:

- **Dependencies** (`dependencies.yml`) builds or validates native pinned
  dependency prefixes. The normal release profiles are Linux GCC, macOS Apple
  Clang and Windows UCRT64; Windows CLANG64 is additionally used for sanitizer
  work.
- **Tests** (`tests.yml`) owns repository quality, native source tests, coverage,
  sanitizers and source-tested build identity.
- **Release** (`release.yml`) resolves one successful Tests attempt, builds the
  common assets and five official candidates, tests the candidates and publishes
  only after the complete matrix succeeds.
- **Docs** (`static.yml`) publishes documentation independently; Pages state does
  not authorize an application release.

Release uses a non-cancelling `cup-release` concurrency group. Candidate matrices
use `fail-fast: false` so one platform failure does not hide results from the
others, but publication still requires the complete successful generation.

## Manual release sequence

The intended operator sequence is:

```text
1. choose and update VERSION
2. review/commit/push the source on main
3. wait for Tests to succeed for that exact commit
4. dispatch Release for the same main commit
5. verify the selected Tests run/attempt
6. build and test all five official candidates
7. publish the verified generation
```

Changing release-relevant source requires a new successful Tests run. A candidate
is never reused for a different source commit.

## `cup update cup`

Official CUP builds can update themselves from the published generation. The
running program reads the public release metadata, verifies the selected
versioned assets and creates a byte-identical native helper copy of itself before
scheduling replacement.

The detached helper exists because the running executable cannot always replace
itself directly, especially on Windows. It follows the update journal/commit
model described in [Transactions](../design/TRANSACTIONS.md), replacing the main
executable last and leaving recoverable evidence when completion becomes
ambiguous.

The helper is operational transaction data, not another versioned public release
asset.

## Main release scripts

| Script | Responsibility |
|---|---|
| `scripts/build/finalize-release.sh` | Finalize and inspect one native platform bundle |
| `scripts/release/common-assets.sh` | Build the public files shared by every platform |
| `scripts/release/build-platform.sh` | Prepare one platform contribution/candidate input |
| `scripts/release/assemble-candidate.sh` | Merge validated release parts without collisions |
| `scripts/release/publish.sh` | Validate, compare and publish the exact generation |

## Related chapters

- [Build](BUILD.md)
- [Testing](TESTING.md)
- [Packages](../design/PACKAGES.md)
- [Transactions](../design/TRANSACTIONS.md)
- [Security](../design/SECURITY.md)
