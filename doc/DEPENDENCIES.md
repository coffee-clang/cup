# Dependencies

This document describes the dependencies and build infrastructure used by `cup`.

It covers:

- local build dependencies for the `cup` binary
- static dependency bootstrap
- static linking notes
- library roles
- Docker/GitHub Actions dependencies for building package archives

## 1. C build requirements

The `cup` binary is written in C and is built with `gcc` and `make`.

The default build uses:

```text
-Wall
-Wextra
-Werror
-std=c11
-D_POSIX_C_SOURCE=200809L
```

The POSIX feature macro is required because the implementation uses POSIX APIs such as:

```text
lstat
opendir
readdir
closedir
rmdir
getpid
rename
```

## 2. External libraries used by cup

The main external libraries are:

```text
libcurl
libarchive
OpenSSL
zlib
liblzma / xz
```

### libcurl

Used by the fetch module to download package archives.

The implementation uses libcurl for HTTP(S) downloads and writes the response directly into a local archive file.

### libarchive

Used by the extract module to read and extract package archives.

The implementation uses libarchive instead of shelling out to `tar`.

### OpenSSL, zlib, liblzma

These are required by libcurl/libarchive and by supported archive/compression formats.

## 3. Static dependency bootstrap

The repository contains:

```text
scripts/bootstrap-static-deps.sh
```

The script builds static dependencies locally.

Default prefix:

```text
~/deps/install
```

The prefix can be overridden:

```sh
PREFIX=/custom/prefix bash scripts/bootstrap-static-deps.sh
```

The script also uses source, build, and download directories, which can be overridden through environment variables.

Default directories:

```text
~/deps/src
~/deps/build
~/deps/downloads
```

## 4. Dependencies built by bootstrap-static-deps.sh

The bootstrap script builds:

```text
zlib
xz / liblzma
OpenSSL
curl
libarchive
```

It expects common build tools to be available on the host system, including:

```text
gcc
make
curl
tar
cmake
perl
pkg-config
```

## 5. Makefile integration

The `Makefile` expects headers and libraries under:

```text
$(PREFIX)/include
$(PREFIX)/lib
$(PREFIX)/lib64
```

Default:

```makefile
PREFIX = $(HOME)/deps/install
```

The link command is static and links against:

```text
-lcurl
-larchive
-lssl
-lcrypto
-lz
-llzma
-ldl
```

The source list should match the current module names:

```text
main.c
commands.c
state.c
filesystem.c
manifest.c
registry.c
fetch.c
extract.c
util.c
interrupt.c
```

A normal local build expects the bootstrap script to have already installed compatible static libraries.

## 6. pkg-config and static link notes

For static linking, library order and transitive dependencies matter.

Useful commands when debugging dependency flags are:

```sh
curl-config --static-libs
pkg-config --static --libs libarchive
```

These commands show the dependency flags required by curl and libarchive in a static build context.

## 7. Editor include paths

For editor support, include paths should point to the same prefix used by the build.

Example include path:

```text
~/deps/install/include
```

This matters for headers such as:

```text
curl/curl.h
archive.h
archive_entry.h
```

The `.vscode/c_cpp_properties.json` file can be adjusted locally if the prefix changes.

## 8. GNU package build environment

GCC and GDB are built from upstream source releases and then published as prebuilt archives.

The current build structure is:

```text
.github/workflows/build-gnu.yml
docker/gnu-builder.Dockerfile
scripts/build-gnu-package.sh
scripts/build-gcc.sh
scripts/build-gdb.sh
```

GitHub Actions is used for orchestration. Docker is used for the build environment.

The Dockerfile uses:

```text
ubuntu:24.04
```

and installs common build tools and libraries for GNU source builds.

Typical packages include:

```text
build-essential
curl
tar
gzip
xz-utils
ca-certificates
flex
bison
texinfo
python3
make
file
patch
libgmp-dev
libmpfr-dev
libmpc-dev
libisl-dev
zlib1g-dev
libexpat1-dev
libncurses-dev
```

## 9. LLVM package build environment

Clang and LLDB are built from LLVM source releases and then published as prebuilt archives.

The current build structure is:

```text
.github/workflows/build-llvm.yml
docker/llvm-builder.Dockerfile
scripts/build-llvm-package.sh
scripts/build-clang.sh
scripts/build-lldb.sh
```

GitHub Actions is used for orchestration. Docker is used for the build environment.

The Dockerfile uses:

```text
ubuntu:24.04
```

and installs tools and libraries needed for LLVM CMake/Ninja builds, including:

