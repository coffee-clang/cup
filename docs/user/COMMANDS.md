# Commands

This document defines the public command-line interface. Architecture and
persistent formats are specified separately in [ARCHITECTURE](../design/ARCHITECTURE.md),
[PACKAGES](../design/PACKAGES.md), [STATE](../design/STATE.md) and
[TRANSACTIONS](../design/TRANSACTIONS.md).

## General syntax

```text
cup --version
cup help [command]
cup search [<component>] [--target <target-platform>]
cup list [<component>] [--target <target-platform>]
cup install <component> [<tool>[@<release>]] [--target <target-platform>] [--format|-f <archive-format>]
cup install profile <name> [--target <target-platform>] [--format|-f <archive-format>]
cup install toolchain <name> [--target <target-platform>] [--format|-f <archive-format>]
cup config [--target <target-platform>]
cup config set <component> <tool> [--target <target-platform>]
cup config reset [<component>] [--target <target-platform>]
cup update [cup|<tool>|<component>]
cup remove <component> <tool>@<release> [--target <target-platform>]
cup default <component> <tool>@<release> [--target <target-platform>]
cup info [<component>] [--target <target-platform>]
cup inspect <component> <tool>@<release> [--target <target-platform>]
cup doctor
cup repair
cup uninstall [--yes]
```

Argtable3 parses and validates each command's positional arguments and options.
The command name itself selects the corresponding argument table. Unknown
commands, unknown options, missing required arguments and excess arguments are
rejected before command execution. Profile, toolchain, component, tool, target
and symbolic `stable` values entered through the CLI are normalized to
lowercase. Concrete version identifiers remain case-sensitive; command and
option names are case-sensitive.

## Entry format

Commands that select a package use:

```text
<tool>@<release>
```

Examples:

```text
gcc@stable
gcc@16.1.0-rev1
gdb@17.1
clang@22.1.5
valgrind@3.27.0
```

`stable` is symbolic. It is resolved through the active catalog before a
concrete package identity is built. Persistent state stores the resolved
version, never the symbolic input.

## Scope

Every package operation uses:

```text
component
host platform
target platform
tool
version
```

The host is detected from the running platform. The target defaults to the host
and can be selected with `--target`. The catalog must contain the exact host
and target tuple.

## Version and help

### `--version`

```sh
cup --version
```

