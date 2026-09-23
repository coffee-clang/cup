# Packages

CUP consumes immutable prebuilt packages published by `cup-components`. The
package pipeline and the runtime catalog deliberately expose concrete identities
rather than asking CUP to reconstruct producer decisions from templates.

## Responsibility split

The compiled registry describes the domain understood by one CUP generation:
component/tool relationships, platform identifiers and version semantics. The
compiled policy describes official abbreviated choices, profiles and toolchains.
The catalog describes what has actually been published. User preferences apply
only to future abbreviated installs.

These roles are independent. Adding a tool to the registry does not make it an
official default, and a catalog containing one tool for a component does not
make that tool an implicit policy choice.

## Package identity and versions

A public package identity is:

```text
(component, tool, version, host, target)
```

Current operational tools use canonical numeric-dotted base versions. Segments
are compared numerically, with a matching extra segment later in the ordering:
`1.2 < 1.2.0`.

Every operational package may add one terminal CUP package revision:

```text
23.1.0
23.1.0-rev1
23.1.0-rev2
```

`-revN` is a package-generation revision, not part of the upstream source
version. Ordering first compares the complete base version and compares the
revision only when the base is identical. Consequently:

```text
23.1.0 < 23.1.0-rev1 < 23.1.1
23.1.0-rev9 < 23.1.0-rev10
1.2-rev99 < 1.2.0
```

A corrected redistribution of the same upstream version receives a new
immutable package identity such as `-rev1`; CUP does not replace published bytes
behind an existing identity.

`source.primary.version` in `info.txt` always contains the unsuffixed upstream
base. A revision-bearing package must also contain `package.revision_reason`.
There is no separate numeric revision field.

Coffee is registry-recognized but remains non-operational until its real version
and package contract is frozen and shipped by a CUP generation. Older CUP
versions can retain future Coffee records structurally without guessing their
ordering.

## Catalog snapshot

`catalog.cfg` is a concrete consumer snapshot. Its top-level header is:

```text
format=1
revision=<monotonic integer>
update_url=<HTTPS catalog URL>
```

A package record contains the complete identity, one derived stable bit and one
or more concrete artifacts. Conceptually:

```text
package.0.component=compiler
package.0.tool=clang
package.0.host=linux-x64
package.0.target=linux-x64
package.0.version=23.1.0-rev1
package.0.revision_reason=<reason>
package.0.stable=true
package.0.artifact.0.format=tar.gz
package.0.artifact.0.url=https://.../clang-23.1.0-rev1-linux-x64-linux-x64.tar.gz
package.0.artifact.0.sha256=<64 lowercase hex>
```

`revision_reason` is present exactly when the package version carries `-revN`.
There are no URL templates, version lists, catalog-side manifest digest or
catalog-side default archive format.

Within one `(component, tool, host, target)` scope, stable is the semantic
maximum package version. The producer materializes this bit, while CUP trusts it
only for a record whose tool/platform/version/archive contract it understands.
If the persisted stable record is future/non-operational, CUP does not promote a
lower record on its own.

The parser distinguishes structural validity from operational support. This
lets an older CUP retain safe future records without treating the complete
snapshot as corrupt. Search and install expose only records that the running CUP
can operate.

## Catalog source and publication

The source catalog belongs to `cup-components`. After one immutable package
release is published and verified, its serialized catalog step activates that
publication, writes and commits the canonical source catalog, then publishes the
same snapshot to the rolling `catalog` GitHub Release. The rolling asset URL is
stable while catalog revision provides consumer chronology. Revision 0 is
published manually during initial bootstrap; manual publication otherwise serves
explicit recovery or administrative cases.

CUP does not track a catalog snapshot. Development may use a local ignored
`config/catalog.cfg` copied from a published `cup-components` snapshot. Release
preparation acquires the currently published catalog before native qualification
and pins those exact bytes as the candidate's bootstrap/recovery seed. A live
runtime catalog may later advance independently.

## Selectors and resolution

Exact selectors identify one concrete immutable package:

```text
clang@23.1.0
clang@23.1.0-rev1
```

The first selector never aliases the second. `stable` is symbolic and resolves
through the current catalog snapshot:

```text
clang@stable
```

A component-only install first checks the user preference for that
`(target, component)` scope, otherwise the compiled official policy, then
resolves that selected tool's stable package. A user preference that exists but
cannot be satisfied is an error; CUP does not silently replace it with the
official choice.

