//! The information panel of the selected file, beside the waveform.
//!
//! It shows the file name split into artist and title at the first " - ", beside the embedded album artwork,
//! actionable error details when analysis fails,
//! four metric cards for the levels, and a scrollable list of technical details and tags.
//! The tooltips show the full path on the name, and each value spelled out.
//! The wide variant puts the metric cards in one row, and the details with short values two to a line,
//! above the tags that can be long, such as the album and the comment, which keep the full width.
//! The compact variant for the collapsed preview shows only the four metric cards, in one row.

use std::sync::Arc;

use audiobatch_core::format::{
    format_bitrate, format_bits_per_sample, format_loudness_compact, format_peak_compact, format_sample_rate,
    format_true_peak_compact, format_type_label, gain_to_decibels,
};
use audiobatch_core::gain_loudness::{GainLoudness, estimate_limiter_integrated};
use audiobatch_core::record::is_silent_loudness;
use audiobatch_core::serato_tags::SeratoData;
use audiobatch_core::{AnalysisRecord, FileTags};
use gpui_kit::{
    AnyElement, Div, Hsla, InteractiveElement as _, IntoElement, ObjectFit, ParentElement as _, RenderImage, Stateful,
    StatefulInteractiveElement as _, Styled as _, StyledImage as _, div, img, px, rems,
};

use crate::results::{ResultRow, RowState};
use crate::theme::{MONO_FONT, color, palette, tooltip, white};

/// Corner radius of the panel and its metric cards.
pub const CORNER_RADIUS: f32 = 6.0;

/// Opacity of the panel and card outlines.
pub const OUTLINE_ALPHA: f32 = 0.08;

/// Horizontal padding inside the panel.
const HORIZONTAL_PADDING: f32 = 10.0;

/// Vertical padding inside the panel.
const VERTICAL_PADDING: f32 = 8.0;

/// Space between the sections and between the metric cards.
const SECTION_SPACING: f32 = 10.0;

/// Height of a metric card.
const METRIC_HEIGHT: f32 = 52.0;

/// Height of the artist and title lines.
const HEADING_HEIGHT: f32 = 22.0;

/// Side of the square album artwork beside the artist and title.
const ARTWORK_SIZE: f32 = 48.0;

/// Space between the album artwork and the artist and title.
const ARTWORK_SPACING: f32 = 8.0;

/// Height of a detail row.
const DETAIL_ROW_HEIGHT: f32 = 20.0;

/// Width of the field names of the detail rows.
const DETAIL_FIELD_WIDTH: f32 = 64.0;

/// Font size of the artist and title.
/// The sizes are derived from JUCE font heights, so the text is as large as in the C++ application's panel.
/// A JUCE height is the ascent plus the descent, 1.17 em for Roboto and 1.32 em for Roboto Mono,
/// so this is its height of 13 in Roboto.
const HEADING_FONT_SIZE: f32 = 11.0;

/// Font size of captions, units, and field names, the JUCE height of 12 in Roboto.
const CAPTION_FONT_SIZE: f32 = 10.25;

/// Font size of detail values and errors, the JUCE height of 13 in Roboto.
const VALUE_FONT_SIZE: f32 = 11.0;

/// Font size of the metric values, the JUCE height of 20 in Roboto Mono.
const METRIC_FONT_SIZE: f32 = 15.0;

/// Font size of the metric values in the compact row,
/// small enough that a value such as "-17.16" fits beside its caption in the narrowest card.
const COMPACT_METRIC_FONT_SIZE: f32 = 13.0;

/// Line height of the metric values.
const METRIC_LINE_HEIGHT: f32 = 20.0;

/// Line height of the caption and unit row of a metric card.
/// Together with the value line it fits inside the card height minus its padding and border.
const METRIC_CAPTION_LINE_HEIGHT: f32 = 14.0;

/// Separator between the artist and the title in a file name.
const TITLE_SEPARATOR: &str = " - ";

/// Space between the cards of the compact panel, and between a card's labels and its value.
pub const COMPACT_METRIC_SPACING: f32 = 5.0;

/// Space between the metric cards of the wide panel, which share one row.
const WIDE_METRIC_SPACING: f32 = 6.0;

/// One level measurement shown on a metric card.
struct Metric {
    /// Full name of the measurement, used in the tooltip and as the element id.
    name: &'static str,
    /// Short caption shown on the card.
    caption: &'static str,
    /// Unit of the value.
    unit: &'static str,
    /// Formatted value, or a dash when there is no result.
    value: String,
    /// Predicted level after custom gain, with approximate loudness values marked by "≈".
    adjusted_value: Option<String>,
}

