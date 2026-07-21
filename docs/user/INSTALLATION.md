# Installation

This document describes how the official CUP asset generation is installed,
reinstalled and removed. Package installation performed later by `cup install`
is documented in [COMMANDS](COMMANDS.md) and [TRANSACTIONS](../design/TRANSACTIONS.md).

## CUP assets model

The public installer installs only the files required to start `cup` safely:

```text
.cup/
  bin/
  config/
  helpers/
```

The first operational command, or `cup repair`, creates the remaining runtime
layout such as `components`, `cache`, `staging`, `state.txt` and `cup.lock`. The
complete layout is documented in [STATE](../design/STATE.md).

The root is fixed:

```text
POSIX   ~/.cup
Windows %USERPROFILE%\.cup
```

It is not derived from the executable location and cannot be redirected through
a `CUP_HOME` option. A canonical root simplifies locking, recovery, uninstall
and entry-point management.

## Immutable release selection

The documented installation URL uses GitHub's `latest` alias only to obtain the
installer script. Each generated installer already contains:

```text
release version
release tag
release commit
immutable vX.Y.Z asset base URL
```

After it starts, the installer downloads every other asset from that immutable
versioned release. An installer downloaded from an older release therefore
installs that older release instead of silently mixing it with newer assets.

The installer verifies `release.txt`, the platform checksum file and the common
checksum file before committing CUP assets. `SHA256SUMS.common` also covers both
installer scripts, allowing Unix-like Windows shells to verify `install.ps1`
before delegating to it. See [SECURITY](../design/SECURITY.md)
for the trust-boundary model and [RELEASES](../development/RELEASES.md) for asset generation.

## Linux and macOS

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

The shell installer detects one of:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
```

It requires common user-space tools such as `sh`, `curl` or `wget`, `mkdir`,
`mv`, `rm`, `chmod` and `uname`. Release downloads and redirects must remain on
HTTPS; loopback HTTP exists only behind an explicit test flag. The installer
uses a restrictive `umask` and makes `~/.cup` owner-only. It does not invoke a
system package manager and does not require `sudo`.

Installed assets include:

```text
~/.cup/bin/cup
~/.cup/config/packages.cfg
~/.cup/config/install.cfg
~/.cup/config/SHA256SUMS.common
~/.cup/config/SHA256SUMS.<platform>
~/.cup/helpers/cup-update-helper
~/.cup/helpers/uninstall.sh
```

The executable, native update helper and uninstall script receive executable
permissions. The package catalog, installation policy, checksum files and
uninstall helper are protected read-only after installation.

## Windows PowerShell

```powershell
$installer = Join-Path $env:TEMP "install-cup.ps1"
iwr https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 `
    -OutFile $installer
powershell -NoProfile -ExecutionPolicy Bypass -File $installer
```

The native installer supports `windows-x64` and writes:

```text
%USERPROFILE%\.cup\bin\cup.exe
%USERPROFILE%\.cup\config\packages.cfg
%USERPROFILE%\.cup\config\install.cfg
%USERPROFILE%\.cup\config\SHA256SUMS.common
%USERPROFILE%\.cup\config\SHA256SUMS.windows-x64
%USERPROFILE%\.cup\helpers\cup-update-helper.exe
%USERPROFILE%\.cup\helpers\uninstall.ps1
```

The CUP root receives a protected ACL for the current user, Local System and
Administrators. The catalog, installation policy, checksum files and uninstall
script receive the Windows read-only attribute. Native PowerShell path handling
rejects a volume-root `USERPROFILE` and ensures that one canonical Windows
installation is shared by PowerShell, `cmd.exe` and Unix-like shells.

## Windows cmd.exe

