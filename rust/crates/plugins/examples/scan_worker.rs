//! A plugin scan worker for the scanner tests.
//!
//! It runs the same worker as the `AudioBatch` executables do when started with the worker flag,
//! so the tests can scan without building the CLI or the GUI.

fn main() {
    if audiobatch_plugins::is_scan_worker_invocation(std::env::args_os()) {
        std::process::exit(audiobatch_plugins::run_scan_worker());
    }
    eprintln!("Start with {}", audiobatch_plugins::WORKER_FLAG);
    std::process::exit(2);
}
