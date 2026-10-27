//! Command line front end of `AudioBatch`.
//!
//! Parses the arguments, runs the analysis through the shared core, and prints the results as a table, CSV, or JSON.
//! With `--list-plugins` it lists the known VST3 plugins instead, after scanning for them with `--rescan`.
//! The options, field names, and exit codes are the same as those of the C++ `audiobatch` CLI,
//! so `scripts/parity.sh` can compare the two directly.

pub mod args;
pub mod completion;
pub mod messages;
pub mod output;
pub mod plugin_list;
pub mod run;
