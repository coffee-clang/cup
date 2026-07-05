# cup

`cup` is a user-space toolchain manager for C development tools.

It installs prebuilt tool archives under the user's home directory, records the local installation state, and lets the user select the default tool for each component without requiring administrator privileges or writing into system locations.

## What cup does

`cup` installs known component/tool pairs from prebuilt package archives described by `packages.cfg`.

For example:

```sh
cup install compiler gcc@stable
```

The command validates the requested component and tool, resolves the symbolic release through the manifest, downloads the selected archive, extracts it into a temporary staging directory, validates the package metadata, commits the package into `~/.cup/components`, and records the installation in `~/.cup/state.txt`.

The current registry accepts these component/tool pairs:

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

A tool must be accepted by the internal registry and configured in the manifest for the current host and requested target to be installable.

## Supported platform identifiers

`cup` uses platform identifiers in this form:

```text
<os>-<arch>
```

The current implementation recognizes:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

A package lookup uses two platform values:

```text
host platform
  where the installed tool runs

target platform
  what the installed tool targets
```

The host platform is detected by `cup`. The target platform defaults to the host and can be overridden with `--target` when a package is available for that tuple.

Examples:

```sh
cup install compiler gcc@stable
cup install compiler gcc@stable --target windows-x64
```

## Installation

Bootstrap assets for `cup` are published in the `coffee-clang/cup` repository. The installer installs the release build of the `cup` executable produced by the project static dependency configuration, together with the manifest and uninstall scripts.

### Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

The installer belongs to one release. The documented `latest` URL fetches the installer from the latest stable release, but the installer itself downloads the binary, manifest, checksum files and uninstall script from its own immutable `vX.Y.Z` release URL and verifies the published SHA-256 files before committing them. It first creates only the bootstrap portion of the canonical root:

```text
~/.cup/bin
~/.cup/config
~/.cup/scripts
```

The complete runtime tree, including `components`, `tmp`, `cache`, `state.txt` and `cup.lock`, is initialized by the first operational command or by `cup repair`. The root is always `~/.cup` and is not configurable. The manifest, checksum files and uninstall script are installed read-only. The installer can also add `~/.cup/bin` to the shell `PATH`.

### Windows PowerShell

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 -OutFile $env:TEMP\install-cup.ps1; powershell -NoProfile -ExecutionPolicy Bypass -File $env:TEMP\install-cup.ps1"
```

The installer belongs to one release. The documented `latest` URL fetches the installer from the latest stable release, but the installer itself downloads the executable, manifest, checksum files and uninstall script from its own immutable `vX.Y.Z` release URL and verifies the published SHA-256 files before committing them under `%USERPROFILE%\.cup`. The complete runtime tree, including state and lock files, is initialized by the first operational command or by `cup repair`. The manifest, checksum files and uninstall script are protected with the Windows read-only attribute.

It can also add `%USERPROFILE%\.cup\bin` to the user `PATH`.

### Windows cmd.exe

From `cmd.exe`, call the PowerShell installer:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -Command "iwr https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 -OutFile $env:TEMP\install-cup.ps1; powershell -NoProfile -ExecutionPolicy Bypass -File $env:TEMP\install-cup.ps1"
```

### Windows Git Bash, MSYS2 or Cygwin

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

When the shell installer detects a Windows Unix-like environment, it delegates to the native PowerShell installer so the canonical root remains `%USERPROFILE%\.cup`.

## Basic usage

Show general or command-specific help and the embedded version:

```sh
cup help
cup help install
cup --version
```

List installed tools for the current host and selected target, optionally by component:

```sh
cup list
cup list compiler
cup list --target windows-x64
```

Install tools:

```sh
cup install compiler gcc@stable
cup install compiler clang@stable
cup install debugger gdb@stable
cup install linker lld@stable
```

The first installation in a `component + host + target` scope becomes its default. Later installations in the same scope do not replace that choice automatically.

Install a compiler targeting Windows from a Linux host when a matching package is available:

```sh
cup install compiler gcc@stable --target windows-x64
```

Choose a persistent default and inspect the configured defaults:

```sh
cup default compiler gcc@stable
cup info
cup info compiler
cup info --target windows-x64
```

`cup default` only changes the selected package for one `component + host + target` scope. `cup info` is the read-only view: without filters it lists every configured target scope for the current host, and it can be restricted by component or target. It also verifies and prints the managed commands exposed through `~/.cup/bin`. Native defaults use their declared entry names, while cross-target defaults use `<target>-<entry>` names to avoid collisions.

