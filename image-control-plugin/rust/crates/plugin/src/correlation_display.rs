//! UI-owned correlation history matching C++ `BandCorrelationMeter`'s trend, extrema, and soft range field.
//! All storage is fixed and updates allocate nothing or acquire locks. Audio publication stays outside this module.

/// Fixed spatial resolution, from signed correlation -1 through +1 inclusive.
pub const RANGE_INTENSITY_SAMPLES: usize = 129;
/// Shortest supported trend time constant and recent-range window, in seconds.
const MIN_PERSISTENCE_SECONDS: f64 = 0.05;
/// Maximum retained observation duration and largest valid refresh interval, in seconds.
const MAX_PERSISTENCE_SECONDS: f64 = 3.0;
/// Time constant selected when the supplied Persistence is nonfinite.
const DEFAULT_PERSISTENCE_SECONDS: f64 = 0.3;
/// Bucket resolution bounds late expiry of exact extrema to at most one hundred twentieth of a second.
const HISTORY_BUCKET_SECONDS: f64 = 1.0 / 120.0;
/// Retains the maximum window, its partial boundary bucket, and one spare bucket.
const HISTORY_CAPACITY: usize = 362;
/// Half-width of each soft spatial edge, in signed correlation units, matching the C++ meter.
const CORRELATION_RANGE_FEATHER: f32 = 0.06;
/// Uniform signed-coordinate spacing of the range field.
const SPATIAL_STEP: f32 = 2.0 / (RANGE_INTENSITY_SAMPLES - 1) as f32;

/// One copyable presentation snapshot. Numeric mode selection never modifies the retained history.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MeterReading {
    /// Immediate finite observation, or unavailable for silence, stale publications, and reset boundaries.
    pub live: Option<f32>,
    /// Display-only exponential trend, initialized directly from the first valid observation.
    pub trend: Option<f32>,
    /// Minimum and maximum display-observed values in the selected trailing window.
    pub range: Option<(f32, f32)>,
    /// Soft coverage at 129 uniformly spaced signed coordinates, fading to zero at Persistence.
    pub range_intensity: [f32; RANGE_INTENSITY_SAMPLES],
}

impl MeterReading {
    /// Selects only the numerical presentation, leaving the live marker, trend, and range untouched.
    #[must_use]
    pub const fn value(&self, smoothed: bool) -> Option<f32> {
        if smoothed { self.trend } else { self.live }
    }
}

impl Default for MeterReading {
    /// Starts unavailable without inventing a zero-valued measurement.
    fn default() -> Self {
        Self {
            live: None,
            trend: None,
            range: None,
            range_intensity: [0.0; RANGE_INTENSITY_SAMPLES],
        }
    }
}

/// Display-observed extrema of a fixed time bucket, with actual times for continuous visual fading.
#[derive(Clone, Copy, Debug, Default)]
struct HistoryBucket {
    /// Bucket start on the meter's relative clock, in seconds.
    start_seconds: f64,
    /// Lowest value observed in the bucket.
    minimum: f32,
    /// Highest value observed in the bucket.
    maximum: f32,
    /// Most recent observation of the minimum.
    minimum_seconds: f64,
    /// Most recent observation of the maximum.
    maximum_seconds: f64,
}

/// Bounded, nonserialized history owned and updated by the UI thread, independently of numeric display mode.
#[derive(Debug)]
pub struct MeterHistory {
    /// Maximum-duration ring, retained even when the selected Persistence is shorter.
    history: [HistoryBucket; HISTORY_CAPACITY],
    /// Cached immediate and historical presentation values.
    reading: MeterReading,
    /// Relative elapsed time since the last unavailable reading or invalid timing boundary.
    history_seconds: f64,
    /// Index of the oldest occupied bucket.
    history_begin: usize,
    /// Occupied bucket count, bounded by `HISTORY_CAPACITY`.
    history_count: usize,
}

impl Default for MeterHistory {
    /// Starts with an empty fixed-capacity observation ring and no presentation values.
    fn default() -> Self {
        Self {
            history: [HistoryBucket::default(); HISTORY_CAPACITY],
            reading: MeterReading::default(),
            history_seconds: 0.0,
            history_begin: 0,
            history_count: 0,
        }
    }
}

