//! Plugin rescans with the `audiobatch-rs` executable as the scan worker,
//! which checks that the executable serves a scan when started with the worker flag.

use std::ffi::OsString;
use std::path::PathBuf;

use audiobatch_cli::args::{PluginGrouping, PluginListing};
use audiobatch_cli::plugin_list::{PluginListContext, run_plugin_list};
use audiobatch_cli::run::EXIT_SUCCESS;
use audiobatch_plugins::{KnownPlugins, PluginDataPaths, WORKER_FLAG, WorkerCommand};
use audiobatch_test_plugin::{Kind, create_bundle};

#[test]
fn a_rescan_finds_the_test_plugins_through_the_cli_worker() {
    let directory = tempfile::tempdir().expect("temporary folder");
    let folder = directory.path().join("plugins");
    std::fs::create_dir_all(&folder).expect("plugin folder");
    create_bundle(&folder, "Effects").expect("test plugin bundle");
    let data_paths = PluginDataPaths::in_folder(&directory.path().join("data"));
    let known = KnownPlugins {
        custom_folders: vec![folder],
        ..KnownPlugins::default()
    };
    known.save_to(&data_paths.known_plugins).expect("saved");
    let context = PluginListContext {
        data_paths,
        worker: WorkerCommand {
            program: PathBuf::from(env!("CARGO_BIN_EXE_audiobatch-rs")),
            arguments: vec![OsString::from(WORKER_FLAG)],
            environment: Vec::new(),
        },
        show_progress: false,
    };
    let listing = PluginListing {
        rescan: true,
        grouping: PluginGrouping::Manufacturer,
    };
    let mut output = Vec::new();
    let mut log = Vec::new();
    let code = run_plugin_list(listing, &context, &mut output, &mut log).expect("listing runs");
    let output = String::from_utf8(output).expect("utf-8");
    let log = String::from_utf8(log).expect("utf-8");
    assert_eq!(code, EXIT_SUCCESS, "{log}");
    for kind in Kind::ALL {
        assert!(output.contains(kind.name()), "{output}");
    }
    assert!(log.contains(&format!("{} plugins", Kind::ALL.len())), "{log}");
    let saved = KnownPlugins::load_from(&context.data_paths.known_plugins).expect("loads");
    assert_eq!(saved.plugins.len(), Kind::ALL.len());
}
