# Testing

This document defines the verification layers, their ownership and the explicit
environment contract used locally and by GitHub Actions.

## Objective

Each behavior has one primary test owner. Unit, repository, integration and
release-candidate tests protect different boundaries; repeated checks are kept
only when they validate distinct trust transitions.

## Repository layout

```text
tests/
  unit/                 C tests and generated-header fixtures
  repository/           repository, build and release-script contracts
  integration/posix/    POSIX real-CLI workflows
  integration/windows/  Windows native real-CLI workflows
  release/              completed candidate asset tests
  support/              sourced assertions and fixture builders
  runners/              stable Make and CI package commands
```

Operational setup and publication scripts live under `scripts/ci/` and
`scripts/release/`. The repository structure test rejects obsolete workflow
names, the former `entrypoints` module paths and the removed dependency-scope
model.

## C unit tests

C tests own deterministic module contracts, parser boundaries, exact size and
validation limits, and failure branches that can be isolated through controlled
external calls. Stubs may replace operating-system or network boundaries, but
the production module under test is compiled and executed.

Test names are concise because the source file already identifies the owning
module. A case uses `test_<scenario>_<result>` only when both parts add useful
information; there is no arbitrary character limit.

Ownership follows production responsibilities:

- command suites cover parsing-independent command policy and public output;
- package suites cover identities, catalogs, metadata, archives, cache and
  installation operations;
- state, journal and transaction suites cover persistence and recovery choices;
- filesystem, layout and system suites cover concrete platform primitives;
- wrapper and CUP-update suites cover derived executables and detached helpers.

`test_package_archive` uses real archives for reader and payload compatibility,
while `test_archive_faults` owns deterministic libarchive adapter failures and
preflight limits. `test_package_transaction` owns package-journal parsing and
recovery decisions. `test_runtime_journal` and `test_cup_update_journal` own the
shared blocker and CUP-update protocol respectively.

`test_storage` combines concrete POSIX system, filesystem and layout cases in
one binary because they share process-wide HOME, permission and lock state. The
cases remain separated by source file. Code after `fork()` may not be reflected
completely by gcov because a child may terminate with `_exit`; final filesystem
state is therefore the authoritative assertion for those cases.

## Repository contract tests

Shell tests under `tests/repository/` verify:

- version and tag policy;
- release metadata and checksum schemas;
- dependency-prefix preparation and linker isolation;
- workflow ownership and exact Tests-run provenance;
- release candidate metadata;
- resumable, idempotent publication;
- deterministic CA generation;
- repository structure and obsolete-path removal.

They invoke the real repository scripts instead of reimplementing their policy.

## POSIX integration tests

POSIX integration uses the real executable with an isolated HOME and exercises
multi-process state, locks, permissions, concurrent commands, recovery,
detached helpers and managed wrappers. Files are grouped by one externally
visible workflow owner:

- `build.sh` prepares the development executable used by the runner; static
  build-system policy remains in repository tests.
- `package-catalog.sh` owns catalog trust and checksum selection; parser edge
  cases remain in `test_package_catalog`.
- `archive-safety.sh` owns real archive formats, format mismatch and malicious
  archives through `cup install`; adapter failures remain in unit tests.
- `cli-contract.sh` owns top-level dispatch, help and Argtable3 shapes.
- `package-lifecycle.sh` owns the complete install/default/update/inspect/remove
  workflow.
- `install-policy.sh` owns preferences, profiles and curated toolchains.
- `state.sh` owns persisted records, scope identity and invalid-state rejection.
- `wrappers.sh` owns wrapper creation, drift, collisions and repair.
- `recovery.sh` owns decisions at persistent transaction commit boundaries.
- `repair.sh` owns reconstruction, quarantine and ambiguity preservation.
- `doctor.sh` owns read-only diagnosis and proof that observation does not
  mutate damage.
- `concurrency.sh` owns real multi-process lock serialization.
- `uninstall.sh` owns detached cleanup and canonical-root removal.

