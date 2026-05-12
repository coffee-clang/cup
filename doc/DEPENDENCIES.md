# Dependencies

This document describes the dependencies used by `cup` itself and by the package builder scripts that produce installable tool archives.

The dependency groups are separate:

```text
cup executable dependencies
  needed to compile the cup binary

package builder dependencies
  needed by CI/scripts to build tool archives

end-user installer dependencies
  needed only to download the prebuilt cup executable and manifest
```

End users who install `cup` through `install.sh` or `install.ps1` do not build `libcurl`, `libarchive`, GCC, GDB, Clang, LLD, or LLDB locally.

## 1. End-user installer dependencies

### 1.1 Linux shell installer

The Linux installer requires:

```text
sh
curl or wget
chmod
mkdir
mv
rm
uname
```

Command:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

It downloads:

```text
cup-linux-x64
packages.cfg
```

and installs them as:

```text
~/.cup/bin/cup
~/.cup/config/packages.cfg
```

### 1.2 Windows PowerShell installer

The Windows installer requires PowerShell and `Invoke-WebRequest`.

Command:

```powershell
irm https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 | iex
```

It downloads:

```text
cup-windows-x64.exe
packages.cfg
```

and installs them as:

```text
%USERPROFILE%\.cup\bin\cup.exe
%USERPROFILE%\.cup\config\packages.cfg
```

### 1.3 Windows cmd.exe

`cmd.exe` does not provide `sh`. Use PowerShell from `cmd.exe`:

```cmd
powershell -ExecutionPolicy Bypass -NoProfile -Command "irm https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 | iex"
```

### 1.4 Windows Git Bash / MSYS2 / Cygwin

The shell installer can run in Unix-like Windows shells:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

On Windows, it asks whether to run the native PowerShell installer or install only inside the current Unix-like environment.

## 2. Building the cup executable locally

`cup` is written in C and built with `make`.

Supported build targets:

