//! Public spectrogram contracts for analytic PCM, strict shared blobs, and the independent `NumPy` fixtures.

use std::collections::BTreeMap;
use std::f64::consts::PI;
use std::sync::Arc;

use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_core::spectrogram::{
    ALGORITHM_VERSION, FFT_SIZE, FREQUENCY_ROWS, HOP_SIZE, MAXIMUM_COLUMNS, MAXIMUM_DECIBELS, MINIMUM_DECIBELS,
    SERIALIZATION_VERSION, SpectrogramBuilder, SpectrogramData, frequency_to_mel, mel_to_frequency,
    spectrogram_decibels,
};
use serde_json::Value;
use test_fixtures::fixture;

const SAMPLE_RATE: u32 = 48_000;
const HEADER_BYTES: usize = 72;
const DECIBEL_TOLERANCE: f64 = 0.29;

/// Generates a bin-centered analytic sinusoid without another analysis implementation.
fn tone(frames: usize, bin: usize, amplitude: f64) -> Vec<f32> {
    (0..frames)
        .map(|frame| (amplitude * (2.0 * PI * bin as f64 * frame as f64 / FFT_SIZE as f64).sin()) as f32)
        .collect()
}

/// Builds consecutive PCM with arbitrary chunk boundaries and harmless empty pushes.
fn build(planes: &[Vec<f32>], chunk_size: usize) -> Option<Arc<SpectrogramData>> {
    let mut builder = SpectrogramBuilder::new(planes.len(), SAMPLE_RATE);
    let frames = planes.first()?.len();
    let empty = vec![&[][..]; planes.len()];
    for first in (0..frames).step_by(chunk_size) {
        builder.push(&empty);
        let end = (first + chunk_size).min(frames);
        let chunk: Vec<_> = planes
            .iter()
            .map(|plane| plane.get(first..end).expect("input chunk"))
            .collect();
        builder.push(&chunk);
    }
    builder.finish()
}

/// Locates a frequency independently through the published HTK logarithmic coordinate.
fn reference_row(frequency: f64, sample_rate: u32) -> usize {
    (FREQUENCY_ROWS as f64 * (frequency / 700.0).ln_1p() / (f64::from(sample_rate) * 0.5 / 700.0).ln_1p())
        .floor()
        .clamp(0.0, (FREQUENCY_ROWS - 1) as f64) as usize
}

/// Reads a byte after the test has established its source geometry.
fn intensity(data: &SpectrogramData, column: usize, row: usize) -> u8 {
    *data
        .intensities()
        .get(column * FREQUENCY_ROWS + row)
        .expect("spectrogram row")
}

/// Quantizes an independent linear-power expectation using the documented physical range.
fn reference_byte(power: f64) -> u8 {
    let decibels = (10.0 * power.max(1.0e-12).log10()).clamp(-120.0, 24.0);
    ((decibels + 120.0) * 255.0 / 144.0 + 0.5).floor() as u8
}

/// Fails with source context when a physical scalar leaves its independently specified bound.
fn close(actual: f64, expected: f64, tolerance: f64) {
    assert!(
        (actual - expected).abs() <= tolerance,
        "actual {actual}, expected {expected}, tolerance {tolerance}"
    );
}

/// Supplies a two-column artifact with recognizable, unchanged payload bytes.
fn codec_data() -> SpectrogramData {
    SpectrogramData::new(
        SAMPLE_RATE,
        2,
        HOP_SIZE as u64 + 1,
        HOP_SIZE as u64,
        (0..2 * FREQUENCY_ROWS).map(|index| (index % 256) as u8).collect(),
    )
    .expect("canonical two-column data")
}

/// Replaces a checked blob field without depending on native struct layout or alignment.
fn replace(bytes: &[u8], offset: usize, field: &[u8]) -> Vec<u8> {
    let mut changed = bytes.to_vec();
    changed
        .get_mut(offset..offset + field.len())
        .expect("header field")
        .copy_from_slice(field);
    changed
}

/// Decodes a committed source through the production decoder while retaining only the artifact.
fn build_fixture(name: &str) -> SpectrogramData {
    let mut decoder = AudioFileDecoder::open(&fixture(&format!("spectrogram_expected/{name}"))).expect("open fixture");
    let mut builder = SpectrogramBuilder::new(decoder.info().channels as usize, decoder.info().sample_rate);
    while let Some(planes) = decoder.next_chunk().expect("decode fixture") {
        builder.push(planes);
    }
    let data = builder.finish().expect("valid fixture spectrogram");
    Arc::try_unwrap(data).expect("unshared artifact")
}

