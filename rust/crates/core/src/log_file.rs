//! Log file of the CLI and the GUI.
//!
//! `install` sets a `tracing` subscriber that appends timestamped lines to `<name>.log` in the platform log folder,
//! like the JUCE file logger of the C++ application.
//! Records of crates that use the `log` facade, such as the decoders, are forwarded to it.
//! The file is trimmed to its last 32 KB when it is opened, and every run starts with a banner.
//! Messages of the `AudioBatch` crates are kept from the debug level in debug builds and from the info level otherwise,
//! and messages of other crates only from the warning level.
//! Messages up to a chosen level are also printed to stderr, except those the caller already showed to the user.
//! Each line names the spans it was logged in, such as the file a worker thread is analyzing.
//! Panics are logged with a backtrace before the default panic message is printed.

use std::backtrace::Backtrace;
use std::fs::{File, OpenOptions};
use std::io::{IsTerminal as _, Read as _, Seek as _, SeekFrom, Write as _};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use anyhow::{Context as _, Result};
use jiff::Zoned;
use tracing::level_filters::LevelFilter;
use tracing_subscriber::Layer as _;
use tracing_subscriber::filter::Targets;
use tracing_subscriber::fmt::format::Writer;
use tracing_subscriber::fmt::time::FormatTime;
use tracing_subscriber::layer::SubscriberExt as _;
use tracing_subscriber::util::SubscriberInitExt as _;

use crate::cache::data_directory;

/// Target of the messages the caller already showed to the user, which only go to the log file.
pub const FILE_ONLY_TARGET: &str = "audiobatch::file_only";

/// Size the log file is trimmed to when it is opened, the same as in the C++ application.
const MAXIMUM_INITIAL_LOG_BYTES: u64 = 32_768;

/// Target prefixes of the `AudioBatch` crates, whose messages are kept below the warning level.
const OWN_TARGET_PREFIXES: [&str; 4] = ["audiobatch", "AudioBatchRs", "serato_tags", "vst3_host"];

/// Formats the local time of day at the start of each line, like the C++ application.
struct LocalTime;

impl FormatTime for LocalTime {
    fn format_time(&self, writer: &mut Writer<'_>) -> std::fmt::Result {
        write!(writer, "{}", Zoned::now().strftime("%H:%M:%S"))
    }
}

/// Opens `<log_name>.log` in the log folder and returns its path.
///
/// The file becomes the target of the `tracing` and `log` macros and of panics,
/// and messages up to `stderr_level` are printed to stderr as well.
///
/// # Errors
///
/// Returns an error when the log file cannot be opened or written, or when a subscriber is already installed.
pub fn install(log_name: &str, stderr_level: LevelFilter) -> Result<PathBuf> {
    let path = log_directory().join(format!("{log_name}.log"));
    let mut file = open_trimmed(&path).with_context(|| format!("Could not open the log file {}", path.display()))?;
    writeln!(
        file,
        "\n**********************************************************\n{log_name}\nLog started: {}\n",
        Zoned::now().strftime("%d %b %Y %H:%M:%S %Z (%:z)")
    )
    .with_context(|| format!("Could not write the log file {}", path.display()))?;

    let file_layer = tracing_subscriber::fmt::layer()
        .with_writer(Mutex::new(file))
        .with_ansi(false)
        .with_timer(LocalTime)
        .with_target(false)
        .with_filter(kept_levels(LevelFilter::TRACE));
    let stderr_layer = tracing_subscriber::fmt::layer()
        .with_writer(std::io::stderr)
        .with_ansi(std::io::stderr().is_terminal() && std::env::var_os("NO_COLOR").is_none())
        .with_timer(LocalTime)
        .with_target(false)
        .with_filter(stderr_levels(stderr_level));
    tracing_subscriber::registry()
        .with(file_layer)
        .with(stderr_layer)
        .try_init()
        .context("A logger is already installed")?;
    install_panic_hook();
    Ok(path)
}

/// Returns the folder of the log files.
/// That is `~/Library/Logs/AudioBatch-rs` on macOS, the folder JUCE uses for the C++ application,
/// and the `logs` folder in the data directory elsewhere.
#[must_use]
pub fn log_directory() -> PathBuf {
    platform_log_directory().unwrap_or_else(|| data_directory().join("logs"))
}

/// Returns the macOS log folder of the Rust version.
#[cfg(target_os = "macos")]
fn platform_log_directory() -> Option<PathBuf> {
    dirs::home_dir().map(|home| {
        home.join("Library")
            .join("Logs")
            .join(crate::cache::DATA_DIRECTORY_NAME)
    })
}

/// Returns nothing, since other platforms keep the logs in the data directory.
#[cfg(not(target_os = "macos"))]
const fn platform_log_directory() -> Option<PathBuf> {
    None
}