```text
linux-x64
windows-x64
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
src/extract.c
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

The build is controlled by:

```text
Makefile
```

Common compiler flags include:

```text
-Wall
-Wextra
-Werror
-std=c11
-g
```

On Linux, the build also defines:

```text
-D_POSIX_C_SOURCE=200809L
```

This is needed for POSIX APIs used by the system and filesystem layers.

## 3. cup executable libraries

`cup` links against these main libraries:

```text
libcurl
libarchive
zlib
liblzma / xz
OpenSSL on Linux builds
Windows system TLS/network libraries on Windows builds
```

### 3.1 libcurl

Used by `src/fetch.c` for archive downloads.

The implementation uses libcurl to:

- follow release asset redirects;
- write downloads directly into local archive files;
- report network and HTTP errors;
- interrupt downloads through the progress callback;
- remove partial downloads on failure or interrupt.

### 3.2 libarchive

Used by `src/extract.c` for archive extraction.

The project uses libarchive instead of invoking external `tar` or `unzip` commands.

Archive formats are controlled by the manifest. Current package formats include:

```text
tar.gz
tar.xz
zip
```

### 3.3 Compression and TLS libraries

`zlib` and `xz/liblzma` provide archive/compression support through libarchive and related tooling.

Linux builds use OpenSSL through the static libcurl build.

Windows builds use Schannel through the static libcurl build and link Windows system libraries instead of OpenSSL.

## 4. Static dependency bootstrap for cup

The repository contains platform-specific dependency bootstrap scripts:

```text
scripts/bootstrap-linux-deps.sh
scripts/bootstrap-windows-deps.sh
```

These scripts are for developers and CI. They are not executed by end-user installers.

### 4.1 Linux dependency bootstrap

```sh
scripts/bootstrap-linux-deps.sh
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
make PLATFORM=linux-x64
```

### 4.2 Windows dependency bootstrap

```sh
scripts/bootstrap-windows-deps.sh
```

Default dependency root:

```text
~/deps/windows-x64
```

Default install prefix:

```text
~/deps/windows-x64/install
```

This script prepares static dependencies for the Windows `cup.exe` build using the MinGW-w64 cross compiler. Then build:

```sh
make PLATFORM=windows-x64
```

The Makefile expects the Windows compiler as:

```text
x86_64-w64-mingw32-gcc
```

## 5. GitHub workflows for cup bootstrap assets

The repository has workflows for prebuilding `cup` itself:

```text
.github/workflows/build-cup-linux.yml
.github/workflows/build-cup-windows.yml
```

The workflows build the executable and upload assets to the `cup-bootstrap` release.

Linux workflow assets:

```text
cup-linux-x64
packages.cfg
install.sh
SHA256SUMS
```

Windows workflow assets:

```text
cup-windows-x64.exe
packages.cfg
install.ps1
SHA256SUMS.windows
```

The release upload uses `--clobber` for the named assets. Uploading the Windows assets does not remove the Linux assets, and uploading the Linux assets does not remove the Windows assets.

The workflows are intended to update the bootstrap release when the main branch changes relevant source files, build scripts, installers, or workflow files.

## 6. Tool package builder scripts

The builder scripts produce the package archives that `cup` later downloads and installs.

They are not run by `cup install`.

Main package builder scripts:

```text
scripts/package-common.sh
scripts/build-gcc.sh
scripts/build-gdb.sh
scripts/build-gnu-package.sh
scripts/build-llvm-tool.sh
```

Shared defaults currently used by `package-common.sh`:

```text
GCC:        16.1.0
GDB:        17.1
Binutils:   2.46.0
MinGW-w64:  14.0.0
LLVM:       22.1.5
```

Package outputs are written to:

```text
dist/
```

Intermediate source, build, and staging directories are under:

```text
.cup-build/
```

The package scripts create self-contained archives and write an `info.txt` metadata file into each package root.

## 7. GNU package builds

### 7.1 GCC

Built by:

```text
scripts/build-gcc.sh
```

Supported package combinations represented by the current script/manifest include:

```text
linux-x64 -> linux-x64
linux-x64 -> windows-x64
windows-x64 -> windows-x64
```

The native Linux GCC package is built from a GCC source release and uses GCC's `contrib/download_prerequisites` flow.

For Windows-target GCC packages, the package recipe builds a self-contained distribution that can include:

```text
GCC
Binutils
MinGW-w64 headers/runtime
winpthreads
```

Windows-target GCC packages use version strings with package revisions, such as:

```text
16.1.0-rev1
```

The builder intentionally packages required supporting files into the GCC archive instead of relying on install-time dependency solving inside `cup`.

### 7.2 GDB

Built by:

```text
scripts/build-gdb.sh
```

The current recipe supports native builds, for example:

```text
linux-x64 -> linux-x64
windows-x64 -> windows-x64
```

Cross GDB packages are not supported by the current recipe.

## 8. LLVM package builds

Built by:

```text
scripts/build-llvm-tool.sh
```

Supported tools:

```text
clang
lld
lldb
```

Current recipes use the LLVM monorepo source release and native host/target combinations.

Project selections:

```text
clang package:
  LLVM_ENABLE_PROJECTS=clang;lld

lld package:
  LLVM_ENABLE_PROJECTS=lld

lldb package:
  LLVM_ENABLE_PROJECTS=clang;lld;lldb
```

The Clang package includes LLD. The LLDB package includes Clang and LLD. The standalone LLD package is also available as the `linker/lld` component.

LLDB configuration currently keeps Python, libxml2, and LZMA enabled. On Windows, LibEdit and Curses are not forced because the CLANG64/MSYS2 detection for those optional terminal features is fragile; the current package recipe follows a simpler native Windows build path for LLDB.

Cross LLVM package builds are not supported by the current recipe.

## 9. Package archive metadata

Every package builder writes `info.txt` into the package root.

Common metadata fields include:

```text
package.component
package.tool
package.version
package.revision
package.mode
package.formats
platform.host
platform.target
platform.host_triple
platform.target_triple
platform.runtime
platform.thread_model
build.environment
build.source_policy
source.primary.name
source.primary.version
source.primary.url
contents.self_contained
```

Tool-specific metadata may include bundled components or build configuration fields, for example:

```text
bundle.binutils.version
bundle.mingw-w64.version
config.llvm_projects
config.llvm_targets
contents.includes_lld
contents.includes_clang
```

`cup` currently validates only generic install structure at runtime. The metadata is primarily for inspection, traceability, and future extension.

## 10. Runtime versus build-time dependencies

The current package strategy favors self-contained tool archives.

This means:

- package builders may use many dependencies while compiling tools;
- selected runtime/support files are included in the produced package archive when needed;
- `cup` does not install package dependencies separately;
- `cup` does not solve dependency graphs during install;
- end users only need the prebuilt `cup` executable, manifest, and network access to package assets.

This separation is intentional and keeps the runtime installer logic small.
