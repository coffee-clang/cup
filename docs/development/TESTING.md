# Testing

The test layout follows the part of the project that is being checked. A parser
rule belongs in a unit test, a command workflow belongs in integration, and a
release archive belongs in the release suite. Keeping these levels separate
makes failures easier to understand and avoids running the same scenario in
several places.

## Test levels

```text
unit         C modules and decisions
integration  public CLI and visible filesystem effects
portability  properties tied to one platform family
repository   build, scripts, workflows and repository rules
release      already assembled release candidates
```

The same behavior may still be run in a normal, sanitizer and coverage build.
Those executions are not duplicates: they check different compiler/runtime
properties while keeping the scenario owner unchanged.

## General rules

Tests are expected to:

- check an intended behavior or failure;
- use the narrowest suitable level;
- avoid changing production code only to make a line executable;
- avoid depending on private function names or source ordering;
- avoid freezing a private helper path or name unless it is part of a public contract;
- keep POSIX and Windows scenarios aligned where the user-visible behavior is
  the same;
- keep native differences in native tests rather than simulating them on another
  operating system.

A percentage by itself is not enough to decide that two tests are equivalent.
Two scenarios may report the same aggregate coverage while reaching different
branches or checking different effects.

## Unit tests

Unit tests are C executables built with Unity. Each test binary links the module
under test and the smallest useful set of collaborators or mocks.

They cover areas such as:

- command argument validation and command decisions;
- domain, platform and package selection;
- catalog and metadata parsing;
- state and tool-preference persistence;
- checksum and archive-format validation;
- package cache and verified-artifact handling;
- package, update and uninstall journal state transitions;
- doctor and repair decisions;
- native filesystem error mapping;
- wrapper planning and reconciliation.

The `VerifiedArtifact` tests are especially important because the program must
consume the same open file that passed size, digest and archive checks. Journal
tests keep physical file handling in `runtime_journal` and test the separate
schema/recovery rules in their own modules.

Unit binaries are declared in `tests/build/unit.sh`. The runner compares the
built directory against that declaration before execution, so stale or missing
binaries cannot be silently ignored.

Build and run them with:

```sh
make PLATFORM=<platform> test-unit
```

The build-only target is:

```sh
make PLATFORM=<platform> test-unit-build
```

## Integration tests

Integration tests execute the real `cup` binary in an isolated home directory.
They verify command output together with the files and directories left behind.

The shared POSIX and Windows suite families cover:

```text
archive-safety
cli-contract
concurrency
doctor
install-policy
network
package-catalog
package-lifecycle
recovery
repair
state
uninstall
wrappers
```

POSIX also has the initial bootstrap scenario. Windows has additional native
filesystem/reparse-point coverage. These differences are intentional because
shell modes, signals, process handling and reparse points do not have one common
implementation.

The integration layer covers:

- public command syntax and exit status;
- install, remove, default and update behavior;
- target-specific state and wrapper changes;
- catalog resolution and package selection;
- archive and path-safety failures visible to the command;
- local HTTP/HTTPS download behavior;
- lock contention and interrupted operations;
- malformed or incomplete transactions;
- `doctor`, `repair` and uninstall effects.

Network scenarios use local fixtures. They do not depend on a public server.
The helper is built from the test dependency prefix and keeps the tests
repeatable.

Run integration tests with:

```sh
make PLATFORM=<platform> test-integration
```

On POSIX, `tests/runners/integration-posix.sh` discovers the scripts in
`tests/integration/posix/`. On Windows, the PowerShell runner discovers the
native suites in `tests/integration/windows/`. There is no separate persistent
suite manifest.

## Combined behavioral tests

```sh
make PLATFORM=<platform> test
```

For Linux and macOS this builds cup, the unit binaries and the test helpers, then
runs the POSIX unit and integration runners. The Bash-based build/test scripts remain compatible with the Bash version
available on supported macOS runners. For Windows it uses the UCRT64 build and the native PowerShell integration runner.

The focused build target is:

```sh
make PLATFORM=<platform> test-build
```

## Repository tests

Repository tests check contracts that are not public CLI behavior. They run
through:

```sh
make quality
```

or directly:

```sh
./tests/runners/repository.sh
```

The runner reports every independent failure instead of stopping after the first
one. Its checks cover:

- repository structure and unsupported tooling;
- controlled test environments;
- safe build/dependency path handling and deterministic race fixtures;
- dependency lock, build recipes and prefix compatibility;
- CA-bundle metadata and generation;
- Make targets and build configuration;
- readable numeric GitHub Action version refs and workflow permissions;
- dependency, source and evidence-index formats;
- native binary inspection rules;
- version generation and official-version policy;
- installer behavior and supported shell syntax;
- release publication, resume and failure recovery.

A repository assertion is kept only when it protects a current build,
dependency, workflow, installer or release rule. Project-process metadata is not
a test input.

Some repository scenarios need generated build output. `make check` enables them
with `CUP_TEST_WITH_BUILD_OUTPUT=1` after the normal build and behavioral tests
have completed.

## Installer portability checks

The public POSIX installer runs on machines that the project does not control.
Its tests therefore do more than parse it with Bash. Uninstall is implemented by
the native executable and is covered by unit, integration and native platform tests.

Where available, the repository suite checks syntax with:

- `/bin/sh`;
- Dash;
- BusyBox `sh`.

It also runs the generated installer while optional text-processing utilities
are blocked. The scenarios check canonical release versions and checksum text,
curl transport options, the explicit curl prerequisite and post-download size
rejection, signal exit status, the exact root reported by the bootstrap, the
exact installed version, permissions, marker and cleanup results. Commands that the
installer requires are declared and checked before the installation starts.

