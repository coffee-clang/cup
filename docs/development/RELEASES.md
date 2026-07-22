# Releases

This document defines official version identity, construction of one tested
candidate generation and manual publication to the public release repository.

## Version model

The root `VERSION` file is the only manually maintained official version input:

```text
X.Y.Z
```

`scripts/version.sh` validates it and generates build metadata. An official
release build requires all of:

```text
controlled workflow official-build flag
clean working tree
HEAD exactly tagged vX.Y.Z
tag version equal to VERSION
`make release` configuration
```

`make release` selects optimized release code but does not establish official
identity by itself.

Development Git builds use:

```text
X.Y.Z-dev.<distance>+<short-commit>
X.Y.Z-dev.<distance>+<short-commit>.dirty
```

A source archive without Git metadata uses `X.Y.Z-dev+archive`. There is no
automatic patch increment or nightly channel.

## Generated and candidate metadata

The build generates `version.h`, `release.txt` and, on Windows, `version.rc`.
The public `release.txt` schema is:

```text
format=1
version=X.Y.Z
commit=<full source SHA>
```

The Tests run also produces internal metadata:

```text
release-decision.env
  SHOULD_RELEASE
  VERSION
  TAG
  SHA

candidate.env
  VERSION
  TAG
  SHA
```

`release-decision.env` is required even when no new candidate is built. It lets
the manual release workflow distinguish an already published version from a
complete candidate and binds that decision to the tested source SHA.
`candidate.env` exists only for a complete candidate. Neither environment file
is a public release asset. `provenance.txt` is generated under `dist/` during the
Tests workflow and published with the candidate; it is not tracked in the source
repository. It binds the candidate to the successful source workflow that
produced it:

```ini
format=1
version=X.Y.Z
source_repository=owner/repository
source_commit=<40-hex-source-commit>
tests_run_id=<workflow-run-id>
tests_run_attempt=<workflow-run-attempt>
```

## Workflow separation

Application workflows are exactly:

```text
.github/workflows/tests.yml
.github/workflows/debug.yml
.github/workflows/release.yml
```

`tests.yml` owns source verification and release candidates, `debug.yml` owns
non-publishable diagnostics, and `release.yml` publishes already tested bytes.
`static.yml` belongs to the documentation site and remains separate.

### Tests

`tests.yml` runs on pushes to `main` and through manual dispatch. It performs all
source validation plus native coverage and sanitizer matrices. For a new or
draft public version it also builds release candidates for:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Common assets and each platform artifact are uploaded to the same Tests run.
Every candidate executable is then downloaded and executed on its native runner.
The final gate succeeds only when all jobs required by the release decision
succeed.

A manual Tests run outside `main` is useful for source verification but cannot
produce release artifacts. When the public version is already published,
`SHOULD_RELEASE=0`; source gates still run, while candidate build and native
candidate jobs are skipped.

### Release

`release.yml` is manual only. It must be dispatched from `main` and:

1. reads `VERSION` and the exact `HEAD` SHA;
2. resolves a successful workflow named `Tests` for that same SHA and branch;
3. optionally validates an explicitly supplied `tests_run_id`;
4. downloads only `cup-release-*` artifacts from that run;
5. validates decision metadata, candidate metadata, release metadata and every
   checksum file;
6. publishes or resumes the verified generation in `coffee-clang/cup`.

The release workflow does not rebuild or rerun tests. The bytes downloaded from
the successful Tests run are the only bytes eligible for publication.

## Tests-run provenance

`scripts/release/resolve-tests-run.sh` rejects a run when any of these differ
from the selected release revision:

```text
workflow name
workflow file
branch
source SHA
completion state
conclusion
allowed event
```

Only successful `push` or `workflow_dispatch` runs of `Tests` on `main` are
accepted. This prevents a successful run for another commit or workflow from
being reused.

## Public repository boundary

