# `cup`

`cup` is a userspace C toolchain manager. It installs and manages prebuilt C
development tools without requiring administrator privileges or writing into
system toolchain directories.

A `cup` installation can keep multiple tool versions side by side, select one
default per component and target, and expose the commands provided by those
defaults through its own `bin` directory. Package production is intentionally
separate: [`cup-components`](https://github.com/coffee-clang/cup-components)
builds and publishes the tool packages that `cup` verifies and consumes.

## What `cup` manages

`cup` currently has first-class component categories for compilers, debuggers,
linkers, formatters, linters, language servers and analyzers. The built-in domain
contains GCC, Clang, GDB, LLDB, GNU `ld`, LLD, `clang-format`, `clang-tidy`,
`clangd` and Valgrind; actual package availability depends on the host, target
and current catalog.

The core model is deliberately small:

- packages are prebuilt and installed below one user-managed `cup` root;
- multiple concrete versions can coexist;
- `stable` is resolved from the catalog before a package is installed;
- defaults decide which installed package provides a component's commands;
- preferences affect abbreviated future installs without changing current defaults;
- profiles select several components, while toolchains select a curated set of tools;
- package bytes, metadata and release assets are verified before installation is accepted;
- interrupted changes leave recovery information for `cup doctor` and `cup repair`.

Supported `cup` hosts are Linux x64/ARM64, macOS x64/ARM64 and Windows x64.
Cross-target packages are supported when they are present in the catalog.

## Install

Linux and macOS:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

Windows PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 | iex"
```

The default root is `~/.cup` on Linux/macOS and `%USERPROFILE%\.cup` on
Windows. The installer may offer to add its `bin` directory to the current
user's PATH; `cup` itself never edits PATH.

## Quick start

```sh
cup search
cup install profile standard
cup list
cup info
cup default compiler clang@stable
cup doctor
```

`cup help` shows the complete command set and `cup help <command>` shows the
syntax and effects of one command.

## Documentation

Start with the [documentation index](docs/INDEX.md). In particular:

- [Getting started](docs/user/GETTING_STARTED.md) walks through a first installation;
- [Concepts](docs/user/CONCEPTS.md) explains components, packages, defaults,
  profiles, toolchains, host/target scopes and the `cup` root;
- [Installation](docs/user/INSTALLATION.md) covers installers, PATH, relocation,
  updates and uninstall;
- [Commands](docs/user/COMMANDS.md) is the CLI reference;
- [Architecture](docs/design/ARCHITECTURE.md) is the entry point for the internal design;
- [Build](docs/development/BUILD.md), [Testing](docs/development/TESTING.md) and
  [Releases](docs/development/RELEASES.md) cover repository development.

## Project boundary

`cup` installs complete packages; it is not a source build system, system package
manager or global sysroot manager. Tool-specific build choices and native
validation belong to `cup-components`. `cup` owns package selection, download
and admission, local state, defaults, command wrappers, recovery and its own
release lifecycle.