/// Returns the most detailed level kept for the `AudioBatch` crates.
const fn own_level() -> LevelFilter {
    if cfg!(debug_assertions) {
        LevelFilter::DEBUG
    } else {
        LevelFilter::INFO
    }
}

/// Returns the levels kept for each target, no more detailed than `most_detailed`.
/// The `AudioBatch` crates keep their own level, and other crates keep warnings and errors.
fn kept_levels(most_detailed: LevelFilter) -> Targets {
    Targets::new()
        .with_default(LevelFilter::WARN.min(most_detailed))
        .with_targets(OWN_TARGET_PREFIXES.map(|prefix| (prefix, own_level().min(most_detailed))))
}

/// Returns the levels printed to stderr, no more detailed than `most_detailed`.
/// Only the `AudioBatch` crates print there, since they report the failures of the other crates themselves,
/// and the messages the caller already showed are left out.
fn stderr_levels(most_detailed: LevelFilter) -> Targets {
    kept_levels(most_detailed)
        .with_default(LevelFilter::OFF)
        .with_target(FILE_ONLY_TARGET, LevelFilter::OFF)
}

/// Opens the log file for appending, creating it and its folder when missing.
/// A file larger than the size limit is first cut to its last lines that fit in it.
fn open_trimmed(path: &Path) -> std::io::Result<File> {
    if let Some(directory) = path.parent() {
        std::fs::create_dir_all(directory)?;
    }
    let length = std::fs::metadata(path).map_or(0, |metadata| metadata.len());
    if length > MAXIMUM_INITIAL_LOG_BYTES {
        let mut file = File::open(path)?;
        file.seek(SeekFrom::Start(length - MAXIMUM_INITIAL_LOG_BYTES))?;
        let mut tail = Vec::new();
        file.read_to_end(&mut tail)?;
        let start = tail.iter().position(|byte| *byte == b'\n').map_or(0, |index| index + 1);
        std::fs::write(path, tail.get(start..).unwrap_or_default())?;
    }
    OpenOptions::new().create(true).append(true).open(path)
}

/// Logs every panic with a backtrace, then runs the panic hook that was set before.
fn install_panic_hook() {
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        tracing::error!(target: FILE_ONLY_TARGET, "Panic: {info}\n{}", Backtrace::force_capture());
        previous(info);
    }));
}

#[cfg(test)]
mod test_log_file {
    use tracing::Level;

    use super::*;

    #[test]
    fn a_large_log_is_cut_to_its_last_whole_lines() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let path = directory.path().join("nested").join("app.log");
        std::fs::create_dir_all(path.parent().expect("parent")).expect("folder");
        let line = "0123456789012345678901234567890123456789\n";
        let content = line.repeat(2000);
        std::fs::write(&path, &content).expect("written");

        drop(open_trimmed(&path).expect("opened"));

        let trimmed = std::fs::read_to_string(&path).expect("read");
        assert!(trimmed.len() as u64 <= MAXIMUM_INITIAL_LOG_BYTES);
        assert!(trimmed.len() as u64 > MAXIMUM_INITIAL_LOG_BYTES - line.len() as u64);
        assert!(trimmed.starts_with("0123"));
        assert!(content.ends_with(&trimmed));
    }

    #[test]
    fn a_small_log_is_kept_and_appended_to() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let path = directory.path().join("logs").join("app.log");

        writeln!(open_trimmed(&path).expect("created"), "first").expect("written");
        writeln!(open_trimmed(&path).expect("opened"), "second").expect("written");

        assert_eq!(std::fs::read_to_string(&path).expect("read"), "first\nsecond\n");
    }

    #[test]
    fn other_crates_are_kept_only_from_the_warning_level() {
        let file = kept_levels(LevelFilter::TRACE);
        assert!(file.would_enable("audiobatch_core::output_file", &Level::INFO));
        assert!(file.would_enable("vst3_host::plugin", &Level::INFO));
        assert!(file.would_enable(FILE_ONLY_TARGET, &Level::INFO));
        assert!(file.would_enable("symphonia_format_riff", &Level::WARN));
        assert!(!file.would_enable("symphonia_format_riff", &Level::INFO));
    }

    #[test]
    fn stderr_shows_only_audiobatch_messages_the_caller_did_not_show() {
        let stderr = stderr_levels(LevelFilter::WARN);
        assert!(stderr.would_enable("audiobatch_core::output_file", &Level::WARN));
        assert!(!stderr.would_enable("audiobatch_core::output_file", &Level::INFO));
        assert!(!stderr.would_enable("symphonia_core::probe", &Level::ERROR));
        assert!(!stderr.would_enable(FILE_ONLY_TARGET, &Level::ERROR));
    }
}
