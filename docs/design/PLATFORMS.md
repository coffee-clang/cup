# Platforms

CUP exposes one product model across Linux, macOS and Windows. Platform-specific
code exists only where filesystem, process, executable or toolchain semantics
actually differ.

## Supported platform identifiers

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

The identifier is a closed `<os>-<arch>` value from the compiled registry. CUP
does not derive support by freely combining known operating systems and
architectures; for example, `windows-arm64` is not currently supported.

## Host and target

A package has two platform coordinates:

```text
host    where the package executable runs
target  the platform the tool handles/produces for
```

Examples:

```text
linux-x64 -> linux-x64      native Linux package
linux-x64 -> windows-x64    Windows cross-target tool running on Linux
```

Target defaults to host. State, package paths, preferences and defaults preserve
both values. One running CUP instance manages packages for its own host; foreign-
host records are reported/preserved rather than adopted automatically.

## User root and executable names

Default roots:

```text
POSIX    $HOME/.cup
Windows  %USERPROFILE%\.cup
```

A custom installer base still uses `.cup`, or `.coffee-cup` when the primary leaf
is foreign. Once installed, CUP derives the active root from its real executable
and validates `root.txt`.

Native executable/helper names are:

```text
POSIX    cup                 helpers/update-helper
Windows  cup.exe             helpers/update-helper.exe
```

Package commands are shell launchers on POSIX and `.cmd` launchers on Windows.

## Native system boundary

Portable modules call `system.h` instead of choosing OS APIs directly.

```text
system.c            portable queries / identity comparison
system_posix.c      POSIX filesystem, locks, processes, durability
system_windows.c    Windows filesystem, handles, reparse/process behavior
windows_utf.h       private UTF-8 <-> UTF-16 path boundary
```

### POSIX

The POSIX backend uses descriptor-relative operations when later mutation must
remain tied to an object already inspected. Its implementation includes
`openat`/`fstatat`/`unlinkat`, native rename operations, `flock`, process creation
and `fsync`.

Managed control-tree traversal does not follow symbolic links. POSIX package
payloads are the exception described by the package contract: confined relative
symbolic links may exist inside a validated package.

A temporary uninstall helper can unlink its own verified pathname and continue
through the already-running executable image, so helper cleanup does not need a
separate lifetime process on POSIX.

### Windows

The Windows backend uses wide-character native APIs and long-path forms where
required. CUP keeps normalized internal path spelling at its protocol boundary;
paths are converted to ordinary native forms only for Windows APIs/process launch
operands that require them.

Reparse points are treated explicitly and later destructive operations can be
bound to observed handle identity. File identity uses the full 128-bit Windows
ID when the filesystem provides it and falls back to the legacy identity only
when necessary.

Windows x64 is built against `_WIN32_WINNT=0x0A00` (Windows 10 API baseline).
That compile value is not, by itself, a promise about the oldest qualified
Windows feature release.

A mapped Windows `.exe` cannot be treated like a POSIX executable pathname during
uninstall. CUP therefore binds `DELETE_ON_CLOSE` to the exact temporary helper
before handoff. A built-in Windows PowerShell process, resolved from the system
directory, acts only as the lifetime carrier for that cleanup handle until the
helper process terminates. It receives no CUP root, token, journal or mutation
authority.

## Path and object rules

Managed relative paths use a platform-independent clean grammar: no empty,
absolute, `.` or `..` components, control characters or identifier separators.
Native roots additionally pass the operating system's absolute/root/UNC/device
checks.

Control paths do not use symlink/junction/reparse shortcuts. Directory
enumeration retains native identity when a later copy/move/remove relies on the
object that was observed. Recursive native cleanup does not follow links/reparse
points and refuses to cross into another device/volume.

Package payload path rules are documented in [Packages](PACKAGES.md).

## Permissions and executable state

### POSIX permissions

CUP creates private runtime/staging data below the user root. Installed package
directories are normalized to `0755`; regular payload files use `0755` when
executable and `0644` otherwise. Declared package entries must pass the executable
check before admission.

### Windows permissions

Windows security is not modeled through POSIX mode bits. Managed private
directories use a protected DACL owned by the current user; access is limited to
the current user, Local System and the local Administrators group and is inherited
by managed descendants. CUP verifies that privacy contract together with object
type, reparse state and native identity. Repository/MSYS test fixtures may still
use mode-like expectations where Git/shell transport needs them.

