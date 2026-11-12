//! Captures native AudioBatch-rs presets from explicit build-tree `ImageControl` artifacts for CLI integration.
//! Uses production format hosts and preset persistence, without installed discovery, editors, or audio devices.

use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, bail, ensure};
use audiobatch_plugins::library::bundle_modified_ms;
use audiobatch_plugins::{ChainSlot, HostInstance, PluginChain, PluginDescription, PresetStore};
use vst3_host::{ProcessMode, UiThread};

/// Explicit integration input and a new isolated preset directory.
struct Options {
    /// Canonical repository-built VST3 or CLAP selected by the caller.
    bundle: PathBuf,
    /// New output directory that must not already exist.
    artifacts: PathBuf,
}

/// Parses paths before initializing native hosts or creating output directories.
fn arguments() -> Result<Option<Options>> {
    let values: Vec<_> = std::env::args_os().skip(1).collect();
    if values.iter().any(|value| value == "-h" || value == "--help") {
        println!(
            "Usage: image_control_presets -b|--bundle <absolute.vst3|absolute.clap> -A|--artifacts <new-directory>"
        );
        return Ok(None);
    }
    let mut bundle = None;
    let mut artifacts = None;
    let mut values = values.into_iter();
    while let Some(option) = values.next() {
        let value = values.next().context("every option requires a path")?;
        match option.to_str() {
            Some("-b" | "--bundle") => bundle = Some(PathBuf::from(value)),
            Some("-A" | "--artifacts") => artifacts = Some(PathBuf::from(value)),
            _ => bail!("unknown option {}", option.to_string_lossy()),
        }
    }
    let bundle = bundle.context("--bundle is required")?;
    ensure!(bundle.is_absolute(), "--bundle must be absolute");
    let bundle = bundle.canonicalize().context("selected bundle must exist")?;
    let repository = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..").canonicalize()?;
    ensure!(bundle.starts_with(repository), "select a repository build artifact");
    let artifacts = artifacts.context("--artifacts is required")?;
    ensure!(
        artifacts.is_absolute() && !artifacts.exists(),
        "--artifacts must be a new absolute directory"
    );
    Ok(Some(Options { bundle, artifacts }))
}

/// Reads the real format descriptor without searching installed plugin locations.
fn description(ui: &UiThread, bundle: &Path) -> Result<PluginDescription> {
    let modified = bundle_modified_ms(bundle);
    match bundle.extension().and_then(|extension| extension.to_str()) {
        Some("vst3") => {
            let module = vst3_host::Module::load(ui, bundle)?;
            let class = module
                .classes()
                .iter()
                .find(|class| {
                    class.is_audio_effect() && matches!(class.name.as_str(), "ImageControl" | "ImageControl RS")
                })
                .context("selected VST3 has no ImageControl effect")?;
            Ok(PluginDescription::from_class(class, bundle, modified))
        }
        Some("clap") => {
            let module = clap_host::Module::load(ui, bundle)?;
            let class = module
                .classes(ui)
                .iter()
                .find(|class| {
                    class.is_effect && class.id == "com.esgrove.imagecontrol-rs-own" && class.name == "ImageControl RS"
                })
                .context("selected CLAP has no ImageControl RS effect")?;
            Ok(PluginDescription::from_clap_class(class, bundle, modified))
        }
        _ => bail!("select a build-tree .vst3 or .clap"),
    }
}

/// Saves state obtained through the production host, with a matching plain output trim in both formats.
fn capture(ui: &UiThread, description: &PluginDescription, gain_db: f64) -> Result<PluginChain> {
    let mut instance = HostInstance::load(ui, description, None)?;
    instance.configure_main_buses(ui, 2)?;
    instance.setup(ui, ProcessMode::Offline, 48_000.0, 512)?;
    instance.activate(ui)?;
    match &instance {
        HostInstance::Vst3(instance) => {
            let id = if description.name == "ImageControl RS" {
                0
            } else {
                instance
                    .parameter_id(ui, "Output Gain")?
                    .context("C++ output parameter")?
            };
            instance.set_parameter(ui, id, (gain_db + 24.0) / 36.0)?;
        }
        HostInstance::Clap(instance) => instance.set_parameter(ui, 0, gain_db)?,
    }
    let mut slot = ChainSlot::new(description.clone());
    let state = instance.state(ui);
    instance.deactivate(ui)?;
    let state = state?;
    ensure!(!state.is_empty(), "the native host returned empty state");
    slot.set_state(state)?;
    Ok(PluginChain { slots: vec![slot] })
}

/// Captures two independently reloadable presets without touching application settings or user libraries.
fn main() -> Result<()> {
    let Some(options) = arguments()? else {
        return Ok(());
    };
    let ui = UiThread::register_current()?;
    let description = description(&ui, &options.bundle)?;
    let store = PresetStore::new(options.artifacts);
    for (name, gain) in [("neutral", 0.0), ("gain-minus-six", -6.0)] {
        let chain = capture(&ui, &description, gain)?;
        let path = store.save(name, &chain)?;
        ensure!(
            store.load_strict(name)?.chain() == chain,
            "native preset did not round-trip"
        );
        println!("{}", path.display());
    }
    Ok(())
}
