# Build

This chapter describes the build contract for CUP: the supported native
platforms, build configurations, pinned dependencies, generated identity and
binary inspection rules. The repository `Makefile` is the normal entry point.

For the command list, run:

```sh
make help
```

## Native platforms

CUP is built natively for five public platform identifiers:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Select one with `PLATFORM`:

```sh
make PLATFORM=linux-x64
make PLATFORM=macos-arm64 debug
make PLATFORM=windows-x64 test
```

Official builds do not cross-compile these targets from a different operating
system. Linux builds run on Linux, macOS builds run on macOS and Windows builds
run under MSYS2 on Windows.

## Build configurations

CUP keeps each configuration in a separate build directory:

```text
build/<platform>/development/
build/<platform>/debug/
build/<platform>/coverage/
build/<platform>/sanitizers/
build/<platform>/release/
```

The corresponding targets are:

```sh
make PLATFORM=<platform>              # development
make PLATFORM=<platform> debug
make PLATFORM=<platform> coverage
make PLATFORM=<platform> sanitizers
make PLATFORM=<platform> release
```

Development and debug builds favor diagnostics. Coverage and sanitizer builds
add their native instrumentation. Release builds use optimized release flags and
binary policy, but a local `release` build is not an official published
candidate. Official metadata and finalization are added by `release-candidate`
inside the release workflow.

All project C sources are compiled as C11 and warnings are errors.

### Toolchain roles

| Role | Linux | macOS | Windows |
|---|---|---|---|
| Development / release | GCC | Apple Clang | MSYS2 UCRT64 GCC |
| Additional compiler check | Clang where configured | Apple Clang | — |
| Coverage | GCC/gcov | Apple Clang + LLVM coverage tools | UCRT64 GCC/gcov |
| ASan/UBSan | Clang/Compiler-RT | Apple Clang | MSYS2 CLANG64 |

`scripts/build/validate-toolchain.sh` verifies the selected host and compiler
before compilation. Windows production builds require UCRT64; sanitizer builds
use the separate CLANG64 environment.

The configured build baselines are macOS 13.0 and Windows 10. They are build
inputs, not a broader compatibility guarantee than the native tests establish.

## Build identity

Every configuration writes `build-config.txt`. It records the inputs needed to
identify the produced binary, including:

- platform and configuration;
- compiler identity and target;
- effective compile and link flags;
- dependency-prefix identity;
- official/development build role.

Objects depend on this generated identity. A meaningful compiler, dependency or
flag change therefore invalidates the affected build without requiring a manual
cleanup.

The same configuration generates `version.h` and, on Windows, `version.rc`.
`scripts/version.sh` reads the manually maintained `VERSION` file and Git state.
Development builds expose Git-derived information; an archive without `.git`
uses the `archive` development identity. Official builds require a Git checkout
and the exact source commit.

The public `release.txt` manifest is not build metadata. Release assembly creates
it last from the exact final public asset set, as described in `RELEASES.md`.

## Build directories

`BUILD_DIR` may select another build root. Repository tooling validates it before
using or deleting it and marks managed roots with `.cup-build-root`.

The practical contract is:

- the effective path must be an acceptable absolute managed path;
- CUP's repository root and the user's home directory cannot become build
  cleanup targets;
- existing managed roots must carry the expected marker;
- destructive cleanup operates only on an owned build root.

These rules prevent ordinary build/cleanup mistakes. They are repository tooling
rules, not a duplicate of the stronger runtime filesystem-identity model used by
CUP itself.

## Local compiler additions

The Makefile owns the mandatory build flags. Local experiments should add flags
through:

```sh
make EXTRA_CPPFLAGS=-DLOCAL_FEATURE
make EXTRA_CFLAGS=-Wconversion
make EXTRA_LDFLAGS=-Wl,--build-id=none
make EXTRA_LDLIBS=-lm
```

Official release candidates reject `EXTRA_*` values so their build identity is
fully controlled by the repository.

## Pinned dependencies

`config/dependencies.lock` is the source/build lock for the private dependency
prefix. It records:

- the lock format;
- the dependency `build_revision`;
- every pinned source version;
- the expected SHA-256 for each source archive.

`scripts/dependencies/sources.sh` maps those identities to their download
locations. A recipe change that can alter the prefix increments
`build_revision`, even when upstream versions do not change. This prevents a
prefix produced by an older recipe from being accepted as current.

The application uses:

- Argtable3 for argument parsing;
- uthash while validating archive path sets;
- libcurl for bounded HTTP/HTTPS downloads;
- libarchive, zlib and liblzma for package archives;
- c-ares in the POSIX curl build;
- OpenSSL as the POSIX TLS backend.

Windows uses Schannel instead of OpenSSL for TLS. SHA-256 used by CUP is the
repository implementation in `src/third_party/sha256.c`, not OpenSSL.

Unity and libevent are test dependencies: Unity is linked into unit tests and
libevent into the local network helper.

### Prefix commands

The default dependency root is repository-local:

```text
<checkout>/deps/<platform-or-toolchain-variant>/
```

Its prepared prefix is the `install/` child of that root. Windows CLANG64
sanitizer builds use a separate variant. `DEPS_ROOT` may select another
whitespace-free absolute dependency root when needed. `DEPS_PREFIX` is normally
derived from `DEPS_ROOT`; supplying `DEPS_PREFIX` explicitly selects a prepared
prefix for build/test consumers instead of rebuilding dependencies.

The normal commands are:

```sh
JOBS=4 make PLATFORM=<platform> deps
make PLATFORM=<platform> deps-check
make PLATFORM=<platform> deps-force
make PLATFORM=<platform> deps-clean
```

