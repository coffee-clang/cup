<div class="logo-container">
  <div class="logo">
    <img src="https://coffee-clang.github.io/logo.svg" alt="Cup logo" width="120" height="120">
  </div>
  <div class="tagline">
    <h1>Cup</h1>
    <p>C Package Manager &mdash; Toolchain Installer</p>
  </div>
</div>

Cup is a toolchain manager for C. It installs and manages C development tools
in user space, without requiring system-wide installation or administrator
privileges. Its goal is to provide a simple command-line interface for
installing, tracking, selecting and removing toolchain components across
supported host and target platforms.

## What Cup Does

`cup` installs known component/tool pairs from prebuilt archives described by a
manifest.

```sh
cup install compiler gcc@stable
```

The command resolves the requested release through the manifest, downloads the
matching package asset, extracts it into a temporary staging directory,
validates the package layout, commits it into the local `.cup` tree and records
it in the local state file.

The current implementation supports:

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

A tool must be accepted by the internal registry and present in the manifest to
be installable.

## Supported Platforms

The current platform identifiers are:

```text
linux-x64
windows-x64
```

A package lookup uses two platform values:

- **host platform** — where the installed tool runs
- **target platform** — what the installed tool targets

The target defaults to the host. It can be overridden with `--target` when the
selected tool supports that target.

```sh
cup install compiler gcc@stable
cup install compiler gcc@stable --target windows-x64
```

## Install Cup

### Linux

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

The installer installs:

```text
~/.cup/bin/cup
~/.cup/config/packages.cfg
~/.cup/scripts/uninstall.sh
```

It can add `~/.cup/bin` to the shell `PATH`.

### Windows PowerShell

```powershell
iwr https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 -OutFile $env:TEMP\install-cup.ps1; & $env:TEMP\install-cup.ps1
```

The installer installs:

```text
%USERPROFILE%\.cup\bin\cup.exe
%USERPROFILE%\.cup\config\packages.cfg
%USERPROFILE%\.cup\scripts\uninstall.ps1
```

It can add `%USERPROFILE%\.cup\bin` to the user `PATH`.

### Windows cmd.exe

From `cmd.exe`, call the PowerShell installer:

```cmd
powershell -ExecutionPolicy Bypass -NoProfile -Command "iwr https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 -OutFile $env:TEMP\install-cup.ps1; & $env:TEMP\install-cup.ps1"
```

### Windows Git Bash, MSYS2, or Cygwin

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

When the shell installer detects a Windows Unix-like environment, it can
delegate to the native PowerShell installer or install only inside the current
shell environment.

## Basic Usage

Show available commands:

```sh
cup help
```

List installed tools:

```sh
cup list
cup list --target windows-x64
```

Install a tool:

```sh
cup install compiler gcc@stable
cup install debugger gdb@stable
cup install formatter clang-format@stable
```

Install a compiler targeting Windows:

```sh
cup install compiler gcc@stable --target windows-x64
```

Remove an installed tool:

```sh
cup remove compiler gcc@stable
```

Set and inspect a default for a component:

```sh
cup default compiler gcc@stable
cup current compiler
```

Check and repair the local cup installation:

```sh
cup doctor
cup repair
```

Uninstall cup and all cup-managed data:

```sh
cup uninstall
```

`cup uninstall` does not remove PATH entries. Leaving the PATH entry in place is
safe; it will be reused if cup is installed again.

## Project Status

Cup is under active development. It is a [Coffee](https://coffee-clang.github.io/)
project.

- [Specification](specification.md) — command model, manifest model, state
  model, filesystem layout, install/remove flows, repair behavior, packaging
  assumptions and design boundaries.
- [Dependencies](dependencies.md) — dependencies for building `cup`, the
  component build infrastructure, Docker/MSYS2 builders, package tests, release
  publishing and runtime packaging notes.
- [GitHub](https://github.com/coffee-clang/cup)