/// Requires a numeric oracle field instead of silently defaulting malformed expectations.
fn number(value: &Value, name: &str) -> f64 {
    value.get(name).and_then(Value::as_f64).expect("numeric oracle field")
}

/// Requires an integer oracle field without accepting strings or fractional values.
fn integer(value: &Value, name: &str) -> u64 {
    value.get(name).and_then(Value::as_u64).expect("integer oracle field")
}

/// Requires an array oracle field before its positions are used.
fn array<'a>(value: &'a Value, name: &str) -> &'a [Value] {
    value.get(name).and_then(Value::as_array).expect("array oracle field")
}

/// Loads the required independently generated dataset through the shared fixture locator.
fn oracle() -> Value {
    let bytes = std::fs::read(fixture("spectrogram_expected/expected.json")).expect("shared NumPy oracle");
    serde_json::from_slice(&bytes).expect("valid oracle JSON")
}

/// HTK anchors and the full intensity range have independently specified values.
#[test]
fn mel_and_decibel_anchors_are_independent() {
    close(frequency_to_mel(0.0), 0.0, 1.0e-12);
    close(frequency_to_mel(1000.0), 999.985_537_139_624_4, 1.0e-9);
    close(mel_to_frequency(999.985_537_139_624_4), 1000.0, 1.0e-9);
    close(spectrogram_decibels(0), -120.0, 1.0e-12);
    close(spectrogram_decibels(255), 24.0, 1.0e-12);
    for pair in (0..=254).map(|value| (spectrogram_decibels(value), spectrogram_decibels(value + 1))) {
        assert!(pair.0 < pair.1);
    }
}

/// Calibrated bin-centered tones establish physical source amplitude at independently selected rows.
#[test]
fn calibrated_tones_cover_low_middle_and_high_mel_rows() {
    for bin in [1, 32, 128, 700, 1800] {
        let data = build(&[tone(5 * HOP_SIZE, bin, 0.5)], 997).expect("tone spectrum");
        let row = reference_row(bin as f64 * f64::from(SAMPLE_RATE) / FFT_SIZE as f64, SAMPLE_RATE);
        close(
            spectrogram_decibels(intensity(&data, 2, row)),
            20.0 * 0.5_f64.log10(),
            DECIBEL_TOLERANCE,
        );
        assert_eq!(data.sample_rate(), SAMPLE_RATE);
        assert_eq!(data.channels(), 1);
        assert_eq!(data.column_count(), 5);
        assert_eq!(data.samples_per_column(), HOP_SIZE as u64);
        assert!(data.matches_stream(SAMPLE_RATE, 5 * HOP_SIZE as u64, 1));
    }
}

/// Independent stereo powers preserve antiphase signals and average rather than sum channel energy.
#[test]
fn antiphase_and_silent_channel_have_mean_power_not_downmix_power() {
    let mono = tone(5 * HOP_SIZE, 128, 0.5);
    let opposite: Vec<_> = mono.iter().map(|sample| -sample).collect();
    let original = build(std::slice::from_ref(&mono), 317).expect("mono");
    let antiphase = build(&[mono.clone(), opposite], 997).expect("antiphase");
    assert_eq!(original.intensities(), antiphase.intensities());
    let stereo = build(&[mono, vec![0.0; 5 * HOP_SIZE]], 17).expect("one active channel");
    close(
        spectrogram_decibels(intensity(&stereo, 2, reference_row(1500.0, SAMPLE_RATE))),
        10.0 * 0.125_f64.log10(),
        DECIBEL_TOLERANCE,
    );
}

/// Real DC and Nyquist endpoints have no doubled partner and very large tones clip to the fixed ceiling.
#[test]
fn real_endpoint_calibration_sub_bin_support_and_ceiling_are_fixed() {
    for nyquist in [false, true] {
        let samples = (0..5 * HOP_SIZE)
            .map(|frame| if nyquist && frame % 2 != 0 { -0.5 } else { 0.5 })
            .collect();
        let data = build(&[samples], 997).expect("real endpoint");
        let row = if nyquist { FREQUENCY_ROWS - 1 } else { 0 };
        close(
            spectrogram_decibels(intensity(&data, 2, row)),
            20.0 * 0.5_f64.log10(),
            DECIBEL_TOLERANCE,
        );
        if !nyquist {
            assert!(intensity(&data, 2, 1) > 0, "sub-bin mel rows share DC support");
        }
    }
    let clipped = build(&[tone(5 * HOP_SIZE, 128, 32.0)], 997).expect("over-range tone");
    assert_eq!(intensity(&clipped, 2, reference_row(1500.0, SAMPLE_RATE)), 255);
}

