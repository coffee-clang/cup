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
format=3
version=X.Y.Z
source_repository=owner/repository
source_commit=<full-source-commit>
tests_run_id=<tests-workflow-run-id>
tests_run_attempt=<tests-workflow-attempt>
tests_evidence_index_sha256=<evidence-index-digest>
release_run_id=<release-workflow-run-id>
release_run_attempt=<release-workflow-attempt>
```

This file connects the public files to:

- the repository and source commit;
- the successful Tests run selected for that commit;
- the exact rerun attempt of that Tests run;
- the evidence index downloaded by Release;
- the Release run that built and tested the candidates.

A rerun attempt is treated as a separate generation. Evidence from different
attempts is never mixed.

## Workflow responsibilities

The three application workflows involved in a release have separate jobs.

### Dependencies

`.github/workflows/dependencies.yml` prepares or restores a dependency prefix for
one native platform/profile. Its evidence records the platform, profile,
toolchain and dependency identity used by the source job.

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
quality, native source tests, coverage and sanitizers. After dependency and
source jobs succeed, it builds an evidence index for the exact workflow attempt.

The release-authorizing set contains:

- six dependency-evidence artifacts;
- five source-evidence artifacts.

`scripts/ci/tests-evidence-artifacts.sh` is the single inventory of the
release-authorizing artifact names. The index stores the artifact IDs and
SHA-256 values returned by GitHub.

### Release

`.github/workflows/release.yml` is manually dispatched. It:

1. selects the exact commit from `main`;
2. finds a successful Tests run for that commit;
3. fixes the selected Tests run ID and run attempt;
4. downloads and verifies the evidence index;
5. downloads every listed evidence artifact by ID;
6. builds common assets and five platform candidates;
7. tests each candidate on its native runner;
8. publishes only after every required job succeeds.

Repository quality, coverage and sanitizers are not repeated inside Release.
Their evidence already belongs to the selected Tests run. Candidate-specific
checks remain in Release because they must examine the final assembled files.

The protected Pages workflow is unrelated to this process. Website deployment
does not authorize a cup release and is not included in the release gate.

## Source evidence

Each source job writes evidence describing the build it tested. The
verifier checks the envelope and its files before Release accepts it. Depending
on the artifact type, the checked data includes:

- repository;
- source commit;
- workflow run ID and attempt;
- artifact name;
- platform and build configuration;
- compiler command, normalized target and numeric version;
- dependency-prefix format, profile, source lock and toolchain fingerprint;
- generated version metadata;
- binary-inspection policy and result.

Compiler/resource-compiler raw targets, paths and full version lines remain
recorded in each build config for diagnosis. Authorization compares the command,
normalized platform target and numeric version because installation paths and
vendor wording can differ without changing compiler identity. Windows candidates
apply the same rule to the tested resource compiler.

The verifier accepts only the repository evidence schema. Unsupported schema
versions are rejected rather than routed through compatibility parsing.

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

- `SHA256SUMS.common` covers files shared by all platforms;
- each platform checksum covers its executable, `release.txt` and the exact common checksum file.

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
mitigation flags. MinGW runtime DLL dependencies are rejected.

Native symbols are stored as workflow artifacts for debugging. They are not part
of the public download set. The public executable is stripped after symbol
separation, and path-leak checks reject repository, dependency and staging paths.

## Native candidate tests

The release jobs download the common files and only their matching platform
artifact. They test the assembled candidate without rebuilding it.

The native release suites check:

- exact file membership;
- checksum files and bytes;
- `release.txt` and provenance identity;
- executable version and startup;
- installation into a fresh user home;
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

It creates the tag for the tested commit, creates a draft release, uploads the
snapshot, downloads the remote assets for comparison and publishes only after
the exact set and bytes match.

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
verify the selected Tests run and evidence attempt
build the five official candidates
test the exact candidates on native runners
publish the verified generation
```

If the release inputs or source commit change, the source Tests run must be
repeated. A candidate is never reused for another commit or workflow attempt.

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
| `scripts/release/assemble-candidate.sh` | Merge validated release parts into one flat candidate directory |
| `scripts/release/publish.sh` | Compare and publish the candidate on GitHub |

None of these scripts rebuilds or rewrites a candidate after assembly.

## Related chapters

- [Build](BUILD.md)
- [Testing](TESTING.md)
- [Security](../design/SECURITY.md)
- [Platforms](../design/PLATFORMS.md)