Search the manifest catalog or inspect immutable metadata from an installed package:

```sh
cup search
cup search compiler
cup inspect compiler gcc@stable
```

Update installed tools to the manifest `stable` release without deleting older versions:

```sh
cup update gcc
cup update compiler
```

If the previous default belonged to an updated tool, the matching stable package becomes the new default for that scope.

Update the canonical `cup` executable and its verified platform bootstrap assets:

```sh
cup self-update
```

Remove a package:

```sh
cup remove compiler gcc@stable
```

Check and repair the local `cup` tree:

```sh
cup doctor
cup repair
```

`doctor` is read-only and reports structural, state, package, bootstrap checksum, permission and interrupted-transaction problems. It also reports checks that could not be completed instead of treating the diagnosis as complete. `repair` applies only deterministic corrections, including journal recovery, verified bootstrap recovery, state/component reconciliation and quarantine of canonically identifiable invalid packages under `~/.cup/recovery/`. Ambiguous paths are reported and left unchanged.

Uninstall `cup` and all cup-managed data:

```sh
cup uninstall
```

`cup uninstall` records an uninstall marker and starts a helper that waits for the current `cup` process to exit. The helper atomically detaches the canonical `.cup` directory before deleting it, so another process cannot observe a partially removed installation. New commands refuse to start while the marker exists. The command does not remove PATH entries; a remaining entry can be reused by a future installation. Running the installer again removes a stale uninstall marker after restoring a valid bootstrap.

When `cup` is executed from the repository root during development, it can use `config/packages.cfg` and the platform uninstall script under `scripts/install/` if the installed bootstrap files are unavailable. Installed files remain the preferred source.


## Versioning and releases

The repository root contains a manually maintained `VERSION` file. A build is an official release only when it is explicitly built in release mode from a clean commit tagged `vX.Y.Z`, and the tag exactly matches `VERSION`. Development builds include the Git distance, abbreviated commit and optional dirty marker; source archives without Git metadata use the explicit `-dev+archive` suffix.

Normal pushes to `main` run source CI. The release workflow also starts on `main`, but it publishes only when `VERSION` names a release tag that does not already exist. In that case it tests the source, builds the static release assets, tests those exact generated assets on native runners, creates tag `vX.Y.Z` and then publishes the GitHub Release. Repeated pushes with the same `VERSION` do not recreate or overwrite an existing release.

`self-update` is available only in official builds. It uses the latest release metadata to discover an official version, then downloads checksums and assets from the immutable `vX.Y.Z` release URL. Development builds are rejected, downgrades are not applied, and every replacement remains checksum-verified and transactional.

## Testing

Run the POSIX regression suites with:

```sh
make test
```

All repository test code lives under `scripts/tests/`. The top-level entry points are `unit.sh`, `integration.sh`, `release.sh` and `all.sh`; the subdirectories hold the actual suites:

```text
scripts/tests/unit/         C unit tests and small repository-policy shell tests
scripts/tests/integration/  POSIX and Windows CLI integration tests
scripts/tests/release/      checks for generated release candidates
scripts/tests/support/      shared test helpers
scripts/tests/workflow/     thin GitHub Actions helper scripts
```

C unit tests cover internal modules such as checksum, manifest parsing, text/path/entry parsing and package metadata loading. Shell and PowerShell integration tests exercise `cup` as a real CLI with isolated homes and fixture packages. Source tests and release-asset tests are separate. Release publication occurs only after the exact static assets pass the native matrix.

## Component packages

`cup` does not build GCC, Clang, GDB, LLDB, LLD or other development tools locally during installation. It consumes prebuilt component archives described by `config/packages.cfg`.

Those archives are produced and released by the separate `cup-components` repository. This repository is responsible for the `cup` executable, the manifest format, local installation state, package validation, SHA-256 verification and user-space installation logic; `cup-components` is responsible for building, testing and packaging the tool archives that `cup` downloads.

## Documentation

The full documentation is split into:

- [Specification](docs/specification.md): command model, manifest model, local state, filesystem layout, install/remove flows, package metadata and design boundaries.
- [Dependencies](docs/dependencies.md): installer dependencies, build dependencies for the `cup` executable, linked libraries, bootstrap scripts and release assets.

The component build infrastructure is documented in the separate [cup-components](https://github.com/coffee-clang/cup-components) repository.
