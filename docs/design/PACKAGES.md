# Packages

This document defines the package catalog and the artifact contract between
`cup` and `cup-components`. Archive security is expanded in
[SECURITY](SECURITY.md).

## Ownership boundary

`cup-components` owns:

```text
building each tool
selecting its configured features
including required runtime files
creating info.txt
creating archives
publishing SHA256SUMS
```

`cup` owns:

```text
catalog parsing and tuple selection
download and checksum verification
archive safety
metadata and executable validation
canonical installation paths
state and default management
```

The contract is intentionally file-based. `cup` does not need knowledge of the
component build system, Docker images, MSYS2 packages or Homebrew formulas.

## PackageCatalog locations

Installed catalog:

```text
~/.cup/config/packages.cfg
```

Repository development copy:

```text
config/packages.cfg
```

A normal installation uses the installed catalog. A development executable may
fall back to the repository copy only when the installed file is missing.

## Installation policy and local preferences

Package availability and install selection are separate contracts. The compiled
registry remains the sole authority for recognized components, recognized tools
and each tool's component. A catalog can make one registered pair available for
a platform, but cannot introduce a new component/tool relationship.

The official installation policy is installed at `~/.cup/config/install.cfg`;
its repository development copy is `config/install.cfg`. It defines:

```text
default.<host>.<target>.<component>=<tool>
profile.<name>=<component>,...
toolchain.<name>=<tool>,...
```

Every reference is validated against the compiled registry. Defaults are scoped
by component, host and target. Profiles contain components and resolve each one
at install time. Curated toolchains contain explicit tools, with at most one tool
per component. The file is covered by `SHA256SUMS.common`, protected as an
official asset and replaced transactionally by `cup update cup`.

Local choices are stored separately at `~/.cup/config/preferences.txt`:

```text
preferred.<host>.<target>.<component>=<tool>
```

This mutable file has no official checksum because the user owns it. CUP parses
it strictly, serializes entries deterministically and replaces it atomically.
Selection order for an abbreviated component install is:

```text
explicit command selector
scoped user preference
scoped official default
error
```

The selected tool is then checked against the registry and the exact host/target
tuple is looked up in `packages.cfg`. Profiles intentionally apply this hierarchy
to each component. Toolchains never consult local preferences or installed
execution defaults. Profile, toolchain, component, tool, platform and symbolic
`stable` values are canonical lowercase; concrete version identifiers remain
case-sensitive. Updates operate only on installed state and never select new
tools from these preferences.

## Catalog record model

The catalog is a line-based key/value document. Keys use:

```text
<component>.<tool>.<host_platform>.<target_platform>.<field>=<value>
```

Every tuple requires:

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
compiler.gcc.linux-x64.windows-x64.url_template=
  https://github.com/coffee-clang/cup-components/releases/download/
  gcc-{version}-{host_platform}-{target_platform}/
  gcc-{version}-{host_platform}-{target_platform}.{format}
compiler.gcc.linux-x64.windows-x64.checksum_url_template=
  https://github.com/coffee-clang/cup-components/releases/download/
  gcc-{version}-{host_platform}-{target_platform}/SHA256SUMS
```

## Catalog validation

Loading rejects:

- malformed key/value lines;
- empty values;
- unknown or missing fields;
- duplicate field keys;
- duplicate values in `available_versions` or `formats`;
- unsupported component/tool pairs;
- invalid host or target identifiers;
- unsupported archive formats;
- a default format absent from `formats`;
- a stable version absent from `available_versions`;
- non-HTTPS templates;
- unknown placeholders;
- missing required placeholders;
- format-dependent checksum templates.

The URL template must distinguish the concrete package by tool, version, host,
target and format. The checksum template identifies the matching release tuple
without depending on archive format.

Validation is strict because a malformed catalog can otherwise map different
identities to the same remote asset or make an installation non-reproducible.

## URL placeholders

Supported placeholders are:

```text
{tool}
{version}
{host_platform}
{target_platform}
{format}
```

`{format}` is used by archive URLs but is not required or accepted as a
checksum-release discriminator. Template expansion occurs only after all
identity values pass their own validation.

## Stable and available versions

`stable_version` is one concrete version from `available_versions`.

A command input such as:

```text
gcc@stable
```

is resolved before a package path or state entry is created. Advancing the
catalog pointer does not mutate existing state. `cup update` explicitly
installs the new stable version and may move a matching default while retaining
older versions.

Version strings are identifiers, not values interpreted through semantic
version precedence. The catalog decides availability and stable selection.

## Archive formats

Current supported formats are:

```text
tar.xz
tar.gz
zip
```

The tuple's `default_format` is used when `install` receives no override.
`--format` or `-f` can select another value only when it appears in that tuple's
`formats` list.

`cup` uses libarchive directly. Runtime installation does not invoke external
`tar`, `gzip`, `xz` or `unzip` programs. The detected stream must match the
selected format; file extensions and catalog values are not accepted as proof
of the actual archive type.

## Package identity

One package identity contains:

```text
component
tool
host platform
target platform
version
```

Canonical installation path:

```text
~/.cup/components/<component>/<tool>/<host>/<target>/<version>/
```

Canonical cache directory and filename:

```text
~/.cup/cache/<component>/<tool>/<host>/<target>/<version>/
  <tool>-<version>-<host>-<target>.<format>
