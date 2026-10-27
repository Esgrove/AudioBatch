---
description: Commenting conventions for the C++ sources, the test plugin, and the documentation next to them
globs: cpp/**,test-plugin/**
paths:
  - "cpp/**"
  - "test-plugin/**"
alwaysApply: false
---

# Commenting conventions

## Comment formatting

Comments in code and files that a change touches, and the documentation related to them, may be reformatted.
Follow the semantic line break rules in `cpp/AGENTS.md`.
The Rust port has the same rules in `rust/AGENTS.md`.
Leave comments in files the change does not touch as they are, unless asked to reformat them.

## Comments describe the current state, not change history

Comments, including doc, block, and line comments and those in CMake and YAML files,
must document what the code currently does and why.
Do not narrate the change that introduced the code,
the previous state of the PR, or what something "used to" do.

Avoid phrases like:

- "Before this PR / change / refactor..."
- "Previously..." / "Formerly..." / "Earlier..."
- "Used to..." / "No longer..." / "Now both/have/holds..."
- "As of the X split / migration / refactor..."
- "This PR / commit / branch..."

A reader of the merged file should be able to understand the comment
without any knowledge of the diff that produced it.

Code that migrates saved settings or caches stays for good:
its comments describe what the migration does and why each step exists, not the change that introduced it.

## Data member docstrings

When one or more data members of a struct have a `///` docstring, give every data member one.
This applies to options and result structs in particular.