Source and public downloads live in different repositories. The commit object
behind a public release tag therefore cannot be used as the identity of the
private source revision. The full source SHA is carried and validated through
the Tests run, `release-decision.env`, `candidate.env` and `release.txt`.

The source-test workflow uses its read-only `github.token` to inspect public
release state. `PUBLIC_RELEASE_TOKEN` is used only by the manual publication
workflow to publish releases in `coffee-clang/cup`. The automatic token of the
source repository is used only
to read its workflow runs and artifacts.

## Release assets

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

`THIRD_PARTY_NOTICES.txt` publishes the notices and license texts maintained
beside the dependency source lock. The package catalog points to component
archives published by `cup-components`; those tool packages are not embedded in
the `cup` release.

## Candidate validation

`scripts/release/candidate-info.sh` first validates the exact decision schema.
For `SHOULD_RELEASE=1` it additionally requires the complete asset set,
`candidate.env`, the three-line `release.txt` schema, the exact
`provenance.txt` schema and checksum-file membership. Native candidate tests
verify executable startup, version identity,
package/state workflows and installer behavior against the local candidate
asset set.

## Publication and recovery

`scripts/release/publish.sh` is resumable and idempotent:

- an existing compatible draft can be resumed;
- unexpected draft assets are removed;
- candidate assets are uploaded with replacement semantics;
- a conflicting tag or incompatible release is rejected;
- an already published release is accepted only when the exact asset set and
  downloaded bytes match the verified candidate;
- the draft is published only after remote assets are downloaded and compared.

Internal files such as `release-decision.env` and `candidate.env` are mandatory
inputs but are never uploaded as public assets.

## Concurrency

Tests runs use a ref-specific concurrency group and may cancel an older run for
the same ref. Release publication uses one non-cancelling `cup-release` group,
so only one manual publication mutates the public release state at a time.
External GitHub Actions use explicit major-version tags such as `@v6` and
`@v7`. The repository does not use Dependabot; Action major versions and pinned
C dependency versions are updated deliberately and reviewed as ordinary source
changes.

## CUP-update relationship

`cup update cup` is available only in official builds. It uses
`latest/release.txt` for discovery, then downloads immutable `vX.Y.Z` assets.
Platform checksums cover the executable, uninstall helper and release metadata;
`SHA256SUMS.common` covers `packages.cfg` and `install.cfg`. Development
identities cannot establish an official installed generation and are rejected.

## Release procedure

```text
update VERSION manually
complete and review the source revision
commit and push main
wait for Tests to succeed for that exact SHA
confirm candidate and native jobs succeeded when SHOULD_RELEASE=1
dispatch Release from the same main SHA
optionally provide the successful Tests run id
publish the already tested candidate
```

Reusing a version that is already public intentionally produces source
validation without rebuilding or replacing that release.

## Linked-binary policy

Every candidate build runs the repository binary inspector after the official
release link. Linux candidates must be static ELF executables without an
interpreter, dynamic dependency or runtime search path. macOS candidates may
link dynamically only to Apple system libraries and public frameworks, must
match the requested architecture, must declare a minimum OS version and must
not contain `LC_RPATH`. Windows candidates must be PE32+ x86-64 console
executables importing only Windows system DLLs, with a non-empty resource
directory plus ASLR and NX compatibility flags.

The generated `binary-inspection.txt` remains in the configuration build
directory and is also included with diagnostic artifacts. Before candidate
assembly, native symbols are split into `cup.debug`/`cup.dSYM`, the public
executable is stripped, and a path-leak guard rejects checkout, dependency-root
and transactional staging paths. Symbol artifacts are retained by the Tests run
for diagnostics but are not part of the public release asset set. Candidate
publication continues to use the already tested executable bytes rather than
rebuilding.

## Related documents

- [BUILD](BUILD.md) — static dependencies and generated files;
- [TESTING](TESTING.md) — verification ownership;
- [SECURITY](../design/SECURITY.md) — asset integrity;
- [INSTALLATION](../user/INSTALLATION.md) — installer consumption.
