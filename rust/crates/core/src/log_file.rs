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
//! The file also names each message's source module, including third-party tag readers and decoders.
//! Bracketed levels are right-aligned, with padding outside the brackets.
//! Warnings and errors include their source file and line without absolute build-machine paths.
//! Panics are logged with a backtrace before the default panic message is printed.

use std::backtrace::Backtrace;
use std::fs::{File, OpenOptions};
use std::io::{IsTerminal as _, Read as _, Seek as _, SeekFrom, Write as _};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use jiff::Zoned;
use tracing::level_filters::LevelFilter;
use tracing::{Event, Level, Metadata, Subscriber};
use tracing_log::NormalizeEvent as _;
use tracing_subscriber::Layer as _;
use tracing_subscriber::filter::{FilterFn, Targets, filter_fn};
use tracing_subscriber::fmt::format::{FormatEvent, FormatFields, Writer};
use tracing_subscriber::fmt::time::FormatTime;
use tracing_subscriber::fmt::{FmtContext, FormattedFields};
use tracing_subscriber::layer::SubscriberExt as _;
use tracing_subscriber::registry::LookupSpan;
use tracing_subscriber::util::SubscriberInitExt as _;

use crate::cache::data_directory;

/// Target of the messages the caller already showed to the user, which only go to the log file.
pub const FILE_ONLY_TARGET: &str = "audiobatch::file_only";

/// Size the log file is trimmed to when it is opened, so the log keeps only recent runs.
const MAXIMUM_INITIAL_LOG_BYTES: u64 = 32_768;

/// Target prefixes of the `AudioBatch` crates, whose messages are kept below the warning level.
const OWN_TARGET_PREFIXES: [&str; 4] = ["audiobatch", "AudioBatchRs", "serato_tags", "vst3_host"];

