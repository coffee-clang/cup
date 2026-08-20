# Packages

This page describes the package contract shared by `cup` and
`cup-components`. It covers the catalog, package identity, archive layout,
metadata, executable entries and cache behavior.

Security checks related to downloads and extraction are collected in
[Security](SECURITY.md).

## Responsibility split

`cup-components` is responsible for:

```text
building each tool
choosing build features
including the required runtime files
generating info.txt
creating archives
publishing SHA256SUMS
```

`cup` is responsible for:

```text
loading the catalog
selecting one package tuple
downloading and checking the archive
validating archive paths and types
extracting into staging
validating info.txt and declared executable entries
installing under the managed root
updating state and defaults
```

The two repositories communicate through files. cup does not need to know which
Docker image, MSYS2 package or build command produced an archive.

## Catalog locations

Installed catalog:

```text
<cup-root>/config/packages.cfg
```

Repository copy used by development builds when no installed catalog exists:

```text
config/packages.cfg
```

An installed release always uses the catalog inside the selected cup root.

## Registry, policy and preferences

Three inputs take part in package selection:

```text
compiled registry     valid components and component/tool relationships
packages.cfg          packages available for host, target and version
install.cfg           official defaults, profiles and toolchains
preferences.txt       optional user choices
```

The compiled registry is the first check. A catalog cannot introduce an unknown
component or attach a tool to a different component.

### `install.cfg`

The installed file is:

```text
<cup-root>/config/install.cfg
```

Its repository copy is `config/install.cfg`. Blank lines and full-line comments
are ignored. The first semantic record must be the schema marker `format=1`;
surrounding whitespace on semantic lines and around keys, values and comma-list
items is accepted. Supported records after the marker are:

```text
default.<host>.<target>.<component>=<tool>
profile.<name>=<component>,...
toolchain.<name>=<tool>,...
```

Defaults are scoped by component, host and target. Profiles list components and
resolve each one when they are installed. Toolchains list explicit tools and may
contain at most one tool for each component. Names and list items must resolve to
canonical lowercase registry identifiers; duplicate list items and duplicate
records are rejected.

The file is part of the official cup assets and is checked by
`SHA256SUMS.common`.

### `preferences.txt`

User choices are stored at:

```text
<cup-root>/config/preferences.txt
```

The document starts with its schema marker and then contains scoped preferences:

```text
format=1
preferred.<host>.<target>.<component>=<tool>
```

This file is not covered by the official checksum because it belongs to the
user. cup validates it, writes entries in a stable order and replaces it
atomically.

For `cup install <component>`, tool selection is:

```text
explicit command selector
user preference for the scope
official default for the scope
error when none is available
```

Profiles use this order for every component. Toolchains never use preferences,
because their tool set is already explicit.

## Catalog format

`packages.cfg` is a line-based `key=value` file. The first physical line is
exactly the explicit schema marker:

```text
format=1
```

The catalog is a release asset and is replaced as a complete authenticated file;
catalog-schema compatibility is not promised across unsupported formats. After
the format marker, blank lines and full-line comments are ignored.
Every other line must contain one non-empty key and value.

Package keys use:

```text
<component>.<tool>.<host>.<target>.<field>=<value>
```

Each package tuple contains:

```text
stable_version
available_versions
default_format
formats
url_template
checksum_url_template
```

Example:

```text
compiler.gcc.linux-x64.windows-x64.stable_version=16.1.0-rev1
compiler.gcc.linux-x64.windows-x64.available_versions=16.1.0-rev1
compiler.gcc.linux-x64.windows-x64.default_format=tar.gz
compiler.gcc.linux-x64.windows-x64.formats=tar.xz,tar.gz,zip
compiler.gcc.linux-x64.windows-x64.url_template=https://github.com/coffee-clang/cup-components/releases/download/gcc-{version}-{host_platform}-{target_platform}/gcc-{version}-{host_platform}-{target_platform}.{format}
compiler.gcc.linux-x64.windows-x64.checksum_url_template=https://github.com/coffee-clang/cup-components/releases/download/gcc-{version}-{host_platform}-{target_platform}/SHA256SUMS
```

Loading fails for:

- malformed or empty records;
- unknown or duplicated fields;
- incomplete tuples;
- unknown components or tools;
- invalid host or target platforms;
- duplicated versions or formats;
- non-canonical concrete release names (including uppercase forms or `stable` as a real version);
- unsupported archive formats;
- a default format missing from `formats`;
- a stable version missing from `available_versions`;
- non-HTTPS templates;
- unsupported or missing placeholders.

The archive URL must identify the tool, version, host, target and format. The
checksum URL identifies the release tuple and does not vary by archive format.

## URL placeholders

Supported placeholders are:

```text
{tool}
{version}
{host_platform}
{target_platform}
{format}
```

`{format}` belongs to archive URLs. It is not accepted as part of the checksum
release identity.

cup expands a template only after every identity field has passed validation.

## Stable and concrete versions

`stable_version` must also appear in `available_versions`.

`stable` is resolved before a package path, cache name or state entry is
created. Installed state stores the resulting concrete version.

