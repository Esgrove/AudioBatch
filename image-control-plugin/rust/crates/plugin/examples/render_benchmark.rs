//! Headless renderer-only benchmark entry point. Help exits before file reads and native GPU initialization.

use image_control_plugin_rs::render_benchmark::{USAGE, parse_options, run, write_report};

/// Runs only an explicitly selected workload and writes its completed versioned timing artifact.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let Some(options) = parse_options(std::env::args_os().skip(1).collect())? else {
        print!("{USAGE}");
        return Ok(());
    };
    let report = run(&options)?;
    write_report(&options, &report)?;
    println!("Renderer benchmark completed. Versioned report written to the requested path.");
    Ok(())
}