/// One row of the details below the metric cards.
struct Detail {
    /// Field name shown on the left, also used as the element id.
    field: &'static str,
    /// Value shown aligned to the right.
    value: String,
    /// True for a value that always stays short, such as the format or the tempo,
    /// which the wide panel shows beside another one on the same line.
    short: bool,
}

/// Renders the panel for the selected row and its tags, or the empty state when nothing is selected.
/// The wide panel puts the four metric cards in one row,
/// and the details with short values two to a line, above the long ones at the full width.
/// The album artwork, when there is one, shows beside the artist and title,
/// and the heading grows to its height.
pub fn render_info_panel(
    row: Option<&ResultRow>,
    tags: Option<&FileTags>,
    artwork: Option<Arc<RenderImage>>,
    wide: bool,
    show_gain_adjusted_metrics: bool,
    gained_loudness: Option<GainLoudness>,
) -> AnyElement {
    let metric_cards = |record| {
        if wide {
            metric_row(record, show_gain_adjusted_metrics, gained_loudness)
        } else {
            metric_grid(record, show_gain_adjusted_metrics, gained_loudness)
        }
    };
    let panel = div()
        .id("info-panel")
        .size_full()
        .flex()
        .flex_col()
        .px(px(HORIZONTAL_PADDING))
        .py(px(VERTICAL_PADDING))
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_SEMI_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA));

    let Some(row) = row else {
        return panel
            .child(heading_line("artist", String::from("No file selected"), None))
            .child(div().h(px(SECTION_SPACING)))
            .child(metric_cards(None))
            .into_any_element();
    };

    let file_name = row.file_name.as_ref();
    let (artist, title) = file_name
        .split_once(TITLE_SEPARATOR)
        .map_or((file_name, None), |(artist, title)| (artist, Some(title)));
    let path = row.path.display().to_string();
    let ready = row.ready_record();
    let panel = panel.child(heading(artist, title, &path, artwork));

    let mut details = div()
        .id("info-details")
        .flex_1()
        .min_h_0()
        .flex()
        .flex_col()
        .overflow_y_scroll();
    if row.state == RowState::Done
        && let Some(record) = row.record.as_ref().filter(|record| record.has_error())
    {
        details = details.child(
            div()
                .pb(px(VERTICAL_PADDING))
                .text_size(px(VALUE_FONT_SIZE))
                .text_color(color(palette::RED))
                .child(record.error_message.clone()),
        );
    }
    let all_details = ready
        .map(record_rows)
        .unwrap_or_default()
        .into_iter()
        .chain(tags.map(tag_rows).unwrap_or_default());
    if wide {
        let (short, long): (Vec<Detail>, Vec<Detail>) = all_details.partition(|detail| detail.short);
        let mut short = short.into_iter();
        while let Some(first) = short.next() {
            let second = short.next().map_or_else(
                || div().flex_1().into_any_element(),
                |detail| detail_row(detail).flex_1().min_w_0().into_any_element(),
            );
            details = details.child(
                div()
                    .flex()
                    .flex_row()
                    .flex_none()
                    .gap(px(SECTION_SPACING))
                    .child(detail_row(first).flex_1().min_w_0())
                    .child(second),
            );
        }
        details = details.children(long.into_iter().map(detail_row));
    } else {
        details = details.children(all_details.map(detail_row));
    }

    panel
        .child(div().h(px(SECTION_SPACING)))
        .child(metric_cards(ready))
        .child(div().h(px(SECTION_SPACING)))
        .child(details)
        .into_any_element()
}

/// Renders the artist and title lines, beside the album artwork when there is one.
/// The artwork is centred against the lines, and the heading grows to its height.
fn heading(artist: &str, title: Option<&str>, path: &str, artwork: Option<Arc<RenderImage>>) -> Div {
    let mut lines =
        div()
            .flex_1()
            .min_w_0()
            .flex()
            .flex_col()
            .child(heading_line("artist", artist.to_string(), Some(path)));
    if let Some(title) = title {
        lines = lines.child(heading_line("title", title.to_string(), Some(path)));
    }
    let Some(artwork) = artwork else {
        return lines.flex_none();
    };
    div()
        .flex()
        .flex_row()
        .flex_none()
        .items_center()
        .min_h(px(ARTWORK_SIZE))
        .gap(px(ARTWORK_SPACING))
        .child(
            img(artwork)
                .id("artwork")
                .size(px(ARTWORK_SIZE))
                .flex_none()
                .object_fit(ObjectFit::Contain),
        )
        .child(lines)
}

