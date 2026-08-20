# Transactions and recovery

cup uses a transaction journal whenever an operation can leave persistent files
half changed. This page explains the shared rules and the three transaction
types: package changes, cup executable updates and uninstall.

The root layout and `state.txt` are described in [State](STATE.md).

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
This avoids three copies of the same in-process file lifecycle without merging
three different transaction meanings.

Uninstall has one unavoidable C/script boundary. The copied helper advances the
same uninstall schema during handoff and detached cleanup, but detach ownership
is platform-specific: POSIX keeps `cup.lock` in the parent C process through the
canonical root move, while Windows transfers the lease to the PowerShell helper.
Script-side journal updates use a temporary canonical file and publish it at
`transaction.txt`; the Windows helper also flushes that temporary file before
replacement. This script-owned continuation does not use the native per-file
identity API and is therefore kept separate from the `runtime_journal`
guarantees below.

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

Install/profile/toolchain preflight resolves these values from one shared state/catalog
snapshot before the first package mutation. The shared preflight context is then released;
each package installation acquires its own exclusive context and revalidates mutable state.
`stable` is not resolved again for the already-pinned artifact.

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
Download, cache verification, extraction and package validation are reconstructible work and
do not need a package journal. The journal begins only immediately before the first canonical
package mutation:

```text
prepare/validate staging
write journal
change canonical package filesystem
replace state.txt              commit point
remove journal
clean staging
rebuild or clean wrappers
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
blindly roll state back after the deciding write may already be visible. Retryable
directory-chain creation and tree removal can leave partial filesystem progress;
recovery therefore preserves that uncertainty as a commit problem, while an explicit
failed restoration remains a rollback error.

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
9. starts the native update helper;
10. waits for the journal and staging entry to disappear.

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
error=<0-or-public-error-code>
recovery=none|pending|rolled-back
```

Field combinations must match:

```text
scheduled or committing  error=0  recovery=none
failed                   error>0  recovery=pending|rolled-back
```

The staging generation contains:

```text
cup or cup.exe
uninstall helper
platform checksum file
packages.cfg
install.cfg
common checksum file
```

All installed assets come from one verified release before the journal is written.

### Detached helper and parent handshake

The helper is stored at:

```text
POSIX   <cup-root>/helpers/cup-update-helper
Windows <cup-root>\helpers\cup-update-helper.exe
```

Before every update, cup rebuilds the helper from the currently installed
executable, restores executable permissions and checks that the copy has the
same SHA-256 digest.

The parent process creates a pipe and passes only the read side to the helper.
The helper waits for EOF or a broken pipe, which proves that every process
holding the parent write side has exited. A PID is not used as the proof of
termination.

On Windows the process attribute list allows inheritance of only that handle.

After the parent has exited, the helper:

```text
waits for the exclusive cup lock
reloads and authenticates the token, scheduled journal and staging directory
validates every staged asset
copies every current destination to `.old` or records `.absent` rollback evidence
publishes phase=committing only after that rollback evidence is complete
installs the five supporting assets
writes and synchronizes the committed marker
replaces cup or cup.exe last             commit point
validates the installed generation
removes transaction.txt
cleans staging when possible
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

`cup uninstall` takes the exclusive lock, validates the root and helper, creates
a unique sibling destination and writes:

```text
format=1
operation=uninstall
phase=scheduled|detaching|failed
temporary_name=.cup-uninstall.<token>
token=<token>
stage=handoff|parent-wait|detach|cleanup
error=<0-or-public-error-code>
```

Allowed combinations are:

```text
scheduled  stage=handoff|parent-wait  error=0
detaching  stage=detach               error=0
failed     error>0
```

The copied helper validates the root marker, journal identity and its own script. The
canonical `cup.lock` is also the handoff authority: while an authorized pre-detach uninstall
can still move the canonical root, another CUP operation cannot acquire that lock.

The actual detach is platform-specific:

```text
POSIX    parent CUP retains cup.lock through helper acknowledgement, then performs
         <cup-root> -> <home>/.cup-uninstall.<token> and releases the lease
Windows  helper inherits the lease handle, waits for the parent, records detaching,
         performs the same root move, then releases the lease before tree cleanup
```

The transaction file moves with the root.

If cleanup later fails, the detached directory keeps three ownership proofs:

```text
root.txt
cup or cup.exe
transaction.txt
```

CUP does not automatically adopt or delete a detached residue on a later installation. The
evidence is retained so the failed cleanup can be identified without guessing at unrelated data.
There is no `uninstall.pending` or separate result file.

### Identifying uninstall residue

A detached directory is recognizable as residue from this uninstall protocol only when:

- the sibling name matches `.cup-uninstall.<token>`;
- it is a real directory rather than a link or reparse point;
- `root.txt` is valid;
- the main executable is a real file;
- the uninstall journal names the same token and directory;
- the journal is either a valid detaching state or a valid cleanup failure.

A matching prefix or familiar layout is not enough. Installers do not treat these siblings as
canonical roots and do not delete them automatically.

When `repair` owns the canonical `cup.lock`, no authorized pre-detach helper can still
move the root. If the journal names a sibling that does not exist, `repair` can therefore
clear a stale `scheduled`, `detaching` or `failed` pre-detach uninstall journal. If the named
sibling exists or ownership cannot be proved, the evidence is preserved.

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

After an update helper acknowledges handoff, that detached native helper owns
completion and the parent can no longer cancel it. Uninstall has a different
pre-detach split: on POSIX the parent retains the canonical lock, moves the root
after the copied helper acknowledges readiness and only then leaves cleanup to
that helper; on Windows the copied helper owns both detach and cleanup after the
handoff. Once the root is detached, cleanup is no longer cancellable by the
original command.

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
cup_update_journal.c       executable-update schema and recovery
cup_update_helper.c        detached executable-update commit
uninstall_journal.c        uninstall schema and recovery
command_doctor.c           read-only diagnosis
command_repair.c           ordered recovery
command_uninstall.c        uninstall handoff
interrupt.c                process interrupt state
```

## Related documents

- [State](STATE.md)
- [Security](SECURITY.md)
- [Commands](../user/COMMANDS.md)
- [Platforms](PLATFORMS.md)
