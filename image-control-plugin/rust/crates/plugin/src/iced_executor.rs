//! Per-editor Iced futures with explicit cancellation, worker-side destruction, and synchronous joining.
//!
//! The native UI owner retains `Shutdown` outside the Iced program and constructs the window inside `with_executor`.
//! After destroying the window runtime and message endpoints, it calls `Shutdown::shutdown`.
//! No executor is constructed outside that scope, and no process-global worker or task registry exists.
//! Futures must obey the async contract: polling and destruction must not block indefinitely.
//! Subscription sources must release registered wakers during teardown before module unload.
//! A weak scheduling reference prevents cycles, but cannot revoke an externally retained plugin waker vtable.

use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::future::Future;
use std::io;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, Weak};
use std::task::{Context, Poll, Wake, Waker};
use std::thread::{self, JoinHandle};

use iced_baseview::futures::task::{ArcWake, waker_ref};

thread_local! {
    /// Supplies only the current UI window. The scope guard always restores the slot before returning.
    /// ManuallyDrop prevents registering a plugin TLS destructor on a host UI thread that may outlive module unload.
    static CONSTRUCTION: ManuallyDrop<RefCell<Option<Arc<Shared>>>> =
        const { ManuallyDrop::new(RefCell::new(None)) };
}

/// Sole UI-thread join owner, retained until after the associated Iced runtime has been destroyed.
/// Dropping it also cancels and joins, including an early native-window construction failure.
pub struct Shutdown {
    /// Shared scheduling state, never audio-thread state.
    shared: Arc<Shared>,
    /// The editor's only futures worker, consumed by the first shutdown.
    worker: Option<JoinHandle<()>>,
    /// Prevents transferring the join owner into one of its own Send subscription futures.
    owner_thread: PhantomData<Rc<()>>,
}

/// Iced's runtime owns this scheduling handle, not the thread's join responsibility.
pub struct Executor {
    /// Admission closes before cancellation starts, including submissions from future destructors.
    shared: Arc<Shared>,
}

/// Construction guards restore nested window scopes even when a builder unwinds.
struct ConstructionScope {
    /// The previous scope is absent for an ordinary top-level native child construction.
    previous: Option<Arc<Shared>>,
}

/// Mutex-protected admission and queues are used only outside real-time processing.
struct Shared {
    /// The worker checks this between polls so a self-waking task cannot starve cancellation.
    stopping: AtomicBool,
    /// Queues contain pending submissions and at most one wake per scheduled task.
    work: Mutex<Work>,
    /// Wakes the editor's single worker when tasks arrive or shutdown starts.
    available: Condvar,
}

/// Incoming futures move to the worker before being polled or cancelled.
#[derive(Default)]
struct Work {
    /// Monotonic identities keep a stale completed-task waker from waking a replacement task.
    next_id: u64,
    /// Only accepted submissions enter this queue. Rejected futures drop on the submitting thread.
    incoming: VecDeque<Task>,
    /// Wake entries contain identities, not futures or strong task ownership.
    ready: VecDeque<u64>,
}

/// Every live future is exclusively owned and destroyed by the worker after admission.
struct Task {
    /// Wakers hold no future and refer to the scheduling state weakly to avoid ownership cycles.
    wake: Arc<TaskWake>,
    /// Pending subscriptions remain registered even when they never wake again.
    future: Pin<Box<dyn Future<Output = ()> + Send + 'static>>,
}

/// External futures may temporarily retain this waker without retaining a task or worker.
struct TaskWake {
    /// Identifies the task only within its editor's worker registry.
    id: u64,
    /// Prevents subscription receiver wakers from retaining their owning executor.
    shared: Weak<Shared>,
    /// Coalesces repeated wakes until the worker starts the next poll.
    queued: AtomicBool,
}

/// Worker-local ownership also cleans accepted submissions if a subscription poll panics.
struct Registry {
    /// Remains available during unwinding so admission closes before pending futures are destroyed.
    shared: Arc<Shared>,
    /// Futures never escape to wakers or to any process-global pool.
    tasks: HashMap<u64, Task>,
}

