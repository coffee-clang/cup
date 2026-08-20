# Platforms

cup keeps one public model across Linux, macOS and Windows, but filesystem,
process and executable details are implemented natively on each platform.

## Platform identifiers

Supported identifiers are:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

The format is `<os>-<arch>`. cup does not build a platform by combining any
known OS with any known architecture; the complete identifier must appear in
the built-in registry.

`windows-arm64` is not currently supported.

## Host and target

A package has:

```text
host    platform where the package runs
target  platform handled by the tool
```

The target defaults to the host.

Examples:

```text
host=linux-x64 target=linux-x64     native Linux package
host=linux-x64 target=windows-x64   Windows cross-tool package running on Linux
```

State, package paths, preferences and defaults include both values.

One running cup executable manages only packages for its own host. Package trees
or state records for a different host are reported and preserved rather than
adopted automatically.

## User roots

```text
POSIX   $HOME/.cup
Windows %USERPROFILE%\.cup
```

The fallback is `.coffee-cup` on the same home directory.

POSIX uses `HOME`. Its value must already be a clean, non-root absolute path: cup does not
canonicalize `.`/`..`, repeated or trailing separators, or backslashes into a different managed
root. Windows uses `USERPROFILE`. The program does not infer the root from the executable path and does
not support an environment override.

## Executable and helper names

```text
POSIX main executable       cup
Windows main executable     cup.exe
POSIX update helper         cup-update-helper
Windows update helper       cup-update-helper.exe
POSIX uninstall helper      uninstall.sh
Windows uninstall helper    uninstall.ps1
```

Public package commands use shell wrappers on POSIX and `.cmd` wrappers on
Windows.

## Portable and native code

Portable code uses the API in `system.h`.

```text
system.c            shared traversal and platform-neutral checks
system_posix.c      openat/fstatat/unlinkat, POSIX locks and processes
system_windows.c    Windows paths, handles, reparse checks and processes
```

The command and package modules do not choose between POSIX and Windows calls
directly.

### POSIX implementation

The POSIX backend uses descriptor-relative operations where an existing tree
must stay tied to the object that was checked. Important calls include:

```text
openat
fstatat
unlinkat
renameat or renameat2 when available
flock advisory locking
fork/exec or posix_spawn-style process setup
fsync
```

Symbolic links are not followed for managed path traversal.

### Windows implementation

The Windows backend uses wide-character Windows APIs. UTF-8 project paths are
converted through the private helpers in `windows_utf.h`.

Managed filesystem paths use the long-path form where required. Paths passed to
external processes use the normal Windows path representation instead of a device
prefix that the child may not understand.

The backend checks reparse points, uses handle identities for later operations,
and configures inherited handles explicitly for detached helpers. Identity snapshots use the
128-bit Windows file ID where the filesystem provides one, with the legacy ID only when the
filesystem explicitly reports that no 128-bit ID exists. A move refreshes identity through its
still-open source handle before proving the destination, because some filesystems can change a
file ID during rename.

## Internal path representation

Public and persistent path text uses `/`-independent project rules, but native
filesystem calls receive the representation expected by the current platform.

Managed relative paths must be clean:

- no empty segment;
- no `.` or `..` segment;
- no absolute package entry;
- no control characters;
- no separator accepted inside an identifier;
- no path longer than the project limit.

Windows drive roots, UNC paths and device-prefixed paths are validated before
use. POSIX paths must be absolute where a managed root or external override
requires it.

## Permissions

### POSIX permissions

The user root is private. Runtime and staging directories use user-only modes
where they hold temporary or transaction data.

Installed package directories are normalized to mode `0755`. Regular package
files are normalized to `0755` when the admitted archive marks them executable
and to `0644` otherwise. Declared executable entries must satisfy the POSIX
executable check before the package is accepted.

cup and helper executables use executable permissions. State, configuration and
journal files are not made executable.

### Windows permissions

Windows does not use POSIX mode bits as the security model. cup checks file type,
reparse state and handle access instead. Test fixtures normalize mode-like
expectations only where Git/MSYS needs them for repository scripts.

## Links, reparse points and archive paths

Managed trees do not accept symbolic links, junctions or other reparse points as
normal package content.

Archive entries must be regular files or directories. Hard links, symbolic
links, device files, FIFOs and sockets are rejected.

When an operation enumerates a directory, the observed child identity is passed
to later copy or removal work. The later operation checks that it opened the
same object instead of trusting that the pathname still refers to it.

