//! Plugin rescans with the `audiobatch-rs` executable as the scan worker,
//! which checks that the executable serves a scan when started with the worker flag.
//! CLAP processing uses an isolated scanned fixture and verifies the ordinary process subcommand's output.

use std::ffi::OsString;
use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_core::storage::StoragePaths;

use audiobatch_cli::args::{PluginGrouping, PluginListing};
use audiobatch_cli::plugin_list::{PluginListContext, run_plugin_list};
use audiobatch_cli::run::EXIT_SUCCESS;
use audiobatch_plugins::{
    ChainSlot, KnownPlugins, PluginChain, PluginDataPaths, PluginFormat, PresetStore, WORKER_FLAG, WorkerCommand,
};
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

#[test]
fn clap_rescan_and_process_use_the_cli_with_isolated_storage() {
    let library = audiobatch_test_clap_plugin::module_library_path().expect("CLAP module path");
    if !library.is_file() {
        assert!(
            std::env::var_os("CI").is_none(),
            "Build CLAP fixture: {}",
            library.display()
        );
        return;
    }
    let directory = tempfile::tempdir().expect("isolated application root");
    let folder = directory.path().join("plugins");
    std::fs::create_dir_all(&folder).expect("fixture folder");
    audiobatch_test_clap_plugin::create_bundle(&folder, "CliPreview").expect("CLAP fixture bundle");
    let storage = StoragePaths::in_root(directory.path()).expect("isolated storage");
    let paths = PluginDataPaths::in_folder(storage.data());
    KnownPlugins {
        custom_folders: vec![folder],
        ..KnownPlugins::default()
    }
    .save_to(&paths.known_plugins)
    .expect("isolated scan folders");
    let mut list = isolated_command(&storage);
    list.args(["list", "--rescan", "--group-by", "format"]);
    let listing = run_bounded(list);
    assert!(listing.status.success(), "{}", String::from_utf8_lossy(&listing.stderr));
    let text = String::from_utf8(listing.stdout).expect("list output");
    assert!(text.contains("CLAP (1)"), "{text}");
    assert!(text.contains("AudioBatch Test MS CLAP"), "{text}");
    let known = KnownPlugins::load_from(&paths.known_plugins).expect("scanned descriptions");
    let plugin = known.plugins.into_iter().next().expect("scanned CLAP plugin");
    assert_eq!(plugin.format, PluginFormat::Clap);
    let chain = PluginChain {
        slots: vec![ChainSlot::new(plugin)],
    };
    let preset = PresetStore::new(paths.presets)
        .save("CLAP", &chain)
        .expect("native CLAP preset");
    let source = directory.path().join("source.wav");
    std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &source).expect("copy source");
    let expected = decode_samples(&source);
    let mut process = isolated_command(&storage);
    process
        .args(["process", "--preset"])
        .arg(preset)
        .args(["--delete", "--jobs", "1", "--output", "json"])
        .arg(&source);
    let result = run_bounded(process);
    assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
    let rows: serde_json::Value = serde_json::from_slice(&result.stdout).expect("ordinary analysis JSON");
    assert_eq!(rows.as_array().expect("record array").len(), 1);
    let output = source.with_extension("aif");
    assert_eq!(
        rows.as_array()
            .and_then(|rows| rows.first())
            .and_then(|row| row.get("path"))
            .and_then(serde_json::Value::as_str),
        output.to_str()
    );
    assert!(!source.exists(), "only the isolated copied source is deleted");
    assert_eq!(
        decode_samples(&output),
        expected,
        "unity CLAP render preserves fixture PCM"
    );
}

/// Applies the storage override only to child processes, never to concurrent tests.
fn isolated_command(storage: &StoragePaths) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_audiobatch-rs"));
    command.env("AUDIOBATCH_DATA_ROOT", storage.data().parent().expect("isolated root"));
    command.env_remove("AUDIOBATCH_DELETE");
    command
}

/// Waits a bounded time without permitting a broken plugin worker to hang the integration suite.
fn run_bounded(mut command: Command) -> std::process::Output {
    let mut child = command
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("CLI starts");
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        if child.try_wait().expect("CLI status").is_some() {
            return child.wait_with_output().expect("CLI output");
        }
        if Instant::now() >= deadline {
            child.kill().expect("stop timed-out CLI");
            let result = child.wait_with_output().expect("timed-out CLI output");
            assert!(
                Instant::now() < deadline,
                "CLI timed out: {}",
                String::from_utf8_lossy(&result.stderr)
            );
            return result;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// Collects decoded planes for the short committed PCM fixture, preserving frame and channel order.
fn decode_samples(path: &std::path::Path) -> Vec<Vec<f32>> {
    let mut decoder = AudioFileDecoder::open(path).expect("decode fixture");
    let mut samples = Vec::<Vec<f32>>::new();
    while let Some(chunk) = decoder.next_chunk().expect("fixture PCM") {
        samples.resize_with(chunk.len(), Vec::new);
        for (plane, block) in samples.iter_mut().zip(chunk) {
            plane.extend_from_slice(block);
        }
    }
    samples
}
