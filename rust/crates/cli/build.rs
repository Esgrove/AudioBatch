//! Build script of the `audiobatch-rs` CLI.
//!
//! It records the version, build time, and git commit that the `--version` flag prints, see the `build-info` crate.

fn main() {
    build_info::emit();
}
