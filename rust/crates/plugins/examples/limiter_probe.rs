//! Manual gain sweep comparing a saved limiter preset against a zero-dBFS hard-clipping model.
//! Operates only on temporary source copies, leaving the input audio and saved preset untouched.
//! Run with a preset-folder path, preset name, and one or more input audio files:
//!
//! ```shell
//! cargo run --release -p audiobatch-plugins --example limiter_probe -- \
//!     ~/Library/Application\ Support/AudioBatch-rs/chain-presets L2 track1.flac track2.wav
//! ```
//!
//! It prints one CSV row per file and gain in `GAINS`,
//! with the source, gain-only, hard-clipped, and rendered integrated loudness, and the rendered peaks.
//! The `source_lufs`, `hard_clip_lufs`, and `limiter_lufs` columns are the observations
//! that `fixtures/limiter_model.py` fits the gain-adjusted limiter loudness model to.
//! Use the release profile, which the committed observations were measured with.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};

use audiobatch_core::analyze_file;
use audiobatch_core::gain_loudness::measure_gained_loudness;
use audiobatch_plugins::{PresetStore, ProcessingChain, ProcessingOptions, process_file};
use tempfile::TempDir;
use vst3_host::{UiDispatcher, UiThread};

/// Input gains in dB, including unity to expose the saved preset's own processing.
const GAINS: [f32; 6] = [0.0, 1.0, 2.0, 3.0, 4.0, 6.0];
/// One UI thread owns every native plugin instance in this process.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Provides the native plugin UI dispatcher.
fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("starts native plugin thread"))
}

/// Measures the gain-and-hard-clip model, retaining only one decode block in memory.
fn clipped_loudness(path: &Path, gain_db: f32) -> anyhow::Result<f64> {
    Ok(measure_gained_loudness(path, gain_db, &|| false)?.integrated())
}

/// Renders temporary copies through fresh instances of the saved preset and prints CSV measurements.
fn main() -> anyhow::Result<()> {
    let mut args = std::env::args_os().skip(1);
    let folder = PathBuf::from(args.next().ok_or_else(|| anyhow::anyhow!("missing preset folder"))?);
    let name = args.next().ok_or_else(|| anyhow::anyhow!("missing preset name"))?;
    let chain = PresetStore::new(folder).load(&name.to_string_lossy())?.chain();
    let sources: Vec<PathBuf> = args.map(PathBuf::from).collect();
    anyhow::ensure!(!sources.is_empty(), "provide audio files");
    let options = ProcessingOptions {
        dispose: Arc::new(|path: &Path| fs::remove_file(path).is_ok() || !path.exists()),
        ..ProcessingOptions::default()
    };
    println!("file,gain_db,source_lufs,gain_only_lufs,hard_clip_lufs,limiter_lufs,limiter_peak,limiter_true_peak");
    for source in sources {
        let original = analyze_file(&source, &|| false, &mut |_| {});
        anyhow::ensure!(original.is_ready(), "{}: {}", source.display(), original.error_message);
        for gain in GAINS {
            let scratch = TempDir::new()?;
            let copy = scratch
                .path()
                .join(source.file_name().ok_or_else(|| anyhow::anyhow!("missing file name"))?);
            fs::copy(&source, &copy)?;
            let mut record = analyze_file(&copy, &|| false, &mut |_| {});
            record.custom_gain_db = Some(gain);
            let saved = chain.clone();
            let processing = dispatcher().call(
                move |ui| ProcessingChain::instantiate(ui, dispatcher(), &saved),
                &|| false,
            )??;
            let result = process_file(&record, &options, &processing, &|| false, &mut |_| {});
            anyhow::ensure!(result.error().is_none(), "{:?}", result.error());
            let output = result.output().ok_or_else(|| anyhow::anyhow!("no output"))?;
            let clipped = clipped_loudness(&source, gain)?;
            println!(
                "\"{}\",{gain:.1},{:.4},{:.4},{clipped:.4},{:.4},{:.6},{:.6}",
                source
                    .file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .replace('"', "\"\""),
                original.integrated_lufs,
                original.integrated_lufs + f64::from(gain),
                output.integrated_lufs,
                output.overall_peak,
                output.overall_true_peak
            );
        }
    }
    Ok(())
}
