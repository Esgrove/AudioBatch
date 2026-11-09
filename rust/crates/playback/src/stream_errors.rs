//! Stream error reporting and its complete callback ownership boundary.
//!
//! Native errors retain their categories and complete messages until the player's event poll formats them.
//! The bounded channel keeps the earliest errors and drops later errors when full.
//! Stream-loss handling does not depend on whether its diagnostic can be queued.
//! Ignored errors and errors returned by a full or disconnected channel are destroyed inside `StreamErrors::report`.
//! CPAL errors can own their message storage, so those paths can deallocate on the callback thread.
//! Bounded storage cannot retain unrestricted incoming owned errors without eventually freeing, allocating, or waiting.
//! Forgetting those errors instead would leak their message storage.
//! An inline diagnostic copy would not remove the incoming error's destructor and could truncate its message.
//!
//! The marked boundary audits this limitation rather than claiming that the transport is realtime-safe.
//! Crossbeam's bounded send can retry atomic operations and can lock to notify registered waiting receivers.
//! The player polls without registering a waiting receiver, but that does not establish a wait-free send contract.
//! Upstream native error construction and callback synchronization remain outside this first-party boundary.

use std::sync::Arc;
use std::sync::atomic::Ordering;

use cpal::ErrorKind;
use crossbeam_channel::Sender;

use crate::is_stream_lost;
use crate::output::SharedState;

/// State captured by a stream's error callback, which may run on the audio host's realtime thread.
/// The player retains the channel receiver and formats successfully queued errors on its owning thread.
pub struct StreamErrors {
    /// Publishes stream loss even when its diagnostic cannot be queued.
    pub(crate) shared: Arc<SharedState>,
    /// Preserves complete native errors until the player polls, subject to the bounded overflow policy.
    pub(crate) sender: Sender<cpal::Error>,
}

impl StreamErrors {
    /// Handles one native callback error, including destruction when it is filtered or cannot be queued.
    /// Owned message destruction on those paths is a known realtime blocker, not deferred cleanup.
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
    #[cfg_attr(audiobatch_rtsan, inline(never))]
    pub(crate) fn report(&self, error: cpal::Error) {
        if matches!(
            error.kind(),
            ErrorKind::Xrun | ErrorKind::DeviceChanged | ErrorKind::RealtimeDenied
        ) {
            return;
        }
        if is_stream_lost(error.kind()) {
            self.shared.stream_lost.store(true, Ordering::Release);
            self.shared.playing.store(false, Ordering::Relaxed);
        }
        let _ = self.sender.try_send(error);
    }
}

#[cfg(test)]
mod test_reporting {
    use crossbeam_channel::{Receiver, TryRecvError, bounded};

    use super::*;
    use crate::STREAM_ERROR_CAPACITY;

    /// Creates an isolated reporting boundary without opening an audio device or starting a worker.
    fn reporter() -> (StreamErrors, Receiver<cpal::Error>) {
        let shared = Arc::new(SharedState::new());
        shared.playing.store(true, Ordering::Relaxed);
        let (sender, receiver) = bounded(STREAM_ERROR_CAPACITY);
        (StreamErrors { shared, sender }, receiver)
    }

    #[test]
    fn stream_loss_stops_playback_and_keeps_the_native_error() {
        for kind in [
            ErrorKind::DeviceNotAvailable,
            ErrorKind::StreamInvalidated,
            ErrorKind::HostUnavailable,
        ] {
            let (errors, receiver) = reporter();
            errors.report(cpal::Error::new(kind));
            assert!(errors.shared.stream_lost.load(Ordering::Acquire));
            assert!(!errors.shared.playing.load(Ordering::Relaxed));
            assert_eq!(receiver.try_recv().expect("stream-loss diagnostic").kind(), kind);
            assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
        }
    }

