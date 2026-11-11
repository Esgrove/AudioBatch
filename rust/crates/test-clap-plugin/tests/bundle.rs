//! Native fixture bundle contracts, never loading installed third-party plugins.

use audiobatch_test_clap_plugin::{PLUGIN_ID, create_bundle};
use clack_host::entry::PluginEntry;
use tempfile::TempDir;

#[test]
fn bundle_names_reject_paths_and_xml_metacharacters() {
    let directory = TempDir::new().expect("isolated bundle directory");
    for name in ["", "../other", "nested/name", "name.clap", "<name>", "name&other"] {
        assert_eq!(
            create_bundle(directory.path(), name).expect_err("invalid name").kind(),
            std::io::ErrorKind::InvalidInput
        );
    }
}

#[test]
#[ignore = "requires cargo build -p audiobatch-test-clap-plugin --example clap_module"]
#[cfg_attr(miri, ignore = "loads a native module")]
#[expect(
    unsafe_code,
    reason = "loads only the CLAP cdylib built from this crate's safe Clack implementation"
)]
fn native_bundle_is_loadable_and_has_the_in_process_identity() {
    let directory = TempDir::new().expect("isolated bundle directory");
    let bundle = create_bundle(directory.path(), "FixtureContract").expect("bundle");
    if cfg!(target_os = "macos") {
        assert!(bundle.is_dir());
        assert!(bundle.join("Contents/MacOS/FixtureContract").is_file());
        assert!(bundle.join("Contents/Info.plist").is_file());
    } else if cfg!(windows) {
        assert!(bundle.is_file());
    }
    // SAFETY: This is exclusively the cdylib compiled from this crate's safe Clack implementation.
    // The temporary directory and its library remain alive until after the entry is dropped.
    let entry = unsafe { PluginEntry::load(&bundle) }.expect("native entry");
    let factory = entry.get_plugin_factory().expect("factory");
    assert_eq!(factory.plugin_count(), 1);
    assert_eq!(factory.plugin_descriptor(0).expect("effect").id(), Some(PLUGIN_ID));
}