Shared assertions live in `tests/support/common.sh`; package and CLI fixtures
live in `tests/support/posix-cli.sh`. Integration functions are named after the
behavior they prove, not after implementation details or issue numbers. A suite
that expects a coherent final runtime uses `assert_cup_healthy` (or
`Assert-CupHealthy` on Windows), which checks the summary and rejects hidden
issues, warnings and incomplete inspections instead of discarding doctor output.
The state suites assert exact persisted records; catalog and default formatting
remain owned by the package lifecycle suites.

## Windows integration tests

Windows uses a native PowerShell harness for Windows paths and attributes,
PowerShell process invocation, `.cmd` wrapper quoting, native package layout and
detached helpers. `filesystem-archives.ps1` owns private root ACLs, ZIP path
collisions, declared/actual format mismatch, reparse-point cleanup and paths
beyond `MAX_PATH`. Native execution is required; cross-compilation is not
treated as a substitute for Windows behavior.

## Release candidate tests

`tests/release/` consumes an already built candidate directory. It verifies
asset names, exact checksum entries, the static executable, native CLI workflows
and generated installers. Candidate tests never rebuild source, so the bytes
that pass are the bytes eligible for publication.

## Runners and Make targets

Stable runners only compose owners and configure instrumentation:

```text
unit.sh               execute unit binaries compiled by Make
repository.sh         run repository contracts
integration-posix.sh  run POSIX real-CLI workflows
all-posix.sh          compose the normal POSIX regression
coverage.sh           reuse owners with GCC coverage instrumentation
sanitizers.sh         reuse owners with ASan, UBSan and LeakSanitizer
```

Make package commands are:

```sh
make test
make test-unit
make test-repository
make test-integration
make test-windows
make test-coverage
make test-sanitizers
```

## Explicit local environment

The runners derive the native CUP platform from `uname` unless
`CUP_TEST_PLATFORM` is explicitly set. A generic ambient `PLATFORM` value such
as `linux/amd64` is ignored because it is not a CUP platform identifier.

The default dependency prefix is:

```text
~/deps/<platform>/install
```

Prepare it before any compiling test:

```sh
make PLATFORM=<platform> deps
```

Tests do not download or build dependencies implicitly. They validate the
complete platform prefix, including Argtable3, uthash, Unity, libevent, curl,
libarchive and their configured transitive libraries, and fail with the explicit
preparation command when the prefix is incomplete.

The prefix is complete and is the only third-party graph used by development,
debug, coverage, sanitizer and release configurations. CUP and archive-related
unit suites consume the prefix headers, static archives and scoped link metadata;
host libcurl and libarchive development libraries are not part of the build.
Unity is linked by exact archive path. Libevent is linked only into the combined
`network-helper`, which supplies the local HTTP server and HTTP CONNECT proxy
fixtures without becoming a CUP runtime dependency.

A native CI environment may override `DEPS_PREFIX`. The Windows source, debug
and release jobs all run `JOBS=4 make PLATFORM=windows-x64 deps` inside MSYS2
UCRT64. They build the complete pinned prefix from source and do not consume
pacman-provided curl, libarchive or Argtable3 binaries.

## Linux network portability

The dedicated target:

```sh
make PLATFORM=linux-x64 test-portability-linux
```

uses local-only fixtures and CUP's real release executable. It generates a
temporary CA, serves a package over HTTPS, verifies that an unrelated CA is
rejected, repeats the package lifecycle through an HTTP CONNECT proxy, executes
the generated wrapper and finishes with `cup doctor`.

This target is not part of the ordinary `make test` loop. It is a focused CI
gate for the glibc standalone path and avoids turning normal source tests into
network or certificate orchestration. It does not by itself establish a final
minimum distribution version or require adoption of musl.

## Coverage

`make test-coverage` runs natively on all five supported platforms. Linux and
Windows use GCC-compatible gcov data; macOS uses Clang source-based coverage
with `llvm-profdata` and `llvm-cov`. `gcovr` normalizes both backends into the
same text, XML, JSON, JSON-summary and HTML artifacts under
`build/coverage/<platform>/`.

