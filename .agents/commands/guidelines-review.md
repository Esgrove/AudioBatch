---
description: Apply a lightweight, behavior-preserving cleanup against the project's existing organization and style rules.
argument-hint: "[all|audiobatch|cpp|rust|imagecontrol] [changes|path] [check-only]"
---

# Review and apply project guidelines

Check the selected code against the repository's existing guidelines and make small, justified cleanup edits.
This is lighter than a full correctness review or an all-checks run.
Arguments: $ARGUMENTS
Treat arguments or instructions appended to this command as scope, file selection, and mode.
Default to the whole repository, applying safe fixes. `check-only` reports without editing.

## Scope

- `all`, or no scope: all first-party code and configuration in the repository.
- `audiobatch`: `cpp/` and `rust/` plus their shared fixtures, test plugin, assets, tests, and tooling.
  Exclude ImageControl-specific code.
- `cpp`: the C++ application and shared support relevant to it.
- `rust`: the Rust application and shared support relevant to it.
- `imagecontrol`, `image-control`, `image-control-plugin`, or `imagecontrol plugin`: `image-control-plugin/`
  and the shared C++ configuration, assets, helpers, and tests relevant to it.

An explicit file or directory narrows that scope.
`changes` limits cleanup to staged, unstaged, and non-ignored untracked changes relative to `HEAD`,
plus nearby declarations that must move together for a coherent cleanup.
Do not infer `changes` from a dirty working tree. Without a file selection, inspect the selected project as a whole.
A clean `changes` selection is a no-op, not permission for a repository-wide sweep.
Exclude generated files, build directories, fetched dependencies, JUCE internals, binary fixtures, and golden snapshots.
Do not edit another project just because a shared formatting script checks it.

## Preparation

Read root `AGENTS.md` and record `git status --short` before edits.
For selected application code, read its `AGENTS.md` and `ARCHITECTURE.md`.
For Rust, also read the C++ prose, naming, and documentation rules that Rust explicitly inherits.
For C++ and ImageControl, read `.agents/rules/code-organization.md` and `.agents/rules/comments.md`.
For ImageControl, read its `README.md` and `ARCHITECTURE.md`.
Apply its own namespace and branding, not AudioBatch application-specific names or parity requirements.
For Rust GUI architecture, ownership, API, or test decisions, load the GPUI Kit coding skill when available.
Do not redesign screens. If a requested cleanup changes presentation or interaction,
load the design guides too and treat that work as a separate, explicitly justified change.

State the scope and whether fixes or check-only mode are selected.
Use written rules and existing domain boundaries as authority. Do not invent new conventions or edit rules to fit code.
Preserve unrelated work. Do not stage, commit, clean build directories, or run destructive Git commands.

## Lightweight checklist

### Organization and boundaries

- Put code in its owning domain and keep reusable helpers in the existing shared topic files.
  Preserve the one-way C++ dependency layering and Rust core/playback separation from GUI and plugin implementations.
- Keep substantial reusable types in cohesive files without mechanically demanding one type per file.
  Around 1000 lines is a signal to inspect concerns, not permission for arbitrary splits or tiny new files.
  Defer large module moves or API redesigns and report them with a concrete reason.
- Follow C++ access-section, special-member, method, override, and field grouping rules.
  Keep dependent declarations together, and let clang-format order includes.
- Follow Rust item and `impl` ordering, put imports at file scope with matching platform gates,
  keep visibility narrow as appropriate for library versus binary crates, and avoid nested test modules.
- Keep headers self-contained and avoid accidental transitive includes, namespace aliases,
  static-only classes, and helpers misplaced in application or component classes.

### Style, naming, and documentation

- Prefer descriptive full names, established domain terms, and the existing local naming conventions.
  Check named constants, C++ digit separators, brace initialization where required,
  and compile-time evaluation where the value and contract are genuinely constant.
- Keep ownership explicit with values, references, RAII, and the project's documented observer types.
  Do not mechanically convert API-boundary raw pointers or rewrite ownership during a cosmetic pass.
