//! Differential tests that run the ebur128 reference and our kernels on the same input and compare the bits.
//!
//! The reference feeds ebur128 the way the analysis always has, in 100 ms blocks with mono as dual mono.
//! The inputs cover every signal kind, the rates where the true peak oversampling changes,
//! one to eight channels, and random chunk sizes from a seeded generator.
//! Every value is compared exactly, except the short-term loudness, whose summation order differs on purpose.

use ebur128::{Channel, EbuR128, Mode};

use crate::golden::measure_signal;
use crate::record::{ShortTermLoudnessHistory, normalize_loudness};
use crate::signals::{SignalKind, Xorshift, generate};

/// One input of the differential tests.
pub struct Case {
    /// Planar samples, one vector per channel.
    pub planes: Vec<Vec<f32>>,
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// Readable description for failure messages.
    pub label: String,
}

/// What the reference measured.
pub struct Reference {
    /// True peak of each channel.
    pub true_peaks: Vec<f64>,
    /// Integrated loudness in LUFS, with the silence sentinel.
    pub integrated_lufs: f64,
    /// Short-term loudness after each 100 ms block, in LUFS, with the silence sentinel.
    pub short_term: Vec<f64>,
}

impl Case {
    /// Generates the case.
    pub fn new(kind: SignalKind, sample_rate: u32, channels: usize, seconds: f64) -> Self {
        let frames = (f64::from(sample_rate) * seconds) as usize;
        Self {
            planes: generate(kind, sample_rate, channels, frames),
            sample_rate,
            label: format!("{kind:?} at {sample_rate} Hz with {channels} channels"),
        }
    }

    /// Returns the number of frames.
    pub fn frames(&self) -> usize {
        self.planes.first().map_or(0, Vec::len)
    }

    /// Returns the number of channels.
    pub fn channels(&self) -> usize {
        self.planes.len()
    }

    /// Splits the frames into random chunks from a seeded generator, from one frame to a little over a block.
    pub fn random_chunks(&self, seed: u64) -> Vec<std::ops::Range<usize>> {
        let mut random = Xorshift::new(seed);
        let block = u64::from(ShortTermLoudnessHistory::samples_per_point_for_rate(self.sample_rate));
        let mut chunks = Vec::new();
        let mut start = 0;
        while start < self.frames() {
            let end = (start + 1 + random.next_below(block + 64) as usize).min(self.frames());
            chunks.push(start..end);
            start = end;
        }
        chunks
    }

    /// Returns the planes of one chunk.
    pub fn chunk(&self, range: std::ops::Range<usize>) -> Vec<&[f32]> {
        self.planes.iter().map(|plane| &plane[range.clone()]).collect()
    }
}

/// Returns the cases every kernel is compared on.
///
/// Stereo and mono music are what the application measures, so they get every signal at the rates
/// where the true peak oversampling changes, with stereo long enough to fill the 3 s short-term window.
/// Wider files only take the generic path, so a short case of each kind of channel map is enough.
pub fn cases() -> Vec<Case> {
    let mut cases = Vec::new();
    for kind in SignalKind::ALL {
        for rate in [22_050, 44_100, 48_000, 96_000, 192_000] {
            cases.push(Case::new(kind, rate, 2, 3.2));
        }
        for rate in [44_100, 88_200, 176_400] {
            cases.push(Case::new(kind, rate, 1, 0.5));
        }
    }
    for channels in [3, 4, 6] {
        cases.push(Case::new(SignalKind::MusicLike, 48_000, channels, 0.5));
    }
    cases
}

/// Measures the case with ebur128 the way the analysis always has, in 100 ms blocks.
pub fn reference(case: &Case) -> Reference {
    let channels = case.channels() as u32;
    let mut meter = EbuR128::new(channels, case.sample_rate, Mode::I | Mode::S | Mode::TRUE_PEAK).expect("meter");
    if channels == 1 {
        meter.set_channel(0, Channel::DualMono).expect("dual mono");
    }
    let block = ShortTermLoudnessHistory::samples_per_point_for_rate(case.sample_rate) as usize;
    let mut short_term = Vec::new();
    let mut start = 0;
    while start < case.frames() {
        let end = (start + block).min(case.frames());
        meter.add_frames_planar_f32(&case.chunk(start..end)).expect("frames");
        short_term.push(normalize_loudness(meter.loudness_shortterm().expect("short-term")));
        start = end;
    }
    Reference {
        true_peaks: (0..channels)
            .map(|channel| meter.true_peak(channel).expect("true peak"))
            .collect(),
        integrated_lufs: normalize_loudness(meter.loudness_global().expect("integrated")),
        short_term,
    }
}

/// Fails the test when any value differs from the reference in any bit.
pub fn assert_bits_equal(label: &str, what: &str, reference: &[f64], candidate: &[f64]) {
    assert_eq!(reference.len(), candidate.len(), "{label}: {what} count");
    for (index, (expected, actual)) in reference.iter().zip(candidate).enumerate() {
        assert_eq!(
            expected.to_bits(),
            actual.to_bits(),
            "{label}: {what}[{index}] is {actual:?}, the reference {expected:?}"
        );
    }
}

/// Fails the test when any value differs from the reference by more than the tolerance in LU,
/// or when one of them is the silence sentinel and the other is not.
pub fn assert_within(label: &str, what: &str, reference: &[f64], candidate: &[f64], tolerance: f64) {
    assert_eq!(reference.len(), candidate.len(), "{label}: {what} count");
    for (index, (expected, actual)) in reference.iter().zip(candidate).enumerate() {
        assert!(
            (expected - actual).abs() <= tolerance,
            "{label}: {what}[{index}] is {actual:?}, the reference {expected:?}"
        );
    }
}

#[cfg(test)]
mod test_harness {
    use super::*;

    #[test]
    fn the_block_meter_matches_the_reference() {
        for case in cases() {
            let reference = reference(&case);
            let output = measure_signal(&case.planes, case.sample_rate, 1_000);
            assert_bits_equal(&case.label, "true peak", &reference.true_peaks, &output.true_peaks);
            assert_bits_equal(
                &case.label,
                "integrated",
                &[reference.integrated_lufs],
                &[output.integrated_lufs],
            );
            let history: Vec<f64> = reference
                .short_term
                .iter()
                .map(|&value| f64::from(value as f32))
                .collect();
            let measured: Vec<f64> = output.history.iter().copied().map(f64::from).collect();
            assert_bits_equal(&case.label, "short-term", &history, &measured);
        }
    }

    #[test]
    fn random_chunks_cover_every_frame_once() {
        let case = Case::new(SignalKind::Noise, 44_100, 2, 0.5);
        let chunks = case.random_chunks(3);
        assert_eq!(chunks.first().map(|range| range.start), Some(0));
        assert_eq!(chunks.last().map(|range| range.end), Some(case.frames()));
        assert!(chunks.windows(2).all(|pair| pair[0].end == pair[1].start));
    }

    #[test]
    fn differences_in_the_last_bit_fail() {
        let result = std::panic::catch_unwind(|| {
            assert_bits_equal("case", "value", &[1.0], &[f64::from_bits(1.0_f64.to_bits() + 1)]);
        });
        assert!(result.is_err());
    }

    #[test]
    fn differences_beyond_the_tolerance_fail() {
        assert_within("case", "value", &[-20.0], &[-20.000_1], 0.001);
        let result = std::panic::catch_unwind(|| {
            assert_within("case", "value", &[-20.0], &[-20.01], 0.001);
        });
        assert!(result.is_err());
    }
}
