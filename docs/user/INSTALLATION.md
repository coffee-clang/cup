# Installation

`cup` is installed for the current user. It does not require administrator
rights and does not write to system toolchain directories.

## Supported platforms

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

The normal installation root is:

```text
Linux/macOS ~/.cup
Windows %USERPROFILE%\.cup
```

If `.cup` already belongs to another application, `cup` leaves it unchanged and
uses `.coffee-cup` in the same home directory. The selected root is reused by
later commands and is not configurable through an environment variable.

## Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

The executable is normally placed at:

```text
~/.cup/bin/cup
```

The installer downloads the official release over HTTPS, verifies the release
metadata and checksums, and then runs the verified `cup` executable to complete
the installation. It reports success only after the installed command can be
validated.

## Windows PowerShell

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 | iex"
```

The executable is normally placed at:

```text
%USERPROFILE%\.cup\bin\cup.exe
```

The PowerShell installer applies the same download and verification rules and
the same checks for choosing and protecting the installation root. On Windows,
`cup uninstall` also starts the built-in Windows PowerShell process briefly to
keep temporary-helper deletion separate from the helper itself. PowerShell does
not remove the managed CUP files.

## Other Windows shells

Git Bash, MSYS2 and Cygwin may start the shell installer (`install.sh`). On
Windows it hands the installation to PowerShell so the same Windows installation
root is used.

From `cmd.exe`, the PowerShell installer can be started explicitly:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 | iex"
```

## Verification and download limits

Official installers verify release metadata and SHA-256 checksums before
installing executable or configuration data. Remote requests and redirects must
stay on HTTPS.

Downloads also have time and size limits so a stalled connection or an
unexpectedly large response cannot run indefinitely. Package archives have a
larger size limit than release metadata because complete toolchains can be much
larger.

After installation, check the result with:

```sh
cup --version
cup doctor
```

## PATH

`cup` does **not** edit the system or user PATH automatically.

To run the command without its full path, add the selected directory manually:

```text
Linux/macOS <cup-root>/bin
Windows <cup-root>\bin
```

Open a new shell after changing PATH. `cup uninstall` leaves that PATH entry in
place, so remove it manually when it is no longer needed.

## Existing and unrecognized directories

`cup` never takes ownership of an existing directory unless it can verify that
the directory belongs to a supported `cup` installation.

If an unrecognized cup-like directory blocks installation, move it to a backup
name outside `.cup` and `.coffee-cup`, then run the current official installer.
Recover only data accepted by the current formats. Do not copy unknown recovery
files into the new installation, and do not create `root.txt` manually.

## Reinstallation

Running the official installer again replaces the `cup` program files only
after the new release has been verified. Installed component packages,
preferences and state remain in the selected root.

Reinstallation is also the supported recovery method when `cup` or `cup.exe` is
missing or has been changed. `repair` intentionally does not recreate the main
executable.

## Updating cup

```sh
cup update cup
```

This command checks the official release metadata and installs only a newer
official version. Development builds cannot update themselves as official
releases. Equal versions are ignored and downgrades are rejected.

If an interrupted update leaves a recoverable condition, inspect it with
`cup doctor` and use `cup repair` when instructed.

Package updates are separate:

```sh
cup update
cup update <tool>
cup update <component>
```

## Uninstall

```sh
cup uninstall
cup uninstall --yes
```

Without `--yes`, `cup` asks for confirmation. Uninstall first moves the managed
root away from its normal `.cup` or `.coffee-cup` path, then removes that detached
tree after the original command has exited. This avoids deleting the directory
in place while cup is still running from it.

PATH is not changed. If cleanup fails after the move, cup leaves the detached
directory and its recovery information in place instead of guessing what is safe
to delete. A later installation does not automatically adopt or remove that
residue. The temporary helper executable has a separate cleanup lifecycle, so a
failure while removing the detached root does not intentionally leave that
helper behind.

## Troubleshooting

```sh
cup doctor
cup repair
```

`doctor` only reports problems. `repair` changes files only when it can determine
a safe recovery result; unknown or ambiguous data is preserved.

For more detail, see:

- [Commands](COMMANDS.md)
- [State](../design/STATE.md)
- [Transactions](../design/TRANSACTIONS.md)
- [Security](../design/SECURITY.md)
