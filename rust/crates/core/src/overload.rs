//! Bounded per-channel source overload history and its validated cache representation.
//!
//! Each analysis block contributes sample and true peak flags, with neighbouring buckets merged by bitwise or
//! when the history would exceed its limit.
//! The Rust cache uses an independent integer-only blob, not the C++ cache representation.
//! Its little-endian header holds the bytes "AOVR", a `u32` version, `u32` sample rate and channel count,
//! `u64` source length and bucket stride, and a `u32` bucket count, followed by bucket-major flag bytes.

use crate::record::ShortTermLoudnessHistory;

/// A decoded sample whose magnitude exceeds full scale.
pub const SAMPLE_OVER: u8 = 1;
/// A block-local true peak whose magnitude exceeds full scale.
pub const TRUE_PEAK_OVER: u8 = 2;
/// Largest number of time buckets, independent of the source length.
pub const MAXIMUM_BUCKETS: usize = 8192;

/// Every flag bit defined by this representation.
const ALL_FLAGS: u8 = SAMPLE_OVER | TRUE_PEAK_OVER;
/// Most source channels accepted by the meter.
const MAXIMUM_CHANNELS: u32 = 64;
/// Largest sample rate represented by the meter's stream parameters.
const MAXIMUM_SAMPLE_RATE: u32 = i32::MAX as u32;
/// Identifies this integer-only representation, the bytes "AOVR" in little-endian order.
const SERIALIZATION_MAGIC: u32 = 0x5256_4f41;
/// Version of the overload blob, independent of the enclosing cache schema.
const SERIALIZATION_VERSION: u32 = 1;
/// Fixed header size, before the flag payload.
const HEADER_BYTES: usize = 36;

/// Source overloads in time order, with one flag byte per channel in each bucket.
///
/// Sample flags describe decoded samples above full scale, not samples exactly at full scale.
/// True peak flags describe the block-local maxima reported by the meter, not precise peak positions.
/// An all-clear history, including an empty stream, is distinct from a missing history.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverloadHistory {
    /// Sample rate of the source in hertz.
    pub sample_rate: u32,
    /// Source length in frames, with one sample per channel in each frame.
    pub length_in_samples: u64,
    /// Frames per bucket, with a possibly shorter final bucket.
    pub samples_per_bucket: u64,
    /// Number of actual source channels, without dual-mono duplication.
    pub channels: u32,
    /// Bucket-major flags, with `channels` consecutive bytes per bucket.
    pub flags: Vec<u8>,
}

/// Accumulates the flags of consecutive analysis blocks without losing isolated overloads.
/// The pending bucket retains its existing blocks when merging doubles the bucket stride.
/// Public only so benchmarks measure the production collector separately from the full meter.
#[doc(hidden)]
#[derive(Debug)]
pub struct OverloadHistoryBuilder {
    history: OverloadHistory,
    pending: Vec<u8>,
    blocks_per_bucket: u64,
    pending_blocks: u64,
    blocks_received: u64,
}

/// Allocation-free cursor for decoding a fixed-size little-endian header.
struct ByteReader<'a> {
    bytes: &'a [u8],
}

