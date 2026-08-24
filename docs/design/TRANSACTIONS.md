# Transactions and recovery

cup uses a transaction journal whenever an operation can leave persistent files
half changed. This page explains the shared rules and the three transaction
types: package changes, cup executable updates and uninstall.

The root layout and `state.txt` are described in [State](STATE.md).

## Terms used on this page

| Term | Meaning |
|---|---|
| canonical root | the active managed root, normally `.cup` |
| detached root | the token-named sibling used after uninstall moves the root |
| native helper | a copied cup executable that continues update or uninstall after the parent exits |
| handoff authority | temporary exclusive ownership held while work moves from parent to helper |
| cleanup carrier | Windows-only process that holds the helper deletion handle until the helper has terminated |

These terms describe ownership and process lifetime. They do not create separate
user-visible commands or additional transaction files.

## Shared transaction file

Every operation uses:

```text
<cup-root>/transaction.txt
```

Package and executable-update staging directories are placed below:

```text
<cup-root>/staging/
```

Only one mutating command may own the root at a time. The runtime lock prevents
live processes from changing the installation together, while
`transaction.txt` keeps the information needed after a process exits or crashes.

The file is written before the first recoverable change. Each operation has its
own `format=1` schema. Unknown, missing, duplicated or inconsistent fields make
the journal invalid.

Invalid transaction data is left at the same path. cup does not rename it out of
the way or guess which fields were intended.

## Physical journal handling

While cup is running, `runtime_journal.c` owns the common file operations:

```text
bounded snapshot reading
key=value line iteration
create-only first publication
identity-checked replacement
identity-checked deletion
file and parent synchronization
```

The package, update and uninstall modules own their fields and recovery rules.
This avoids three copies of the same physical file lifecycle without merging
three different transaction meanings.

Update and uninstall continuation is native on every supported platform. A
copied cup executable runs the internal helper mode, so journal publication,
identity checks, long-path handling and no-follow cleanup continue to use the
same C filesystem contract as the parent process.

The first journal write is create-only. If `transaction.txt` already exists,
the new operation stops and preserves it.

When an operation advances the same journal, replacement is allowed only when
the path still names the file identity that was opened earlier. Deletion uses
the same check. A different file that appears at the same pathname is preserved.
If identity-bound deletion succeeds but synchronization of the parent directory
cannot be proved, the journal is already gone and the result is a commit error.

## Which commands are allowed

| Command | Behavior when a journal exists |
|---|---|
| `help`, `--help`, `--version` | run without opening the cup root |
| `doctor` | read-only inspection |
| `repair` | recovery or resolution when the result can be proved |
| every other command | blocked |

A read-only command never clears or rewrites the transaction file.

## Root snapshot used by one command

A public command selects the cup root once. The command context stores the path
and, when the root exists, its native identity.

After a mutating command acquires the lock, it checks that the same root still
exists. Layout functions then reuse the stored selection instead of performing a
second root search.

When the selected root did not exist, cup creates it exclusively. If another
process creates the path first, cup does not adopt that new directory during the
same command.

## Package plan before mutation

Package selection produces a `PackageArtifactSpec` containing only:

```text
concrete package identity
archive format
package URL
checksum URL
```

Install, profile and toolchain preflight resolve these values from one shared
state/catalog snapshot before the first package mutation. The shared preflight
context is then released. Each package installation acquires its own exclusive
context and revalidates mutable state. `stable` is not resolved again for an
already pinned artifact.

The cache returns a `VerifiedArtifact` with the archive still open. Preflight and
extraction consume the same file that was hashed.

## Package transaction

Package journals use:

```text
format=1
operation=install|remove|update
component=<component>
tool=<tool>
host_platform=<host>
target_platform=<target>
package_version=<concrete-version>
temporary_name=<identity-based-name>
```

`stable` never appears in the file. The operation, identity and staging name must
agree.

### Commit point

For package install and remove, the deciding step is replacement of `state.txt`.
Download, cache verification, extraction and package validation are
reconstructible work, so they do not need a package journal. The journal begins
immediately before the first canonical package mutation:

```text
prepare/validate staging
write journal
change canonical package filesystem
replace state.txt              commit point
perform operation-specific post-commit cleanup
clear the journal when its owned recovery data is no longer needed
rebuild or clean wrappers as required by the command
```

