//! The UI thread rule of VST3 and the job queue that brings work onto that thread.
//!
//! VST3 requires most calls on a plugin to come from the UI thread:
//! creating and terminating it, setting up its buses and processing, activating it, its state, and its views.
//! Only `process` and `setProcessing` may run on a processing thread.
//! `UiThread` is a token that exists only on the registered UI thread, and it is not `Send`,
//! so every lifecycle method that takes `&UiThread` can only be called there.
//! `UiDispatcher` lets other threads run jobs on the UI thread,
//! and `UiJobs` is the receiving end that the UI thread drains,
//! from the GUI's foreground executor or from a plain loop in a process without a GUI.

use std::marker::PhantomData;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, OnceLock};
use std::thread::{self, ThreadId};
use std::time::Duration;

use crossbeam_queue::ArrayQueue;
use thiserror::Error;

/// How often a blocked `UiDispatcher::call` checks whether its caller was cancelled.
const CANCELLATION_POLL_INTERVAL: Duration = Duration::from_millis(20);

/// State of a call job that has not started yet.
const CALL_PENDING: u8 = 0;

/// State of a call job that the UI thread has started.
const CALL_STARTED: u8 = 1;

/// State of a call job whose caller gave up before it started.
const CALL_ABANDONED: u8 = 2;

/// The thread registered as the UI thread, set once per process.
static UI_THREAD: OnceLock<ThreadId> = OnceLock::new();

/// Why `UiDispatcher::call` returned without a result.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum CallError {
    /// The caller was cancelled before the UI thread started the job, so the job will not run.
    #[error("The call was cancelled before it started")]
    Cancelled,
    /// The UI thread no longer receives jobs.
    #[error("The UI thread no longer runs plugin jobs")]
    Closed,
}

/// Why the UI thread could not be registered or started.
#[derive(Debug, Error)]
pub enum UiThreadError {
    /// A different thread already owns the UI thread token.
    #[error("Another thread is already registered as the VST3 UI thread")]
    AlreadyRegistered,
    /// The operating system could not start the thread.
    #[error(transparent)]
    Spawn(#[from] std::io::Error),
    /// The thread exited before reporting whether registration succeeded.
    #[error(transparent)]
    Startup(#[from] std::sync::mpsc::RecvError),
}

/// Proof that the current thread is the UI thread.
///
/// It cannot be sent to or shared with another thread,
/// so a function taking `&UiThread` can only be called on the UI thread.
#[derive(Debug)]
pub struct UiThread {
    /// Makes the token neither `Send` nor `Sync`.
    not_send: PhantomData<*const ()>,
}

/// Runs jobs on the UI thread from any thread.
#[derive(Clone, Debug)]
pub struct UiDispatcher {
    /// Sends jobs to the `UiJobs` the UI thread drains.
    sender: async_channel::Sender<UiJob>,
}

/// The UI thread's end of the job queue.
#[derive(Debug)]
pub struct UiJobs {
    /// Receives the jobs that dispatchers send.
    receiver: async_channel::Receiver<UiJob>,
}

/// A job to run on the UI thread.
pub struct UiJob {
    /// The work, given the UI thread token.
    work: Box<dyn FnOnce(&UiThread) + Send>,
}

impl UiThread {
    /// Registers the current thread as the UI thread and returns its token.
    ///
    /// Call it once at startup, on the main thread in a GUI.
    /// Registering the same thread again returns another token.
    ///
    /// # Errors
    ///
    /// Returns an error when another thread is already registered.
    pub fn register_current() -> Result<Self, UiThreadError> {
        let current = thread::current().id();
        let registered = *UI_THREAD.get_or_init(|| current);
        if registered != current {
            return Err(UiThreadError::AlreadyRegistered);
        }
        Ok(Self { not_send: PhantomData })
    }

    /// Returns the token when the current thread is the registered UI thread.
    #[must_use]
    pub fn current() -> Option<Self> {
        Self::is_current().then_some(Self { not_send: PhantomData })
    }

    /// Returns true when the current thread is the registered UI thread.
    #[must_use]
    pub fn is_current() -> bool {
        UI_THREAD
            .get()
            .is_some_and(|registered| *registered == thread::current().id())
    }

    /// Starts a dedicated thread as the UI thread, for hosts and tests without a GUI,
    /// and returns the dispatcher that runs jobs on it.
    ///
    /// The thread runs until every dispatcher is dropped.
    ///
    /// # Errors
    ///
    /// Returns an error when another thread is already registered or the thread cannot be started.
    pub fn spawn_dedicated() -> Result<UiDispatcher, UiThreadError> {
        let (dispatcher, jobs) = ui_channel();
        let (started_sender, started_receiver) = std::sync::mpsc::channel();
        thread::Builder::new()
            .name(String::from("vst3-ui"))
            .spawn(move || match Self::register_current() {
                Ok(ui) => {
                    let _ = started_sender.send(Ok(()));
                    jobs.run_until_closed(&ui);
                }
                Err(error) => {
                    let _ = started_sender.send(Err(error));
                }
            })?;
        started_receiver.recv()??;
        Ok(dispatcher)
    }
}

impl UiDispatcher {
    /// Queues a job for the UI thread without waiting for it.
    ///
    /// Returns false when the UI thread no longer receives jobs, in which case the job is dropped unrun.
    pub fn post(&self, job: impl FnOnce(&UiThread) + Send + 'static) -> bool {
        self.sender.try_send(UiJob { work: Box::new(job) }).is_ok()
    }

    /// Runs a job on the UI thread and waits for its result.
    ///
    /// On the UI thread itself the job runs at once.
    /// While waiting, `is_cancelled` is polled, and a job that has not started when it returns true is abandoned,
    /// so the UI thread can wait for its workers after cancelling them without a deadlock.
    /// A job that already started is always waited for.
    ///
    /// # Errors
    ///
    /// Returns `CallError::Cancelled` when the job was abandoned,
    /// and `CallError::Closed` when the UI thread no longer receives jobs.
    pub fn call<R: Send + 'static>(
        &self,
        job: impl FnOnce(&UiThread) -> R + Send + 'static,
        is_cancelled: &dyn Fn() -> bool,
    ) -> Result<R, CallError> {
        if let Some(ui) = UiThread::current() {
            return Ok(job(&ui));
        }
        let state = Arc::new(AtomicU8::new(CALL_PENDING));
        let result = Arc::new(ArrayQueue::new(1));
        let job_state = Arc::clone(&state);
        let job_result = Arc::clone(&result);
        // The waiting thread is woken as soon as the result is in, instead of at its next poll.
        let caller = thread::current();
        let posted = self.post(move |ui| {
            if job_state
                .compare_exchange(CALL_PENDING, CALL_STARTED, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
            {
                let _ = job_result.push(job(ui));
                caller.unpark();
            }
        });
        if !posted {
            return Err(CallError::Closed);
        }
        loop {
            if let Some(value) = result.pop() {
                return Ok(value);
            }
            if self.sender.is_closed() && state.load(Ordering::Acquire) == CALL_PENDING {
                return Err(CallError::Closed);
            }
            if is_cancelled()
                && state
                    .compare_exchange(CALL_PENDING, CALL_ABANDONED, Ordering::AcqRel, Ordering::Acquire)
                    .is_ok()
            {
                return Err(CallError::Cancelled);
            }
            if state.load(Ordering::Acquire) == CALL_STARTED && Arc::strong_count(&result) == 1 && result.is_empty() {
                // The job started but was dropped without a result, because it panicked on the UI thread.
                return Err(CallError::Closed);
            }
            thread::park_timeout(CANCELLATION_POLL_INTERVAL);
        }
    }
}

impl UiJobs {
    /// Waits for the next job, or returns `None` once every dispatcher is dropped.
    pub async fn recv(&self) -> Option<UiJob> {
        self.receiver.recv().await.ok()
    }

    /// Runs jobs until every dispatcher is dropped.
    pub(crate) fn run_until_closed(&self, ui: &UiThread) {
        while let Ok(job) = self.receiver.recv_blocking() {
            job.run(ui);
        }
    }
}

impl UiJob {
    /// Runs the job on the UI thread.
    pub fn run(self, ui: &UiThread) {
        (self.work)(ui);
    }
}

impl std::fmt::Debug for UiJob {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_struct("UiJob").finish_non_exhaustive()
    }
}

/// Creates a connected dispatcher and job queue.
#[must_use]
pub fn ui_channel() -> (UiDispatcher, UiJobs) {
    let (sender, receiver) = async_channel::unbounded();
    (UiDispatcher { sender }, UiJobs { receiver })
}

#[cfg(test)]
mod test_dispatcher {
    use std::error::Error as _;
    use std::sync::atomic::AtomicBool;