impl OverloadHistory {
    /// Restores a history only after validating its complete header and payload.
    /// Rejects unknown versions, trailing data, illegal flag bits, and noncanonical bucket strides.
    /// No flag storage is allocated until every validation has passed.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        let mut reader = ByteReader { bytes };
        if reader.u32()? != SERIALIZATION_MAGIC || reader.u32()? != SERIALIZATION_VERSION {
            return None;
        }
        let sample_rate = reader.u32()?;
        let channels = reader.u32()?;
        let length_in_samples = reader.u64()?;
        let samples_per_bucket = reader.u64()?;
        let buckets = usize::try_from(reader.u32()?).ok()?;
        let expected = expected_bucket_count(sample_rate, length_in_samples, samples_per_bucket, channels)?;
        let payload_bytes = buckets.checked_mul(usize::try_from(channels).ok()?)?;
        if buckets != expected
            || bytes.len() != HEADER_BYTES.checked_add(payload_bytes)?
            || reader.bytes.len() != payload_bytes
            || !legal_flags(reader.bytes)
        {
            return None;
        }
        Some(Self {
            sample_rate,
            length_in_samples,
            samples_per_bucket,
            channels,
            flags: reader.bytes.to_vec(),
        })
    }

    /// Returns the number of time buckets, or zero when no channels are declared.
    /// A malformed partial bucket is excluded, and `is_valid` rejects it.
    #[must_use]
    pub const fn bucket_count(&self) -> usize {
        match self.flags.len().checked_div(self.channels as usize) {
            Some(buckets) => buckets,
            None => 0,
        }
    }

    /// Checks the stream parameters, exact payload size, legal flags, and minimal bounded stride.
    /// The stride must be the smallest power-of-two multiple of the analysis block size
    /// that represents the entire source in at most `MAXIMUM_BUCKETS` buckets.
    #[must_use]
    pub fn is_valid(&self) -> bool {
        expected_bucket_count(
            self.sample_rate,
            self.length_in_samples,
            self.samples_per_bucket,
            self.channels,
        )
        .and_then(|buckets| buckets.checked_mul(self.channels as usize))
        .is_some_and(|expected| self.flags.len() == expected && legal_flags(&self.flags))
    }

    /// Returns true only when a valid history belongs to the supplied source stream.
    #[must_use]
    pub fn matches_stream(&self, sample_rate: u32, length: u64, channels: u32) -> bool {
        self.sample_rate == sample_rate
            && self.length_in_samples == length
            && self.channels == channels
            && self.is_valid()
    }

    /// Serializes a valid history to an explicit little-endian blob.
    /// Public fields may be edited by callers, so inconsistent histories are rejected rather than persisted.
    #[must_use]
    pub fn to_bytes(&self) -> Option<Vec<u8>> {
        if !self.is_valid() {
            return None;
        }
        let buckets = u32::try_from(self.bucket_count()).ok()?;
        let mut bytes = Vec::with_capacity(HEADER_BYTES.checked_add(self.flags.len())?);
        bytes.extend_from_slice(&SERIALIZATION_MAGIC.to_le_bytes());
        bytes.extend_from_slice(&SERIALIZATION_VERSION.to_le_bytes());
        bytes.extend_from_slice(&self.sample_rate.to_le_bytes());
        bytes.extend_from_slice(&self.channels.to_le_bytes());
        bytes.extend_from_slice(&self.length_in_samples.to_le_bytes());
        bytes.extend_from_slice(&self.samples_per_bucket.to_le_bytes());
        bytes.extend_from_slice(&buckets.to_le_bytes());
        bytes.extend_from_slice(&self.flags);
        Some(bytes)
    }
}

impl OverloadHistoryBuilder {
    /// Starts an empty history for 1 through 64 source channels and a rate from 1 through `i32::MAX`.
    /// Invalid stream parameters allocate no flag storage and produce an invalid history on finish.
    #[must_use]
    pub fn new(channels: u32, sample_rate: u32) -> Self {
        let valid = (1..=MAXIMUM_CHANNELS).contains(&channels) && (1..=MAXIMUM_SAMPLE_RATE).contains(&sample_rate);
        let channel_count = if valid { channels as usize } else { 0 };
        Self {
            history: OverloadHistory {
                sample_rate,
                length_in_samples: 0,
                samples_per_bucket: u64::from(ShortTermLoudnessHistory::samples_per_point_for_rate(sample_rate)),
                channels,
                flags: Vec::with_capacity(MAXIMUM_BUCKETS * channel_count),
            },
            pending: vec![0; channel_count],
            blocks_per_bucket: 1,
            pending_blocks: 0,
            blocks_received: 0,
        }
    }

