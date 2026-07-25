# Commands

This document describes the public command-line interface of `cup`. Use
`cup help` for the command list and `cup help <command>` for the exact syntax
accepted by the installed version.

## General syntax

```text
cup <command> [arguments] [options]
```

Command names, component names, tool names, releases, platforms and archive
formats are case-sensitive and use lowercase canonical values.

Common selectors use:

```text
<tool>@<release>
```

`stable` resolves through the active package catalog. A concrete version selects
that exact release.

Supported components currently include:

```text
compiler
linker
debugger
linter
formatter
language-server
analyzer
```

Platform identifiers use `<os>-<arch>`, for example `linux-x64`,
`macos-arm64` or `windows-x64`.

## Help and version

```sh
cup help
cup help <command>
cup --help
cup <command> --help
cup --version
```

Help and version queries are read-only and do not initialize the local runtime.

## Package discovery

### `search`

```sh
cup search
cup search <component>
cup search <component> --target <platform>
```

Shows packages available from the active catalog. It does not require packages
to be installed.

### `list`

```sh
cup list
cup list <component>
cup list --target <platform>
```

Shows installed packages for the current host. Output includes target scopes and
marks defaults, stable releases and entries that need attention.

### `info`

```sh
cup info
cup info <component>
cup info <component> --target <platform>
```

Shows configured defaults and the commands currently exposed for them. Without
filters, every relevant target scope is shown.

### `inspect`

```sh
cup inspect <component> <tool>@<release>
cup inspect <component> <tool>@<release> --target <platform>
```

Shows the verified metadata of one installed package. It does not search the
catalog or install a missing package.

## Installation preferences

### `config`

```sh
cup config
cup config --target <platform>
cup config set <component> <tool>
cup config reset <component>
cup config reset --target <platform>
```

The read-only form shows the effective tool selection for each component.
`set` stores a preferred tool for future abbreviated installs in one target
scope. `reset <component>` removes one preference; `reset` without a component
clears that scope.

Preferences affect abbreviated component installs and profiles. Explicit tool
selectors and curated toolchains are not changed by them.

## Installing packages

### `install`

```sh
cup install <component>
cup install <component> <tool>
cup install <component> <tool>@<release>
cup install <component> <tool>@<release> --target <platform>
cup install profile <name>
cup install toolchain <name>
```

When the tool is omitted, `cup` uses the scoped preference and then the official
default. When the release is omitted, `stable` is used.

A profile resolves its components through the current preferences. A toolchain
uses its defined set of tools. The complete group is validated before package
installation begins.

The first valid package installed for a component and target becomes its
default. Later installations in the same scope do not replace that default.
Installing a package that is already present and valid is a no-op.

`--format` or `-f` selects `tar.xz`, `tar.gz` or `zip` when more than one
format is available.

### `remove`

```sh
cup remove <component> <tool>@<release>
cup remove <component> <tool>@<release> --target <platform>
```

Removes one installed release. Other versions remain installed. A matching
default is cleared and the exposed commands are updated.

### `default`

```sh
cup default <component> <tool>@<release>
cup default <component> <tool>@<release> --target <platform>
```

Selects one already installed and valid package as the default for that
component and target. It never installs a missing package.

## Updating

### `update`

```sh
cup update
cup update <tool>
cup update <component>
cup update cup
```

Without a selector, `cup` updates installed tool scopes only; it does not update
the `cup` executable.

A tool or component selector installs the current stable package in every
matching installed scope. Older versions are retained. A default moves only
when it selected an older release of the same tool.

`cup update cup` checks for a newer official `cup` release. It is unavailable in
development builds. An equal version is a no-op and downgrades are rejected.

## Diagnosis and recovery

### `doctor`

```sh
cup doctor
```

Checks the installation, configuration, state, installed packages and exposed
commands. It is read-only and returns a nonzero status when issues are found.

### `repair`

```sh
cup repair
```

Recovers supported interrupted operations and rebuilds data that can be derived
safely. Ambiguous data is reported and preserved.

`repair` never removes or replaces the canonical `cup` or `cup.exe` executable.
Use the official installer when that executable is missing or altered.

### `uninstall`

```sh
cup uninstall
cup uninstall --yes
```

Removes the canonical installation and all packages managed by `cup`. The
operation prompts unless `--yes` is supplied and does not edit PATH.

## Concurrency and interrupted operations

Read-only commands may run together. Commands that modify the installation use
exclusive access and fail rather than racing another modification.

When an interrupted operation requires recovery, normal commands report it.
Use `cup doctor` to inspect the problem and `cup repair` for supported recovery.

## Exit status

```text
0    success
2    invalid command, option or argument
3    requested package or selection is unavailable
4    invalid catalog, state or metadata
5    network or download failure
6    filesystem, locking, archive or transaction failure
70   internal failure
130  interrupted operation
```

Diagnostics are written to standard error. Command results are written to
standard output.

## Related documents

- [INSTALLATION](INSTALLATION.md) — installation, update and uninstall;
- [PACKAGES](../design/PACKAGES.md) — package and catalog formats;
- [STATE](../design/STATE.md) — persistent state and defaults;
- [PLATFORMS](../design/PLATFORMS.md) — platform behavior.