/// A stack-scoped blocking executor notification, without a cached thread-local futures executor.
#[derive(Default)]
struct BlockingWake {
    /// Remembers wakeups that happen during a poll or just before waiting.
    notified: Mutex<bool>,
    /// Waits only on the thread explicitly calling Iced's synchronous `block_on` method.
    available: Condvar,
}

impl Shutdown {
    /// Starts exactly one named worker for a new editor, outside audio processing.
    ///
    /// # Errors
    /// Returns the operating system's thread-creation error without creating a detached worker.
    pub fn new() -> io::Result<Self> {
        let shared = Arc::new(Shared {
            stopping: AtomicBool::new(false),
            work: Mutex::new(Work::default()),
            available: Condvar::new(),
        });
        let worker_shared = Arc::clone(&shared);
        let worker = thread::Builder::new()
            .name("ImageControl Iced".into())
            .spawn(move || run(worker_shared))?;
        Ok(Self {
            shared,
            worker: Some(worker),
            owner_thread: PhantomData,
        })
    }

    /// Closes admission, cancels every admitted future on its worker, then joins that worker.
    /// Call on the UI thread after destroying the window's Iced runtime and message endpoints.
    /// Repeated calls succeed without creating another worker or waiting again.
    ///
    /// # Errors
    /// Returns a subscription's panic payload if the worker unwound.
    /// Its registry still cancels accepted tasks on the worker before the join completes.
    pub fn shutdown(&mut self) -> thread::Result<()> {
        {
            let _work = self.shared.lock();
            self.shared.stopping.store(true, Ordering::Release);
            self.shared.available.notify_one();
        }
        self.worker.take().map_or(Ok(()), JoinHandle::join)
    }
}

impl Drop for Shutdown {
    /// Never detaches an editor worker, and does not silently discard a worker panic.
    fn drop(&mut self) {
        if let Err(payload) = self.shutdown() {
            std::panic::resume_unwind(payload);
        }
    }
}

/// Makes Iced's argument-free executor constructor use only this editor's retained shutdown owner.
/// The closure must construct the native child synchronously on this same UI thread.
/// Nested scopes and construction errors restore the previous scope without retaining global tasks.
pub fn with_executor<Result>(controller: &Shutdown, create: impl FnOnce() -> Result) -> Result {
    let previous = CONSTRUCTION.with(|scope| scope.replace(Some(Arc::clone(&controller.shared))));
    let _scope = ConstructionScope { previous };
    create()
}

impl Drop for ConstructionScope {
    /// Restores the outer UI construction context before the shutdown controller can be destroyed.
    fn drop(&mut self) {
        CONSTRUCTION.with(|scope| {
            scope.replace(self.previous.take());
        });
    }
}

impl iced_baseview::Executor for Executor {
    /// Rejects unscoped or already-closed construction instead of creating an unowned thread pool.
    fn new() -> io::Result<Self> {
        let shared = CONSTRUCTION.with(|scope| scope.borrow().clone());
        let Some(shared) = shared else {
            return Err(io::Error::other(
                "Iced executor requires a scoped editor shutdown controller",
            ));
        };
        if shared.stopping.load(Ordering::Acquire) {
            return Err(io::Error::other("the editor's Iced executor is shutting down"));
        }
        Ok(Self { shared })
    }

    /// Admits a cancellable task, or drops the rejected future on the caller without polling it.
    fn spawn(&self, future: impl Future<Output = ()> + Send + 'static) {
        let mut work = self.shared.lock();
        if self.shared.stopping.load(Ordering::Acquire) {
            drop(work);
            return;
        }
        let id = work.next_id;
        work.next_id = id
            .checked_add(1)
            .expect("Iced task identities cannot exhaust u64 within an editor lifetime");
        let wake = Arc::new(TaskWake {
            id,
            shared: Arc::downgrade(&self.shared),
            queued: AtomicBool::new(true),
        });
        work.incoming.push_back(Task {
            wake,
            future: Box::pin(future),
        });
        drop(work);
        self.shared.available.notify_one();
    }

