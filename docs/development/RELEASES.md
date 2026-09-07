# Releases

A cup release is built from one reviewed commit on `main`. The workflow first
checks that the matching source tests succeeded, then builds one candidate for
each supported platform, tests those exact files on native runners and finally
publishes the common generation.

The important rule is that publication never rebuilds or edits a candidate.
The files tested by the release jobs are the files compared and uploaded by the
publisher.

## Version numbers

`VERSION` contains the public version:

```text
MAJOR.MINOR.PATCH
```

The value is changed manually. There is no automatic patch increment and there
is no nightly channel.

A local development build adds Git information to the version shown by the
program. An official build uses the exact value from `VERSION` and also records
the full source commit.

The release tag is:

```text
vMAJOR.MINOR.PATCH
```

Before building, the workflow checks that the version, tag and selected commit
agree and that the workflow was dispatched from `main`.

## Release metadata

Each platform build generates `release.txt`:

```ini
format=1
version=X.Y.Z
commit=<full-source-commit>
```

This is an exact three-line physical schema: comments, blank lines, duplicate
records and trailing records are invalid.

A development build generated from a source archive has no Git object to record.
It keeps the same fixed schema and uses forty zeroes as the reserved commit
sentinel. `cup --version` still identifies that build as `dev+archive`. Official
release builds require a Git checkout and always record the real full commit.

The assembled release also contains `provenance.txt`:

```ini
format=4
version=X.Y.Z
source_repository=owner/repository
source_commit=<full-source-commit>
tests_run_id=<tests-workflow-run-id>
tests_run_attempt=<tests-workflow-attempt>
release_run_id=<release-workflow-run-id>
```

This file connects the public files to:

- the repository and source commit;
- the successful Tests run selected for that commit;
- the exact Tests rerun attempt that supplied the source-tested build identity;
- the Release run that built and tested the candidates.

The Tests attempt is part of the cross-workflow authorization. The Release
attempt deliberately is not: retrying jobs inside the same Release run must not
change the common release bytes merely because `github.run_attempt` increased.
Internal Release artifacts therefore use stable names within that run and are
uploaded with explicit overwrite semantics when an upstream job is rerun.

## Workflow responsibilities

The three application workflows involved in a release have separate jobs.

### Dependencies

`.github/workflows/dependencies.yml` prepares or restores a dependency prefix for
one native platform/profile. Prefix compatibility is determined by the canonical
metadata and verifier rather than by a separate cross-workflow artifact.
The `primary` target selects the five profiles used for distributable CUP
artifacts; `all` additionally includes Windows CLANG64 for test/sanitizer work.

The normal profiles are:

```text
linux-x64-gcc
linux-arm64-gcc
macos-x64-apple-clang
macos-arm64-apple-clang
windows-x64-ucrt64
windows-x64-clang64
```

The CLANG64 Windows prefix is used for sanitizers. The official Windows binary
uses UCRT64 GCC.

### Tests

`.github/workflows/tests.yml` owns source verification. It runs repository
quality, native source tests, coverage and sanitizers. Each successful source job
also uploads the canonical source-tested `build-config.txt` for its release
platform under a name bound to the current Tests run attempt. The final Tests
gate requires every release-authorizing job family to succeed.

### Release

`.github/workflows/release.yml` is manually dispatched. It:

1. selects the exact commit from `main`;
2. finds a successful Tests run for that commit;
3. fixes the selected Tests run ID and run attempt;
4. downloads that attempt's source-tested build config for each release platform;
5. builds common assets and five platform candidates from verified dependency prefixes;
6. compares each candidate build identity with the corresponding source-tested build;
7. tests each candidate on its native runner;
8. publishes only after every required job succeeds.

Repository quality, coverage and sanitizers are not repeated inside Release;
the selected successful Tests run already owns those results. Candidate-specific
build-identity and native tests remain in Release because they must examine the
actual official files.

The protected Pages workflow is unrelated to this process. Website deployment
does not authorize a cup release and is not included in the release gate.

## Source-tested build identity

The cross-workflow artifact is the canonical `build-config.txt` produced by the
source build itself; there is no second metadata envelope. Release accepts only
the artifact name for the selected Tests run attempt and then independently
validates the source-development config against the candidate-release config.