/// Empty input has no artifact, while every nonempty partial hop has exactly the centered EOF geometry.
#[test]
fn empty_single_frame_and_partial_eof_have_exact_center_counts() {
    assert!(SpectrogramBuilder::new(1, SAMPLE_RATE).finish().is_none());
    let single = build(&[vec![1.0]], 1).expect("one-frame spectrum");
    assert_eq!(single.column_count(), 1);
    assert_eq!(single.length_in_frames(), 1);
    assert!(intensity(&single, 0, reference_row(1500.0, SAMPLE_RATE)) > 0);
    for frames in [
        HOP_SIZE - 1,
        HOP_SIZE,
        HOP_SIZE + 1,
        FFT_SIZE - 1,
        FFT_SIZE,
        FFT_SIZE + 1,
    ] {
        let silence = build(&[vec![0.0; frames], vec![0.0; frames]], 17).expect("partial silence");
        assert_eq!(silence.column_count(), frames.div_ceil(HOP_SIZE));
        assert_eq!(silence.length_in_frames(), frames as u64);
        assert!(silence.intensities().iter().all(|&value| value == 0));
    }
}

/// Centered impulses establish source timing, symmetric Hann normalization, and retained EOF energy.
#[test]
fn centered_and_eof_impulses_are_chunk_invariant() {
    let mut samples = vec![0.0; 6 * HOP_SIZE + 7];
    *samples.get_mut(2 * HOP_SIZE).expect("center impulse") = 1.0;
    let centered = build(std::slice::from_ref(&samples), 997).expect("centered impulse");
    let row = reference_row(1500.0, SAMPLE_RATE);
    assert_eq!(intensity(&centered, 0, row), 0);
    assert!(intensity(&centered, 2, row) > intensity(&centered, 1, row));
    assert!(intensity(&centered, 2, row) > intensity(&centered, 3, row));
    assert_eq!(intensity(&centered, 4, row), 0);
    close(
        spectrogram_decibels(intensity(&centered, 2, row)),
        20.0 * (2.0_f64 / 2047.5).log10(),
        DECIBEL_TOLERANCE,
    );
    *samples.last_mut().expect("EOF impulse") = 0.75;
    let whole = build(std::slice::from_ref(&samples), samples.len()).expect("whole stream");
    for chunk in [1, 17, 317, 1024, 4096] {
        let chunked = build(std::slice::from_ref(&samples), chunk).expect("chunked stream");
        assert_eq!(whole.to_bytes(), chunked.to_bytes(), "chunk size {chunk}");
    }
    assert!(intensity(&whole, whole.column_count() - 1, row) > 0);
}

/// Unsupported or malformed streams never expose partial artifacts or alter caller PCM.
#[test]
fn invalid_geometry_nonfinite_pcm_and_fft_overflow_are_artifact_misses() {
    for (channels, rate) in [
        (0, SAMPLE_RATE),
        (3, SAMPLE_RATE),
        (usize::MAX, SAMPLE_RATE),
        (1, 0),
        (1, u32::MAX),
    ] {
        let mut builder = SpectrogramBuilder::new(channels, rate);
        builder.push(&[&[1.0][..]]);
        assert!(builder.finish().is_none());
    }
    for value in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
        let samples = vec![value];
        let bits = samples.first().expect("source sample").to_bits();
        assert!(build(std::slice::from_ref(&samples), 1).is_none());
        assert_eq!(samples.first().expect("unchanged source").to_bits(), bits);
    }
    assert!(build(&[vec![f32::MAX; FFT_SIZE]], FFT_SIZE).is_none());
    for planes in [vec![vec![0.0]], vec![vec![0.0], vec![0.0, 1.0]], vec![vec![]; 3]] {
        let mut builder = SpectrogramBuilder::new(2, SAMPLE_RATE);
        builder.push(&planes);
        builder.push(&[vec![0.0; FFT_SIZE], vec![0.0; FFT_SIZE]]);
        assert!(builder.finish().is_none());
    }
    let mut missing = SpectrogramBuilder::new(1, SAMPLE_RATE);
    missing.push::<&[f32]>(&[]);
    assert!(missing.finish().is_none());
}

