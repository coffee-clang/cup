# Packages

This page defines the package contract between CUP and `cup-components`. It
covers package selection, catalog/policy files, archive admission, `info.txt`,
`manifest.txt`, installed paths and cache behavior.

Tool-specific build recipes are not part of this repository. CUP consumes the
published result and validates the parts of the package format it owns.

## Producer and consumer responsibilities

`cup-components` owns:

```text
tool build and feature selection
runtime closure inside the package
package metadata
manifest generation
archive production
release SHA256SUMS
native validation of the produced tool
```

CUP owns:

```text
component/tool/platform domain
catalog and install policy parsing
package selection
archive/checksum download and cache
archive path/type/resource admission
staged extraction
metadata and executable-entry validation
manifest integrity verification
canonical installation, state and defaults
```

The package files are the interface. CUP does not need to understand the Docker,
MSYS2, Xcode or native build process that produced them.

## Selection inputs

Four inputs participate in installation selection:

| Input | Purpose |
|---|---|
| compiled registry | valid components, tools and platforms |
| `packages.cfg` | available host/target/version/archive tuples |
| `install.cfg` | official defaults, profiles and toolchains |
| `preferences.txt` | optional user choices for abbreviated installs |

The compiled registry is a closed domain. A catalog can make a known tool
available or unavailable for a scope; it cannot add a new component/tool
relationship.

Installed official copies live below `<cup-root>/config/`. Development builds
can use the repository `config/` copies when no installed generation exists.

## `install.cfg`

The file starts with:

```text
format=1
```

It accepts these record families:

```text
default.<host>.<target>.<component>=<tool>
profile.<name>=<component>,...
toolchain.<name>=<tool>,...
```

Official defaults are scoped by host, target and component. A profile contains
components; each component is resolved through user preference and then the
official default when the profile is installed. A toolchain contains explicit
tools and therefore does not consult preferences.

Names must resolve through the compiled domain. Duplicate records/items are
invalid, and a toolchain may contain at most one tool for each component.
Availability is still checked against `packages.cfg`, so a preset is not assumed
to exist for every host/target combination.

`install.cfg` is part of the official CUP generation and is authenticated by the
release checksum chain.

## `preferences.txt`

User preferences are stored as:

```text
format=1
preferred.<host>.<target>.<component>=<tool>
```

The file belongs to the user and is not an official checksummed asset. CUP
validates the complete document and publishes updates atomically.

For an abbreviated component install, the selection order is:

```text
explicit tool in the command, when present
user preference for the scope
official default for the scope
error when no valid selection exists
```

Preferences influence future component/profile installs only; they do not change
current defaults.

## `packages.cfg`

The catalog is a line-based `key=value` document whose first physical line is:

```text
format=1
```

Blank lines and full-line comments are permitted after the marker. Each package
tuple uses:

```text
<component>.<tool>.<host>.<target>.<field>=<value>
```

Required fields are:

```text
stable_version
available_versions
default_format
formats
url_template
checksum_url_template
```

Example shape:

```text
compiler.gcc.linux-x64.windows-x64.stable_version=<version>
compiler.gcc.linux-x64.windows-x64.available_versions=<version>,...
compiler.gcc.linux-x64.windows-x64.default_format=tar.gz
compiler.gcc.linux-x64.windows-x64.formats=tar.xz,tar.gz,zip
compiler.gcc.linux-x64.windows-x64.url_template=https://.../{tool}-{version}-{host_platform}-{target_platform}.{format}
compiler.gcc.linux-x64.windows-x64.checksum_url_template=https://.../SHA256SUMS
```

The parser rejects malformed/duplicate/incomplete tuples, unknown domain values,
duplicate versions or formats, unsupported archive formats, invalid stable/default
selections, non-HTTPS templates and invalid placeholders.

Supported placeholders are:

```text
{tool}
{version}
{host_platform}
{target_platform}
{format}
```

`{format}` belongs to archive URLs and is not part of the checksum release
identity. CUP expands templates only after every identity field has been
validated.

## Releases

`stable_version` must also appear in `available_versions`. `stable` itself is a
selector and is never stored as a concrete package version.

Version strings are opaque package identifiers. CUP does not infer package
ordering through semantic-version rules; the catalog decides what exists and
which release is stable. Producer packaging revisions can therefore be part of a
version string, for example:

```text
16.2.0-rev1
```

The complete string participates in package lookup, filenames, metadata, state
and paths.

## Archive formats

CUP accepts:

```text
tar.xz
tar.gz
zip
```

The catalog's `default_format` is used unless the install command chooses another
published format with `--format`/`-f`.

Archives are decoded with libarchive. CUP does not execute system `tar`, `gzip`,
`xz` or `unzip` during package installation. The decoder-reported archive/filter
stack must agree with the selected format; the filename extension is not treated
as proof of content.

## Package identity and paths

One package identity is:

```text
component
tool
host platform
target platform
concrete version
```

The installed path is:

```text
<cup-root>/components/<component>/<tool>/<host>/<target>/<version>/
```

The cache path is:

```text
<cup-root>/cache/<component>/<tool>/<host>/<target>/<version>/
  <tool>-<version>-<host>-<target>.<format>
```

Cache names are constructed from validated identity fields; response headers and
remote pathnames never select local destinations.

## Archive tree

A package archive contains one top-level directory. That directory name is only
archive structure: package identity still comes from the selected request and
validated `info.txt`.

