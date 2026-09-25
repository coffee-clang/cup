# Releases

A `cup` release turns one qualified source commit into five native binaries and one
immutable flat public asset set. Release never rebuilds or edits a candidate
after native candidate qualification; the tested bytes are the bytes considered
for publication.

## Release model

```text
source commit on main
        ↓
complete Tests workflow for that commit/attempt
        ↓
source-tested build identity for each platform
        ↓
five official native candidates
        ↓
native candidate tests
        ↓
assemble complete flat public set
        ↓
generate release.txt last
        ↓
publish immutable release
```

Supported release platforms are:

```text
linux-x64
linux-arm64
macos-x64
macos-arm64
windows-x64
```

## Version and source identity

`VERSION` is the manual public version source of truth. The matching release tag
is `v<version>`. There is no nightly/version auto-increment mechanism.

An official build requires the requested version/tag/full source commit to agree
with the clean checkout being built. Development builds may include Git/archive
identity; official binaries expose exactly the public version.

`.github/workflows/tests.yml` owns source qualification. Release resolves one
successful Tests run **and attempt** for the exact source commit and verifies the
source-tested `build-config.txt` evidence before candidate publication.

Cross-runner equality compares the properties that define build identity:
platform/toolchain, dependency prefix format/profile/revision and source-lock
identity, compiler target/version, and Windows resource compiler identity where
applicable. Runner paths and harmless diagnostic strings remain evidence but are
not equality keys.

## Common public assets

`scripts/release/common-assets.sh` creates every public asset that is independent
of the native platform binary:

```text
LICENSE
THIRD_PARTY_NOTICES.txt
catalog.cfg
install.sh
install.ps1
provenance.txt
```

`catalog.cfg` is acquired from the already-published `cup-components` rolling
release before native candidate qualification. The common release artifact pins
those exact bytes for every platform candidate and for final assembly. `cup` source
does not track a catalog copy, so a later rolling-catalog update cannot silently
change a candidate already under qualification.

Official install policy is compiled into `cup`.

The installers are stamped with the exact release version/tag/commit. The common
asset stage deliberately does **not** generate `release.txt`; at that point the
five final binary bytes are not all known yet.

## Native platform contribution

Each release-matrix job builds one official release configuration from the
verified dependency prefix, finalizes/inspects the executable and keeps native
debug symbols as workflow evidence rather than public release assets.

The public contribution from one platform is only:

```text
cup-<platform>[.exe]
```

`scripts/release/build-platform.sh` prepares that contribution. Generic flat
merging belongs to `assemble-candidate.sh`, which rejects collisions instead of
silently choosing one input.

## Final public asset set

A complete public release contains exactly:

```text
LICENSE
THIRD_PARTY_NOTICES.txt
catalog.cfg
install.sh
install.ps1
provenance.txt
cup-linux-x64
cup-linux-arm64
cup-macos-x64
cup-macos-arm64
cup-windows-x64.exe
release.txt
```

`release.txt` is generated **last**, after all other files have their final
bytes/modes. It is format 2:

```text
format=2
version=<version>
commit=<full source commit>
root_layout=2
catalog_format=1
asset_count=11
asset.0.name=<lexically first public asset except release.txt>
asset.0.sha256=<sha256>
...
```

Every other public asset appears exactly once in lexical name order. The manifest
has no self-hash.

`provenance.txt` records the source repository/commit, exact Tests run/attempt and
Release run that produced the candidate. It is authenticated by `release.txt`.

## Installed generation vs public release

A public release contains installers, catalog seed, provenance and binaries for
all platforms. One installed `cup` generation retains only:

```text
bin/cup[.exe]
release.txt
LICENSE
THIRD_PARTY_NOTICES.txt
```

The live runtime catalog is not a generation asset. Fresh install seeds it from
the release `catalog.cfg`; afterward it may advance independently. Existing-root
reinstall and self-update preserve a valid compatible live catalog.

## Candidate qualification

Native candidate jobs test assembled files on their matching runners without
rebuilding them. Candidate-specific tests cover the published byte contract:

- exact asset membership and `release.txt` digests;
- release/provenance identity;
- executable version/startup/linkage policy;
- fresh install and custom-base selection;
- reinstall/relocation while preserving managed runtime state;
- doctor/repair and generation recovery boundaries;
- self-update through a separate private newer-version fixture;
- uninstall.

