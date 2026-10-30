//! The effect the preview can run on its output, such as a plugin chain, and how the player hands it over.
//!
//! An insert runs inside the audio callback, so it is handed to the callback without locks:
//! the player sends it over a lock-free queue, and the callback swaps it in at the start of its next block.
//! The insert it replaces goes back over a second queue, so it is dropped on the player's thread,
//! never on the audio thread where freeing memory could cause a dropout.
//! The callback counts the swaps it has made, which tells the player when a detach has taken effect.

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use rtrb::{Consumer, Producer, RingBuffer};
use thiserror::Error;

use crate::output::SharedState;
use crate::output::insert::InsertRenderer;

/// How many insert changes can wait for the audio callback.
const COMMAND_CAPACITY: usize = 8;

/// A control-thread insert change could not be queued for the audio callback.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum InsertError {
    /// Earlier changes fill the queue while the callback is not taking them.
    #[error("The audio output is not taking plugin changes, because its stream is not running")]
    QueueFull,
}

/// An effect the preview runs on its stereo output inside the audio callback.
///
/// Every method except `tail_frames` runs on the audio thread,
/// so none of them may allocate, lock, block, or wait for I/O.
pub trait PreviewInsert: Send {
    /// Returns the most frames one `process` call may get.
    fn max_block_frames(&self) -> usize;

    /// Returns how many frames the insert delays its output by.
    fn latency_frames(&self) -> usize;

    /// Returns how many frames of output may follow the end of the input, such as the decay of a reverb.
    /// It is read once when the insert is handed to the player,
    /// and later changes go through `Player::set_insert_tail`.
    fn tail_frames(&self) -> usize;

    /// Clears the audio the insert holds, such as delay lines and decaying tails, without changing its settings.
    /// `position_frames` is where playback starts, in frames at the output rate,
    /// which tempo-synced effects read as the project time.
    fn reset(&mut self, position_frames: u64);

    /// Processes a block of frames in place.
    /// Returns false when the output is not finite, which stops playback.
    fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool;
}

/// An insert and its control-thread tail value, handed over as one queue entry.
pub struct InsertChange {
    /// The next insert, or none to remove the current one.
    pub(crate) insert: Option<Box<dyn PreviewInsert>>,
    /// Tail of this insert, shared only with its own control-thread updates.
    pub(crate) tail: Arc<AtomicUsize>,
}

/// The player's side of the insert queues.
pub struct InsertControl {
    /// Sends the next insert and its associated tail.
    commands: Producer<InsertChange>,
    /// Tail of the latest queued insert, not an earlier insert still awaiting retirement.
    tail: Arc<AtomicUsize>,
    /// Receives replaced inserts and tail storage for control-thread disposal.
    retired: Consumer<InsertChange>,
    /// Number of changes sent so far.
    sent: u64,
    /// Holds the count of changes the callback has made.
    shared: Arc<SharedState>,
}

impl InsertControl {
    /// Sends an insert to the audio callback, or none to remove the current one.
    ///
    /// # Errors
    ///
    /// Returns an error when earlier changes are still waiting, because the audio callback is not running.
    /// The insert is dropped on this thread then.
    pub fn set(&mut self, insert: Option<Box<dyn PreviewInsert>>) -> Result<(), InsertError> {
        let tail = Arc::new(AtomicUsize::new(
            insert.as_ref().map_or(0, |insert| insert.tail_frames()),
        ));
        if self
            .commands
            .push(InsertChange {
                insert,
                tail: Arc::clone(&tail),
            })
            .is_err()
        {
            return Err(InsertError::QueueFull);
        }
        self.tail = tail;
        self.sent += 1;
        Ok(())
    }

    /// Returns the inserts the audio callback has replaced since the last call, for dropping on this thread.
    pub fn take_retired(&mut self) -> Vec<Box<dyn PreviewInsert>> {
        std::iter::from_fn(|| self.retired.pop().ok())
            .filter_map(|change| change.insert)
            .collect()
    }

    /// Returns true when the audio callback has made every change sent so far.
    #[must_use]
    pub fn is_settled(&self) -> bool {
        self.shared.insert_changes_applied.load(Ordering::Acquire) == self.sent
    }

    /// Sets the tail of the latest queued insert, without changing an older insert still active in the callback.
    pub fn set_tail(&self, frames: usize) {
        self.tail.store(frames, Ordering::Relaxed);
    }
}

/// Creates the queues between the player and the audio callback.
/// Replaced insert and tail storage return together, so neither can be freed on the audio thread.
/// A full retire queue stalls changes until the player collects its entries.
pub fn insert_channel(shared: Arc<SharedState>) -> (InsertControl, InsertRenderer) {
    let (command_producer, command_consumer) = RingBuffer::new(COMMAND_CAPACITY);
    let (retired_producer, retired_consumer) = RingBuffer::new(COMMAND_CAPACITY + 1);
    let control = InsertControl {
        commands: command_producer,
        tail: Arc::new(AtomicUsize::new(0)),
        retired: retired_consumer,
        sent: 0,
        shared,
    };
    (control, InsertRenderer::new(command_consumer, retired_producer))
}

#[cfg(test)]
mod test_insert_control {
    use super::*;

    #[test]
    fn tail_storage_is_retired_with_removal_entries_until_control_thread_collection() {
        let shared = Arc::new(SharedState::new());
        let (mut control, mut renderer) = insert_channel(Arc::clone(&shared));
        control.set(None).expect("first removal");
        let first = Arc::downgrade(&control.tail);
        renderer.apply_changes(&shared);
        control.set(None).expect("second removal");
        control.set_tail(42);
        renderer.apply_changes(&shared);
        assert!(control.is_settled());
        assert!(
            first.upgrade().is_some(),
            "tail storage must not be freed by the callback"
        );
        assert!(
            control.take_retired().is_empty(),
            "removals have no processor to return"
        );
        assert!(first.upgrade().is_none(), "collection frees the retired tail storage");
        assert_eq!(control.tail.load(Ordering::Relaxed), 42);
    }

    #[test]
    fn full_queue_returns_a_typed_error_without_applying_the_change() {
        let shared = Arc::new(SharedState::new());
        let (mut control, _renderer) = insert_channel(Arc::clone(&shared));
        for _ in 0..COMMAND_CAPACITY {
            control.set(None).expect("queue has space");
        }
        control.set_tail(42);
        assert_eq!(control.set(None), Err(InsertError::QueueFull));
        assert_eq!(control.sent, COMMAND_CAPACITY as u64);
        assert_eq!(control.tail.load(Ordering::Relaxed), 42);
        assert_eq!(
            InsertError::QueueFull.to_string(),
            "The audio output is not taking plugin changes, because its stream is not running"
        );
    }
}
