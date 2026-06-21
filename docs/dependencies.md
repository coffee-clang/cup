# Dependencies

This document describes the dependencies used to install, build and release `cup` itself.

`cup` installs prebuilt component packages at runtime. End users do not need to build GCC, Clang, GDB, LLDB, LLD or Valgrind locally when using `cup install`. Those component packages are produced by the separate `cup-components` repository.

## 1. Dependency groups

The dependency groups are separate:

```text
end-user installer dependencies
  required to download and install the prebuilt cup executable

cup executable build dependencies
  required to compile cup from source

static dependency bootstrap dependencies
  required by CI/developers to build static third-party libraries for cup

documentation dependencies
  required to build the mdBook documentation
```

## 2. End-user installer dependencies

### 2.1 Linux and macOS shell installer

The shell installer requires common Unix tools:

```text
sh
curl or wget
chmod
mkdir
mv
rm
uname
```

Install command:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

The installer downloads and verifies:

```text
cup binary for the detected platform
packages.cfg
uninstall.sh
SHA256SUMS.<platform>
SHA256SUMS.common
```

and installs them under:

```text
~/.cup/bin
~/.cup/config
~/.cup/scripts
```

Supported shell-installer assets are selected from the detected operating system and architecture:

```text
cup-linux-x64
cup-linux-arm64
cup-macos-x64
cup-macos-arm64
```

### 2.2 Windows PowerShell installer

The Windows installer requires PowerShell and `Invoke-WebRequest`.