A valid `state.txt` therefore tells recovery whether the package change became
committed.

If state is missing or invalid beside a package journal, cup cannot safely decide
whether to finish or undo the package change. `repair` leaves the journal,
staging, package paths and wrappers unchanged.

### Install recovery

```text
package identity is present in valid state
  -> keep or restore a valid installed package

package identity is absent from valid state
  -> remove the uncommitted installed/staged package
```

When the state expects the package but the installed copy is damaged, a valid
staged copy may replace it. The damaged object is first preserved under a unique
`.invalid` name.

### Remove recovery

```text
package identity is present in valid state
  -> restore the staged package

package identity is absent from valid state
  -> finish deleting staging
```

A cleanup error after state commit is reported as a commit problem. cup does not
blindly roll state back after the deciding write may already be visible. Directory
creation and tree removal can stop after making only part of their filesystem
change, so recovery preserves that uncertainty as a commit problem. A restoration
that was attempted and failed is reported separately as a rollback error.

## Initial bootstrap

The public POSIX and PowerShell installers only transport and verify release
files. They download one release generation to a private directory and call:

```text
cup --internal-bootstrap <verified-source-directory>
```

The hidden command:

1. verifies that the running executable belongs to the downloaded generation;
2. validates the catalog, installation policy and checksums;
3. selects the cup root;
4. acquires the normal exclusive lock;
5. refuses an existing transaction file;
6. prepares the runtime directories;
7. stages the same installed cup assets used by `cup update cup`;
8. writes the update-style journal;
9. starts the native update helper and returns once the asynchronous handoff is
   scheduled.

The public POSIX and PowerShell installers own completion waiting: they wait for
the journal/staging transition to finish and verify the exact installed version
before reporting installation success.

For rollback, each destination receives either an `.old` copy or an `.absent`
marker. Fresh installation and `cup update cup` therefore use the same commit and
recovery logic.

There is no separate `.bootstrap` state directory or bootstrap journal.

## `cup update cup`

The public command is:

```text
cup update cup
```

The journal schema is:

```text
format=1
operation=cup-update
phase=scheduled|committing|failed
temporary_name=cup-update-<unique-id>
token=<handoff-token>
version=<MAJOR.MINOR.PATCH>
error=0|<CupError>
recovery=none|pending|rolled-back
```

`<CupError>` is a nonzero value from the current internal `CupError` domain
(`CUP_ERR_INVALID_INPUT` through `CUP_ERR_INTERRUPT`). Arbitrary positive
integers are invalid.

Field combinations must match:

```text
scheduled or committing  error=0           recovery=none
failed                   error=<CupError>  recovery=pending|rolled-back
```

The staging generation contains:

```text
cup or cup.exe
platform checksum file
packages.cfg
install.cfg
common checksum file
```

All installed assets come from one verified release before the journal is written.

### Native helper and operation handoff

The persistent update helper is derived from the installed executable and stored at:

```text
POSIX   <cup-root>/helpers/update-helper
Windows <cup-root>\helpers\update-helper.exe
```

Before every update, cup refreshes that helper from the currently installed
executable, restores executable permissions and verifies that the copy has the
same SHA-256 digest.

The parent starts the helper while it still owns the exclusive canonical lock.
The system backend establishes a parent-lifetime signal and child authority
before the start call can succeed. A successful start consumes the caller-visible
`SystemLock`, but the parent keeps a lifetime authority reference until it exits.
At every point in the handoff, either the canonical lock or the inherited handoff
authority remains held, so no third process can become the mutation owner.

The handoff is implemented differently only where the operating system requires it:

```text
POSIX    parent and child retain references to the same flock open-file description
Windows  parent and child retain a named per-user kernel authority outside <cup-root>
```

The child waits for the inherited parent-lifetime object to close rather than
polling a PID. It is detached from the initiating command's standard streams, so
command capture receives EOF when the parent exits rather than when the helper
finishes. On Windows, root path arguments retain CUP's normalized internal spelling
through command-line serialization; only the helper executable itself is converted
to an ordinary native process path. After parent exit, the update child returns to
the canonical lock. On POSIX, it converts the inherited flock authority directly
into its `SystemLock`. On Windows, it acquires `cup.lock` while the external
authority is still active and then releases that temporary authority.

Only after that transition does the helper:

```text
reload and authenticate the token, scheduled journal and staging directory
require each referenced staged asset to still be a regular file
copy every current destination to `.old` or record `.absent` rollback evidence
publish phase=committing only after that rollback evidence is complete
install the four supporting assets
write and synchronize the committed marker
replace cup or cup.exe last             commit point
validate the installed generation
remove transaction.txt
clean staging when possible
```

The main executable is replaced last so that the helper can restore the old
complete generation before the commit marker when needed.

A successful update does not create a result sidecar. The initiating command
reports that the transition was scheduled; a later `cup --version` shows the
installed version.

### Update failure and recovery

A successful rollback leaves:

```text
phase=failed
error=<original-error>
recovery=rolled-back
```

`doctor` reports this terminal journal. `repair` may remove it after checking
that the staging directory no longer contains referenced recovery files.

With `recovery=pending`, `repair` chooses only from evidence it can prove:

```text
committed marker + valid new generation
  -> finish the new generation

no committed marker + complete old generation
  -> restore the old generation

mixed, missing or invalid files
  -> preserve everything and stop
```

The normal `repair` process never replaces its own running executable. If a safe
recovery would require that step, the transaction remains for the detached
helper or official installer.

## Uninstall transaction

`cup uninstall` takes the exclusive canonical lock, validates the selected root,
creates a unique detached sibling name and writes:

```text
format=1
operation=uninstall
phase=scheduled|detaching|failed
temporary_name=.cup-uninstall-<token>
token=<token>
stage=handoff|detach
error=0|6
```

Allowed combinations are:

```text
scheduled  stage=handoff  error=0
detaching  stage=detach   error=0
failed     stage=detach   error=6
```

The parent then copies its running native executable to a reserved temporary
sibling outside the managed root and starts it in the internal uninstall-helper
mode. Starting the child uses the same continuous handoff rule as
`cup update cup`: the parent still owns `cup.lock` when child authority is
established, so there is no interval without a mutation owner.

Before the common uninstall transaction can change the root, the temporary
helper must have a safe cleanup plan:

```text
POSIX
  prove reserved helper pathname == running executable
  unlink that pathname
  continue from the already-open executable image

Windows
  parent binds DELETE_ON_CLOSE to the exact helper file
  parent starts the handle-only cleanup carrier and waits for its readiness event
  child proves the inherited cleanup handle == its running executable
  carrier waits on the helper process object itself
  carrier releases the cleanup handle only after that process object is signaled
```

The Windows carrier is deliberately not a second uninstall implementation. It
knows no root, token, journal or handoff authority and performs no cup filesystem
walk. Its only job is to wait on the inherited helper process handle and keep the
verified cleanup handle alive until that process has terminated. The parent
requires an explicit readiness signal before it can release the canonical lock.

After this platform preflight succeeds, the common transaction is:

1. accept handoff and wait for the parent to exit;
2. validate the exact root, journal, token and detached destination;
3. publish `detaching/detach`;
4. move the canonical root to the detached sibling;
5. remove managed payload while preserving `transaction.txt`;
6. remove `transaction.txt` by its retained identity;
7. remove the empty detached root.

The important recovery boundaries are:

- **Windows cleanup carrier cannot be armed:** the canonical root is untouched,
  `scheduled/handoff` remains, and the exact token-bound helper may remain for
  `repair`.
- **Carrier armed, but failure occurs before a durable detach:** the canonical
  root and journal remain; the temporary helper is still scheduled to disappear
  when the helper process exits.
- **Durable detach succeeds, but payload cleanup fails:** managed residue and the
  detached `detaching/detach` journal remain together; temporary-helper cleanup
  is independent and still completes after helper exit.

Temporary-helper deletion is tied to actual helper-process termination, not to a
successful exit status. Once the carrier has been armed, either a successful or
failing helper termination signals the process object. The carrier then exits and
releases the final cleanup handle.
- **Complete success:** the canonical root is absent, the journal is removed
  last, and the temporary helper disappears after its process lifetime ends.

Before `repair` clears a stale canonical `scheduled/handoff` transaction it
removes only the exact token-bound helper regular file by filesystem identity.
Failure to prove or remove that file keeps the journal as a blocker.

```text
<cup-root> -> <home>/.cup-uninstall-<token>
```