/// Renders one line of the heading, with the full path of the file as its tooltip.
fn heading_line(id: &'static str, text: String, path: Option<&str>) -> Stateful<Div> {
    let line = div()
        .id(id)
        .h(px(HEADING_HEIGHT))
        .flex()
        .items_center()
        .flex_none()
        .text_size(px(HEADING_FONT_SIZE))
        .text_color(color(palette::GREY_SUPER_LIGHT))
        .child(div().min_w_0().truncate().child(text));
    match path {
        Some(path) => line.tooltip(tooltip(path.to_string())),
        None => line,
    }
}

/// Renders the compact variant for the collapsed preview: the four level cards in one row,
/// each with its caption above its unit on the left and the value on the right, and dashes when there is no result.
/// The source value never shrinks, while the caption, unit, and gain prediction can truncate in narrow cards.
/// The file name and the details are left out, so the metrics fit next to the waveform.
pub fn render_compact_info_panel(
    row: Option<&ResultRow>,
    show_gain_adjusted_metrics: bool,
    gained_loudness: Option<GainLoudness>,
) -> Div {
    let metrics = metrics(
        row.and_then(ResultRow::ready_record),
        show_gain_adjusted_metrics,
        gained_loudness,
    );
    div()
        .size_full()
        .flex()
        .flex_row()
        .gap(px(COMPACT_METRIC_SPACING))
        .children(metrics.into_iter().map(|metric| {
            metric_card_frame(&metric)
                .h_full()
                .flex_row()
                .items_center()
                .gap(px(COMPACT_METRIC_SPACING))
                .px(px(6.0))
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .flex()
                        .flex_col()
                        .text_size(px(CAPTION_FONT_SIZE))
                        .line_height(px(METRIC_CAPTION_LINE_HEIGHT))
                        .text_color(color(palette::GREY_MIDDLE_LIGHT))
                        .child(div().truncate().child(metric.caption))
                        .child(div().truncate().child(metric.unit)),
                )
                .child(metric_value(metric.value, metric.adjusted_value).text_size(px(COMPACT_METRIC_FONT_SIZE)))
        }))
}

/// Returns the four level metrics of a record, with dashes when there is no result.
fn metrics(
    record: Option<&AnalysisRecord>,
    show_gain_adjusted_metrics: bool,
    gained_loudness: Option<GainLoudness>,
) -> [Metric; 4] {
    let value = |format: fn(&AnalysisRecord) -> String| record.map_or_else(|| String::from("-"), format);
    let adjusted_record = record.filter(|_| show_gain_adjusted_metrics);
    [
        Metric {
            name: "Peak",
            caption: "Peak",
            unit: "dBFS",
            value: value(|record| format_peak_compact(f64::from(record.overall_peak))),
            adjusted_value: adjusted_record
                .and_then(|record| adjusted_peak(f64::from(record.overall_peak), record.custom_gain_db)),
        },
        Metric {
            name: "True peak",
            caption: "True peak",
            unit: "dBTP",
            value: value(|record| format_true_peak_compact(record.overall_true_peak)),
            adjusted_value: adjusted_record
                .and_then(|record| adjusted_peak(record.overall_true_peak, record.custom_gain_db)),
        },
        Metric {
            name: "Max short-term",
            caption: "Short-term",
            unit: "LUFS",
            value: value(|record| format_loudness_compact(record.max_short_term_lufs)),
            adjusted_value: adjusted_record.and_then(|record| {
                adjusted_loudness(gained_loudness.map(GainLoudness::max_short_term), record.custom_gain_db)
            }),
        },
        Metric {
            name: "Integrated loudness",
            caption: "Integrated",
            unit: "LUFS",
            value: value(|record| format_loudness_compact(record.integrated_lufs)),
            adjusted_value: adjusted_record.and_then(|record| {
                let gain = record.custom_gain_db?;
                let measured = gained_loudness?;
                adjusted_loudness(
                    Some(estimate_limiter_integrated(
                        record.integrated_lufs,
                        gain,
                        measured.integrated(),
                    )),
                    Some(gain),
                )
            }),
        },
    ]
}

