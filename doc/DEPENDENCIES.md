# Dependencies

This document describes the dependencies used by `cup` itself and by the build infrastructure that produces installable component archives.

`cup` is a user-space toolchain manager for C development tools. End users install the prebuilt `cup` executable and then use it to install prebuilt component packages described by the manifest. The dependencies in this document are therefore split between the `cup` executable, the installer scripts, and the component build/test workflows.

The dependency groups are separate:

```text
end-user installer dependencies
  needed only to install the prebuilt cup executable and manifest

cup executable dependencies
  needed to compile the cup binary

component package builder dependencies
  needed by CI/scripts to build tool archives

package runtime dependencies
  files bundled into produced tool packages when required
```

End users who install `cup` through `install-cup.sh` or `install-cup.ps1` do not build `libcurl`, `libarchive`, GCC, GDB, Clang, LLD, LLDB, Valgrind, or other tool packages locally.

## 1. End-user installer dependencies

### 1.1 Linux shell installer

The Linux installer requires common shell tools:

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
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install-cup.sh | sh
```

The installer downloads the prebuilt `cup` executable and the package manifest, then installs them under:

```text
~/.cup/bin
~/.cup/config
```

It also installs the uninstall script used by `cup uninstall`.

### 1.2 Windows PowerShell installer

The Windows installer requires PowerShell and `Invoke-WebRequest`.

Install command:

```powershell
irm https://github.com/coffee-clang/cup/releases/latest/download/install-cup.ps1 | iex
```

It installs:

```text
%USERPROFILE%\.cup\bin\cup.exe
%USERPROFILE%\.cup\config\packages.cfg
%USERPROFILE%\.cup\bin\uninstall.ps1
```

### 1.3 Windows cmd.exe

From `cmd.exe`, use PowerShell:

```cmd
powershell -ExecutionPolicy Bypass -NoProfile -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install-cup.ps1 | iex"
```

### 1.4 Windows Git Bash, MSYS2, or Cygwin

The shell installer can run in Unix-like Windows environments:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install-cup.sh | sh
```

When it detects Windows, it can delegate to the native PowerShell installer or install only inside the current shell environment.

## 2. Building the cup executable locally

`cup` is written in C and built with `make`.

Supported `PLATFORM` values:

```text
linux-x64
windows-x64
```

Supported `LINK_MODE` values:

```text
dynamic
static
```

Build examples:

```sh
make PLATFORM=linux-x64 LINK_MODE=dynamic
make PLATFORM=linux-x64 LINK_MODE=static
make PLATFORM=windows-x64 LINK_MODE=static
```

Build outputs:

```text
build/linux-x64/bin/cup
build/windows-x64/bin/cup.exe
```

Main source modules:

```text
src/main.c
src/options.c
src/commands.c
src/state.c
src/filesystem.c
src/manifest.c
src/registry.c
src/fetch.c
src/package_archive.c
src/extract.c
src/path.c
src/util.c
src/interrupt.c
src/platform.c
src/system_posix.c
src/system_windows.c
```

Headers are under:

```text
include/
```

Common compiler flags include:

```text
-Wall
-Wextra
-Werror
-std=c11
-g
```

Linux builds also define:

```text
-D_POSIX_C_SOURCE=200809L
```

This is needed for POSIX APIs used by the system and filesystem layers.

## 3. cup executable libraries

`cup` links against:

```text
libcurl
libarchive
zlib
liblzma / xz
OpenSSL on Linux static builds
Windows TLS/network system libraries on Windows static builds
```

### 3.1 libcurl

Used by `src/fetch.c` for archive downloads.

The implementation uses libcurl to:

- follow release asset redirects;
- write downloads directly into archive files;
- report network and HTTP errors;
- support interrupt-aware downloads through the progress callback;
- remove partial downloads on failure or interrupt.

### 3.2 libarchive

Used by `src/extract.c` for archive extraction.

The project uses libarchive instead of invoking external `tar` or `unzip` commands at runtime.

Current package formats include:

```text
tar.gz
tar.xz
zip
```

### 3.3 Compression and TLS libraries

`zlib` and `xz/liblzma` provide archive/compression support through libarchive and related tooling.

Linux static builds use OpenSSL through the static libcurl build.

Windows static builds use Schannel through the static libcurl build and link Windows system libraries such as:

```text
ws2_32
crypt32
bcrypt
advapi32
iphlpapi
secur32
```

## 4. Static dependency bootstrap for cup

The repository contains platform-specific dependency bootstrap scripts:

```text
scripts/bootstrap/bootstrap-linux-deps.sh
scripts/bootstrap/bootstrap-windows-deps.sh
```

These scripts are for developers and CI. They are not executed by end-user installers.

### 4.1 Linux dependency bootstrap

