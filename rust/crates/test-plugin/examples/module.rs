//! The fake plugins as a loadable VST3 library, exporting the entry points of macOS and Windows modules.
//!
//! Two environment variables change how `GetPluginFactory` behaves, for testing how scans handle broken plugins:
//! `AUDIOBATCH_TEST_PLUGIN_CRASH` set to "1" aborts the process,
//! and `AUDIOBATCH_TEST_PLUGIN_SLEEP_MS` waits that many milliseconds first.

#![expect(unsafe_code, reason = "exports the unmangled VST3 entry points")]

use std::time::Duration;

use vst3::Steinberg::IPluginFactory;

/// Environment variable that makes `GetPluginFactory` abort the process.
const CRASH_VARIABLE: &str = "AUDIOBATCH_TEST_PLUGIN_CRASH";

/// Environment variable with a delay before `GetPluginFactory` returns, in milliseconds.
const SLEEP_VARIABLE: &str = "AUDIOBATCH_TEST_PLUGIN_SLEEP_MS";

/// Returns the plugin factory, after the crash or delay the environment asks for.
#[unsafe(no_mangle)]
extern "system" fn GetPluginFactory() -> *mut IPluginFactory {
    if let Some(milliseconds) = std::env::var(SLEEP_VARIABLE).ok().and_then(|value| value.parse().ok()) {
        std::thread::sleep(Duration::from_millis(milliseconds));
    }
    if std::env::var(CRASH_VARIABLE).is_ok_and(|value| value == "1") {
        std::process::abort();
    }
    audiobatch_test_plugin::factory().into_raw()
}

/// Called by macOS hosts after loading the bundle.
#[cfg(target_os = "macos")]
#[unsafe(no_mangle)]
const extern "system" fn bundleEntry(_bundle: *mut std::ffi::c_void) -> bool {
    true
}

/// Called by macOS hosts before unloading the bundle.
#[cfg(target_os = "macos")]
#[unsafe(no_mangle)]
const extern "system" fn bundleExit() -> bool {
    true
}

/// Called by Windows hosts after loading the library.
#[cfg(windows)]
#[unsafe(no_mangle)]
const extern "system" fn InitDll() -> bool {
    true
}

/// Called by Windows hosts before unloading the library.
#[cfg(windows)]
#[unsafe(no_mangle)]
const extern "system" fn ExitDll() -> bool {
    true
}
