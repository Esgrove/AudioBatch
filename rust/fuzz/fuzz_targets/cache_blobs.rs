//! libFuzzer target for the readers of the binary blobs in the analysis cache.
//!
//! The shared database can hold bytes that no writer produced, such as a corrupted file or one from another version,
//! so every reader must reject them without panicking or allocating unbounded memory.
//! Each input goes to the waveform, overload, spectrogram, and Serato Overview readers,
//! which tell their formats apart by their magic.
//! A blob that a reader accepts must be canonical, so writing it back must give the same bytes.

#![no_main]

use audiobatch_core::WaveformOverview;
use audiobatch_core::overload::OverloadHistory;
use audiobatch_core::spectrogram::SpectrogramData;
use libfuzzer_sys::fuzz_target;
use serato_tags::Overview;

fuzz_target!(|input: &[u8]| {
    if let Some(waveform) = WaveformOverview::from_bytes(input) {
        assert_eq!(waveform.to_bytes(), input, "the waveform blob is not canonical");
    }
    if let Some(overloads) = OverloadHistory::from_bytes(input) {
        assert_eq!(
            overloads.to_bytes().as_deref(),
            Some(input),
            "the overload blob is not canonical"
        );
    }
    if let Some(spectrogram) = SpectrogramData::from_bytes(input) {
        assert_eq!(spectrogram.to_bytes(), input, "the spectrogram blob is not canonical");
    }
    if let Some(overview) = Overview::deserialize(input) {
        assert_eq!(overview.serialize(), input, "the Serato Overview blob is not canonical");
    }
});
