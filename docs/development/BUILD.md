# Build

This chapter explains how `cup` is compiled, where build files are written and
how the third-party libraries are prepared. The Makefile is the entry point for
normal local work and for the CI jobs.

## Selecting a platform

The public platform value is:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Use it through `PLATFORM`:

```sh
make PLATFORM=linux-x64
make PLATFORM=macos-arm64 debug
make PLATFORM=windows-x64 test
```

A build is always native. Linux builds run on Linux, macOS builds run on macOS
and Windows builds run in MSYS2 on Windows. The project does not use one host to
cross-compile the official five-platform release.

## Build configurations

Configurations are selected by targets instead of combinations of public flag
variables:

```sh
make PLATFORM=linux-x64            # development
make PLATFORM=linux-x64 debug
make PLATFORM=linux-x64 coverage
make PLATFORM=linux-x64 sanitizers
make PLATFORM=linux-x64 release
```

They are kept in separate directories:

```text
build/<platform>/development/
build/<platform>/debug/
build/<platform>/coverage/
build/<platform>/sanitizers/
build/<platform>/release/
```

Objects from one configuration are therefore never reused by another one.

The normal `release` target selects release compiler flags, but it is still a
local build. An official candidate is produced only by `release-candidate`, with
an explicit version, tag and source commit supplied by the release workflow.

## Build-root ownership

`BUILD_DIR` selects the build root. A relative value is resolved from the
repository root; an absolute value is accepted after validation.

Before writing anything, the build checks that:

- the path is absolute after normalization;
- it is not the repository root;
- existing components are real directories rather than symbolic links;
- the path does not contain `.` or `..` components;
- the value can be passed safely through Make and the supported shells.

A build root belongs to this repository only when it contains the expected
`.cup-build-root` marker. The first creation is handled by one native
`prepare-build-root` operation. It stages a new private directory, writes and
synchronizes the marker, then publishes the directory atomically. An existing
unmarked directory is rejected even when it is empty, so the build never adopts
an unrelated path by accident.

Build and clean operations lock the marker before changing the root. A second
operation on the same root fails instead of racing with the first one. `clean`
removes managed entries, then the marker and finally the empty root.

## Build identity

Each configuration contains `build-config.txt`. It records the inputs that can
change the generated binary:

- platform and configuration;
- host architecture;
- compiler command, resolved path, target and version;
- Windows resource compiler identity where needed;
- effective preprocessor, compiler, linker and library flags;
- dependency-prefix compatibility information;
- whether the build is official.

The file is replaced atomically only when its contents change. Objects depend on
it, so changing the compiler, dependency prefix, local additions or official
status causes the required objects to be rebuilt without rebuilding unrelated
third-party libraries.

Version files are generated under the same configuration directory:

```text
version.h
release.txt
version.rc       Windows only
```

When the source has no `.git` directory, the development version remains
human-readable as `dev+archive`. The generated `release.txt` uses the reserved
all-zero 40-character commit sentinel so that archive builds retain the same
strict metadata schema. Official builds are accepted only from a Git checkout
and record the real source commit.

`scripts/version.sh` reads the manually maintained `VERSION` file and Git state.
Development builds include the tag distance, short commit and dirty state.
Official builds use the exact `MAJOR.MINOR.PATCH` value and require an explicit
source commit.

## Compilers and flags

The main compiler for each platform is fixed by role:

| Role | Linux | macOS | Windows |
|---|---|---|---|
| Development, integration and release | GCC | Apple Clang | MSYS2 UCRT64 GCC |
| Additional compiler check | Clang on x64 | native compiler | CLANG64 for sanitizers |
| Coverage | GCC/gcov | Clang source coverage | UCRT64 GCC/gcov |
| ASan/UBSan | Clang/Compiler-RT | Apple Clang | CLANG64 Clang/Compiler-RT |

All C sources use C11 and warnings are treated as errors. Development and debug
builds use `-O0 -g3`; release uses `-O2 -g1 -DNDEBUG`. Coverage and sanitizer
instrumentation remain inside their own configurations.