Portable archive paths are relative slash-separated printable-ASCII paths. CUP
rejects empty, `.` or `..` segments, backslashes, colons, Windows-reserved
punctuation/device names, trailing-dot aliases, case-fold collisions, file/
directory aliases, hard links and special filesystem objects.

POSIX packages may contain relative symbolic links. Extraction admits only
lexically confined targets and never allows a link to become the parent of a
later archive write. Final manifest validation is stricter: each link must
resolve physically within the package and end at a regular file. Declared
commands must resolve to executable regular files. Windows package content does
not admit symbolic links/reparse objects.

Raw archive hard links are deliberately outside the consumer contract.
`cup-components` materializes final hard-linked staging files as independent
regular files before publication.

The package may otherwise choose the internal layout needed by its tool, such as
`bin/`, `lib/`, `libexec/`, `include/`, `share/` or target-specific directories.

## `info.txt`

Every package contains line-based semantic metadata. The identity fields are:

```text
package.component
package.tool
package.version
platform.host
platform.target
```

The common consumer contract also requires:

```text
package.mode=self-contained
package.formats=<exact set: tar.xz,tar.gz,zip>
platform.host_triple
platform.target_triple
platform.family
platform.runtime
platform.thread_model
build.environment
build.source_policy
source.primary.name
source.primary.version
source.primary.url
source.primary.sha256
```

The source digest is canonical SHA-256. GCC packages additionally use
`package.revision`, which must agree with the package version's `-revN` suffix.

Every package declares at least one provided command through `entry.*`.
Additional producer-owned metadata is grouped as:

```text
features.*    behavioral capabilities described by the producer
contents.*    important payload groups
bundle.*      composed tool/source inputs
requires.*    platform prerequisites outside the payload
config.*      build choices useful when inspecting the package
```

CUP validates the syntax and common package contract and exposes this data through
`cup inspect`; tool-specific interpretation and native validation remain producer-owned.

Example excerpt:

```text
package.component=compiler
package.tool=gcc
package.version=16.2.0-rev1
platform.host=linux-x64
platform.target=linux-x64
entry.gcc=bin/gcc
features.c=true
contents.self_contained=true
```

Duplicate keys, malformed lines, empty values and values beyond the format limits
are rejected. Identity must match both the selected package and its canonical
installed path.

## `manifest.txt`

A finalized package contains a complete logical inventory using `format=2`.
`manifest.txt` itself is not listed; every other descendant is present exactly
once and records are path-ordered:

```text
d<TAB>0755<TAB>-<TAB><path>
f<TAB>0644|0755<TAB><sha256><TAB><path>
l<TAB>-<TAB><sha256-of-target-text><TAB><path>
```

Directory/file modes are normalized by the producer. Link records are POSIX-only
and bind the exact relative target text. Windows manifests therefore contain only
directories and regular files.

`cup-components` generates the manifest from the final package tree and
regenerates/checks it for every published archive format. Tar.xz, tar.gz and ZIP
are required to represent the same logical package.

During installation CUP validates metadata/entries and then compares the entire
staged tree with the manifest:

- every declared object must exist with the expected type;
- regular-file digest and normalized mode must match;
- directories and POSIX link target text must match;
- finalized POSIX links must resolve inside the package to regular files;
- undeclared objects are rejected.

This full integrity check is also appropriate for package scanning, `doctor` and
repair admission. Lightweight metadata queries do not re-hash every package file.

## Provided commands

Each `entry.<name>` is a safe package-relative path. Final validation requires it
to resolve inside the package and reach a non-empty executable regular file.

Default packages expose their entries through CUP launchers:

```text
native target  <entry>
cross target   <target>-<entry>
```

Launcher planning rejects collisions and the reserved CUP executable name.
Launchers are derived from defaults; they are not part of package identity.

## Admission sequence

A package entering the installed tree passes these boundaries:

```text
catalog/request validation
        ↓
checksum-authenticated archive stream
        ↓
archive format/path/type/resource admission
        ↓
private staged tree
        ↓
info.txt + entry validation
        ↓
manifest.txt full-tree integrity validation
        ↓
transactional publication + state commit
```

The separation matters: archive safety is checked while constructing the staged
tree, while package identity/integrity is checked against the final staged tree.

## Cache behavior

A cached archive is reusable only after its digest matches the published
`SHA256SUMS` record. The cache returns a `VerifiedArtifact` that owns the already
opened file; hashing and extraction therefore refer to the same stream.

If checksum metadata is refreshed, CUP revalidates the digest already calculated
for that open file rather than reopening the cache pathname.

A cached object that fails extraction or package validation is removed only when
the pathname still identifies that same observed object. CUP performs one fresh
network download; a second failure is returned instead of retried indefinitely.

## Scanning and repair

`repair` can adopt a valid current-host package that exists at the canonical
component path but is missing from state. Invalid identifiable package objects
may be moved intact to recovery/quarantine storage. Unknown or ambiguous paths,
links and special objects are reported and preserved.

Scans retain both returned entries and the real discovered totals. If a capacity
limit prevents a complete view, repair stops before reconstructing state from a
partial scan.

## Resource limits

Archive and package processing is bounded. Important ceilings include:

```text
262,144 package entries
256 MiB stored path-table text
16 GiB one regular file
64 GiB total extracted bytes
64 path segments
```

These are safety limits, not expected package sizes. Exceeding one is an error;
CUP does not silently truncate a package.

## Related documents

- [Concepts](../user/CONCEPTS.md)
- [Architecture](ARCHITECTURE.md)
- [State](STATE.md)
- [Security](SECURITY.md)
- [Commands](../user/COMMANDS.md)
