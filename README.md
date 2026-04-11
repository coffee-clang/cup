# cup

`cup` is a prototype toolchain manager inspired by tools like `rustup`.

It allows managing installable components (such as compilers) and selecting default versions for them.

---

## Status

This is an early prototype.

- No real installation is performed yet
- Components are simulated via filesystem structure
- Focus is on CLI behavior and internal architecture

---

## Features

- Install component entries (`<tool>@<release>`)
- Remove installed entries
- List installed components
- Set a default entry per component
- Store state locally under `~/.cup`

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

## Storage

```
All data is stored locally in:

`~/.cup`

Structure:

~/.cup/
├── state.txt
├── components/
│   └── compiler/
│       └── gcc/
│           └── linux/
│               └── stable/
└── tmp/
```

### State File

The file `~/.cup/state.txt` contains:

#### Installed entries

`installed.<component>=<tool>@<release>`

#### Defaults (one per component)

`default.<component>=<tool>@<release>`

---

## Build


Build through `make`
<br>
Clean through:
- `make clean` removes the binary
- `make dev-clean` removes everything, binary and `~/.cup`, with a console cleanup too