Mandatory flags are owned by the Makefile. Direct command-line replacement of
`CPPFLAGS`, `CFLAGS`, `LDFLAGS` or `LDLIBS` is not the supported customization
path. Local experiments use additive variables:

```sh
make EXTRA_CPPFLAGS=-DLOCAL_FEATURE
make EXTRA_CFLAGS=-Wconversion
make EXTRA_LDFLAGS=-Wl,--build-id=none
make EXTRA_LDLIBS=-lm
```

Official candidates reject all `EXTRA_*` values.

`scripts/build/validate-toolchain.sh` checks the host, target triple and resource
compiler before compilation. Windows production builds require the UCRT64
environment and reject the older MINGW64/MSVCRT runtime. Sanitizer jobs use the
separate CLANG64 environment.

The current build baselines are macOS 13.0 and Windows 10
(`_WIN32_WINNT=0x0A00`). They describe how CI builds the program; they should not
be read as a final promise about the oldest operating-system version until the
native compatibility tests establish that promise.

## Coverage backend

`make PLATFORM=<platform> test-coverage` uses one report and threshold flow on
all supported platforms. `gcovr` writes the text, XML, JSON, summary and HTML
reports and applies the same saved-tracefile gates.

The raw instrumentation remains native to the compiler:

| Platform | Raw counters | Report frontend |
|---|---|---|
| Linux | GCC `.gcda` | `gcovr` |
| macOS | Clang `.profraw` with `llvm-profdata`/`llvm-cov` from `xcrun` | `gcovr` |
| Windows | UCRT64 GCC `.gcda` | `gcovr` |

For macOS, cup and every instrumented test/helper use the common external
coverage entry wrapper with a distinct internal entry symbol. This allows all
executables to be supplied to the LLVM backend without merging incompatible
`main` definitions. A failure to merge profiles or read an object is an ordinary
coverage failure; there is no repository rule tied to an old warning string.

## Third-party dependencies

End users receive a built executable and do not need the dependency toolchain.
The source build uses these libraries:

### Used by the application

- **Argtable3** parses command-specific arguments.
- **uthash** stores normalized archive paths while duplicates are checked.
- **libcurl** performs bounded HTTP and HTTPS downloads.
- **libarchive** validates and extracts package archives.
- **zlib** and **XZ/liblzma** are part of the archive stack.
- **c-ares** is libcurl's resolver on the supported builds.
- **OpenSSL** is the POSIX TLS backend. Windows uses Schannel instead.

SHA-256 for cup files is implemented by `src/third_party/sha256.c`; OpenSSL is not used as a
second checksum implementation.

### Used only by tests

- **Unity** is linked into C unit-test programs.
- **libevent** is linked into the local network test helper.

Coverage tools, sanitizer runtimes, compilers and binary-inspection utilities are
host tools. They are not part of the application dependency prefix or the
published executable.

## Dependency lock and source definitions

`config/dependencies.lock` contains the format, the manual build revision, each
source version and its SHA-256 value. The current lock selects:

| Dependency | Version |
|---|---|
| zlib | 1.3.2 |
| XZ | 5.8.3 |
| OpenSSL | 3.5.7 |
| c-ares | 1.34.8 |
| curl | 8.21.0 |
| libarchive | 3.8.8 |
| Argtable3 | 3.3.1 |
| uthash | 2.3.0 |
| Unity | 2.6.1 |
| libevent | 2.1.13-stable |

`scripts/dependencies/sources.sh` owns the source identities and download URLs.
The remaining shared code is split by responsibility:

- `environment.sh` prepares deterministic tools and flags;
- `root-transaction.sh` owns prefix locking, staging, commit and cleanup;
- `prefix-metadata.sh` reads, validates and writes prefix metadata;
- `source-build.sh` owns common download, extraction and build operations;
- `common.sh` loads those four modules and defines their shared constants.

`scripts/dependencies/THIRD_PARTY_NOTICES.txt` contains the corresponding
license notices and is included in releases. It is documentation, not a second
source of dependency versions.