    /// Runs Iced's explicitly synchronous operation on its calling thread without global workers.
    /// Subscription futures use `spawn`, not this method, so they remain cancellable by the owner.
    fn block_on<T>(&self, future: impl Future<Output = T>) -> T {
        let notification = Arc::new(BlockingWake::default());
        let waker = Waker::from(Arc::clone(&notification));
        let mut context = Context::from_waker(&waker);
        let mut future = std::pin::pin!(future);
        loop {
            if let Poll::Ready(result) = future.as_mut().poll(&mut context) {
                return result;
            }
            let mut notified = notification
                .notified
                .lock()
                .expect("blocking Iced notification is not poisoned");
            while !*notified {
                notified = notification
                    .available
                    .wait(notified)
                    .expect("blocking Iced notification is not poisoned");
            }
            *notified = false;
        }
    }
}

impl Shared {
    /// Locks queue bookkeeping only. No future is polled or destroyed while this mutex is held.
    fn lock(&self) -> MutexGuard<'_, Work> {
        self.work
            .lock()
            .expect("Iced queue bookkeeping does not execute subscription code")
    }
}

impl ArcWake for TaskWake {
    /// Coalesces a wake without retaining its future, and never queues work once shutdown starts.
    fn wake_by_ref(task: &Arc<Self>) {
        if task.queued.swap(true, Ordering::AcqRel) {
            return;
        }
        let Some(shared) = task.shared.upgrade() else {
            return;
        };
        let mut work = shared.lock();
        if shared.stopping.load(Ordering::Acquire) {
            drop(work);
            return;
        }
        work.ready.push_back(task.id);
        drop(work);
        shared.available.notify_one();
    }
}

impl Drop for Registry {
    /// Closes admission even on unwind, then destroys queued and registered futures on the worker.
    /// Future destructors can wake or submit again without reentering a held queue mutex.
    fn drop(&mut self) {
        let incoming = {
            let mut work = self.shared.lock();
            self.shared.stopping.store(true, Ordering::Release);
            work.ready.clear();
            std::mem::take(&mut work.incoming)
        };
        drop(incoming);
        self.tasks.clear();
    }
}

impl Wake for BlockingWake {
    /// Implements owned wake by using the same remembered notification as borrowed wake.
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    /// Preserves notifications during synchronous polling without starting a helper thread.
    fn wake_by_ref(self: &Arc<Self>) {
        *self
            .notified
            .lock()
            .expect("blocking Iced notification is not poisoned") = true;
        self.available.notify_one();
    }
}