impl MeterHistory {
    /// Removes all presentation history when a meter is hidden or its publication generation changes.
    pub fn clear(&mut self) {
        self.reading = MeterReading::default();
        self.history_seconds = 0.0;
        self.history_begin = 0;
        self.history_count = 0;
    }

    /// Returns the immediate observation without display smoothing.
    #[must_use]
    pub const fn live(&self) -> Option<f32> {
        self.reading.live
    }

    /// Returns the exponential display trend, not the audio thread's correlation integration.
    #[must_use]
    pub const fn trend(&self) -> Option<f32> {
        self.reading.trend
    }

    /// Returns the selected trailing-window extrema with at most one bucket of late expiry.
    #[must_use]
    pub const fn range(&self) -> Option<(f32, f32)> {
        self.reading.range
    }

    /// Borrows soft coverage at uniformly spaced signed coordinates from -1 to +1.
    #[must_use]
    pub const fn field(&self) -> &[f32; RANGE_INTENSITY_SAMPLES] {
        &self.reading.range_intensity
    }

    /// Copies the renderer's bounded snapshot without exposing or cloning the retained observation ring.
    #[must_use]
    pub const fn snapshot(&self) -> MeterReading {
        MeterReading {
            live: self.live(),
            trend: self.trend(),
            range: self.range(),
            range_intensity: *self.field(),
        }
    }

    /// Advances real-time presentation from an already freshness-checked audio publication.
    /// None and nonfinite readings clear history. Invalid timing or gaps longer than three seconds reseed it.
    /// An elapsed interval exactly at three seconds remains valid, matching C++ `BandCorrelationMeter`.
    pub fn update(&mut self, reading: Option<f32>, elapsed_seconds: f64, persistence_seconds: f64) {
        let live = reading
            .filter(|value| value.is_finite())
            .map(|value| value.clamp(-1.0, 1.0));
        let valid_elapsed = elapsed_seconds.is_finite() && (0.0..=MAX_PERSISTENCE_SECONDS).contains(&elapsed_seconds);
        let persistence = if persistence_seconds.is_finite() {
            persistence_seconds.clamp(MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS)
        } else {
            DEFAULT_PERSISTENCE_SECONDS
        };
        if live.is_none() || !valid_elapsed {
            self.clear();
        }
        self.reading.live = live;
        if let Some(value) = live {
            self.advance_history(value, if valid_elapsed { elapsed_seconds } else { 0.0 }, persistence);
            self.update_range_intensity(value, persistence);
        }
    }