/// A failure to open the log, write its banner, or install the global subscriber.
#[derive(Debug, thiserror::Error)]
pub enum LogFileError {
    /// Opening or trimming the log failed.
    #[error("Could not open the log file {}", path.display())]
    Open {
        /// Log file to open.
        path: PathBuf,
        /// Filesystem failure.
        #[source]
        source: std::io::Error,
    },
    /// Writing the run banner failed.
    #[error("Could not write the log file {}", path.display())]
    Write {
        /// Log file to write.
        path: PathBuf,
        /// Filesystem failure.
        #[source]
        source: std::io::Error,
    },
    /// The global tracing subscriber could not be installed.
    #[error("A logger is already installed")]
    Install(#[source] tracing_subscriber::util::TryInitError),
}

/// Formats the local time of day at the start of each line.
struct LocalTime;

/// Formats bracketed levels, source locations, and the operation spans of each message.
struct LogFormat;

impl FormatTime for LocalTime {
    fn format_time(&self, writer: &mut Writer<'_>) -> std::fmt::Result {
        write!(writer, "{}", Zoned::now().strftime("%H:%M:%S"))
    }
}

impl<S, N> FormatEvent<S, N> for LogFormat
where
    S: Subscriber + for<'lookup> LookupSpan<'lookup>,
    N: for<'writer> FormatFields<'writer> + 'static,
{
    fn format_event(
        &self,
        context: &FmtContext<'_, S, N>,
        mut writer: Writer<'_>,
        event: &Event<'_>,
    ) -> std::fmt::Result {
        let normalized = event.normalized_metadata();
        let metadata = normalized.as_ref().unwrap_or_else(|| event.metadata());
        LocalTime.format_time(&mut writer)?;
        let padding = if matches!(*metadata.level(), Level::INFO | Level::WARN) {
            " "
        } else {
            ""
        };
        write!(writer, " {padding}[{}]: ", metadata.level())?;

        let unit = Some(metadata.target())
            .filter(|target| !target.is_empty())
            .or_else(|| metadata.module_path().filter(|module| !module.is_empty()));
        if let Some(unit) = unit {
            write!(writer, "{unit}: ")?;
        }
        if *metadata.level() <= Level::WARN
            && let Some(file) = metadata.file()
        {
            write!(writer, "{}", short_source_path(file))?;
            if let Some(line) = metadata.line() {
                write!(writer, ":{line}")?;
            }
            write!(writer, ": ")?;
        }
        if let Some(scope) = context.event_scope() {
            for span in scope.from_root() {
                write!(writer, "{}", span.metadata().name())?;
                let extensions = span.extensions();
                if let Some(fields) = extensions.get::<FormattedFields<N>>()
                    && !fields.is_empty()
                {
                    write!(writer, "{{{fields}}}")?;
                }
                drop(extensions);
                write!(writer, ": ")?;
            }
        }
        context.format_fields(writer.by_ref(), event)?;
        writeln!(writer)
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
pub fn install(log_name: &str, stderr_level: LevelFilter) -> Result<PathBuf, LogFileError> {
    let path = log_directory().join(format!("{log_name}.log"));
    let mut file = open_trimmed(&path).map_err(|source| LogFileError::Open {
        path: path.clone(),
        source,
    })?;
    writeln!(
        file,
        "\n**********************************************************\n{log_name}\nLog started: {}\n",
        Zoned::now().strftime("%d %b %Y %H:%M:%S %Z (%:z)")
    )
    .map_err(|source| LogFileError::Write {
        path: path.clone(),
        source,
    })?;

    let file_layer = tracing_subscriber::fmt::layer()
        .with_writer(Mutex::new(file))
        .with_ansi(false)
        .event_format(LogFormat)
        .with_filter(kept_levels(LevelFilter::TRACE));
    let stderr_layer = tracing_subscriber::fmt::layer()
        .with_writer(std::io::stderr)
        .with_ansi(std::io::stderr().is_terminal() && std::env::var_os("NO_COLOR").is_none())
        .event_format(LogFormat)
        .with_filter(stderr_filter(stderr_level));
    tracing_subscriber::registry()
        .with(file_layer)
        .with(stderr_layer)
        .try_init()
        .map_err(LogFileError::Install)?;
    install_panic_hook();
    Ok(path)
}

/// Returns the folder of the log files.
/// That is `~/Library/Logs/AudioBatch-rs` on macOS, the folder JUCE uses for the C++ application,
/// and the `logs` folder in the data directory elsewhere.
#[must_use]
pub(crate) fn log_directory() -> PathBuf {
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

/// Returns the filter of the stderr layer, which prints the events `stderr_levels` keeps.
/// Spans are kept at the levels of the log file,
/// so a warning on stderr names the spans it was logged in, such as the file a worker is analyzing.
fn stderr_filter(most_detailed: LevelFilter) -> FilterFn<impl Fn(&Metadata<'_>) -> bool> {
    let events = stderr_levels(most_detailed);
    let spans = kept_levels(LevelFilter::TRACE);
    filter_fn(move |metadata| {
        let levels = if metadata.is_span() { &spans } else { &events };
        levels.would_enable(metadata.target(), metadata.level())
    })
}

/// Keeps the crate and source-relative path, removing build-machine prefixes from source locations.
/// Both separator styles are recognized because the location was embedded when the binary was compiled.
fn short_source_path(file: &str) -> &str {
    if let Some((source_root, _)) = file.rsplit_once(r"\src\").or_else(|| file.rsplit_once("/src/"))
        && let Some((build_root, _)) = source_root.rsplit_once(['\\', '/'])
        && let Some(relative) = file
            .strip_prefix(build_root)
            .and_then(|relative| relative.strip_prefix(['\\', '/']))
    {
        return relative;
    }
    if file.starts_with(['\\', '/']) || file.as_bytes().get(1) == Some(&b':') {
        return file.rsplit(['\\', '/']).next().unwrap_or(file);
    }
    file
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
    use std::sync::Arc;

    use super::*;

    /// Writer that collects what a layer prints, standing in for stderr.
    #[derive(Clone, Default)]
    struct CapturedOutput(Arc<Mutex<Vec<u8>>>);

    impl std::io::Write for CapturedOutput {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.0.lock().expect("output lock").extend_from_slice(bytes);
            Ok(bytes.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

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

    #[test]
    fn stderr_warnings_name_the_spans_below_the_stderr_level() {
        let output = CapturedOutput::default();
        let writer = output.clone();
        let layer = tracing_subscriber::fmt::layer()
            .with_writer(move || writer.clone())
            .with_ansi(false)
            .event_format(LogFormat)
            .with_filter(stderr_filter(LevelFilter::WARN));
        tracing::subscriber::with_default(tracing_subscriber::registry().with(layer), || {
            let _span =
                tracing::info_span!(target: "audiobatch_core::analysis", "analyze", file = "song.wav").entered();
            tracing::info!(target: "audiobatch_core::analysis", "not printed");
            tracing::warn!(target: "audiobatch_core::analysis", "decoder warning");
        });

        let printed = String::from_utf8(output.0.lock().expect("output lock").clone()).expect("utf-8");
        assert!(printed.contains("analyze{file=\"song.wav\"}"), "{printed}");
        assert!(printed.contains("decoder warning"), "{printed}");
        assert!(!printed.contains("not printed"), "{printed}");
    }

    #[test]
    fn tag_reader_warnings_name_the_file_and_source_without_an_analysis_span() {
        let output = CapturedOutput::default();
        let writer = output.clone();
        let layer = tracing_subscriber::fmt::layer()
            .with_writer(move || writer.clone())
            .with_ansi(false)
            .event_format(LogFormat)
            .with_filter(kept_levels(LevelFilter::TRACE));
        let _subscriber = tracing_subscriber::registry().with(layer).set_default();
        let path = test_fixtures::fixture("sine_stereo_44100_192k_no_xing.mp3");
        crate::tags::read_tags(&path).expect("tags");

        let printed = String::from_utf8(output.0.lock().expect("output lock").clone()).expect("utf-8");
        let warning = printed
            .lines()
            .find(|line| line.contains("Using bitrate to estimate duration"))
            .unwrap_or_else(|| panic!("MP3 duration warning missing: {printed}"));
        assert!(warning.contains("sine_stereo_44100_192k_no_xing.mp3"), "{warning}");
        assert!(warning.contains("lofty::mpeg"), "{warning}");
        assert!(warning.contains(&path.display().to_string()), "{warning}");
    }

    #[test]
    fn decoder_warnings_after_seek_name_the_file_and_source() {
        let output = CapturedOutput::default();
        let writer = output.clone();
        let layer = tracing_subscriber::fmt::layer()
            .with_writer(move || writer.clone())
            .with_ansi(false)
            .event_format(LogFormat)
            .with_filter(kept_levels(LevelFilter::TRACE));
        let _subscriber = tracing_subscriber::registry().with(layer).set_default();
        let path = test_fixtures::fixture("sine_stereo_44100_192k_no_xing.mp3");
        let mut decoder = crate::decode::AudioFileDecoder::open(&path).expect("decoder");
        decoder.seek(0.5).expect("seek");
        while decoder.next_chunk().expect("decoded").is_some() {}

        let printed = String::from_utf8(output.0.lock().expect("output lock").clone()).expect("utf-8");
        let warning = printed
            .lines()
            .find(|line| line.contains("invalid main_data_begin"))
            .unwrap_or_else(|| panic!("MP3 reservoir warning missing: {printed}"));
        assert!(warning.contains("sine_stereo_44100_192k_no_xing.mp3"), "{warning}");
        assert!(warning.contains("symphonia"), "{warning}");
        assert!(warning.contains(&path.display().to_string()), "{warning}");
    }

    #[test]
    fn levels_are_bracketed_and_errors_keep_their_source_and_operation() {
        let output = CapturedOutput::default();
        let writer = output.clone();
        let layer = tracing_subscriber::fmt::layer()
            .with_writer(move || writer.clone())
            .with_ansi(false)
            .event_format(LogFormat)
            .with_filter(kept_levels(LevelFilter::TRACE));
        tracing::subscriber::with_default(tracing_subscriber::registry().with(layer), || {
            tracing::info!(target: "audiobatch_core::analysis", "Found 3 audio files");
            let _span =
                tracing::info_span!(target: "audiobatch_core::analysis", "analyze", file = "song.wav").entered();
            tracing::warn!(target: "audiobatch_core::analysis", "decoder warning");
            tracing::error!(target: "audiobatch_core::analysis", frames = 42, "decode failed");
        });

        let printed = String::from_utf8(output.0.lock().expect("output lock").clone()).expect("utf-8");
        let lines: Vec<_> = printed.lines().collect();
        assert_eq!(lines.len(), 3, "{printed}");
        for (line, level) in lines.iter().zip(["INFO", "WARN", "ERROR"]) {
            let (time, message) = line.split_once(' ').expect("timestamp");
            assert_eq!(time.len(), 8, "{line}");
            let label = format!("[{level}]");
            assert!(
                message.starts_with(&format!("{label:>7}: audiobatch_core::analysis: ")),
                "{line}"
            );
            assert_eq!(line.find("audiobatch_core::analysis:"), Some(18), "{line}");
            if level != "INFO" {
                assert!(message.contains("log_file.rs:"), "{line}");
                assert!(message.contains("analyze{file=\"song.wav\"}"), "{line}");
            }
        }
        assert!(printed.contains("decode failed frames=42"), "{printed}");
    }

    #[test]
    fn empty_target_errors_keep_the_original_log_callers_file_and_line() {
        let output = CapturedOutput::default();
        let writer = output.clone();
        let layer = tracing_subscriber::fmt::layer()
            .with_writer(move || writer.clone())
            .with_ansi(false)
            .event_format(LogFormat)
            .with_filter(kept_levels(LevelFilter::TRACE));
        let _subscriber = tracing_subscriber::registry().with(layer).set_default();
        log::logger().log(
            &log::Record::builder()
                .target("")
                .file(Some(
                    r"C:\Users\builder\.cargo\registry\src\index.crates.io-1949cf8c6b5b557f\gpui-pre-windows-0.3.10\src\window.rs",
                ))
                .line(Some(42))
                .level(log::Level::Error)
                .args(format_args!("Invalid window handle (0x80040102)"))
                .build(),
        );

        let printed = String::from_utf8(output.0.lock().expect("output lock").clone()).expect("utf-8");
        assert!(
            printed
                .ends_with("[ERROR]: gpui-pre-windows-0.3.10\\src\\window.rs:42: Invalid window handle (0x80040102)\n"),
            "{printed}"
        );
        assert!(!printed.contains("[ERROR]: :"), "{printed}");
        assert!(!printed.contains("C:\\Users\\"), "{printed}");
        assert!(!printed.contains("registry"), "{printed}");
    }

    #[test]
    fn source_paths_keep_the_crate_without_the_build_machine_prefix() {
        for (file, expected) in [
            (
                r"C:\Users\builder\.cargo\registry\src\index.crates.io-hash\gpui-pre-0.3.10\src\window.rs",
                r"gpui-pre-0.3.10\src\window.rs",
            ),
            (
                "/Users/builder/.cargo/registry/src/index.crates.io-hash/gpui-pre-0.3.10/src/window.rs",
                "gpui-pre-0.3.10/src/window.rs",
            ),
            (
                r"D:\custom-cargo\registry\src\private-index\gpui-pre-windows-0.3.10\src\dispatcher.rs",
                r"gpui-pre-windows-0.3.10\src\dispatcher.rs",
            ),
            (
                r"C:\build\AudioBatch\rust\crates\core\src\log_file.rs",
                r"core\src\log_file.rs",
            ),
            (
                "/build/AudioBatch/rust/crates/core/src/log_file.rs",
                "core/src/log_file.rs",
            ),
            ("crates/core/src/log_file.rs", "core/src/log_file.rs"),
            (
                r"C:\build\framework\src\platform\window.rs",
                r"framework\src\platform\window.rs",
            ),
            (
                "/build/framework/src/platform/window.rs",
                "framework/src/platform/window.rs",
            ),
        ] {
            assert_eq!(short_source_path(file), expected, "{file}");
        }
    }

    #[test]
    fn other_source_paths_remain_relative_without_exposing_absolute_directories() {
        for (file, expected) in [
            ("src/main.rs", "src/main.rs"),
            ("window.rs", "window.rs"),
            ("platform/window.rs", "platform/window.rs"),
            (r"C:\build\generated\window.rs", "window.rs"),
            ("/build/generated/window.rs", "window.rs"),
            (r"\\server\build\generated\window.rs", "window.rs"),
            ("", ""),
        ] {
            assert_eq!(short_source_path(file), expected, "{file}");
        }
    }
}