/// The exact storage limit and two subsequent merges retain isolated start and EOF energy.
#[test]
fn streaming_merges_bound_the_grid_and_preserve_isolated_maxima() {
    let mut builder = SpectrogramBuilder::new(1, SAMPLE_RATE);
    let mut block = [0.0; HOP_SIZE];
    *block.first_mut().expect("start impulse") = 1.0;
    builder.push(&[block]);
    block.fill(0.0);
    for _ in 1..2 * MAXIMUM_COLUMNS {
        builder.push(&[block]);
    }
    builder.push(&[&[1.0][..]]);
    let data = builder.finish().expect("twice-merged stream");
    assert_eq!(data.samples_per_column(), 4 * HOP_SIZE as u64);
    assert_eq!(data.column_count(), MAXIMUM_COLUMNS / 2 + 1);
    assert_eq!(data.length_in_frames(), (2 * MAXIMUM_COLUMNS * HOP_SIZE + 1) as u64);
    assert!(data.intensities().len() <= MAXIMUM_COLUMNS * FREQUENCY_ROWS);
    let row = reference_row(1500.0, SAMPLE_RATE);
    assert!(intensity(&data, 0, row) > 0);
    assert!(intensity(&data, data.column_count() - 1, row) > 0);
}

/// The byte-exact fixed envelope round trips without changing the column-major payload.
#[test]
fn shared_spec_header_is_explicit_little_endian_and_exactly_72_bytes() {
    let data = codec_data();
    let bytes = data.to_bytes();
    let mut header = Vec::new();
    header.extend_from_slice(b"SPEC");
    for value in [
        SERIALIZATION_VERSION,
        ALGORITHM_VERSION,
        4096,
        1024,
        512,
        4096,
        48_000,
        2,
        2,
    ] {
        header.extend_from_slice(&value.to_le_bytes());
    }
    for value in [1025_u64, 1024, (-120.0_f64).to_bits(), 24.0_f64.to_bits()] {
        header.extend_from_slice(&value.to_le_bytes());
    }
    assert_eq!(header.len(), HEADER_BYTES);
    assert_eq!(bytes.get(..HEADER_BYTES), Some(header.as_slice()));
    assert_eq!(bytes.get(HEADER_BYTES..), Some(data.intensities()));
    assert_eq!(SpectrogramData::from_bytes(&bytes), Some(data.clone()));
    assert!(data.matches_stream(SAMPLE_RATE, 1025, 2));
    assert!(!data.matches_stream(44_100, 1025, 2));
    assert!(!data.matches_stream(SAMPLE_RATE, 1026, 2));
    assert!(!data.matches_stream(SAMPLE_RATE, 1025, 1));
}

/// Unknown profiles, every truncation, and trailing bytes are rejected before adoption.
#[test]
fn codec_rejects_every_profile_mutation_truncation_and_trailing_byte() {
    let bytes = codec_data().to_bytes();
    for length in 0..bytes.len() {
        assert!(SpectrogramData::from_bytes(bytes.get(..length).expect("truncated prefix")).is_none());
    }
    for offset in [0, 4, 8, 12, 16, 20, 24, 32, 36, 40, 48, 56, 64] {
        let mut changed = bytes.clone();
        *changed.get_mut(offset).expect("profile field") ^= 1;
        assert!(SpectrogramData::from_bytes(&changed).is_none(), "offset {offset}");
    }
    let mut trailing = bytes;
    trailing.push(0);
    assert!(SpectrogramData::from_bytes(&trailing).is_none());
    assert!(SpectrogramData::from_bytes(&vec![0; HEADER_BYTES + MAXIMUM_COLUMNS * FREQUENCY_ROWS + 1]).is_none());
}

/// Signed source limits and exact canonical geometry exclude hostile allocation sizes and coarse strides.
#[test]
fn codec_and_constructor_reject_invalid_and_noncanonical_geometry() {
    let bytes = codec_data().to_bytes();
    for (offset, values) in [
        (28, vec![0, i32::MAX as u32 + 1, u32::MAX]),
        (32, vec![0, 3, u32::MAX]),
        (36, vec![0, 1, MAXIMUM_COLUMNS as u32 + 1, u32::MAX]),
    ] {
        for value in values {
            assert!(SpectrogramData::from_bytes(&replace(&bytes, offset, &value.to_le_bytes())).is_none());
        }
    }
    for (offset, values) in [
        (40, vec![0, 1024, i64::MAX as u64 + 1, u64::MAX]),
        (48, vec![0, 1023, 1025, 2048, 3072, u64::MAX]),
    ] {
        for value in values {
            assert!(SpectrogramData::from_bytes(&replace(&bytes, offset, &value.to_le_bytes())).is_none());
        }
    }
    for (rate, channels, length, stride, payload) in [
        (0, 2, 1025, 1024, 1024),
        (u32::MAX, 2, 1025, 1024, 1024),
        (48_000, 0, 1025, 1024, 1024),
        (48_000, 3, 1025, 1024, 1024),
        (48_000, 2, 0, 1024, 1024),
        (48_000, 2, 1025, 2048, 512),
        (48_000, 2, 1025, 1024, 1023),
        (48_000, 2, 1025, 1024, MAXIMUM_COLUMNS * FREQUENCY_ROWS + 1),
    ] {
        assert!(SpectrogramData::new(rate, channels, length, stride, vec![0; payload]).is_none());
    }
}

