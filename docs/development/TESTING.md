# Testing

This document defines the verification layers used by `cup`, their ownership
and the local and GitHub Actions entry points.

## Objectives

The test system separates three questions:

1. does the C implementation satisfy its behavioral contracts;
2. do complete native workflows behave correctly on their owning platform;
3. is the repository and its automation internally coherent.

These questions use different targets so a repository-format problem is not
reported as a failed unit test and a successful unit suite is not confused with
complete release verification.

## Repository layout

```text
tests/unit/              deterministic C unit suites
tests/integration/posix/ native POSIX command workflows
tests/integration/windows.ps1
                         native Windows command workflows
tests/repository/        source, script, workflow and policy contracts
tests/release/           tests for already built release candidates
tests/runners/           stable orchestration entry points
tests/support/           shared test-only libraries and fixtures
```

The production source remains under `src/`. Tests may expose controlled
boundaries through test-only hooks, but production code does not depend on test
runners or fixtures.

## Local target model

### Behavioral verification

```sh
make PLATFORM=<platform> test
```

`make test` prepares or reuses the compatible dependency prefix, compiles the
necessary binaries and runs:

```text
C unit tests
native integration tests
```

Focused forms are:

```sh
make PLATFORM=<platform> test-unit
make PLATFORM=<platform> test-integration
```

On Windows, `make test-windows` is the native PowerShell-owned aggregate.

### Repository quality

```sh
make quality
```

This target does not require the complete third-party prefix. It verifies
contracts such as:

```text
repository structure and tracked-file policy
shell syntax and entry-point permissions
Makefile public targets and platform selection
workflow responsibilities and YAML structure
dependency lock, recipe and transaction behavior
release metadata and publication recovery
documentation and generated-file policy
```

Repository tests check current external or operational guarantees. They do not
freeze helper ownership, the textual order of YAML steps or other internal
organization that can change without altering behavior.

### Complete local verification

```sh
make PLATFORM=<platform> check
```

`make check` composes dependency preparation, behavioral tests and repository
quality. It is the recommended command before a substantial push.

Coverage and sanitizer gates remain explicit because they require additional
host tools and take longer than the normal development loop:

```sh
make PLATFORM=<platform> test-coverage
make PLATFORM=<platform> test-sanitizers
```

## Dependency preparation

A developer can run `make test` on a fresh checkout. The public test target first
runs the idempotent dependency preparation path:

- a compatible prefix is verified and reused;
- a missing or incompatible prefix is built transactionally;
- `DEPS_PREFIX` may select an explicitly prepared native prefix;
- `deps-check` is available when validation without modification is required.

