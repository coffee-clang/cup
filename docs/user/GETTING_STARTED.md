# Getting started

This page takes a new `cup` installation from zero to a usable set of C tools. It
assumes no knowledge of `cup`'s state or package layout.

## 1. Install `cup`

Linux and macOS:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

Windows PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 | iex"
```

Then verify the installed executable:

```sh
cup --version
cup doctor
```

If `cup` is not on PATH, run it through the path printed by the installer or see
[Installation](INSTALLATION.md#path).

## 2. See what is available

```sh
cup search
```

`cup` groups tools by **component**. For example, GCC and Clang are compilers;
GDB and LLDB are debuggers. Availability depends on the platform where `cup` is
running (**host**) and the platform handled by the package (**target**).

Filter by component or target when needed:

```sh
cup search compiler
cup search compiler --target linux-x64
```

The [Concepts](CONCEPTS.md) page explains these terms in detail.

## 3. Install tools

The shortest install form names a tool:

```sh
cup install clang
```

The omitted release means `stable`. You can also let `cup` choose the configured
tool for a component:

```sh
cup install compiler
```

For a useful group of tools, install a profile:

```sh
cup install profile standard
```

The built-in profiles are:

```text
minimal   compiler + linker
standard  compiler + linker + debugger + language server
extended  standard + formatter + linter
```

A profile uses your configured preferences, falling back to `cup`'s official
default for each component. A **toolchain** is different: it names a curated,
fixed set of tools. For example:

```sh
cup install toolchain llvm
```

`cup` checks the whole group before installing the first package, but individual
packages are installed sequentially. If a later package fails, packages that
completed earlier remain installed.

## 4. Understand defaults and commands

`cup` may keep several versions or tools for the same component. One installed
package can be the **default** for each component/target scope.

Show installed packages and defaults:

```sh
cup list
cup info
```

The first package installed in an empty scope becomes its default. Later
installs do not silently replace it. Change the selection explicitly:

```sh
cup default compiler clang@stable
```

Defaults determine the commands `cup` exposes in its `bin` directory. Native
commands use their normal entry name; cross-target commands are prefixed with
the target platform to avoid collisions.

## 5. Choose tools for abbreviated installs

Preferences control what `cup install <component>` and profile installs select:

```sh
cup config
cup config set compiler gcc
cup install compiler
```

A preference does **not** change the current default and does not alter packages
already installed. Reset it with:

```sh
cup config reset compiler
```

## 6. Inspect and maintain the installation

Useful read-only commands are:

```sh
cup list
cup info
cup inspect compiler clang@stable
cup doctor
```

Update installed tools without removing old versions:

```sh
cup update
```

Update `cup` itself separately:

```sh
cup update cup
```

If an interrupted operation leaves recovery data, `cup doctor` reports the
condition and `cup repair` handles cases that can be resolved safely.

To remove `cup` and every package stored below its managed root:

```sh
cup uninstall
```

Uninstall deliberately leaves existing PATH configuration unchanged.

## Where to go next

- [Concepts](CONCEPTS.md) explains `cup`'s model and terminology.
- [Installation](INSTALLATION.md) covers custom bases, relocation and recovery.
- [Commands](COMMANDS.md) is the complete CLI reference.