## Locks and handoff

Normal commands coordinate through `<cup-root>/cup.lock`. Detached self-update
or uninstall must transfer exclusive ownership to another process without an
unlocked interval.

```text
POSIX    parent/child share the inherited flock open-file description
Windows  parent/child share a named handoff authority keyed by root-parent
         filesystem identity and the canonical .cup/.coffee-cup slot
```

For self-update, the child converts that temporary authority back into the normal
root lock before commit. Uninstall keeps handoff authority while detaching the
root because the normal lock file moves with the root being removed.

The transaction-level behavior is documented in
[Transactions and recovery](TRANSACTIONS.md).

## Atomic publication and durability

Both native backends expose create-without-replace, identity-bound replacement
and identity-bound removal where CUP needs them. The runtime does not implement
no-replace as an unsafe “check then rename” sequence.

Mutation results distinguish:

```text
not applied
applied but not fully confirmed durable
durable
```

POSIX can use file/directory `fsync` at the required boundaries. Some Windows
filesystems reject directory `FlushFileBuffers`; CUP reports the strongest state
it can prove instead of pretending to have POSIX-equivalent durability.

## Native detached helpers

Both `cup update cup` and `cup uninstall` continue through a copied native CUP
executable after the initiating process exits. Parent lifetime is observed
through inherited OS objects, not PID polling. Detached helpers do not retain the
caller's standard streams.

- update uses the persistent `helpers/update-helper[.exe]` refreshed from the
  installed executable before each operation;
- uninstall uses one token-bound temporary helper outside the managed root;
- Windows temporary-helper deletion uses the handle-lifetime carrier described
  above; all root/journal cleanup remains native C.

## Build and linkage matrix

| Platform | Primary build toolchain | Release linkage |
|---|---|---|
| Linux x64 | GCC | fully static ELF |
| Linux arm64 | GCC | fully static ELF |
| macOS x64 | Apple Clang | third-party static, Apple system dynamic |
| macOS arm64 | Apple Clang | third-party static, Apple system dynamic |
| Windows x64 | MSYS2 UCRT64 GCC | third-party/compiler runtime static, approved system DLLs dynamic |

Linux x64 also receives a secondary Clang compile/unit pass. Windows sanitizers
use the separate MSYS2 CLANG64 profile with LLVM Compiler-RT.

Current release-build baselines include macOS deployment target 13.0 and the
Windows 10 API baseline above. Runtime support still requires native test
evidence; a compile flag alone is not compatibility proof.

## Linux static runtime

Official Linux releases contain no ELF interpreter or `DT_NEEDED` entries. CUP's
third-party graph and glibc runtime are linked into the executable. glibc
resolver/NSS behavior can still rely on compatible host facilities, so the
portability suite tests DNS, TLS, direct HTTPS and CONNECT proxy behavior rather
than claiming libc independence.

## Public installer portability

The POSIX installer targets `/bin/sh` and a deliberately small command set. It
must work before CUP or a compiler is available. The Windows installer uses
Windows PowerShell-compatible syntax.

Build/test/release scripts have a broader contract because CI prepares their
runtime environment.

## Native verification

Binary inspection checks the native output rather than inferring linkage from
Make flags:

```text
Linux    architecture + fully static ELF + no RPATH/RUNPATH
macOS    architecture/deployment target + approved Apple deps + no LC_RPATH
Windows  PE32+ x86-64 + approved system imports + resource/mitigation flags
```

The main test workflow executes all five supported host platforms natively.
Platform-specific suites cover the behaviors that cannot be meaningfully
simulated elsewhere, such as Windows reparse/process semantics, macOS Mach-O
metadata and Linux static-runtime behavior.

## Current platform limits

- Windows ARM64 is not supported.
- CUP does not install tool/runtime files outside its managed root.
- PATH integration is optional and user-level only.
- Windows directory durability can be weaker to prove on filesystems that reject
  directory flushes.
- The oldest practical OS release must be established by native testing; a
  compiler deployment baseline alone is not sufficient.

## Related documents

- [Architecture](ARCHITECTURE.md)
- [Packages](PACKAGES.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Build](../development/BUILD.md)
- [Testing](../development/TESTING.md)
