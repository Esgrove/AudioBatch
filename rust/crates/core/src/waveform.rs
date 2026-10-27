//! Waveform overview built from the decoded samples during analysis.
//!
//! `WaveformBuilder` folds every decoded frame into per-channel min and max buckets,
//! merging neighbouring buckets whenever the count doubles past the limit,
//! so a file of any length ends up with a bounded overview of a few thousand buckets.
//! `WaveformOverview` stores the buckets quantized to 8 bits, keeps coarser levels for fast drawing,
//! and serializes to a compact blob for the analysis cache.

use std::sync::Arc;

/// Bucket count at which the builder merges neighbouring buckets.
/// The finished overview has between half this and this many buckets per channel.
const MAX_BUCKETS: usize = 8192;

/// Smallest bucket size in frames, used for very short files.
const MIN_FRAMES_PER_BUCKET: u64 = 64;

/// Scale between a sample in the range -1 to 1 and its 8-bit quantized value.
const QUANTIZATION_SCALE: f32 = 127.0;

/// Identifies a serialized overview blob.
const SERIALIZATION_MAGIC: u32 = 0x5746_5641;

/// Version of the serialized layout, stored in the blob and in the cache row.
pub const SERIALIZATION_VERSION: u32 = 1;

/// Size of the serialized header in bytes.
const HEADER_BYTES: usize = 32;

/// Min and max of one channel over one bucket, quantized to 8 bits.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct QuantizedPeak {
    /// Smallest sample, rounded down.
    pub min: i8,
    /// Largest sample, rounded up.
    pub max: i8,
}

/// Bounded min and max overview of a decoded file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WaveformOverview {
    sample_rate: u32,
    channels: u32,
    length_in_frames: u64,
    /// The finest level first, each following level with half the buckets of the previous one.
    levels: Vec<WaveformLevel>,
}

/// Accumulates decoded audio into overview buckets.
/// Feed it with `push` in decode order and turn it into an overview with `finish`.
#[derive(Debug, Clone)]
pub struct WaveformBuilder {
    channels: usize,
    frames_per_bucket: u64,
    frames_in_bucket: u64,
    current_min: Vec<f32>,
    current_max: Vec<f32>,
    peaks: Vec<QuantizedPeak>,
}

/// One resolution level of the overview.
/// Buckets are stored frame by frame, with one peak per channel in each bucket.
#[derive(Debug, Clone, PartialEq, Eq)]
struct WaveformLevel {
    frames_per_bucket: u64,
    peaks: Vec<QuantizedPeak>,
}

/// Small cursor over a little-endian byte slice, returning `None` when the data runs out.
struct ByteReader<'a> {
    bytes: &'a [u8],
}

impl QuantizedPeak {
    /// Quantizes a sample range, rounding outwards so the overview never hides a peak.
    #[must_use]
    pub fn from_range(min: f32, max: f32) -> Self {
        Self {
            min: quantize(min, f32::floor),
            max: quantize(max, f32::ceil),
        }
    }

    /// Returns the smallest sample as a value from -1 to 1.
    #[must_use]
    pub fn min_value(self) -> f32 {
        f32::from(self.min) / QUANTIZATION_SCALE
    }

    /// Returns the largest sample as a value from -1 to 1.
    #[must_use]
    pub fn max_value(self) -> f32 {
        f32::from(self.max) / QUANTIZATION_SCALE
    }

    /// Returns the peak covering both ranges.
    #[must_use]
    pub fn merge(self, other: Self) -> Self {
        Self {
            min: self.min.min(other.min),
            max: self.max.max(other.max),
        }
    }
}

impl WaveformOverview {
    /// Builds an overview with all coarser levels from the finest buckets.
    fn from_base(sample_rate: u32, channels: u32, length_in_frames: u64, base: WaveformLevel) -> Self {
        let channel_count = channels.max(1) as usize;
        let mut levels = vec![base];
        while let Some(previous) = levels.last() {
            let bucket_count = previous.peaks.len() / channel_count;
            if bucket_count <= 1 {
                break;
            }
            let next = WaveformLevel {
                frames_per_bucket: previous.frames_per_bucket * 2,
                peaks: merge_bucket_pairs(&previous.peaks, channel_count),
            };
            levels.push(next);
        }
        Self {
            sample_rate,
            channels,
            length_in_frames,
            levels,
        }
    }