Profiles select component roles and therefore respect preference/policy.
Toolchains select concrete tools and do not consult preferences.

A group command performs at most one command-scoped refresh, freezes one logical
catalog snapshot and preflights all requested members and formats before the
first package mutation.

## Archive formats

Normal package publication provides `tar.xz`, `tar.gz` and `zip`. CUP chooses a
transport default as consumer policy: `tar.gz` on POSIX hosts and `zip` on
Windows. `--format` can request another published format.

Archive format is transport, not package identity. All formats for one package
identity must unpack to the same package tree. Raw hardlinks are outside the CUP
archive boundary; the producer materializes required content before publication.

## Package paths

The root already authenticates the host, so the local package path is:

```text
<root>/components/<component>/<tool>/<target>/<version>/
```

Host remains explicit in catalog and package metadata because those objects can
cross roots and repositories.

## `info.txt`

`info.txt` describes the package. Required information includes:

```text
package.component=<component>
package.tool=<tool>
package.version=<complete package version>
package.revision_reason=<only for -revN>
platform.host=<host>
platform.target=<target>
source.primary.name=<source>
source.primary.version=<unsuffixed upstream version>
source.primary.url=<source URL>
source.primary.sha256=<source digest>
entry.<metadata-id>=<relative command path>
```

Producer metadata may additionally describe triples, runtime family, build
information, bundled dependencies or features when that information has real
inspection value. `info.txt` does not repeat transport formats or a constant
`self-contained` flag.

The metadata key after `entry.` is an identifier, not necessarily the public
command spelling. The public wrapper name is derived from the basename of the
entry path. For example:

```text
entry.clang_tidy=bin/clang-tidy
```

provides the public command `clang-tidy`.

## `manifest.txt`

`manifest.txt` is the exact extracted-tree authority. Format 2 records every
managed directory, file and permitted symlink with its required mode/content
identity. Package admission validates the tree against this manifest before any
canonical package commit.

The catalog authenticates the archive SHA-256. Because the authenticated archive
contains `manifest.txt`, CUP does not duplicate the manifest digest in the
runtime catalog or state.

## Download and cache

The cache is an optimization keyed only by the catalog artifact digest:

```text
<root>/cache/<artifact-sha256>
```

A cache hit must be a safe regular file and is rehashed before use. A mismatch,
wrong object type or unusable cache entry is bypassed. Cache write failure does
not make an otherwise verified package install fail.

On a miss, CUP downloads into private temporary storage, verifies the catalog
digest, may publish those verified bytes into the cache, and continues package
admission from the verified artifact. The cache never reconstructs catalog
availability.

## Admission and installed integrity

Before a package is committed, CUP validates archive safety, metadata identity,
manifest/tree integrity and the prospective wrapper namespace. An exact package
already recorded in state is not silently overwritten. If its installed tree is
invalid, repair/remove/reinstall is the explicit path.

After installation, `state.txt` owns the logical installed identity while
`manifest.txt` owns the package tree. Scanning can reconstruct valid package
identities for repair, but ordinary commands do not adopt untracked package
directories just because their paths look plausible.

## Update semantics

Package update first performs a cheap local installed-family precheck. If there
is something to update, it performs one required catalog refresh and plans from
the resulting snapshot.

For each `(component, tool, target)` family, the reference is a same-tool active
default when one exists, otherwise the maximum installed release of that tool.
Lower catalog stable never downgrades the reference. Equal stable is verified
and left unchanged. Newer stable is installed/adopted; the old version remains
installed. The default advances only when it was an older release of that same
tool.

A locally installed version that has disappeared from the current catalog is
not deleted. Current availability and local history are separate authorities.

## Resource boundaries

Catalog and state documents have explicit byte budgets rather than arbitrary
small record-count caps. Package/state collections are dynamically represented
inside those bounds. Defaults and preferences remain naturally bounded by the
finite `(component, target)` scope set.

Archive extraction, transfer sizes, metadata lengths and path segments retain
separate defensive limits at the boundary where those resources are consumed.

## Related documents

- [Architecture](ARCHITECTURE.md)
- [State](STATE.md)
- [Transactions](TRANSACTIONS.md)
- [Security](SECURITY.md)
- [Commands](../user/COMMANDS.md)