Both files must have the exact current build-config schema and the expected
platform, configuration and official-build role. Cross-runner equality is
required for:

- dependency prefix format, profile and build revision;
- dependency source-lock and toolchain SHA-256 identities;
- compiler command, normalized target and numeric version;
- on Windows, the corresponding resource-compiler command, normalized target
  and numeric version.

Resolved paths, full vendor version lines and effective flags stay in
`build-config.txt` for diagnosis, but are not equality keys because native runner
locations and harmless vendor wording may differ. A source artifact from another
Tests attempt is not substituted when the selected attempt lacks its own file;
that condition fails closed.

## Building common assets

Common assets are created once for the whole release by:

```text
scripts/release/common-assets.sh
```

They include the package and installer configuration, public installers,
release metadata, provenance, notices and common checksum file. Their values are
built from the selected version, tag, source commit and workflow identities.

The output is written below the managed build root and is not published directly.
It is one input to candidate assembly.

## Building platform candidates

The release matrix contains:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Each native job uses the verified dependency prefix for its platform, builds an
official release configuration and runs binary inspection. The platform output
contains:

- the native executable;
- platform-specific checksum data;
- native debug symbols (`cup.debug` on Linux/Windows and an UUID-matched,
  `dwarfdump --verify`-validated `cup.dSYM` on macOS);
- build and release metadata;
- the files needed by the native release test.

`scripts/build/finalize-release.sh` creates this platform bundle in a private
sibling staging directory. It performs the late checks before replacing the
previous finalized directory. A failed inspection or metadata step removes the
staging directory and leaves the previous complete bundle unchanged.

`scripts/release/build-platform.sh` combines the finalized platform output with
the verified common assets. `scripts/release/assemble-candidate.sh` is the generic
collision-safe flat merger used for common/platform parts; candidate-specific
consumers such as native release tests and `publish.sh` enforce the exact public
asset set.

GitHub artifact transport does not preserve POSIX modes, so assembly restores
them before testing and publication:

- directories: `0755`;
- POSIX executables and shell entry points: `0755`;
- other public files: `0644`.

## Public file set

A complete public release contains:

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

The checksum split is intentional:

- `SHA256SUMS.common` covers the shared installer/configuration inputs
  (`packages.cfg`, `install.cfg`, `install.sh` and `install.ps1`);
- each platform checksum covers its executable, `release.txt` and the exact
  common checksum file.

This lets installers and `cup update cup` verify both the shared generation and
the platform-specific files without trusting two unrelated manifests.

Component compiler/debugger/linter/linker packages are not part of this release.
They are built and published by the separate `cup-components` project.

## Binary requirements

Every official platform build runs binary inspection before assembly.

### Linux

The public ELF executable must be fully static. It must not contain:

- an ELF interpreter;
- `DT_NEEDED` entries;
- `RPATH` or `RUNPATH`.

### macOS

Pinned third-party libraries are static. The Mach-O executable may reference
only approved Apple libraries and frameworks, must match the requested
architecture and deployment target, and must not contain `LC_RPATH` or Homebrew
paths.

### Windows

The executable must be PE32+ x86-64, use the console subsystem, import only the
approved Windows system DLLs and contain the expected version resource and
mitigation flags. MinGW runtime DLL dependencies are rejected. Release linking
disables the PE insertion timestamp, and Windows finalization forces
`SOURCE_DATE_EPOCH=1` for the `objcopy`/`strip` writers so an ambient clock or
caller epoch does not own the final PE/debug bytes. Native reproducibility tests
still compare independent builds rather than treating these flags alone as proof.

Native symbols are stored as workflow artifacts for debugging. They are not part
of the public download set. The public executable is stripped after symbol
separation, and path-leak checks reject repository, dependency and staging paths.

## Native candidate tests

The release jobs download the common files and only their matching platform
artifact. They test that assembled candidate without rebuilding or rewriting its bytes.
A private update fixture is built separately from the same checkout with a genuine
newer official version so `cup update cup` can exercise a real version transition
without patching or modifying the candidate under test.

The native release suites check:

- exact file membership;
- checksum files and bytes;
- `release.txt` and provenance identity;
- executable version and startup;
- installation into the default base and a custom user-manageable base;
- complete canonical-root relocation and reinstall at the relocated base;
- a successful `cup doctor` after installation;
- relevant preservation, repair and uninstall behavior.

