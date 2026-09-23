# Transactions and recovery

`cup` journals only persistent mutations whose commit evidence must survive a
process exit. A journal is not an event log and does not record cosmetic phases
unless the lifecycle genuinely needs them.

All managed transaction schemas share:

```text
<cup-root>/transaction.txt
```

and use private work below `staging/` when bytes must survive recovery. Only one
transaction can own the root at a time.

## Journal transport

`runtime_journal.c` owns the common file mechanics: bounded regular-file
snapshots, create-only first publication, native identity checks and
identity-bound deletion. The operation-specific modules own schema and recovery
semantics.

Unknown, malformed or contradictory journal data is preserved. Ordinary
commands do not clear a journal just to make the root usable again.

While a journal is present:

- help/version remain root-independent;
- `doctor` reports the pending operation without modifying it;
- `repair` may perform only evidence-backed recovery;
- ordinary conflicting commands are blocked.

## Package install/remove

Package journals are format 2:

```text
format=2
operation=install|remove
component=<component>
tool=<tool>
target_platform=<target>
package_version=<concrete-version>
temporary_name=<identity-bound-staging-name>
```

Host is implicit from `root.txt`. There is no persistent package `update`
operation: updating to a newer immutable package identity reuses the normal
install transaction.

Download, digest verification, extraction, metadata/manifest validation and
prospective wrapper planning occur before journal publication while staging is
still disposable.

### Install

```text
prepare verified staging
publish package journal
publish canonical package tree
commit state.txt                         <- logical commit evidence
reconcile derived wrappers
clear journal and disposable staging
```

If valid state already contains the identity after a crash, recovery keeps or
restores the journal-owned package. If state does not contain it, recovery
removes the uncommitted package/staging. If state is missing or malformed,
recovery cannot decide which side committed and preserves the evidence.

### Remove

Removal journals before moving the exact canonical package into journal-owned
staging. State then decides recovery:

```text
identity still present in valid state
  -> removal did not commit; restore the staged package

identity absent from valid state
  -> removal committed; finish deleting staged bytes
```

A corrupt package can therefore be removed without first pretending it is
healthy. If a conflicting canonical object appears, repair preserves the invalid
object before restoring proven staged bytes.

## Catalog refresh

Catalog refresh is intentionally **not** a `transaction.txt` operation. Its
commit evidence is the local catalog revision/digest plus native file identity.
The lifecycle is:

```text
shared lock -> snapshot local catalog -> unlock
network download + parse/validate
exclusive lock -> reload local catalog
compare-and-swap -> replace, retry, or stop
```

Network I/O never occurs while the runtime lock is held. Retry is bounded.
Remote rollback is rejected; equal revision requires byte identity; a higher
validated revision may replace the local snapshot.

A catalog refresh that committed is not rolled back merely because later package
planning/download fails.

## Fresh installation

Fresh install is not a generation journal operation. The native bootstrap builds
a complete private sibling under the selected base, for example:

```text
.cup-install-<token>/
```

That private root receives an authenticated root marker, runtime directories,
empty state, the release-pinned catalog snapshot and the complete `cup` generation.
Immediately before publication `cup` rechecks root selection. The same originally selected
final path must still be absent.

Publication is one no-clobber directory move into `.cup` or the selected
`.coffee-cup`. A crash before that move may leave only disposable private sibling
state; there is no half-published managed root to recover.

## Existing-root generation replacement

Reinstall and self-update preserve packages, state, preferences, cache and a
valid live catalog. Only the `cup` generation changes.

The minimal generation journal is:

```text
format=2
operation=cup-generation
target_release_sha256=<sha256 of target release.txt>
temporary_name=<generation-workspace>
```

The workspace contains fixed `new/` and `old/` directories. `new/` holds the
complete target generation. `old/` contains the previous generation bytes needed
for rollback.

Generation commit is binary-last:

```text
verify complete target generation
snapshot old generation
publish journal
install target release.txt / LICENSE / notices
install target cup[.exe] last             <- generation commit boundary
validate canonical target generation
clear journal/workspace
```

The main binary is the decisive boundary because normal repair must not replace
its own running executable.

### Generation recovery

Recovery reasons from actual bytes, not a stored phase:

