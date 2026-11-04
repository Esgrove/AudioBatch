//! Bounded whole-file source statistics, independent of loudness weighting and preview gain.
//! Pearson covariance uses online centered moments, so DC does not masquerade as correlation.

/// Whole-file source measurements. Missing ratios have no finite denominator.
#[derive(Debug, Default, Clone, PartialEq)]
#[non_exhaustive]
pub struct SignalMetrics {
    /// Whole-file true peak in dBTP minus integrated LUFS, in dB.
    pub plr_db: Option<f64>,
    /// Whole-file true peak in dBTP minus maximum short-term LUFS, in dB.
    pub psr_db: Option<f64>,
    /// Sample peak divided by unweighted RMS over all channel samples, in dB.
    pub crest_factor_db: Option<f64>,
    /// Signed arithmetic mean of each channel in linear full-scale units.
    pub dc_offsets: Vec<f64>,
    /// Samples with magnitude at least one, counted separately in each channel.
    pub clip_counts: Vec<u64>,
    /// Whole-file centered Pearson coefficient, +1 for mono and absent above two channels.
    pub stereo_correlation: Option<f64>,
}

/// Fixed-size online moments, with no storage proportional to stream duration.
pub(crate) struct SignalMetricsAccumulator {
    frames: u64,
    channels: usize,
    means: [f64; 64],
    moments: [f64; 64],
    energies: [f64; 64],
    clips: [u64; 64],
    covariance: f64,
}

impl SignalMetrics {
    /// Returns the total number of full-scale channel samples, not the number of affected frames.
    #[must_use]
    pub fn total_clips(&self) -> u64 {
        self.clip_counts.iter().copied().sum()
    }

    /// Returns the largest absolute channel DC offset, retaining signed values separately.
    #[must_use]
    pub fn maximum_dc_offset(&self) -> Option<f64> {
        self.dc_offsets.iter().map(|value| value.abs()).reduce(f64::max)
    }
}

impl SignalMetricsAccumulator {
    /// Allocates only fixed channel state for a previously validated stream.
    pub(crate) const fn new(channels: usize) -> Self {
        Self {
            frames: 0,
            channels,
            means: [0.0; 64],
            moments: [0.0; 64],
            energies: [0.0; 64],
            clips: [0; 64],
            covariance: 0.0,
        }
    }

    /// Accumulates in source order, independent of input packet and meter interval boundaries.
    pub(crate) fn push(&mut self, planes: &[&[f32]]) {
        for frame in 0..planes.first().map_or(0, |plane| plane.len()) {
            self.frames += 1;
            let mut left_delta = 0.0;
            for (channel, ((((plane, mean), moment), energy), clips)) in planes
                .iter()
                .zip(&mut self.means)
                .zip(&mut self.moments)
                .zip(&mut self.energies)
                .zip(&mut self.clips)
                .enumerate()
            {
                let sample = f64::from(*plane.get(frame).expect("equal measured plane lengths"));
                let delta = sample - *mean;
                *mean += delta / self.frames as f64;
                *moment = delta.mul_add(sample - *mean, *moment);
                *energy = sample.mul_add(sample, *energy);
                *clips += u64::from(sample.abs() >= 1.0);
                if channel == 0 {
                    left_delta = delta;
                } else if channel == 1 {
                    self.covariance = left_delta.mul_add(sample - *mean, self.covariance);
                }
            }
        }
    }

    /// Reduces unweighted moments and the existing peak/loudness results without changing those meters.
    pub(crate) fn finish(self, peak: f64, true_peak: f64, integrated: f64, short_term: f64) -> SignalMetrics {
        let ratio = |loudness: f64| {
            (true_peak > 0.0 && loudness > crate::record::NEGATIVE_INFINITY_LOUDNESS)
                .then(|| 20.0_f64.mul_add(true_peak.log10(), -loudness))
                .filter(|value| value.is_finite())
        };
        let energy: f64 = self
            .energies
            .get(..self.channels)
            .expect("validated channel count")
            .iter()
            .sum();
        let rms = (energy / (self.frames as f64 * self.channels as f64)).sqrt();
        let correlation = if self.channels == 1 {
            Some(1.0)
        } else if self.channels == 2 && self.moments[0] > 0.0 && self.moments[1] > 0.0 {
            let value = self.covariance / (self.moments[0].sqrt() * self.moments[1].sqrt());
            value.is_finite().then(|| value.clamp(-1.0, 1.0))
        } else {
            None
        };
        SignalMetrics {
            plr_db: ratio(integrated),
            psr_db: ratio(short_term),
            crest_factor_db: (rms > 0.0 && peak > 0.0)
                .then(|| 20.0 * (peak / rms).log10())
                .filter(|value| value.is_finite()),
            dc_offsets: if self.frames > 0 {
                self.means
                    .get(..self.channels)
                    .expect("validated channel count")
                    .to_vec()
            } else {
                Vec::new()
            },
            clip_counts: self
                .clips
                .get(..self.channels)
                .expect("validated channel count")
                .to_vec(),
            stereo_correlation: correlation,
        }
    }
}
