# Commands

`cup help` is the version-specific command index shipped by the executable.
`cup help <command>` shows the accepted syntax, defaults and effects for one
command. This page explains the same interface with the behavior that matters
when commands are combined.

## CLI model

```text
cup <command> [arguments] [options]
```

Command names are lowercase and case-sensitive. Components, tools, platforms
and archive formats accept ASCII case differences and are normalized to
lowercase. Concrete package version strings are case-sensitive identifiers.

A package selector is:

```text
<tool>@<release>
```

`<release>` may be a concrete catalog version or `stable`. Omitting a release
from an install means `stable`. `stable` is resolved before anything is stored;
installed packages always have concrete versions.

Platforms use `<os>-<arch>`, for example `linux-x64`, `macos-arm64` and
`windows-x64`. The target defaults to the current host where the command permits
`--target`.

For the terminology behind these arguments, see [Concepts](CONCEPTS.md).

## Command overview

| Command | Purpose | Changes the `cup` root? |
|---|---|---|
| `help` / `--version` | CLI help and build identity | no |
| `search` | show catalog packages | catalog refresh only |
| `list` | show installed packages | no |
| `info` | show defaults and provided commands | no |
| `inspect` | show metadata for one installed package | no |
| `config` | show or change install preferences | only `set`/`reset` |
| `install` | install a package, profile or toolchain | yes |
| `remove` | remove one installed package version | yes |
| `default` | choose the installed default for one scope | yes |
| `update` | update installed tools or `cup` itself | yes |
| `doctor` | diagnose the installation | no |
| `repair` | recover or rebuild safely derivable state | yes |
| `uninstall` | remove the selected `cup` root | yes |

## Help and version

```sh
cup help
cup help <command>
cup --help
cup <command> --help
cup --version
```

These forms do not need a healthy local installation and never initialize one.

## Find packages: `search`

```sh
cup search
cup search <component>
cup search <component> --target <platform>
```

Shows packages available from the current catalog. On an existing runtime `cup`
performs one best-effort catalog refresh first; if refresh fails but the local
snapshot is valid, it warns and shows that snapshot. From a source checkout
with no runtime root, search may read a developer-provided
`./config/catalog.cfg` snapshot without creating a root. That file is local input
from `cup-components` and is not tracked by `cup`.

Examples:

```sh
cup search compiler
cup search debugger --target windows-x64
```

## List installed packages: `list`

```sh
cup list
cup list <component>
cup list --target <platform>
cup list <component> --target <platform>
```

Shows installed packages for the current host, including target, concrete
version and default/stable annotations. An installed entry that cannot be
validated is reported rather than silently omitted.

## Show defaults: `info`

```sh
cup info
cup info <component>
cup info <component> --target <platform>
```

Shows the selected default package for each requested component/target and the
commands that package provides.

## Inspect one package: `inspect`

```sh
cup inspect <component> <tool>@<release>
cup inspect <component> <tool>@<release> --target <platform>
```

Validates one installed package's semantic metadata and prints its package,
platform, source/build, command and producer-described capability information.
A concrete version is resolved from local state. `stable` first resolves through
the current catalog and must already be installed.

`inspect` does not download a package and is intended as a lightweight metadata
query; full tree-integrity scanning belongs to `doctor`, `repair` and package
admission.

## Install preferences: `config`

```sh
cup config [--target <platform>]
cup config set <component> <tool> [--target <platform>]
cup config reset [<component>] [--target <platform>]
```

The view form shows the effective tool used by abbreviated component installs.

`config set` stores a preference for one component/target scope. The root supplies
host implicitly. It affects future `cup install <component>` and profile installs;
it does not change the current default or existing packages. If no preference
exists, install planning uses the official default compiled into that `cup`
generation.

`config reset <component>` removes one scoped preference. `config reset` without
a component clears every preference for the selected target.

Explicit tool selectors and curated toolchains do not use preferences.

## Install packages: `install`

Accepted forms are:

```sh
cup install <tool>[@<release>] [--target <platform>] [-f|--format <format>]
cup install <component> [--target <platform>] [-f|--format <format>]
cup install <component> <tool>[@<release>] [--target <platform>] [-f|--format <format>]
cup install profile <name> [--target <platform>] [-f|--format <format>]
cup install toolchain <name> [--target <platform>] [-f|--format <format>]
```

When a tool is supplied without its component, `cup` infers the unique component
from the built-in registry. When only a component is supplied, selection is:

```text
user preference -> official default -> error if neither exists
```

An omitted release means `stable`.

Profiles select several components through the same preference/default rule.
Toolchains contain explicit curated tools and ignore preferences. `cup` resolves
and validates a complete group before the first package is installed, so an
unavailable member does not cause a knowingly partial plan. Package commits are
still sequential: if a later package fails, earlier completed packages remain
installed.

