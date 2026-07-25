# Transactions

This document defines the current recovery protocol for package mutations, cup
updates and uninstall. The canonical state and paths are specified in
[STATE](STATE.md).

## Shared runtime blocker

Only one mutation can own the installation at a time. The canonical journal is:

```text
~/.cup/transaction.txt
```

Staged objects are placed below:

```text
~/.cup/staging/
```

The journal is written before the first recoverable persistent mutation. It
contains only a safe staging basename; recovery reconstructs the path below the
canonical staging directory.

The command policy is intentionally small:

| Command | Valid or invalid journal |
|---|---|
| `help`, `--version` | allowed |
| `doctor` | allowed, read-only diagnosis |
| `repair` | recovery, or preservation and failure when ambiguous |
| every other command, including `uninstall` | blocked |

An invalid journal remains at the canonical path. `repair` does not rename it
away, reconcile state, modify packages or rebuild wrappers. The journal and all
staging evidence remain available for diagnosis.

There is no compatibility reader for pre-release journal formats.

## Package journal

The package journal is strict `format=1` data:

```ini
format=1
operation=install|remove|update
component=<component>
tool=<tool>
host_platform=<host>
target_platform=<target>
package_version=<concrete-version>
temporary_name=<identity-bound-basename>
```

`stable` is never persisted. The operation, package identity and temporary name
must agree exactly.

### Commit point

For install and remove, the atomic replacement of `state.txt` is authoritative:

```text
journal
→ filesystem mutation
→ atomic state replacement                 commit point
→ journal removal
→ cleanup and wrapper reconstruction
```

Recovery is allowed only when `state.txt` is present, readable, syntactically
valid and semantically valid. A missing or invalid state with a package journal
makes the commit point unknowable; `repair` stops without changing journal,
staging, canonical package paths, state or wrappers.

### Install recovery

```text
identity present in valid state
  canonical package must be valid; restore a valid staged copy when needed

identity absent from valid state
  remove the uncommitted canonical/staged package
```

A corrupted canonical package is preserved under a unique `.invalid` name
before a valid staged copy is restored.

### Remove recovery

```text
identity present in valid state
  restore the staged package to its canonical path

identity absent from valid state
  complete deletion of staging
```

A failure after state commit is not rolled back blindly. Cleanup failures are
reported as commit failures and remain recoverable.

## cup update protocol

The public command is:

```text
cup update cup
```

cup update uses the same `transaction.txt`, but a distinct strict record:

```ini
format=1
operation=cup-update
phase=scheduled|committing|failed
temporary_name=cup-update-<unique-id>
token=<handoff-token>
version=<concrete-version>
error=<publicly diagnosable internal code>
```

The update generation contains the executable, uninstall helper, platform
checksums, package catalog, installation policy and common checksums. All are
fetched from one immutable release and verified before the journal is written.

### Native helper and parent handshake

The installed native helper is:

```text
POSIX   ~/.cup/helpers/cup-update-helper
Windows %USERPROFILE%\.cup\helpers\cup-update-helper.exe
```

Before scheduling an update, cup refreshes that helper from the currently
installed executable. The parent creates an inherited pipe and starts the helper
with the journal token and the read endpoint. The helper proceeds only after it
observes EOF/broken-pipe, which proves that every process holding the parent
write endpoint has exited. A numeric PID is not used as the completion signal
and therefore cannot be confused with a reused process identifier.

The helper then:

```text
validates token, journal phase and staging path
waits for the exclusive lock with one bounded cross-platform policy
changes phase to committing
copies the six current assets to rollback backups
atomically installs the five verified supporting assets
writes the durable committed marker
atomically replaces cup or cup.exe last              commit point
clears transaction.txt
writes cup-update-result.txt
cleans staging when possible
```

The helper result is persisted at:

```text
~/.cup/cup-update-result.txt
```

It records `success` or `failed`, the error code and the target version so a
later command can report an update completed by the detached process. Once the
journal has been cleared, inability to remove stale staging is reported as a
warning and does not change an otherwise successful update into a failure.

### cup update recovery

