//! Coloured status messages for the log, which is stderr for the CLI.
//!
//! Every helper writes to the given writer instead of stderr directly,
//! so the tests can capture the messages and check them.
//! Each message also goes to the log file, without being printed a second time.
//! The macros take the writer first, then `format!` arguments.

use std::io::{self, Write};

use audiobatch_core::log_file::FILE_ONLY_TARGET;
use colored::Colorize;

/// Writes "Error: " and the message in red.
///
/// # Errors
///
/// Returns an error when the log cannot be written.
pub fn write_error(log: &mut dyn Write, message: &str) -> io::Result<()> {
    tracing::error!(target: FILE_ONLY_TARGET, "{message}");
    writeln!(log, "{}", format!("Error: {message}").red())
}

/// Writes "Warning: " and the message in yellow.
///
/// # Errors
///
/// Returns an error when the log cannot be written.
pub fn write_warning(log: &mut dyn Write, message: &str) -> io::Result<()> {
    tracing::warn!(target: FILE_ONLY_TARGET, "{message}");
    writeln!(log, "{}", format!("Warning: {message}").yellow())
}

/// Writes the message in red without a prefix, for failures that name what failed themselves.
///
/// # Errors
///
/// Returns an error when the log cannot be written.
pub fn write_failure(log: &mut dyn Write, message: &str) -> io::Result<()> {
    tracing::error!(target: FILE_ONLY_TARGET, "{message}");
    writeln!(log, "{}", message.red())
}

/// Writes the message as it is.
///
/// # Errors
///
/// Returns an error when the log cannot be written.
pub fn write_info(log: &mut dyn Write, message: &str) -> io::Result<()> {
    tracing::info!(target: FILE_ONLY_TARGET, "{message}");
    writeln!(log, "{message}")
}

/// Writes the message in bold.
///
/// # Errors
///
/// Returns an error when the log cannot be written.
pub fn write_bold(log: &mut dyn Write, message: &str) -> io::Result<()> {
    tracing::info!(target: FILE_ONLY_TARGET, "{message}");
    writeln!(log, "{}", message.bold())
}

/// Writes the message dimmed, for details such as counts.
///
/// # Errors
///
/// Returns an error when the log cannot be written.
pub fn write_dimmed(log: &mut dyn Write, message: &str) -> io::Result<()> {
    tracing::info!(target: FILE_ONLY_TARGET, "{message}");
    writeln!(log, "{}", message.dimmed())
}

/// Writes a formatted error message with `write_error`.
#[macro_export]
macro_rules! write_error {
    ($log:expr, $($arg:tt)*) => {
        $crate::messages::write_error($log, &format!($($arg)*))
    };
}

/// Writes a formatted warning message with `write_warning`.
#[macro_export]
macro_rules! write_warning {
    ($log:expr, $($arg:tt)*) => {
        $crate::messages::write_warning($log, &format!($($arg)*))
    };
}

/// Writes a formatted failure message with `write_failure`.
#[macro_export]
macro_rules! write_failure {
    ($log:expr, $($arg:tt)*) => {
        $crate::messages::write_failure($log, &format!($($arg)*))
    };
}

/// Writes a formatted plain message with `write_info`.
#[macro_export]
macro_rules! write_info {
    ($log:expr, $($arg:tt)*) => {
        $crate::messages::write_info($log, &format!($($arg)*))
    };
}

/// Writes a formatted bold message with `write_bold`.
#[macro_export]
macro_rules! write_bold {
    ($log:expr, $($arg:tt)*) => {
        $crate::messages::write_bold($log, &format!($($arg)*))
    };
}

/// Writes a formatted dimmed message with `write_dimmed`.
#[macro_export]
macro_rules! write_dimmed {
    ($log:expr, $($arg:tt)*) => {
        $crate::messages::write_dimmed($log, &format!($($arg)*))
    };
}

#[cfg(test)]
mod test_messages {
    use super::*;

    /// Runs the writer and returns its text without the terminal styling.
    fn written(write: impl FnOnce(&mut dyn Write) -> io::Result<()>) -> String {
        let mut log = Vec::new();
        write(&mut log).expect("written");
        let text = String::from_utf8(log).expect("utf-8");
        let mut plain = String::new();
        let mut characters = text.chars();
        while let Some(character) = characters.next() {
            if character == '\x1b' {
                characters.by_ref().find(|next| *next == 'm');
            } else {
                plain.push(character);
            }
        }
        plain
    }

    #[test]
    fn errors_and_warnings_have_a_prefix() {
        assert_eq!(
            written(|log| write_error!(log, "{} failed", "Scan")),
            "Error: Scan failed\n"
        );
        assert_eq!(
            written(|log| write_warning!(log, "{} skipped", 2)),
            "Warning: 2 skipped\n"
        );
    }

    #[test]
    fn failures_and_plain_messages_have_no_prefix() {
        assert_eq!(
            written(|log| write_failure!(log, "Analysis failed: {}", "a.wav")),
            "Analysis failed: a.wav\n"
        );
        assert_eq!(written(|log| write_info!(log, "{} files", 2)), "2 files\n");
    }

    #[test]
    fn bold_and_dimmed_keep_the_text() {
        assert_eq!(
            written(|log| write_bold!(log, "Scanning {}", "VST3")),
            "Scanning VST3\n"
        );
        assert_eq!(written(|log| write_dimmed!(log, "{} plugins", 3)), "3 plugins\n");
    }
}
