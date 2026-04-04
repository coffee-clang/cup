# cup
A modern way to manage a C toolchain.

## Description
Initial prototype of cup.

## Features
- list
- install <name>
- remove <name>
- default <name>
- current

## Behavior
- local state management
- no real installation is performed
- simplified toolchain model (compiler only)

## Structure
- command handling (CLI)
- local state management
- toolchain logic

## Limitations
- no download support
- no full component management
- no multi-architecture support

## Build
make

## Usage
./cup <command>