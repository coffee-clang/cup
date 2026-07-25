# Installation

`cup` is installed for the current user and does not require administrator
privileges.

```text
POSIX   ~/.cup
Windows %USERPROFILE%\.cup
```

The installation root is fixed. Package installation is documented in
[COMMANDS](COMMANDS.md).

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

The executable is installed as:

```text
~/.cup/bin/cup
```

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

The executable is installed as:

```text
%USERPROFILE%\.cup\bin\cup.exe
```

## Other Windows shells

Git Bash, MSYS2 and Cygwin may start the POSIX installer command. On Windows it
delegates installation to the native PowerShell installer, so every shell uses
the same `%USERPROFILE%\.cup` installation.

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

The installer can add the command directory to the user PATH:

```text
POSIX   ~/.cup/bin
Windows %USERPROFILE%\.cup\bin
```

Open a new shell after changing PATH. `cup uninstall` does not remove the PATH
entry; a later installation can reuse it.

## Reinstallation

Running the official installer again replaces the installed `cup` files only
after the new release has been verified. Reinstallation is also the supported
way to recover a missing or altered `cup` or `cup.exe` executable.

User preferences, installed packages and state remain under the same installation
root unless `cup uninstall` is used.

## Updating cup

```sh
cup update cup
```

This checks for a newer official release and updates the existing installation.
Development builds cannot update themselves as official releases.

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
unchanged.

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
