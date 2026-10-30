//! Identity-checked source overload attachment and pixel-column projection for the waveform.
//!
//! Sample regions retain their original buckets, while true peak regions conservatively cover interpolation history.
//! Projection combines flags before choosing a warning shape, so isolated subpixel overs remain visible.

use std::sync::Arc;

use audiobatch_core::AnalysisRecord;
use audiobatch_core::overload::{OverloadHistory, SAMPLE_OVER, TRUE_PEAK_OVER};
use gpui_kit::{Bounds, Hsla, PathBuilder, Window, fill, point, px, size};

use super::{PaintData, Viewport};
use crate::theme::color;

/// Amber #CEAF6C shared with the C++ warning renderer and the application warning palette.
const WARNING_AMBER: u32 = 0x00CE_AF6C;
/// Height of solid or hollow caps at each channel edge.
const CAP_HEIGHT: f32 = 4.0;
/// Minimum warning width, retaining a visible hollow interior for isolated true peak overs.
const MINIMUM_MARK_WIDTH: f32 = 3.0;
/// Width of the hollow cap outline.
const OUTLINE_WIDTH: f32 = 1.0;
/// Restrained sample-over tint, leaving the underlying Serato colors legible.
const TINT_ALPHA: f32 = 0.18;

/// Conservative source-frame coverage before a true peak bucket, covering the interpolator's retained history.
/// This is not a precise peak timestamp and does not extend the analyzer's unflushed EOF tail.
const TRUE_PEAK_COVERAGE_FRAMES: u64 = 32;

/// Warning shape after all flags in a screen column have been combined.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WarningKind {
    /// Solid caps and a restrained lane tint, taking precedence over true peak flags.
    Sample,
    /// Hollow edge caps without a reconstructed waveform or lane tint.
    TruePeak,
}

/// Validated history belonging to the waveform and the current on-disk source identity.
#[derive(Default)]
pub(super) struct SourceOverloads {
    history: Option<Arc<OverloadHistory>>,
}

/// Neighbouring pixel columns that share one warning shape.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct WarningRun {
    start: usize,
    end: usize,
    kind: WarningKind,
}

impl SourceOverloads {
    /// Attaches only a ready record whose source identity and waveform timing still match its valid history.
    /// Rejected or missing data replaces any previously attached history, including for the same path.
    pub(super) fn set_record(&mut self, record: &AnalysisRecord) {
        self.history = matching_history(record);
    }

    /// Drops warnings whenever the waveform is replaced or cleared, including by a placeholder or synthetic overview.
    pub(super) fn clear(&mut self) {
        self.history = None;
    }

    /// Returns the attached source history, including a valid all-clear history.
    pub(super) const fn history(&self) -> Option<&Arc<OverloadHistory>> {
        self.history.as_ref()
    }

    /// Returns whether the attached source has a warning to explain in the contextual tooltip.
    pub(super) fn has_overloads(&self) -> bool {
        self.history
            .as_ref()
            .is_some_and(|history| history.flags.iter().any(|&flags| flags != 0))
    }
}

/// Paints source-only caps and sample tint before loudness, cue markers, and the playhead.
/// The lane split is the same as the envelopes, independent of display gain and plugin preview.
pub(super) fn paint(data: &PaintData, viewport: &Viewport, window: &mut Window) {
    let Some(history) = data.overloads.as_deref() else {
        return;
    };
    let plot = viewport.plot;
    let columns = f32::from(plot.size.width) as usize;
    let channels = data.overview.channels().clamp(1, 2) as usize;
    let lane_height = f32::from(plot.size.height) / channels as f32;
    let amber = color(WARNING_AMBER);
    let tint = Hsla { a: TINT_ALPHA, ..amber };
    for channel in 0..channels {
        let projected = project_columns(history, channel, viewport.start, viewport.length, columns);
        let runs = warning_runs(&projected);
        let lane_top = lane_height.mul_add(channel as f32, f32::from(plot.origin.y));
        let cap_height = CAP_HEIGHT.min(lane_height / 4.0);
        // Sample marks paint last, so their stronger treatment wins even where minimum-width caps overlap.
        for kind in [WarningKind::TruePeak, WarningKind::Sample] {
            for run in runs.iter().filter(|run| run.kind == kind) {
                let width = ((run.end - run.start) as f32).max(MINIMUM_MARK_WIDTH);
                let center = f32::midpoint(run.start as f32, run.end as f32);
                let left = f32::from(plot.origin.x) + center - width / 2.0;
                if kind == WarningKind::Sample {
                    window.paint_quad(fill(
                        Bounds {
                            origin: point(px(left), px(lane_top)),
                            size: size(px(width), px(lane_height)),
                        },
                        tint,
                    ));
                }
                for top in [lane_top, lane_top + lane_height - cap_height] {
                    let bounds = Bounds {
                        origin: point(px(left), px(top)),
                        size: size(px(width), px(cap_height)),
                    };
                    if kind == WarningKind::Sample {
                        window.paint_quad(fill(bounds, amber));
                    } else {
                        paint_outline(left, top, width, cap_height, amber, window);
                    }
                }
            }
        }
    }
}