```text
canonical generation exactly matches target
  -> commit completed; finalize by clearing journal/workspace

canonical binary still proves old generation
  -> binary commit did not happen; roll non-binary generation assets back

canonical binary is neither proven old nor complete target
  -> ambiguous evidence; preserve everything and stop
```

This keeps the journal small and avoids inventing a second phase state machine
whose claims could disagree with the filesystem.

## `cup update cup`

Self-update is available only to an official managed generation. Before creating
helper or journal state, the running canonical `cup` binary must hash-match the
entry in the current valid installed `release.txt`. A missing/corrupt manifest or
binary mismatch stops before generation mutation.

The updater resolves a concrete target release, then downloads metadata/assets
from that exact versioned release. Equal version is a no-op and downgrade is
rejected. The live catalog is not a generation asset and is not replaced.

A byte-identical helper copy is prepared lazily from the trusted running binary.
The parent starts the helper while still owning exclusive root authority. After
the parent execution boundary, the helper reacquires the canonical lock under the
handoff protocol and performs the binary-last generation commit.

Parent success means the handoff was accepted. It does not claim that the target
binary was already committed before the parent returned.

If helper commit fails, it leaves journal/workspace evidence for later
inspection/recovery rather than writing a secondary “failed phase” over an
already uncertain commit.

## Uninstall

Uninstall keeps its separate proven lifecycle because its commit boundary is
root detachment rather than state or binary replacement.

The journal is:

```text
format=2
operation=uninstall
phase=scheduled|detaching|failed
temporary_name=.cup-uninstall-<token>
token=<token>
error=0|6
```

The parent authenticates and locks the root, publishes the journal and starts a
verified native helper with continuous handoff authority. After the parent exits,
the helper validates root/journal/token, publishes `detaching`, and moves the
canonical root to the token-bound sibling path:

```text
canonical root -> detached sibling          <- uninstall commit
```

Cleanup then removes managed payload, the transaction last, and finally the
empty detached root. POSIX can unlink the verified running helper path; Windows
uses its process/file-lifetime mechanism for deferred executable deletion.

A later fresh installer does not adopt or automatically delete detached uninstall
residue. Repair can cancel/acknowledge stale **pre-detach** uninstall state only
when no detached root owns the operation.

## Repair order

Repair follows evidence in this order:

```text
pending transaction
package scan / quarantine
state reconstruction / validation
preferences
wrappers
staging garbage
installed generation
live catalog
```

Transaction ambiguity stops later reconstruction. State reconstruction is
preflighted against the 4 MiB canonical budget before repair performs mutations
that depend on that reconstructed state. Repair never invents defaults.

A malformed official catalog may be preserved and restored from the authenticated
release snapshot. Development repair preserves the failure and requires an explicit
published `cup-components` snapshot before `cup update catalog`. A well-formed
unsupported future catalog is preserved/refused rather than downgraded.

Generation repair never guesses a replacement main binary. Same-version
metadata/legal repair is permitted only when the existing canonical binary can
be authenticated against trusted release metadata; binary replacement remains
installer/self-update territory.

## Interrupts and commit results

Native interrupt handlers record intent only. Download/archive/filesystem loops
observe it at safe points; durable commits either complete or leave recovery
evidence. Public cancellation maps to status 130.

Filesystem replace/move primitives distinguish not-applied, applied-but-not-
fully-confirmed and durable outcomes. Callers must not turn an uncertain visible
commit into a false pre-commit error and blindly restore old state.

## Main implementation owners

| Module | Responsibility |
|---|---|
| `runtime_journal.c` | shared `transaction.txt` transport/detection |
| `package_transaction.c` | install/remove journal and package recovery |
| `catalog_refresh.c` | lock-free-network catalog CAS lifecycle |
| `update_journal.c` | `cup`-generation workspace, commit and recovery |
| `update_helper.c` | detached self-update handoff/commit |
| `uninstall_journal.c` | uninstall phase journal |
| `uninstall_helper.c` | root detach and cleanup |
| `command_doctor.c` | read-only diagnosis |
| `command_repair.c` | ordered evidence-based reconciliation |

## Related documents

- [Architecture](ARCHITECTURE.md)
- [State](STATE.md)
- [Packages](PACKAGES.md)
- [Platforms](PLATFORMS.md)
- [Security](SECURITY.md)
