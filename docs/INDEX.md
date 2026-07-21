# Documentation

`cup` is a user-space toolchain manager for prebuilt C development tools. This
documentation describes the current implementation, its public behavior and the
design constraints that keep installation and recovery deterministic.

The rendered documentation is available at
[coffee-clang.github.io/cup](https://coffee-clang.github.io/cup/). The root
`README.md` is intentionally a short project introduction; this index is the
starting point for complete user, design and development documentation.

The documentation is organized by stable responsibility rather than by source
file or development chronology. Each subject has one primary document and links
to related contracts where a boundary is crossed.

## User guide

- [INSTALLATION](user/INSTALLATION.md) explains the CUP asset installers,
  reinstallation, canonical paths and uninstall behavior.
- [COMMANDS](user/COMMANDS.md) is the complete CLI reference, including state changes
  and relevant failure conditions.

## Design

- [ARCHITECTURE](design/ARCHITECTURE.md) defines the domain model, module boundaries and
  the separation between runtime C code and operational scripts.
- [PLATFORMS](design/PLATFORMS.md) describes platform identifiers and the differences
  between POSIX and Windows implementations.
- [PACKAGES](design/PACKAGES.md) defines `packages.cfg`, the verified
  `install.cfg` policy, local installation preferences, package identities,
  `info.txt`, cache names and the contract with `cup-components`.
- [STATE](design/STATE.md) defines `~/.cup`, `state.txt`, defaults, locks and managed
  package commands.
- [TRANSACTIONS](design/TRANSACTIONS.md) defines journals, commit points, rollback,
  recovery, `doctor`, `repair` and uninstall.
- [SECURITY](design/SECURITY.md) collects the HTTPS, checksum, archive, path and
  CUP assets integrity rules.

## Development

- [BUILD](development/BUILD.md) describes build modes, dependencies, static CUP assets and
  generated sources.
- [TESTING](development/TESTING.md) defines test ownership, fixtures, coverage, sanitizers
  and repository structure.
- [RELEASES](development/RELEASES.md) describes version derivation, candidate artifacts,
  release gates and resumable publication.

## Recommended reading order

For users:

```text
INSTALLATION -> COMMANDS -> PLATFORMS
```

For implementation and operational review:

```text
ARCHITECTURE -> PLATFORMS -> PACKAGES -> STATE -> TRANSACTIONS -> SECURITY
             -> BUILD -> TESTING -> RELEASES
```

## Project boundary

This repository does not build GCC, Clang, GDB, LLDB, LLD, Valgrind or the
other component packages during `cup install`. Those archives are produced and
published by the separate
[`cup-components`](https://github.com/coffee-clang/cup-components) project.

The boundary is deliberate:

```text
cup-components
  builds, tests and packages complete tool distributions

cup
  resolves, downloads, verifies, installs and manages those packages
```

[PACKAGES](design/PACKAGES.md) documents the shared artifact contract. Component build
recipes, Docker images, MSYS2/Homebrew setup and tool-specific packaging belong
to the `cup-components` repository and are not duplicated here.

## Documentation authority

The current source, tests and generated release contracts are authoritative.
Historical documentation was used to recover design rationale and discarded
alternatives, but superseded command names, layouts and release models are not
retained as current behavior.

Each document explains the implemented contract and the reason for decisions
that are not obvious from the interface. It does not reproduce a chronological
log of development discussions.
