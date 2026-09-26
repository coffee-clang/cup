# Concepts

`cup` manages prebuilt C development tools through a small set of concepts. The
same terms appear in the CLI, package catalog, local state and documentation.

## Component and tool

A **component** is a role in a C development environment. `cup` currently knows:

```text
compiler
debugger
linker
formatter
linter
language-server
analyzer
```

A **tool** is one implementation of a component. The built-in relationships are:

| Component | Tools |
|---|---|
| compiler | `gcc`, `clang` |
| debugger | `gdb`, `lldb` |
| linker | `lld`, `ld` |
| formatter | `clang-format` |
| linter | `clang-tidy` |
| language-server | `clangd` |
| analyzer | `valgrind` |

The executable's built-in registry defines which relationships are valid. The
package catalog decides which of those tools are actually available for a given
host, target and version.

## Package identity

One installed package is identified by five values:

```text
component + tool + host + target + concrete version
```

For example, a compiler package may be Clang running on `linux-x64`, targeting
`linux-x64`, at one concrete release. A Linux-hosted Windows cross compiler is a
different package because its target is different.

`cup` can keep more than one concrete version of the same tool and scope at the
same time.

## Host and target

The **host** is where the package executables run. The **target** is the platform
the tool handles or produces code for.

```text
host=linux-x64  target=linux-x64    native package
host=linux-x64  target=windows-x64  cross-target package
```

The target defaults to the current host when `--target` is omitted. Cross-target
packages exist only where the installed catalog publishes them.

## `stable` and concrete versions

`stable` is a catalog selector, not an installed version. `cup` resolves it to the
catalog's current stable release before creating package paths or state records.

```sh
cup install clang@stable
```

After installation, the package keeps the concrete version that was resolved at
that time even if the catalog later changes its stable release.

The catalog defines the available versions and which one is `stable`. `cup` uses
its package-version ordering only to compare concrete releases; it does not derive
`stable` from the version list.

## Preferences and defaults

These two settings solve different problems.

A **preference** answers:

> Which tool should an abbreviated future install choose for this component?

For example:

```sh
cup config set compiler gcc
cup install compiler
```

A **default** answers:

> Which already installed package currently provides this component's commands?

For example:

```sh
cup default compiler gcc@stable
```

Preferences do not change installed packages or defaults. Installing another
version also does not silently replace an existing default. The first valid
package installed in an empty component/target scope becomes its default.

## Profiles and toolchains

A **profile** is a list of components. `cup` resolves each component using the
same preference → official-default rule as `cup install <component>`.

Built-in profiles are:

```text
minimal   compiler, linker
standard  compiler, linker, debugger, language-server
extended  compiler, linker, debugger, language-server, formatter, linter
```

A **toolchain** is a curated list of explicit tools. It does not consult user
preferences.

```text
llvm  clang, lldb, lld, clang-format, clang-tidy, clangd
gnu   gcc, gdb, ld
```

Availability is still checked against the selected host/target catalog. `cup`
preflights a group before installing its first package; it does not silently
remove unavailable members from a preset.

## Defaults and provided commands

Packages declare the commands they provide. `cup` derives launchers in its own
`bin` directory from the current defaults.

For a native target, the launcher uses the package entry name. For a cross target,
the target platform is prefixed so native and cross-target commands can coexist:

```text
gcc
clang
windows-x64-gcc
```

These launchers are derived data. `cup doctor` checks them and `cup repair` can
rebuild them from valid defaults.

## The `cup` root

`cup` keeps application state, packages, cache data and launchers below one
user-managed root. The normal location is `.cup` below the user's home/profile;
`.coffee-cup` is the deterministic fallback when `.cup` already belongs to
something else.

The root is authenticated by `cup`'s ownership marker. Once installed, `cup` derives
the active root from its own executable rather than from a persistent `CUP_HOME`
environment variable. The complete root can therefore be relocated to another
user-manageable base as long as its managed leaf remains valid.

## Package producer and package manager

`cup` does not build GCC, LLVM or the other tools during `cup install`.
`cup-components` produces complete packages and publishes their checksums and
metadata. `cup` selects, downloads, verifies, extracts and installs those packages.

This split keeps tool-specific build knowledge in the producer and package/state
management in `cup`. See [Packages](../design/PACKAGES.md) for the package contract.

## Recovery model

Commands that can be interrupted keep recovery information until the result can be
determined safely. Package update installs a newer immutable package through the same
installation path as an ordinary package. Self-update and uninstall may continue in a
separate process after the current `cup` process exits.

Normal state-changing commands stop when recovery information is pending. `cup doctor`
is read-only and reports the condition; `cup repair` changes files only when the
saved state and filesystem give one safe result.

For implementation details, see [Transactions and recovery](../design/TRANSACTIONS.md).
