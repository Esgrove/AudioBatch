---
description: Update gpui-kit, its gpui-pre pins, and all other Rust crates, then adapt the code to the release notes.
argument-hint: "[version] [check-only]"
---

# Update gpui-kit and the Rust dependencies

Move the Rust workspace to a new gpui-kit release and refresh every other crate.
Then read the release notes and adapt the code that they affect.
Arguments: $ARGUMENTS
Treat arguments or instructions appended to this command as the target version and options.
If the arguments line still shows a literal `$ARGUMENTS` placeholder, use the text the user wrote after the command.
A version such as `0.11.0` or `v0.11.0` selects that release. Without one, use the latest release.
`check-only` reports the available update, the breaking changes, and the code they touch, without editing anything.

Only the Rust application uses gpui-kit, through `crates/gui`.
The C++ application and ImageControl are not affected, so leave them alone.
Do not add Linux code paths or features, even where the release notes describe Linux changes.

## Find the release

1. Read the current version from `gpui-kit` in the root `Cargo.toml`.
2. List the releases with `gh release list -R longbridge/gpui-kit` and pick the target.
   Stop and report if the target is not newer than the current version.
3. Read the full notes of every release after the current one up to the target with `gh release view <tag> -R longbridge/gpui-kit`,
   not only the latest, because breaking changes accumulate across skipped releases.
4. Collect the breaking changes, deprecations, renamed or removed APIs, new default features,
   and the `gpui-pre` and `gpui-fast` versions that each release moves to.

## Update the pins and crates

1. Set `gpui-kit` in the root `[workspace.dependencies]` to the target as a full `major.minor.patch` version.
2. Set the exact `gpui-pre` and `gpui-pre-platform` pins to the `gpui-pre` version that the target gpui-kit depends on.
   The release notes name it, and the `Cargo.toml` of the published crate confirms it.
   Both pins keep their `=` and their `test-support` feature, and always carry the same version.
   Keep every other exact pin, such as `vst3` and the Clack crates, as it is.
3. Run `cargo upgrade -i` from the repository root.
   Here `-i` is `--incompatible`, which also takes semver-breaking releases.
   It skips exact pins on its own.
4. Run `cargo update` to refresh `Cargo.lock`.
5. If `rust/fuzz/` shares updated crates, refresh its lockfile with `cargo update --manifest-path rust/fuzz/Cargo.toml`.
   `bump-version.sh` owns the application package entries in that lockfile, so do not edit those.
6. Summarize the `cargo upgrade -i` and `cargo update` results for the user:
   list every crate that changed version, and flag any major bump for review.

## Adapt the code

1. Build everything that the update touches with `cargo check --workspace --all-targets --all-features`.
   Fix each compile error at its cause, following the migration advice in the release notes.
2. Search `rust/crates/gui` for every API that the notes list as changed, removed, or renamed,
   because not every break is a compile error.
   Behavior changes in input handling, focus, popovers, tables, scrollbars, text rendering,
   and accessibility can alter how the GUI or its tests behave.
3. Look for improvements the notes make possible, and apply the small ones that are clearly better:
   a new API that replaces a local workaround, a bug fix that makes a workaround unnecessary,
   or a new component option that matches an existing custom implementation.
   List larger or debatable improvements for the user instead of doing them.
   Respect behavioral parity with the C++ application: do not change user-visible behavior in only the Rust GUI
   without asking, and report any such change the update forces.
4. Keep `rust/AGENTS.md`, `rust/ARCHITECTURE.md`, and the READMEs accurate if the update changes what they state,
   such as the pin names, a gpui test helper, or a component the GUI relies on.
   Write them as current state, not as a changelog.

## Verify

1. Build the dev plugins that the plugin-hosting GUI tests load:
   `test-plugin/build.sh`, and `cargo build --examples -p audiobatch-test-plugin -p audiobatch-test-clap-plugin`.
   Without them the plugin tests fail for reasons unrelated to the update.
2. Run `cargo test -p audiobatch-gui`, then the tests of the other crates that changed dependencies.
   Run `rust/test.sh` if it covers more.
3. Run `./format.sh rust` and `./lint.sh rust --check`, and fix what they report.
4. Build with the `snapshot` feature if the `gpui-pre` pins changed, since only that feature uses them directly.
5. Do not widen tolerances or edit expectations to make a test pass.
   A failing test after the update is either a real regression or a deliberate upstream change:
   find out which, and report it.

## Report

Do not commit unless the user asks.
When committing, use a descriptive prefix such as `rust:` and never `chore:` or `feat:`.
Summarize:

- the old and new gpui-kit and `gpui-pre` versions,
- the crates that changed, with major bumps called out,
- each code change and the release note that required it,
- improvements applied and improvements left for the user to decide,
- the verification that ran and its results, and anything that could not run.
