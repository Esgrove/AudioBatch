//! Audio-thread-owned, centered exponential stereo correlation with fixed storage and partition-rate readings.
//!
//! Observation and publication never allocate or lock. The processing owner copies readings into shared atomics.

/// Integration time of the channel moments, independent of sample rate and host block geometry.
const CORRELATION_SECONDS: f64 = 0.15;
/// Minimum centered power in each channel, equivalent to -100 dBFS RMS.
const MIN_CORRELATION_POWER: f64 = 1.0e-10;

/// Double-precision exponentially weighted means, second moments, and cross moment for one band.
#[derive(Clone, Copy, Default)]
struct CorrelationMoments {
    left_mean: f64,
    right_mean: f64,
    left_power: f64,
    right_power: f64,
    cross_power: f64,
}

/// Fixed four-band correlation state accessed exclusively by the processing thread.
/// A NaN reading denotes insufficient power in either channel, not zero correlation.
pub struct BandCorrelation {
    moments: [CorrelationMoments; 4],
    readings: [f32; 4],
    sample_weight: f64,
    revision: u64,
}

impl BandCorrelation {
    /// Precomputes one sample's weight from a validated preparation rate and starts every band unavailable.
    pub fn new(sample_rate: f32) -> Self {
        Self {
            moments: [CorrelationMoments::default(); 4],
            readings: [f32::NAN; 4],
            sample_weight: -(-1.0 / (f64::from(sample_rate) * CORRELATION_SECONDS)).exp_m1(),
            revision: 0,
        }
    }

    /// Clears moments and invalidates the cached readings without changing the prepared time constant.
    pub fn reset(&mut self) {
        self.moments.fill(CorrelationMoments::default());
        self.readings.fill(f32::NAN);
        self.revision = self.revision.wrapping_add(1);
    }

    /// Accumulates one processed stereo band before global output, Delta, and Mono controls.
    /// A non-finite sample clears only its own band's history, permitting recovery on subsequent finite samples.
    pub fn push(&mut self, band: usize, left: f32, right: f32) {
        let Some(state) = self.moments.get_mut(band) else {
            return;
        };
        if !left.is_finite() || !right.is_finite() {
            *state = CorrelationMoments::default();
            if let Some(reading) = self.readings.get_mut(band) {
                *reading = f32::NAN;
            }
            return;
        }
        let left = f64::from(left);
        let right = f64::from(right);
        state.left_mean = self.sample_weight.mul_add(left - state.left_mean, state.left_mean);
        state.right_mean = self.sample_weight.mul_add(right - state.right_mean, state.right_mean);
        state.left_power = self
            .sample_weight
            .mul_add(left.mul_add(left, -state.left_power), state.left_power);
        state.right_power = self
            .sample_weight
            .mul_add(right.mul_add(right, -state.right_power), state.right_power);
        state.cross_power = self
            .sample_weight
            .mul_add(left.mul_add(right, -state.cross_power), state.cross_power);
    }

    /// Computes bounded centered covariance once per completed partition, without touching audio controls.
    pub fn publish(&mut self) {
        for (state, reading) in self.moments.iter().zip(&mut self.readings) {
            let left_variance = state.left_mean.mul_add(-state.left_mean, state.left_power);
            let right_variance = state.right_mean.mul_add(-state.right_mean, state.right_power);
            let covariance = state.left_mean.mul_add(-state.right_mean, state.cross_power);
            *reading = if left_variance.is_finite()
                && right_variance.is_finite()
                && left_variance > MIN_CORRELATION_POWER
                && right_variance > MIN_CORRELATION_POWER
                && covariance.is_finite()
            {
                let correlation = covariance / (left_variance * right_variance).sqrt();
                if correlation.is_finite() {
                    correlation.clamp(-1.0, 1.0) as f32
                } else {
                    f32::NAN
                }
            } else {
                f32::NAN
            };
        }
        self.revision = self.revision.wrapping_add(1);
    }

    /// Returns the latest independent per-band readings from processing-thread-owned storage.
    #[must_use]
    pub const fn readings(&self) -> [f32; 4] {
        self.readings
    }

    /// Identifies partition publications and invalidations so a caller need not republish every output frame.
    #[must_use]
    pub const fn revision(&self) -> u64 {
        self.revision
    }
}
