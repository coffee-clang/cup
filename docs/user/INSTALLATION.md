# Installation

`cup` is installed for the current user and does not require administrator
privileges.

The preferred installation root is `~/.cup` on POSIX and
`%USERPROFILE%\.cup` on Windows. If that path is already an unrelated
directory, the installer preserves it and uses `.coffee-cup` in the same home
directory. `root.txt` records which candidate CUP owns; the choice is automatic,
persistent and not configurable.

An installation created before `root.txt` is adopted only when the installer can
verify the complete installed CUP generation: canonical executable, matching
native update helper, uninstall helper, exact checksum sets, configuration
files and optional state. The installer never executes an unknown discovered
binary. A markerless directory with the canonical CUP executable but incomplete
or inconsistent generation evidence is preserved as a probable damaged legacy
root, and the alternative root is not selected silently. Familiar directory
names, `state.txt` or checksum files without that executable do not establish
ownership and leave the directory foreign. Package installation is documented
in [COMMANDS](COMMANDS.md).

## Supported platforms

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

## Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

The executable is normally installed as:

```text
~/.cup/bin/cup
```

The POSIX bootstrap deliberately uses only the shell plus a small command set
available on the supported desktop installations:

- the POSIX shell plus `uname`, `mkdir`, `mktemp`, `chmod`, `cp`, `mv` and `rm`;
- either `curl` with HTTPS protocol restrictions or GNU-compatible `wget`;
- either `sha256sum` or `shasum -a 256`.

The detached POSIX uninstaller uses the same shell contract plus `chmod`, `mv`,
`rm` and `rmdir`. The bootstrap and uninstaller do not require `awk`, `grep`, `sed`,
`find`, `wc`, `tr`, `cat` or `basename`. Required
commands are checked before the corresponding filesystem operation; downloader
and SHA-256 alternatives are diagnosed explicitly. Windows installation from
an MSYS2/Cygwin-like shell additionally requires `cygpath` and a supported
PowerShell executable. Release and CI scripts have a separate, broader tool
contract because they run in environments prepared by the workflows.

## Windows PowerShell

```powershell
$installer = Join-Path $env:TEMP "install-cup.ps1"
try {
    iwr https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 `
        -OutFile $installer -ErrorAction Stop
    powershell -NoProfile -ExecutionPolicy Bypass -File $installer
    if ($LASTEXITCODE -ne 0) {
        throw "cup installer failed with exit code $LASTEXITCODE"
    }
} finally {
    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
}
```

The executable is normally installed as:

```text
%USERPROFILE%\.cup\bin\cup.exe
```

## Other Windows shells

Git Bash, MSYS2 and Cygwin may start the POSIX installer command. On Windows it
delegates installation to the native PowerShell installer, so every shell uses
the same selected native Windows root.

From `cmd.exe`, run the PowerShell installer explicitly:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "iwr https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 -OutFile $env:TEMP\install-cup.ps1"
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File "%TEMP%\install-cup.ps1"
```

## Verification

Official installers verify release metadata and SHA-256 checksums before
installing files. Downloads use HTTPS.

After installation, verify the command with:

```sh
cup --version
cup doctor
```

## PATH

The installer can add the selected root's command directory to the user PATH:

```text
POSIX   <cup-root>/bin
Windows <cup-root>\bin
```

Open a new shell after changing PATH. `cup uninstall` does not remove the PATH
entry; a later installation can reuse it.

## Reinstallation

Running the official installer again replaces the installed `cup` files only
after the new release has been verified. Reinstallation is also the supported
way to recover a missing or altered `cup` or `cup.exe` executable.

User preferences, installed packages and state remain under the same selected
installation root unless `cup uninstall` is used.

## Updating cup

```sh
cup update cup
```

This verifies and schedules a newer official release. A native helper completes
the replacement after the initiating process exits. Successful completion leaves
no result sidecar; `cup --version` reports the installed version. A failed update
remains in `transaction.txt` for read-only diagnosis and explicit recovery with
`cup repair`. Development builds cannot update themselves as official releases.

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

The command removes the canonical installation and every package managed by
`cup`. Without `--yes`, confirmation is required. PATH configuration is left
unchanged. The detached helper moves the complete root to a uniquely named
sibling before deleting it; a later installer removes a residue only after
validating its root marker, canonical binary and uninstall transaction journal.

## Troubleshooting

```sh
cup doctor
cup repair
```

`doctor` reports problems without modifying files. `repair` applies only repairs
that can be determined safely. It never removes or replaces the canonical
`cup` or `cup.exe` executable; use the official installer when that executable
must be restored.

See [COMMANDS](COMMANDS.md) for command details and
[SECURITY](../design/SECURITY.md) for the verification model.
