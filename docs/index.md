<div class="logo-container">
  <div class="logo">
    <img src="https://coffee-clang.github.io/logo.svg" alt="Cup logo" width="120" height="120">
  </div>
  <div class="tagline">
    <h1>Cup</h1>
    <p>C Toolchain Installer</p>
  </div>
</div>

`cup` is a user-space toolchain manager for C development tools.

It installs prebuilt tool archives under the user's home directory, keeps a local state file, and lets the user select the default tool for each component without requiring system-wide installation or administrator privileges.

`cup` is intentionally separate from the component build system. This repository contains the command-line installer, the manifest reader, the state manager, the archive downloader/extractor and the bootstrap installers. The archives consumed by `cup` are produced by the separate `cup-components` repository.

## Overview

A typical command is:

```sh
cup install compiler gcc@stable
```

The command:

1. validates the requested component and tool;
2. detects the host platform;
3. resolves the target platform;
4. resolves `stable` through the manifest;
5. downloads the matching package archive;
6. extracts it into a temporary staging directory;
7. validates `info.txt` package metadata;
8. commits the package into `~/.cup/components`;
9. records the installation in `~/.cup/state.txt`.

The current registry accepts:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
linker/lld
formatter/clang-format
linter/clang-tidy
language-server/clangd
analyzer/valgrind
```

Package availability is determined by `packages.cfg`, not by the registry alone.

## Platform model

Platform identifiers use this format:

```text
<os>-<arch>
```

Recognized identifiers are:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Each package lookup uses:

```text
host platform
  where the installed tool runs

target platform
  what the installed tool targets
```

The host is detected by `cup`. The target defaults to the host and can be overridden with `--target`.

## Install cup

### Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

The installer verifies published SHA-256 checksums and creates the canonical per-user tree under `~/.cup`. It installs the executable, a read-only package manifest, a read-only uninstall script, the state file and the operation lock together.

### Windows PowerShell

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 -OutFile $env:TEMP\install-cup.ps1; & $env:TEMP\install-cup.ps1"
```

The Windows installer creates the same canonical tree under `%USERPROFILE%\.cup` and applies the read-only attribute to the manifest and uninstall script. Git Bash, MSYS2 and Cygwin installation delegates to this native installer rather than creating a second shell-specific root.

## Basic commands

```sh
cup help
cup list
cup install compiler gcc@stable
cup default compiler gcc@stable
cup current compiler
cup info compiler gcc@stable
cup remove compiler gcc@stable
cup doctor
cup repair
cup uninstall
```

`doctor` never modifies the installation. `repair` owns safe structural repair, state/component reconciliation and recovery of interrupted transactions recorded in `tmp/transaction.txt`.

Use `--target` when a command should operate on a non-default target platform:

```sh
cup install compiler gcc@stable --target windows-x64
cup list --target windows-x64
cup current compiler --target windows-x64
```

## Documentation sections

- [Specification](specification.md) describes the implemented command model, manifest format, state file, filesystem layout, package metadata, transaction flow and design boundaries.
- [Dependencies](dependencies.md) describes installer dependencies, local build dependencies, linked libraries, bootstrap scripts and release artifacts.