From `cmd.exe`, invoke the same PowerShell installer:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "iwr https://github.com/coffee-clang/cup/releases/latest/download/install.ps1 -OutFile $env:TEMP\install-cup.ps1"
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File "%TEMP%\install-cup.ps1"
```

No independent `cmd.exe` installer is maintained. A second implementation would
have to duplicate verification, recovery and path rules without adding a new
runtime capability.

## Git Bash, MSYS2 and Cygwin

The POSIX installer can be started from a Unix-like Windows shell:

```sh
curl -fsSL https://github.com/coffee-clang/cup/releases/latest/download/install.sh | sh
```

When such an environment is detected, it downloads `SHA256SUMS.common` and
`install.ps1` into a private temporary directory, verifies the exact installer
record and only then delegates to PowerShell. This prevents both unverified
script delegation and parallel installations under an emulated `$HOME` and
`%USERPROFILE%`.

## PATH handling

The installers can add the canonical `bin` directory to the user environment:

```text
POSIX   ~/.cup/bin
Windows %USERPROFILE%\.cup\bin
```

For Bash and Zsh, the POSIX installer appends one idempotent export to the
selected shell profile. For Fish, it creates the dedicated file:

```text
~/.config/fish/conf.d/cup.fish
```

with:

```fish
fish_add_path "$HOME/.cup/bin"
```

The dedicated Fish file avoids rewriting the user's general `config.fish` and
is loaded automatically by Fish. An already configured entry is detected and
is not duplicated. Shell profiles and their directories are never modified
through symbolic links or non-regular files; replacement uses a private file in
the same directory followed by an atomic rename.

`cup uninstall` intentionally does not remove a PATH entry. Editing shell
profiles or user environment data safely requires knowing which installer or
user created each setting. A remaining entry is harmless and can be reused by a
future installation.

## Reinstallation and interrupted CUP assets replacement

A CUP asset replacement uses the persistent `.bootstrap` staging directory
with per-asset backup and commit markers. A later installer run can inspect that
state and complete or restore an interrupted replacement instead of assuming
that all canonical assets belong to the same release.

The installer commits only verified files and restores read-only/executable
attributes after replacement. It also removes a stale `uninstall.pending`
marker only after a valid CUP asset generation has been restored.

This installer recovery is separate from the runtime transaction journal used
by `cup install`, `cup remove` and `cup update cup`. Runtime recovery is
specified in [TRANSACTIONS](../design/TRANSACTIONS.md).

## Development checkout fallback

A development executable launched from the repository root may use:

```text
config/packages.cfg
config/install.cfg
scripts/install/uninstall-cup.sh
scripts/install/uninstall-cup-windows.ps1
```

only when installed CUP assets are unavailable. Installed assets remain the
preferred source. `doctor` reports when a development fallback is being used so
it is not mistaken for a complete installed CUP asset generation.

## Updating CUP

```sh
cup update cup
```

This is not a replacement for the initial installer. It updates an existing
official installation and uses the runtime journal plus a detached helper to
replace the running executable and the complete verified CUP asset generation:

```text
cup executable
uninstall helper
platform checksum file
packages.cfg
install.cfg
common checksum file
```

Development builds cannot update themselves as an official generation. See
[COMMANDS](COMMANDS.md#update) and
[TRANSACTIONS](../design/TRANSACTIONS.md#cup-update-transaction).

## Uninstall

```sh
cup uninstall
```

The command creates a pending marker and starts a detached helper. The helper
waits for the current process to exit, atomically detaches the canonical `.cup`
root to a unique sibling directory, and deletes the detached tree. This avoids
exposing a partially removed canonical root to another process.

The helper refuses non-canonical roots and does not edit `PATH`. The complete
protocol is documented in [TRANSACTIONS](../design/TRANSACTIONS.md#uninstall-protocol).

## Verification after installation

Useful checks are:

```sh
cup --version
cup doctor
cup search
```

`cup --version` verifies that the executable starts. `cup doctor` validates the
CUP assets, local state and runtime tree without modifying them. `cup search`
confirms that the installed catalog can be loaded for the detected host.

## Implementation and verification

Installer and CUP-assets ownership is listed in
[ARCHITECTURE](../design/ARCHITECTURE.md). Native installer and runtime
verification are described in [TESTING](../development/TESTING.md) and
[RELEASES](../development/RELEASES.md).

## Related documents

- [PLATFORMS](../design/PLATFORMS.md) — platform-specific paths and process behavior;
- [SECURITY](../design/SECURITY.md) — metadata, checksum and TLS validation;
- [RELEASES](../development/RELEASES.md) — how installer assets are generated;
- [STATE](../design/STATE.md) — files created after CUP asset installation;
- [TRANSACTIONS](../design/TRANSACTIONS.md) — CUP-update and uninstall recovery.