When a build recipe changes without a source-version change,
`build_revision` is incremented. This prevents an older prefix produced by a
different recipe from being reused as compatible. Dependency builders also
force a fixed nonzero `SOURCE_DATE_EPOCH`; generated build metadata therefore
does not depend on the wall clock or on an ambient caller setting.

## Dependency roots and profiles

The normal prefix is:

```text
~/deps/<platform>/install
```

Windows sanitizers use a separate CLANG64 prefix:

```text
~/deps/windows-x64-clang64/install
```

`DEPS_ROOT` and `DEPS_PREFIX` may select another absolute, whitespace-free
location. Dependency roots have their own ownership marker and no-follow path
checks. A prefix is reusable only when its recorded data matches:

- prefix format;
- platform and build profile;
- dependency build revision;
- semantic digest of the source lock;
- native compiler/toolchain fingerprint.

Comments or harmless formatting changes do not invalidate a prefix. A changed
version, digest, recipe revision, platform, profile or compiler does.

The main dependency commands are:

```sh
JOBS=4 make PLATFORM=<platform> deps
make PLATFORM=<platform> deps-check
make PLATFORM=<platform> deps-force
make PLATFORM=<platform> deps-clean
```

`deps` reuses a compatible prefix or builds a new one. `deps-check` only
validates and never repairs it. `deps-force` performs a new transactional build.
`deps-clean` removes only a marked dependency root.

Source archives are downloaded to a managed cache, checked against the lock and
extracted into private staging directories. Libraries are built into a staged
prefix. The complete result is verified before it replaces the final prefix, so
a failed build does not leave a half-updated installation.

An offline cache is a dependency root containing the canonical
`.cup-dependencies-root` marker and a `src/` directory with the archives named by
`config/dependencies.lock`. It may omit `build/` and `install/`; `make deps`
creates those transactionally. Extra or stale archives should not be shipped in
the cache even though the builder ignores names that are not present in the
lock.

The dependency scripts intentionally install only the files needed by cup and
its tests. For example, XZ contributes `liblzma` rather than all command-line
programs, and OpenSSL contributes static libraries, headers and metadata rather
than the `openssl` executable, engines or modules.

## Linking policy

All configurations use the same pinned headers and static third-party archives
from `DEPS_PREFIX`.

- Argtable3 is linked by its exact archive path.
- `curl-config --static-libs` supplies curl and its pinned transitive graph.
- prefix-scoped `pkg-config --static --libs libarchive` supplies libarchive.
- Unity is linked only into unit-test executables.
- prefix-scoped libevent metadata is used only by the network helper.

Development, debug, coverage and sanitizer configurations do not apply a global
`-static` flag. Their third-party dependencies are still static, while ordinary
operating-system libraries keep their native linkage.

The release policy depends on the platform:

- Linux produces a fully static ELF executable.
- macOS links pinned third-party libraries statically but uses approved Apple
  system libraries and frameworks dynamically.
- Windows links third-party and compiler runtimes statically and imports only
  the approved Windows system DLLs.

## Safe helper used by repository scripts

Build, dependency and release scripts sometimes need filesystem operations that
cannot be made race-safe with shell commands alone. `scripts/lib/path-ops.c`
provides a small command-line frontend for those operations.

It is compiled on demand together with only the required production modules:

```text
src/system.c
src/system_posix.c
src/path.c
src/text.c
```

The helper is cached in a private per-user runtime directory. Its cache identity
includes the source digests, helper protocol, host, compiler and flags. It is not
installed with cup and is not persistent cup state.

The division of responsibility is:

- `system` owns native no-follow traversal, object identity, move, copy, remove
  and locking primitives;
- `filesystem` owns composite snapshot and atomic-publication operations used by
  the program;
- `path-ops` parses script commands and applies repository-specific policies,
  such as the build-root marker.

Existing-tree traversal is anchored to directory descriptors and carries the
observed identity into later copy or removal operations. New temporary files or
private directories begin from a validated parent path and are then published
with the native atomic operation. A platform without the required no-replace
primitive fails instead of falling back to check-then-move.

## Embedded certificate authority (CA) bundle

cup contains a CA bundle for HTTPS validation. The tracked inputs are:

