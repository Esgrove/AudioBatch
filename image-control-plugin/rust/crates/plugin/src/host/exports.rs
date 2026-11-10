//! Native VST3 module exports without global logging, framework registration, or process-wide initialization.

#![expect(unsafe_code, reason = "exports the native VST3 ABI symbols")]

use vst3::Steinberg::IPluginFactory;

/// Transfers an owned factory reference to the native host.
#[unsafe(no_mangle)]
extern "system" fn GetPluginFactory() -> *mut IPluginFactory {
    std::panic::catch_unwind(crate::factory).map_or(std::ptr::null_mut(), vst3::ComPtr::into_raw)
}

/// Accepts a macOS module load without retaining the borrowed bundle.
#[cfg(target_os = "macos")]
#[unsafe(no_mangle)]
const extern "system" fn bundleEntry(_bundle: *mut std::ffi::c_void) -> bool {
    true
}

/// Completes macOS teardown after the host releases its objects.
#[cfg(target_os = "macos")]
#[unsafe(no_mangle)]
const extern "system" fn bundleExit() -> bool {
    true
}

/// Accepts a Windows module load without process-wide initialization.
#[cfg(windows)]
#[unsafe(no_mangle)]
const extern "system" fn InitDll() -> bool {
    true
}

/// Completes Windows teardown after the host releases its objects.
#[cfg(windows)]
#[unsafe(no_mangle)]
const extern "system" fn ExitDll() -> bool {
    true
}
