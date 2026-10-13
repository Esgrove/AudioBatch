---
description: Commenting conventions
alwaysApply: true
---

# Commenting conventions

## Comment formatting

Comments in code and files that a change touches, and the documentation related to them, may be reformatted.
Follow the semantic line break rules in `AGENTS.md`.
Leave comments in files the change does not touch as they are, unless asked to reformat them.

## Comments describe the current state, not change history

Comments (including Javadoc, block, line, and SQL/YAML comments) must
document what the code currently does and why.
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

Migration files are permanent:
their comments should describe what the migration does and why each step exists,
not the PR that introduced it,
but they should have the reasoning why the change was made.

## Data member docstrings

When one or more data members of a struct have a `///` docstring, give every data member one.
This applies to options and result structs in particular.
