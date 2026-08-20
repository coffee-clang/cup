# cup documentation

These documents describe cup as implemented in this repository. They are divided by
audience so the user guide stays separate from internal design and from the
build/release process.

The documentation follows the product and repository contracts present here. It
does not describe `cup-components` internals or proposed package formats.

## User guide

- [Installation](user/INSTALLATION.md) explains the public installers, the
  selected root, PATH handling, updates and uninstall.
- [Commands](user/COMMANDS.md) lists every public command, its arguments and the
  main failure cases.

These two pages are enough for someone who only wants to use cup.

## Design

- [Architecture](design/ARCHITECTURE.md) gives the overall structure and maps the
  C modules and script families to their responsibilities.
- [Packages](design/PACKAGES.md) explains the catalog, package identity, archive
  layout, metadata and cache.
- [State](design/STATE.md) describes the managed root, `state.txt`, preferences,
  wrappers and cup assets.
- [Transactions](design/TRANSACTIONS.md) explains `transaction.txt`, commit
  points, recovery and detached helpers.
- [Platforms](design/PLATFORMS.md) covers Linux, macOS and Windows differences.
- [Security](design/SECURITY.md) collects the trust, download, archive and path
  checks used by the project.

## Development

- [Build](development/BUILD.md) covers Make targets, dependencies, generated
  files, binary inspection and the documentation target.
- [Testing](development/TESTING.md) explains test levels, local commands,
  coverage, sanitizers and CI.
- [Releases](development/RELEASES.md) describes versioning, evidence, candidate
  assembly, native validation and publication.

## Project limits

The current project intentionally keeps a small scope:

- cup works in user space and never requires `sudo` or administrator rights;
- it installs complete prebuilt packages instead of building tools locally;
- it does not manage a global sysroot;
- it does not modify the system PATH;
- it uses one local root and one transaction file;
- it supports only the platforms and tools listed by the built-in registry;
- `stable` is the only symbolic release selector;
- package production remains in `cup-components`.