## Filesystem boundaries

Recursive removal and repository helper operations record the starting device or
volume. They refuse to cross into another mounted filesystem or reparse target.

This rule is important for cleanup commands: removing an owned build or staging
directory must not continue into a separately mounted tree that happens to be
inside it.

## Locks and atomic replacement

cup uses one runtime lock at `<cup-root>/cup.lock`.

The native backends also provide atomic file and directory publication used by
state, preferences, wrappers, journals and release/build staging.

A replacement reports whether it was:

```text
not applied
applied but not fully confirmed as durable
durable
```

No-replace operations must use a real native primitive. cup does not replace
that guarantee with “check whether the path exists, then move”, because another
process could create the destination between those two steps.

## Detached helpers

### Uninstall

The uninstall helper validates the handoff and waits for the parent through an
inherited lifetime descriptor or handle. On POSIX, the parent performs the
no-replace move to the detached sibling after the helper acknowledges startup;
after EOF the helper verifies that exact detach and removes it. On Windows, the
helper waits for the parent process handle and then performs the detach itself.

Parent termination is proved only by the inherited operating-system object. The
helper protocol does not pass or poll a process ID.

### `cup update cup`

The update helper follows the same parent-lifetime approach. It waits for the
main executable to exit, reacquires the cup lock and replaces the installed
assets with the staged generation.

On POSIX only the pipe read descriptor is inherited. On Windows an explicit
handle list contains only the required read handle.

## Linux static runtime

Official Linux candidates statically link cup's third-party libraries and the
glibc runtime. The resulting executable has no ELF dynamic interpreter, but
glibc resolver and NSS behavior can still depend on compatible host facilities.
The release portability test exercises DNS, TLS, direct HTTPS and proxy CONNECT;
it does not claim a musl-based or libc-independent runtime.

## Public installer portability

The POSIX installer and uninstaller run on machines that cup does not prepare.
They use `/bin/sh` and a deliberately small command set. They do not depend on a
compiler or on repository-only helpers.

The PowerShell installer uses Windows PowerShell-compatible syntax and native
filesystem paths.

Build, dependency, test and release scripts have a broader contract because the
workflow prepares their environment.

## Build matrix

| Platform | Main toolchain |
|---|---|
| Linux x64 | GCC |
| Linux ARM64 | GCC |
| macOS x64 | Apple Clang |
| macOS ARM64 | Apple Clang |
| Windows x64 | MSYS2 UCRT64 GCC |

Linux also receives a secondary Clang compile/unit pass. Windows sanitizer work
uses CLANG64 so ASan/UBSan use LLVM Compiler-RT.

Current CI build values are:

```text
macOS deployment target  13.0
Windows _WIN32_WINNT      0x0A00
```

These values describe the current build configuration. They should not be
advertised as final minimum supported OS versions until they have been tested on
those actual operating-system versions.

## Linked-binary policy

Release candidates may depend only on the platform runtime allowed by the
project:

```text
Linux    fully static executable
macOS    Apple system libraries and frameworks only
Windows  allowlisted operating-system DLLs only
```

Third-party project dependencies are linked statically.

`make check-binary` checks object format, architecture, runtime dependencies,
minimum OS metadata, debug/sanitizer instrumentation and path leaks.

## Test matrix

The five identifiers above are the product support matrix. Native test evidence
belongs to a specific commit and workflow run; listing a platform here does not
by itself claim that a particular repository snapshot was executed there.

The workflow runs native jobs for all five supported platforms. Public CLI
integration scenarios have matching POSIX and PowerShell suites where the
behavior is shared.

Platform-only tests cover:

```text
POSIX bootstrap and shell portability
Windows reparse points and native filesystem behavior
Linux fully static runtime
macOS load commands and coverage tooling
Windows PE imports and console/process handling
```

Repository tests can check workflow structure, but the real Windows and macOS
runners remain the final test for native APIs and shell behavior.

## Current limitations

- Windows ARM64 is not supported.
- cup does not install a system compiler or runtime outside its own root.
- The user must configure PATH manually.
- Windows directory flushing may provide weaker confirmation than POSIX
  directory `fsync` on filesystems that reject `FlushFileBuffers` for a
  directory handle.
- Final minimum OS support still needs evidence from the matching native OS
  versions.

## Related documents

- [Architecture](ARCHITECTURE.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Build](../development/BUILD.md)
- [Testing](../development/TESTING.md)
