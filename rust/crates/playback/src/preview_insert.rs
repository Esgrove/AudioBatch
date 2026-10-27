//! The effect the preview can run on its output, such as a plugin chain, and how the player hands it over.
//!
//! An insert runs inside the audio callback, so it is handed to the callback without locks:
//! the player sends it over a lock-free queue, and the callback swaps it in at the start of its next block.
//! The insert it replaces goes back over a second queue, so it is dropped on the player's thread,
//! never on the audio thread where freeing memory could cause a dropout.
//! The callback counts the swaps it has made, which tells the player when a detach has taken effect.

use std::sync::Arc;
use std::sync::atomic::Ordering;

use anyhow::{Result, bail};
use rtrb::{Consumer, Producer, RingBuffer};

use crate::output::SharedState;
use crate::output::insert::InsertRenderer;

/// How many insert changes can wait for the audio callback.
const COMMAND_CAPACITY: usize = 8;

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
    fn reset(&mut self);

    /// Processes a block of frames in place.
    /// Returns false when the output is not finite, which stops playback.
    fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool;
}

/// The player's side of the insert queues.
pub struct InsertControl {
    /// Sends the next insert, or none to remove the current one.
    commands: Producer<Option<Box<dyn PreviewInsert>>>,
    /// Receives the inserts the callback replaced.
    retired: Consumer<Box<dyn PreviewInsert>>,
    /// Number of changes sent so far.
    sent: u64,
    /// Holds the count of changes the callback has made, and the tail of the current insert.
    shared: Arc<SharedState>,
}

impl InsertControl {
    /// Sends an insert to the audio callback, or none to remove the current one.
    ///
    /// # Errors
    ///
    /// Returns an error when earlier changes are still waiting, because the audio callback is not running.
    /// The insert is dropped on this thread then.
    pub fn set(&mut self, insert: Option<Box<dyn PreviewInsert>>) -> Result<()> {
        let tail = insert.as_ref().map_or(0, |insert| insert.tail_frames());
        if self.commands.push(insert).is_err() {
            bail!("The audio output is not taking plugin changes, because its stream is not running");
        }
        self.sent += 1;
        self.set_tail(tail);
        Ok(())
    }

    /// Returns the inserts the audio callback has replaced since the last call, for dropping on this thread.
    pub fn take_retired(&mut self) -> Vec<Box<dyn PreviewInsert>> {
        std::iter::from_fn(|| self.retired.pop().ok()).collect()
    }

    /// Returns true when the audio callback has made every change sent so far.
    #[must_use]
    pub fn is_settled(&self) -> bool {
        self.shared.insert_changes_applied.load(Ordering::Acquire) == self.sent
    }

    /// Sets how many frames of output may follow the end of the input for the current insert.
    pub fn set_tail(&self, frames: usize) {
        self.shared.insert_tail_frames.store(frames as u64, Ordering::Relaxed);
    }
}

/// Creates the queues between the player and the audio callback.
/// The retire queue has room for every queued change and the current insert,
/// so the callback can always hand back the insert it replaces.
pub fn insert_channel(shared: Arc<SharedState>) -> (InsertControl, InsertRenderer) {
    let (command_producer, command_consumer) = RingBuffer::new(COMMAND_CAPACITY);
    let (retired_producer, retired_consumer) = RingBuffer::new(COMMAND_CAPACITY + 1);
    let control = InsertControl {
        commands: command_producer,
        retired: retired_consumer,
        sent: 0,
        shared,
    };
    (control, InsertRenderer::new(command_consumer, retired_producer))
}