The selected Tests workflow already owns source unit/integration/coverage/
sanitizer qualification; Release does not repeat that entire source suite merely
for ceremony.

## Platform binary policy

Release inspection enforces the linkage/format rules in
[Platforms](../design/PLATFORMS.md):

- Linux release executables satisfy the project static-runtime contract;
- macOS statically links third-party dependencies while retaining only approved
  Apple system libraries/frameworks dynamically;
- Windows statically links third-party/compiler runtimes and imports only the
  approved system DLL surface.

Final public binaries are stripped after symbol separation. Debug/source path
metadata belongs in separate workflow artifacts, not publishable executables.

## Fresh installer

The public installer is a minimal transport frontend. It is stamped for one
concrete release and downloads from that exact immutable tag.

On POSIX the script verifies `release.txt`, target binary, legal assets and the
catalog seed before invoking:

```text
cup --internal-bootstrap <verified-source-directory> <selected-base>
```

The Windows shell handoff authenticates the versioned `install.ps1`; PowerShell
then applies the same release-manifest transport contract.

Native bootstrap creates a complete private sibling root and publishes it by one
no-clobber move. Optional Coffee bootstrap and optional PATH integration happen
after the core root is committed and never roll the core generation back.

## Existing-root reinstall and self-update

An explicit installer target may replace a compatible installed generation while
preserving packages, state, preferences, cache and a valid live catalog. A
healthy newer generation is not downgraded.

`cup update cup` first authenticates the **current** canonical binary against the
current installed `release.txt`. It then discovers a newer version and fetches
all target metadata/assets from that exact versioned release. Equal version is a
no-op; downgrade is rejected.

A lazy byte-identical helper copy of the trusted running binary performs the
generation transaction after handoff. Legal files/manifest are committed before
the canonical binary, which is replaced last. The live catalog is never part of
that transaction.

See [Transactions](../design/TRANSACTIONS.md) for commit/recovery semantics.

## Publication

`scripts/release/publish.sh` is the only release script that mutates GitHub
Release state. It snapshots the completed local candidate and validates its exact
asset set and `release.txt` before remote comparison/upload.

Only the publication job receives `contents: write`.

Remote states are handled explicitly:

- no release: create a draft for the tested commit, upload exact bytes, verify,
  then publish;
- matching draft: reconcile only a draft proven to belong to this same candidate;
- concurrent creation: accept only when tag/assets/bytes match the local snapshot;
- already public: read-only success only when the complete remote release already
  matches exactly.

Ambiguous API/network outcomes fail conservatively. Public generations are
immutable by project policy; publication never rewrites an already-published
version into different bytes.

## Workflow responsibilities

- **Dependencies** builds/validates pinned native dependency prefixes.
- **Tests** owns repository quality, native source tests, coverage, sanitizers and
  source-tested build identity.
- **Release** selects one Tests attempt, builds common assets and five native
  platform contributions, then assembles the complete candidate from those exact
  inputs on each native runner. Publication starts only after the complete
  candidate passes all five native release-test jobs.
- **Docs** publishes documentation independently and does not authorize an
  application release.

Release uses a non-cancelling concurrency group. Candidate matrices keep
`fail-fast: false` so one platform failure does not hide evidence from the
others, while publication still requires the complete successful generation.

## Operator sequence

```text
1. choose/update VERSION
2. review and commit/push source on main
3. obtain a successful Tests attempt for that exact commit
4. dispatch Release for that commit
5. build all five native platform contributions
6. assemble the exact public set and release.txt on each native test runner
7. natively test that complete candidate on all five platforms
8. publish the same verified contribution set as the immutable generation
```

Any release-relevant source change requires new source qualification. Candidate
bytes are never reused for another source commit.

## Main release owners

| Script | Responsibility |
|---|---|
| `scripts/build/finalize-release.sh` | finalize/inspect one native binary bundle |
| `scripts/release/common-assets.sh` | prepare common public assets except manifest |
| `scripts/release/build-platform.sh` | prepare one platform binary contribution |
| `scripts/release/assemble-candidate.sh` | merge all parts, generate `release.txt` last, validate exact set |
| `scripts/release/publish.sh` | compare/publish immutable candidate snapshot |

## Related chapters

- [Build](BUILD.md)
- [Testing](TESTING.md)
- [Architecture](../design/ARCHITECTURE.md)
- [Packages](../design/PACKAGES.md)
- [Transactions](../design/TRANSACTIONS.md)
- [Security](../design/SECURITY.md)