/// Polls only this editor's tasks and destroys all pending subscriptions before the thread returns.
/// A stop check between each poll prevents ready-queue pressure from starving cancellation.
fn run(shared: Arc<Shared>) {
    let mut registry = Registry {
        shared,
        tasks: HashMap::new(),
    };
    loop {
        let (incoming, mut ready) = {
            let mut work = registry.shared.lock();
            while !registry.shared.stopping.load(Ordering::Acquire) && work.incoming.is_empty() && work.ready.is_empty()
            {
                work = registry
                    .shared
                    .available
                    .wait(work)
                    .expect("Iced queue bookkeeping is not poisoned");
            }
            if registry.shared.stopping.load(Ordering::Acquire) {
                return;
            }
            (std::mem::take(&mut work.incoming), std::mem::take(&mut work.ready))
        };
        for task in incoming {
            ready.push_back(task.wake.id);
            registry.tasks.insert(task.wake.id, task);
        }
        for id in ready {
            if registry.shared.stopping.load(Ordering::Acquire) {
                return;
            }
            let Some(task) = registry.tasks.get_mut(&id) else {
                continue;
            };
            task.wake.queued.store(false, Ordering::Release);
            let waker = waker_ref(&task.wake);
            let mut context = Context::from_waker(&waker);
            if task.future.as_mut().poll(&mut context).is_ready() {
                registry.tasks.remove(&id);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::mpsc::{self, Receiver, Sender};
    use std::time::Duration;

    use iced_baseview::Executor as _;
    use iced_baseview::futures::{FutureExt as _, future};

    use super::*;

    /// Bounds coordination failures without timing assumptions or test sleeps.
    const WAIT: Duration = Duration::from_secs(5);

    /// Reports the thread that actually destroys a task-owned resource.
    struct DropProbe(Sender<thread::ThreadId>);

    impl Drop for DropProbe {
        /// A queued task's probe must be dropped before its worker's join completes.
        fn drop(&mut self) {
            self.0
                .send(thread::current().id())
                .expect("probe observer remains alive");
        }
    }

    /// Waits for one deterministic worker observation without an unbounded channel receive.
    fn receive<T>(receiver: &Receiver<T>) -> T {
        receiver
            .recv_timeout(WAIT)
            .expect("worker responds within the bounded test interval")
    }

    /// Constructs a real trait executor only through its editor's synchronous construction scope.
    fn executor(controller: &Shutdown) -> Executor {
        with_executor(controller, || Executor::new().expect("scoped executor"))
    }

    /// Submits a never-completing subscription and waits until its first worker poll.
    fn pending_subscription(executor: &Executor) -> Receiver<thread::ThreadId> {
        let (polled, observed) = mpsc::channel();
        let (dropped, destruction) = mpsc::channel();
        let probe = DropProbe(dropped);
        executor.spawn(async move {
            let _probe = probe;
            polled
                .send(thread::current().id())
                .expect("poll observer remains alive");
            future::pending::<()>().await;
        });
        let worker = receive(&observed);
        assert_ne!(worker, thread::current().id());
        destruction
    }

    #[test]
    fn construction_slot_registers_no_host_thread_destructor() {
        assert!(!std::mem::needs_drop::<ManuallyDrop<RefCell<Option<Arc<Shared>>>>>());
    }

    #[test]
    fn construction_requires_a_scope_and_restores_nested_scopes() {
        assert!(Executor::new().is_err());
        let mut outer = Shutdown::new().expect("outer worker");
        let mut inner = Shutdown::new().expect("inner worker");
        with_executor(&outer, || {
            let first = Executor::new().expect("outer scope");
            with_executor(&inner, || {
                let second = Executor::new().expect("inner scope");
                assert!(Arc::ptr_eq(&second.shared, &inner.shared));
                assert!(!Arc::ptr_eq(&second.shared, &first.shared));
            });
            let restored = Executor::new().expect("restored outer scope");
            assert!(Arc::ptr_eq(&restored.shared, &outer.shared));
        });
        assert!(Executor::new().is_err());
        inner.shutdown().expect("inner joins");
        outer.shutdown().expect("outer joins");
        assert!(with_executor(&outer, Executor::new).is_err());
    }

    #[test]
    fn construction_scope_is_restored_when_the_builder_unwinds() {
        let mut controller = Shutdown::new().expect("worker");
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            with_executor(&controller, || std::panic::resume_unwind(Box::new("builder failed")));
        }));
        assert!(result.is_err());
        assert!(Executor::new().is_err());
        controller.shutdown().expect("worker joins after a construction error");
    }

    #[test]
    fn runtime_drop_does_not_detach_and_shutdown_cancels_on_the_worker() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let destruction = pending_subscription(&executor);
        drop(executor);
        controller.shutdown().expect("worker joins");
        assert_ne!(receive(&destruction), thread::current().id());
        assert!(controller.worker.is_none());
        assert!(controller.shared.lock().incoming.is_empty());
        assert!(controller.shared.lock().ready.is_empty());
        controller.shutdown().expect("repeated shutdown is harmless");
    }

    #[test]
    fn queued_unpolled_subscriptions_are_cancelled_on_the_worker() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let (started, observed) = mpsc::channel();
        let (resume, resumed) = mpsc::channel();
        executor.spawn(async move {
            started
                .send(thread::current().id())
                .expect("poll observer remains alive");
            // This deliberately paused test poll establishes an exact queued-but-unpolled interleaving.
            resumed
                .recv_timeout(WAIT)
                .expect("the UI releases the test poll before joining");
        });
        let worker = receive(&observed);
        let (dropped, destruction) = mpsc::channel();
        let probe = DropProbe(dropped);
        let polled = Arc::new(AtomicBool::new(false));
        let task_polled = Arc::clone(&polled);
        executor.spawn(async move {
            let _probe = probe;
            task_polled.store(true, Ordering::Release);
            future::pending::<()>().await;
        });
        {
            let _work = controller.shared.lock();
            controller.shared.stopping.store(true, Ordering::Release);
        }
        resume.send(()).expect("test poll is waiting for release");
        controller
            .shutdown()
            .expect("worker joins with an unpolled queued task");
        assert_eq!(receive(&destruction), worker);
        assert!(!polled.load(Ordering::Acquire));
        assert!(controller.shared.lock().incoming.is_empty());
    }

    #[test]
    fn every_subscription_for_one_editor_uses_the_same_single_worker() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let (polled, observed) = mpsc::channel();
        let (dropped, destruction) = mpsc::channel();
        for _ in 0..32 {
            let polled = polled.clone();
            let probe = DropProbe(dropped.clone());
            executor.spawn(async move {
                let _probe = probe;
                polled
                    .send(thread::current().id())
                    .expect("poll observer remains alive");
                future::pending::<()>().await;
            });
        }
        let worker = receive(&observed);
        for _ in 1..32 {
            assert_eq!(receive(&observed), worker);
        }
        controller.shutdown().expect("one worker joins all subscriptions");
        for _ in 0..32 {
            assert_eq!(receive(&destruction), worker);
        }
    }

    #[test]
    fn completed_tasks_are_destroyed_on_the_worker_before_editor_shutdown() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let (dropped, destruction) = mpsc::channel();
        let probe = DropProbe(dropped);
        executor.spawn(async move {
            drop(probe);
        });
        assert_ne!(receive(&destruction), thread::current().id());
        controller.shutdown().expect("worker joins");
    }

    #[test]
    fn several_editors_cancel_and_join_independently_without_global_workers() {
        let mut first = Shutdown::new().expect("first worker");
        let mut second = Shutdown::new().expect("second worker");
        let first_executor = executor(&first);
        let second_executor = executor(&second);
        let first_destruction = pending_subscription(&first_executor);
        let second_destruction = pending_subscription(&second_executor);
        first.shutdown().expect("first joins");
        let first_thread = receive(&first_destruction);
        assert!(matches!(second_destruction.try_recv(), Err(mpsc::TryRecvError::Empty)));
        second.shutdown().expect("second joins");
        assert_ne!(first_thread, receive(&second_destruction));
        for _ in 0..8 {
            let controller = Shutdown::new().expect("replacement worker");
            let executor = executor(&controller);
            let destruction = pending_subscription(&executor);
            let shared = Arc::downgrade(&controller.shared);
            drop(executor);
            drop(controller);
            assert_ne!(receive(&destruction), thread::current().id());
            assert!(shared.upgrade().is_none());
        }
    }

    #[test]
    fn submissions_after_shutdown_drop_unpolled_on_the_ui_caller() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        controller.shutdown().expect("worker joins");
        let (dropped, destruction) = mpsc::channel();
        let probe = DropProbe(dropped);
        let polled = Arc::new(AtomicBool::new(false));
        let observed = Arc::clone(&polled);
        executor.spawn(async move {
            observed.store(true, Ordering::Release);
            drop(probe);
        });
        assert_eq!(receive(&destruction), thread::current().id());
        assert!(!polled.load(Ordering::Acquire));
    }

    /// Exercises reentrant submission from the destructor of a cancelled subscription.
    struct SpawnOnDrop {
        /// Retains only a scheduling handle, never the non-Send shutdown owner.
        executor: Executor,
        /// Reports destruction of the rejected nested task on the cancelling worker.
        dropped: Sender<thread::ThreadId>,
    }

    impl Drop for SpawnOnDrop {
        /// Shutdown must reject this nested task without a held queue lock or another poll.
        fn drop(&mut self) {
            let probe = DropProbe(self.dropped.clone());
            self.executor.spawn(async move {
                future::pending::<()>().await;
                drop(probe);
            });
        }
    }

    #[test]
    fn cancellation_can_submit_and_wake_without_deadlock_or_retaining_futures() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let (dropped, destruction) = mpsc::channel();
        let (polled, observed) = mpsc::channel();
        let reentrant = SpawnOnDrop {
            executor: Executor {
                shared: Arc::clone(&executor.shared),
            },
            dropped,
        };
        executor.spawn(async move {
            let _reentrant = reentrant;
            let mut polled = Some(polled);
            future::poll_fn(|context| {
                context.waker().wake_by_ref();
                if let Some(polled) = polled.take() {
                    polled
                        .send(thread::current().id())
                        .expect("poll observer remains alive");
                }
                Poll::<()>::Pending
            })
            .await;
        });
        let worker = receive(&observed);
        controller.shutdown().expect("self-waking worker joins");
        assert_eq!(receive(&destruction), worker);
        assert!(controller.shared.lock().incoming.is_empty());
        assert!(controller.shared.lock().ready.is_empty());
    }

    #[test]
    fn retained_wakers_do_not_retain_futures_or_the_executor_state() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let shared = Arc::downgrade(&controller.shared);
        let (wakers, observed) = mpsc::channel();
        let (dropped, destruction) = mpsc::channel();
        let probe = DropProbe(dropped);
        executor.spawn(async move {
            let _probe = probe;
            future::poll_fn(|context| {
                wakers
                    .send(context.waker().clone())
                    .expect("waker observer remains alive");
                Poll::<()>::Pending
            })
            .await;
        });
        let waker = receive(&observed);
        controller.shutdown().expect("worker joins despite the external waker");
        assert_ne!(receive(&destruction), thread::current().id());
        waker.wake_by_ref();
        assert!(controller.shared.lock().ready.is_empty());
        drop(executor);
        drop(controller);
        assert!(shared.upgrade().is_none());
        waker.wake_by_ref();
    }

    #[test]
    fn scoped_block_on_preserves_wakes_without_a_global_executor() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let mut first = true;
        let result = executor.block_on(future::poll_fn(|context| {
            if std::mem::take(&mut first) {
                context.waker().wake_by_ref();
                Poll::Pending
            } else {
                Poll::Ready(42)
            }
        }));
        assert_eq!(result, 42);
        controller.shutdown().expect("worker joins");
    }

    #[test]
    fn worker_panic_closes_admission_and_cancels_accepted_subscriptions() {
        let mut controller = Shutdown::new().expect("worker");
        let executor = executor(&controller);
        let destruction = pending_subscription(&executor);
        let (failed, observed) = mpsc::channel();
        executor.spawn(async move {
            failed.send(()).expect("panic observer remains alive");
            std::panic::resume_unwind(Box::new("subscription failed"));
        });
        receive(&observed);
        let result = controller.shutdown();
        assert!(result.is_err());
        assert_ne!(receive(&destruction), thread::current().id());
        assert!(controller.shared.stopping.load(Ordering::Acquire));
        assert!(controller.shared.lock().incoming.is_empty());
        let (dropped, destruction) = mpsc::channel();
        let probe = DropProbe(dropped);
        executor.spawn(future::pending::<()>().map(move |()| drop(probe)));
        assert_eq!(receive(&destruction), thread::current().id());
    }
}