    #[test]
    fn other_errors_do_not_stop_playback_or_clear_a_latched_loss() {
        for kind in [
            ErrorKind::DeviceBusy,
            ErrorKind::InvalidInput,
            ErrorKind::PermissionDenied,
            ErrorKind::ResourceExhausted,
            ErrorKind::UnsupportedConfig,
            ErrorKind::UnsupportedOperation,
            ErrorKind::BackendError,
            ErrorKind::Other,
        ] {
            let (errors, receiver) = reporter();
            errors.report(cpal::Error::new(kind));
            assert!(errors.shared.playing.load(Ordering::Relaxed));
            assert!(!errors.shared.stream_lost.load(Ordering::Acquire));
            assert_eq!(receiver.try_recv().expect("nonfatal diagnostic").kind(), kind);

            errors.shared.stream_lost.store(true, Ordering::Release);
            errors.shared.playing.store(false, Ordering::Relaxed);
            errors.report(cpal::Error::new(kind));
            assert!(errors.shared.stream_lost.load(Ordering::Acquire));
            assert!(!errors.shared.playing.load(Ordering::Relaxed));
            assert_eq!(receiver.try_recv().expect("later diagnostic").kind(), kind);
        }
    }

    #[test]
    fn notification_policy_filters_all_three_kinds_without_changing_flags() {
        let (errors, receiver) = reporter();
        for kind in [ErrorKind::Xrun, ErrorKind::DeviceChanged, ErrorKind::RealtimeDenied] {
            errors.report(cpal::Error::with_message(kind, "ignored native notification"));
            assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
            assert!(errors.shared.playing.load(Ordering::Relaxed));
            assert!(!errors.shared.stream_lost.load(Ordering::Acquire));

            errors.shared.stream_lost.store(true, Ordering::Release);
            errors.shared.playing.store(false, Ordering::Relaxed);
            errors.report(cpal::Error::with_message(kind, "ignored native notification"));
            assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
            assert!(errors.shared.stream_lost.load(Ordering::Acquire));
            assert!(!errors.shared.playing.load(Ordering::Relaxed));

            errors.shared.stream_lost.store(false, Ordering::Release);
            errors.shared.playing.store(true, Ordering::Relaxed);
        }
    }

    #[test]
    fn successful_send_moves_the_complete_owned_message_without_copying() {
        let (errors, receiver) = reporter();
        let message = String::from("Native output error with complete context: 音声, device 17, status -10863");
        let message_pointer = message.as_ptr();
        errors.report(cpal::Error::with_message(ErrorKind::BackendError, message));
        let diagnostic = receiver.try_recv().expect("complete native diagnostic");
        assert_eq!(diagnostic.kind(), ErrorKind::BackendError);
        let diagnostic_message = diagnostic.message().expect("native message");
        assert_eq!(diagnostic_message.as_ptr(), message_pointer);
        assert_eq!(
            diagnostic_message,
            "Native output error with complete context: 音声, device 17, status -10863"
        );
        drop(diagnostic);
        assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
    }

    #[test]
    fn full_queue_keeps_the_earliest_errors_but_still_publishes_stream_loss() {
        let (errors, receiver) = reporter();
        for index in 0..STREAM_ERROR_CAPACITY {
            errors.report(cpal::Error::with_message(
                ErrorKind::BackendError,
                format!("error {index}"),
            ));
        }
        errors.report(cpal::Error::with_message(
            ErrorKind::DeviceNotAvailable,
            String::from("later device-loss diagnostic"),
        ));
        assert!(errors.shared.stream_lost.load(Ordering::Acquire));
        assert!(!errors.shared.playing.load(Ordering::Relaxed));
        assert_eq!(receiver.len(), STREAM_ERROR_CAPACITY);
        for index in 0..STREAM_ERROR_CAPACITY {
            let diagnostic = receiver.try_recv().expect("earliest diagnostic retained");
            assert_eq!(diagnostic.kind(), ErrorKind::BackendError);
            assert_eq!(diagnostic.message(), Some(format!("error {index}").as_str()));
        }
        assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
    }

    #[test]
    fn disconnected_receiver_does_not_prevent_stream_loss_publication() {
        let (errors, receiver) = reporter();
        drop(receiver);
        errors.report(cpal::Error::with_message(
            ErrorKind::StreamInvalidated,
            String::from("diagnostic without a receiver"),
        ));
        assert!(errors.shared.stream_lost.load(Ordering::Acquire));
        assert!(!errors.shared.playing.load(Ordering::Relaxed));
    }
}

