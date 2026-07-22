# Build

This document defines the target-based build interface and the configuration-
specific output layout used by `cup`.

## Public build interface

When the repository is distributed as a ZIP snapshot, the archive extractor may
lose Unix executable bits. Restore the intended shell entry-point/library modes
before invoking repository scripts:

```sh
sh scripts/fix-shell-permissions.sh --fix
```

The same script supports `--check`; repository structure tests use that mode so
the permission policy has one authoritative implementation.

The platform remains the only public selector:

```text
PLATFORM  linux-x64, linux-arm64, macos-x64, macos-arm64, windows-x64
```

Build configurations are selected by targets, not combinable variables:

```sh
make PLATFORM=linux-x64
make PLATFORM=linux-x64 debug
make PLATFORM=linux-x64 coverage
make PLATFORM=linux-x64 sanitizers
make PLATFORM=linux-x64 release
```

The removed `LINK_MODE`, `BUILD_MODE`, `DEBUG_SYMBOLS`, `COVERAGE`,
`SANITIZERS` and `RELEASE_BUILD` selectors are rejected rather than retained as
compatibility aliases. A normal local `make release` produces release-mode code
with a development version identity. Official identity remains restricted to
the controlled candidate workflow.


## Quality-tool prerequisites

The native quality runners validate optional host tools before starting and
print platform-specific installation guidance instead of failing with an opaque
`command not found`. The canonical entry points are:

```sh
make PLATFORM=<platform> test-coverage
make PLATFORM=<platform> test-sanitizers
```

Linux and Windows coverage use GCC/gcov. macOS coverage uses Clang source-based
instrumentation and therefore requires `gcovr` 8.5 or newer together with
matching `llvm-profdata` and `llvm-cov` tools. ASan/UBSan are exercised natively
on every supported platform; leak detection is enabled on Linux and macOS and
disabled on Windows. These tools are host-side diagnostics and are not added to
the pinned application/test-library prefix or release notices.

## Output layout

```text
build/<platform>/development/
build/<platform>/debug/
build/<platform>/coverage/
build/<platform>/sanitizers/
build/<platform>/release/
```

Each directory owns its objects, generated version data, build identity and
executable. Changing configuration therefore cannot reuse objects from another
configuration.

Every configuration contains `build-config.txt`. It records the platform,
configuration, host architecture, compiler command/path/target/version, resource
compiler identity, effective preprocessor/compiler/linker flags, dependency
prefix identity and official-build status. The file is replaced atomically only
when this content changes. Objects depend on it, so changing the compiler,
`EXTRA_*` flags, dependency identity or official status invalidates the prior
objects without rebuilding when the identity is unchanged.

## Compiler configuration

Compiler choice is role-based rather than globally uniform. Each published
platform has one canonical compiler, while a different compiler is used only
when it provides independent diagnostic value:

| Role | Linux | macOS | Windows |
|---|---|---|---|
| Development, native integration and release owner | GCC | Apple Clang | MSYS2 UCRT64 GCC |
| Secondary compiler portability | Clang build and C unit tests on x64 | Covered by the native compiler | Covered by the CLANG64 sanitizer path |
| Coverage | GCC/gcov | Clang source-based coverage | UCRT64 GCC/gcov |
| ASan/UBSan | Clang/Compiler-RT | Apple Clang | MSYS2 CLANG64 Clang/Compiler-RT |
| Canonical dependency compiler | GCC | Apple Clang | UCRT64 GCC; separate CLANG64 prefix for sanitizers |

This is intentional diversity, not interchangeable compiler selection. Release
artifacts and native integration behavior always have one compiler owner. The
secondary Linux Clang pass compiles the complete application and runs all C unit
tests against the same canonical dependency prefix; it does not create a second
release graph. Sanitizers use Clang consistently so diagnostics and runtime
behavior do not vary between GCC's libsanitizer and LLVM Compiler-RT.

All C builds use C11 with warnings treated as errors. Development and debug use
`-O0 -g3`; release uses `-O2 -DNDEBUG`; coverage and sanitizer instrumentation
are isolated in their own directories. The debug target adds the richest
portable symbol information selected for the platform.

Mandatory flags are owned by the Makefile and cannot be replaced with direct
`CPPFLAGS=`, `CFLAGS=`, `LDFLAGS=` or `LDLIBS=` command-line assignments. Local
additions use:

```sh
make EXTRA_CPPFLAGS=-DLOCAL_FEATURE
make EXTRA_CFLAGS=-Wconversion
make EXTRA_LDFLAGS=-Wl,--build-id=none
make EXTRA_LDLIBS=-lm
```

Ambient direct flag variables are ignored. Official candidate builds reject all
`EXTRA_*` additions so their identity cannot depend on an uncontrolled local
environment.

Before compiling, `scripts/build/validate-toolchain.sh` verifies the native host,
the compiler target triple and the Windows resource compiler where applicable.
Linux and macOS builds are native. Every Windows x64 path—development, tests,
debug dependencies and release candidates—runs natively in an MSYS2 UCRT64
shell and rejects the MINGW64/MSVCRT toolchain.

macOS x64 and ARM64 currently use deployment target `13.0` for CUP and every
pinned dependency. Windows currently defines `_WIN32_WINNT` and `WINVER` as
`0x0A00`. These are explicit CI build baselines, not yet final minimum-support
promises; the final floors require evidence from native runners. The Makefile
rejects conflicting ambient values and records the effective compiler/linker
flags in `build-config.txt`.

## Dependency ownership

### End-user runtime

Official releases are static application artifacts. End users do not need a C
compiler, package sources, Unity, MSYS2, Homebrew or dependency preparation to
run `cup`.

### Direct application dependencies

- **Argtable3** implements command-specific argument parsing in `main.c`. The
  initial subcommand dispatch is only command selection, not a second parser.
- **uthash** is used by archive extraction to reject duplicate normalized paths.
- **libcurl** provides bounded HTTPS transfers, redirect handling and timeouts.
- **libarchive** performs archive preflight and extraction.
- **SHA-256** is implemented in-tree by `sha256.c`; OpenSSL is not a direct
  checksum dependency. POSIX builds use OpenSSL as libcurl's TLS backend.

### Test dependencies

Unity is linked only into C unit-test binaries. Libevent is linked only into the
`network-helper` fixture used by release and network-portability tests; it does
not enter the CUP application link or any published CUP executable. `gcovr` and
sanitizer runtime support are host tools and are not part of the application
prefix contract.

The canonical source lock is `scripts/dependencies/sources.sh`. The adjacent
`scripts/dependencies/THIRD_PARTY_NOTICES.txt` preserves the corresponding
notices and license texts and is published as a release disclosure; it is not an
alternate dependency resolver.

## One complete dependency prefix

The canonical production/native prefix is:

```text
~/deps/<platform>/install
```

The Windows sanitizer job intentionally uses a second, toolchain-distinct
prefix because CLANG64 archives must not be mixed with the UCRT64/GCC release
graph:

```text
~/deps/windows-x64-clang64/install
```

Either prefix can be overridden with `DEPS_PREFIX`. The separation is by
toolchain identity, not by development/release scope. Relative overrides are
normalized to an absolute path before they enter compiler flags or
`build-config.txt`.

A source checkout may be placed in a path containing ordinary spaces. CUP
escapes the absolute source include path before invoking the compiler and tests
this with a real native build. Explicit `BUILD_DIR` and `DEPS_PREFIX` values are
currently required to be whitespace-free: GNU Make target names and generated
`curl-config`/`pkg-config` metadata do not yet share one portable quoting model
across POSIX and MSYS2. Unsupported values fail immediately instead of being
silently split.

Prepare the prefix explicitly with controlled parallelism:

```sh
JOBS=4 make PLATFORM=linux-x64 deps
```

`JOBS` must be a positive integer and defaults to `4`. The builders do not
derive an unbounded value from the machine CPU count. They also normalize
`LC_ALL`, `LANG`, `TZ` and the process umask, and discard ambient compiler,
linker, package-discovery, cache and Make flags before configuring third-party
projects. Package recipes then supply only their explicit canonical flags.

For an offline Linux x64 build using the verified source bundle, populate the
canonical source directory before running the same command:

```sh
mkdir -p "$HOME/deps/linux-x64"
tar -xJf cup-linux-x64-dependency-sources.tar.xz \
    -C "$HOME/deps/linux-x64"
JOBS=4 make PLATFORM=linux-x64 deps
```

Every cached archive is still checked for minimum size and exact SHA-256. A
valid cache entry is reused without network access; an invalid entry is removed
and fetched again through the normal verified download path.

Dependency preparation is owned by five files:

```text
scripts/dependencies/sources.sh
scripts/dependencies/common.sh
scripts/dependencies/build-posix.sh
scripts/dependencies/build-windows.sh
scripts/dependencies/verify.sh
```

`sources.sh` is the canonical source lock. `common.sh` is a non-executable
library for identity, download, staging and prefix validation. The executable
POSIX and Windows builders own their platform recipes, while `verify.sh` checks
a prepared prefix without downloading or rebuilding anything. Linux and macOS
share `build-posix.sh`; platform settings are selected directly from `PLATFORM`
instead of thin wrappers. The builders download pinned source archives, verify
their SHA-256 values and configure packages for the final prefix. Installation
is redirected below
a separate sibling `DESTDIR`, so generated files such as `curl-config`,
`libarchive.pc` and compiled library defaults retain the final path rather than
a disposable staging path. Generated text metadata is normalized, and its
link flags are validated for residual staging paths before the payload replaces
the final prefix. Binary archives are not rewritten because embedded build or
debug paths do not control the linker. The committed prefix records exactly three identity fields:

```ini
prefix_format=3
platform=<platform>
dependency_id=<sha256>
```

The dependency ID hashes the canonical source versions, URLs, archive digests,
minimum-size policy, applicable TLS graph, essential toolchain identity and the
ordered dependency recipe scripts. Canonical pins are repository constants and
cannot be replaced by ambient variables. A matching identity is not sufficient
by itself: every required header, archive and metadata file is checked before a
prefix is reused. An incomplete
or path-contaminated staging tree is discarded without exposing it as the
final prefix.

The complete prefix contains Argtable3, uthash, Unity, libevent, curl,
libarchive, zlib, xz and, on POSIX, OpenSSL. Windows uses Schannel and therefore
does not require OpenSSL in its prefix. Libevent remains test-only even though
it shares the deterministic platform prefix.

## Explicit preparation contract

Compilation and test targets validate the prefix but never create it. A missing
or incomplete prefix fails with an instruction to run `make PLATFORM=<platform>
deps`. This keeps dependency installation visible, avoids network access hidden
inside `make test`, and makes failures attributable to either preparation or
compilation.

CI follows the same contract: environment setup installs host prerequisites,
then invokes the explicit dependency preparation package command before compiling
or running tests.

## One pinned application dependency graph

Every build configuration consumes the same headers and static third-party
libraries from `DEPS_PREFIX`:

- the prefix include directory is used for Argtable3, uthash, libcurl,
  libarchive, zlib, xz and the platform TLS backend;
- Argtable3 is linked by exact archive path;
- `curl-config --static-libs` supplies the pinned curl graph;
- prefix-scoped `pkg-config --static --libs libarchive` supplies the pinned
  archive graph;
- empty metadata or an incomplete prefix is a hard error before compilation.

Development, debug, coverage and sanitizer builds do **not** add a global
`-static` option. Their third-party libraries are still the pinned static
archives, while normal operating-system libraries retain the platform's native
linkage. This keeps diagnostics and instrumented builds comparable to release
without forcing a fully static process during local development.

The release configuration uses the same third-party graph and adds only the
standalone policy for its platform. Linux currently adds global static linking;
macOS keeps Apple system libraries and frameworks dynamic; Windows uses static
third-party/runtime libraries with the approved system import libraries.

Unity is linked into C test executables by exact archive path. The network
fixture obtains `libevent_extra` and `libevent_core` from prefix-scoped static
`pkg-config` metadata. Unit suites that exercise archive code obtain libarchive
and its transitive flags from the same prefix-scoped metadata instead of using
a host `-larchive` fallback. Production build configuration is checked not to
contain libevent link inputs.

Only release-configuration executables are eligible for publication. Debug and
instrumented artifacts remain diagnostic even though their third-party graph is
identical. Candidate packaging separates native symbols (`cup.debug` or
`cup.dSYM`), strips the public executable and rejects checkout, dependency-root
and transactional staging paths in both dependency archives and the final
binary. OpenSSL is built with the deterministic, non-existent
`/__cup_runtime__/openssl` default namespace while automatic configuration and
DSO loading remain disabled.

## Generated version files

`scripts/version.sh` reads `VERSION` and Git state, then generates under the
selected build directory:

```text
version.h
release.txt
version.rc       Windows
```

The Makefile declares all three outputs. If one is missing while the stamp
exists, the stamp is invalidated and generation is repeated. Source files
include the generated header rather than rewriting tracked files.