The Windows installer is exercised by PowerShell release and integration tests.
Windows uninstall is native and is exercised through the executable, including its
handoff, detach, cleanup and recovery behavior.

Linux sanitizer unit and integration tests enable LeakSanitizer together with
AddressSanitizer and UndefinedBehaviorSanitizer. Process-heavy fixtures that may
spawn descendants use a test-only process-group boundary so timeout cleanup
terminates the whole fixture tree rather than weakening leak coverage.

## Coverage

Coverage is run explicitly:

```sh
make PLATFORM=<platform> test-coverage
```

Reports are written below:

```text
build/reports/coverage/<platform>/
```

All platforms use the same `gcovr` report, saved-tracefile and threshold flow.
Linux and Windows feed it GCC/gcov `.gcda` data. macOS feeds it Clang `.profraw`
profiles together with matching `llvm-profdata`, `llvm-cov` and every
instrumented executable resolved from the current build.

macOS names raw profiles with LLVM's `%m` merge-pool pattern. The profiling
runtime serializes updates for repeated executions of the same instrumented
binary, avoiding a separate `.profraw` for every short-lived test process while
preserving distinct profiles for distinct binary signatures. Report processing
uses one worker by default on every backend; an explicit higher
`CUP_COVERAGE_REPORT_JOBS` may fall back to one worker after a timeout.

On macOS, the product, unit tests and helpers share one external coverage entry
wrapper but keep distinct internal entry symbols. This lets the LLVM backend
consume all current objects without treating unrelated `main` functions as the
same function. Profile or object incompatibility is detected by report
generation itself rather than by matching warning text.

Thresholds are applied independently to each platform. A missing branch on
Windows should not be hidden by a higher Linux result. Coverage filters include
production sources rather than test fixtures. Profile, object and report inputs
come only from the current isolated coverage build.

A new test should come from a missing behavior or error contract, not from the
goal of executing an otherwise meaningless line.

## Sanitizers

```sh
make PLATFORM=<platform> test-sanitizers
```

The sanitizer configuration uses Clang/Compiler-RT and runs the normal unit and
integration owners. AddressSanitizer and UndefinedBehaviorSanitizer are enabled.
Leak detection is enabled on Linux and disabled where the native platform/tool
combination does not provide a reliable equivalent.

Sanitizer objects and reports remain separate from development and coverage
output. The produced executable is also passed through binary inspection.

## Linux static-runtime portability

```sh
make PLATFORM=linux-x64 test-portability-linux
```

This is not a general integration suite. It verifies properties specific to the
fully static Linux release:

- no unexpected dynamic runtime requirement;
- embedded CA validation;
- rejection of an unknown CA;
- direct HTTPS transfer;
- HTTP CONNECT proxy tunnelling.

All servers and certificates are local to the test.

## Release tests

Release tests receive an already assembled candidate:

```sh
make PLATFORM=<platform> test-release RELEASE_DIR=<candidate-directory>
```

They do not rebuild cup. They check the bytes that would be published:

- exact public file set;
- checksum membership and digest values;
- version and source identity;
- native startup;
- installation into a fresh home;
- `doctor` after installation;
- preservation and cleanup behavior needed by repair/uninstall.

POSIX and Windows have native release runners. A candidate is accepted for
publication only after all five platform jobs have checked their matching files.

## Local full check

The broad local entry point is:

```sh
make PLATFORM=<platform> check
```

It prepares or validates dependencies, runs unit and integration behavior, then
runs repository quality with the build-dependent checks enabled.

This command is useful before pushing, but it cannot replace native CI for the
other operating systems.

## CI organization

The workflows have separate responsibilities:

- `dependencies.yml` prepares or restores each native dependency prefix;
- `tests.yml` runs repository checks, source tests, evidence indexing, coverage
  and sanitizers;
- `release.yml` accepts evidence from one successful Tests run, builds official
  candidates, tests those candidates natively and publishes them;
- `debug.yml` creates diagnostic artifacts;
- `static.yml` belongs to the protected website/Pages surface and is not part of
  cup source or release validation.

The Tests workflow runs on pushes to `main`, pull requests and manual dispatch.
The final gate checks the result of every required job directly.

### Evidence used by releases

A successful Tests run produces dependency evidence for the six dependency
profiles and source evidence for the five supported platform identifiers. The
single inventory in `scripts/ci/tests-evidence-artifacts.sh` defines the
release-authorizing artifact set.

After those jobs succeed, the evidence-index job records the GitHub artifact ID,
name and SHA-256 digest for the current run attempt. Release later selects one
successful Tests run for the exact commit and downloads the listed artifacts by
ID. Repository, commit, run ID, run attempt, artifact name and local file schema
are checked before any official candidate is built.

When Release supplies its candidate build config, the verifier also requires
the tested compiler command, normalized target and numeric version, plus the
dependency source lock and toolchain fingerprint. Raw targets, executable paths
and full vendor version lines are retained for diagnostics but are not compared
across native runners. Windows applies the same rule to the resource compiler.

A rerun attempt is a different evidence generation. Files from two attempts are
not combined.

## Timeouts

Unit, integration and repository runners support positive timeout environment
variables. When a timeout is requested, GNU `timeout` or `gtimeout` must be
available. Long-running concurrency and child-process scenarios also contain
bounded waits and cleanup paths.

The default local run does not invent a timeout when none was requested.

## Related chapters

- [Build](BUILD.md)
- [Releases](RELEASES.md)
- [Platforms](../design/PLATFORMS.md)
- [Security](../design/SECURITY.md)