Install command:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 -OutFile $env:TEMP\install-cup.ps1; & $env:TEMP\install-cup.ps1"
```

The installer downloads and verifies:

```text
cup-windows-x64.exe
packages.cfg
uninstall.ps1
SHA256SUMS.windows-x64
SHA256SUMS.common
```

and installs them under:

```text
%USERPROFILE%\.cup\bin
%USERPROFILE%\.cup\config
%USERPROFILE%\.cup\scripts
```

### 2.3 Windows Unix-like shells

When `install.sh` runs under Git Bash, MSYS2 or Cygwin, it delegates to the native PowerShell installer. This keeps the single Windows root under `%USERPROFILE%\.cup` and avoids a second shell-specific installation tree.

## 3. Building cup from source

`cup` is written in C and built with `make`. The release executable installed by the bootstrap scripts is produced with the project static dependency configuration.

Supported `PLATFORM` values are:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Release builds use:

```sh
make PLATFORM=linux-x64 LINK_MODE=static BUILD_MODE=release
make PLATFORM=macos-arm64 LINK_MODE=static BUILD_MODE=release
make PLATFORM=windows-x64 LINK_MODE=static BUILD_MODE=release
```

Development builds use `BUILD_MODE=development`, the default. They compile with `-O0 -g3`; release builds compile with `-O2 -DNDEBUG`. A mode stamp under `build/<platform>/<link-mode>/` forces object recompilation when the build mode changes while preserving the established output layout.

A dynamic link mode is available only for local development and troubleshooting, where using system libraries makes iteration easier. It is not the bootstrap installation mode.

Build outputs and object files are separated by platform and link mode:

```text
build/<platform>/<link-mode>/obj/
build/<platform>/<link-mode>/bin/
```

Examples:

```text
build/linux-x64/static/bin/cup
build/linux-x64/dynamic/bin/cup
build/macos-arm64/static/bin/cup
build/windows-x64/static/bin/cup.exe
```

The Makefile emits `.d` dependency files with `-MMD -MP`, so changes to project headers rebuild the affected objects. The static configuration is the only configuration published; the dynamic configuration remains a development and diagnostic tool.

## 4. Source-level build requirements

Common build requirements are:

```text
C compiler
make
C11-capable standard library
libcurl development files
libarchive development files
```

Linux builds use:

```text
gcc
-D_POSIX_C_SOURCE=200809L
```

macOS builds use:

```text
clang
-D_DARWIN_C_SOURCE
```

Windows builds use:

```text
x86_64-w64-mingw32-gcc
MinGW-w64 headers and import libraries
```

All configurations use:

```text
-Wall
-Wextra
-Werror
-std=c11
```

Development and release optimization/debug flags are selected through `BUILD_MODE` rather than being mixed into every build.

## 5. Linked libraries

`cup` links against:

```text
libcurl
libarchive
zlib
xz / liblzma
TLS libraries used by libcurl
platform networking and crypto libraries on Windows
```

### 5.1 libcurl

`src/fetch.c` uses libcurl to download package archives into the local cache.

The implementation uses libcurl to:

```text
follow release asset redirects
write downloads directly to files
surface network and HTTP errors
support interrupt-aware progress callbacks
remove partial downloads after failure or interrupt
```

### 5.2 libarchive

`src/extract.c` uses libarchive to extract package archives.

The project uses libarchive instead of invoking external `tar`, `xz`, `gzip` or `unzip` commands at runtime.

Current package formats are:

```text
tar.xz
tar.gz
zip
```

### 5.3 Compression and TLS

Compression support is provided through libarchive and its dependencies, including zlib and xz/liblzma.

TLS support is provided through libcurl's TLS backend. Static Linux builds use the static dependency tree built by the bootstrap scripts. Windows builds use Windows networking and crypto system libraries through the configured libcurl build.

### 5.4 Embedded CA bundle

The build defines:

```text
CUP_USE_EMBEDDED_CA_BUNDLE
```

The CA bundle source is generated from:

```text
certs/cacert.pem
```

The update script is:

```text
scripts/certs/update-ca-bundle.sh
```

Generated files are:

```text
include/ca_bundle.h
src/ca_bundle.c
```

The update script downloads and validates the PEM, generates the header and a byte-array source in a temporary directory on the repository filesystem, compiles the generated source, and replaces tracked files only after every step succeeds. This keeps the release binary independent from distribution-specific CA bundle paths where the configured libcurl backend requires an explicit certificate bundle.

## 6. Static dependency bootstrap

The repository contains bootstrap scripts for third-party dependencies used by static builds:

```text
scripts/bootstrap/bootstrap-linux-deps.sh
scripts/bootstrap/bootstrap-macos-deps.sh
scripts/bootstrap/bootstrap-windows-deps.sh
scripts/bootstrap/bootstrap-common.sh
```

These scripts are for developers and CI. They are not executed by end-user installers.

Default dependency root:

```text
~/deps/<platform>
```

Default install prefix:

```text
~/deps/<platform>/install
```

The bootstrap scripts pin and verify SHA-256 values for every downloaded source archive before extraction. They also record platform, toolchain, dependency versions and hashes in the dependency prefix. An interrupted or incompatible prefix is rejected with an explicit request for a clean rebuild. The scripts build or install the libraries needed to link the `cup` executable, such as:

```text
zlib
xz / liblzma
OpenSSL where required by the selected libcurl build
libcurl
libarchive
pkg-config metadata used by the Makefile
```

After installation, each bootstrap script requires non-empty static link metadata. The Makefile also fails immediately when the expected metadata tools or flags are missing. It reads dependency metadata from:

```text
$(DEPS_PREFIX)/bin/curl-config
$(DEPS_PREFIX)/lib/pkgconfig/libarchive.pc
```

## 7. Windows link libraries

Static Windows builds link against Windows system libraries required by the selected libcurl and archive stack, including:

```text
ws2_32
crypt32
bcrypt
advapi32
iphlpapi
secur32
```

The produced executable is:

```text
build/windows-x64/static/bin/cup.exe
```

## 8. Release workflows

The repository contains one coordinated executable build and publication workflow:

```text
.github/workflows/build-cup.yml
```

Its Linux, macOS and Windows jobs produce optimized static platform assets. Build jobs use read-only repository permissions; only the dependent publication job receives write permission. The publication job verifies the complete asset set and all checksum files before issuing one release upload command.

Release assets for the bootstrap installer include:

```text
cup-linux-x64
cup-linux-arm64
cup-macos-x64
cup-macos-arm64
cup-windows-x64.exe
packages.cfg
install.sh
install.ps1
uninstall.sh
uninstall.ps1
SHA256SUMS.common
SHA256SUMS.<platform>
```

The `packages.cfg` asset is the manifest used by installed copies of `cup` to locate component packages produced by `cup-components`.


The destructive development cleanup target is guarded explicitly:

```sh
CUP_ALLOW_DEV_CLEAN=1 make reset-dev-home
```

It rejects a missing, relative or root `HOME`.

## 9. Documentation dependencies

The documentation is built with mdBook.

Useful targets:

```sh
make docs
make serve
```

The documentation source is under:

```text
docs/
```

The book configuration is:

```text
book.toml
```

The documentation theme assets are handled by:

```text
scripts/fetch-docs-assets.sh
```

## 10. Runtime dependency boundary

An installed `cup` binary needs only its linked libraries and normal operating system services to run. End users do not need Docker, MSYS2, Homebrew, GCC source trees, LLVM source trees or component build dependencies to use `cup install`.

Component package dependencies are handled by the package contents produced by `cup-components`. `cup` only downloads, validates, extracts and records those packages.
