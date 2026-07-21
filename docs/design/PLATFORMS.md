# Platforms

This document defines platform identifiers and implementation differences. The
portable behavior remains the one described in the other documents.

## Identifiers

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

Identifiers are exact lowercase values in the form `<os>-<arch>`. Unsupported
operating systems and architectures are rejected before catalog or path use.

## Host and target

```text
host    platform where the package executable runs
target  platform the package targets
```

The host is detected from the running executable. The target defaults to the
host and can be overridden by commands that accept `--target`.

A cross-target package is still built for the current host. For example:

```text
host=linux-x64 target=windows-x64
```

means a Linux executable that produces or manages Windows-targeted output. It
does not mean a Windows executable installed on Linux.

Availability is determined by the exact catalog tuple. Not every tool supports
every host/target combination.

## Canonical roots

```text
Linux/macOS  $HOME/.cup
Windows      %USERPROFILE%\.cup
```

Unix-like shells on Windows delegate CUP asset installation to PowerShell and
use the native Windows root.

## Executable names

```text
POSIX    cup
Windows  cup.exe
```

Component entry files retain their package-specific names. Managed default
wrappers are:

```text
native       <entry>
cross-target <target>-<entry>
```

On Windows the managed wrapper uses `.cmd` representation where required by the
entry-point implementation.

## POSIX implementation

`system_posix.c` uses native POSIX primitives for:

- `HOME` resolution;
- `lstat`-style path inspection without following links;
- permissions and executable bits;
- atomic rename/replacement;
- file and parent-directory synchronization;
- advisory shared/exclusive locking;
- directory traversal;
- forked/detached helper execution;
- process existence checks.

Linux builds define `_POSIX_C_SOURCE=200809L`. macOS builds define
`_DARWIN_C_SOURCE` where required by the platform API.

## Windows implementation

`system_windows.c` uses wide-character Windows APIs for canonical filesystem and
process operations. It handles:

- `USERPROFILE` resolution;
- regular file, directory, link/reparse and other path kinds;
- Windows read-only and executable conventions;
- replace/move semantics and handle flushing;
- native file locks;
- temporary files and directories;
- detached PowerShell helpers;
- `.cmd` quoting and path conversion;
- protected owner-private ACLs for the CUP root;
- `\\?\`/UNC long-path normalization and reparse-point checks.

Using wide APIs avoids making the internal path model depend on the active ANSI
code page. The generated PE manifest declares `longPathAware`; native tests use
paths beyond `MAX_PATH` rather than treating larger C buffers as sufficient.

## Permissions

### POSIX

The CUP root is created with mode `0700` under a restrictive process `umask`.
Installed executables and wrappers receive executable bits. Protected metadata
receives write bits removed according to the project permission contract.

### Windows

Windows does not use POSIX executable bits. The CUP root uses a protected DACL
restricted to the current user, Local System and Administrators. Executable
validity is based on file type and expected extension/entry behavior. Protected
metadata receives the read-only attribute.

Read-only protection is an accidental-modification guard, not a cryptographic
integrity mechanism. See [SECURITY](SECURITY.md#read-only-protection).

## Links and archive paths

Package archive validation applies both POSIX and Windows path rules on every
host. A backslash or drive-qualified path cannot become safe merely because the
archive is being extracted on Linux.

Relative internal symlinks are accepted only when normalization stays within the
single package root. Hard links must reference an already extracted regular
file. Platform filesystem inspection does not follow a canonical package-level
link.

## Locks and replacement

Both implementations expose the same `SystemLockMode` and `SystemCommitState`
contracts. Native APIs differ, but higher layers receive the same semantic
answers:

```text
shared or exclusive lock
replacement not applied, applied or durable
```

Durability capabilities differ by filesystem and operating system. When a
platform cannot prove the required synchronization after replacement, the
operation remains recoverable rather than being reported as a normal success.

## Detached helpers

### Uninstall

POSIX copies and starts a shell helper. Windows copies and starts a PowerShell
helper. Both receive the canonical root and parent process identifier, wait for
the parent, atomically detach the root and delete the detached tree.

### CUP update

The platform layer generates or starts a helper capable of waiting for the
running binary, reacquiring the lock and replacing the complete canonical CUP asset generation.
The portable C layer has already selected and verified those assets.

See [TRANSACTIONS](TRANSACTIONS.md).

## Build matrix

```text
Linux x64     GCC
Linux ARM64   GCC
macOS x64     Clang
macOS ARM64   Clang
Windows x64   MSYS2 UCRT64 GCC
```

macOS x64 and ARM64 currently build with deployment target `13.0`. Windows
x64 builds natively in MSYS2 UCRT64 with `_WIN32_WINNT=0x0A00`. These values
are provisional CI compatibility baselines; they become public minimum-support
claims only after native validation on the corresponding operating-system
versions.

Official candidates are static with respect to project third-party dependencies
according to the platform build model. Linux release executables are fully
static. macOS retains only Apple system/framework dependencies, while Windows
retains only allowlisted operating-system DLL imports. `make check-binary`
verifies object format, architecture and the corresponding linked-binary policy
for each supported platform.

## Test matrix

Source validation runs natively on all five supported platform identifiers.
Release candidate binaries are also downloaded and tested on native runners.

Linux x64 additionally owns GCC coverage, ASan/UBSan and a local network
portability smoke test. The latter runs the static executable through DNS,
embedded-CA HTTPS validation, direct and proxied package downloads, checksum
verification and extraction. These reports do not include Windows-only code,
so the native Windows suite remains a separate requirement.

See [TESTING](../development/TESTING.md) and [RELEASES](../development/RELEASES.md).

## Current limitations

The recognized set does not currently include Windows ARM64 or additional
operating systems. A new identifier requires coordinated support in:

```text
platform detection and validation
system implementation or build selection
catalog tuples
static dependency CUP assets
release asset naming
installer detection
source and native test runners
```

Adding a catalog entry alone is not sufficient.

## Implementation and verification

Platform-module ownership is listed in [ARCHITECTURE](ARCHITECTURE.md). Native
verification and candidate execution are described in
[TESTING](../development/TESTING.md) and
[RELEASES](../development/RELEASES.md).

## Related documents

- [ARCHITECTURE](ARCHITECTURE.md) — host/target domain model;
- [INSTALLATION](../user/INSTALLATION.md) — platform installers;
- [STATE](STATE.md) — platform-aware scopes and wrappers;
- [BUILD](../development/BUILD.md) — compiler and linkage configuration.