Version strings are treated as identifiers. cup does not compare component
package versions using semantic-version precedence; the catalog decides which
versions are available and which one is stable.

A packaging revision can be part of the version:

```text
16.1.0-rev1
```

The whole string is used in catalog lookup, asset names, metadata, state and
paths.

## Archive formats

Supported formats are:

```text
tar.xz
tar.gz
zip
```

The tuple's `default_format` is used unless `install` receives `--format` or
`-f`. An override must appear in that tuple's `formats` list.

cup reads archives with libarchive. It does not run external `tar`, `gzip`, `xz`
or `unzip` commands during installation.

The detected stream format must match the selected format. A filename extension
is not enough to prove the archive type.

## Package identity and paths

One package identity contains:

```text
component
tool
host platform
target platform
concrete version
```

Installed path:

```text
<cup-root>/components/<component>/<tool>/<host>/<target>/<version>/
```

Cache directory and archive name:

```text
<cup-root>/cache/<component>/<tool>/<host>/<target>/<version>/
  <tool>-<version>-<host>-<target>.<format>
```

The cache name is built locally from validated identity fields. It is never
copied from a response header or remote pathname.

## Archive root and internal paths

A package archive contains one top-level directory. cup does not trust that
directory name as the package identity; `info.txt` must still match the package
selected from the command and catalog.

Archive paths must use safe portable segments. They cannot:

- be absolute;
- contain `.` or `..` segments;
- contain control characters;
- collide after ASCII case folding;
- describe the same path as both a file and a directory;
- contain links or special filesystem objects.

A package may contain tool-specific directories such as:

```text
bin/
lib/
libexec/
include/
share/
<target-triple>/
```

cup does not force every tool to use the same internal layout.

## `info.txt`

Every package also contains a line-based `info.txt`.

Required identity fields are:

```text
package.component
package.tool
package.version
platform.host
platform.target
```

Every package must declare at least one executable entry using an `entry.*`
field. Optional groups may include:

```text
features.*
contents.*
config.*
```

Example:

```text
package.component=compiler
package.tool=gcc
package.version=16.1.0-rev1
platform.host=linux-x64
platform.target=linux-x64
entry.gcc=bin/gcc
features.c=true
features.cpp=true
contents.self_contained=true
config.languages=c,c++,lto
```

The parser rejects malformed lines, duplicate keys, empty values and fields that
exceed the configured limits.

The identity in `info.txt` must match the installed path and the request that
selected the package.

## Executable entries and wrappers

Each `entry.<name>` value must be a safe relative path inside the package. The
target must be present, non-empty, a regular file and executable according to the
platform validation rules.

Wrapper names are derived as follows:

```text
native target    <entry>
cross target     <target>-<entry>
```

Planning rejects duplicate wrapper names and a package entry named `cup`.
Wrappers are derived from the defaults; they are not part of package
identity.

## Package validation

A package is accepted only when:

1. its identity fields are valid;
2. the root is a real directory;
3. `info.txt` is a bounded regular file that parses successfully;
4. metadata matches the selected identity;
5. each declared executable entry is a safe package-relative path to a present
   regular executable file;
6. the package root and `info.txt` still name the same filesystem objects
   observed during validation.

`ValidatedPackage` owns the parsed metadata used for the decision. Commands such
as `inspect`, `default`, `doctor` and wrapper planning all use this same
validation path.

The read-only protection applied to `info.txt` is a managed permission rule. A
permission change is reported by `doctor` and can be restored by `repair`, but
the package still has the same semantic identity.

## Scanning and repair

`repair` scans the component hierarchy. A valid package missing from `state.txt`
can be adopted because the canonical path and validated metadata provide its
identity.

An invalid object is quarantined only when its path gives a safe package identity
and the regular file or directory still has the native identity observed during
the scan. Unknown or ambiguous paths, links and special filesystem objects are
reported and left unchanged.

The scan records both the returned entries and the real totals. If an internal
capacity is exceeded, repair stops before changing state instead of acting on a
partial view.

## Cache behavior

A cache entry is reused only after its digest has been checked against the
release `SHA256SUMS`.

The cache returns a `VerifiedArtifact` that owns the open file. The same stream
is used for hashing and the single archive validation/extraction pass.

When checksum metadata is refreshed, cup compares the new expected digest with
the digest already calculated for that open file. It does not reopen the path.

If a cached archive fails extraction or package validation, cup removes it only
when the cache pathname still identifies the same opened object. It then performs
one network refresh. A second failure is returned to the user instead of being
retried forever.

## Limits

cup limits package scans and archive work to bounded arrays and counters. Important ceilings include:

```text
262,144 entries
256 MiB of stored path-table text
16 GiB for one regular file
64 GiB total extracted bytes
64 path segments
```

These values are safety ceilings, not expected package sizes. Exceeding a limit
causes a failure; it never silently truncates the package.

## Implementation and tests

The responsible C modules are listed in [Architecture](ARCHITECTURE.md). Test
levels are described in [Testing](../development/TESTING.md).

## Related documents

- [Architecture](ARCHITECTURE.md)
- [Security](SECURITY.md)
- [State](STATE.md)
- [Commands](../user/COMMANDS.md)
