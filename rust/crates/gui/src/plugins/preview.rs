//! Live preview through the plugin chain.
//!
//! `ChainInsert` hands a `PreviewChain` to the player as its insert,
//! and `PluginPreview` remembers what the player holds, so the main view can check it on its housekeeping tick.
//! The live instances are already prepared,
//! so a change to the structure of the chain builds a new chain and swaps it in.
//! Reconfiguring an instance, for a new output rate or a new latency, needs its processor back from the audio thread,
//! so the chain is first taken out of the player, then the instances are prepared again, and a new chain goes in.
//! The block size of the device needs no reconfiguration, because the player splits its blocks to fit the chain.

use std::sync::Arc;

use audiobatch_playback::PreviewInsert;
use audiobatch_plugins::{PreviewChain, PreviewFailure};

/// A plugin chain as the player's insert.
pub struct ChainInsert(pub PreviewChain);

/// What the player holds of the plugin chain.
#[derive(Debug, Default)]
pub struct PluginPreview {
    /// Failure record of the chain the player holds, `None` when it holds none.
    attached: Option<Arc<PreviewFailure>>,
    /// Tail last passed to the player, in frames.
    tail_frames: usize,
    /// Set when the player was replaced, so the next tick prepares the instances for it and attaches a new chain.
    refresh_pending: bool,
}

impl PreviewInsert for ChainInsert {
    fn max_block_frames(&self) -> usize {
        self.0.max_block_frames()
    }

    fn latency_frames(&self) -> usize {
        self.0.latency_frames()
    }

    fn tail_frames(&self) -> usize {
        self.0.tail_frames()
    }

    fn reset(&mut self) {
        self.0.reset();
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool {
        self.0.process(left, right)
    }
}

impl PluginPreview {
    /// Returns true while the player holds a chain.
    #[must_use]
    pub const fn is_attached(&self) -> bool {
        self.attached.is_some()
    }

    /// Records that the player took a chain with a failure record and a tail.
    pub fn attached(&mut self, failure: Arc<PreviewFailure>, tail_frames: usize) {
        self.attached = Some(failure);
        self.tail_frames = tail_frames;
    }

    /// Records that the player holds no chain.
    pub fn detached(&mut self) {
        self.attached = None;
    }

    /// Returns the failure of the chain the player holds, as "<name>: <error>".
    #[must_use]
    pub fn failure(&self) -> Option<String> {
        self.attached.as_ref().and_then(|failure| failure.message())
    }

    /// Records a tail and returns true when it differs from the tail last passed to the player.
    pub const fn tail_changed(&mut self, tail_frames: usize) -> bool {
        let changed = tail_frames != self.tail_frames;
        self.tail_frames = tail_frames;
        changed
    }

    /// Records that the player was replaced and holds no chain, so the next tick attaches one again.
    pub fn player_replaced(&mut self) {
        self.attached = None;
        self.refresh_pending = true;
    }

    /// Returns and clears whether the player was replaced since the last call.
    pub const fn take_refresh_pending(&mut self) -> bool {
        std::mem::replace(&mut self.refresh_pending, false)
    }
}

#[cfg(test)]
mod test_plugin_preview {
    use super::*;

    #[test]
    fn only_a_changed_tail_is_passed_on() {
        let mut preview = PluginPreview::default();
        assert!(!preview.tail_changed(0));
        assert!(preview.tail_changed(480));
        assert!(!preview.tail_changed(480));
        assert!(preview.tail_changed(0));
    }

    #[test]
    fn a_replaced_player_asks_for_one_refresh() {
        let mut preview = PluginPreview::default();
        assert!(!preview.take_refresh_pending());
        preview.player_replaced();
        assert!(!preview.is_attached());
        assert!(preview.take_refresh_pending());
        assert!(!preview.take_refresh_pending());
    }

    #[test]
    fn a_detached_preview_has_no_failure() {
        let mut preview = PluginPreview::default();
        assert!(preview.failure().is_none());
        preview.detached();
        assert!(!preview.is_attached());
        assert!(preview.failure().is_none());
    }
}