```text
build-essential
cmake
ninja-build
curl
git
python3
python3-dev
swig
libedit-dev
libffi-dev
libxml2-dev
libzstd-dev
libncurses-dev
zlib1g-dev
```

## 10. Why Docker is used

Docker does not replace GitHub Actions.

The roles are:

```text
GitHub Actions:
  orchestration

Docker:
  reproducible build environment
```

The Docker image fixes the base system and build dependencies. This avoids depending directly on changes to the `ubuntu-latest` runner image.

## 11. GNU build dispatcher

The GNU dispatcher script is:

```text
scripts/build-gnu-package.sh
```

It receives:

```text
tool
version
build_mode
```

and calls the tool-specific build script.

Examples:

```sh
bash scripts/build-gnu-package.sh gcc 15.2.0 standard
bash scripts/build-gnu-package.sh gdb 17.1 standard
```

## 12. LLVM build dispatcher

The LLVM dispatcher script is:

```text
scripts/build-llvm-package.sh
```

It receives:

```text
tool
version
platform
```

and calls the tool-specific build script.

Examples:

```sh
bash scripts/build-llvm-package.sh clang 22.1.3 linux-x64
bash scripts/build-llvm-package.sh lldb 22.1.3 linux-x64
```

The current platform mapping is:

```text
linux-x64 -> LLVM_TARGETS_TO_BUILD=X86
```

## 13. GCC build script

The GCC build script is:

```text
scripts/build-gcc.sh
```

It downloads GCC source releases from the upstream GCC release directory, extracts them, runs GCC's `contrib/download_prerequisites`, configures the build, installs into a staging directory, and creates both `.tar.gz` and `.tar.xz` archives.

Supported build modes are:

```text
minimal
standard
full
```

The exact configure flags are defined by the script.

## 14. GDB build script

The GDB build script is:

```text
scripts/build-gdb.sh
```

It downloads GDB source releases from GNU FTP, extracts them, configures the build, installs into a staging directory, and creates both `.tar.gz` and `.tar.xz` archives.

Supported build modes are:

```text
minimal
standard
full
```

The exact configure flags are defined by the script.

## 15. Clang build script

The Clang build script is:

```text
scripts/build-clang.sh
```

It downloads LLVM source releases, configures an LLVM build with Clang enabled, installs into a staging directory, and creates both `.tar.gz` and `.tar.xz` archives.

The Clang build enables:

```text
LLVM_ENABLE_PROJECTS=clang
```

`lld` is not included in the Clang package, so it can remain a possible separate linker tool later.

## 16. LLDB build script

The LLDB build script is:

```text
scripts/build-lldb.sh
```

It downloads LLVM source releases, configures an LLVM build with Clang and LLDB enabled, installs into a staging directory, and creates both `.tar.gz` and `.tar.xz` archives.

The LLDB build enables:

```text
LLVM_ENABLE_PROJECTS=clang;lldb
```

Clang is included as a technical dependency of LLDB. The resulting package is still treated as the `debugger.lldb` package.

## 17. Package archive layout

Prebuilt package archives must contain a top-level directory.

Example GNU package:

```text
gcc-15.2.0-linux-x64-standard/bin/gcc
gcc-15.2.0-linux-x64-standard/lib/...
```

Example LLVM packages:

```text
clang-22.1.3-linux-x64/bin/clang
lldb-22.1.3-linux-x64/bin/lldb
```

This is required because `extract.c` strips the first path component during installation.

If an archive does not contain a top-level directory, extraction may produce an invalid layout.

## 18. Release assets

The GNU workflow publishes build-mode-based assets in the same repository.

Example tag:

```text
gdb-17.1-standard
```

Example assets:

```text
gdb-17.1-linux-x64-standard.tar.gz
gdb-17.1-linux-x64-standard.tar.xz
```

The LLVM workflow publishes platform-based assets in the same repository.

Example tag:

```text
clang-22.1.3-linux-x64
```

Example assets:

```text
clang-22.1.3-linux-x64.tar.gz
clang-22.1.3-linux-x64.tar.xz
```

The manifest points to these assets using the repository URL.

Example GNU URL:

```text
debugger.gdb.url_template=https://github.com/coffee-clang/cup/releases/download/gdb-{version}-standard/gdb-{version}-linux-x64-standard.{format}
```

Example LLVM URL:

```text
compiler.clang.url_template=https://github.com/coffee-clang/cup/releases/download/clang-{version}-linux-x64/clang-{version}-linux-x64.{format}
```