```

The cache filename is built locally from validated identity fields. It is not
copied from a URL path or response header.

## Archive root

An archive must contain exactly one common top-level directory. Internal names
use printable portable ASCII segments and cannot collide under ASCII
case-folding or as file-versus-directory paths. The root name is not used as the
trusted identity; after extraction, `info.txt` must match the identity selected
from the command and catalog.

The package root can contain tool-specific directories such as:

```text
bin/
lib/
libexec/
include/
share/
<target-triple>/
```

`cup` does not require every package to have the same internal layout. It
requires valid metadata and declared executable entries.

## `info.txt`

Every package root contains a line-based `info.txt` generated by
`cup-components`.

Required identity fields:

```text
package.component
package.tool
package.version
platform.host
platform.target
```

Additional grouped fields can include:

```text
entry.*
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

Parsing rejects malformed lines, empty keys or values, duplicate keys and input
that exceeds defined line, key or value limits.

## Executable entries

Each `entry.<name>` value is a safe relative path inside the package. Package
validation requires the declared object to be a regular executable file.

The entry name is later used to derive managed commands:

```text
native target       <entry>
cross target        <target>-<entry>
```

Entry-point planning rejects duplicate names, collisions between defaults and a
wrapper named `cup`. See [STATE](STATE.md#managed-entry-points).

## Package validation

A package is valid only when:

- the canonical identity is syntactically valid;
- the package root is a directory;
- `info.txt` is a regular file;
- all required identity fields are present;
- metadata identity matches the canonical path;
- every declared entry path is safe and exists as an executable regular file.

Read-only protection of `info.txt` is a separate managed invariant. Installation
applies it, `doctor` diagnoses drift and `repair` restores it, but a permission
change does not alter the package identity or make metadata parsing a different
command-specific contract.

`inspect`, `default`, `info`, `doctor` and state reconciliation use the same
package contract rather than implementing command-specific interpretations.

## Package scan and adoption

`repair` scans the canonical component hierarchy. A valid package not present in
state can be adopted because its path and metadata provide a complete identity.

An invalid object at the complete version level can be quarantined only when its
path provides a safe, canonical identity. Ambiguous or unrecognized paths are
reported and left unchanged.

The scan records both returned entries and total entries. If results were
truncated by capacity limits, repair stops before changing packages or state.
This avoids treating an incomplete observation as a complete model.

## Cache behavior

A cached archive is reused only after its checksum is verified against the
release `SHA256SUMS`. If extraction or package validation fails for a cached
archive, `cup` discards it and performs one network refresh. A second failure is
reported; it is not retried indefinitely.

This distinction handles a stale or locally corrupted cache without hiding a
consistently invalid published package.

## Version revisions

A tool release can include a packaging revision in its concrete version, for
example:

```text
16.1.0-rev1
```

The complete string is part of the identity. `cup` does not separate upstream
version from packaging revision. This keeps catalog, asset tag, archive name,
metadata, state and path comparison exact.

## Limits

Current in-memory package scanning limits are defined in `include/package.h`.
Archive limits are defined in `include/constants.h`: 262,144 entries, 16 GiB for
one regular file, 64 GiB total extracted bytes and 64 path segments. These are
conservative ceilings for complete compiler and debugger packages, not allocation
targets. Exceeding one produces a failing or incomplete result rather than silent
truncation. This repository does not contain published `cup-components` package
artifacts from which to claim a final measured maximum, so final tuning remains
coordinated with an inventory of those release packages.

## Implementation and verification

Package-module ownership is listed in [ARCHITECTURE](ARCHITECTURE.md). The
focused and process-level checks for these contracts are listed in
[TESTING](../development/TESTING.md).

## Related documents

- [ARCHITECTURE](ARCHITECTURE.md) — registry and package model;
- [SECURITY](SECURITY.md) — archive and checksum protections;
- [STATE](STATE.md) — installed identities and defaults;
- [COMMANDS](../user/COMMANDS.md) — package-facing CLI behavior.