    /// Restores an overview from a blob written by `to_bytes`.
    /// Returns `None` when the blob is truncated, from another version, or inconsistent.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        let mut reader = ByteReader::new(bytes);
        if reader.u32()? != SERIALIZATION_MAGIC || reader.u32()? != SERIALIZATION_VERSION {
            return None;
        }
        let sample_rate = reader.u32()?;
        let channels = reader.u32()?;
        let length_in_frames = reader.u64()?;
        let frames_per_bucket = u64::from(reader.u32()?);
        let bucket_count = reader.u32()? as usize;
        if channels == 0 || frames_per_bucket == 0 {
            return None;
        }
        if (bucket_count as u64).checked_mul(frames_per_bucket)? < length_in_frames {
            return None;
        }
        let peak_count = bucket_count.checked_mul(channels as usize)?;
        let payload = reader.remaining();
        if payload.len() != peak_count.checked_mul(2)? {
            return None;
        }
        let peaks = payload
            .as_chunks::<2>()
            .0
            .iter()
            .map(|&[min, max]| QuantizedPeak {
                min: i8::from_le_bytes([min]),
                max: i8::from_le_bytes([max]),
            })
            .collect();
        Some(Self::from_base(
            sample_rate,
            channels,
            length_in_frames,
            WaveformLevel {
                frames_per_bucket,
                peaks,
            },
        ))
    }

    /// Serializes the finest level into an explicit little-endian blob for the cache.
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        let base = self.base_level();
        let mut bytes = Vec::with_capacity(HEADER_BYTES + base.peaks.len() * 2);
        bytes.extend_from_slice(&SERIALIZATION_MAGIC.to_le_bytes());
        bytes.extend_from_slice(&SERIALIZATION_VERSION.to_le_bytes());
        bytes.extend_from_slice(&self.sample_rate.to_le_bytes());
        bytes.extend_from_slice(&self.channels.to_le_bytes());
        bytes.extend_from_slice(&self.length_in_frames.to_le_bytes());
        bytes.extend_from_slice(&u32::try_from(base.frames_per_bucket).unwrap_or(u32::MAX).to_le_bytes());
        bytes.extend_from_slice(&u32::try_from(self.bucket_count()).unwrap_or(u32::MAX).to_le_bytes());
        for peak in &base.peaks {
            bytes.extend_from_slice(&peak.min.to_le_bytes());
            bytes.extend_from_slice(&peak.max.to_le_bytes());
        }
        bytes
    }

    /// Returns the sample rate of the source in hertz.
    #[must_use]
    pub const fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// Returns the number of channels.
    #[must_use]
    pub const fn channels(&self) -> u32 {
        self.channels
    }

    /// Returns the source length in frames.
    #[must_use]
    pub const fn length_in_frames(&self) -> u64 {
        self.length_in_frames
    }

    /// Returns the source duration in seconds.
    #[must_use]
    pub fn duration_seconds(&self) -> f64 {
        if self.sample_rate == 0 {
            0.0
        } else {
            self.length_in_frames as f64 / f64::from(self.sample_rate)
        }
    }

    /// Returns the number of buckets per channel at the finest level.
    #[must_use]
    pub fn bucket_count(&self) -> usize {
        self.base_level().peaks.len() / self.channels.max(1) as usize
    }

    /// Returns the number of frames per bucket at the finest level.
    #[must_use]
    pub fn frames_per_bucket(&self) -> u64 {
        self.base_level().frames_per_bucket
    }

    /// Returns the min and max of one channel for each of `columns` equal slices of the frame range,
    /// as values from -1 to 1.
    /// Picks the coarsest level that still has at least one bucket per column, so drawing stays cheap.
    /// Columns outside the source or for a missing channel are silent.
    #[must_use]
    pub fn column_peaks(&self, channel: usize, start_frame: f64, end_frame: f64, columns: usize) -> Vec<(f32, f32)> {
        let mut result = vec![(0.0, 0.0); columns];
        if columns == 0 || end_frame <= start_frame || channel >= self.channels as usize {
            return result;
        }
        let frames_per_column = (end_frame - start_frame) / columns as f64;
        let level = self
            .levels
            .iter()
            .rev()
            .find(|level| level.frames_per_bucket as f64 <= frames_per_column)
            .unwrap_or_else(|| self.base_level());
        let channel_count = self.channels as usize;
        let bucket_count = level.peaks.len() / channel_count;
        let bucket_frames = level.frames_per_bucket as f64;

        for (column, value) in result.iter_mut().enumerate() {
            let column_start = (column as f64).mul_add(frames_per_column, start_frame);
            let column_end = column_start + frames_per_column;
            if column_end <= 0.0 || column_start >= self.length_in_frames as f64 {
                continue;
            }
            let first_bucket = (column_start.max(0.0) / bucket_frames).floor() as usize;
            let last_bucket = ((column_end / bucket_frames).ceil() as usize)
                .max(first_bucket + 1)
                .min(bucket_count);
            let merged = (first_bucket..last_bucket)
                .filter_map(|bucket| level.peaks.get(bucket * channel_count + channel))
                .copied()
                .reduce(QuantizedPeak::merge);
            if let Some(peak) = merged {
                *value = (peak.min_value(), peak.max_value());
            }
        }
        result
    }

    /// Returns the finest level, which every overview has.
    fn base_level(&self) -> &WaveformLevel {
        static EMPTY: WaveformLevel = WaveformLevel {
            frames_per_bucket: 1,
            peaks: Vec::new(),
        };
        self.levels.first().unwrap_or(&EMPTY)
    }
}