/// Projects one actual source channel onto the visible range, combining every overlapping bucket with bitwise OR.
/// Mono has one lane, stereo keeps channel order, and callers display only the waveform's existing lanes.
/// True peak coverage is separate from sample coverage, including for a bucket containing both kinds of over.
fn project_columns(
    history: &OverloadHistory,
    channel: usize,
    visible_start: f64,
    visible_length: f64,
    columns: usize,
) -> Vec<u8> {
    let mut projected = vec![0; columns];
    if columns == 0
        || channel >= history.channels as usize
        || !visible_start.is_finite()
        || !visible_length.is_finite()
        || visible_length <= 0.0
        || !history.is_valid()
    {
        return projected;
    }
    let rate = f64::from(history.sample_rate);
    let start_frame = visible_start * rate;
    let end_frame = visible_length.mul_add(rate, start_frame);
    let frames_per_column = visible_length * rate / columns as f64;
    if !start_frame.is_finite() || !end_frame.is_finite() || !frames_per_column.is_finite() || frames_per_column <= 0.0
    {
        return projected;
    }
    for (bucket, flags) in history.flags.chunks_exact(history.channels as usize).enumerate() {
        let flags = flags.get(channel).copied().unwrap_or(0);
        if flags == 0 {
            continue;
        }
        let start = (bucket as u64).saturating_mul(history.samples_per_bucket);
        let end = start
            .saturating_add(history.samples_per_bucket)
            .min(history.length_in_samples);
        let mut fill = |mark_start: u64, bits: u8| {
            if end as f64 <= start_frame || mark_start as f64 >= end_frame {
                return;
            }
            let first =
                (((mark_start as f64 - start_frame) / frames_per_column).floor().max(0.0) as usize).min(columns - 1);
            let last = (((end as f64 - start_frame) / frames_per_column).ceil().max(0.0) as usize)
                .max(first + 1)
                .min(columns);
            for value in projected.iter_mut().take(last).skip(first) {
                *value |= bits;
            }
        };
        if flags & SAMPLE_OVER != 0 {
            fill(start, SAMPLE_OVER);
        }
        if flags & TRUE_PEAK_OVER != 0 {
            fill(start.saturating_sub(TRUE_PEAK_COVERAGE_FRAMES), TRUE_PEAK_OVER);
        }
    }
    projected
}

/// Groups equal warning shapes after projection, with sample overs stronger than simultaneous true peak overs.
fn warning_runs(columns: &[u8]) -> Vec<WarningRun> {
    let mut runs: Vec<WarningRun> = Vec::new();
    for (column, &flags) in columns.iter().enumerate() {
        let kind = if flags & SAMPLE_OVER != 0 {
            WarningKind::Sample
        } else if flags & TRUE_PEAK_OVER != 0 {
            WarningKind::TruePeak
        } else {
            continue;
        };
        if let Some(previous) = runs.last_mut()
            && previous.end == column
            && previous.kind == kind
        {
            previous.end = column + 1;
        } else {
            runs.push(WarningRun {
                start: column,
                end: column + 1,
                kind,
            });
        }
    }
    runs
}

/// Rejects stale file measurements and mismatched stream metadata before sharing history with the paint closure.
fn matching_history(record: &AnalysisRecord) -> Option<Arc<OverloadHistory>> {
    let history = record.overloads.as_ref()?;
    let overview = record.waveform.as_ref()?;
    if !record.is_ready()
        || !history.matches_stream(record.sample_rate, record.length_in_samples, record.channels)
        || overview.sample_rate() != record.sample_rate
        || overview.channels() != record.channels
        || (overview.duration_seconds() - record.length_in_samples as f64 / f64::from(record.sample_rate)).abs()
            > f64::EPSILON
        || !record.matches_file_on_disk()
    {
        return None;
    }
    Some(Arc::clone(history))
}

/// Strokes a hollow edge cap inside its bounds, retaining the underlying waveform in its interior.
fn paint_outline(left: f32, top: f32, width: f32, height: f32, amber: Hsla, window: &mut Window) {
    let inset = OUTLINE_WIDTH / 2.0;
    let mut builder = PathBuilder::stroke(px(OUTLINE_WIDTH));
    builder.add_polygon(
        &[
            point(px(left + inset), px(top + inset)),
            point(px(left + width - inset), px(top + inset)),
            point(px(left + width - inset), px(top + height - inset)),
            point(px(left + inset), px(top + height - inset)),
        ],
        true,
    );
    if let Ok(path) = builder.build() {
        window.paint_path(path, amber);
    }
}