On POSIX the inherited authority is the original flock open-file description.
On Windows it is a named per-user kernel object outside the root; normal root
admission checks that authority before inspecting a candidate root and again
after acquiring `cup.lock`. The Windows move retries only bounded transient
sharing failures. All detach and cleanup filesystem work is native C on both
platforms.

The transaction file moves with the root. Once the move is durably proved, the
helper removes all managed contents except `transaction.txt`, then removes that
journal by its retained file identity, and finally removes the now-empty detached
root by its retained directory identity.

This ordering defines the recovery evidence:

- while managed payload remains, the strict token-bound `transaction.txt` also remains;
- once `transaction.txt` is removed, no managed payload remains; at most an empty
  reserved-name directory shell can survive a final directory-removal failure.

A cleanup failure therefore does **not** require `root.txt`, the executable and
the journal to coexist. Earlier cleanup steps may already have removed either of
the first two. The journal is the last persistent ownership evidence.

A detached residue is recognized conservatively: the sibling must use the
reserved token-bound name, be a real directory, and contain a coherent
`detaching/detach` uninstall journal naming that same token and sibling. A
matching prefix or familiar layout alone is not ownership proof. Installers do
not adopt or delete detached siblings automatically.

`repair` only resolves a journal still present in the canonical root. While it
owns the canonical exclusive lock and no named detached sibling exists, a stale
`scheduled/handoff` or `failed` pre-detach transaction can be cancelled or
acknowledged. Any token-bound temporary native helper is removed first. If the
detached sibling exists, helper ownership cannot be proved, or cleanup fails,
evidence is preserved.

## Repair order

`repair` is a sequence of smaller steps:

```text
validate state and journal relationship
recover or resolve one transaction
restore checkable cup assets
refresh the update helper
scan current-host packages
preserve foreign-host packages
quarantine identified invalid packages
rebuild and save current-host state
rebuild wrappers
remove safe staging leftovers
```

Each phase is designed to be repeatable. If one phase becomes ambiguous, later
phases stop. For example, an invalid package journal prevents state rebuilding
and wrapper changes.

## Interrupt handling

Mutating commands install native interrupt handling before entering their main
operation and restore the previous process behavior on exit.

- POSIX handles `SIGINT` and `SIGTERM`;
- Windows handles console control events;
- handlers only set a flag;
- download, archive and filesystem loops check the flag at safe points;
- code inside a commit step finishes or leaves the journal for recovery;
- a handled cancellation returns exit status `130`.

After a helper has been started successfully, its handoff authority is continuous
with the parent authority and the original command can no longer cancel work in
that child. Both update and uninstall helpers wait for parent exit before their
first authoritative mutation. Uninstall detach and cleanup are then owned entirely
by the native child.

## Commit and durability results

Native replacement functions report:

```text
SYSTEM_COMMIT_NOT_APPLIED
SYSTEM_COMMIT_APPLIED
SYSTEM_COMMIT_DURABLE
```

These states distinguish an error before replacement from an error after the
new destination may already be visible.

Related cup errors include:

```text
CUP_ERR_TRANSACTION  invalid or ambiguous saved transaction data
CUP_ERR_COMMIT       replacement may be visible or not fully synchronized
CUP_ERR_ROLLBACK     restoration did not complete
CUP_ERR_LOCK         another process owns the mutation lock
CUP_ERR_INTERRUPT    cancellation was observed at a safe point
```

POSIX synchronizes the changed file or directory and then its parent directory.
Windows uses `FlushFileBuffers` where the filesystem supports it. Some Windows
filesystems reject directory flushing; cup reports the strongest result the
platform can provide rather than treating it as POSIX durability.

## Main implementation files

```text
runtime_journal.c          shared transaction-file operations
package_transaction.c      package schema and state-based recovery
update_journal.c           executable-update schema and recovery
update_helper.c            detached executable-update commit
uninstall_journal.c        uninstall schema and recovery
uninstall_helper.c         native detach and cleanup
command_doctor.c           read-only diagnosis
command_repair.c           ordered recovery
command_uninstall.c        uninstall planning and handoff
interrupt.c                process interrupt state
```

## Related documents

- [State](STATE.md)
- [Security](SECURITY.md)
- [Commands](../user/COMMANDS.md)
- [Platforms](PLATFORMS.md)