These suites are smaller than the source integration suites because their job is
to validate the packaged generation, not to repeat every internal fault case.
Publication depends on all native candidate results.

## Publication

`scripts/release/publish.sh` owns the remote GitHub release operation. It first
copies the completed local candidate to a private snapshot. Hashing, comparison
and upload use only that snapshot.

Before changing remote state, the script validates:

- the complete public file set;
- the exact `release.txt` and `provenance.txt` schemas;
- checksum membership and values;
- installer version, tag and source metadata;
- the source commit selected for the tag;
- public file permissions.

The publisher handles these cases:

### No tag or release exists

It creates a draft release targeted at the tested commit, uploads the snapshot,
downloads the remote assets for comparison and publishes only after the exact
set and bytes match. The publisher does not pre-create a tag for this path. If a
tag becomes observable during the draft lifecycle it must resolve to the tested
commit, and successful publication requires the published release to have that
resolvable tag. A tag that existed before draft creation is validated before it
is used.

### A matching draft exists

It resumes the draft only when the remote provenance identifies the same
candidate generation. Missing or stale expected assets can then be corrected.
Unexpected assets are removed only after that ownership check.

### A release was created concurrently

The state is read again before creation and publication. A concurrently created
or published release is accepted only when its tag, exact asset set and bytes
match the local snapshot.

### The release is already published

A published release is treated as read-only. It is successful only when every
remote asset matches the snapshot exactly. The script does not edit an already
published generation.

Network or API failures are not interpreted as “not found”. Ambiguous drafts or
releases are preserved for manual inspection instead of being adopted or
deleted.

Only the publication job receives `contents: write`. Earlier jobs use read-only
permissions.

## Concurrency

Tests use a ref-specific concurrency group and may cancel an older run for the
same ref. Release publication uses one non-cancelling `cup-release` group and the
protected `release` environment.

Candidate matrices use `fail-fast: false`, so an independent platform failure
does not hide the remaining results. Publication still verifies remote state
instead of relying on workflow serialization as its only protection.

## Manual release sequence

The intended sequence is:

```text
update VERSION manually
review and commit the source changes
push the commit to main
let Tests finish successfully for that exact commit
dispatch Release from the same main commit
verify the selected Tests run and source-build-config attempt
build the five official candidates
test the exact candidates on native runners
publish the verified generation
```

If the release inputs or source commit change, the source Tests run must be
repeated. A candidate is never reused for another source commit or a distinct
Release run. A retry inside the same Release run may reuse an unchanged upstream
artifact, which is why the internal artifact names and public provenance remain
stable across `run_attempt` changes.

## Relationship with `cup update cup`

`cup update cup` is available only in official builds. cup reads the public
`latest/release.txt`, compares the version and then downloads immutable
versioned assets.

Before scheduling the update, the running program creates a native helper copy
from its own executable and verifies the two files byte for byte. The detached
helper can then replace the installed executable even on Windows, where the
running file cannot be replaced directly.

The helper:

1. verifies the full staged generation;
2. backs up the installed generation assets;
3. replaces supporting files atomically;
4. writes the durable commit marker;
5. replaces `cup` or `cup.exe` last;
6. completes cleanup or leaves enough evidence for recovery.

The helper itself is operational data, not a seventh versioned generation file.
`cup repair` does not replace its own running executable. When safe completion
requires the detached helper, repair preserves the journal and staging data
instead of pretending the update was completed.

## Main release scripts

| Script | Responsibility |
|---|---|
| `scripts/build/finalize-release.sh` | Finalize one inspected platform bundle |
| `scripts/release/common-assets.sh` | Build files shared by every platform |
| `scripts/release/build-platform.sh` | Combine one native bundle with common assets |
| `scripts/release/assemble-candidate.sh` | Merge validated parts into one flat candidate |
| `scripts/release/publish.sh` | Compare and publish the candidate on GitHub |

None of these scripts rebuilds or rewrites a candidate after assembly.

## Related chapters

- [Build](BUILD.md)
- [Testing](TESTING.md)
- [Security](../design/SECURITY.md)
- [Platforms](../design/PLATFORMS.md)
