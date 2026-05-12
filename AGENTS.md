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

## Prose Style (Comments, Docstrings, Markdown)

Use [semantic line breaks](https://sembr.org/) for all prose written in this repository:
code comments, docstrings, commit messages, and Markdown files.

- Target a soft line length of about 120 characters.
  If a sentence (or short run of sentences) fits within that, leave it on one line.
  Only break a line when it would otherwise exceed the limit, or when a break aids readability.
- When you do break, break at semantic boundaries:
  after a sentence, or at a logical clause boundary.
  Do not hard-wrap mid-clause at an arbitrary column.
- These rules apply equally to single-line `///` doc comments grouped together
  and to multi-line `/** ... */` blocks.
- Prefer simple sentences separated by periods and commas.
  Avoid semicolons and em dashes.
  Split a long sentence into shorter ones on separate lines
  rather than joining clauses with `;` or `—`.

Example (Markdown):

```
All human beings are born free and equal in dignity and rights.
They are endowed with reason and conscience
and should act towards one another in a spirit of brotherhood.
```

Example (C++ doc comment):

```cpp
/// Processes a single file through the supplied plugin instance.
/// The plugin must already be prepared with the appropriate sample rate and block size;
/// the function resets the plugin internally before processing
/// and releases its resources afterwards.
```

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