#[cfg(test)]
mod test_projection {
    use super::*;

    /// Builds a valid history with one tenth-second bucket per supplied channel group.
    fn history(channels: u32, length: u64, flags: &[u8]) -> OverloadHistory {
        let history = OverloadHistory {
            sample_rate: 1000,
            length_in_samples: length,
            samples_per_bucket: 100,
            channels,
            flags: flags.to_vec(),
        };
        assert!(history.is_valid());
        history
    }

    #[test]
    fn interpolation_boundary_coverage_does_not_shift_sample_regions() {
        let history = history(1, 300, &[0, SAMPLE_OVER | TRUE_PEAK_OVER, 0]);
        assert_eq!(project_columns(&history, 0, 0.060, 0.040, 4), [TRUE_PEAK_OVER; 4]);
        assert_eq!(project_columns(&history, 0, 0.0, 0.060, 6), [0; 6]);
        assert_eq!(
            project_columns(&history, 0, 0.100, 0.100, 1),
            [SAMPLE_OVER | TRUE_PEAK_OVER]
        );
        assert_eq!(project_columns(&history, 0, 0.200, 0.100, 1), [0]);
        let sample_only = history_with_flags(&history, &[0, SAMPLE_OVER, 0]);
        assert_eq!(project_columns(&sample_only, 0, 0.060, 0.040, 4), [0; 4]);
    }

    /// Changes only the flags while retaining a valid stream and bucket layout.
    fn history_with_flags(history: &OverloadHistory, flags: &[u8]) -> OverloadHistory {
        OverloadHistory {
            flags: flags.to_vec(),
            ..history.clone()
        }
    }

    #[test]
    fn subpixel_aggregation_preserves_isolated_flags_and_sample_precedence() {
        let history = history(1, 500, &[0, TRUE_PEAK_OVER, 0, SAMPLE_OVER, 0]);
        let columns = project_columns(&history, 0, 0.0, 0.5, 1);
        assert_eq!(columns, [SAMPLE_OVER | TRUE_PEAK_OVER]);
        assert_eq!(
            warning_runs(&columns),
            [WarningRun {
                start: 0,
                end: 1,
                kind: WarningKind::Sample
            }]
        );
        assert_eq!(
            warning_runs(&[
                SAMPLE_OVER,
                SAMPLE_OVER | TRUE_PEAK_OVER,
                TRUE_PEAK_OVER,
                0,
                TRUE_PEAK_OVER
            ]),
            [
                WarningRun {
                    start: 0,
                    end: 2,
                    kind: WarningKind::Sample
                },
                WarningRun {
                    start: 2,
                    end: 3,
                    kind: WarningKind::TruePeak
                },
                WarningRun {
                    start: 4,
                    end: 5,
                    kind: WarningKind::TruePeak
                },
            ]
        );
    }

    #[test]
    fn mono_and_stereo_keep_actual_source_lanes_separate() {
        let stereo = history(2, 200, &[SAMPLE_OVER, 0, 0, TRUE_PEAK_OVER]);
        assert_eq!(project_columns(&stereo, 0, 0.0, 0.2, 2), [SAMPLE_OVER, 0]);
        assert_eq!(
            project_columns(&stereo, 1, 0.0, 0.2, 2),
            [TRUE_PEAK_OVER, TRUE_PEAK_OVER]
        );
        assert_eq!(project_columns(&stereo, 2, 0.0, 0.2, 2), [0; 2]);
        let mono = history(1, 200, &[SAMPLE_OVER, 0]);
        assert_eq!(project_columns(&mono, 0, 0.0, 0.2, 2), [SAMPLE_OVER, 0]);
        assert_eq!(project_columns(&mono, 1, 0.0, 0.2, 2), [0; 2]);
    }

    #[test]
    fn final_partial_bucket_and_interpolation_stop_at_source_eof() {
        let history = history(1, 215, &[0, 0, SAMPLE_OVER | TRUE_PEAK_OVER]);
        assert_eq!(
            project_columns(&history, 0, 0.200, 0.030, 2),
            [SAMPLE_OVER | TRUE_PEAK_OVER, 0]
        );
        assert_eq!(project_columns(&history, 0, 0.215, 0.100, 1), [0]);
        assert_eq!(project_columns(&history, 0, 0.0, 0.0, 2), [0; 2]);
        assert_eq!(project_columns(&history, 0, f64::NAN, 0.2, 2), [0; 2]);
        assert_eq!(project_columns(&history, 0, 0.0, 0.2, 0), [] as [u8; 0]);
        let malformed = history_with_flags(&history, &[SAMPLE_OVER]);
        assert_eq!(project_columns(&malformed, 0, 0.0, 0.2, 2), [0; 2]);
    }
}

