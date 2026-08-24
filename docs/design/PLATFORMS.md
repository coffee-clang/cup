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

POSIX uses `HOME`. Its value must already be a clean, non-root absolute path.
cup does not turn `.`/`..`, repeated or trailing separators, or backslashes into
a different managed root. Windows uses `USERPROFILE`. The program does not infer
the root from the executable path and does not support an environment override.

## Executable and helper names

```text
POSIX main executable       cup
Windows main executable     cup.exe
POSIX update helper         update-helper
Windows update helper       update-helper.exe
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

A temporary POSIX uninstall helper can remove its own exact pathname after
proving native identity and continue running through the already-open executable
image. cup uses that property only on POSIX.

### Windows implementation

The Windows backend uses wide-character Windows APIs. UTF-8 project paths are
converted through the private helpers in `windows_utf.h`.

Managed filesystem paths use the long-path form where required. Paths passed to
external processes use the normal Windows path representation instead of a device
prefix that the child may not understand.

The backend checks reparse points, uses handle identities for later operations
and configures inherited handles explicitly for detached helpers. Identity
snapshots use the 128-bit Windows file ID where the filesystem provides one.
The legacy ID is used only when the filesystem explicitly reports that no
128-bit ID exists. After a move, the backend refreshes identity through the
still-open source handle before proving the destination because some filesystems
can change a file ID during rename.

Windows x64 is built against the Windows 10 API baseline
(`_WIN32_WINNT=0x0A00`). That build setting does not by itself identify the
oldest Windows 10 release that is qualified to run cup.

Windows uninstall also has one platform-specific problem: a running `.exe` is a
mapped executable image, so cup does not require it to unlink its own pathname.
Instead, the parent binds a `DELETE_ON_CLOSE` handle to the exact helper file
before launch. The helper verifies that inherited handle against its own running
executable before accepting handoff.

That delete handle must remain alive until the helper process has terminated.
The helper therefore starts `%SystemRoot%\System32\sort.exe` as a small
*lifetime carrier*. A private pipe keeps that process alive while the helper is
running. The carrier receives only the cleanup handle, the pipe read end and
`NUL` output handles; it receives no cup root, uninstall token, journal or
handoff authority. When the helper exits, the pipe writer closes, `sort.exe`
receives EOF and exits, and the final cleanup handle is released. No timer or
`PATH` lookup is part of this protocol.

The complete uninstall sequence and its recovery boundaries are described in
[Transactions](TRANSACTIONS.md).

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

## Locks, handoff and atomic replacement

Normal root operations use one runtime lock at `<cup-root>/cup.lock`. A detached
self-update or uninstall must transfer authority to a child without creating an
unlocked interval, so the system layer also provides a temporary `SystemHandoff`.

On POSIX, parent and child retain references to the same `flock` open-file
description across `fork`/`exec`. On Windows, where `LockFileEx` ownership cannot
be transferred to another process, a named per-user kernel object outside the
managed root bridges the transition. Root admission checks that Windows handoff
before inspecting a root and again after acquiring `cup.lock`.

For self-update, the child returns from handoff authority to the canonical lock
before changing update state. For uninstall, the child keeps handoff authority
while it detaches and removes the root because `cup.lock` itself lives inside the
root being removed.

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

## Native detached helpers

Both self-update and uninstall run a copied native cup executable after the
initiating process exits. The parent-lifetime proof is an inherited operating-system object; helpers do
not pass or poll a PID. Detached helpers do not retain
the caller's standard streams: POSIX reconnects stdin/stdout/stderr to
`/dev/null`, while Windows inherits only the explicitly allowlisted handles
required by that operation.

### Uninstall

The parent creates a unique native helper copy outside the managed root, starts
it while still holding the exclusive canonical lock and establishes handoff
authority before that lock can be released. Temporary-helper cleanup is armed
before the root can be mutated: POSIX unlinks the verified running helper path;
Windows uses the deferred cleanup mechanism described above.

After parent exit, the child validates the root and uninstall journal, publishes
`detaching/detach`, then performs the root move and no-follow cleanup itself.
Windows retries only bounded transient sharing failures during the root move.
All Windows cleanup uses the native long-path filesystem backend; PowerShell is
not part of the uninstall protocol.

### `cup update cup`

The persistent `helpers/update-helper[.exe]` is refreshed from the current
installed executable before each update. It receives the same continuous
handoff. After parent exit, POSIX converts the inherited flock authority directly
into the helper's `SystemLock`; Windows acquires `cup.lock` while the external
authority is still active and then releases that temporary authority. The helper
then validates and commits the staged generation.

## Linux static runtime

Official Linux candidates statically link cup's third-party libraries and the
glibc runtime. The resulting executable has no ELF dynamic interpreter, but
glibc resolver and NSS behavior can still depend on compatible host facilities.
The release portability test exercises DNS, TLS, direct HTTPS and proxy CONNECT;
it does not claim a musl-based or libc-independent runtime.

## Public installer portability

The POSIX installer runs on machines that cup does not prepare. It uses `/bin/sh`
and a deliberately small command set. It does not depend on a compiler or on
repository-only helpers.

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

These values describe the build configuration rather than deriving runtime
support by themselves. In particular, `_WIN32_WINNT=0x0A00` selects the Windows
10 API baseline; it does not name a specific Windows 10 feature release. The
oldest supported runtime still requires matching native qualification evidence.

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
