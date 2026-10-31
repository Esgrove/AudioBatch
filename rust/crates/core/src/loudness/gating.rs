//! Block-energy windows and the absolute and relative gates of integrated loudness.
//!
//! Complete short-term queries sum thirty weighted block energies, rather than scanning filtered audio.
//! A shared ring of weighted per-frame energies preserves the final partial window's outgoing suffix.
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
        let retained: f64 = if frames == self.block_size {
            0.0
        } else {
            let other_blocks: f64 = self
                .energies
                .iter()
                .enumerate()
                .filter(|(index, _)| *index != self.next)
                .map(|(_, &energy)| energy)
                .sum();
            let suffix: f64 = self
                .boundary_energy
                .get(start + frames..start + self.block_size)
                .expect("aligned block fits boundary storage")
                .iter()
                .sum();
            other_blocks + suffix
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
            energy_to_loudness((retained + energy) / (self.block_size * 30) as f64)
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
    use crate::golden::{GoldenText, diagnostic_signals, write_diagnostic_report};
    use crate::signals::generate;

    #[test]
    #[ignore = "writes cross-platform gate diagnostics; run explicitly with --ignored"]
    fn numerical_diagnostics_gating() {
        let mut text = GoldenText::default();
        for (kind, rate, channels, seconds) in diagnostic_signals() {
            let key = format!("{}_{rate}_{channels}ch", kind.name());
            let planes = generate(kind, rate, channels, (f64::from(rate) * seconds) as usize);
            text.value(&format!("{key}.pcm_sha256"), test_fixtures::pcm_sha256(rate, &planes));
            let frames = planes.first().expect("generated channels").len();
            let mut meter = LoudnessMeter::new(channels, rate, Some(frames as u64));
            for (block, start) in (0..frames).step_by(meter.block_size).enumerate() {
                let end = (start + meter.block_size).min(frames);
                let slices: Vec<_> = planes
                    .iter()
                    .map(|plane| plane.get(start..end).expect("aligned planes"))
                    .collect();
                let retained = if end - start == meter.block_size {
                    0.0
                } else {
                    let other_blocks: f64 = meter
                        .energies
                        .iter()
                        .enumerate()
                        .filter(|(index, _)| *index != meter.next)
                        .map(|(_, &energy)| energy)
                        .sum();
                    let boundary = meter.next * meter.block_size;
                    let suffix: f64 = meter
                        .boundary_energy
                        .get(boundary + end - start..boundary + meter.block_size)
                        .expect("partial boundary")
                        .iter()
                        .sum();
                    other_blocks + suffix
                };
                let mut probe = meter.filter.clone();
                let mut frame_energy = vec![0.0; end - start];
                let energy = probe.energy_with_frames(&slices, &mut frame_energy);
                let admitted = meter.integrated.len();
                let boundary = meter.next * meter.block_size;
                let loudness = meter.push(&slices);
                assert_eq!(
                    frame_energy.iter().map(|value| value.to_bits()).collect::<Vec<_>>(),
                    meter
                        .boundary_energy
                        .get(boundary..boundary + end - start)
                        .expect("recorded block energy")
                        .iter()
                        .map(|value| value.to_bits())
                        .collect::<Vec<_>>(),
                    "diagnostics must match the production filtering path"
                );
                let total_energy = if end - start == meter.block_size {
                    meter.energies.iter().sum::<f64>()
                } else {
                    retained + energy
                };
                let mean_energy = total_energy / (meter.block_size * 30) as f64;
                assert_eq!(energy_to_loudness(mean_energy).to_bits(), loudness.to_bits());
                text.float(&format!("{key}.block[{block}].energy"), energy);
                text.float(&format!("{key}.block[{block}].short_term_mean_energy"), mean_energy);
                text.float(&format!("{key}.block[{block}].short_term_lufs"), loudness);
                text.float32(&format!("{key}.block[{block}].history_lufs"), loudness as f32);
                text.value(
                    &format!("{key}.block[{block}].absolute_admitted"),
                    meter.integrated.len() > admitted,
                );
                if meter.complete_blocks >= 4 && end - start == meter.block_size {
                    let gating_energy: f64 = (1..=4)
                        .map(|offset| meter.energies[(meter.next + 30 - offset) % 30])
                        .sum::<f64>()
                        / (meter.block_size * 4) as f64;
                    text.float(&format!("{key}.block[{block}].absolute_gate_energy"), gating_energy);
                }
            }
            let threshold = meter.admitted_sum / meter.integrated.len().max(1) as f64 * 0.1;
            let admitted: Vec<_> = meter.integrated.iter().map(|&energy| energy >= threshold).collect();
            let (sum, count) = meter
                .integrated
                .iter()
                .filter(|&&energy| energy >= threshold)
                .fold((0.0, 0_u64), |(sum, count), &energy| (sum + energy, count + 1));
            let mean_energy = if count == 0 { 0.0 } else { sum / count as f64 };
            assert_eq!(energy_to_loudness(mean_energy).to_bits(), meter.integrated().to_bits());
            text.float(&format!("{key}.relative_threshold"), threshold);
            text.value(&format!("{key}.relative_admitted"), admitted);
            text.value(&format!("{key}.absolute_count"), meter.integrated.len());
            text.value(&format!("{key}.relative_count"), count);
            text.float(&format!("{key}.integrated_mean_energy"), mean_energy);
            text.float(&format!("{key}.integrated_lufs"), meter.integrated());
        }
        write_diagnostic_report("gating", &text);
    }

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
    fn partial_window_retains_a_quiet_suffix_after_a_loud_prefix_expires() {
        let mut meter = LoudnessMeter::new(1, 8_000, None);
        meter.energies[0] = 1e20;
        meter.boundary_energy[0] = 1e20;
        meter.boundary_energy[1] = 1.0;
        let measured = meter.push(&[&[0.0]]);
        let expected = energy_to_loudness(1.0 / 24_000.0);
        assert_eq!(measured.to_bits(), expected.to_bits());
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