```sh
scripts/bootstrap/bootstrap-linux-deps.sh
```

Default dependency root:

```text
~/deps/linux-x64
```

Default install prefix:

```text
~/deps/linux-x64/install
```

The script builds static dependencies used by the Linux `cup` executable, including:

```text
zlib
xz / liblzma
OpenSSL
curl
libarchive
```

Then build `cup`:

```sh
make PLATFORM=linux-x64 LINK_MODE=static
```

### 4.2 Windows dependency bootstrap

```sh
scripts/bootstrap/bootstrap-windows-deps.sh
```

Default dependency root:

```text
~/deps/windows-x64
```

Default install prefix:

```text
~/deps/windows-x64/install
```

The Windows bootstrap builds static libraries for the `cup.exe` build. The Windows static libcurl build uses Schannel rather than OpenSSL.

Then build `cup`:

```sh
make PLATFORM=windows-x64 LINK_MODE=static
```

## 5. Repository script groups

Scripts are grouped by role:

```text
scripts/build/
  component package build recipes

scripts/package/
  common package helper functions

scripts/test/
  post-package validation tests

scripts/install/
  end-user cup installer/uninstaller scripts

scripts/bootstrap/
  developer/CI dependency bootstrap scripts for cup itself
```

This separation keeps workflow files smaller and prevents build, package, install, and test logic from being mixed in one flat directory.

## 6. Component package build infrastructure

Component package build scripts are not executed by normal users. They are used by CI and developers to produce the archives referenced by `config/packages.cfg`.

Current build scripts:

```text
scripts/build/build-gcc.sh
scripts/build/build-gdb.sh
scripts/build/build-gnu-package.sh
scripts/build/build-llvm-tool.sh
scripts/build/build-valgrind.sh
```

Common package helpers are in:

```text
scripts/package/package-common.sh
```

Package tests are in:

```text
scripts/test/test-gcc.sh
scripts/test/test-gcc-windows.ps1
scripts/test/test-gdb.sh
scripts/test/test-gdb-windows.ps1
scripts/test/test-llvm-tool.sh
scripts/test/test-llvm-tool-windows.ps1
scripts/test/test-valgrind.sh
```

## 7. Component asset publishing

The `cup` executable and bootstrap installers are released from:

```text
coffee-clang/cup
```

Component packages are released from:

```text
coffee-clang/cup-components
```

The manifest points to component assets in `coffee-clang/cup-components`. The component repository is used for release assets produced by the build workflows, while the `coffee-clang/cup` repository remains the source repository for the toolchain manager itself.

In component build workflows:

```text
publish=false
  upload packages as workflow artifacts

publish=true
  publish release assets to coffee-clang/cup-components
```

Publishing to the component repository uses a fine-grained GitHub token stored as the workflow secret:

```text
CUP_COMPONENTS_PAT
```

That token is only needed by workflows that publish component packages. It is not used for `cup` bootstrap releases.

## 8. Linux toolchain builder image

Linux package builds for GCC, GDB, and Valgrind use:

```text
docker/toolchain-builder.Dockerfile
```

This builder is based on Ubuntu 24.04 and includes development tools and libraries for GNU-style `configure`/`make` builds.

Important packages include:

```text
build-essential
ca-certificates
curl
wget
file
flex
bison
make
patch
perl
python3
python3-dev
tar
texinfo
unzip
xz-utils
bzip2
zip
pkg-config
libgmp-dev
libmpfr-dev
libreadline-dev
libexpat1-dev
zlib1g-dev
libncurses-dev
liblzma-dev
libzstd-dev
libdebuginfod-dev
libsource-highlight-dev
libxxhash-dev
libbabeltrace-dev
libipt-dev
openmpi-bin
libopenmpi-dev
libc6-dbg
```

Reasons for notable dependencies:

```text
libc6-dbg
  required for reliable Valgrind Memcheck tests inside the container

openmpi-bin, libopenmpi-dev
  allow Valgrind to build MPI support when available

libdebuginfod-dev
  enables GDB debuginfod support on Linux

libsource-highlight-dev
  enables GDB source highlighting

libxxhash-dev
  enables GDB xxHash support

libbabeltrace-dev
  enables GDB Babeltrace support

libipt-dev
  enables GDB Intel Processor Trace support
```

## 9. Linux LLVM builder image

Linux package builds for LLVM-family tools use:

```text
docker/llvm-builder.Dockerfile
```

Important packages include:

```text
build-essential
ca-certificates
cmake
curl
file
ninja-build
patch
pkg-config
python3
python3-dev
swig
tar
unzip
xz-utils
bzip2
zip
zlib1g-dev
libzstd-dev
libxml2-dev
libedit-dev
libncurses-dev
liblzma-dev
libffi-dev
```

