//! Synthetic data for the GUI stress test, started with `--demo-rows <count>`.
//!
//! Fills the table with generated rows and updates a batch of random rows on every tick,
//! like a large analysis run streaming results,
//! and gives the waveform a generated overview with a looping playhead.
//! It validates that the table and the waveform stay smooth without needing real audio files.

use std::path::PathBuf;
use std::sync::Arc;

use audiobatch_core::waveform::WaveformBuilder;
use audiobatch_core::{AnalysisRecord, AnalysisStatus, ShortTermLoudnessHistory, WaveformOverview};

use crate::results::{ResultRow, RowState};

/// Number of rows updated on every tick.
pub const ROWS_PER_TICK: usize = 64;

/// Sample rate of the generated overview.
const DEMO_SAMPLE_RATE: u32 = 44_100;

/// Length of the generated overview in seconds.
const DEMO_SECONDS: u32 = 180;

/// Small deterministic xorshift generator, enough for picking rows and levels.
pub struct DemoRandom {
    state: u64,
}

impl DemoRandom {
    /// Creates a generator from a non-zero seed.
    #[must_use]
    pub const fn new(seed: u64) -> Self {
        Self { state: seed | 1 }
    }

    /// Returns the next value.
    pub const fn next_u64(&mut self) -> u64 {
        self.state ^= self.state << 13;
        self.state ^= self.state >> 7;
        self.state ^= self.state << 17;
        self.state
    }

    /// Returns a value from 0 to 1.
    pub fn next_unit(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 / (1_u64 << 53) as f64
    }

    /// Returns an index below `count`.
    pub const fn next_index(&mut self, count: usize) -> usize {
        if count == 0 {
            0
        } else {
            (self.next_u64() % count as u64) as usize
        }
    }
}

/// Generates the rows of the stress test.
#[must_use]
pub fn demo_rows(count: usize, random: &mut DemoRandom) -> Vec<ResultRow> {
    (0..count)
        .map(|index| {
            let path = PathBuf::from(format!("/demo/Artist {} - Track {index:05}.wav", index % 97));
            let mut row = ResultRow::pending(path);
            if index % 3 != 0 {
                row.state = RowState::Done;
                row.record = Some(demo_record(&row, random));
            }
            row
        })
        .collect()
}

/// Advances one random row: pending rows start analyzing, analyzing rows progress and finish.
pub fn advance_row(row: &mut ResultRow, random: &mut DemoRandom) {
    row.state = match row.state {
        RowState::Pending => RowState::Analyzing(0.0),
        RowState::Analyzing(fraction) if fraction < 0.9 => RowState::Analyzing(fraction + 0.1),
        RowState::Analyzing(_) | RowState::Normalizing(_) | RowState::Processing(_) => {
            row.record = Some(demo_record(row, random));
            RowState::Done
        }
        RowState::Done => {
            row.record = None;
            RowState::Pending
        }
    };
}

/// Generates a three minute overview and loudness history with a varying envelope.
#[must_use]
pub fn demo_waveform() -> (Arc<WaveformOverview>, Arc<ShortTermLoudnessHistory>) {
    let total_frames = u64::from(DEMO_SAMPLE_RATE * DEMO_SECONDS);
    let block = DEMO_SAMPLE_RATE as usize / 10;
    let mut builder = WaveformBuilder::new(2, Some(total_frames));
    let mut history = Vec::new();
    let mut random = DemoRandom::new(0x5eed);
    let mut left = vec![0.0_f32; block];
    let mut right = vec![0.0_f32; block];
    for block_index in 0..(DEMO_SECONDS * 10) {
        let time = f64::from(block_index) / 10.0;
        let slow = 0.3_f64.mul_add((time / 20.0).sin().abs(), 0.35);
        let envelope = 0.2_f64.mul_add((time / 3.0).sin().abs(), slow);
        for (left_sample, right_sample) in left.iter_mut().zip(&mut right) {
            let noise = random.next_unit().mul_add(2.0, -1.0);
            *left_sample = (envelope * noise) as f32;
            *right_sample = (envelope * 0.9 * noise) as f32;
        }
        builder.push(&[&left, &right]);
        history.push(20.0_f64.mul_add(envelope.log10(), -3.0) as f32);
    }
    let overview = builder.finish(DEMO_SAMPLE_RATE, total_frames);
    let history = Arc::new(ShortTermLoudnessHistory {
        values: history,
        sample_rate: f64::from(DEMO_SAMPLE_RATE),
        length_in_samples: total_frames,
        samples_per_point: DEMO_SAMPLE_RATE / 10,
    });
    (overview, history)
}

/// Generates a plausible record for a row.
fn demo_record(row: &ResultRow, random: &mut DemoRandom) -> AnalysisRecord {
    let peak = 0.7_f64.mul_add(random.next_unit(), 0.3) as f32;
    let loudness = 18.0_f64.mul_add(-random.next_unit(), -6.0);
    AnalysisRecord {
        path: row.path.clone(),
        file_name: row.file_name.to_string(),
        format_name: String::from("WAV file"),
        file_size: 40_000_000,
        duration_seconds: 240.0_f64.mul_add(random.next_unit(), 120.0),
        overall_peak: peak,
        peak_left: peak,
        peak_right: peak * 0.95,
        overall_true_peak: f64::from(peak) * 1.02,
        integrated_lufs: loudness,
        max_short_term_lufs: loudness + 3.0,
        sample_rate: DEMO_SAMPLE_RATE,
        channels: 2,
        bits_per_sample: 24,
        status: AnalysisStatus::Analyzed,
        ..Default::default()
    }
}
