# CUP DEPENDENCIES

## 1. Build dependencies

The project itself is compiled as a C11 program.

Core build tools:

- `gcc`
- `make`

## 2. Current project library dependencies

The current implementation uses:

- `libcurl`
- `libarchive`

### libcurl
Used by the fetch layer to download package archives without delegating to shell commands.

### libarchive
Used by the archive layer to extract package archives without delegating extraction to shell commands.

## 3. Static dependency bootstrap

The repository contains a helper script:

```text
scripts/bootstrap-static-deps.sh
```

This script is intended to build and install static dependencies into a local prefix.

The current dependency bootstrap flow is designed to build:

- `zlib`
- `xz` / `liblzma`
- `OpenSSL`
- `curl`
- `libarchive`

The output is installed under a user-controlled prefix, typically:

```text
$HOME/deps/install
```

with the usual structure:

```text
$HOME/deps/install/
├── include/
├── lib/
└── bin/
```

## 4. Static linking notes

The current project is intended to be linked statically.

In practice, this means the build environment must provide:

- static library archives (`.a`)
- matching headers
- all required transitive dependencies

Typical tools used to inspect the required link flags include:

```bash
$HOME/deps/install/bin/curl-config --static-libs
PKG_CONFIG_PATH="$HOME/deps/install/lib/pkgconfig" pkg-config --static --libs libarchive
```

## 5. Runtime filesystem requirements

The implementation expects:

- a Linux-like environment
- the `HOME` environment variable to be defined
- permission to create and modify files under `~/.cup`

The design does not require administrator privileges for normal execution.

## 6. Runtime data

At runtime, `cup` manages data under:

```text
~/.cup/
```

This includes:

- `state.txt`
- `components/`
- `cache/`
- `tmp/`

## 7. Package manifest dependency

The current implementation expects the manifest file at:

```text
config/packages.cfg
```

The manifest currently provides:

- stable versions
- available versions
- default archive formats
- supported archive formats
- URL templates

## 8. Package source assumptions

The current implementation assumes that package archives are available remotely and can be fetched using the URLs described in the manifest.

The current configured examples use:

- upstream LLVM release assets for `clang`
- repository-hosted prebuilt assets for `gcc`
- repository-hosted prebuilt assets for `gdb`

## 9. Archive assumptions

The current implementation is built around archive formats currently declared in the manifest, including:

- `tar.gz`
- `tar.xz`

The chosen archive format is used during package selection and caching.

Archive extraction is handled through `libarchive`.

## 10. Editor configuration note

When the project is built against locally installed static dependencies, editor tooling may also need the corresponding include paths.

For example, a local editor configuration may need to reference:

```text
/home/<user>/deps/install/include
```

so that headers such as:

- `curl/curl.h`
- `archive.h`
- `archive_entry.h`

are visible to IntelliSense or equivalent tooling.

## 11. Future dependency evolution

Future versions of the project may introduce additional dependencies, for example:

- checksum libraries
- signature verification libraries
- richer manifest parsing libraries
- provider-specific download helpers