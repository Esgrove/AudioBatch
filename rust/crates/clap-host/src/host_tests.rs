//! Bounded host extension contracts independent of native plugin loading.

use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

use clack_extensions::gui::{GuiSize, HostGuiImpl};
use clack_extensions::params::HostParamsImplShared;
use clack_extensions::timer::{HostTimerImpl, TimerId};
use clack_host::prelude::SharedHandler;

use crate::host::{MainThread, Shared, SharedHandlerData};

#[test]
fn callback_requests_only_publish_independent_atomic_flags() {
    let shared = Arc::new(Shared::new());
    let handler = SharedHandlerData(Arc::clone(&shared));
    handler.request_callback();
    handler.request_process();
    handler.request_restart();
    handler.request_flush();
    assert!(shared.callback.load(Ordering::Acquire));
    assert!(shared.process.load(Ordering::Acquire));
    assert!(shared.flush.load(Ordering::Acquire));
    assert_eq!(shared.restart.load(Ordering::Acquire), 1);
}

#[test]
fn gui_requests_are_bounded_and_only_accept_an_existing_editor() {
    let shared = Arc::new(Shared::new());
    let handler = SharedHandlerData(Arc::clone(&shared));
    assert!(handler.request_show().is_err());
    assert!(handler.request_resize(GuiSize { width: 10, height: 10 }).is_err());
    shared.gui_open.store(true, Ordering::Release);
    assert!(handler.request_resize(GuiSize { width: 0, height: 10 }).is_err());
    assert!(
        handler
            .request_resize(GuiSize {
                width: u32::MAX,
                height: 10
            })
            .is_err()
    );
    handler
        .request_resize(GuiSize {
            width: 800,
            height: 600,
        })
        .expect("bounded size");
    assert_eq!(shared.gui_resize.load(Ordering::Acquire), (800_u64 << 32) | 0x0258);
    handler.request_hide().expect("existing editor");
    assert_eq!(shared.gui_visibility.load(Ordering::Acquire), 2);
}

#[test]
fn timers_receive_unique_ids_and_can_unregister_without_holding_callbacks() {
    let handler = MainThread::new(Arc::new(Shared::new()));
    let first = handler.register_timer(0).expect("zero period clamps");
    let second = handler.register_timer(100).expect("second timer");
    assert_ne!(first, second);
    assert_eq!(
        handler.timers.borrow().first().expect("timer").period,
        Duration::from_millis(1)
    );
    handler.unregister_timer(first).expect("unregister");
    assert!(handler.unregister_timer(first).is_err());
    assert!(handler.unregister_timer(TimerId(u32::MAX)).is_err());
    assert_eq!(handler.timers.borrow().len(), 1);
}

#[test]
fn timer_registration_is_bounded() {
    let handler = MainThread::new(Arc::new(Shared::new()));
    for _ in 0..128 {
        handler.register_timer(10).expect("bounded timer");
    }
    assert!(handler.register_timer(10).is_err());
}