/// Formats the predicted peak after custom gain without changing the source measurement.
/// Silence stays silent, and values rounding to zero have a positive sign rather than negative zero.
fn adjusted_peak(amplitude: f64, gain_db: Option<f32>) -> Option<String> {
    let gain_db = gain_db.filter(|gain| gain.is_finite())?;
    let decibels = gain_to_decibels(amplitude.abs(), f64::NEG_INFINITY) + f64::from(gain_db);
    let rounded = (decibels * 10.0).round() / 10.0;
    Some(if amplitude.abs() <= 0.0 {
        String::from("(-INF)")
    } else if rounded.abs() < f64::EPSILON {
        String::from("(+0.0)")
    } else {
        format!("({rounded:+.1})")
    })
}

/// Formats only completed gain-and-clip remeasurements, never a misleading source LUFS plus gain.
fn adjusted_loudness(loudness: Option<f64>, gain_db: Option<f32>) -> Option<String> {
    gain_db.filter(|gain| gain.is_finite())?;
    let loudness = loudness.filter(|value| value.is_finite())?;
    Some(if is_silent_loudness(loudness) {
        String::from("(≈-INF)")
    } else {
        format!("(≈{loudness:.1})")
    })
}

/// Renders the four level cards in two rows.
fn metric_grid(
    record: Option<&AnalysisRecord>,
    show_gain_adjusted_metrics: bool,
    gained_loudness: Option<GainLoudness>,
) -> Div {
    let [peak, true_peak, short_term, integrated] = metrics(record, show_gain_adjusted_metrics, gained_loudness);
    div()
        .flex()
        .flex_col()
        .flex_none()
        .gap(px(SECTION_SPACING))
        .child(
            div()
                .flex()
                .flex_row()
                .gap(px(SECTION_SPACING))
                .child(metric_card(peak))
                .child(metric_card(true_peak)),
        )
        .child(
            div()
                .flex()
                .flex_row()
                .gap(px(SECTION_SPACING))
                .child(metric_card(short_term))
                .child(metric_card(integrated)),
        )
}

/// Renders the four level cards in one row, for the wide panel.
fn metric_row(
    record: Option<&AnalysisRecord>,
    show_gain_adjusted_metrics: bool,
    gained_loudness: Option<GainLoudness>,
) -> Div {
    div()
        .flex()
        .flex_row()
        .flex_none()
        .gap(px(WIDE_METRIC_SPACING))
        .children(
            metrics(record, show_gain_adjusted_metrics, gained_loudness)
                .into_iter()
                .map(metric_card),
        )
}

/// Renders a metric card: the caption and unit on top, the value large below.
/// A card too narrow for both cuts off the caption, never the unit.
fn metric_card(metric: Metric) -> Stateful<Div> {
    let caption_color: Hsla = color(palette::GREY_MIDDLE_LIGHT);
    metric_card_frame(&metric)
        .h(px(METRIC_HEIGHT))
        .flex_col()
        .justify_between()
        .p(px(6.0))
        .child(
            div()
                .flex()
                .flex_row()
                .justify_between()
                .gap(px(4.0))
                .text_size(px(CAPTION_FONT_SIZE))
                .line_height(px(METRIC_CAPTION_LINE_HEIGHT))
                .text_color(caption_color)
                .child(div().min_w_0().truncate().child(metric.caption))
                .child(div().flex_none().child(metric.unit)),
        )
        .child(metric_value(metric.value, metric.adjusted_value))
}

/// Renders the outlined frame of a metric card, with a tooltip spelling out the measurement with its value and unit.
fn metric_card_frame(metric: &Metric) -> Stateful<Div> {
    let description = metric.adjusted_value.as_ref().map_or_else(
        || format!("{}: {} {}", metric.name, metric.value, metric.unit),
        |adjusted| {
            format!(
                "{}: {} {}. After custom gain: {} {}. Peaks predict gain before clipping. \
                 Short-term loudness uses hard clipping at 0 dBFS. Integrated loudness uses an empirical L2 limiter fit \
                 with a -0.3 dB ceiling for 0 through +6 dB gain, otherwise hard clipping. \
                 Calibrated on music at -13.33 through -8.68 LUFS-I, extrapolated outside that source range. \
                 This is not a measurement of the active plugin chain.",
                metric.name, metric.value, metric.unit, adjusted, metric.unit
            )
        },
    );
    div()
        .id(metric.name)
        .tooltip(tooltip(description))
        .flex_1()
        .min_w_0()
        .flex()
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_MEDIUM_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
}

