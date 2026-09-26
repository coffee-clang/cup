# Installation

`cup` installs entirely for the current user. It does not require `sudo`, UAC or
administrator rights and does not write tools into system compiler directories.

## Supported hosts

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Package availability is a separate question: a supported `cup` host may have only
some component/target combinations in the current catalog. Use `cup search` to
see what can be installed from the current catalog.

## Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

The default executable path is:

```text
~/.cup/bin/cup
```

The installer downloads the official release over HTTPS, verifies its release
metadata and SHA-256 checksum chain, and then runs the verified `cup` executable to
finish bootstrap. Download, verification and installation are shown as separate
phases; an interactive terminal may also show in-place transfer progress for the
main executable. Redirected/non-interactive output does not emit animation frames.
Installation is reported successful only after the installed version can be validated.

## Windows

From Windows PowerShell or `cmd.exe`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 | iex"
```

The default executable path is:

```text
%USERPROFILE%\.cup\bin\cup.exe
```

Git Bash, MSYS2 and Cygwin may also start `install.sh`; on Windows the shell
installer hands the actual installation to PowerShell so both entry points use
the same native root and verification path.

## Installation root

The default base is the current user's home/profile directory:

```text
Linux/macOS  $HOME
Windows      %USERPROFILE%
```

`cup` normally creates `<base>/.cup`. If that leaf already exists but is not a `cup`
root, it is preserved and `cup` uses `<base>/.coffee-cup` instead. The managed leaf
is fixed; choosing another base does not create an arbitrary root name.

Interactive installers can select another existing user-writable base.
Non-interactive installation can provide the same installer-only choice through
`CUP_INSTALL_BASE_DIR`. `cup` does not persist or consult a `CUP_HOME` override.

After installation, the running executable derives the active root from its own
real location and verifies the root marker. This lets a complete `cup` root be
moved to another user-manageable base while retaining the `.cup` or
`.coffee-cup` leaf.

## PATH

PATH is optional for `cup`'s correctness. The installer may offer to add
`<cup-root>/bin` to the **current user's** PATH configuration; it never changes a
system-wide/Machine PATH.

If the root is not on PATH, `cup` remains usable through its full executable path.
`cup doctor` reports the missing PATH entry and, when it can be represented
safely, prints a command that updates only the current shell session. `doctor`
does not apply the change itself.

Relocating a `cup` root does not rewrite PATH. `cup uninstall` also leaves existing
PATH configuration unchanged.

A root whose `bin` pathname contains the platform PATH separator (`:` on POSIX,
`;` on Windows) is still valid, but that pathname cannot be represented as one
PATH entry. In that case the installer skips automatic PATH integration and
`doctor` does not print a misleading PATH command.

## Verify the installation

```sh
cup --version
cup doctor
```

`cup --version` proves that the installed executable starts. `cup doctor` checks
the managed root, installed `cup` assets, state, packages, defaults and launchers
without modifying them.

For the first package workflow, continue with [Getting started](GETTING_STARTED.md).

## Reinstall

Running the official installer again verifies a complete release before
replacing the managed `cup` program files. Installed component packages,
preferences, state and a valid live catalog remain in the selected root.

The installer can update an older `cup` or reinstall the same release. It does not
silently replace a newer installed `cup` with an older release. Reinstallation is
also the supported recovery path when the main `cup`/`cup.exe` executable is
missing or damaged; `cup repair` does not recreate the executable that is
currently running it.

## Update `cup`

```sh
cup update cup
```

This operation checks the official release metadata and installs only a newer
official `cup` release. Development builds do not update themselves as official
releases. Equal versions are left unchanged and downgrades are rejected.

Updating installed tools is separate:

```sh
cup update
cup update <tool>
cup update <component>
```

Tool updates retain older package versions.

## Relocate an installation

A complete managed root can be moved to another user-manageable base. Move the
whole `.cup` or `.coffee-cup` directory, including `bin`, `components`, `config`
and the root marker. Do not copy individual state files into a new directory or
create `root.txt` manually.

After a move, invoke `cup` from the relocated `bin` directory and run:

```sh
cup doctor
```

If PATH still refers to the old location, update it explicitly; relocation does
not change shell configuration automatically.

## Existing or unrecognized directories

`cup` never takes ownership of a directory merely because it is named `.cup` or
contains familiar filenames. If an unrecognized `cup`-like directory blocks
installation, move that directory to a backup location outside the managed
`.cup`/`.coffee-cup` names and run the current installer again.

Only restore data that the current `cup` formats accept. Do not manufacture an
ownership marker to force adoption of an unknown tree.

A recognized `cup` 0.3.5 layout-1 root is not upgraded in place and is not bypassed
by silently switching to `.coffee-cup`. Back up anything you need, uninstall or
move the old managed root out of the way, then run the current installer for a
fresh current-layout `cup` root.

## Uninstall

```sh
cup uninstall
cup uninstall --yes
```

Without `--yes`, `cup` asks for confirmation. Uninstall removes the selected `cup`
root and the packages stored inside it, but does not edit PATH.

Cleanup continues after the initiating executable exits. A successful command means
that cleanup has started; final removal may finish shortly afterwards. If cleanup
cannot complete, `cup` prints where the remaining data can be recovered.

If such a recovery directory remains, preserve it until the failure is
understood. A later installer does not automatically adopt or delete it.

## Troubleshooting and recovery

Start with:

```sh
cup doctor
```

`doctor` is strictly read-only. If it reports a recoverable interrupted
operation, use:

```sh
cup repair
```

`repair` changes files only when current state and recorded recovery data give a
safe result. Unknown or ambiguous objects are preserved instead of guessed away.

If the main executable itself is missing or invalid, use the official installer
rather than `repair`.

For command semantics and exit statuses, see [Commands](COMMANDS.md). Internal
state and recovery rules are documented in [State](../design/STATE.md) and
[Transactions](../design/TRANSACTIONS.md).
