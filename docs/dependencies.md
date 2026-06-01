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

The installer downloads:

```text
cup binary for the detected platform
packages.cfg
uninstall.sh
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

The installer downloads:

```text
cup-windows-x64.exe
packages.cfg
uninstall.ps1
```

and installs them under:

```text
%USERPROFILE%\.cup\bin
%USERPROFILE%\.cup\config
%USERPROFILE%\.cup\scripts
```

### 2.3 Windows Unix-like shells

When `install.sh` runs under Git Bash, MSYS2 or Cygwin, it detects the Windows shell environment and offers two modes:

```text
native Windows installation through PowerShell
current Unix-like shell installation under $HOME/.cup
```

The native Windows installation is the recommended mode when the user wants `cup` available from normal Windows terminals.

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
make PLATFORM=linux-x64 LINK_MODE=static
make PLATFORM=macos-arm64 LINK_MODE=static
make PLATFORM=windows-x64 LINK_MODE=static
```

A dynamic link mode is available only for local development and troubleshooting, where using system libraries makes iteration easier. It is not the bootstrap installation mode.

Build outputs are written under:

```text
build/<platform>/bin/
```

Examples:

```text
build/linux-x64/bin/cup
build/macos-arm64/bin/cup
build/windows-x64/bin/cup.exe
```

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

The project is compiled with:

```text
-Wall
-Wextra
-Werror
-std=c11
-g
```

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

This keeps the release binary independent from distribution-specific CA bundle paths where the configured libcurl backend requires an explicit certificate bundle.

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

The bootstrap scripts build or install the libraries needed to link the `cup` executable, such as:

```text
zlib
xz / liblzma
OpenSSL where required by the selected libcurl build
libcurl
libarchive
pkg-config metadata used by the Makefile
```

The Makefile reads dependency metadata from:

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
build/windows-x64/bin/cup.exe
```

## 8. Release workflows

The repository contains platform-specific GitHub Actions workflows:

```text
.github/workflows/build-cup-linux.yml
.github/workflows/build-cup-macos.yml
.github/workflows/build-cup-windows.yml
.github/workflows/static.yml
```

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
```

The `packages.cfg` asset is the manifest used by installed copies of `cup` to locate component packages produced by `cup-components`.

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
