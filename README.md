# cup

`cup` is a user-space manager for prebuilt C development tools. It installs,
updates and selects compilers, linkers, debuggers and related tools without
requiring administrator privileges or modifying system toolchain directories.

## What CUP provides

- verified package downloads over HTTPS;
- per-host and per-target tool selections;
- profiles and curated LLVM/GNU toolchains;
- transactional install, update and removal;
- local diagnosis and recovery through `doctor` and `repair`;
- native support for Linux, macOS and Windows.

Tool packages are produced separately by
[`cup-components`](https://github.com/coffee-clang/cup-components). This
repository contains the CUP CLI, installers, local state management and release
pipeline.

## Install

### Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

### Windows PowerShell

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command '$installer = Join-Path $env:TEMP "install-cup.ps1"; iwr "https://github.com/coffee-clang/cup/releases/latest/download/install.ps1" -OutFile $installer; & $installer'
```

The installer places CUP under `~/.cup` or `%USERPROFILE%\.cup` and verifies the
release metadata and SHA-256 checksums before replacing existing assets.

See the [installation guide](docs/user/INSTALLATION.md) for PATH handling,
reinstallation and uninstall details.

## Quick start

```sh
cup search
cup install compiler
cup install profile standard
cup list
cup info
cup config
cup update
cup doctor
```

Use `cup help` for the command list and `cup help <command>` for detailed usage.
The complete reference is available in
[COMMANDS](docs/user/COMMANDS.md).

## Documentation

- [Project website and rendered documentation](https://coffee-clang.github.io/cup/)
- [Documentation index](docs/INDEX.md)
- [Installation](docs/user/INSTALLATION.md)
- [Commands](docs/user/COMMANDS.md)
- [Architecture](docs/design/ARCHITECTURE.md)
- [Platforms and portability](docs/design/PLATFORMS.md)
- [Build](docs/development/BUILD.md)
- [Testing](docs/development/TESTING.md)
- [Releases](docs/development/RELEASES.md)
