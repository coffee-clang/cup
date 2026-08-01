# Transactions

This document defines the current recovery protocol for package mutations, CUP
updates and uninstall. The canonical state and paths are specified in
[STATE](STATE.md).

## Shared runtime blocker

Only one mutation can own the installation at a time. Every persistent mutation
uses the same canonical journal:

```text
<cup-root>/transaction.txt
```

Package and CUP-update staging objects are placed below:

```text
<cup-root>/staging/
```

The journal is written before the first recoverable persistent mutation. Each
operation has one strict `format=1` schema. Unknown, missing, duplicate or
incoherent fields make the journal invalid; invalid evidence remains at the
canonical path and is never guessed or renamed away.

The command policy is intentionally small:

| Command | Valid or invalid journal |
|---|---|
| `help`, `--help`, `--version` | allowed without consulting or modifying the runtime |
| `doctor` | allowed, read-only diagnosis |
| `repair` | deterministic recovery or acknowledgement; preservation on ambiguity |
| every other command, including `uninstall` | blocked |

A read-only command never consumes, clears or rewrites transaction evidence.
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
valid and semantically valid. A missing or invalid state beside a package
journal makes the commit point unknowable; `repair` preserves journal, staging,
canonical package paths, state and wrappers unchanged.

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

## CUP update protocol

The public command is:

```text
cup update cup
```

CUP update uses the shared journal with this strict schema:

```ini
format=1
operation=cup-update
phase=scheduled|committing|failed
temporary_name=cup-update-<unique-id>
token=<handoff-token>
version=<MAJOR.MINOR.PATCH>
error=<0-or-public-error-code>
recovery=none|pending|rolled-back
```

Coherence rules are part of the format:

```text
scheduled or committing  error=0, recovery=none
failed                   error>0, recovery=pending|rolled-back
```

The target version is a strict concrete semantic version. The token must embed
the exact staging basename. The update generation contains the executable,
uninstall helper, platform checksums, package catalog, installation policy and
common checksums fetched from one immutable release and verified before the
journal is written.

### Native helper and parent handshake

The installed native helper is:

```text
POSIX   <cup-root>/helpers/cup-update-helper
Windows <cup-root>\helpers\cup-update-helper.exe
```

Before scheduling an update, CUP refreshes that helper from the canonical
executable and verifies their identity. The parent creates an inherited pipe
and starts the helper with the journal token and read endpoint. The helper
proceeds only after EOF/broken-pipe proves that every process holding the parent
write endpoint has exited. A numeric PID is not used as the completion signal.

The helper then:

```text
validates token, journal phase and staging path
waits for the exclusive lock with the bounded cross-platform policy
changes phase to committing
copies the six current assets to rollback backups
atomically installs the five verified supporting assets
writes and synchronizes the committed marker
atomically replaces cup or cup.exe last              commit point
validates the installed generation
clears and synchronizes transaction.txt
cleans staging when possible
```

A successful detached update creates no persistent result file. The initiating
command reports only that the verified transition was scheduled; a later
`cup --version` reports the actual installed version.

### Failure and recovery

Before the committed marker, the detached helper may restore the complete old
generation, including the executable because it runs from a separate copy. A
successful rollback persists:

```text
phase=failed
error=<original-nonzero-error>
recovery=rolled-back
```

This terminal journal preserves the failed target version and original error.
`doctor` reports it without modification. A later `repair`, under the exclusive
lock, verifies that no referenced staging residue remains and acknowledges the
completed rollback by durably clearing the journal.

With `recovery=pending`, `repair` uses the committed marker and the complete
asset evidence to choose only one provable action:

- committed marker plus a valid installed generation: finalize the new generation;
- no committed marker and a restorable old generation: roll back;
- any mixed, missing or invalid evidence: preserve everything and fail.

The normal `repair` process never replaces its own running executable. If safe
recovery would require that replacement, it leaves the journal and staging
unchanged for the detached helper or official installer.

There is no separate `cup-update-result.txt`, no implicit acknowledgement by a
normal command and no legacy update operation.

## Uninstall protocol

`cup uninstall` obtains the exclusive lock, validates the selected owned root
and installed helper, creates a unique direct sibling destination and writes:

```ini
format=1
operation=uninstall
phase=scheduled|detaching|failed
temporary_name=.cup-uninstall.<token>
token=<token>
stage=handoff|parent-wait|detach|cleanup
error=<0-or-public-error-code>
```

