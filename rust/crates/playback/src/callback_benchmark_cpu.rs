//! Native CPU accounting for the ignored callback benchmark, sampled outside callback timing.

#![expect(unsafe_code, reason = "test-only native thread and process CPU accounting")]

#[cfg(target_os = "macos")]
use std::ffi::c_int;
#[cfg(windows)]
use std::ffi::c_void;

/// CPU time used by the callback thread and by all threads of its process.
#[derive(Clone, Copy)]
pub struct CpuTimes {
    pub thread_ns: u64,
    pub process_ns: u64,
}

/// Owns the native identity of the current thread, never shared with another thread.
pub struct CpuClock {
    #[cfg(target_os = "macos")]
    port: u32,
    #[cfg(windows)]
    thread: *mut c_void,
}

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn mach_thread_self() -> u32;
    fn thread_info(thread: u32, flavor: c_int, information: *mut c_int, count: *mut u32) -> c_int;
    fn mach_port_deallocate(task: u32, name: u32) -> c_int;
    static mach_task_self_: u32;
    fn clock() -> i64;
}

#[cfg(windows)]
#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentThread() -> *mut c_void;
    fn GetCurrentProcess() -> *mut c_void;
    fn GetThreadTimes(handle: *mut c_void, creation: *mut u64, exit: *mut u64, kernel: *mut u64, user: *mut u64)
    -> i32;
    fn GetProcessTimes(
        handle: *mut c_void,
        creation: *mut u64,
        exit: *mut u64,
        kernel: *mut u64,
        user: *mut u64,
    ) -> i32;
}

impl CpuClock {
    /// Captures the callback thread's identity before measuring any work.
    pub fn new() -> Self {
        Self {
            #[cfg(target_os = "macos")]
            // SAFETY: this returns a send right for the current thread, released by Drop on this thread.
            port: unsafe { mach_thread_self() },
            #[cfg(windows)]
            // SAFETY: the pseudo handle refers to the calling thread and needs no release.
            thread: unsafe { GetCurrentThread() },
        }
    }

    /// Reads cumulative native CPU counters, with no reads inside an individual callback interval.
    pub fn read(&self) -> CpuTimes {
        #[cfg(target_os = "macos")]
        {
            const THREAD_BASIC_INFO: c_int = 3;
            const THREAD_BASIC_INFO_COUNT: u32 = 10;
            const CLOCK_TICKS_PER_SECOND: u64 = 1_000_000;
            let mut information = [0_i32; THREAD_BASIC_INFO_COUNT as usize];
            let mut count = THREAD_BASIC_INFO_COUNT;
            // SAFETY: THREAD_BASIC_INFO writes ten integer words into the correctly sized live buffer.
            let result = unsafe { thread_info(self.port, THREAD_BASIC_INFO, information.as_mut_ptr(), &raw mut count) };
            assert_eq!(result, 0, "thread_info failed");
            let [user_seconds, user_microseconds, system_seconds, system_microseconds, ..] = information;
            let thread_ns = (u64::try_from(user_seconds + system_seconds).expect("nonnegative CPU seconds")
                * 1_000_000
                + u64::try_from(user_microseconds + system_microseconds).expect("nonnegative CPU microseconds"))
                * 1000;
            // SAFETY: clock takes no pointers and reports cumulative process CPU in Darwin microsecond ticks.
            let process_ticks = unsafe { clock() };
            CpuTimes {
                thread_ns,
                process_ns: u64::try_from(process_ticks).expect("process clock available")
                    * (1_000_000_000 / CLOCK_TICKS_PER_SECOND),
            }
        }
        #[cfg(windows)]
        {
            let (mut creation, mut exit, mut kernel, mut user) = (0_u64, 0_u64, 0_u64, 0_u64);
            assert_ne!(
                // SAFETY: the thread handle is live and every output points to an eight-byte FILETIME buffer.
                unsafe {
                    GetThreadTimes(
                        self.thread,
                        &raw mut creation,
                        &raw mut exit,
                        &raw mut kernel,
                        &raw mut user,
                    )
                },
                0
            );
            let thread_ns = (kernel + user) * 100;
            assert_ne!(
                // SAFETY: the process pseudo handle is valid and the writable FILETIME buffers remain live.
                unsafe {
                    GetProcessTimes(
                        GetCurrentProcess(),
                        &raw mut creation,
                        &raw mut exit,
                        &raw mut kernel,
                        &raw mut user,
                    )
                },
                0
            );
            CpuTimes {
                thread_ns,
                process_ns: (kernel + user) * 100,
            }
        }
    }
}

#[cfg(target_os = "macos")]
impl Drop for CpuClock {
    fn drop(&mut self) {
        // SAFETY: the task owns the send right returned by mach_thread_self exactly until this call.
        let result = unsafe { mach_port_deallocate(mach_task_self_, self.port) };
        assert_eq!(result, 0, "thread port release failed");
    }
}
