# Transactions and recovery

CUP records a transaction whenever a persistent mutation can outlive one
process or leave filesystem/state changes only partly completed. The journal is
not an event log; it contains the minimum operation identity and phase needed to
prove what recovery may safely do next.

The root and `state.txt` model are documented in [State](STATE.md).

## Shared model

Every runtime transaction uses:

```text
<cup-root>/transaction.txt
```

Package/self-update staging lives below:

```text
<cup-root>/staging/
```

Only one mutating command can own a CUP root. The runtime lock coordinates live
processes; `transaction.txt` survives a crash/exit and coordinates the next one.
A new mutation refuses to start when a journal already exists.

Each transaction type owns its versioned schema. Unknown, missing, duplicate or
inconsistent fields invalidate the journal. Invalid transaction data is
preserved rather than renamed or cleared just to unblock normal commands.

## Physical journal operations

`runtime_journal.c` owns mechanics shared by every transaction type:

```text
bounded regular-file snapshot
key=value iteration
create-only first publication
identity-bound replacement
identity-bound deletion
file/parent persistence handling
```

`package_transaction.c`, `update_journal.c` and `uninstall_journal.c` define the
meaning of their fields and the corresponding recovery decisions.

First publication is create-only. Later replacement/deletion is permitted only
while the pathname still names the journal identity already observed by the
command. A different file that appears under `transaction.txt` is never adopted
as the transaction being advanced.

## Commands while recovery is pending

| Command family | With `transaction.txt` present |
|---|---|
| `help`, help options, `--version` | do not open the root |
| `doctor` | inspect and report |
| `repair` | attempt only provably safe recovery |
| other normal commands | blocked |

Read-only diagnosis does not acknowledge or modify the saved transaction.

## Root and lock snapshot

A command selects one root and retains its path/native identity for that command
lifetime. A mutating command then acquires the exclusive lock and validates the
same root before changing persistent data.

A genuinely missing bootstrap root may be created in order to create its first
lock. An existing root is not initialized or permission-rewritten before the
canonical lock is held.

For group installs, shared preflight resolves package artifacts first. Each
package later reacquires exclusive ownership and revalidates mutable local state.
The already resolved package artifact remains pinned; `stable` is not
reinterpreted halfway through the command.

## Package transactions

Package journals record:

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

The package identity and staging name must agree. `stable` never appears because
selection has already resolved a concrete release.

Download, cache verification, extraction and full package validation happen
before the journal because they only build disposable private staging. The
journal starts immediately before the first canonical package-tree mutation.

### Package commit point

For install, update and remove, `state.txt` is the deciding commit:

```text
prepare/validate staging
write transaction.txt
change canonical package filesystem
replace state.txt                 <- commit point
perform post-commit cleanup
clear transaction.txt when its recovery data is no longer needed
reconcile launchers
```

Recovery uses the valid committed state to decide which filesystem result should
exist. If state is missing or invalid beside a package journal, CUP cannot prove
which side won and preserves the transaction/package/staging evidence.

### Install/update recovery

```text
identity present in valid state
  -> keep/restore one valid canonical package

identity absent from valid state
  -> remove the uncommitted canonical/staged package
```

When state expects the package but the installed object is damaged, recovery may
replace it with a complete valid staged copy after preserving the damaged object.

### Remove recovery

```text
identity present in valid state
  -> restore the staged package

identity absent from valid state
  -> finish deleting removal staging
```

A failure after the state commit is not handled by blindly restoring old state.
Native publication reports whether a change was not applied, may already be
visible, or is durably confirmed; ambiguous post-commit results remain recovery
work.

## Initial installation

The public shell/PowerShell installers are transport frontends. They download
and verify one release generation in private storage, then invoke the hidden C
bootstrap mode:

```text
cup --internal-bootstrap <verified-source-directory> <selected-base>
```

Bootstrap validates the transported generation, selects/locks the managed root,
prepares runtime directories and stages the same five installed assets used by
self-update. It then writes the update-style transaction and hands continuation
to the native update helper.

The public installer waits for that asynchronous transition and verifies the
installed version before reporting success. Fresh installation and self-update
therefore share one asset commit/recovery model instead of maintaining a separate
bootstrap transaction format.

## `cup update cup`

The update journal is:

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

Valid phase/error relationships are:

```text
scheduled|committing  error=0           recovery=none
failed                error=<CupError>  recovery=pending|rolled-back
```

The staged installed generation contains:

```text
cup or cup.exe
packages.cfg
install.cfg
SHA256SUMS.common
SHA256SUMS.<platform>
```

All five must come from one verified release before the journal is created.

Immediately before the main executable is replaced, the helper writes a durable
`committed` marker in update staging. It binds the intended version to the exact
five staged/installed asset digests:

```text
format=1
version=<MAJOR.MINOR.PATCH>
binary_sha256=<sha256>
platform_checksums_sha256=<sha256>
packages_sha256=<sha256>
install_policy_sha256=<sha256>
common_checksums_sha256=<sha256>
```

Recovery treats this marker as evidence only after parsing the complete fixed
schema and rechecking the referenced generation; its presence alone is not a
commit decision.

### Update helper and handoff

The persistent helper is refreshed from the currently installed executable
before each update:

```text
POSIX    <cup-root>/helpers/update-helper
Windows  <cup-root>\helpers\update-helper.exe
```