Coherence rules are strict:

```text
scheduled  stage=handoff|parent-wait, error=0
detaching  stage=detach,              error=0
failed     error>0
```

The process ID passed to the platform helper is diagnostic input only. Parent
lifetime is proven through an inherited pipe on POSIX and inherited handles on
Windows; no PID is persisted.

The helper validates `root.txt`, the complete journal identity and its own
copied script, persists `parent-wait`, acknowledges the handoff, waits for the
parent lifetime signal, and then records `detaching`. The logical uninstall
commit point is the atomic move:

```text
<cup-root> → <home>/.cup-uninstall.<token>
```

After the move the same `transaction.txt` travels inside the detached root. The
helper records a cleanup failure and preserves `root.txt`, the canonical
executable and the journal until every unrelated payload entry has been removed.
A failed cleanup therefore leaves the three ownership proofs that a later
official installer requires before it may retry or delete the detached residue.
Complete deletion removes those proofs only from the final minimal residue. No
`uninstall.pending`, result or failure sidecar file exists.

### Residue validation

A later installer removes a detached sibling only when all evidence agrees:

- the sibling name is exactly `.cup-uninstall.<token>` with a safe token;
- it is a real directory, not a link or reparse point;
- it contains the valid CUP root marker;
- it contains the canonical CUP executable as a real file;
- its seven-line uninstall journal names that exact sibling and token;
- the journal is either `detaching/detach/error=0` or
  `failed/cleanup/error>0`.

A name prefix or familiar directory shape alone is never sufficient.
Unrecognized lookalikes are preserved and installation stops.

If a failed uninstall journal remains in the canonical root before detachment,
`doctor` reports it and `repair` may acknowledge it only when the named sibling
does not exist. A scheduled or detaching canonical journal is not guessed or
acknowledged.

## Repair pipeline

`repair` is a sequence of monotonic, idempotent phases rather than one global
transaction:

```text
validate state/journal relationship
recover or acknowledge one unambiguous transaction
restore CUP assets and native update helper
scan current-host packages
preserve foreign-host package trees
quarantine only fully identified invalid current-host packages
reconcile and atomically save current-host state
rebuild wrappers
clean unambiguous staging leftovers
```

An ambiguous phase stops every later phase. In particular, an invalid journal
or invalid/missing state beside a state-owning package or uninstall journal
prevents package scanning, state reconciliation and wrapper changes.

## Interrupt lifecycle

Every public mutating command installs native interrupt handling before entering
its command implementation and restores the previous process disposition on
exit.

- POSIX observes `SIGINT` and `SIGTERM`;
- Windows observes console control events;
- handlers only set an async-safe flag;
- network, archive and recursive-filesystem work checks that flag at safe boundaries;
- code inside a commit boundary finishes or leaves its journal for deterministic recovery;
- an observed cancellable interrupt maps to public exit status `130`.

After an update or uninstall handoff is acknowledged, the detached helper owns
completion and the parent can no longer cancel it.

## Commit-state and durability errors

Native replacement primitives distinguish:

```text
SYSTEM_COMMIT_NOT_APPLIED
SYSTEM_COMMIT_APPLIED
SYSTEM_COMMIT_DURABLE
```

This maps to the important error classes:

```text
CUP_ERR_TRANSACTION  journal or evidence is invalid/ambiguous
CUP_ERR_COMMIT       replacement may be visible or durability is uncertain
CUP_ERR_ROLLBACK     restoration did not complete
CUP_ERR_LOCK         another process owns the installation
CUP_ERR_INTERRUPT    cancellation observed at a safe boundary
```

Journal deletion is complete only after the parent directory synchronization
supported by the platform succeeds. POSIX uses directory `fsync`. Windows opens
the parent directory with backup-semantics and calls `FlushFileBuffers`; Windows
filesystems may reject directory flushing, in which case CUP documents and
accepts that platform capability limit rather than claiming a stronger guarantee.

## Relevant implementation

```text
package_transaction.c        package journal and state-based recovery
cup_update_journal.c         CUP update journal and recovery
cup_update_helper.c          native detached update commit
uninstall_journal.c          uninstall journal validation and acknowledgement
runtime_journal.c            shared journal classification and command blocker
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