    /// Folds one analysis block into the pending bucket, including the source's final partial block.
    /// Missing channels contribute no flags, extra channels are ignored, and unknown flag bits are masked out.
    pub fn push(&mut self, block_flags: &[u8]) {
        if self.pending.is_empty() || self.history.samples_per_bucket == 0 {
            return;
        }
        let Some(blocks_received) = self.blocks_received.checked_add(1) else {
            self.history.samples_per_bucket = 0;
            return;
        };
        self.blocks_received = blocks_received;
        for (pending, &flags) in self.pending.iter_mut().zip(block_flags) {
            *pending |= flags & ALL_FLAGS;
        }
        self.pending_blocks += 1;
        if self.pending_blocks < self.blocks_per_bucket {
            return;
        }
        if self.history.bucket_count() == MAXIMUM_BUCKETS {
            // The pending bucket is now the first half of a bucket at the doubled stride.
            self.merge_pairs();
        } else {
            self.close_pending();
        }
    }

    /// Flushes a final incomplete bucket and supplies the source length in frames.
    /// The caller supplies one block per push, with only the final block allowed to be shorter than the base stride.
    /// An inconsistent source length produces a history that fails `is_valid`.
    #[must_use]
    pub fn finish(mut self, length_in_samples: u64) -> OverloadHistory {
        self.history.length_in_samples = length_in_samples;
        if self.pending_blocks != 0 {
            if self.history.bucket_count() == MAXIMUM_BUCKETS {
                self.merge_pairs();
            }
            self.close_pending();
        }
        let block_size = u64::from(ShortTermLoudnessHistory::samples_per_point_for_rate(
            self.history.sample_rate,
        ));
        if self.blocks_received != length_in_samples.div_ceil(block_size) {
            self.history.samples_per_bucket = 0;
        }
        self.history
    }

    /// Appends the pending bucket without changing its stride and resets its accumulation state.
    fn close_pending(&mut self) {
        self.history.flags.extend_from_slice(&self.pending);
        self.pending.fill(0);
        self.pending_blocks = 0;
    }

    /// Merges neighbouring complete buckets in place while leaving the pending blocks untouched.
    /// Arithmetic exhaustion marks the history invalid instead of wrapping the stride.
    fn merge_pairs(&mut self) {
        let channels = self.pending.len();
        let merged_bytes = self.history.flags.len() / 2;
        for destination in 0..merged_bytes {
            let bucket = destination / channels;
            let channel = destination % channels;
            let source = bucket * channels * 2 + channel;
            let flags = self.history.flags.get(source).copied().unwrap_or(0)
                | self.history.flags.get(source + channels).copied().unwrap_or(0);
            if let Some(value) = self.history.flags.get_mut(destination) {
                *value = flags;
            }
        }
        self.history.flags.truncate(merged_bytes);
        self.blocks_per_bucket = self.blocks_per_bucket.checked_mul(2).unwrap_or(0);
        self.history.samples_per_bucket = self.history.samples_per_bucket.checked_mul(2).unwrap_or(0);
    }
}

impl ByteReader<'_> {
    /// Reads one little-endian 32-bit integer, returning `None` for an incomplete field.
    fn u32(&mut self) -> Option<u32> {
        let (value, remaining) = self.bytes.split_first_chunk::<4>()?;
        self.bytes = remaining;
        Some(u32::from_le_bytes(*value))
    }

    /// Reads one little-endian 64-bit integer, returning `None` for an incomplete field.
    fn u64(&mut self) -> Option<u64> {
        let (value, remaining) = self.bytes.split_first_chunk::<8>()?;
        self.bytes = remaining;
        Some(u64::from_le_bytes(*value))
    }
}

/// Validates the timing and channel metadata and returns its exact bounded bucket count.
/// Ceiling division avoids overflowing at source lengths near `u64::MAX`.
fn expected_bucket_count(sample_rate: u32, length: u64, stride: u64, channels: u32) -> Option<usize> {
    if !(1..=MAXIMUM_CHANNELS).contains(&channels) || !(1..=MAXIMUM_SAMPLE_RATE).contains(&sample_rate) {
        return None;
    }
    let block_size = u64::from(ShortTermLoudnessHistory::samples_per_point_for_rate(sample_rate));
    if stride < block_size || !stride.is_multiple_of(block_size) {
        return None;
    }
    let blocks_per_bucket = stride / block_size;
    if !blocks_per_bucket.is_power_of_two() {
        return None;
    }
    let blocks = length.div_ceil(block_size);
    let buckets = blocks.div_ceil(blocks_per_bucket);
    if buckets > MAXIMUM_BUCKETS as u64
        || (blocks_per_bucket > 1 && blocks.div_ceil(blocks_per_bucket / 2) <= MAXIMUM_BUCKETS as u64)
    {
        return None;
    }
    usize::try_from(buckets).ok()
}