The compatibility manifest records platform, profile, recipe and the semantic
source-lock digest. It is independent of comments and script formatting. See
[BUILD](BUILD.md#dependency-prefixes-and-compatibility).

## C unit tests

Unit suites use Unity and are compiled as separate executables. Each suite owns
one coherent module or cross-module contract. Test binaries are written below:

```text
build/<platform>/<configuration>/tests/unit/
```

Unit tests cover deterministic behavior, parser boundaries, state validation,
transaction state machines, package metadata, archive policy, wrapper planning,
update rules and error propagation. Expected failure diagnostics may appear in
the log while the corresponding assertion passes.

Test-only fault injection must be explicit and local. It may replace filesystem,
network or transaction boundaries, but it must not turn production code into a
second implementation used only by tests.

## Native integration tests

POSIX and Windows integration suites exercise the built CLI through real files,
processes and command invocations. They own behavior that cannot be established
by a pure unit test, including:

```text
installation and removal lifecycle
state and wrapper persistence
transaction recovery
concurrent command exclusion
doctor and repair behavior
detached uninstall helpers
platform path and permission semantics
```

Linux and macOS use the POSIX runner. Windows uses the PowerShell suite and the
wide-character Windows implementation. A POSIX simulation is not a substitute
for native Windows behavior.

## Release candidate tests

```sh
make PLATFORM=<platform> test-release RELEASE_DIR=<candidate-directory>
```

Release tests consume an already assembled candidate. They do not rebuild CUP.
They verify the exact executable and common assets that are eligible for
publication, including checksums, version identity, CLI startup and installer or
uninstaller behavior.

In GitHub Actions the candidate is uploaded once, downloaded onto its native
runner and tested there. Publication consumes the same artifact bytes only after
all native candidate jobs succeed.

## Coverage

`make test-coverage` runs the behavioral owners with instrumentation and writes
reports below:

```text
build/coverage/<platform>/
```

Linux and Windows use GCC/gcov. macOS uses Clang source-based coverage with
`llvm-profdata` and `llvm-cov`. `gcovr` produces the common text, XML, JSON and
HTML reports. Thresholds are applied per platform so platform-specific branches
cannot disappear inside one aggregate number.

Coverage is evidence for implemented contracts. It is not a reason to add
unreachable workflows or assertions that merely execute defensive lines.

## Sanitizers

`make test-sanitizers` runs AddressSanitizer and UndefinedBehaviorSanitizer with
Clang/Compiler-RT on every supported platform. Linux enables leak detection;
macOS and Windows run ASan/UBSan without leak detection. Windows uses the
separate CLANG64 dependency profile, while production and coverage remain owned
by UCRT64/GCC.

Sanitizer artifacts and runtimes are diagnostic only and never enter an official
release candidate.

## Linux network portability

The focused target:

```sh
make PLATFORM=linux-x64 test-portability-linux
```

uses local fixtures and the real release executable to exercise DNS, embedded-CA
HTTPS validation, direct and proxied downloads, checksum validation, extraction,
wrapper execution and `cup doctor`. It remains separate from the normal local
test loop because certificate and proxy orchestration is comparatively costly.

## GitHub Actions workflows

### Dependencies

`.github/workflows/dependencies.yml` owns reusable dependency preparation. It:

- can be dispatched manually for all profiles or one selected profile;
- is called automatically by Tests and Release;
- serializes each platform/profile so overlapping runs reuse the cache prepared first;
- restores a platform/profile cache and falls back to `make deps` on a miss;
- verifies the final prefix with `make deps-check`.

The cache is an optimization, never a prerequisite for correctness.

### Tests

`.github/workflows/tests.yml` runs on pushes to `main`, manual dispatch and
reusable workflow calls. It first calls the dependency workflow for all required
profiles, then runs independent jobs for:

```text
repository quality
Linux x64 and ARM64 source tests
macOS x64 and ARM64 source tests
Windows x64 source tests
coverage on all five platform identifiers
ASan/UBSan on all five platform identifiers
```

A final gate reports failure when any required family did not succeed. The test
workflow does not build or publish release candidates.

### Debug artifacts

`.github/workflows/debug.yml` is dispatched manually when diagnostic executables
or native symbol data are needed. It uses the same dependency caches and binary
inspection policy. Debug artifacts never satisfy a release gate.

### Release

`.github/workflows/release.yml` is manual and calls the reusable Tests workflow
for the selected `main` commit. It then builds candidates, tests the exact
artifacts natively and publishes them in the same release run. Publication uses
only evidence and candidate bytes produced by that run. See [RELEASES](RELEASES.md).

## Ownership rules

Before adding a test:

1. identify the smallest behavior or operational guarantee that failed;
2. place deterministic branches in the owning unit suite;
3. keep real multi-module or CLI transitions in the native integration owner;
4. use repository tests only for static source and automation contracts;
5. repeat a check only when a second trust boundary provides different evidence.

Shared helpers are justified only by multiple real consumers. The assertion
quality contract ensures tests are registered, invoked and observable, but it
does not replace review of whether each assertion proves the intended behavior.

## Related documents

- [ARCHITECTURE](../design/ARCHITECTURE.md) — runtime ownership;
- [BUILD](BUILD.md) — dependency and linker contracts;
- [RELEASES](RELEASES.md) — same-run candidate and publication gates;
- [PLATFORMS](../design/PLATFORMS.md) — native responsibilities.