```text
certs/cacert.pem
certs/cacert.meta
```

`scripts/certs/generate-ca-bundle.sh` creates `ca_bundle.h` and `ca_bundle.c`
inside the build directory. The metadata records the source, source date,
SHA-256, certificate count and accepted age.

Use:

```sh
make check-ca-bundle
make update-ca-bundle
```

The first command works offline. The second downloads a candidate, checks its
identity and contents, compiles the generated source and replaces the PEM and
metadata only after the candidate passes.

## Public Make targets

`make help` lists the supported targets. The main groups are shown below.

### Build and inspection

```sh
make PLATFORM=<platform>
make PLATFORM=<platform> debug
make PLATFORM=<platform> coverage
make PLATFORM=<platform> sanitizers
make PLATFORM=<platform> release
make PLATFORM=<platform> check-toolchain
make PLATFORM=<platform> check-binary
make PLATFORM=<platform> check-debug
make PLATFORM=<platform> check-coverage
make PLATFORM=<platform> check-sanitizers
make PLATFORM=<platform> check-release
make clean
```

`check-binary` and the configuration-specific variants inspect the produced
binary and write `binary-inspection.txt` beside the build output.

### Tests

```sh
make PLATFORM=<platform> test
make PLATFORM=<platform> test-unit
make PLATFORM=<platform> test-integration
make quality
make PLATFORM=<platform> check
make PLATFORM=<platform> test-coverage
make PLATFORM=<platform> test-sanitizers
make PLATFORM=linux-x64 test-portability-linux
make test-windows
make PLATFORM=<platform> test-release RELEASE_DIR=<candidate-directory>
```

`test` runs unit and native integration tests. `quality` checks repository,
build, dependency, workflow and release-script contracts. `check` runs both and
enables the repository checks that need build output.

The focused preparation targets are:

```sh
make test-unit-build
make test-helpers
make test-build
```

### Version and release preparation

```sh
make version
make validate-release
make release-metadata
make release-common-assets
make PLATFORM=<platform> release-candidate
make PLATFORM=<platform> debug-artifact
```

Release construction is explained in [Releases](RELEASES.md).

### Documentation and certificates

```sh
make docs-assets
make docs
make serve
make check-ca-bundle
make update-ca-bundle
```

`docs-assets` runs the existing website helper that fetches the optional mdBook
theme template. `docs` and `serve` depend on that target and then invoke mdBook.
The website files and the Pages workflow are kept separate from cup's build,
test and release mechanisms because that surface belongs to the existing
project website.

### Guarded local cleanup

```sh
CUP_ALLOW_DEV_CLEAN=1 make reset-dev-home
```

This command is intended only for a development home. It rejects a missing,
relative or root `HOME`, and deletes only a candidate root whose strict marker
identifies `coffee-clang/cup`. It stops when the ownership evidence is missing or
ambiguous.

## Binary inspection

`scripts/build/inspect-binary.sh` checks the native format and writes a report
with architecture, SHA-256 and linkage information.

- Linux release binaries must have no ELF interpreter, `DT_NEEDED`, `RPATH` or
  `RUNPATH` entries.
- macOS binaries may reference only approved `/usr/lib` and
  `/System/Library/Frameworks` locations, must match the selected architecture
  and deployment target, and must not contain `LC_RPATH`.
- Windows binaries must be PE32+ x86-64 console programs, import only approved
  Windows system DLLs and contain the expected resource and mitigation flags.

Release finalization also separates native symbols, strips the public executable
and checks that repository, dependency and staging paths are not embedded in the
published files.

## Linux static-runtime test

```sh
make PLATFORM=linux-x64 test-portability-linux
```

This test builds an isolated static Linux release with a temporary CA. It then
uses local servers to verify rejection of an unknown CA, acceptance of the
embedded test CA, direct HTTPS downloads and HTTP CONNECT proxy tunnelling. It
does not contact the public Internet.

## Related chapters

- [Testing](TESTING.md)
- [Releases](RELEASES.md)
- [Platforms](../design/PLATFORMS.md)
- [Security](../design/SECURITY.md)