The parent starts it while still holding exclusive root ownership. The platform
backend establishes both parent-lifetime observation and continuous child
handoff authority before launch can succeed. The child waits for the actual
parent-lifetime object to close rather than polling a PID.

After parent exit, the update child returns to the canonical `cup.lock` while the
handoff authority is still active. Only then does it advance the update:

```text
revalidate journal/token/staging
record complete rollback evidence for current destinations
publish phase=committing
install four support assets
write/synchronize committed marker
replace cup/cup.exe last             <- update commit point
validate installed generation
remove transaction.txt
clean staging
```

Replacing the main executable last keeps rollback possible before the committed
marker and avoids treating a mixed generation as complete.

A successful initiating command means the update was scheduled; the detached
helper completes it after the parent exits. The installed version is observable
through the next `cup --version`.

### Update recovery

A completed rollback records:

```text
phase=failed
error=<original-error>
recovery=rolled-back
```

`doctor` reports it; `repair` can acknowledge/remove the terminal journal only
after checking that referenced recovery data no longer needs it.

For `recovery=pending`, the safe choices are:

```text
committed marker + valid new generation
  -> finish/accept the new generation

no committed marker + complete old generation
  -> restore the old generation

mixed or incomplete evidence
  -> preserve everything
```

The normal repair process does not replace its own running executable. Recovery
that needs that step remains assigned to the detached helper or official
installer.

## Uninstall

The uninstall journal is:

```text
format=2
operation=uninstall
phase=scheduled|detaching|failed
temporary_name=.cup-uninstall-<token>
token=<token>
error=0|6
```

Allowed combinations are:

```text
scheduled   error=0
detaching  error=0
failed     error=6
```

The parent validates the root under its exclusive lock, writes this transaction,
creates one reserved temporary native helper outside the managed root and starts
that helper with continuous handoff authority.

The helper cannot safely delete its own executable the same way on every OS:

- POSIX proves and unlinks the running helper pathname before continuing from the
  mapped/open executable image;
- Windows binds deferred deletion to the exact helper file and keeps that handle
  alive until the helper process has terminated.

This difference belongs to the system backend. The CUP uninstall transaction
itself remains the same.

After parent exit, the helper:

```text
validate root + journal + token + detached destination
publish phase=detaching
move canonical root -> token-named sibling    <- detach commit
remove managed payload, keeping transaction.txt last
remove transaction.txt by retained identity
remove the empty detached root
```

The transaction moves with the root. While managed payload remains after a
failure, the strict token-bound journal also remains as recovery/ownership
evidence. `root.txt` and the main executable are not required to survive until
that point because cleanup may already have removed them.

A later installer does not adopt or automatically delete a detached sibling.
`repair` can cancel/acknowledge only stale **pre-detach** uninstall state still in
the canonical root when it can also prove that no detached root owns the
operation.

See [Platforms](PLATFORMS.md) for the POSIX/Windows handoff mechanisms.

## Repair order

`cup repair` deliberately runs recovery before general reconstruction:

```text
validate state/journal relationship
recover or resolve one transaction
restore checkable CUP support assets
refresh update helper
scan current-host packages
preserve foreign-host packages
quarantine identifiable invalid packages
rebuild/save current-host state
rebuild launchers
remove safe staging leftovers
```

Each phase must leave a result that the next phase can trust. An ambiguous
journal or incomplete scan stops later reconstruction instead of allowing CUP to
build a plausible-looking state from partial evidence.

## Interrupts

Mutating commands install native interrupt observation around their operation:

```text
POSIX    SIGINT, SIGTERM
Windows  console control events
```

Handlers record intent only. Download/archive/filesystem loops check at safe
points, and a commit step either finishes or leaves recovery evidence. A handled
cancellation maps to public status `130`.

Once a detached helper has accepted continuous handoff, the initiating process no
longer owns that child's transaction. The helper waits for parent exit before its
first authoritative mutation and then completes or records failure independently.

## Commit-state results

Native replace/move operations distinguish:

```text
SYSTEM_COMMIT_NOT_APPLIED
SYSTEM_COMMIT_APPLIED
SYSTEM_COMMIT_DURABLE
```

`APPLIED` means the destination may already have changed even though required
persistence could not be fully confirmed. Callers must not turn that into an
indistinguishable pre-commit error and blindly roll back.

Relevant internal errors include transaction, commit, rollback, lock and
interrupt failures. The public CLI maps them to the stable exit-status groups in
[Commands](../user/COMMANDS.md#exit-status).

## Main implementation files

| Module | Responsibility |
|---|---|
| `runtime_journal.c` | shared `transaction.txt` file lifecycle |
| `package_transaction.c` | package schema/recovery |
| `update_journal.c` | CUP-update schema/recovery |
| `update_helper.c` | detached CUP update commit |
| `uninstall_journal.c` | uninstall schema/recovery |
| `uninstall_helper.c` | native root detach and cleanup |
| `command_doctor.c` | read-only transaction diagnosis |
| `command_repair.c` | ordered recovery/reconciliation |
| `interrupt.c` | process interrupt observation |

## Related documents

- [State](STATE.md)
- [Packages](PACKAGES.md)
- [Platforms](PLATFORMS.md)
- [Security](SECURITY.md)
- [Commands](../user/COMMANDS.md)