/// Returns true when all bytes hold only flags defined by this blob version.
fn legal_flags(flags: &[u8]) -> bool {
    flags.iter().all(|&flags| flags & !ALL_FLAGS == 0)
}

#[cfg(test)]
mod test_builder {
    use super::*;

    #[test]
    fn empty_and_all_clear_streams_are_valid_for_actual_channels() {
        for channels in [1, 2, MAXIMUM_CHANNELS] {
            let empty = OverloadHistoryBuilder::new(channels, 44_100).finish(0);
            assert!(empty.is_valid());
            assert_eq!(empty.flags, [] as [u8; 0]);
            assert_eq!(empty.bucket_count(), 0);
            assert_eq!(empty.samples_per_bucket, 4410);
            let bytes = empty.to_bytes().expect("valid empty history");
            assert_eq!(OverloadHistory::from_bytes(&bytes), Some(empty));

            let mut builder = OverloadHistoryBuilder::new(channels, 44_100);
            builder.push(&[0; MAXIMUM_CHANNELS as usize]);
            let clear = builder.finish(7);
            assert!(clear.is_valid());
            assert_eq!(clear.flags, vec![0; channels as usize]);
            assert!(clear.matches_stream(44_100, 7, channels));
            assert!(!clear.matches_stream(48_000, 7, channels));
            assert!(!clear.matches_stream(44_100, 8, channels));
            assert!(!clear.matches_stream(44_100, 7, channels + 1));
            let bytes = clear.to_bytes().expect("valid all-clear history");
            assert_eq!(OverloadHistory::from_bytes(&bytes), Some(clear));
        }
    }

    #[test]
    fn block_flags_remain_bucket_major_and_keep_the_final_partial_block() {
        let mut builder = OverloadHistoryBuilder::new(3, 44_100);
        builder.push(&[SAMPLE_OVER, 0, TRUE_PEAK_OVER]);
        builder.push(&[0, ALL_FLAGS, 0]);
        builder.push(&[0, SAMPLE_OVER, TRUE_PEAK_OVER]);
        let history = builder.finish(2 * 4410 + 7);
        assert!(history.is_valid());
        assert_eq!(history.bucket_count(), 3);
        assert_eq!(history.channels, 3);
        assert_eq!(history.samples_per_bucket, 4410);
        assert_eq!(
            history.flags,
            [
                SAMPLE_OVER,
                0,
                TRUE_PEAK_OVER,
                0,
                ALL_FLAGS,
                0,
                0,
                SAMPLE_OVER,
                TRUE_PEAK_OVER
            ]
        );
        let bytes = history.to_bytes().expect("valid multichannel history");
        assert_eq!(OverloadHistory::from_bytes(&bytes), Some(history));
    }

    #[test]
    fn several_merges_preserve_isolated_flags_and_pending_channel_state() {
        let blocks = MAXIMUM_BUCKETS * 5 + 13;
        let mut builder = OverloadHistoryBuilder::new(3, 44_100);
        let mut expected = vec![0; blocks.div_ceil(8) * 3];
        for block in 0..blocks {
            let flags = [
                if block == 1 || block == MAXIMUM_BUCKETS * 4 + 9 {
                    SAMPLE_OVER
                } else {
                    0
                },
                if block == MAXIMUM_BUCKETS || block == blocks - 1 {
                    TRUE_PEAK_OVER
                } else {
                    0
                },
                if block == MAXIMUM_BUCKETS * 2 + 1 { ALL_FLAGS } else { 0 },
            ];
            builder.push(&flags);
            for (channel, flags) in flags.into_iter().enumerate() {
                *expected.get_mut((block / 8) * 3 + channel).expect("reference bucket") |= flags;
            }
            assert!(builder.history.bucket_count() <= MAXIMUM_BUCKETS);
            assert!(builder.history.flags.len() <= MAXIMUM_BUCKETS * 3);
        }
        let history = builder.finish((blocks as u64 - 1) * 4410 + 1);
        assert!(history.is_valid());
        assert_eq!(history.samples_per_bucket, 4410 * 8);
        assert_eq!(history.flags, expected);
    }