impl WaveformBuilder {
    /// Creates a builder for the given channel count.
    /// The expected length, when known, picks a starting bucket size that avoids most merges.
    #[must_use]
    pub fn new(channels: usize, expected_frames: Option<u64>) -> Self {
        let channels = channels.max(1);
        let target = expected_frames.unwrap_or(0) / (MAX_BUCKETS as u64 / 2);
        let frames_per_bucket = target.max(MIN_FRAMES_PER_BUCKET).next_power_of_two();
        Self {
            channels,
            frames_per_bucket,
            frames_in_bucket: 0,
            current_min: vec![0.0; channels],
            current_max: vec![0.0; channels],
            peaks: Vec::new(),
        }
    }

    /// Folds a block of planar samples into the overview.
    /// Every channel slice must have the same number of frames.
    pub fn push(&mut self, channels: &[&[f32]]) {
        let frame_count = channels.first().map_or(0, |samples| samples.len());
        let mut offset = 0;
        while offset < frame_count {
            let remaining_in_bucket = (self.frames_per_bucket - self.frames_in_bucket) as usize;
            let take = remaining_in_bucket.min(frame_count - offset);
            for (channel, samples) in channels.iter().enumerate().take(self.channels) {
                let Some(segment) = samples.get(offset..offset + take) else {
                    continue;
                };
                let (minimum, maximum) = segment
                    .iter()
                    .fold((f32::INFINITY, f32::NEG_INFINITY), |(low, high), &sample| {
                        (low.min(sample), high.max(sample))
                    });
                if let (Some(current_min), Some(current_max)) =
                    (self.current_min.get_mut(channel), self.current_max.get_mut(channel))
                {
                    if self.frames_in_bucket == 0 {
                        *current_min = minimum;
                        *current_max = maximum;
                    } else {
                        *current_min = current_min.min(minimum);
                        *current_max = current_max.max(maximum);
                    }
                }
            }
            self.frames_in_bucket += take as u64;
            offset += take;
            if self.frames_in_bucket == self.frames_per_bucket {
                self.close_bucket();
            }
        }
    }

    /// Finishes the overview, closing a partial final bucket.
    #[must_use]
    pub fn finish(mut self, sample_rate: u32, length_in_frames: u64) -> Arc<WaveformOverview> {
        if self.frames_in_bucket > 0 {
            self.close_bucket();
        }
        Arc::new(WaveformOverview::from_base(
            sample_rate,
            self.channels as u32,
            length_in_frames,
            WaveformLevel {
                frames_per_bucket: self.frames_per_bucket,
                peaks: self.peaks,
            },
        ))
    }

    /// Stores the current bucket and merges bucket pairs when the count reaches the limit.
    fn close_bucket(&mut self) {
        for (&minimum, &maximum) in self.current_min.iter().zip(&self.current_max) {
            self.peaks.push(QuantizedPeak::from_range(minimum, maximum));
        }
        self.frames_in_bucket = 0;
        if self.peaks.len() / self.channels >= MAX_BUCKETS {
            self.peaks = merge_bucket_pairs(&self.peaks, self.channels);
            self.frames_per_bucket *= 2;
        }
    }
}

impl<'a> ByteReader<'a> {
    /// Starts reading at the beginning of the slice.
    const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes }
    }

    /// Reads a little-endian `u32`.
    fn u32(&mut self) -> Option<u32> {
        let (head, rest) = self.bytes.split_first_chunk::<4>()?;
        self.bytes = rest;
        Some(u32::from_le_bytes(*head))
    }

    /// Reads a little-endian `u64`.
    fn u64(&mut self) -> Option<u64> {
        let (head, rest) = self.bytes.split_first_chunk::<8>()?;
        self.bytes = rest;
        Some(u64::from_le_bytes(*head))
    }

    /// Returns the unread bytes.
    const fn remaining(&self) -> &'a [u8] {
        self.bytes
    }
}

