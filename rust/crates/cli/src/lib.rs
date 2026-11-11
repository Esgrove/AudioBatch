//! Command line front end of `AudioBatch`.
//!
//! Parses the arguments, runs the analysis through the shared core, and prints the results as a table, CSV, or JSON.
//! The `list` subcommand lists the known VST3 and CLAP plugins instead, after scanning for them with `--rescan`.
//! The options, field names, and exit codes are the same as those of the C++ `audiobatch` CLI,
//! so `scripts/parity.sh` can compare the two directly.

pub mod args;
pub mod completion;
pub mod messages;
pub mod output;
pub mod plugin_list;
pub mod process;
pub mod run;

/// Captured output of an in-process CLI command, with text decoded only at the test boundary.
#[cfg(test)]
pub(crate) struct CommandOutput {
    pub exit_code: u8,
    pub stdout: String,
    pub stderr: String,
}

#[cfg(test)]
impl CommandOutput {
    /// Decodes the output buffers returned by a command invocation.
    fn new(exit_code: u8, stdout: Vec<u8>, stderr: Vec<u8>) -> Self {
        Self {
            exit_code,
            stdout: String::from_utf8(stdout).expect("UTF-8 stdout"),
            stderr: String::from_utf8(stderr).expect("UTF-8 stderr"),
        }
    }
}