## Embedded CA bundle

`certs/cacert.pem` is converted deterministically by
`scripts/certs/generate-ca-bundle.sh` into generated `ca_bundle.h` and
`ca_bundle.c`. `certs/cacert.meta` records the authenticated source, Mozilla
source date, SHA-256, certificate count and maximum accepted age.
`make check-ca-bundle` verifies that contract without network access.
`make update-ca-bundle` validates and compiles a newly downloaded candidate,
rejects rollback/future/suspiciously small bundles and replaces PEM plus
metadata with rollback protection. See
[SECURITY](../design/SECURITY.md#embedded-ca-bundle).

## Make targets

`make help` is the authoritative compact index of all public targets. It includes
builds, dependency checks, complete and focused test suites, release/version
operations, CA maintenance and documentation commands.

Common build and dependency targets:

```sh
make
make debug
make coverage
make sanitizers
make release
JOBS=4 make PLATFORM=<platform> deps
make check-dependencies
make check-toolchain
make check-binary
make clean
```

Test targets:

```sh
make test
make test-unit
make test-integration
make test-posix
make test-repository
make test-coverage
make test-sanitizers
make test-portability-linux
make test-windows
make test-release RELEASE_DIR=<candidate-directory>
```

Test-only build helpers are also public because CI and focused local workflows
use them directly:

```sh
make test-unit-build
make test-helpers
make test-build
```

Version, release, documentation and CA maintenance:

```sh
make version
make validate-release
make release-metadata
make finalize-release
make docs-assets
make docs
make serve
make check-ca-bundle
make update-ca-bundle
```

Destructive local cleanup is guarded:

```sh
CUP_ALLOW_DEV_CLEAN=1 make reset-dev-home
```

The target rejects a missing, relative or root `HOME` before deleting
`$HOME/.cup`.

## Documentation build

Documentation uses mdBook with `book.toml` and Markdown sources under `docs/`.
Remote theme assets can be fetched through `scripts/fetch-docs-assets.sh`. The
static documentation workflow remains independent of application testing and
release publication.

## Linux network portability smoke test

`make PLATFORM=linux-x64 test-portability-linux` builds one isolated static
release with a temporary test CA and exercises CUP itself against local
fixtures. It verifies rejection of an unknown CA, DNS resolution of `localhost`,
direct HTTPS downloads, HTTP CONNECT proxy tunnelling, checksum validation,
archive extraction, wrapper execution and `doctor`.

The target does not contact the public Internet and is intentionally separate
from `make test` because it creates certificates, starts local servers and
builds an additional release executable. CI runs it on Linux x64 as the initial
glibc portability gate. A multi-distribution matrix and a musl switch remain
separate decisions and are not implied by this test.

## Native binary inspection

`make PLATFORM=<platform> check-binary` verifies the executable produced by the
current configuration and writes
`build/<platform>/<configuration>/binary-inspection.txt`. The report records the
object format, exact architecture, SHA-256 and the native dynamic-link policy.

On Linux, development and diagnostic configurations may depend only on the
explicit system/compiler-runtime allowlist. A Linux release must have no ELF
interpreter, `DT_NEEDED`, `RPATH` or `RUNPATH` entries. macOS does not provide
the same fully static system-linking model: CUP links all pinned third-party
dependencies statically, while the Mach-O executable may dynamically reference
only `/usr/lib` and public `/System/Library/Frameworks`. Homebrew, `@rpath`,
`@loader_path`, `@executable_path` and `LC_RPATH` are rejected. The
report requires and records the repository deployment target `13.0` encoded in
the Mach-O load commands. On Windows, the executable must be PE32+ x86-64,
use the console subsystem, import only allowlisted Windows system DLLs, contain
resources and advertise `DYNAMIC_BASE` and `NX_COMPAT`; MinGW and third-party
runtime DLLs are rejected.

Source CI, debug-artifact construction and release-candidate construction run
the inspector on all five supported platform identifiers. Platform-native tools
are used where available: `readelf` for ELF, `lipo`/`otool` for Mach-O and a
PE-capable `objdump` for Windows.

## Related documents

- [ARCHITECTURE](../design/ARCHITECTURE.md) — runtime and script boundaries;
- [TESTING](TESTING.md) — test environments and gates;
- [RELEASES](RELEASES.md) — official identity and candidate publication;
- [PLATFORMS](../design/PLATFORMS.md) — platform-specific behavior.