/// Renders the source value with a smaller, muted custom-gain prediction on the same baseline.
/// The source value stays visible first when the card is too narrow for both.
fn metric_value(value: String, adjusted_value: Option<String>) -> Div {
    div()
        .min_w_0()
        .flex()
        .items_baseline()
        .gap_1()
        .font_family(MONO_FONT)
        .text_size(px(METRIC_FONT_SIZE))
        .line_height(px(METRIC_LINE_HEIGHT))
        .text_color(color(palette::GREY_SUPER_LIGHT))
        .overflow_hidden()
        .child(div().flex_none().child(value))
        .children(adjusted_value.map(|adjusted| {
            div()
                .min_w_0()
                .truncate()
                .text_size(rems(0.75))
                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                .child(adjusted)
        }))
}

/// Renders a field name and its value aligned to the right edge, with both in the tooltip for a value too long to show.
fn detail_row(detail: Detail) -> Stateful<Div> {
    let Detail { field, value, .. } = detail;
    div()
        .id(field)
        .tooltip(tooltip(format!("{field}: {value}")))
        .h(px(DETAIL_ROW_HEIGHT))
        .flex_none()
        .flex()
        .flex_row()
        .items_center()
        .child(
            div()
                .w(px(DETAIL_FIELD_WIDTH))
                .flex_none()
                .text_size(px(CAPTION_FONT_SIZE))
                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                .child(field),
        )
        .child(
            div()
                .flex_1()
                .min_w_0()
                .text_right()
                .truncate()
                .text_size(px(VALUE_FONT_SIZE))
                .text_color(color(palette::GREY_LIGHT))
                .child(value),
        )
}

/// Returns the technical details of a ready record, all of them short.
fn record_rows(record: &AnalysisRecord) -> Vec<Detail> {
    let channels = match record.channels {
        1 => String::from("Mono"),
        2 => String::from("Stereo"),
        count => count.to_string(),
    };
    [
        (
            "Format",
            format!(
                "{} / {channels} / {} bit",
                format_type_label(record),
                format_bits_per_sample(record)
            ),
        ),
        (
            "Audio",
            format!("{} / {}", format_sample_rate(record), format_bitrate(record)),
        ),
        ("Duration", describe_duration(record.duration_seconds)),
        ("Samples", record.length_in_samples.to_string()),
    ]
    .into_iter()
    .map(|(field, value)| Detail {
        field,
        value,
        short: true,
    })
    .collect()
}

/// Returns the musical details and tags that are set.
/// The tempo prefers Serato's analyzed value over the tag and the beat grid.
/// The tempo, the key, and the year are short, and the album, the genre, and the comment can be long.
fn tag_rows(tags: &FileTags) -> Vec<Detail> {
    let serato_bpm = tags.serato.as_ref().and_then(SeratoData::bpm);
    let bpm = serato_bpm
        .or_else(|| tags.bpm.as_deref().and_then(|bpm| bpm.trim().parse::<f64>().ok()))
        .filter(|bpm| bpm.is_finite() && *bpm > 0.0)
        .map(|bpm| format!("{bpm:.2}"));
    [
        ("BPM", bpm, true),
        ("Key", tags.key.clone(), true),
        ("Album", tags.album.clone(), false),
        ("Genre", tags.genre.clone(), false),
        ("Year", tags.year.map(|year| year.to_string()), true),
        ("Comment", tags.comment.clone(), false),
    ]
    .into_iter()
    .filter_map(|(field, value, short)| {
        value
            .filter(|value| !value.trim().is_empty())
            .map(|value| Detail { field, value, short })
    })
    .collect()
}

/// Describes a duration in words.
/// Shows the two largest nonzero units, for example "3 mins 25 secs" or "1 hr 2 mins",
/// and milliseconds only for durations under one second.
fn describe_duration(seconds: f64) -> String {
    if !seconds.is_finite() || seconds.abs() < 0.001 {
        return String::from("0");
    }
    if seconds < 0.0 {
        return format!("-{}", describe_duration(-seconds));
    }
    let whole_seconds = seconds as u64;
    let fields = [
        (whole_seconds / 604_800, "week", "weeks"),
        ((whole_seconds / 86_400) % 7, "day", "days"),
        ((whole_seconds / 3600) % 24, "hr", "hrs"),
        ((whole_seconds / 60) % 60, "min", "mins"),
        (whole_seconds % 60, "sec", "secs"),
    ];
    let parts: Vec<String> = fields
        .iter()
        .filter(|(count, ..)| *count > 0)
        .take(2)
        .map(|&(count, singular, plural)| {
            if count == 1 {
                format!("1 {singular}")
            } else {
                format!("{count} {plural}")
            }
        })
        .collect();
    if parts.is_empty() {
        let milliseconds = ((seconds * 1000.0) as u64) % 1000;
        return format!("{milliseconds} ms");
    }
    parts.join(" ")
}

