//! Command line front end of `AudioBatch`.
//!
//! Parses the arguments, runs the analysis through the shared core, and prints the results as a table, CSV, or JSON.
//! The options, field names, and exit codes are the same as those of the C++ `audiobatch` CLI,
//! so `scripts/parity.sh` can compare the two directly.

pub mod args;
pub mod completion;
pub mod output;
pub mod run;