/// Geometry at exact merge boundaries and the signed maximum length uses overflow-safe ceiling division.
#[test]
fn canonical_geometry_covers_merge_and_signed_integer_boundaries() {
    for (length, stride, columns) in [
        (1, HOP_SIZE as u64, 1),
        ((MAXIMUM_COLUMNS * HOP_SIZE) as u64, HOP_SIZE as u64, MAXIMUM_COLUMNS),
        (
            (MAXIMUM_COLUMNS * HOP_SIZE + 1) as u64,
            2 * HOP_SIZE as u64,
            MAXIMUM_COLUMNS / 2 + 1,
        ),
        (
            (2 * MAXIMUM_COLUMNS * HOP_SIZE + 1) as u64,
            4 * HOP_SIZE as u64,
            MAXIMUM_COLUMNS / 2 + 1,
        ),
        (i64::MAX as u64, 1_u64 << 51, MAXIMUM_COLUMNS),
    ] {
        let data = SpectrogramData::new(i32::MAX as u32, 1, length, stride, vec![0; columns * FREQUENCY_ROWS])
            .expect("canonical boundary geometry");
        assert_eq!(data.column_count(), columns);
        assert_eq!(SpectrogramData::from_bytes(&data.to_bytes()), Some(data));
    }
}

/// Independent `NumPy` landmarks and boundaries match at every supported fixture source rate.
#[test]
fn shared_numpy_mel_landmarks_and_profile_are_required() {
    let oracle = oracle();
    assert_eq!(integer(&oracle, "schema_version"), 1);
    let contract = oracle.get("contract").expect("contract");
    assert_eq!(integer(contract, "fft_size"), FFT_SIZE as u64);
    assert_eq!(integer(contract, "hop_size"), HOP_SIZE as u64);
    assert_eq!(integer(contract, "mel_rows"), FREQUENCY_ROWS as u64);
    assert_eq!(contract.get("window").and_then(Value::as_str), Some("symmetric_hann"));
    close(MINIMUM_DECIBELS, -120.0, 1.0e-12);
    close(MAXIMUM_DECIBELS, 24.0, 1.0e-12);
    let rates = array(&oracle, "mel");
    assert_eq!(rates.len(), 3);
    for expected in rates {
        let landmarks = array(expected, "landmarks");
        let boundaries = array(expected, "boundaries");
        assert_eq!(landmarks.len(), 8);
        assert_eq!(boundaries.len(), 7);
        for landmark in landmarks {
            close(
                frequency_to_mel(number(landmark, "hz")),
                number(landmark, "mel"),
                1.0e-8,
            );
            close(
                mel_to_frequency(number(landmark, "mel")),
                number(landmark, "roundtrip_hz"),
                1.0e-8,
            );
        }
        for boundary in boundaries {
            close(
                frequency_to_mel(number(boundary, "hz")),
                number(boundary, "mel"),
                1.0e-8,
            );
            close(
                mel_to_frequency(number(boundary, "mel")),
                number(boundary, "hz"),
                1.0e-8,
            );
        }
    }
}

/// Every committed fixture has actual source geometry, and tonal row maxima match independent `NumPy` powers.
#[test]
fn shared_numpy_fixtures_pin_source_geometry_and_tonal_power() {
    let oracle = oracle();
    let files = oracle.get("files").and_then(Value::as_object).expect("oracle files");
    assert_eq!(files.len(), 20);
    for (name, expected) in files {
        let data = build_fixture(name);
        let rate = u32::try_from(integer(expected, "sample_rate")).expect("fixture rate");
        let length = integer(expected, "sample_count");
        assert!(
            data.matches_stream(rate, length, integer(expected, "channels") as u32),
            "{name}"
        );
        assert_eq!(data.samples_per_column(), HOP_SIZE as u64);
        assert_eq!(data.column_count() as u64, integer(expected, "frame_count"));
        let source = expected.get("source").expect("source description");
        let kind = source.get("kind").and_then(Value::as_str).expect("source kind");
        match kind {
            "silence" => assert!(data.intensities().iter().all(|&value| value == 0)),
            "periodic_noise" => {}
            "tone" | "antiphase" | "dc" | "nyquist" => check_tonal_powers(&data, expected, source, kind, name),
            _ => unreachable!("unsupported oracle source kind"),
        }
    }
}

