# Releases

This document defines version identity, candidate construction, native release
verification and publication for `cup`.

## Version model

`VERSION` contains the manually selected public base version:

```text
MAJOR.MINOR.PATCH
```

Official release builds use this exact version. Development builds derive an
additional Git identity from the nearest tag, commit distance, short commit and
dirty state. No script increments the public version automatically.

The manual release workflow validates that:

- it was dispatched from `main`;
- `VERSION` is syntactically valid;
- generated metadata refers to the selected source commit;
- the official build tree is clean and uses controlled flags.

## Public and internal metadata

Every platform build generates `version.h`, `release.txt` and, on Windows,
`version.rc`. The public `release.txt` schema is:

```text
format=1
version=X.Y.Z
commit=<full source SHA>
```

The same release run creates `candidate.env` for internal coordination:

```text
VERSION=X.Y.Z
TAG=vX.Y.Z
SHA=<full source SHA>
```

`candidate.env` is required by the publication script but is not uploaded as a
public release asset.

`provenance.txt` is public and binds the asset generation to the source and the
manual release run that produced and tested it:

```ini
format=1
version=X.Y.Z
source_repository=owner/repository
source_commit=<40-hex-source-commit>
release_run_id=<workflow-run-id>
release_run_attempt=<workflow-run-attempt>
```

The source repository and public download repository may be different. The
public repository tag is therefore not used as the identity of the private
source revision; `release.txt`, `candidate.env` and `provenance.txt` carry that
identity explicitly.

## Workflow responsibilities

Application automation is separated by purpose:

```text
dependencies.yml  build or restore pinned dependency prefixes
tests.yml         verify source behavior and repository quality
debug.yml         produce non-publishable diagnostic artifacts
release.yml       build, test and publish one official candidate generation
```

The documentation workflow remains independent.

### Dependency preparation

The release path uses the same reusable dependency workflow and cache keys as
normal source testing. A cache miss rebuilds the prefix through `make deps`; no
release step depends on a cache being present.

### Source gates

`release.yml` calls `.github/workflows/tests.yml` as a reusable workflow for the
selected `main` revision. The release cannot assemble candidates until all
source, quality, coverage and sanitizer gates succeed.

The reusable call belongs to the same release execution. There is no search for
an earlier workflow run, no run-ID input and no cross-run candidate trust.

### Candidate construction

After the source gates, the release workflow builds official candidates for:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Common assets are assembled once. Each platform build produces its executable,
platform checksum file, native symbol artifact and release-test helpers. Public
candidate parts are uploaded as workflow artifacts so later jobs in the same run
consume the exact bytes built by the owner runner.

### Native candidate verification

The release workflow downloads the common and platform artifacts onto native
runners. Each candidate is tested without rebuilding. The Windows candidate is
verified by the Windows PowerShell suite; Linux and macOS candidates use the
POSIX release runner.

Publication depends on every native candidate job. This establishes that the
published bytes, rather than a similar local rebuild, passed the release tests.

## Public assets

Published assets are:

```text
cup-linux-x64
cup-linux-arm64
cup-macos-x64
cup-macos-arm64
cup-windows-x64.exe
packages.cfg
install.cfg
install.sh
install.ps1
uninstall.sh
uninstall.ps1
release.txt
provenance.txt
THIRD_PARTY_NOTICES.txt
SHA256SUMS.common
SHA256SUMS.linux-x64
SHA256SUMS.linux-arm64
SHA256SUMS.macos-x64
SHA256SUMS.macos-arm64
SHA256SUMS.windows-x64
```

`THIRD_PARTY_NOTICES.txt` contains the notices and license texts corresponding
to the pinned CUP dependency graph. Component tool packages remain owned by the
separate `cup-components` project and are not embedded in a CUP release.

## Candidate validation

Before publication, `scripts/release/publish.sh` requires:

- the exact three-key `candidate.env` schema;
- the exact three-line `release.txt` schema;
- the exact six-line `provenance.txt` schema;
- every required public asset and no missing checksum member;
- installer metadata matching version, tag and source commit;
- checksum files whose membership exactly matches their contract.

Internal metadata is validated but excluded from the public asset allowlist.

## Linked-binary policy

Every official build runs `make check-binary` before candidate assembly.

- Linux candidates must be static ELF executables without an interpreter,
  dynamic dependency or runtime search path.
- macOS candidates may link only to approved Apple system libraries and
  frameworks, must match the selected architecture and deployment target and
  must not contain `LC_RPATH`.
- Windows candidates must be PE32+ x86-64 console executables importing only
  allowlisted Windows system DLLs and carrying the expected resource and
  mitigation flags.

Native symbols are split into diagnostic artifacts, the public executable is
stripped and path-leak checks reject checkout, dependency-root and transactional
staging paths. Symbol artifacts remain attached to the workflow run but are not
published as release downloads.

## Publication and recovery

`scripts/release/publish.sh` is resumable and idempotent:

- an existing compatible draft can be resumed;
- unexpected draft assets are removed;
- expected assets are uploaded with replacement semantics;
- public tag and release state are queried without treating network errors as
  an absent release;
- an already published release is accepted only when its exact asset set and
  downloaded bytes match the verified candidate;
- a draft is published only after remote assets have been downloaded and
  compared byte-for-byte.

The publication job uses `PUBLIC_RELEASE_TOKEN` only after source and native
candidate gates succeed. The automatic source-repository token remains
read-only.

## Concurrency

Tests use ref-specific cancellable concurrency. Release publication uses one
non-cancelling `cup-release` group so two manual runs cannot mutate public
release state simultaneously. Candidate build and native-test matrices use
`fail-fast: false` so all independent platform outcomes remain visible.

## Release procedure

```text
update VERSION manually
review and commit the source revision
push the revision to main
use the normal Tests workflow for earlier feedback when desired
dispatch Release from that main revision
allow the release run to execute its reusable source gates
build candidates for every supported platform
test the exact candidate artifacts on native runners
publish the verified generation
```

The manual Release run is self-contained. The normal Tests workflow can provide
earlier feedback, while publication depends only on gates and artifacts from the
release run itself.

## CUP update relationship

`cup update cup` is available only in official builds. It discovers the latest
public version through `latest/release.txt`, then downloads immutable versioned
assets. Platform checksum files cover the executable, uninstall helper and
release metadata; `SHA256SUMS.common` covers the shared configuration assets.
Development identities cannot establish an official installed generation.

## Related documents

- [BUILD](BUILD.md) — dependency compatibility and official build configuration;
- [TESTING](TESTING.md) — source and native candidate verification;
- [SECURITY](../design/SECURITY.md) — checksums, downloads and asset integrity;
- [PLATFORMS](../design/PLATFORMS.md) — platform linkage policy.