/// Quantizes a sample to 8 bits with the given rounding, clamping values outside -1 to 1.
fn quantize(sample: f32, round: fn(f32) -> f32) -> i8 {
    if !sample.is_finite() {
        return 0;
    }
    round(sample * QUANTIZATION_SCALE).clamp(-QUANTIZATION_SCALE, QUANTIZATION_SCALE) as i8
}

/// Merges neighbouring buckets pairwise, halving the bucket count.
/// A final unpaired bucket is kept as it is.
fn merge_bucket_pairs(peaks: &[QuantizedPeak], channels: usize) -> Vec<QuantizedPeak> {
    peaks
        .chunks(channels * 2)
        .flat_map(|pair| {
            let (first, second) = pair.split_at(channels.min(pair.len()));
            (0..first.len()).map(move |channel| match (first.get(channel), second.get(channel)) {
                (Some(left), Some(right)) => left.merge(*right),
                (Some(left), None) => *left,
                _ => QuantizedPeak::default(),
            })
        })
        .collect()
}

#[cfg(test)]
mod test_waveform {
    use super::*;

    fn sine(frames: usize, amplitude: f32) -> Vec<f32> {
        (0..frames)
            .map(|index| amplitude * (index as f32 * 0.05).sin())
            .collect()
    }

    #[test]
    fn quantization_rounds_outwards() {
        let peak = QuantizedPeak::from_range(-0.5, 0.5);
        assert!(peak.min_value() <= -0.5);
        assert!(peak.max_value() >= 0.5);
        assert_eq!(
            QuantizedPeak::from_range(-2.0, 2.0),
            QuantizedPeak { min: -127, max: 127 }
        );
    }

    #[test]
    fn bucket_count_stays_bounded_for_long_input() {
        let mut builder = WaveformBuilder::new(2, None);
        let block = sine(4096, 0.5);
        for _ in 0..2000 {
            builder.push(&[&block, &block]);
        }
        let overview = builder.finish(44_100, 4096 * 2000);
        assert!(overview.bucket_count() <= MAX_BUCKETS);
        assert!(overview.bucket_count() >= MAX_BUCKETS / 4);
        assert_eq!(overview.channels(), 2);
    }

    #[test]
    fn column_peaks_match_signal_amplitude() {
        let mut builder = WaveformBuilder::new(1, Some(44_100));
        let block = sine(44_100, 0.5);
        builder.push(&[&block]);
        let overview = builder.finish(44_100, 44_100);
        let columns = overview.column_peaks(0, 0.0, 44_100.0, 100);
        assert_eq!(columns.len(), 100);
        for (minimum, maximum) in columns {
            assert!((maximum - 0.5).abs() < 0.02, "max {maximum}");
            assert!((minimum + 0.5).abs() < 0.02, "min {minimum}");
        }
    }

    #[test]
    fn serialization_round_trips() {
        let mut builder = WaveformBuilder::new(2, Some(10_000));
        let left = sine(10_000, 0.25);
        let right = sine(10_000, 0.75);
        builder.push(&[&left, &right]);
        let overview = builder.finish(48_000, 10_000);
        let restored = WaveformOverview::from_bytes(&overview.to_bytes()).expect("valid blob");
        assert_eq!(*overview, restored);
    }

    #[test]
    fn corrupt_blobs_are_rejected() {
        assert!(WaveformOverview::from_bytes(&[]).is_none());
        let mut builder = WaveformBuilder::new(1, None);
        builder.push(&[&sine(1000, 0.5)]);
        let mut bytes = builder.finish(44_100, 1000).to_bytes();
        bytes.pop();
        assert!(WaveformOverview::from_bytes(&bytes).is_none());

        let mut empty = WaveformBuilder::new(1, None).finish(44_100, 0).to_bytes();
        empty.splice(16..24, 1000_u64.to_le_bytes());
        assert!(WaveformOverview::from_bytes(&empty).is_none());
    }

    #[test]
    fn missing_channel_and_empty_range_are_silent() {
        let mut builder = WaveformBuilder::new(1, None);
        builder.push(&[&sine(1000, 0.5)]);
        let overview = builder.finish(44_100, 1000);
        assert!(
            overview
                .column_peaks(1, 0.0, 1000.0, 4)
                .iter()
                .all(|&(low, high)| low == 0.0 && high == 0.0)
        );
        assert!(
            overview
                .column_peaks(0, 10.0, 10.0, 4)
                .iter()
                .all(|&(low, high)| low == 0.0 && high == 0.0)
        );
    }
}