    #[test]
    fn finishing_at_the_limit_and_immediately_after_merging_is_canonical() {
        for (blocks, stride) in [
            (MAXIMUM_BUCKETS, 1),
            (MAXIMUM_BUCKETS + 1, 2),
            (MAXIMUM_BUCKETS * 2 + 1, 4),
            (MAXIMUM_BUCKETS * 2 + 2, 4),
            (MAXIMUM_BUCKETS * 4 + 1, 8),
            (MAXIMUM_BUCKETS * 4 + 4, 8),
        ] {
            let mut builder = OverloadHistoryBuilder::new(1, 10);
            for block in 0..blocks {
                builder.push(&[if block == blocks - 1 { SAMPLE_OVER } else { 0 }]);
            }
            let history = builder.finish(blocks as u64);
            assert!(history.is_valid(), "{blocks} blocks");
            assert_eq!(history.samples_per_bucket, stride);
            assert_eq!(history.bucket_count(), blocks.div_ceil(stride as usize));
            assert_eq!(history.flags.last(), Some(&SAMPLE_OVER));
            assert_eq!(history.flags.iter().filter(|&&flags| flags != 0).count(), 1);
        }
    }

    #[test]
    fn input_flags_are_masked_and_invalid_stream_parameters_are_not_adopted() {
        let mut builder = OverloadHistoryBuilder::new(2, 44_100);
        builder.push(&[0xff, TRUE_PEAK_OVER, SAMPLE_OVER]);
        builder.push(&[SAMPLE_OVER]);
        let history = builder.finish(4411);
        assert!(history.is_valid());
        assert_eq!(history.flags, [ALL_FLAGS, TRUE_PEAK_OVER, SAMPLE_OVER, 0]);
        for (channels, rate) in [(0, 44_100), (65, 44_100), (u32::MAX, 44_100), (1, 0), (1, u32::MAX)] {
            let mut builder = OverloadHistoryBuilder::new(channels, rate);
            builder.push(&[ALL_FLAGS]);
            assert!(!builder.finish(1).is_valid());
        }
        assert!(!OverloadHistoryBuilder::new(1, 44_100).finish(1).is_valid());
        let mut builder = OverloadHistoryBuilder::new(1, 10);
        for _ in 0..=MAXIMUM_BUCKETS {
            builder.push(&[0]);
        }
        // Both lengths imply the same merged payload size, but only one matches the supplied blocks.
        assert!(!builder.finish(MAXIMUM_BUCKETS as u64 + 2).is_valid());
    }
}

#[cfg(test)]
mod test_validation {
    use super::*;

    /// Returns a valid single-bucket history for mutation checks.
    fn history() -> OverloadHistory {
        let mut builder = OverloadHistoryBuilder::new(1, 44_100);
        builder.push(&[SAMPLE_OVER]);
        builder.finish(4410)
    }

    /// Replaces one header field or payload byte without relying on unchecked indexing.
    fn replace(bytes: &[u8], offset: usize, value: &[u8]) -> Vec<u8> {
        let mut changed = bytes.to_vec();
        changed
            .get_mut(offset..offset + value.len())
            .expect("known blob field")
            .copy_from_slice(value);
        changed
    }

    #[test]
    fn rejects_every_truncation_trailing_data_and_unknown_blob_versions() {
        let bytes = history().to_bytes().expect("valid history");
        for length in 0..bytes.len() {
            assert!(OverloadHistory::from_bytes(bytes.get(..length).expect("prefix")).is_none());
        }
        let mut trailing = bytes.clone();
        trailing.push(0);
        assert!(OverloadHistory::from_bytes(&trailing).is_none());
        assert!(OverloadHistory::from_bytes(&replace(&bytes, 0, &[0])).is_none());
        assert!(OverloadHistory::from_bytes(&replace(&bytes, 4, &2_u32.to_le_bytes())).is_none());
        assert!(OverloadHistory::from_bytes(&replace(&bytes, HEADER_BYTES, &[4])).is_none());
    }