The first valid package installed in an empty component/target scope may become
the default. Later installs do not replace that default automatically. Installing
an already valid package succeeds without changing it.

Supported archive overrides are:

```text
tar.xz
tar.gz
zip
```

The selected format must be published for that package.

Examples:

```sh
cup install clang
cup install compiler
cup install compiler gcc@stable
cup install profile standard
cup install toolchain llvm
cup install gcc --target windows-x64 --format tar.gz
```

## Remove a package: `remove`

```sh
cup remove [<component>] <tool>[@<release>] [--target <platform>]
```

Removes one installed package version. If the release is omitted, `cup` proceeds
only when exactly one installed version matches the selected tool/target.
With multiple matches it prints the candidates and requires an explicit release.

Removing the current default clears that default and reconciles the provided
commands. Other installed versions remain in place. Profiles and toolchains are
installation presets, not removal units.

## Select a default: `default`

```sh
cup default <component> <tool>@<release> [--target <platform>]
```

Selects an **already installed** package as the default for one
component/target scope. `stable` resolves through the current local catalog and
must already be installed. This command never installs a missing package.

Examples:

```sh
cup default compiler clang@stable
cup default compiler clang@23.1.0 --target linux-x64
```

## Update: `update`

```sh
cup update
cup update <tool>
cup update <component>
cup update catalog
cup update cup
```

Without a selector, `cup` updates installed tool families and does **not** update
`cup` itself. If at least one family matches, package update performs one required
catalog refresh before planning; no matching installed scope avoids network. A
tool or component limits the plan to matching installed families.

For each `(component, tool, target)` family, the reference is the active
same-tool default when present, otherwise the semantic maximum installed version.
A missing stable warns/skips that family, a lower stable never downgrades it, an
equal stable is integrity-checked and left alone, and a newer stable is installed
or adopted as another immutable identity.

`cup update catalog` refreshes only the live catalog snapshot. It does not update
packages or `cup` itself.

Old package versions are retained. A default advances only when it already
selects the same tool at an older release; updating does not switch a default
from one tool to another.

`cup update cup` is separate. Official builds check the `cup` release metadata and
install only a newer verified official version. Equal versions are ignored and
downgrades are rejected. Development builds cannot self-update as official
releases.

## Diagnose: `doctor`

```sh
cup doctor
```

Checks the installed `cup` generation, live catalog, local state, preferences,
packages, defaults, pending transactions and derived wrappers. It is strictly read-only. A nonzero exit
status means at least one problem was found or an inspection could not complete.

## Recover: `repair`

```sh
cup repair
```

`repair` handles states that can be reconstructed or recovered safely. It can,
for example, finish or undo an interrupted package operation, reconcile valid
packages with state, preserve invalid identifiable objects for diagnosis,
repair verifiable same-generation metadata/legal assets, restore a malformed
official live catalog from its authenticated release snapshot, and rebuild
wrappers from valid defaults. Development repair does not manufacture a catalog snapshot.

Ambiguous data is left untouched and reported. `repair` does not replace its own
main executable; reinstall `cup` when `cup`/`cup.exe` itself is missing or damaged.

## Uninstall: `uninstall`

```sh
cup uninstall
cup uninstall --yes
```

Removes the selected `cup` root and its installed packages. The command asks for
confirmation unless `--yes` is present. Cleanup continues through a detached
native helper after the initiating process exits, so successful return means the
handoff was accepted, not necessarily that every pathname has already vanished.

`cup` prints the detached recovery path used if cleanup later fails. PATH is not
changed.

## Concurrency and recovery

Help/version do not use the managed root. Read-only commands can share access to
one healthy runtime snapshot. State-changing commands require exclusive access
and fail when another mutation owns the installation.

A pending transaction blocks normal commands that could conflict with recovery.
`doctor` can inspect it and `repair` is the public command allowed to resolve it.

## Input validation

Malformed selectors, unsupported platforms/tools, invalid options and values
that exceed the documented internal limits are rejected before the requested
operation commits state. Read-only queries may still print useful information
when optional catalog data is unavailable, but they report that degraded result
instead of claiming full success.

## Exit status

| Status | Meaning |
|---:|---|
| `0` | success |
| `2` | invalid command, option or argument |
| `3` | requested package or selection is unavailable |
| `4` | invalid catalog, state, metadata or validated package state |
| `5` | network, TLS, timeout or download failure |
| `6` | filesystem, locking, archive, transaction, commit or recovery failure |
| `70` | internal failure |
| `130` | interrupted operation |

Normal results use standard output. Diagnostics use standard error.

## Related documentation

- [Getting started](GETTING_STARTED.md)
- [Concepts](CONCEPTS.md)
- [Installation](INSTALLATION.md)
- [Packages](../design/PACKAGES.md)
- [State](../design/STATE.md)
- [Transactions and recovery](../design/TRANSACTIONS.md)