`repair` distinguishes only the current `format=1` protocol:

- a committed marker finalizes the new generation only when the complete
  installed asset set validates;
- otherwise it can roll back supporting assets only when the canonical
  executable already equals its rollback backup, including the crash window
  after marker creation but before executable replacement;
- it never replaces `cup` or `cup.exe`; when rollback would require that change,
  it preserves the journal, staging tree and every canonical asset unchanged;
- the detached helper may restore the executable because it runs from a separate
  copy after the parent process has exited;
- any mixed, missing or invalid evidence remains blocked and is not guessed.

There is no legacy update operation and no compatibility path for older cup
asset generations.

## Repair pipeline

`repair` is a sequence of monotonic, idempotent phases rather than one global
transaction:

```text
validate state/journal relationship
recover one unambiguous transaction
restore cup assets and native update helper
scan current-host packages
preserve foreign-host package trees
quarantine only fully identified invalid current-host packages
reconcile and atomically save current-host state
rebuild wrappers
clean unambiguous staging leftovers
```

An ambiguous phase stops every later phase. In particular, an invalid journal
or an invalid/missing state beside a package journal prevents package scanning,
state reconciliation and wrapper changes.

State records and package trees for a host different from the current host are
reported and preserved. cup does not adopt, quarantine, delete or operate on
them in this generation.

## Interrupt lifecycle

Every public mutating command installs native interrupt handling before entering
its command implementation and restores the previous process disposition on
exit.

- POSIX observes `SIGINT` and `SIGTERM`;
- Windows observes `CTRL_C_EVENT`, `CTRL_BREAK_EVENT` and console close;
- handlers only set an async-safe flag;
- network, archive and recursive-filesystem work checks that flag at safe
  boundaries;
- code inside a commit boundary finishes or leaves its journal for deterministic
  recovery rather than rolling back an uncertain commit;
- an observed cancellable interrupt maps to public exit status `130`.

After the cup-update or uninstall handoff has been committed, the detached
helper is responsible for completion and the parent can no longer cancel it.

## Uninstall protocol and residues

`cup uninstall` uses `~/.cup/uninstall.pending`, obtains the exclusive lock and
starts its detached platform helper. The logical uninstall commit point is the
atomic rename:

```text
~/.cup → ~/.cup-uninstall.<unique-id>/root
```

Failure to delete the detached tree can leave a sibling residue. A later
installer removes such a residue only when all of these checks succeed:

- the sibling is a real directory, not a link/reparse point;
- it contains exactly the expected `root` child;
- that root contains the cup uninstall marker;
- the expected canonical cup executable exists inside it.

A name prefix alone is never sufficient. Unrecognized lookalike directories are
preserved and installation stops with a diagnostic.

## Commit-state errors

Native replacement primitives distinguish:

```text
SYSTEM_COMMIT_NOT_APPLIED
SYSTEM_COMMIT_APPLIED
SYSTEM_COMMIT_DURABLE
```

This maps to the important error classes:

```text
CUP_ERR_TRANSACTION  journal or evidence is invalid/ambiguous
CUP_ERR_COMMIT       a replacement may be visible or durability is uncertain
CUP_ERR_ROLLBACK     restoration did not complete
CUP_ERR_LOCK         another process owns the installation
CUP_ERR_INTERRUPT    cancellation observed at a safe boundary
```

## Relevant implementation

```text
package_transaction.c        package journal and state-based recovery
cup_update_journal.c         cup journal, result and recovery
cup_update_helper.c          native detached update commit
runtime_journal.c            journal handling and command blocker
interrupt.c                  process-wide native handler lifecycle
command_doctor.c             read-only diagnosis
command_repair.c             conservative ordered recovery
command_uninstall.c          uninstall handoff
```

## Related documents

- [STATE](STATE.md) — layout, state format and foreign-host policy;
- [SECURITY](SECURITY.md) — checksum and archive trust boundaries;
- [COMMANDS](../user/COMMANDS.md) — public CLI behavior;
- [PLATFORMS](PLATFORMS.md) — native platform differences.
