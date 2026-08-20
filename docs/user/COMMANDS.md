# Commands

This page describes the public CLI. The installed executable is the final
reference, so `cup help` and `cup help <command>` should always match the version
being used.

## Basic syntax

```text
cup <command> [arguments] [options]
```

Command names are lowercase and case-sensitive. Components, tools, platforms
and archive formats accept ASCII case differences and are normalized to
lowercase. Concrete version strings are case-sensitive and must already use
the canonical lowercase form published by the catalog.

Package selectors use:

```text
<tool>@<release>
```

`stable` selects the stable version from the package catalog. A specific
version, such as `gcc@16.1.0-rev1`, refers only to that exact version.

Supported components are:

```text
compiler
debugger
linker
formatter
linter
language-server
analyzer
```

Platforms use `<os>-<arch>`, for example `linux-x64`, `macos-arm64` and
`windows-x64`. The **host** is the platform where `cup` is running; the
**target** is the platform handled by the selected tool. They are normally the
same.

## Help and version

```sh
cup help
cup help <command>
cup --help
cup <command> --help
cup --version
```

These forms do not initialize or modify the local installation. They remain
available even when the installation needs repair.

## Finding packages

### `search`

```sh
cup search
cup search <component>
cup search <component> --target <platform>
```

Shows packages available in the current catalog. Installed state is not
required.

### `list`

```sh
cup list
cup list <component>
cup list --target <platform>
cup list <component> --target <platform>
```

Shows packages installed for the current host. The output includes the target,
version and default annotation. Entries that cannot be validated are reported.

### `info`

```sh
cup info
cup info <component>
cup info <component> --target <platform>
```

Shows the default package for each requested component, host and target, along
with the commands provided by that package.

### `inspect`

```sh
cup inspect <component> <tool>@<release>
cup inspect <component> <tool>@<release> --target <platform>
```

Reads the metadata of one installed package. A specific version is looked up
locally. With `stable`, `cup` first resolves the current stable version from the
catalog and then checks that the resulting package is already installed.
`inspect` never downloads a package.

## Installation preferences

### `config`

```sh
cup config
cup config --target <platform>
cup config set <component> <tool>
cup config set <component> <tool> --target <platform>
cup config reset <component>
cup config reset <component> --target <platform>
cup config reset
cup config reset --target <platform>
```

The read-only form shows the effective tool selected for abbreviated installs.

`config set` stores a preference for one component and target. Commands such as
`cup install compiler` and profile installs use that preference. If no user
preference exists, `cup` uses the official default.

`config reset <component>` removes one preference. `config reset` without a
component removes all preferences for the selected target.

Explicit selectors and curated toolchains do not depend on user preferences.

## Installing packages

### `install`

```sh
cup install <tool>
cup install <tool>@<release>
cup install <component>
cup install <component> <tool>
cup install <component> <tool>@<release>
cup install <component> <tool>@<release> --target <platform>
cup install profile <name>
cup install toolchain <name>
```

When only a tool is supplied, `cup` infers its component from the built-in
registry. The tool must belong to exactly one component.

When only a component is supplied, `cup` uses the configured preference and
then the official default. When no release is supplied, `stable` is used.

Profiles select one tool for several components using the current preferences.
Toolchains use fixed curated selections. `cup` resolves and validates the
complete group before installing its first package.

Group installs are sequential rather than all-or-nothing. If a later package
fails, packages that completed earlier remain installed and `cup` reports the
partial result. If `cup` cannot determine whether the current package completed,
run `cup repair` before another command that changes state.

The first valid package installed for a component, host and target becomes the
default for that combination. Installing another version does not silently
replace that default. Installing a package that is already present and valid
succeeds without changing it.

The optional archive format can be selected with:

```sh
--format tar.xz
--format tar.gz
--format zip
```

The short form is `-f`. The selected format must be available for that package.

### `remove`

```sh
cup remove <tool>
cup remove <tool>@<release>
cup remove <component> <tool>
cup remove <component> <tool>@<release> --target <platform>
```

The command removes one installed version.

If the release is omitted, `cup` proceeds only when exactly one installed
version matches the selected tool, host and target. With no match it reports
that the package is not installed. With more than one match it prints the
installed versions and requires an explicit release. Nothing is removed in the
ambiguous case.

Removing a default clears that default and updates the commands provided by the
remaining defaults. Other installed versions are left in place. Profiles and
toolchains are not removal units.

### `default`

```sh
cup default <component> <tool>@<release>
cup default <component> <tool>@<release> --target <platform>
```

Selects an installed package as the default for one component, host and target.
A specific version is looked up in local state. `stable` is resolved through the
current catalog and must already be installed. The command does not install
missing packages.

## Updating

### `update`

```sh
cup update
cup update <tool>
cup update <component>
cup update cup
```

`cup update` updates installed tools to the current stable release for each
matching host and target. It does not update the `cup` executable.

A tool or component selector limits the operation to matching installed
packages. Old versions are retained. A default moves only when it already
selected the same tool at an older version.

`cup update cup` checks the official release metadata and installs only a newer
official version. Development builds cannot use this operation. Equal versions
are ignored and downgrades are rejected.

If `cup` cannot determine safely whether the replacement completed, `doctor`
reports the recovery condition. `repair` uses the recorded recovery information
when the safe result can be determined.

## Diagnosis and recovery

### `doctor`

```sh
cup doctor
```

Checks the local `cup` installation, stored state, preferences, installed
packages, defaults and the commands provided by those defaults. It is read-only.
A non-zero exit status means that at least one problem was found.

### `repair`

```sh
cup repair
```

Handles recovery cases that `cup` can resolve safely. It can recover interrupted
package or `cup update cup` operations, resolve a stale pre-detach uninstall
handoff when its ownership and inactivity can be proved, preserve invalid
packages for diagnosis, repair stored state and rebuild commands from valid
defaults.

Unknown or ambiguous data is left untouched and reported. `repair` never
replaces the main `cup` or `cup.exe` executable; use the official installer when
that executable is missing or damaged.

### `uninstall`

```sh
cup uninstall
cup uninstall --yes
```

Removes the selected `cup` installation and every package inside it. The command
asks for confirmation unless `--yes` is supplied. PATH is not modified.

## Input validation

Invalid options, overlong values, malformed selectors and unsupported platforms
are rejected before an operation changes local state. Help and version remain
available even when the installation needs repair.

Read-only queries can still show useful local information when optional catalog
data is unavailable, but they report the catalog error instead of claiming full
success.

## Concurrency and interruption

Help and version do not depend on the local installation state. Read-only
commands may run at the same time. Commands that change state run one at a time
and fail when another state-changing command is already active.

A pending recovery condition blocks normal commands that could conflict with it.
`doctor` reports the condition and `repair` is the public command that can
resolve it.

## Exit status

```text
0    success
2    invalid command, option or argument
3    requested package or selection is unavailable
4    invalid catalog, state or metadata
5    network or download failure
6    filesystem, locking, archive or recovery failure
70   internal failure
130  interrupted operation
```

Normal results use standard output. Diagnostics use standard error.

## Related documents

- [Installation](INSTALLATION.md)
- [Packages](../design/PACKAGES.md)
- [State](../design/STATE.md)
- [Transactions](../design/TRANSACTIONS.md)
- [Platforms](../design/PLATFORMS.md)
