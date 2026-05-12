# cup

`cup` is a user-space manager for C development tools.

The project installs prebuilt tool archives described by a manifest, records installed packages in a local state file, and keeps its runtime data under the user's `.cup` directory. It does not install into system directories, does not require privileged operations, and does not try to replace the operating system package manager.

`cup` is intentionally small: a command selects a known component/tool pair, resolves a release through the manifest, downloads the matching archive, extracts it into a temporary staging directory, validates the extracted layout, commits the installation, and updates local state.

## Current scope

The current implementation provides:

- a fixed registry of supported C development tool components;
- manifest-driven version, archive format, and URL resolution;
- symbolic release resolution through `stable`;
- canonical internal entries such as `gcc@16.1.0`, `gcc@16.1.0-rev1`, and `gdb@17.1`;
- host/target-aware package lookup;
- package download through `libcurl`;
- archive extraction through `libarchive`;
- local archive caching;
- temporary staging for install and remove;
- rollback when state updates fail after filesystem commits;
- interrupt-aware download and extraction cleanup;
- local state stored in `~/.cup/state.txt`;
- one default entry per component/host/target pair;
- Linux and Windows builds of the `cup` executable;
- shell and PowerShell bootstrap installers for installing the prebuilt `cup` executable and manifest.

## Supported components and tools

The current registry supports:

```text
compiler/gcc
compiler/clang
debugger/gdb
debugger/lldb
linker/lld
```

These are the component/tool pairs accepted by the command line. A tool being present in `config/packages.cfg` is not enough by itself: it must also be accepted by the internal registry.

## Supported platforms

The platform identifiers currently used by the code and manifest are:

```text
linux-x64
windows-x64
```

Each package lookup uses two platform values:

```text
host_platform
  where the installed tool runs

target_platform
  what the installed tool targets
```

The host platform is detected by `cup`. The target platform defaults to the host unless `--target` is passed.

Examples:

```sh
cup install compiler gcc@stable
cup install compiler gcc@stable --target windows-x64
```

## Installation

Bootstrap assets are published in the GitHub release tag:

```text
cup-bootstrap
```

This release is separate from tool package releases such as `gcc-...`, `gdb-...`, `clang-...`, `lld-...`, and `lldb-...`. It contains the `cup` executable, the package manifest, and installer scripts.

### Linux

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

The installer downloads:

```text
cup-linux-x64
packages.cfg
```

and installs them as:

```text
~/.cup/bin/cup
~/.cup/config/packages.cfg
```

The installer can add `~/.cup/bin` to the shell `PATH`. Interactive prompts read from `/dev/tty`, so prompts still work when the script is executed through `curl | sh`.

### Windows PowerShell

```powershell
irm https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 | iex
```

The installer downloads:

```text
cup-windows-x64.exe
packages.cfg
```

and installs them as:

```text
%USERPROFILE%\.cup\bin\cup.exe
%USERPROFILE%\.cup\config\packages.cfg
```

It can also add `%USERPROFILE%\.cup\bin` to the user `PATH`.

### Windows cmd.exe

`cmd.exe` does not provide `sh`, so the Linux-style `curl ... | sh` command is not the native Windows command. Use PowerShell from `cmd.exe` instead:

```cmd
powershell -ExecutionPolicy Bypass -NoProfile -Command "irm https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.ps1 | iex"
```

### Windows Git Bash / MSYS2 / Cygwin

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh
```

When the shell installer detects a Windows Unix-like environment, it asks whether to run the native PowerShell installer or install only inside the current shell environment.

## Commands

### List installed tools

```sh
cup list
```

With an explicit target:

```sh
cup list --target windows-x64
```

If nothing is installed for the selected host/target pair, `cup` prints an empty-state message.

### Install a tool

```sh
cup install <component> <tool>@<release>
```

Examples:

```sh
cup install compiler gcc@stable
cup install compiler clang@stable
cup install debugger gdb@stable
cup install debugger lldb@stable
cup install linker lld@stable
```

With a target override:

```sh
cup install compiler gcc@stable --target windows-x64
```

With an archive format override:

```sh
cup install compiler gcc@stable --format tar.xz
cup install compiler gcc@stable -f tar.xz
```

If no format is provided, `cup` uses the manifest `default_format` for the selected component/tool/host/target tuple.

### Remove a tool

```sh
cup remove <component> <tool>@<release>
```

Examples:

```sh
cup remove debugger gdb@stable
cup remove compiler gcc@16.1.0
cup remove linker lld@stable
```

Removal is staged. `cup` first moves the installed directory to a temporary remove directory, updates the state, then deletes the temporary directory. If the state update fails, it attempts to move the installation back.

### Set a default

```sh
cup default <component> <tool>@<release>
```

Example:

```sh
cup default compiler gcc@stable
```

A default can only be set for an installation that exists in both state and on disk. The stored value is canonical, so `gcc@stable` is resolved before it is saved.

### Show the current default

```sh
cup current <component>
```

Examples:

```sh
cup current compiler
cup current linker --target windows-x64
```

If the current default matches the manifest's stable version, the output may annotate it as stable while still relying internally on the concrete version.

## Entry model

User input uses this form:

```text
<tool>@<release>
```

Examples:

```text
gcc@stable
gcc@16.1.0
gcc@16.1.0-rev1
gdb@17.1
lld@22.1.5
```

`stable` is symbolic. It is resolved through the manifest before the state is changed.

The state stores canonical entries only:

```text
gcc@stable -> gcc@16.1.0
gcc@stable -> gcc@16.1.0-rev1
lld@stable -> lld@22.1.5
```

The exact version depends on the manifest entry for the selected host/target pair.

## Filesystem layout

The installed runtime layout is under the user's home directory:

```text
~/.cup/
  bin/
    cup
    cup.exe
  config/
    packages.cfg
  cache/
  components/
  tmp/
  state.txt
```

Tool installations use this layout:

```text
~/.cup/components/<component>/<tool>/<host_platform>/<target_platform>/<version>/
```

Examples:

```text
~/.cup/components/debugger/gdb/linux-x64/linux-x64/17.1/
~/.cup/components/linker/lld/windows-x64/windows-x64/22.1.5/
```

The temporary directory is used for install and remove staging. Temporary names include the operation, component, tool, version, and process-specific/random data to avoid collisions.

## Manifest lookup

`cup` looks for the manifest in this order:

```text
./config/packages.cfg
~/.cup/config/packages.cfg
```

The first path supports development from a repository checkout. The second path supports bootstrap installations.

If no manifest is found, `cup` prints an error explaining how to install the bootstrap assets.

## Package archives

Archives are expected to contain a usable tool directory and an `info.txt` metadata file. During extraction, `cup` normalizes the common top-level archive directory so the final installation path has the expected structure.

A valid installation must include a `bin` directory. This is the current generic validation rule.

## Building locally

Build the static dependencies first:

```sh
scripts/bootstrap-linux-deps.sh
```

Then build `cup` for Linux:

```sh
make PLATFORM=linux-x64
```

For the Windows executable, use the Windows dependency bootstrap and the Windows platform target:

```sh
scripts/bootstrap-windows-deps.sh
make PLATFORM=windows-x64
```

Build outputs:

```text
build/linux-x64/bin/cup
build/windows-x64/bin/cup.exe
```

More details are in `DEPENDENCIES.md`.