Prints the build identifier embedded during compilation. Official builds print
the exact `VERSION`; development builds include Git or archive metadata. See
[RELEASES](../development/RELEASES.md#version-model).

### `help`

```sh
cup help
cup -h
cup --help
cup help install
cup install -h
cup install --help
```

With no argument, prints the command list, entry syntax and examples. Detailed
help includes usage, description, arguments, options, omission defaults,
examples and relevant side effects. An unknown command is an input error. Help
and version queries never initialize the CUP runtime.

## Catalog and installed views

### `search`

```sh
cup search
cup search compiler
cup search --target windows-x64
cup search compiler --target windows-x64
```

Reads the catalog and shows installable tools for the detected host. Without a
component, output is grouped by component and tool. Without `--target`, every
configured target for the host can be shown. A component or target narrows the
view.

`search` does not inspect local package directories and does not modify state.
It fails when the component is unknown or the active catalog cannot be loaded.

### `list`

```sh
cup list
cup list compiler
cup list --target windows-x64
cup list compiler --target windows-x64
```

Shows packages recorded as installed for the current host. Without `--target`,
it includes native and cross-target scopes. Output identifies the target and
annotates packages that are default, catalog-stable, missing or invalid.

`list` is read-only. It loads state and attempts to load the catalog so stable
annotations can be shown, but installed packages remain listable when a
catalog is unavailable. A syntactically or semantically invalid state file
fails the command before listing. Individual entries whose canonical package is
missing or invalid are retained in the listing with a status annotation so they
can still be diagnosed or removed.

### `info`

```sh
cup info
cup info compiler
cup info --target windows-x64
cup info compiler --target windows-x64
```

Shows configured defaults and their managed wrappers for the current host.
Without filters, every target scope is included. Each default must still point
to an installed and valid package, and the expected wrappers must match the
canonical `bin` directory.

`info` is the read-only view of default selection. `default` is the mutating
command.

### `inspect`

```sh
cup inspect compiler clang@stable
cup inspect compiler gcc@16.1.0-rev1 --target windows-x64
```

Resolves the requested package, requires it to be installed and valid, loads its
immutable `info.txt`, and prints all metadata fields. It is intended for exact
package details, not catalog search.

## Install preferences

### `config`

```sh
cup config
cup config --target windows-x64
cup config set compiler gcc
cup config set compiler gcc --target windows-x64
cup config reset compiler
cup config reset --target windows-x64
```

With no action, shows the effective tool, official default and selection source
for every component in the selected `host + target` scope. It also lists the
official profiles and curated toolchains. This view is read-only and does not
initialize `~/.cup` when the runtime is absent.

`set` creates or replaces one scoped preferred tool after validating the
component/tool relationship. `reset <component>` removes that preference in the
selected scope. `reset` without a component removes every preference in that
scope only. Preferences for other targets remain unchanged.

The official file uses:

```text
default.<host>.<target>.<component>=<tool>
```

The user-owned file uses:

```text
preferred.<host>.<target>.<component>=<tool>
```

Preferences affect abbreviated component installs and profiles only. They never
change installed defaults in `state.txt`, never alter an explicit toolchain plan
and are never consulted by `update`.

## Package mutation

### `install`

```sh
cup install compiler
cup install compiler gcc
cup install compiler gcc@16.1.0-rev1
cup install compiler gcc@stable --target windows-x64
cup install profile standard
cup install toolchain llvm
```

A direct component selection accepts an optional tool and release:

- omitting the release selects `stable`;
- omitting the tool resolves the effective install preference for that
  component;
- an explicit tool always overrides scoped preferences and official defaults.

The effective tool hierarchy for `cup install <component>` is:

```text
explicit CLI selector
scoped user preference
scoped official default from install.cfg
error when no selection exists
```

`install.cfg` is an official checksum-verified policy asset. It defines scoped
official defaults, component lists in profiles and explicit tool lists in
curated toolchains. `preferences.txt` is a separate mutable local overlay
managed by `cup config`. The compiled registry remains authoritative for
component/tool relationships, while `packages.cfg` remains authoritative for
packages actually available for one host, target and archive format.

Profiles intentionally resolve each listed component through the preference
hierarchy. Toolchains do not: their tool list is explicit, contains at most one
tool for each component and is independent of local preferences. Official
profile and toolchain names are stored in lowercase, while corresponding domain
values from the CLI are normalized to lowercase. CUP validates every item in a
profile or toolchain before it installs the first package and freezes the exact
version and archive format in the plan. A known tool that is absent from the catalog is reported as not
currently available and blocks the complete group.

After plan validation, each package uses the normal single-package transaction.
Packages that are already installed and valid are reported as already installed.
A recorded package whose files are missing or invalid causes a nonzero state error
and directs the user to `cup doctor` and `cup repair`. If a later download or
installation fails, completed package installations are retained and the summary
identifies where the group stopped; the group is not one global rollback transaction.

For each package, the command:

```text
validates component, tool, release, target and optional format
loads valid state and catalog, plus installation policy only when selector
resolution requires scoped defaults, profiles or toolchains
resolves stable and archive format
validates an existing package identity and treats it as idempotent only when intact
creates staging and a transaction journal
downloads or reuses a checksum-verified cache entry
extracts and validates one package root
protects info.txt read-only
moves the package to its canonical directory
adds the installed state entry
creates the first default in the scope when necessary
atomically saves state
rebuilds managed wrappers when the default changed
clears the journal after the operation is complete
```

The state replacement is the install commit point. A failed or interrupted
operation is rolled back or recovered according to
[TRANSACTIONS](../design/TRANSACTIONS.md#install-transaction).

The first package in a `component + host + target` scope becomes its default.
A later installation in the same scope does not replace that choice.

### `remove`

```sh
cup remove compiler gcc@stable
cup remove compiler gcc@16.1.0-rev1 --target windows-x64
```

Resolves the concrete version, requires a matching state entry, stages the
canonical package for removal, removes its installed entry, clears a matching
default, saves state and then removes the staged tree.

Removal remains available when state records a package whose canonical object
is invalid. This allows deterministic cleanup rather than making corruption
permanent. A missing state entry is still rejected.

See [TRANSACTIONS](../design/TRANSACTIONS.md#remove-transaction) for rollback and recovery.

### `default`

```sh
cup default compiler gcc@stable
cup default compiler gcc@16.1.0-rev1 --target windows-x64
```

Selects one installed and valid package as the default for exactly one
`component + host + target` scope. It builds and validates the complete
entry-point plan before saving the candidate state, then applies that same plan
after the state commit.

The command does not install a missing package. It rejects entry-point name
collisions and invalid package metadata before changing state.

### `update`

```sh
cup update gcc
cup update compiler
cup update cup
cup update
```

A tool selector scans every installed host/target scope of that tool. A
component selector scans every installed tool in that component. For each scope,
CUP reacquires the exclusive lock and revalidates that the scope still exists
before acting.

The catalog's concrete stable version is installed when absent. Older versions
are retained. A default moves only when it still belongs to the same tool; a
default changed concurrently to another tool is not overwritten.

`cup update cup` is available only in official release builds. CUP discovers a newer official
version through `latest`, rejects malformed metadata and downgrades, then
fetches the immutable versioned executable, uninstall helper, platform
checksums, catalog, `install.cfg` and common checksums. A detached helper
replaces the running generation after the process exits.

`cup update` with no selector updates every installed tool scope and does not
update CUP itself. `cup update cup` is the only form that updates the CUP
executable and official assets. Package-update atomicity remains per scope, so a
later scope failure does not undo stable packages already installed. Output
reports completed package updates, moved defaults and skipped scopes.

Development builds cannot update themselves as an official generation. An
equal official version is a no-op.

See [TRANSACTIONS](../design/TRANSACTIONS.md#cup-update-transaction) and
[SECURITY](../design/SECURITY.md#cup-update-assets).


## Diagnosis and recovery

### `doctor`

```sh
cup doctor
```

Read-only diagnosis. It checks:

- canonical CUP assets and runtime paths;
- uninstall and transaction markers;
- catalog and CUP assets checksums;
- executable and uninstall helper integrity;
- read-only and executable attributes;
- state syntax and semantics;
- state-to-package and package-to-state correspondence;
- package identities, metadata and declared executables;
- managed wrappers;
- direct temporary leftovers.

A failed inspection is reported as an incomplete check and causes a failing
result. `doctor` never repairs or quarantines data.

### `repair`

```sh
cup repair
```

Acquires the exclusive lock and applies only deterministic corrections. It can
recover a pending transaction, restore verified CUP assets that the
current build can replace safely, preserve invalid text files, reconstruct
state from valid canonical packages, remove
stale state entries and defaults, quarantine identifiable invalid packages,
restore permissions, clean temporary leftovers and rebuild package commands.

Ambiguous paths are reported and left unchanged. A truncated package scan or a
valid-package count that cannot fit in state stops reconciliation before data is
changed.

The distinction is strict:

```text
doctor  observes and reports
repair  changes only what can be derived deterministically
```

On Windows, a missing or altered canonical executable must be replaced by the
official installer; a development build cannot download an official CUP assets
generation. See [TRANSACTIONS](../design/TRANSACTIONS.md#doctor-and-repair).

### `uninstall`

```sh
cup uninstall [--yes]
```

Marks uninstall as pending and starts a detached platform helper. The helper
waits for the current process to exit, atomically detaches the canonical `.cup`
root and deletes the detached tree. It refuses a non-canonical root and does not
edit `PATH`.

See [INSTALLATION](INSTALLATION.md#uninstall) and
[TRANSACTIONS](../design/TRANSACTIONS.md#uninstall-protocol).

## Locking and pending operations

Read-only commands acquire a shared operating-system lock. Mutating commands
acquire an exclusive non-blocking lock for the relevant operation. Normal
commands reject a pending transaction or uninstall marker instead of acting on
an installation whose commit state may be unknown.

`doctor` can inspect a pending state. `repair` owns transaction recovery.

## Errors and exit status

Internal `CupError` values are mapped to a small stable public process-status
contract; enum numbering is not exposed:

```text
0    success
2    invalid command, option, argument or unsupported domain value
3    requested package or selection is unavailable/not installed
4    invalid catalog, state, metadata or other persistent consistency failure
5    network, TLS, timeout or download-size failure
6    filesystem, locking, transaction, archive, extraction or commit failure
70   internal invariant, buffer or cryptographic failure
130  operation cancelled by SIGINT
```

Read-only `list` and `info` may print every valid entry they can inspect and
still return status 4 when one or more entries are missing or invalid. Diagnostics
are written to standard error; command results and reports use standard output.

## Implementation and verification

Argtable3 parsing and command dispatch are owned by `main.c`. Shared command
setup is in `command_context.c`. Public handlers use singular `command_*.c`
modules, while reusable package installation lives in `package_install.c`.
`command_doctor.c`, `command_repair.c`, `cup_update.c` and
`command_uninstall.c` own the corresponding maintenance commands.

Public command behavior is exercised through the real executable by the POSIX
and Windows integration suites. Focused C tests cover deterministic branches
such as context setup, remove rollback, CUP-update validation and CUP assets
selection. See [TESTING](../development/TESTING.md) for the boundary between those levels.

## Related documents

- [PACKAGES](../design/PACKAGES.md) — catalog, versions, formats and `info.txt`;
- [STATE](../design/STATE.md) — defaults, state entries and wrappers;
- [TRANSACTIONS](../design/TRANSACTIONS.md) — commit and recovery rules;
- [PLATFORMS](../design/PLATFORMS.md) — target and platform behavior.