#[cfg(test)]
mod test_metrics {
    use audiobatch_core::record::decibels_to_gain;

    use super::*;

    #[test]
    fn custom_gain_predicts_both_peaks_without_changing_source_values() {
        let record = AnalysisRecord {
            overall_peak: decibels_to_gain(0.9) as f32,
            overall_true_peak: decibels_to_gain(1.5),
            custom_gain_db: Some(-1.0),
            max_short_term_lufs: -8.5,
            integrated_lufs: -12.5,
            ..AnalysisRecord::default()
        };
        let [peak, true_peak, short_term, integrated] = metrics(Some(&record), true, None);
        assert_eq!(peak.value, "0.90");
        assert_eq!(peak.adjusted_value.as_deref(), Some("(-0.1)"));
        assert_eq!(true_peak.value, "1.50");
        assert_eq!(true_peak.adjusted_value.as_deref(), Some("(+0.5)"));
        assert!(short_term.adjusted_value.is_none());
        assert!(integrated.adjusted_value.is_none());
        assert_eq!(integrated.value, "-12.50");
        assert!(
            metrics(Some(&record), false, None)
                .iter()
                .all(|metric| metric.adjusted_value.is_none())
        );
    }

    #[test]
    fn predictions_are_absent_without_gain_or_a_record() {
        for record in [None, Some(AnalysisRecord::default()).as_ref()] {
            assert!(
                metrics(record, true, None)
                    .iter()
                    .all(|metric| metric.adjusted_value.is_none())
            );
        }
        assert!(adjusted_peak(1.0, Some(f32::NAN)).is_none());
    }

    #[test]
    fn loudness_predictions_require_completed_measurements_and_do_not_add_gain_again() {
        assert_eq!(adjusted_loudness(Some(-14.0), Some(12.0)).as_deref(), Some("(≈-14.0)"));
        assert!(adjusted_loudness(None, Some(12.0)).is_none());
        assert!(adjusted_loudness(Some(-14.0), None).is_none());
        assert!(adjusted_loudness(Some(-14.0), Some(f32::INFINITY)).is_none());
        assert!(adjusted_loudness(Some(f64::NAN), Some(10.0)).is_none());
        assert_eq!(
            adjusted_loudness(Some(audiobatch_core::record::NEGATIVE_INFINITY_LOUDNESS), Some(10.0)).as_deref(),
            Some("(≈-INF)")
        );
    }

    #[test]
    fn only_integrated_loudness_uses_the_limiter_fit() {
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        let measured = audiobatch_core::gain_loudness::measure_gained_loudness(&path, 2.0, &|| false)
            .expect("measures gained fixture");
        let record = AnalysisRecord {
            integrated_lufs: -12.0,
            custom_gain_db: Some(2.0),
            ..AnalysisRecord::default()
        };
        let [_, _, short_term, integrated] = metrics(Some(&record), true, Some(measured));
        assert_eq!(
            integrated.adjusted_value,
            adjusted_loudness(
                Some(estimate_limiter_integrated(-12.0, 2.0, measured.integrated())),
                Some(2.0)
            )
        );
        assert_eq!(
            short_term.adjusted_value,
            adjusted_loudness(Some(measured.max_short_term()), Some(2.0))
        );
    }

    #[test]
    fn positive_gain_and_silence_are_formatted() {
        assert_eq!(
            adjusted_peak(decibels_to_gain(-2.0), Some(3.0)).as_deref(),
            Some("(+1.0)")
        );
        assert_eq!(adjusted_peak(0.0, Some(3.0)).as_deref(), Some("(-INF)"));
        assert_eq!(
            adjusted_peak(decibels_to_gain(0.99), Some(-1.0)).as_deref(),
            Some("(+0.0)")
        );
    }
}

#[cfg(test)]
mod test_describe_duration {
    use super::*;

    #[test]
    fn shows_the_two_largest_units() {
        assert_eq!(describe_duration(1.0), "1 sec");
        assert_eq!(describe_duration(205.4), "3 mins 25 secs");
        assert_eq!(describe_duration(3725.0), "1 hr 2 mins");
        assert_eq!(describe_duration(120.0), "2 mins");
    }

    #[test]
    fn short_durations_use_milliseconds() {
        assert_eq!(describe_duration(0.25), "250 ms");
        assert_eq!(describe_duration(0.0), "0");
    }
}
