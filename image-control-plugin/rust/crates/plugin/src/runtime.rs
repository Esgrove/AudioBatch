//! Format-independent instance ownership and stopped preparation shared by host adapters.

use std::sync::Arc;

use image_control_dsp::{Imager, SharedState};

use crate::editor_geometry::EditorGeometry;
use crate::host::audio::AudioState;
use crate::params::Parameters;

/// Host-visible sample counts from one successful stopped preparation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PreparedProcessing {
    /// Input-to-output FIR delay, unchanged by bypass.
    pub(super) latency_samples: u32,
    /// Finite FIR ring beyond the latency-compensated endpoint.
    pub(super) tail_samples: u32,
}

/// Stable parameter, monitoring, geometry, and exclusively admitted DSP ownership.
pub struct Instance {
    /// Authoritative sound controls, independent of mutable processing history.
    pub(super) parameters: Arc<Parameters>,
    /// DSP targets and analysis transport, never replaced while callbacks run.
    pub(super) shared: Arc<SharedState>,
    /// Coherent logical geometry survives native view destruction.
    pub(super) geometry: Arc<EditorGeometry>,
    /// Mutable DSP history, accessed only through bounded admission.
    pub(super) audio: AudioState,
}

impl Instance {
    /// Allocates controls and transport before exposing the instance to a host.
    pub(super) fn new() -> Self {
        let imager = Imager::new();
        Self {
            parameters: Arc::new(Parameters::new()),
            shared: imager.shared(),
            geometry: Arc::new(EditorGeometry::default()),
            audio: AudioState::new(imager),
        }
    }

    /// Prepares stopped processing outside realtime callbacks and returns its latency and finite tail.
    pub(super) fn prepare(
        &self,
        sample_rate: f64,
        channels: usize,
        offline: bool,
        maximum_frames: u32,
    ) -> Option<PreparedProcessing> {
        if !sample_rate.is_finite() || sample_rate <= 0.0 || ![1, 2].contains(&channels) || maximum_frames == 0 {
            return None;
        }
        let report = self.audio.try_with(|imager| {
            self.parameters.publish(&self.shared);
            imager.set_channels(channels);
            imager.set_offline(offline);
            imager.prepare(sample_rate as f32).then(|| PreparedProcessing {
                latency_samples: imager.latency_samples(),
                tail_samples: imager.tail_samples(),
            })
        })??;
        self.shared.set_maximum_block_frames(maximum_frames);
        self.shared.set_processing_active(false);
        self.shared.invalidate_monitoring();
        Some(report)
    }
}

#[cfg(test)]
mod tests {
    use super::{Instance, PreparedProcessing};

    /// Deactivation retires processing resources without disconnecting an already-open editor.
    #[test]
    fn release_and_reprepare_preserve_shared_transport_identity() {
        let instance = Instance::new();
        assert_eq!(
            instance.prepare(48_000.0, 2, false, 256),
            Some(PreparedProcessing {
                latency_samples: 4352,
                tail_samples: 4096,
            })
        );
        instance
            .audio
            .try_with(image_control_dsp::Imager::release)
            .expect("stopped release");
        instance
            .audio
            .try_with(|imager| {
                assert!(std::sync::Arc::ptr_eq(&instance.shared, &imager.shared()));
                assert_eq!(imager.latency_samples(), 0);
            })
            .expect("same transport");
        assert_eq!(
            instance.prepare(48_000.0, 1, true, 256),
            Some(PreparedProcessing {
                latency_samples: 4352,
                tail_samples: 4096,
            })
        );
        instance
            .audio
            .try_with(|imager| {
                assert!(std::sync::Arc::ptr_eq(&instance.shared, &imager.shared()));
            })
            .expect("same transport after reactivation");
    }
}
