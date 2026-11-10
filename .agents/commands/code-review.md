---
description: Review project correctness, safety, architecture, and tests, optionally limited to a subproject.
argument-hint: "[all|audiobatch|cpp|rust|imagecontrol] [changes|base=<ref>] [focus]"
---

# Review the project

Perform an evidence-based code review. Do not modify code unless the user explicitly requests fixes.
Arguments: $ARGUMENTS
Treat arguments or instructions appended to this command as scope, review target, and focus.

## Select scope and target

Default to reviewing the current first-party code across the whole repository, not just the latest diff.
Do not infer a narrower scope from an open file, current directory, or recent conversation about one implementation.
Accept these scope selectors, case-insensitively:

- `all`, or no selector: both applications, ImageControl, and all first-party shared support and configuration.
- `audiobatch`: both applications in `cpp/` and `rust/`, including shared fixtures, test plugin, assets,
  root scripts, executable contracts, and relevant CI. Exclude ImageControl-specific code.
- `cpp`: the C++ application and shared code, configuration, tests, and workflows that affect it.
- `rust`: the Rust application and shared code, configuration, tests, and workflows that affect it.
- `imagecontrol`, `image-control`, `image-control-plugin`, or `imagecontrol plugin`: `image-control-plugin/`
  and the JUCE integration, shared C++ configuration, assets, helpers, and workflows it depends on.

Shared support is in scope only where it affects the selected project.
Read dependencies and counterpart implementations as needed to establish behavior,
but do not turn a limited review into an unrelated audit of the other projects.
Exclude generated files, build products, fetched dependencies, and vendored JUCE internals from routine review.
Review first-party dependency configuration and submodule changes when relevant.

An explicit file or directory further narrows the selected scope.
`changes` selects staged, unstaged, and non-ignored untracked changes relative to `HEAD`.
`base=<ref>` selects branch changes from the merge base of that ref and `HEAD`, plus local changes.
Resolve the named ref without changing branches or fetching unless permitted.
If an explicitly requested ref is missing, ask rather than guessing a different comparison.
For either diff mode, read the full affected functions and callers and report issues introduced by the selected changes.
A clean diff is an empty change review, not permission to silently switch to a whole-project audit.
Do not interpolate arbitrary command arguments into shell commands.

## Load project guidance

1. Read root `AGENTS.md` and record `git status --short`.
2. For `cpp`, read `cpp/AGENTS.md`, `cpp/ARCHITECTURE.md`, and applicable `.agents/rules/` files.
3. For `rust`, read `rust/AGENTS.md` and `rust/ARCHITECTURE.md`.
   Read the C++ instructions where Rust explicitly inherits prose, naming, and documentation conventions.
4. For ImageControl, read `image-control-plugin/README.md`, `image-control-plugin/ARCHITECTURE.md`,
   and applicable `.agents/rules/` files. Do not impose AudioBatch branding or application parity on this plugin.
5. Load the GPUI Kit coding skill before reviewing Rust GUI architecture, ownership, API, or testing decisions.
   Load its design guides too when assessing layout, interaction, or interface copy, if those skills are available.

Use repository instructions as the authority, not personal style preferences.
A focused style-and-organization cleanup belongs in `.agents/commands/guidelines-review.md`.
Do not invoke specialized Bugbot or security-review agents unless the user explicitly requests that kind of review.

## Review workflow

1. State the interpreted scope, target, and any requested focus before investigating.
   Map the selected domains and their tests so a default whole-project review does not become a diff-only review.
2. Inspect implementation, call sites, ownership, invariants, error paths, and the tests that establish the contract.
   Prioritize risky boundaries over mechanically reading generated or repetitive code.
3. Where useful, split independent domains among read-only reviewers with explicit scope and shared contracts.
   Reconcile findings centrally. Do not duplicate reports or let reviewers edit the working tree.
4. Validate each candidate finding against the actual API, platform behavior, and existing tests.
   Look for a concrete triggering input or sequence, then trace it to an observable failure.
   Run a focused existing test or a safe scratch reproduction where practical.
   Distinguish confirmed defects from hypotheses. Drop speculative findings instead of padding the report.
5. Run only checks needed to substantiate findings, using non-autofixing modes and isolated test runners.
   Read `.agents/commands/all-checks.md` for verification commands, but do not run the complete suite by default.
   Missing tools or unavailable platform coverage must be reported as limits, not converted into code defects.

### Project-specific checklist

- Correctness and preservation of meaningful C++/Rust results for the same supported input and operation.
  Validate parsed metadata, source values versus presentation fallbacks, names, positions, colors, and numeric results.
  C++ is a reference implementation, not an oracle over independently verified file-format expectations.
- DSP precision and operation order, bit-exact true peak, documented loudness and normalization tolerances,
  channel handling, quantization, latency compensation, tail draining, cancellation, and no-op decisions.
  Do not propose relaxed tolerances or implementation-specific expected values to hide divergence.
- Cache schema and measurement contract coordination, provenance, source identity changes,
  stale asynchronous results, persisted explicit gain clears, and optional-artifact misses versus measurement misses.
- File replacement and rollback, metadata and artwork preservation, path handling, temporary outputs,
  deletion versus trash behavior, and invalid `AUDIOBATCH_DATA_ROOT` never falling back to production storage.
- VST3 and Audio Unit lifecycle constraints, UI-thread affinity, COM and RAII lifetimes,
  editor teardown, scanner isolation, and cancellation while a worker waits on the UI thread.
- Audio callbacks that allocate, lock, block, log, or destroy UI-bound or heavyweight values.
  Keep genuine clean real-time contracts separate from documented dependency blockers.
- Ownership and reverse destruction order, borrowed data lifetimes, unsafe boundaries and safety comments,
  worker admission, synchronization, shutdown, and callbacks after cancellation or view destruction.
- GUI state ownership, stable shared control ids, accessible roles and labels,
  persisted settings, parity of shared workflows, and production-view event routing in integration tests.
- macOS and Windows compatibility, Debug and Release behavior, MSVC checked iterators,
  compiler and standard-library differences, feature-gated code, and platform-specific dependencies.
  A native check or cross-compile lint is not proof of a successful build and run on another platform.
- ImageControl's own DSP and state contracts, including neutral transparency, crossover ordering,
  parameter automation and smoothing, latency and tail reporting, presets, undo/A-B state, and analyzer handoff.
  Apply these only when ImageControl is selected.
- Test independence and failure visibility, immutable recording inputs, fixture provenance,
  shared expectation coverage, silently skipped plugin or LFS tests, CI path filters, and packaging contracts.
- Architecture and guideline violations when they create a concrete maintenance, safety, or dependency problem.
  Keep cosmetic preferences and broad speculative refactors out of the defect list.

## Report

Put actionable findings first, ordered by severity.
For each finding, give a concise title, repository-relative file and precise line range,
the triggering conditions, the observable consequence, and the evidence or reproduction.
Use priorities consistently: P0 for a release-blocking critical failure, P1 for a high-impact defect,
P2 for a normal defect, and P3 for a low-impact actionable issue.
Do not assign a high priority without explaining the impact and reach.

Then summarize the areas inspected, checks actually run, and remaining coverage gaps or open questions.
If there are no confirmed findings, say so explicitly and still state the review limits.
Do not imply the repository is bug-free, both platforms were tested, or the whole scope was reviewed
when only a subset was inspected. Verify that the review left existing work unchanged.
