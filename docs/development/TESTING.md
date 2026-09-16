# Testing

CUP tests are split by the boundary they exercise. The goal is to make a failure
say something useful: local decisions belong in unit tests, public workflows in
integration tests, repository/build contracts in repository tests, and packaged
bytes in release tests.

Tests should verify behavior or an externally meaningful build property. They
should not freeze an implementation shape merely because the current code uses
one particular helper, shell line or internal call sequence.

## Test layers

| Layer | Purpose |
|---|---|
| Unit | C modules, parsers, policy decisions, state/journal logic and isolated command behavior |
| Integration | Public CLI workflows and filesystem effects through a real CUP executable |
| Repository | Make/scripts/workflows/installers/dependency/release contracts |
| Coverage | Measure exercised production code and enforce project thresholds |
| Sanitizers | Execute native tests under ASan/UBSan |
| Portability | Native properties that cannot be inferred from ordinary unit tests |
| Release | Validate the exact assembled release candidate without rebuilding it |

The repository keeps these layers distinct even when they touch the same feature.
For example, a parser can have unit tests while an install transaction is also
exercised through the CLI; those tests cross different boundaries rather than
duplicating one assertion at two levels.

## Normal local test commands

```sh
make PLATFORM=<platform> test
```

runs unit tests followed by the native integration suite.

Focused targets are:

```sh
make PLATFORM=<platform> test-unit
make PLATFORM=<platform> test-integration
make quality
make PLATFORM=<platform> check
```

`check` combines dependency validation, source tests and repository checks for
the selected native platform.

Windows uses the native PowerShell integration runner. POSIX platforms use the
shell runners under `tests/runners/`.

## Unit tests

Unit suites are independent executables built from `tests/unit/`. Suite
registration in `tests/build/unit.sh` is explicit so each test states which
production modules and special compile/link inputs it needs.

The build uses one optimization that does not change test semantics: outside the
coverage configuration, production `.c` files compiled with the ordinary shared
unit-test flags may be compiled once and reused by compatible suites. A suite
with additional compiler arguments, preprocessor definitions or special inputs
falls back to its own one-shot compilation.

Coverage deliberately disables this object cache. Coverage counters and mapping
files belong to the instrumented test binary/configuration that produced them;
sharing those objects would make coverage ownership less clear for little gain.

This distinction is tested behaviorally: repository tests exercise the build
with a fake compiler and verify whether compatible sources are actually compiled
once rather than grepping for a particular implementation line in the script.

Unity provides the test harness. A successful unit run requires every registered
suite to compile and every test to finish without failures or ignored cases.

## Integration tests

Integration tests execute the built CUP binary and observe public behavior and
managed filesystem state. They cover workflows such as:

- bootstrap and root selection;
- install/remove/default/config operations;
- package download, cache and archive admission;
- wrapper generation;
- doctor and repair;
- transaction recovery and interrupted operations;
- concurrent command behavior;
- self-update mechanics with controlled local fixtures;
- uninstall and relocation;
- platform-specific filesystem behavior.

Network-dependent cases use local controlled servers/fixtures. They do not rely
on the public package service to decide whether source tests pass.

Fault-injection helpers exist where a real failure boundary is important—for
example publication, journal or filesystem transitions. A test is not added
merely to reach an internal defensive branch that cannot occur through the
owned API contract.

## Repository quality

Run:

```sh
make quality
```

or:

```sh
./tests/runners/repository.sh
```

Repository tests validate properties that do not belong to CUP's runtime CLI.
The current groups cover:

- repository structure and environment assumptions;
- managed path/cleanup rules;
- dependency source locks, build recipes and prefix compatibility;
- embedded CA metadata and generation;
- Make/build-identity contracts;
- workflow permissions and pinned action references;
- source/release build identity;
- binary-inspection policy;
- version policy;
- installer behavior;
- release assembly, publication and recovery behavior.

These tests should prefer an observable result over source-shape assertions. For
example, a dependency test should verify the produced prefix capability when
that is the real contract instead of requiring one exact upstream configure
command in a script.

