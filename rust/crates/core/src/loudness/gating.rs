//! Block-energy windows and the absolute and relative gates of integrated loudness.
//!
//! Complete updates retain only thirty weighted energies, rather than three seconds of filtered audio.
//! The short-term sum is recomputed from that fixed ring, so long streams cannot accumulate subtraction drift.
//! Integrated history retains only blocks admitted by the absolute gate and applies the relative gate at finish.

use super::k_weighting::KWeighting;

/// Energy at the absolute gate of -70 LUFS, including the BS.1770 calibration offset.
const ABSOLUTE_GATE: f64 = 0.000_000_117_246_530_458_229_81;

/// Streaming loudness for aligned 100 ms planes, with a final partial update.
pub struct LoudnessMeter {
    filter: KWeighting,
    block_size: usize,
    energies: [f64; 30],
    /// Weighted squared energy, one value per frame, used only for the final fractional window boundary.
    boundary_energy: Vec<f64>,
    next: usize,
    complete_blocks: u64,
    integrated: Vec<f64>,
    admitted_sum: f64,
}

impl LoudnessMeter {
    /// Creates empty windows and reserves the integrated history from the expected stream length.
    pub fn new(channels: usize, sample_rate: u32, expected_frames: Option<u64>) -> Self {
        let block_size = ((sample_rate as usize + 5) / 10).max(1);
        let capacity = expected_frames.map_or(0, |frames| frames / block_size as u64);
        Self {
            filter: KWeighting::new(channels, sample_rate),
            block_size,
            energies: [0.0; 30],
            boundary_energy: vec![0.0; block_size * 30],
            next: 0,
            complete_blocks: 0,
            integrated: Vec::with_capacity(usize::try_from(capacity).unwrap_or(0)),
            admitted_sum: 0.0,
        }
    }

    /// Measures a complete aligned block, returning the zero-padded three-second loudness.
    pub fn push(&mut self, planes: &[&[f32]]) -> f64 {
        let frames = planes.first().map_or(0, |plane| plane.len());
        let start = self.next * self.block_size;
        let expired: f64 = if frames == self.block_size {
            0.0
        } else {
            self.boundary_energy
                .get(start..start + frames)
                .expect("aligned block fits boundary storage")
                .iter()
                .sum()
        };
        let energy = self.filter.energy_with_frames(
            planes,
            self.boundary_energy
                .get_mut(start..start + frames)
                .expect("aligned block fits boundary storage"),
        );
        if frames == self.block_size {
            *self.energies.get_mut(self.next).expect("ring cursor is bounded") = energy;
            self.next = (self.next + 1) % self.energies.len();
            self.complete_blocks += 1;
            if self.complete_blocks >= 4 {
                let sum: f64 = (1..=4)
                    .map(|offset| {
                        *self
                            .energies
                            .get((self.next + 30 - offset) % 30)
                            .expect("ring offset is bounded")
                    })
                    .sum();
                let gating_energy = sum / (self.block_size * 4) as f64;
                if gating_energy >= ABSOLUTE_GATE || gating_energy.is_nan() {
                    self.integrated.push(gating_energy);
                    self.admitted_sum += gating_energy;
                }
            }
            energy_to_loudness(self.energies.iter().sum::<f64>() / (self.block_size * 30) as f64)
        } else {
            // A partial update does not create an incomplete integrated gating block.
            energy_to_loudness((self.energies.iter().sum::<f64>() - expired + energy) / (self.block_size * 30) as f64)
        }
    }

    /// Returns integrated loudness after both gates, or nonfinite loudness for an empty or invalid stream.
    pub fn integrated(&self) -> f64 {
        if self.integrated.is_empty() {
            return f64::NEG_INFINITY;
        }
        if self.admitted_sum.is_nan() {
            return f64::NAN;
        }
        let threshold = self.admitted_sum / self.integrated.len() as f64 * 0.1;
        let (sum, count) = self
            .integrated
            .iter()
            .filter(|&&energy| energy >= threshold)
            .fold((0.0, 0_u64), |(sum, count), &energy| (sum + energy, count + 1));
        if count == 0 {
            f64::NEG_INFINITY
        } else {
            energy_to_loudness(sum / count as f64)
        }
    }
}

/// Converts weighted mean-square energy to calibrated LUFS without normalizing silence or invalid values.
#[expect(clippy::suboptimal_flops, reason = "keep the reference calibration rounding")]
fn energy_to_loudness(energy: f64) -> f64 {
    if energy <= 0.0 {
        f64::NEG_INFINITY
    } else {
        10.0 * energy.log10() - 0.691
    }
}

#[cfg(test)]
mod test_gates {
    use super::*;

    #[test]
    fn empty_and_silent_windows_have_no_loudness() {
        let mut meter = LoudnessMeter::new(1, 48_000, None);
        assert_eq!(meter.integrated(), f64::NEG_INFINITY);
        let silence = vec![0.0; 4_800];
        for _ in 0..31 {
            assert_eq!(meter.push(&[&silence]), f64::NEG_INFINITY);
        }
        assert_eq!(meter.integrated(), f64::NEG_INFINITY);
    }

    #[test]
    fn absolute_gate_rejects_quiet_blocks_and_relative_gate_rejects_quiet_sections() {
        let mut meter = LoudnessMeter::new(2, 8_000, None);
        meter.integrated = vec![1.0, 0.001, 1.0];
        meter.admitted_sum = 2.001;
        assert!((meter.integrated() + 0.691).abs() < 1e-12);
        let quiet = vec![0.000_001; 800];
        let mut meter = LoudnessMeter::new(1, 8_000, None);
        for _ in 0..5 {
            meter.push(&[&quiet]);
        }
        assert_eq!(meter.integrated, [] as [f64; 0]);
    }

    #[test]
    fn ring_wraps_do_not_accumulate_running_sum_drift() {
        let mut meter = LoudnessMeter::new(1, 8_000, Some(80_000));
        let tone: Vec<_> = (0..800).map(|index| if index % 2 == 0 { 0.5 } else { -0.5 }).collect();
        for _ in 0..100 {
            let measured = meter.push(&[&tone]);
            let expected = energy_to_loudness(meter.energies.iter().sum::<f64>() / 24_000.0);
            assert_eq!(measured.to_bits(), expected.to_bits());
        }
        assert_eq!(meter.integrated.len(), 97);
    }
}