    /// Records every valid refresh, retaining same-bucket extrema and selecting the requested range window.
    #[expect(
        clippy::suboptimal_flops,
        reason = "Preserves the reference meter's trend evaluation order and precision"
    )]
    fn advance_history(&mut self, value: f32, elapsed_seconds: f64, persistence_seconds: f64) {
        self.history_seconds += elapsed_seconds;
        self.reading.trend = Some(self.reading.trend.map_or(value, |trend| {
            let weight = -(-elapsed_seconds / persistence_seconds).exp_m1();
            (f64::from(trend) + weight * f64::from(value - trend)) as f32
        }));
        let oldest_retained = self.history_seconds - MAX_PERSISTENCE_SECONDS;
        while self.history_count > 0 && self.bucket(0).start_seconds + HISTORY_BUCKET_SECONDS <= oldest_retained {
            self.history_begin = (self.history_begin + 1) % HISTORY_CAPACITY;
            self.history_count -= 1;
        }
        let bucket_start = (self.history_seconds / HISTORY_BUCKET_SECONDS).floor() * HISTORY_BUCKET_SECONDS;
        if self.history_count > 0 && bucket_start <= self.bucket(self.history_count - 1).start_seconds {
            let last = (self.history_begin + self.history_count - 1) % HISTORY_CAPACITY;
            let bucket = self
                .history
                .get_mut(last)
                .expect("ring index is reduced modulo its capacity");
            if value <= bucket.minimum {
                bucket.minimum = value;
                bucket.minimum_seconds = self.history_seconds;
            }
            if value >= bucket.maximum {
                bucket.maximum = value;
                bucket.maximum_seconds = self.history_seconds;
            }
        } else {
            if self.history_count == HISTORY_CAPACITY {
                self.history_begin = (self.history_begin + 1) % HISTORY_CAPACITY;
                self.history_count -= 1;
            }
            let next = (self.history_begin + self.history_count) % HISTORY_CAPACITY;
            *self
                .history
                .get_mut(next)
                .expect("ring index is reduced modulo its capacity") = HistoryBucket {
                start_seconds: bucket_start,
                minimum: value,
                maximum: value,
                minimum_seconds: self.history_seconds,
                maximum_seconds: self.history_seconds,
            };
            self.history_count += 1;
        }
        let mut minimum = value;
        let mut maximum = value;
        let oldest_shown = self.history_seconds - persistence_seconds;
        for index in 0..self.history_count {
            let bucket = self.bucket(index);
            if bucket.start_seconds + HISTORY_BUCKET_SECONDS > oldest_shown {
                minimum = minimum.min(bucket.minimum);
                maximum = maximum.max(bucket.maximum);
            }
        }
        self.reading.range = Some((minimum, maximum));
    }

    /// Reads a logical ring entry without assuming occupied buckets are physically contiguous.
    fn bucket(&self, index: usize) -> &HistoryBucket {
        self.history
            .get((self.history_begin + index) % HISTORY_CAPACITY)
            .expect("ring index is reduced modulo its capacity")
    }

    /// Rebuilds the C++ field from newest to oldest with its covered-interval pruning and feather constant.
    /// Independent extreme timestamps avoid bucket-boundary opacity steps or a release tail beyond Persistence.
    #[expect(
        clippy::suboptimal_flops,
        reason = "Preserves the reference meter's opacity and feather evaluation order"
    )]
    fn update_range_intensity(&mut self, live: f32, persistence_seconds: f64) {
        let mut field = [0.0_f32; RANGE_INTENSITY_SAMPLES];
        let mut minimum_covered = live;
        let mut maximum_covered = live;
        let mut minimum_covered_seconds = self.history_seconds;
        let mut maximum_covered_seconds = self.history_seconds;
        let mut add_observation = |value: f32, observed_seconds: f64| {
            // A newer enclosing interval also covers this older interval's identical soft spatial edge.
            if value >= minimum_covered
                && value <= maximum_covered
                && observed_seconds < minimum_covered_seconds.min(maximum_covered_seconds)
            {
                return;
            }
            let remaining = (1.0 - (self.history_seconds - observed_seconds) / persistence_seconds).clamp(0.0, 1.0);
            let opacity = (remaining * remaining * (3.0 - 2.0 * remaining)) as f32;
            if opacity <= 0.0 {
                return;
            }
            let minimum = value.min(live);
            let maximum = value.max(live);
            for (sample, intensity) in field.iter_mut().enumerate() {
                let position = -1.0 + sample as f32 * SPATIAL_STEP;
                let distance = (minimum - position).max(position - maximum).max(0.0);
                let coverage = (1.0 - distance / CORRELATION_RANGE_FEATHER).clamp(0.0, 1.0);
                let feather = coverage * coverage * (3.0 - 2.0 * coverage);
                *intensity = (*intensity).max(opacity * feather);
            }
            if value < minimum_covered {
                minimum_covered = value;
                minimum_covered_seconds = observed_seconds;
            }
            if value > maximum_covered {
                maximum_covered = value;
                maximum_covered_seconds = observed_seconds;
            }
        };
        add_observation(live, self.history_seconds);
        for index in (0..self.history_count).rev() {
            let bucket = self.bucket(index);
            add_observation(bucket.minimum, bucket.minimum_seconds);
            add_observation(bucket.maximum, bucket.maximum_seconds);
        }
        self.reading.range_intensity = field;
    }
}

#[cfg(test)]
mod tests {
    use std::collections::VecDeque;

    use super::*;

    /// Compares the float-state trend with an independent double-precision exponential expectation.
    fn expect_trend(meter: &MeterHistory, expected: f64) {
        let actual = f64::from(meter.trend().expect("valid history has a trend"));
        assert!(
            (actual - expected).abs() < 5.0e-6,
            "trend {actual} differs from {expected}"
        );
    }

    /// Selects a field sample at an exactly represented grid coordinate for analytical tests.
    fn intensity_at(meter: &MeterHistory, value: f32) -> f32 {
        let sample = (value.midpoint(1.0) * (RANGE_INTENSITY_SAMPLES - 1) as f32).round() as usize;
        *meter
            .field()
            .get(sample)
            .expect("test coordinate lies in the signed unit interval")
    }