Some repository checks need an existing build output. `make check` enables those
checks after building and running the normal tests.

## Coverage

Run:

```sh
make PLATFORM=<platform> test-coverage
```

The default minimums are:

```text
lines      85%
branches   70%
functions  97%
```

Linux and Windows use GCC/gcov counters. macOS uses Apple Clang/LLVM
instrumentation and converts the native profile data for the common gcovr report
flow.

The runner generates text, XML, JSON and HTML reports plus saved status/threshold
metadata. A coverage job fails if instrumentation/report generation fails or if
any configured threshold is missed.

Coverage is used as a gap detector, not as a target that overrides test quality.
A lower-covered path is investigated for a realistic scenario before a test is
added; impossible internal inputs or secondary OS-error branches do not receive
artificial tests solely to raise the percentage.

## Sanitizers

Run:

```sh
make PLATFORM=<platform> test-sanitizers
```

The sanitizer configuration executes the native unit and integration suites with
ASan and UBSan. Linux additionally enables leak detection. Windows uses the
CLANG64 toolchain/prefix so sanitizer runtime and ABI stay separate from the
UCRT64 GCC release build.

A job is successful only when the tests finish normally and the sanitizer runner
finds no sanitizer report.

## Portability tests

Some properties require a native environment rather than a unit mock. The Linux
portability target is:

```sh
make PLATFORM=linux-x64 test-portability-linux
```

Release/native jobs additionally inspect executable format, architecture,
runtime dependencies/imports, deployment target and other platform-specific
properties. Windows-specific unit/integration work is also available through:

```sh
make test-windows
```

## Release candidate tests

An unpacked candidate is tested with:

```sh
make PLATFORM=<platform> test-release RELEASE_DIR=<candidate-directory>
```

Release tests do not rebuild or patch the candidate. Their purpose is to prove
properties of the bytes that may be published, including:

- exact expected candidate membership;
- checksum and release/provenance metadata;
- executable identity and startup;
- default/custom installation;
- relocation and reinstall behavior;
- doctor/repair/preservation behavior relevant to the packaged generation;
- uninstall.

The release suite is intentionally smaller than the source integration suite.
It answers a different question: whether this finalized candidate is a valid
release, not whether every internal failure path has already been tested again.

## Test infrastructure

Shared fixtures and helpers live under `tests/support`, `tests/helpers` and
platform integration directories. A shared helper should own a genuinely common
mechanism—such as process cleanup, test environment restoration or hash fixture
creation—rather than hiding test intent behind generic wrappers.

Temporary roots are isolated per test/run and cleaned through the same managed
path rules used by the test infrastructure. Process-based tests also own their
child/process-tree cleanup so an interrupted runner does not leave servers or
helpers running after the test has ended.

The test suite must not depend on developer-specific HOME contents, an existing
CUP installation or ambient package state.

## CI test matrix

`.github/workflows/tests.yml` runs the source verification matrix for:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

It has separate job families for repository quality, native source tests,
coverage and sanitizers. Successful source jobs also publish the canonical
`build-config.txt` used later to authorize a release build from the same tested
commit/attempt.

The workflow ends in one gate job that requires the complete release-authorizing
matrix. A green release is therefore not inferred from one representative host.

`debug.yml` separately builds native debug artifacts for all five platforms.
Those artifacts are useful for diagnostics but do not replace source tests or
release-candidate qualification.

## Choosing the right test

When adding or changing behavior, use the narrowest layer that observes the real
contract:

- pure decision/parser/module rule → unit test;
- behavior visible through `cup` → integration test;
- build/script/workflow invariant → repository test;
- platform ABI/filesystem property → native portability/platform test;
- property of publishable bytes → release test.

Add a second layer only when it crosses another meaningful boundary. Do not add
a repository grep for a behavior already proven by executing the responsible
script unless the source form itself is part of the contract.

## Related chapters

- [Build](BUILD.md)
- [Releases](RELEASES.md)
- [Transactions](../design/TRANSACTIONS.md)
- [Platforms](../design/PLATFORMS.md)
