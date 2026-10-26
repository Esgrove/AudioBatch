# AGENTS.md

AudioBatch analyzes and batch processes audio files.
This repository holds two versions of it side by side, each with its own agent instructions:

- `cpp/`: the C++23 and JUCE application, the reference implementation.
  Read [`cpp/AGENTS.md`](cpp/AGENTS.md) before changing anything under `cpp/`.
- `rust/`: the Rust port. Read [`rust/AGENTS.md`](rust/AGENTS.md) before changing anything under `rust/`.

## Shared Files

- `assets/` holds the icons and fonts that both applications bundle.
- `fixtures/` holds the committed audio fixtures that both test suites read in place.
  `fixtures/generate.sh` regenerates them with ffmpeg.
  When a fixture changes, update the expected values in the C++ tests and in the Rust `test-fixtures` crate.
- `.agents/rules/` holds rules for the C++ sources, and `.claude/rules` is a symlink to it.
- `.pre-commit-config.yaml` runs clang-format and cmake-format on `cpp/`, cargo fmt on `rust/`,
  and basic file checks through [prek](https://prek.j178.dev).
- `.github/workflows/cpp.yml` builds and checks the C++ project, and `rust.yml` the Rust port.
  Each runs only when its own directory, the fixtures, or the assets change.

## Working Rules

- Run each project's build, format, lint, and test commands from its own directory.
- Stage with explicit paths, such as `git add cpp` or `git add rust`.
- Commit messages and all prose follow the semantic line break style described in both project files.
- **NEVER run destructive git commands**, such as `git checkout -- <file>`, `git restore`, `git reset --hard`,
  `git clean`, or `git stash drop`.
  Ask the user to do it manually if something needs undoing.
- `CLAUDE.md` is a symlink to this file. Only edit `AGENTS.md` directly.