LLVM tools are built with CMake/Ninja rather than the GNU `configure`/`make` flow used by GCC/GDB/Valgrind.

## 10. MSYS2 dependencies for Windows GCC packages

Windows-host GCC package builds use MSYS2 UCRT64.

The workflow installs packages such as:

```text
base-devel
git
curl
tar
gzip
bzip2
xz
zip
unzip
patch
texinfo
python
mingw-w64-ucrt-x86_64-gcc
mingw-w64-ucrt-x86_64-binutils
mingw-w64-ucrt-x86_64-make
mingw-w64-ucrt-x86_64-cmake
mingw-w64-ucrt-x86_64-ninja
mingw-w64-ucrt-x86_64-autotools
mingw-w64-ucrt-x86_64-pkgconf
mingw-w64-ucrt-x86_64-gmp
mingw-w64-ucrt-x86_64-mpfr
mingw-w64-ucrt-x86_64-mpc
mingw-w64-ucrt-x86_64-isl
mingw-w64-ucrt-x86_64-zstd
mingw-w64-ucrt-x86_64-xz
```

GCC Windows packages include Binutils, MinGW-w64 headers/runtime, winpthreads, and GCC itself.

## 11. MSYS2 dependencies for Windows GDB packages

Windows-host GDB package builds also use MSYS2 UCRT64.

The workflow installs packages such as:

```text
base-devel
git
curl
tar
gzip
bzip2
xz
zip
unzip
patch
texinfo
python
mingw-w64-ucrt-x86_64-gcc
mingw-w64-ucrt-x86_64-binutils
mingw-w64-ucrt-x86_64-make
mingw-w64-ucrt-x86_64-autotools
mingw-w64-ucrt-x86_64-pkgconf
mingw-w64-ucrt-x86_64-python
mingw-w64-ucrt-x86_64-readline
mingw-w64-ucrt-x86_64-expat
mingw-w64-ucrt-x86_64-zlib
mingw-w64-ucrt-x86_64-ncurses
mingw-w64-ucrt-x86_64-xz
mingw-w64-ucrt-x86_64-zstd
```

Windows GDB packages include runtime DLLs and a Python standard library copy so `gdb.exe` can run outside the MSYS2 build environment.

Some advanced GDB features that are enabled on Linux are intentionally left aside on Windows for now because they need separate runtime packaging validation:

```text
debuginfod
source-highlight
xxhash
babeltrace
intel-pt
```

## 12. MSYS2 dependencies for Windows LLVM packages

Windows-host LLVM packages use MSYS2 CLANG64.

The workflow installs packages such as:

```text
base-devel
git
curl
tar
gzip
bzip2
xz
zip
unzip
patch
python
mingw-w64-clang-x86_64-clang
mingw-w64-clang-x86_64-lld
mingw-w64-clang-x86_64-cmake
mingw-w64-clang-x86_64-ninja
mingw-w64-clang-x86_64-pkgconf
mingw-w64-clang-x86_64-swig
mingw-w64-clang-x86_64-python
mingw-w64-clang-x86_64-zlib
mingw-w64-clang-x86_64-zstd
mingw-w64-clang-x86_64-libxml2
mingw-w64-clang-x86_64-xz
```

Windows LLDB currently disables terminal features that are straightforward on Linux but require careful Windows validation:

```text
LLDB_ENABLE_LIBEDIT=OFF
LLDB_ENABLE_CURSES=OFF
```

If these are enabled later, the build and package tests should verify the correct Windows equivalents and runtime DLL behavior.

## 13. GCC package build notes

GCC packages are built from upstream GCC sources.

Current package version:

```text
GCC 16.1.0-rev1
```

Supporting versions used by the GCC package recipe include:

```text
Binutils 2.46.0
MinGW-w64 14.0.0
```

Current GCC package forms:

```text
linux-x64 -> linux-x64
linux-x64 -> windows-x64
windows-x64 -> windows-x64
```

Linux native GCC packages include native Binutils to make the package more self-contained than a bare GCC driver.

Windows-target GCC packages include:

```text
Binutils
MinGW-w64 headers/runtime
winpthreads
GCC final compiler
```

For Windows targets, the build uses a staged flow:

```text
build Binutils
install MinGW headers
build GCC stage1
build MinGW CRT
build winpthreads
build final GCC
```

The final Windows-target GCC build uses `--disable-bootstrap`. The staged MinGW flow already performs the necessary bootstrap sequence for the target runtime. Letting GCC run its own full stage2/stage3 bootstrap for the Windows target is fragile and unnecessary in this build recipe.

Linux native GCC uses `--enable-bootstrap`.

Post-package tests validate:

```text
C compile/link
C++ compile/link
pthread or winpthreads compile/link
LTO compile/link
basic generated executable behavior where runnable on the host
```

## 14. GDB package build notes