    /// Checks every externally visible indicator and the retained ring after an unavailable boundary.
    fn expect_clear(meter: &MeterHistory) {
        assert_eq!(meter.snapshot(), MeterReading::default());
        assert_eq!(meter.history_count, 0);
        assert_eq!(meter.history_seconds, 0.0);
    }

    #[test]
    fn first_reading_seeds_without_zero_bias() {
        for value in [-0.75, 0.0, 0.5] {
            for elapsed in [0.0, 1.0 / 60.0, 3.0] {
                let mut meter = MeterHistory::default();
                expect_clear(&meter);
                meter.update(Some(value), elapsed, 0.3);
                assert_eq!(meter.live(), Some(value));
                expect_trend(&meter, f64::from(value));
                assert_eq!(meter.range(), Some((value, value)));
            }
        }
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn analytical_time_constant_and_half_life() {
        let persistence = 0.6;
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, persistence);
        meter.update(Some(1.0), persistence * std::f64::consts::LN_2, persistence);
        expect_trend(&meter, 0.0);
        meter.update(Some(1.0), persistence * std::f64::consts::LN_2, persistence);
        expect_trend(&meter, 0.5);
        meter.clear();
        meter.update(Some(-0.75), 0.0, persistence);
        meter.update(Some(0.75), persistence, persistence);
        expect_trend(&meter, 0.75 - 1.5 * (-1.0_f64).exp());
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn held_response_is_refresh_invariant_including_irregular_steps() {
        let persistence = 0.6;
        let duration = 1.2_f64;
        let expected = 1.0 - 2.0 * (-duration / persistence).exp();
        for frames in [1, 18, 36, 72, 144, 288, 1200] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-1.0), 0.0, persistence);
            for _ in 0..frames {
                meter.update(Some(1.0), duration / f64::from(frames), persistence);
            }
            expect_trend(&meter, expected);
        }
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, persistence);
        for elapsed in [0.01, 0.19, 0.003, 0.497, 0.5] {
            meter.update(Some(1.0), elapsed, persistence);
        }
        expect_trend(&meter, expected);
    }

    #[test]
    fn numeric_selection_and_snapshot_do_not_mutate_history() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-0.75), 0.0, 0.6);
        meter.update(Some(0.75), 0.6, 0.6);
        let before = meter.snapshot();
        for _ in 0..4 {
            assert_eq!(meter.snapshot().value(false), Some(0.75));
            assert_eq!(meter.snapshot().value(true), meter.trend());
            assert_eq!(meter.snapshot(), before);
        }
        meter.update(Some(-0.25), 0.0, 0.6);
        assert_eq!(meter.snapshot().value(false), Some(-0.25));
        assert_eq!(meter.trend(), before.trend);
        assert_eq!(before.live, Some(0.75));
        assert_eq!(meter.range(), Some((-0.75, 0.75)));
    }

    #[test]
    fn high_refresh_and_zero_time_updates_preserve_both_bucket_extremes() {
        let mut meter = MeterHistory::default();
        meter.update(Some(0.1), 0.0, 1.0);
        for value in [-0.9, 0.8, -0.3, 0.2] {
            meter.update(Some(value), HISTORY_BUCKET_SECONDS * 0.125, 1.0);
        }
        assert_eq!(meter.history_count, 1);
        assert_eq!(meter.range(), Some((-0.9, 0.8)));
        for _ in 0..600 {
            meter.update(Some(0.2), HISTORY_BUCKET_SECONDS * 0.125, 1.0);
        }
        assert_eq!(meter.range(), Some((-0.9, 0.8)));
        let trend = meter.trend();
        meter.update(Some(-1.0), 0.0, 1.0);
        meter.update(Some(1.0), 0.0, 1.0);
        assert_eq!(meter.trend(), trend);
        assert_eq!(meter.range(), Some((-1.0, 1.0)));
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Preserves the independently chosen bucket-boundary timing"
    )]
    fn extrema_expiry_is_never_early_and_at_most_one_bucket_late() {
        let persistence = 0.1;
        for phase in [0.125, 0.375, 0.625, 0.875] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-1.0), phase * HISTORY_BUCKET_SECONDS, persistence);
            meter.update(Some(0.0), persistence - HISTORY_BUCKET_SECONDS * 0.01, persistence);
            assert_eq!(meter.range(), Some((-1.0, 0.0)));
            meter.update(Some(0.0), HISTORY_BUCKET_SECONDS * 1.01, persistence);
            assert_eq!(meter.range(), Some((0.0, 0.0)));
        }
    }

    #[test]
    fn minimum_and_maximum_expire_independently() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 0.1);
        meter.update(Some(1.0), 0.05, 0.1);
        meter.update(Some(0.0), 0.025, 0.1);
        assert_eq!(meter.range(), Some((-1.0, 1.0)));
        meter.update(Some(0.0), 0.035, 0.1);
        assert_eq!(meter.range(), Some((0.0, 1.0)));
        meter.update(Some(0.0), 0.05, 0.1);
        assert_eq!(meter.range(), Some((0.0, 0.0)));
    }

    #[test]
    fn persistence_changes_reselect_retained_history_without_advancing_trend() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 3.0);
        meter.update(Some(0.6), 0.5, 3.0);
        meter.update(Some(0.2), 0.1, 3.0);
        let trend = meter.trend();
        meter.update(Some(0.2), 0.0, 0.05);
        assert_eq!(meter.range(), Some((0.2, 0.2)));
        assert_eq!(intensity_at(&meter, -1.0), 0.0);
        meter.update(Some(0.2), 0.0, 1.0);
        assert_eq!(meter.range(), Some((-1.0, 0.6)));
        assert_eq!(meter.trend(), trend);
        assert!(intensity_at(&meter, -1.0) > 0.0);
    }

    #[test]
    fn growing_persistence_cannot_resurrect_more_than_three_seconds() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 0.05);
        meter.update(Some(0.4), 3.0, 0.05);
        meter.update(Some(0.4), HISTORY_BUCKET_SECONDS * 2.0, 0.05);
        meter.update(Some(0.4), 0.0, 3.0);
        assert_eq!(meter.range(), Some((0.4, 0.4)));
        assert_eq!(intensity_at(&meter, -1.0), 0.0);
    }

    #[test]
    fn repeated_equal_extreme_refreshes_its_actual_fade_time() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 0.8);
        meter.update(Some(-1.0), 0.004, 0.8);
        meter.update(Some(1.0), 0.4, 0.8);
        assert!((intensity_at(&meter, -1.0) - 0.5).abs() < 1.0e-6);
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn range_field_fades_analytically_to_zero_without_a_release_tail() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 0.8);
        assert_eq!(intensity_at(&meter, -1.0), 1.0);
        for step in 1..=4 {
            meter.update(Some(1.0), 0.2, 0.8);
            let remaining = 1.0 - step as f32 * 0.25;
            let expected = remaining * remaining * (3.0 - 2.0 * remaining);
            assert!((intensity_at(&meter, -1.0) - expected).abs() < 1.0e-6);
            assert_eq!(intensity_at(&meter, 1.0), 1.0);
        }
        assert_eq!(intensity_at(&meter, -1.0), 0.0);
    }

    #[test]
    fn fading_uses_observation_age_not_refresh_count_or_bucket_phase() {
        for frames in [1, 24, 60, 144, 1000] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-1.0), 0.003, 0.8);
            for _ in 0..frames {
                meter.update(Some(1.0), 0.4 / f64::from(frames), 0.8);
            }
            assert!((intensity_at(&meter, -1.0) - 0.5).abs() < 1.0e-6);
        }
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn fading_edges_are_independent_and_match_cpp_spatial_feather() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 1.0);
        meter.update(Some(1.0), 0.25, 1.0);
        meter.update(Some(0.0), 0.25, 1.0);
        assert!((intensity_at(&meter, -1.0) - 0.5).abs() < 1.0e-6);
        assert!((intensity_at(&meter, 1.0) - 0.84375).abs() < 1.0e-6);
        assert_eq!(intensity_at(&meter, 0.0), 1.0);
        meter.clear();
        meter.update(Some(0.0), 0.0, 0.3);
        let coverage = 1.0 - 0.03125_f32 / 0.06;
        let expected = coverage * coverage * (3.0 - 2.0 * coverage);
        assert_eq!(intensity_at(&meter, 0.03125), expected);
        assert_eq!(intensity_at(&meter, -0.03125), expected);
        assert_eq!(intensity_at(&meter, 0.0625), 0.0);
        assert_eq!(intensity_at(&meter, -0.0625), 0.0);
    }

    #[test]
    fn range_field_reweights_retained_observations_when_persistence_grows() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 3.0);
        meter.update(Some(1.0), 0.4, 3.0);
        meter.update(Some(1.0), 0.0, 0.2);
        assert_eq!(intensity_at(&meter, -1.0), 0.0);
        meter.update(Some(1.0), 0.0, 0.8);
        assert!((intensity_at(&meter, -1.0) - 0.5).abs() < 1.0e-6);
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn persistence_is_clamped_or_defaults_when_nonfinite() {
        for (supplied, effective) in [
            (-100.0, 0.05),
            (0.0, 0.05),
            (0.025, 0.05),
            (0.05, 0.05),
            (0.3, 0.3),
            (3.0, 3.0),
            (6.0, 3.0),
            (f64::MAX, 3.0),
            (f64::NAN, 0.3),
            (f64::INFINITY, 0.3),
            (f64::NEG_INFINITY, 0.3),
        ] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-1.0), 0.0, supplied);
            meter.update(Some(1.0), 0.025, supplied);
            expect_trend(&meter, 1.0 - 2.0 * (-0.025_f64 / effective).exp());
            assert_eq!(meter.range(), Some((-1.0, 1.0)));
            meter.update(Some(1.0), effective, supplied);
            assert_eq!(meter.range(), Some((1.0, 1.0)));
        }
    }

    #[test]
    fn unavailable_stale_and_nonfinite_readings_clear_and_reseed() {
        for invalid in [None, Some(f32::NAN), Some(f32::INFINITY), Some(f32::NEG_INFINITY)] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-0.8), 0.0, 3.0);
            meter.update(Some(0.9), 0.1, 3.0);
            meter.update(invalid, 0.01, 3.0);
            expect_clear(&meter);
            meter.update(Some(0.4), 0.1, 3.0);
            expect_trend(&meter, 0.4);
            assert_eq!(meter.range(), Some((0.4, 0.4)));
            assert_eq!(intensity_at(&meter, -0.8), 0.0);
        }
    }

    #[test]
    fn explicit_clearing_is_idempotent_and_forgets_retained_extrema() {
        let mut meter = MeterHistory::default();
        meter.update(Some(-1.0), 0.0, 3.0);
        meter.update(Some(1.0), 0.1, 3.0);
        for _ in 0..2 {
            meter.clear();
            expect_clear(&meter);
        }
        meter.update(Some(0.25), 0.0, 3.0);
        assert_eq!(meter.range(), Some((0.25, 0.25)));
        expect_trend(&meter, 0.25);
    }

    #[test]
    fn invalid_or_long_timing_reseeds_the_current_finite_value() {
        for elapsed in [
            -0.001,
            f64::NAN,
            f64::INFINITY,
            f64::NEG_INFINITY,
            3.0_f64.next_up(),
            6.0,
            f64::MAX,
        ] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-1.0), 0.0, 3.0);
            meter.update(Some(0.5), 0.05, 3.0);
            meter.update(Some(-0.25), elapsed, 3.0);
            assert_eq!(meter.live(), Some(-0.25));
            expect_trend(&meter, -0.25);
            assert_eq!(meter.range(), Some((-0.25, -0.25)));
            assert_eq!(intensity_at(&meter, -1.0), 0.0);
            assert_eq!(intensity_at(&meter, 0.5), 0.0);
            assert_eq!(meter.history_count, 1);
            assert_eq!(meter.history_seconds, 0.0);
        }
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn zero_and_exactly_three_seconds_are_valid_elapsed_endpoints() {
        for elapsed in [0.0, -0.0, 3.0] {
            let mut meter = MeterHistory::default();
            meter.update(Some(-1.0), 0.0, 3.0);
            meter.update(Some(1.0), elapsed, 3.0);
            expect_trend(&meter, 1.0 - 2.0 * (-elapsed / 3.0).exp());
        }
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn finite_out_of_range_readings_are_clamped_before_history() {
        for sign in [-1.0, 1.0] {
            let mut meter = MeterHistory::default();
            meter.update(Some(sign * f32::MAX), 0.0, 0.3);
            assert_eq!(meter.live(), Some(sign));
            expect_trend(&meter, f64::from(sign));
            assert_eq!(meter.range(), Some((sign, sign)));
            meter.update(Some(-sign * f32::MAX), 0.3, 0.3);
            assert_eq!(meter.live(), Some(-sign));
            expect_trend(&meter, f64::from(sign) * (2.0 * (-1.0_f64).exp() - 1.0));
            assert_eq!(meter.range(), Some((-1.0, 1.0)));
        }
    }

    #[test]
    fn ring_wraps_bound_capacity_and_match_independent_observation_extrema() {
        let mut meter = MeterHistory::default();
        let mut observations = VecDeque::new();
        let windows = [0.05, 0.125, 0.5, 3.0];
        for reading in 0..8192 {
            let now = f64::from(reading) / 256.0;
            let persistence = *windows
                .get((reading / 257) as usize % windows.len())
                .expect("window index is bounded");
            let value = match reading {
                value if value % 257 == 0 => -1.0,
                value if value % 509 == 0 => 1.0,
                _ => (reading % 11 - 5) as f32 * 0.05,
            };
            meter.update(Some(value), if reading == 0 { 0.0 } else { 1.0 / 256.0 }, persistence);
            observations.push_back((now, value));
            while observations
                .front()
                .is_some_and(|&(time, _)| time < now - 3.0 - HISTORY_BUCKET_SECONDS)
            {
                observations.pop_front();
            }
            assert!(meter.history_count <= HISTORY_CAPACITY);
            let trend = meter.trend().expect("valid history has a trend");
            assert!(trend.is_finite() && (-1.0..=1.0).contains(&trend));
            assert!(
                meter
                    .field()
                    .iter()
                    .all(|&intensity| intensity.is_finite() && (0.0..=1.0).contains(&intensity))
            );
            if reading % 127 != 0 {
                continue;
            }
            let mut exact_minimum = value;
            let mut exact_maximum = value;
            let mut extended_minimum = value;
            let mut extended_maximum = value;
            for &(time, observed) in &observations {
                if time >= now - persistence {
                    exact_minimum = exact_minimum.min(observed);
                    exact_maximum = exact_maximum.max(observed);
                }
                if time >= now - persistence - HISTORY_BUCKET_SECONDS {
                    extended_minimum = extended_minimum.min(observed);
                    extended_maximum = extended_maximum.max(observed);
                }
            }
            let (minimum, maximum) = meter.range().expect("valid history has extrema");
            assert!(minimum >= extended_minimum && minimum <= exact_minimum);
            assert!(maximum >= exact_maximum && maximum <= extended_maximum);
        }
    }

    #[test]
    #[expect(
        clippy::suboptimal_flops,
        reason = "Keeps independent analytical expectations in unfused arithmetic"
    )]
    fn range_field_matches_independent_coverage_over_repeated_ring_wraps() {
        let mut meter = MeterHistory::default();
        let mut observations = VecDeque::new();
        let windows = [0.05, 0.25, 0.3, 3.0];
        for reading in 0..2048 {
            let now = f64::from(reading) / 64.0;
            let persistence = *windows
                .get((reading / 257) as usize % windows.len())
                .expect("window index is bounded");
            let value = (f64::from(reading) * 0.031).sin() as f32;
            meter.update(Some(value), if reading == 0 { 0.0 } else { 1.0 / 64.0 }, persistence);
            observations.push_back((now, value));
            while observations.front().is_some_and(|&(time, _)| time < now - 3.0) {
                observations.pop_front();
            }
            if reading % 31 != 0 {
                continue;
            }
            for (sample, &actual) in meter.field().iter().enumerate() {
                let position = -1.0 + sample as f64 / 64.0;
                let mut expected = 0.0_f64;
                for &(time, observed) in &observations {
                    let remaining = (1.0 - (now - time) / persistence).clamp(0.0, 1.0);
                    let opacity = remaining * remaining * (3.0 - 2.0 * remaining);
                    let minimum = f64::from(observed.min(value));
                    let maximum = f64::from(observed.max(value));
                    let distance = (minimum - position).max(position - maximum).max(0.0);
                    let coverage = (1.0 - distance / 0.06).clamp(0.0, 1.0);
                    expected = expected.max(opacity * coverage * coverage * (3.0 - 2.0 * coverage));
                }
                assert!(
                    (f64::from(actual) - expected).abs() < 1.0e-6,
                    "reading {reading}, sample {sample}: {actual} differs from {expected}"
                );
            }
        }
    }
}