`deps` reuses a compatible prefix or builds one. `deps-check` is read-only.
`deps-force` rebuilds transactionally. `deps-clean` removes only a marked
managed dependency root.

Prefix compatibility depends on the platform/profile, source-lock digest,
build revision and native toolchain fingerprint. Source archives are verified
before extraction, libraries are installed into staging, and the completed
prefix is verified before publication.

An offline source cache may provide the exact archives named by
`config/dependencies.lock` under the managed dependency root's `src/`
directory. The normal builder still verifies their SHA-256 values.

### Prefix product contract

The prefix verifier checks what CUP actually consumes rather than merely
checking that upstream build commands returned success. Among the current
contract properties:

- zlib's static library is present;
- libcurl exposes exactly the required HTTP and HTTPS protocols;
- libarchive libraries are present while its unused command-line utilities are
  absent;
- the generated OpenSSL configuration records the required no-apps,
  no-autoload-config, no-docs and no-DSO build properties;
- the required static archives, headers, pkg-config/curl-config metadata and
  test-only libraries are present for the selected profile.

This is why dependency validation is part of the build contract rather than a
cache-existence check.

## Linking policy

Pinned third-party libraries are linked statically from the dependency prefix.
Operating-system linkage differs by platform:

- **Linux:** the release executable is fully static.
- **macOS:** third-party libraries are static; approved Apple system libraries
  and frameworks remain dynamic.
- **Windows:** third-party/compiler runtime pieces are static; only approved
  Windows system DLLs are imported.

Development, debug, coverage and sanitizer configurations do not force the same
global release-link mode, but still consume the pinned third-party prefix.

## Coverage and sanitizer builds

`make PLATFORM=<platform> test-coverage` builds, executes and reports coverage
using the native instrumentation backend:

| Platform | Instrumentation | Report frontend |
|---|---|---|
| Linux | GCC/gcov | gcovr |
| macOS | Apple Clang/LLVM | gcovr over LLVM-produced data |
| Windows | UCRT64 GCC/gcov | gcovr |

The default gates are 85% lines, 70% branches and 97% functions. The report
runner saves machine-readable status and report artifacts in the coverage build
area.

`make PLATFORM=<platform> test-sanitizers` runs the ASan/UBSan configuration.
Linux additionally enables leak detection; platform-specific sanitizer options
live in the test runner rather than in application code.

Testing strategy and unit-build reuse are described in [Testing](TESTING.md).

## Binary inspection

Every native configuration can be inspected with:

```sh
make PLATFORM=<platform> check-binary
make PLATFORM=<platform> check-debug
make PLATFORM=<platform> check-coverage
make PLATFORM=<platform> check-sanitizers
make PLATFORM=<platform> check-release
```

The inspector writes `binary-inspection.txt` beside the build output. It checks
the native file format, architecture, dependency/import policy and properties
specific to the selected configuration.

Release inspection additionally enforces the public binary policy:

- Linux release binaries must satisfy the static-runtime contract;
- macOS binaries may reference only the approved system libraries/frameworks,
  must match the requested architecture/deployment target and must not carry an
  `LC_RPATH`;
- Windows binaries must be PE32+ x86-64, contain the expected version resource
  and mitigation flags, and import only approved system DLLs.

Release finalization also separates native debug symbols and checks the stripped
public executable for build-path leakage. Debug artifacts intentionally retain
symbol/debug metadata and are a separate CI product.

## Embedded CA bundle

HTTPS verification uses the tracked CA inputs:

```text
certs/cacert.pem
certs/cacert.meta
```

The build generates C source/header data from them. Use:

```sh
make check-ca-bundle
make update-ca-bundle
```

`check-ca-bundle` validates the checked-in bytes and metadata offline.
`update-ca-bundle` downloads and validates a replacement candidate before the
tracked PEM and metadata are replaced.

## Main Make targets

The target groups below are the normal development interface. `make help` is the
version-specific reference.

### Build targets

```sh
make
make debug
make coverage
make sanitizers
make release
make check-toolchain
make check-binary
make check-{debug,coverage,sanitizers,release}
make clean
```

### Dependencies

```sh
make deps
make deps-check
make deps-force
make deps-clean
```

### Tests and quality

```sh
make test
make test-unit
make test-integration
make quality
make check
make test-coverage
make test-sanitizers
make test-portability-linux
make test-windows
make test-release RELEASE_DIR=<candidate-directory>
```

### Release preparation

```sh
make version
make validate-release
make release-metadata
make release-common-assets
make release-candidate
make debug-artifact
```

### Documentation and certificates

```sh
make docs-assets
make docs
make serve
make check-ca-bundle
make update-ca-bundle
```

`docs-assets` refreshes the optional remote mdBook theme asset. Documentation
publication is independent of the CUP release workflow.

## CI relationship

The GitHub workflows use the same Make targets and dependency-prefix contract:

- `dependencies.yml` prepares pinned native prefixes;
- `tests.yml` owns repository quality, native source tests, coverage and
  sanitizers;
- `debug.yml` packages native debug builds and symbols;
- `release.yml` builds and tests official release candidates;
- `static.yml` builds the documentation site.

The release workflow does not redefine the build model. It supplies official
identity/provenance and requires the source-tested build identity to match the
candidate. See [Releases](RELEASES.md).

## Related chapters

- [Testing](TESTING.md)
- [Releases](RELEASES.md)
- [Platforms](../design/PLATFORMS.md)
- [Security](../design/SECURITY.md)