#[cfg(all(test, audiobatch_rtsan))]
mod test_realtime {
    use std::hint::black_box;

    use crossbeam_channel::{Receiver, TryRecvError, bounded};

    use super::*;

    /// Nonempty storage whose destruction must remain observable when the callback cannot retain an error.
    const OWNED_MESSAGE: &str =
        "Preallocated native stream diagnostic retained in full, including device and backend context";

    /// Prepares the channel and shared state outside the marked callback boundary.
    /// No receiver waits on this channel, matching the player's polling access.
    fn reporter() -> (StreamErrors, Receiver<cpal::Error>) {
        let shared = Arc::new(SharedState::new());
        shared.playing.store(true, Ordering::Relaxed);
        let (sender, receiver) = bounded(1);
        (StreamErrors { shared, sender }, receiver)
    }

    /// Allocates and exposes the complete native payload before entering the marked reporting method.
    fn owned_error(kind: ErrorKind) -> cpal::Error {
        black_box(cpal::Error::with_message(kind, black_box(String::from(OWNED_MESSAGE))))
    }

    #[test]
    fn rtsan_owned_success_is_clean() {
        let (errors, receiver) = reporter();
        let error = owned_error(ErrorKind::BackendError);
        let message_pointer = error.message().expect("preallocated native payload").as_ptr();

        errors.report(black_box(error));

        let diagnostic = receiver.try_recv().expect("queued owned diagnostic");
        assert_eq!(diagnostic.kind(), ErrorKind::BackendError);
        assert_eq!(diagnostic.message(), Some(OWNED_MESSAGE));
        assert_eq!(
            diagnostic.message().expect("diagnostic native payload").as_ptr(),
            message_pointer
        );
        drop(black_box(diagnostic));
        assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
        assert!(errors.shared.playing.load(Ordering::Relaxed));
        assert!(!errors.shared.stream_lost.load(Ordering::Acquire));
    }

    #[test]
    fn rtsan_borrowed_ignored_is_clean() {
        let (errors, receiver) = reporter();
        let error = black_box(cpal::Error::with_message(ErrorKind::Xrun, OWNED_MESSAGE));

        errors.report(black_box(error));

        assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
        assert!(errors.shared.playing.load(Ordering::Relaxed));
        assert!(!errors.shared.stream_lost.load(Ordering::Acquire));
    }

    #[test]
    fn rtsan_owned_ignored_is_blocked() {
        let (errors, receiver) = reporter();
        let error = owned_error(ErrorKind::Xrun);

        errors.report(black_box(error));

        assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
        assert!(errors.shared.playing.load(Ordering::Relaxed));
        assert!(!errors.shared.stream_lost.load(Ordering::Acquire));
    }

    #[test]
    fn rtsan_owned_full_is_blocked() {
        let (errors, receiver) = reporter();
        errors
            .sender
            .try_send(cpal::Error::with_message(
                ErrorKind::BackendError,
                "earliest diagnostic",
            ))
            .expect("fill the channel before entering the marked boundary");
        let error = owned_error(ErrorKind::DeviceNotAvailable);

        errors.report(black_box(error));

        assert!(errors.shared.stream_lost.load(Ordering::Acquire));
        assert!(!errors.shared.playing.load(Ordering::Relaxed));
        let retained = receiver.try_recv().expect("earliest diagnostic");
        assert_eq!(retained.kind(), ErrorKind::BackendError);
        assert_eq!(retained.message(), Some("earliest diagnostic"));
        drop(retained);
        assert_eq!(receiver.try_recv(), Err(TryRecvError::Empty));
    }

    #[test]
    fn rtsan_owned_disconnected_is_blocked() {
        let (errors, receiver) = reporter();
        drop(receiver);
        let error = owned_error(ErrorKind::StreamInvalidated);

        errors.report(black_box(error));

        assert!(errors.shared.stream_lost.load(Ordering::Acquire));
        assert!(!errors.shared.playing.load(Ordering::Relaxed));
    }
}
