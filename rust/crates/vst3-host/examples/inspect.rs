//! Loads VST3 bundles in this process and prints their classes.
//!
//! Run with `cargo run -p vst3-host --example inspect -- <bundle>...`.
//! With no arguments it inspects every bundle in the default plugin folders.

use std::path::PathBuf;

use anyhow::Result;
use vst3_host::{Module, UiThread, default_search_folders, find_bundles, read_module_info};

fn main() -> Result<()> {
    let ui = UiThread::register_current()?;
    let arguments: Vec<PathBuf> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    let bundles = if arguments.is_empty() {
        find_bundles(&default_search_folders())
    } else {
        arguments
    };
    for bundle in bundles {
        println!("{}", bundle.display());
        let has_module_info = read_module_info(&bundle).is_some();
        println!("  moduleinfo.json: {}", if has_module_info { "yes" } else { "no" });
        match Module::load(&ui, &bundle) {
            Ok(module) => {
                println!("  factory vendor: {}", module.factory_info().vendor);
                for class in module.classes() {
                    println!(
                        "  {} | {} | {} | {} | {} | {}",
                        class.id, class.category, class.name, class.vendor, class.version, class.subcategories
                    );
                }
            }
            Err(error) => println!("  error: {:#}", anyhow::Error::new(error)),
        }
    }
    Ok(())
}
