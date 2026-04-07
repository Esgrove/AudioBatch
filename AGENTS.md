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

- Windows release build script: `bash ./build.sh -b Release`
- Standard CMake debug build: `cmake -S . -B build` then `cmake --build build --config Debug`
- Windows release outputs are expected at:
  - `cmake-build-windows-release/AudioBatch_artefacts/Release/AudioBatchApp.exe`
  - `cmake-build-windows-release/AudioBatchCli_artefacts/Release/audiobatch.exe`

## Notes

- The main application code lives in `src/`.
- Formatting is configured by `.clang-format`.
- Shell helpers live in the repository root.
