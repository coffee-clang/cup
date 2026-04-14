# CUP DEPENDENCIES

## 1. Build dependencies

The following tools are required to build the project:

- `gcc`
- `make`

Notes:

- the project is compiled as a C11 program
- the current `Makefile` builds a static binary

## 2. Runtime dependencies

The following tools are required during execution:

- `curl`
- `tar`

### curl
Used to download package archives into the local cache.

### tar
Used to extract downloaded archives into the temporary staging directory.

## 3. Environment requirements

The current implementation expects:

- a Linux-like environment
- the `HOME` environment variable to be defined
- permission to create and modify files under `~/.cup`

The current design does not require administrator privileges.

## 4. Local runtime data

At runtime, `cup` creates and manages:

```text
~/.cup/
```

This includes:

- `state.txt`
- `components/`
- `cache/`
- `tmp/`

## 5. Manifest file

The current implementation expects the package manifest at:

```text
config/packages.cfg
```

The manifest is used to resolve:

- stable versions
- nightly versions
- package URL templates

## 6. Possible future dependencies

Future versions may introduce additional dependencies, for example:

- JSON or TOML parsing libraries
- checksum verification tools
- signature verification tools