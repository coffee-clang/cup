# cup

`cup` is a command-line tool for installing and managing prebuilt C development
tools in a user-managed installation root. It follows a `rustup`-like model for
C toolchains, while keeping package production in the separate
[`cup-components`](https://github.com/coffee-clang/cup-components) repository.

`cup` works without administrator privileges and verifies downloaded packages before
installing them. The installer can optionally add the selected CUP `bin` directory to the
current user's PATH; CUP never requires system-wide PATH changes.

## Installation

Linux and macOS:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

Windows PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 | iex"
```

The default installation root is `~/.cup` on Linux and macOS and
`%USERPROFILE%\.cup` on Windows. The installer can use another existing user-writable
base directory, but the managed leaf is always `.cup`, with `.coffee-cup` as the fallback
when `.cup` is foreign.

## Getting started

```sh
cup search
cup install gcc
cup list
cup default compiler gcc@stable
cup info
```

Use `cup help` for the command list and `cup help <command>` for command-specific
syntax.

## Documentation

The complete user and developer documentation starts at
[docs/INDEX.md](docs/INDEX.md).
