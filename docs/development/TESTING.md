# Testing

The test system separates behavioral verification from repository and release
checks. Each scenario belongs to the narrowest layer that can verify it without
repeating the same workflow elsewhere.

## Principles

Tests verify observable `cup` behavior, errors and filesystem effects. They do
not require production-only branches, private command modes or source-text
patterns merely to increase coverage.

The suite avoids:

- duplicate end-to-end scenarios across layers;
- micro-suites for single assertions;
- tests that freeze private function names or source layout;
- fault cases that are equivalent to cases already covered;
- changes to production code whose only purpose is testing.

Coverage is evidence of useful scenarios, not a target to maximize artificially.

## Layers

```text
unit         deterministic C behavior
integration  native command workflows
repository   build, dependency and release contracts
release      verification of assembled release candidates
```

### Unit tests

Unit tests use Unity and focus on parsers, state transitions, package selection,
metadata, checksums, transaction decisions and error propagation. Platform-
specific behavior is tested only where the native implementation can be built
and exercised reliably.

### Integration tests

Integration tests run the real `cup` executable in an isolated user directory.
They cover public commands, persistent effects, package lifecycle, diagnosis,
repair, recovery, concurrency and uninstall.

Linux and macOS use the POSIX suites. Windows uses the native PowerShell suites.
Equivalent user-visible behavior is checked on both platform families, while
platform-specific filesystem and process semantics remain native.

### Repository tests

Repository checks exercise operational contracts that are not CLI behavior,
such as dependency preparation, public Make targets, version generation and
release publication recovery.

They do not inspect production source for particular functions, macros or
implementation order, and they do not validate the internal organization of the
test suite itself.

### Release tests

Release tests consume already assembled candidates. They verify the exact files
that would be published, including checksums, version identity, startup,
installation, repair preservation and uninstall. They do not rebuild the
candidate or repeat detailed unit fault injection.

## Local commands

```sh
make PLATFORM=<platform> test
make PLATFORM=<platform> test-unit
make PLATFORM=<platform> test-integration
make quality
make PLATFORM=<platform> check
```

Coverage and sanitizer runs are explicit:

```sh
make PLATFORM=<platform> test-coverage
make PLATFORM=<platform> test-sanitizers
```

An already assembled release candidate is checked with:

```sh
make PLATFORM=<platform> test-release RELEASE_DIR=<candidate-directory>
```

## Dependencies

`make test` prepares or reuses the compatible dependency prefix. `DEPS_PREFIX`
may select an existing native prefix, and `make deps-check` validates it without
rebuilding.

Dependency compatibility is based on the platform, build profile, recipe and
semantic source lock. Unrelated comments or formatting changes do not invalidate
the prefix.

## Platform matrix

Source tests, coverage and sanitizers run natively for the supported platform
matrix. Linux and Windows coverage use GCC/gcov; macOS uses Clang source-based
coverage. Sanitizers use Clang/Compiler-RT.

A POSIX simulation does not replace native Windows testing, and Windows results
do not stand in for POSIX mode, signal or shell behavior.

## Coverage

Reports are written below:

```text
build/coverage/<platform>/
```

Thresholds are platform-specific so native branches remain visible. Coverage
improvements come from a missing behavior or failure contract, not from
executing an otherwise redundant defensive line.

## Continuous integration

The dependency workflow prepares reusable native prefixes. The tests workflow
runs repository checks, source tests, coverage and sanitizers. The release
workflow requires a successful test result for the selected commit, builds
release candidates and verifies those exact candidates before publication.

## Related documents

- [BUILD](BUILD.md) — build and dependency configuration;
- [RELEASES](RELEASES.md) — release candidates and publication;
- [PLATFORMS](../design/PLATFORMS.md) — native platform differences.
