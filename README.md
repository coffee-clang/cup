# cup

`cup` is a prototype toolchain manager inspired by tools like `rustup`.

It provides a simple CLI to install, manage, and select default toolchain components (currently focused on compilers)

---

## Status

This is an early prototype.

- Installations are simulated via filesystem operations
- No real downloads are performed yet
- Focus is on architecture, CLI design, and robustness

Despit being a prototype, `cup` already implements:

- staged installation (temporary → validated → committed)
- cleanup of partial installs
- basic error handling and roolback

---

## Features

- Install component entries (`<tool>@<release>`)
- Remove installed entries
- List installed components
- Set a default entry per component
- Query current default
- Local state management under `~/.cup`
- Safe installation flow with validation and roolback
- Automatic cleanup of temporary installations

---

## Usage

### Install

```bash
./cup install <component> <tool>@<release>
```

Example:
```bash
./cup install compiler gcc@stable
./cup install compiler clang@nightly
```

### Remove

```bash
./cup remove <component> <tool>@<release>
```

Example:
```bash
./cup remove compiler clang@nightly
```

### List installed entries

```bash
./cup list
```

### Set default

```bash
./cup default <component> <tool>@<release>
```

Example:
```bash
./cup default compiler clang@nightly
```

### Show current default

```bash
./cup current <component>
```
Example:
```bash
./cup current compiler
```

---

## Storage Layout

All data is stored locally in:

`~/.cup`

Structure:

```bash
~/.cup/
├── state.txt
├── components/
│   └── <component>/
│       └── <tool>/
│           └── <platform>/
│               └── <release>/
└── tmp/
```
Example:
```bash
~/.cup/components/compiler/gcc/linux/stable
```

---

## State File

The file `~/.cup/state.txt` tracks installed entries and defaults.

### Installed entries

```bash
installed.<component>=<tool>@<release>
```
Example:
```bash
installed.compiler=gcc@stable
installed.compiler=clang@nightly
```

### Defaults (one per component)

```bash
default.<component>=<tool>@<release>
```
Example:
```bash
default.compiler=gcc@stable
```

---

## Build

Build the project using:
```bash
make
```

## Cleaning

```bash
make clean
```
Removes the binary.

```bash
make dev-clean
```
Removes:
- the binary 
- the entire `~/.cup` directory (development only)