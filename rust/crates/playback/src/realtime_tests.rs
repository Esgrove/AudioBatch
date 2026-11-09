//! Process-isolated native `RTSan` controls, enabled only by the explicit nightly sanitizer configuration.

#![expect(
    unsafe_code,
    reason = "positive controls exercise native allocation and deallocation interceptors"
)]

use std::ffi::c_void;
use std::hint::black_box;
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::sync::Mutex;

unsafe extern "C" {
    /// Allocates native storage for the interceptor control.
    fn malloc(size: usize) -> *mut c_void;
    /// Frees storage obtained from the native allocator.
    fn free(pointer: *mut c_void);
}

/// Runs an allocation-free region and returns an observable result.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn clean_region(value: usize) -> usize {
    black_box(usize::wrapping_add as fn(usize, usize) -> usize)(value, 1)
}

/// Allocates observable Rust-owned storage inside a marked region.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn rust_allocation_region() -> Box<[u8; 4096]> {
    black_box(Box::new([black_box(7); 4096]))
}

/// Destroys preallocated Rust-owned storage inside a marked region.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn rust_free_region(value: Box<[u8; 4096]>) {
    drop(black_box(value));
}

/// Allocates through libc without relying on Rust allocation lowering.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn native_allocation_region() -> *mut c_void {
    // SAFETY: malloc accepts any byte count, and the returned pointer is only consumed outside this region.
    black_box(unsafe { malloc(black_box(4096)) })
}

/// Locks a preinitialized native-backed mutex inside a marked region.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn mutex_region(mutex: &Mutex<usize>) {
    *mutex.lock().expect("unpoisoned control") += black_box(1);
}

/// Nests native compiler scopes without adding any test-owned allocation.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn nested_region() -> usize {
    clean_region(41)
}

/// Verifies that returning from an inner scope does not unmark its nonblocking caller.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn nested_allocation_region() -> Box<[u8; 4096]> {
    black_box(clean_region(41));
    black_box(Box::new([black_box(7); 4096]))
}

/// Unwinds an already allocated payload without invoking the panic formatter.
/// Rust still allocates its native exception object, which the fatal detector control must observe.
/// Nightly 1.102.0 does not clear its nonblocking state on this unwind, so this is not a clean cleanup claim.
#[sanitize(realtime = "nonblocking")]
#[inline(never)]
fn unwind_region(payload: Box<dyn std::any::Any + Send>) {
    resume_unwind(payload);
}

#[test]
fn clean_control() {
    assert_eq!(clean_region(41), 42);
    drop(black_box(Box::new([1_u8; 4096])));
}

#[test]
fn rust_allocation_control() {
    drop(rust_allocation_region());
}

#[test]
fn rust_free_control() {
    let value = black_box(Box::new([7; 4096]));
    rust_free_region(value);
}

#[test]
fn native_allocation_control() {
    let pointer = native_allocation_region();
    assert!(!pointer.is_null());
    // SAFETY: this pointer was returned by malloc above and has not yet been freed.
    unsafe { free(pointer) };
}

#[test]
fn mutex_control() {
    let mutex = Mutex::new(0);
    // The control isolates lock interception from Rust's lazy allocation of native mutex storage.
    drop(
        mutex
            .lock()
            .expect("initialize native mutex storage outside the measured region"),
    );
    mutex_region(&mutex);
}

#[test]
fn nested_scope_cleanup() {
    assert_eq!(nested_region(), 42);
    drop(black_box(Box::new([1_u8; 4096])));
}

#[test]
fn nested_allocation_control() {
    drop(nested_allocation_region());
}

#[test]
fn unwind_outside_scope_cleanup() {
    let payload: Box<dyn std::any::Any + Send> = Box::new(42_usize);
    let result = catch_unwind(AssertUnwindSafe(|| {
        assert_eq!(nested_region(), 42);
        resume_unwind(payload);
    }));
    assert!(result.is_err());
    drop(result);
    drop(black_box(Box::new([1_u8; 4096])));
}

#[test]
fn unwind_scope_baseline() {
    let payload: Box<dyn std::any::Any + Send> = Box::new(42_usize);
    let result = catch_unwind(AssertUnwindSafe(|| unwind_region(payload)));
    assert!(result.is_err());
    drop(result);
    drop(black_box(Box::new([1_u8; 4096])));
}