    #[test]
    fn rejects_bad_rates_channels_bucket_counts_and_lengths_before_adoption() {
        let bytes = history().to_bytes().expect("valid history");
        for (offset, values) in [
            (8, vec![0, MAXIMUM_SAMPLE_RATE + 1, u32::MAX]),
            (12, vec![0, MAXIMUM_CHANNELS + 1, u32::MAX]),
            (32, vec![0, 2, MAXIMUM_BUCKETS as u32 + 1, u32::MAX]),
        ] {
            for value in values {
                assert!(OverloadHistory::from_bytes(&replace(&bytes, offset, &value.to_le_bytes())).is_none());
            }
        }
        for length in [0, 4411, u64::MAX] {
            assert!(OverloadHistory::from_bytes(&replace(&bytes, 16, &length.to_le_bytes())).is_none());
        }
    }

    #[test]
    fn rejects_illegal_or_unnecessarily_coarse_strides_and_mutated_histories() {
        let original = history();
        let bytes = original.to_bytes().expect("valid history");
        assert_eq!(
            bytes,
            [
                0x41,
                0x4f,
                0x56,
                0x52,
                1,
                0,
                0,
                0,
                0x44,
                0xac,
                0,
                0,
                1,
                0,
                0,
                0,
                0x3a,
                0x11,
                0,
                0,
                0,
                0,
                0,
                0,
                0x3a,
                0x11,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                SAMPLE_OVER,
            ]
        );
        for stride in [0, 4409, 4411, 4410 * 2, 4410 * 3, u64::MAX] {
            assert!(OverloadHistory::from_bytes(&replace(&bytes, 24, &stride.to_le_bytes())).is_none());
            let mut changed = original.clone();
            changed.samples_per_bucket = stride;
            assert!(!changed.is_valid());
            assert!(changed.to_bytes().is_none());
        }
        let mut changed = original.clone();
        changed.flags = vec![4];
        assert!(changed.to_bytes().is_none());
        assert!(!changed.matches_stream(44_100, 4410, 1));
        changed = original.clone();
        changed.channels = 0;
        assert_eq!(changed.bucket_count(), 0);
        assert!(changed.to_bytes().is_none());
        changed = original.clone();
        changed.flags.push(0);
        assert!(changed.to_bytes().is_none());
        changed = original;
        changed.length_in_samples = MAXIMUM_BUCKETS as u64 + 1;
        changed.sample_rate = 1;
        changed.samples_per_bucket = 1;
        changed.flags = vec![0; MAXIMUM_BUCKETS + 1];
        assert!(!changed.is_valid());
    }

    #[test]
    fn maximum_source_length_uses_overflow_safe_ceiling_division() {
        let history = OverloadHistory {
            sample_rate: 1,
            length_in_samples: u64::MAX,
            samples_per_bucket: 1 << 51,
            channels: 1,
            flags: vec![0; MAXIMUM_BUCKETS],
        };
        assert!(history.is_valid());
        let bytes = history.to_bytes().expect("valid maximal length");
        assert_eq!(OverloadHistory::from_bytes(&bytes), Some(history));
    }

    #[test]
    fn rate_boundaries_and_partial_payloads_are_validated() {
        for rate in [1, 5, 15, 44_100, MAXIMUM_SAMPLE_RATE] {
            let mut builder = OverloadHistoryBuilder::new(2, rate);
            builder.push(&[0, TRUE_PEAK_OVER]);
            let mut history = builder.finish(1);
            assert!(history.is_valid());
            let bytes = history.to_bytes().expect("valid boundary rate");
            assert_eq!(OverloadHistory::from_bytes(&bytes), Some(history.clone()));
            history.flags.push(0);
            assert!(!history.is_valid());
        }
    }
}
