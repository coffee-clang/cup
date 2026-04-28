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

## 9. Why Docker is used

Docker does not replace GitHub Actions.

The roles are:

```text
GitHub Actions:
  orchestration

Docker:
  reproducible build environment
```

The Docker image fixes the base system and build dependencies. This avoids depending directly on changes to the `ubuntu-latest` runner image.

## 10. GNU build dispatcher

The dispatcher script is:

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

## 11. GCC build script

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

## 12. GDB build script

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

## 13. Optional LLVM package build environment

The repository can also include optional files for building separated Clang and LLDB archives.

The optional structure is:

```text
.github/workflows/build-llvm.yml
docker/llvm-builder.Dockerfile
scripts/build-llvm-package.sh
scripts/build-clang.sh
scripts/build-lldb.sh
```

The LLVM Dockerfile uses:

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

The LLVM workflow is separate from the GNU workflow.

## 14. LLVM build dispatcher

The optional LLVM dispatcher is:

```text
scripts/build-llvm-package.sh
```

It receives:

```text
tool
version
platform
```

and calls either:

```text
scripts/build-clang.sh
scripts/build-lldb.sh
```

Example:

```sh
bash scripts/build-llvm-package.sh clang 22.1.3 linux-x64
bash scripts/build-llvm-package.sh lldb 22.1.3 linux-x64
```

The current platform mapping is:

```text
linux-x64 -> LLVM_TARGETS_TO_BUILD=X86
```

## 15. Package archive layout

Prebuilt package archives must contain a top-level directory.

Example:

```text
gcc-15.2.0-linux-x64-standard/bin/gcc
gcc-15.2.0-linux-x64-standard/lib/...
```

For optional LLVM packages:

```text
clang-22.1.3-linux-x64/bin/clang
lldb-22.1.3-linux-x64/bin/lldb
```

This is required because `extract.c` strips the first path component during installation.

If an archive does not contain a top-level directory, extraction may produce an invalid layout.

## 16. Release assets

The GNU workflow publishes assets in the same repository.

Example tag:

```text
gdb-17.1-standard
```

Example assets:

```text
gdb-17.1-linux-x64-standard.tar.gz
gdb-17.1-linux-x64-standard.tar.xz
```

The manifest points to these assets using the repository URL.

Example:

```text
debugger.gdb.url_template=https://github.com/coffee-clang/cup/releases/download/gdb-{version}-standard/gdb-{version}-linux-x64-standard.{format}
```

The optional LLVM workflow would publish assets with platform-based names.

Example tag:

```text
clang-22.1.3-linux-x64
```

Example assets:

```text
clang-22.1.3-linux-x64.tar.gz
clang-22.1.3-linux-x64.tar.xz
```

At the moment, Clang and LLDB can still use upstream LLVM assets in `packages.cfg`.