GDB packages are built from upstream GDB sources.

Current package version:

```text
GDB 17.1
```

Linux GDB enables the main useful features currently intended for distribution:

```text
Python
readline
curses/TUI
expat
zlib
lzma
zstd
debuginfod
source-highlight
xxhash
Babeltrace
Intel PT
threading
```

Windows GDB currently enables the stable set validated for standalone packaging:

```text
Python
readline
curses/TUI
expat
zlib
lzma
zstd
threading
```

Windows GDB packages copy runtime DLL dependencies and Python runtime files into the package so `gdb.exe` works outside MSYS2.

Post-package tests validate:

```text
gdb --version
gdb --configuration
Python scripting
breakpoint/run/print/backtrace on a small C program
standalone execution outside MSYS2 on Windows
```

## 15. LLVM package build notes

LLVM packages are built from upstream `llvm-project` sources.

Current package version:

```text
LLVM 22.1.5
```

The same build script handles:

```text
compiler/clang
linker/lld
debugger/lldb
language-server/clangd
formatter/clang-format
linter/clang-tidy
```

The script selects the required LLVM projects for each tool and prunes only `bin/` to avoid exposing unrelated tools while preserving `lib/`, `libexec/`, `include/`, and `share/`.

This avoids removing runtime files, resource directories, plugins, libraries, or support files that the selected tool may need.

Important packaging rules:

```text
clang packages keep clang, clang++, clang-cpp, clang-cl, clang-scan-deps, and LLD frontends where available.
lld packages test frontend commands such as ld.lld, lld-link, and wasm-ld rather than using lld as a generic driver.
lldb packages preserve runtime/support directories and test interactive-debugger behavior in batch mode.
clangd packages test --check against a small compile_commands.json project.
clang-format packages test real formatting.
clang-tidy packages test checks listing and a real analysis invocation.
```

Windows LLVM packages copy required runtime DLLs into `bin/`.

## 16. Valgrind package build notes

Valgrind packages are built from upstream Valgrind sources.

Current package version:

```text
Valgrind 3.27.0
```

Current package scope:

```text
linux-x64 -> linux-x64
```

The build currently uses:

```text
--enable-only64bit
```

This is intentional for the current `linux-x64` platform. 32-bit support is left for a future explicit 32-bit platform implementation.

The package includes the standard Valgrind tools and records metadata for:

```text
memcheck
cachegrind
callgrind
massif
helgrind
drd
dhat
lackey
exp-bbv
none
```

MPI support is enabled when the builder provides a usable MPI compiler and headers. When enabled, the package contains `libmpiwrap-*` and records `contents.mpi=true` in `info.txt`.

Valgrind uses a generated launcher script because the installed package must be relocatable under `.cup`. The launcher sets `VALGRIND_LIB` to the package's Valgrind runtime directory and then executes the real binary.

Post-package tests validate:

```text
valgrind --version
memcheck help
real leak detection
MPI metadata when enabled
relocation by copying the package to /tmp and rerunning memcheck
```

## 17. Windows runtime DLL packaging

Windows packages built with MSYS2 may depend on DLLs from UCRT64 or CLANG64.

The package helper:

```text
copy_windows_runtime_dlls
```

uses `ldd` to inspect `.exe` and `.dll` files in `bin/`, copies allowed MSYS2 runtime DLLs into `bin/`, and recursively scans copied DLLs.

Allowed runtime source locations include:

```text
/ucrt64/bin
/mingw64/bin
/mingw32/bin
/clang64/bin
/clangarm64/bin
```

Windows system DLLs are not copied.

This is required because a standalone Windows package should work when extracted and run from `cmd.exe` or PowerShell without MSYS2 on the PATH.

## 18. Package tests

Each package workflow runs tests after packaging and before publishing.

The tests intentionally extract the archive produced by the build rather than testing the staging prefix directly. This verifies that the released package is usable.

Linux tests run through shell scripts in:

```text
scripts/test/*.sh
```

Windows tests run through PowerShell scripts in:

```text
scripts/test/*.ps1
```

Windows tests isolate PATH where useful to ensure the package does not accidentally depend on MSYS2 runtime directories.

## 19. What end users need for installed tools

End users installing tool packages through `cup` do not need the build dependencies listed above.

They need only:

```text
cup installed
network access to the release asset URL
normal system runtime support for the host platform
```

Some installed tools may naturally use system libraries or system runtime files. For example:

- Linux native GCC links generated programs against the host system C runtime;
- Linux GDB with Python uses the host Linux runtime libraries it was built against;
- Windows packages bundle MSYS2 runtime DLLs needed by the packaged executables;
- Valgrind depends on the host Linux system and its supported dynamic loader/libc behavior.

The package tests are designed to catch missing package-side runtime files before publishing.