/// Compares only sparse probes known to establish a tonal row maximum in fully supported interior windows.
fn check_tonal_powers(data: &SpectrogramData, expected: &Value, source: &Value, kind: &str, name: &str) {
    let peaks: Vec<_> = match kind {
        "dc" => vec![0],
        "nyquist" => vec![FFT_SIZE / 2],
        _ => array(source, "tone_bins")
            .iter()
            .map(|bin| bin.as_u64().expect("peak bin") as usize)
            .collect(),
    };
    let probes = array(expected, "probe_bins");
    let mut checked = 0;
    for frame in array(expected, "frames") {
        let center = integer(frame, "center_sample");
        if center < FFT_SIZE as u64 / 2 || center + FFT_SIZE as u64 / 2 > data.length_in_frames() {
            continue;
        }
        let powers = array(frame, "powers");
        assert_eq!(powers.len(), probes.len());
        for &bin in &peaks {
            let probe = probes
                .iter()
                .position(|value| value.as_u64() == Some(bin as u64))
                .expect("peak probe");
            let power = powers.get(probe).and_then(Value::as_f64).expect("linear power");
            assert!(power > 0.0, "{name}: source peak must retain positive power");
            if bin > 0 && bin < FFT_SIZE / 2 {
                for neighbor in [bin - 1, bin + 1] {
                    let index = probes
                        .iter()
                        .position(|value| value.as_u64() == Some(neighbor as u64))
                        .expect("neighbor probe");
                    assert!(power > powers.get(index).and_then(Value::as_f64).expect("neighbor power"));
                }
            }
            let row = reference_row(
                bin as f64 * f64::from(data.sample_rate()) / FFT_SIZE as f64,
                data.sample_rate(),
            );
            let actual = intensity(data, center as usize / HOP_SIZE, row);
            // One byte is 144/255 dB, the shared float-FFT versus NumPy-double tonal expectation bound.
            assert!(
                actual.abs_diff(reference_byte(power)) <= 1,
                "{name}: peak bin {bin}, center {center}"
            );
            checked += 1;
        }
    }
    assert_eq!(
        checked,
        peaks.len(),
        "{name}: every tonal peak needs a full interior window"
    );
}

/// Filtered and full-band fixtures establish cutoff contrast away from transition rows and padded source edges.
#[test]
fn shared_noise_cutoffs_have_40_db_interior_contrast_at_every_rate() {
    for rate in [44_100, 48_000, 96_000] {
        let mut spectra = BTreeMap::new();
        for kind in ["noise_16000hz", "noise_18000hz", "noise_full"] {
            spectra.insert(kind, build_fixture(&format!("{kind}_{rate}.wav")));
        }
        let full = spectra.get("noise_full").expect("full bandwidth");
        for (cutoff, kind) in [(16_000, "noise_16000hz"), (18_000, "noise_18000hz")] {
            let filtered = spectra.get(kind).expect("filtered bandwidth");
            let below_row = reference_row(f64::from(cutoff) - 1000.0, rate);
            let above_row = reference_row(f64::from(cutoff) + 1000.0, rate);
            let middle = filtered.length_in_frames() as usize / 2 / HOP_SIZE;
            for column in middle - 1..=middle + 1 {
                let center = column * HOP_SIZE;
                assert!(center >= FFT_SIZE / 2);
                assert!(
                    center as u64 + FFT_SIZE as u64 / 2 <= filtered.length_in_frames().min(full.length_in_frames())
                );
                let below = spectrogram_decibels(intensity(filtered, column, below_row));
                let above = spectrogram_decibels(intensity(filtered, column, above_row));
                let full_above = spectrogram_decibels(intensity(full, column, above_row));
                assert!(below - above > 40.0, "{rate}: cutoff {cutoff}, center {center}");
                assert!(
                    full_above - above > 40.0,
                    "{rate}: full/filtered cutoff {cutoff}, center {center}"
                );
            }
        }
    }
}