    use super::*;
    use crate::test_helpers::dispatcher;

    #[test]
    fn call_returns_the_result_from_the_ui_thread() {
        let on_ui_thread = dispatcher()
            .call(|_| UiThread::is_current(), &|| false)
            .expect("call result");
        assert!(on_ui_thread);
        assert!(!UiThread::is_current());
        assert!(UiThread::current().is_none());
    }

    #[test]
    fn cancelled_call_abandons_a_job_that_has_not_started() {
        let (dispatcher, _jobs) = ui_channel();
        let ran = Arc::new(AtomicBool::new(false));
        let job_ran = Arc::clone(&ran);
        let result = dispatcher.call(move |_| job_ran.store(true, Ordering::SeqCst), &|| true);
        assert_eq!(result, Err(CallError::Cancelled));
        assert!(!ran.load(Ordering::SeqCst));
    }

    #[test]
    fn call_fails_when_the_queue_is_closed() {
        let (dispatcher, jobs) = ui_channel();
        drop(jobs);
        assert_eq!(dispatcher.call(|_| 1, &|| false), Err(CallError::Closed));
        assert!(!dispatcher.post(|_| {}));
    }

    #[test]
    fn post_runs_jobs_in_order() {
        let (sender, receiver) = std::sync::mpsc::channel();
        for index in 0..3 {
            let job_sender = sender.clone();
            assert!(dispatcher().post(move |_| {
                let _ = job_sender.send(index);
            }));
        }
        let order: Vec<i32> = (0..3).map(|_| receiver.recv().expect("job ran")).collect();
        assert_eq!(order, vec![0, 1, 2]);
    }

    #[test]
    fn dispatcher_errors_stay_small_and_keep_their_messages() {
        assert_eq!(size_of::<CallError>(), size_of::<u8>());
        assert_eq!(
            CallError::Cancelled.to_string(),
            "The call was cancelled before it started"
        );
        assert_eq!(
            CallError::Closed.to_string(),
            "The UI thread no longer runs plugin jobs"
        );
        assert!(CallError::Closed.source().is_none());
    }

    #[test]
    fn another_thread_cannot_register_as_the_ui_thread() {
        let _dispatcher = dispatcher();
        let error = UiThread::register_current().expect_err("the dedicated thread is registered");
        assert!(matches!(error, UiThreadError::AlreadyRegistered));
        assert_eq!(
            error.to_string(),
            "Another thread is already registered as the VST3 UI thread"
        );
    }
}