#[cfg(test)]
mod test_attachment {
    use audiobatch_core::AnalysisStatus;
    use audiobatch_core::record::file_identity;
    use audiobatch_core::waveform::WaveformBuilder;
    use tempfile::NamedTempFile;

    use super::*;

    /// Makes an identity-matched record without decoding audio or creating any GUI context.
    fn record(file: &NamedTempFile, flags: u8) -> AnalysisRecord {
        let (file_size, modified_time_ms) = file_identity(file.path()).expect("temporary source identity");
        let mut waveform = WaveformBuilder::new(1, Some(100));
        waveform.push(&[&[0.0; 100]]);
        AnalysisRecord {
            path: file.path().to_path_buf(),
            file_size,
            modified_time_ms,
            length_in_samples: 100,
            sample_rate: 1000,
            channels: 1,
            status: AnalysisStatus::Analyzed,
            waveform: Some(waveform.finish(1000, 100)),
            overloads: Some(Arc::new(OverloadHistory {
                sample_rate: 1000,
                length_in_samples: 100,
                samples_per_bucket: 100,
                channels: 1,
                flags: vec![flags],
            })),
            ..AnalysisRecord::default()
        }
    }

    #[test]
    fn identity_changes_missing_sources_and_clear_drop_previous_warnings() {
        let first = NamedTempFile::new().expect("first source");
        let second = NamedTempFile::new().expect("second source");
        let first_record = record(&first, SAMPLE_OVER);
        let second_record = record(&second, TRUE_PEAK_OVER);
        let mut attached = SourceOverloads::default();
        for next in [&first_record, &second_record, &first_record] {
            attached.set_record(next);
            assert!(Arc::ptr_eq(
                attached.history().expect("attached history"),
                next.overloads.as_ref().expect("history")
            ));
        }
        let mut stale = first_record.clone();
        stale.file_size += 1;
        attached.set_record(&stale);
        assert!(attached.history().is_none());
        attached.set_record(&first_record);
        attached.clear();
        assert!(attached.history().is_none());
        assert!(!attached.has_overloads());
        first.as_file().set_len(1).expect("same-path source replacement");
        attached.set_record(&first_record);
        assert!(attached.history().is_none());
        drop(first);
        attached.set_record(&first_record);
        assert!(attached.history().is_none());
    }

    #[test]
    fn rejects_unready_missing_or_inconsistent_analysis_without_retaining_previous_history() {
        let source = NamedTempFile::new().expect("source");
        let original = record(&source, SAMPLE_OVER);
        let mut attached = SourceOverloads::default();
        for invalid in [
            AnalysisRecord {
                overloads: None,
                ..original.clone()
            },
            AnalysisRecord {
                waveform: None,
                ..original.clone()
            },
            AnalysisRecord {
                status: AnalysisStatus::Pending,
                ..original.clone()
            },
            AnalysisRecord {
                sample_rate: 2000,
                ..original.clone()
            },
            AnalysisRecord {
                channels: 2,
                ..original.clone()
            },
            AnalysisRecord {
                length_in_samples: 101,
                ..original.clone()
            },
        ] {
            attached.set_record(&original);
            assert!(attached.has_overloads());
            attached.set_record(&invalid);
            assert!(attached.history().is_none());
        }
        let mut wrong_waveform = original.clone();
        wrong_waveform.waveform = Some(WaveformBuilder::new(2, Some(100)).finish(1000, 100));
        attached.set_record(&wrong_waveform);
        assert!(attached.history().is_none());
        let mut wrong_duration = original.clone();
        wrong_duration.waveform = Some(WaveformBuilder::new(1, Some(100)).finish(1000, 99));
        attached.set_record(&wrong_duration);
        assert!(attached.history().is_none());
    }

    #[test]
    fn all_clear_is_attached_and_input_gain_does_not_change_source_history() {
        let source = NamedTempFile::new().expect("source");
        let mut record = record(&source, 0);
        let mut attached = SourceOverloads::default();
        attached.set_record(&record);
        assert!(attached.history().is_some());
        assert!(!attached.has_overloads());
        record.overloads = Some(Arc::new(OverloadHistory {
            flags: vec![SAMPLE_OVER],
            ..record.overloads.as_ref().expect("history").as_ref().clone()
        }));
        attached.set_record(&record);
        let history = Arc::clone(attached.history().expect("source warnings"));
        for gain in [-24.0, 0.0, 12.0] {
            record.custom_gain_db = Some(gain);
            attached.set_record(&record);
            assert!(Arc::ptr_eq(attached.history().expect("source warnings"), &history));
        }
    }
}
