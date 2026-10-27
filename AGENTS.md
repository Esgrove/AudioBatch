# AGENTS.md

AudioBatch analyzes and batch processes audio files.
This repository holds two versions of it side by side, each with its own agent instructions:

- `cpp/`: the C++23 and JUCE application, the reference implementation.
  Read [`cpp/AGENTS.md`](cpp/AGENTS.md) before changing anything under `cpp/`.
- `rust/`: the Rust port. Read [`rust/AGENTS.md`](rust/AGENTS.md) before changing anything under `rust/`.

## Shared Files

- `assets/` holds the icons and fonts that both applications bundle.
- `common.sh` holds the shell helpers shared by the scripts of both projects,
  such as platform detection and colored output.
  `cpp/common.sh` and `rust/common.sh` source it and add their project specific parts.
- `build.sh`, `install.sh`, and `format.sh` run the scripts of the same name in both projects,
  or in the one named by their first argument, `cpp` or `rust`, through `run_project_scripts` in `common.sh`.
  `format.sh` also formats the root shell scripts and the prose outside the projects,
  and runs ruff on the Python scripts.
- `fixtures/` holds the committed audio fixtures that both test suites read in place.
  `fixtures/generate.sh` regenerates them with ffmpeg,
  and calls `fixtures/serato_overview.py` to add the rainbow Serato Overview tag that ffmpeg cannot write.
  When a fixture changes, update the expected values in the C++ tests and in the Rust `test-fixtures` crate.
- `test-plugin/` holds `AudioBatch Test MS`, the JUCE VST3 effect that both test suites load,
  with a gain, a mid/side width, and a latency parameter that the hosts must compensate.
  The C++ build adds it with its tests,
  and `test-plugin/build.sh` builds it into `test-plugin/build` for the Rust tests.
  It builds with the JUCE submodule in `cpp/JUCE`, and `cpp/format.sh` formats it.
  `uv run fixtures/plugin_expected.py` computes its expected outputs with numpy into `fixtures/plugin_expected/`.
  When its processing changes, update the script, regenerate the files,
  and update the expectation lists in `cpp/tests/PluginPresetTests.cpp` and the Rust `test-fixtures` crate.
- `fixtures/normalization/` holds a larger collection for comparing the normalized output of both applications.
  It has music-like audio in every supported format, sample rate, and bit depth,
  from a fifth of a second to 30 seconds long, with a wide range of tags and edge cases.
  The audio is stored with Git LFS, so run `git lfs install` once per clone.
  `uv run fixtures/normalization/generate.py` regenerates it with ffmpeg and mutagen.
  The unit tests do not read it. Only `rust/scripts/parity.sh` and the parity workflow do.
- `.agents/rules/` holds rules for the C++ sources, and `.claude/rules` is a symlink to it.
- `pyproject.toml` configures ruff for the Python scripts in `fixtures/` and `rust/scripts/`,
  and pins it in `uv.lock`.
  Run Python only through [uv](https://docs.astral.sh/uv/), never a bare `python3`.
  Each script declares its Python version and packages in an inline `# /// script` block
  and runs in its own environment with `uv run --script`, so add a script's packages there, not to `pyproject.toml`.
  Check the scripts with `uv run ruff format` and `uv run ruff check --fix` from the repository root.
- `.pre-commit-config.yaml` runs clang-format and cmake-format on `cpp/` and `test-plugin/`, cargo fmt on `rust/`,
  ruff on the Python scripts, and basic file checks through [prek](https://prek.j178.dev).
- `.github/workflows/cpp.yml` builds and checks the C++ project, and `rust.yml` the Rust port.
  Each runs only when its own directory, the fixtures, the test plugin, or the assets change.
  `rust.yml` checks out only the JUCE submodule, to build the test plugin.
  `parity.yml` compares the levels both CLIs report and the files they write when normalizing,
  and is the only workflow that downloads the LFS files.

## Working Rules

- Run each project's build, format, lint, and test commands from its own directory.
- Stage with explicit paths, such as `git add cpp` or `git add rust`.
- Commit messages and all prose follow the semantic line break style described in both project files.
- **NEVER run destructive git commands**, such as `git checkout -- <file>`, `git restore`, `git reset --hard`,
  `git clean`, or `git stash drop`.
  Ask the user to do it manually if something needs undoing.
- `CLAUDE.md` is a symlink to this file. Only edit `AGENTS.md` directly.