- Follow Rust error-type and lint-attribute conventions. Avoid plain `unwrap` and broad lint suppression.
  Keep unsafe scope narrow and retain accurate `SAFETY` explanations.
- Check file-level documentation, function docstrings, definition-versus-declaration placement,
  consistent enum and data-member documentation, and comments describing current behavior rather than change history.
- Use semantic line breaks and simple prose, with no trailing code comments.
  Reformat comments only in code touched by the cleanup and directly related documentation.
- Use the prescribed formatting and logging helpers and avoid redundant formatting wrappers.
  Keep literal user-facing text, CLI output, and public messages unchanged in a style-only cleanup.
- Check shared package configuration and build source lists when reorganizing code.
  Keep dependency versions in authoritative workspace configuration, not duplicated in prose.

## Apply safe fixes

Prefer a small coherent batch of mechanical or clearly behavior-preserving edits.
For a broad scope, inventory every selected area first, then work through bounded batches.
Do not stop after the first file and claim the whole repository was checked.
If the full cleanup cannot reasonably fit one pass, report inspected areas and deferred batches explicitly.

Declaration sorting is subordinate to correctness and lifetime dependencies.
Before moving fields, inspect construction and reverse destruction order, initializer lists,
aggregate callers, structured bindings, defaulted comparisons, and layout contracts.
Keep dependency-sensitive order and explain it briefly rather than adding manual teardown just to sort fields.
Update direct callers and CMake source lists only when a safe scoped reorganization requires it.

Preserve DSP operation order, precision, rounding, allocation behavior, cache formats, threading,
state ownership, and all observable application behavior.
Do not introduce floating-point reassociation, FMA, table replacements, or performance claims as cosmetic cleanup.
Do not refresh fixture checksums, input identities, snapshots, tolerances, or expected output.

Review every formatter or lint autofix, especially pointer qualification and MSVC iterator assumptions.
Use scoped formatting so unrelated files and pre-existing changes are not rewritten.
In check-only mode, do not run autofixing commands.
If a correctness bug or a risky refactor is discovered, report it separately instead of silently expanding the task.

## Proportionate verification

Run commands from their project's directory and use the current project instructions for exact options.
After edits, the required project validation still applies. A lightweight review is not an exemption from it.

- Format every changed C++ source or header with `clang-format -i --style=file`.
  Run CMake formatting for changed CMake files, then the applicable C++ or ImageControl lint script.
  Use `--check --changed` for targeted feedback where suitable,
  but remember that header-driven lint only adds the neighboring source with the same stem.
  Build all affected consumers, including both GUI and CLI for shared C++ code,
  and run relevant tests after declaration, header, or organization changes.
- For Rust code edits, follow `rust/AGENTS.md`'s clippy, format, and test-tier requirements.
  In check-only mode use `./lint.sh --check` and `./format.sh --check` instead of autofixes.
  Formatting or comment-only edits need not trigger unrelated sanitizer, fuzz, coverage, or benchmark campaigns.
- Check changed Python with `uv run --frozen ruff format --check <paths>`
  and `uv run --frozen ruff check <paths>` from the repository root.
  Format changed shell files with shfmt and prose with slb when available.
- Escalate to the existing sanitizers or Miri when a change affects ownership, threading,
  unsafe code, or buffer handling, following the project instructions.
  Such a change is no longer merely cosmetic.

Do not launch every expensive tier from `.agents/commands/all-checks.md` by default.
Do not install applications or plugins, alter production storage, use system trash, or modify committed test inputs.
Report unavailable tools, tests not run, and platform limits honestly.

## Completion

Review the final diff for accidental behavior changes and unrelated formatting churn.
Summarize the scope actually inspected, guideline categories corrected, checks run,
and concrete remaining violations or risky changes deferred for a full review.
In check-only mode, provide concise file-and-line findings tied to the written rule.
Do not claim a full correctness or security audit from this lighter pass.
