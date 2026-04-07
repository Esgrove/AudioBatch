# AGENTS.md

This repository contains a JUCE-based C++23 application with two deliverables:

- GUI target: `AudioBatch` producing `AudioBatchApp`
- CLI target: `AudioBatchCli` producing `audiobatch`

## Working Rules

- Keep changes focused and minimal.
- Do not edit vendored code under `JUCE/` unless the task explicitly requires it.
- Prefer fixing warnings and build issues in project code or project configuration before considering third-party changes.
- Preserve the existing source layout under `src/` and the current naming used in `CMakeLists.txt`.

## Formatting

- After changing any C++ source or header file, run `clang-format -i --style=file` on each changed file before finishing.
- For repo-wide or multi-file formatting, use `./format.sh`.

## Build And Verification

- Do not use build.sh, it is only for making final release builds
- When modifying code, use the CMake debug presets for validation so terminal builds reuse the same build directory as the editor or IDE.
- On Windows, prefer `cmake --preset windows-debug` then `cmake --build --preset windows-debug`.
- If you need Ninja and `compile_commands.json` on Windows, use `cmake --preset windows-ninja-debug` then `cmake --build --preset windows-ninja-debug`.
- On macOS, use `cmake --preset macos-debug` then `cmake --build --preset macos-debug`.

## Notes

- The main application code lives in `src/`.
- Formatting is configured by `.clang-format`.
- Shell helpers live in the repository root.
