//! Command line front end of the `AudioBatch` Rust port.
//!
//! Parses the arguments, runs the analysis through the shared core, and prints the results as a table, CSV, or JSON,
//! with the same options, field names, and exit codes as the C++ `audiobatch` CLI.

pub mod args;
pub mod completion;
pub mod output;
pub mod run;