The gate executes instrumented unit tests plus the native integration owner:
POSIX suites on Linux/macOS and the PowerShell suite on Windows. The same line,
function and branch thresholds are applied independently to every platform so
platform-specific C branches cannot be hidden by an aggregate-only report.
The runner records compiler/tool versions, preserves logs, bounds unit and
integration execution with `timeout`/`gtimeout`, and prints concrete installation
guidance when an optional quality tool is missing. macOS requires `gcovr` 8.5 or
newer because that is the first supported source-based LLVM backend.

## Sanitizers

`make test-sanitizers` runs Clang/Compiler-RT AddressSanitizer and
UndefinedBehaviorSanitizer on Linux x64/ARM64, macOS x64/ARM64 and Windows x64.
Linux and macOS also enable LeakSanitizer; Windows uses an isolated MSYS2
CLANG64 dependency graph with leak detection disabled. Linux release and native
integration ownership remains GCC, while UCRT64/GCC remains the Windows
production and coverage toolchain. The official Windows release remains
GCC-built.

The Linux x64 source job additionally performs a complete Clang application
build, binary-policy inspection and C unit-test pass after the primary GCC
native suite. This secondary pass is deliberately narrower than the primary
owner: integration and network-portability behavior are not duplicated merely
to increase the matrix.
Sanitizer logs are stored under `build/sanitizers/<platform>/`. Unit and native
integration execution are bounded so a sanitizer-discovered deadlock cannot
stall a runner indefinitely.

The sanitizer configuration is deliberately separate from release linking:
ASan runtimes are diagnostic dependencies and are never accepted in official
release artifacts.

## GitHub Actions integration

`.github/workflows/tests.yml` is the complete application verification workflow.
It runs automatically for pushes to `main` and can be dispatched manually.
It owns:

```text
Linux x64 and ARM64 source tests
macOS x64 and ARM64 source tests
Windows x64 source tests
native coverage gates for all five platforms
native ASan/UBSan gates for all five platforms
release candidate builds for all supported platforms
native execution of every candidate
one final Tests gate
```

A manual run outside `main` performs source verification but does not create a
publishable candidate. On `main`, the prepare job checks the public release
state. A new or draft version produces candidate jobs; an already published
version runs source gates only.

`.github/workflows/release.yml` performs no compilation or testing. It accepts
only a successful `Tests` run for the exact selected `main` SHA, downloads that
run's artifacts, validates their metadata and delegates publication. See
[RELEASES](RELEASES.md).

`.github/workflows/debug.yml` produces downloadable, non-stripped development
executables and platform-native symbol data for every supported platform. It is
diagnostic-only and does not replace native coverage or sanitizer ownership.

## Ownership rules

A repeated assertion is acceptable only when the second suite crosses a
different trust boundary. For example, a unit test proves that a wrapper plan
rejects a collision, while the POSIX integration suite proves that the real CLI
leaves state and the filesystem unchanged after that rejection. Repeating the
same parser error or the same pure helper result in both layers is redundant.

`tests/repository/assertions.sh` enforces the assertion discipline: every local
C test is registered exactly once and has an observable assertion, every named
POSIX integration test is invoked once, failure-output fixtures are consumed,
opaque placeholder assertions are forbidden, and health checks may not discard
doctor output. These are structural safeguards, not substitutes for reviewing
whether each assertion proves the intended contract.

Before adding a test:

1. identify the smallest public or internal contract that failed;
2. place deterministic branches in the owning C unit suite;
3. keep one real-CLI happy path and the essential failure transition in the
   integration owner;
4. use repository tests only for static files, scripts and pipeline policy;
5. do not add cases whose only purpose is executing defensive lines that cannot
   occur through a valid caller.

- Unit-test internal deterministic branches once.
- Integration-test the externally visible workflow joining modules.
- Keep platform-native behavior in the native suite.
- Keep release policy in versioned scripts rather than duplicating it in YAML.
- Share helpers only where there are multiple real consumers.
- Retain repeated validation only across distinct trust boundaries.
- Treat coverage as evidence of contracts, not as a reason to create fake
  workflows.

## Related documents

- [ARCHITECTURE](../design/ARCHITECTURE.md) — runtime ownership;
- [BUILD](BUILD.md) — dependency and linker contracts;
- [RELEASES](RELEASES.md) — candidate and publication gates;
- [PLATFORMS](../design/PLATFORMS.md) — native responsibilities.
