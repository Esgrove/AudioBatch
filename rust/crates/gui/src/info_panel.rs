//! The information panel of the selected file, beside the waveform.
//!
//! It shows the file name split into artist and title at the first " - ", a status line while the file has no result,
//! four metric cards for the levels, and a scrollable list of technical details and tags.
//! The tooltips show the full path on the name, and each value spelled out.
//! The compact variant for the collapsed preview shows only the four metric cards, in one row.

use audiobatch_core::format::{
    format_bitrate, format_bits_per_sample, format_loudness_compact, format_peak_compact, format_sample_rate,
    format_true_peak_compact, format_type_label,
};
use audiobatch_core::serato_tags::BeatGrid;
use audiobatch_core::{AnalysisRecord, FileTags};
use gpui_kit::{
    AnyElement, Div, Hsla, InteractiveElement as _, IntoElement, ParentElement as _, Stateful,
    StatefulInteractiveElement as _, Styled as _, div, px,
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

/// Height of a detail row.
const DETAIL_ROW_HEIGHT: f32 = 20.0;

/// Width of the field names of the detail rows.
const DETAIL_FIELD_WIDTH: f32 = 64.0;

/// Font size of the artist and title.
const HEADING_FONT_SIZE: f32 = 13.0;

/// Font size of captions, units, and field names.
const CAPTION_FONT_SIZE: f32 = 12.0;

/// Font size of detail values and errors.
const VALUE_FONT_SIZE: f32 = 13.0;

/// Font size of the metric values.
/// It matches the C++ panel's JUCE font height of 20, the ascent plus the descent, so both applications look alike.
const METRIC_FONT_SIZE: f32 = 17.0;

/// Line height of the metric values.
const METRIC_LINE_HEIGHT: f32 = 20.0;

/// Line height of the caption and unit row of a metric card.
/// Together with the value line it fits inside the card height minus its padding and border.
const METRIC_CAPTION_LINE_HEIGHT: f32 = 14.0;

/// Separator between the artist and the title in a file name.
const TITLE_SEPARATOR: &str = " - ";

/// Space between the cards of the compact panel, and between a card's labels and its value.
pub const COMPACT_METRIC_SPACING: f32 = 5.0;

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
}

/// Renders the panel for the selected row and its tags, or the empty state when nothing is selected.
pub fn render_info_panel(row: Option<&ResultRow>, tags: Option<&FileTags>) -> AnyElement {
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
            .child(metric_grid(None))
            .into_any_element();
    };

    let file_name = row.file_name.as_ref();
    let (artist, title) = file_name
        .split_once(TITLE_SEPARATOR)
        .map_or((file_name, None), |(artist, title)| (artist, Some(title)));
    let path = row.path.display().to_string();
    let ready = row.ready_record();
    let mut panel = panel.child(heading_line("artist", artist.to_string(), Some(&path)));
    if let Some(title) = title {
        panel = panel.child(heading_line("title", title.to_string(), Some(&path)));
    }
    if ready.is_none() {
        let status_color = if row.has_error() {
            color(palette::RED)
        } else {
            color(palette::GREY_MIDDLE_LIGHT)
        };
        panel = panel.child(
            div()
                .h(px(18.0))
                .flex()
                .items_center()
                .text_size(px(CAPTION_FONT_SIZE))
                .text_color(status_color)
                .child(row.status_text()),
        );
    }

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
    let mut rows = ready.map(record_rows).unwrap_or_default();
    if let Some(tags) = tags {
        rows.extend(tag_rows(tags));
    }
    for (field, value) in rows {
        details = details.child(detail_row(field, value));
    }

    panel
        .child(div().h(px(SECTION_SPACING)))
        .child(metric_grid(ready))
        .child(div().h(px(SECTION_SPACING)))
        .child(details)
        .into_any_element()
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
/// The file name and the details are left out, so the metrics fit next to the waveform.
pub fn render_compact_info_panel(row: Option<&ResultRow>) -> Div {
    let metrics = metrics(row.and_then(ResultRow::ready_record));
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
                        .flex()
                        .flex_col()
                        .flex_none()
                        .text_size(px(CAPTION_FONT_SIZE))
                        .line_height(px(METRIC_CAPTION_LINE_HEIGHT))
                        .text_color(color(palette::GREY_MIDDLE_LIGHT))
                        .child(metric.caption)
                        .child(metric.unit),
                )
                .child(metric_value(metric.value).flex_1().text_right())
        }))
}

/// Returns the four level metrics of a record, with dashes when there is no result.
fn metrics(record: Option<&AnalysisRecord>) -> [Metric; 4] {
    let value = |format: fn(&AnalysisRecord) -> String| record.map_or_else(|| String::from("-"), format);
    [
        Metric {
            name: "Peak",
            caption: "Peak",
            unit: "dBFS",
            value: value(|record| format_peak_compact(f64::from(record.overall_peak))),
        },
        Metric {
            name: "True peak",
            caption: "True peak",
            unit: "dBTP",
            value: value(|record| format_true_peak_compact(record.overall_true_peak)),
        },
        Metric {
            name: "Max short-term",
            caption: "Short-term",
            unit: "LUFS",
            value: value(|record| format_loudness_compact(record.max_short_term_lufs)),
        },
        Metric {
            name: "Integrated loudness",
            caption: "Integrated",
            unit: "LUFS",
            value: value(|record| format_loudness_compact(record.integrated_lufs)),
        },
    ]
}

/// Renders the four level cards in two rows.
fn metric_grid(record: Option<&AnalysisRecord>) -> Div {
    let [peak, true_peak, short_term, integrated] = metrics(record);
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

/// Renders a metric card: the caption and unit on top, the value large below.
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
                .text_size(px(CAPTION_FONT_SIZE))
                .line_height(px(METRIC_CAPTION_LINE_HEIGHT))
                .text_color(caption_color)
                .child(metric.caption)
                .child(metric.unit),
        )
        .child(metric_value(metric.value))
}

/// Renders the outlined frame of a metric card, with a tooltip spelling out the measurement with its value and unit.
fn metric_card_frame(metric: &Metric) -> Stateful<Div> {
    div()
        .id(metric.name)
        .tooltip(tooltip(format!("{}: {} {}", metric.name, metric.value, metric.unit)))
        .flex_1()
        .min_w_0()
        .flex()
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_MEDIUM_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
}

/// Renders the large value of a metric card.
fn metric_value(value: String) -> Div {
    div()
        .min_w_0()
        .font_family(MONO_FONT)
        .text_size(px(METRIC_FONT_SIZE))
        .line_height(px(METRIC_LINE_HEIGHT))
        .text_color(color(palette::GREY_SUPER_LIGHT))
        .truncate()
        .child(value)
}

/// Renders a field name and its value, with both in the tooltip for a value too long to show.
fn detail_row(field: &'static str, value: String) -> Stateful<Div> {
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
                .min_w_0()
                .truncate()
                .text_size(px(VALUE_FONT_SIZE))
                .text_color(color(palette::GREY_LIGHT))
                .child(value),
        )
}

/// Returns the technical details of a ready record.
fn record_rows(record: &AnalysisRecord) -> Vec<(&'static str, String)> {
    let channels = match record.channels {
        1 => String::from("Mono"),
        2 => String::from("Stereo"),
        count => count.to_string(),
    };
    vec![
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
        (
            "Peak L / R",
            format!(
                "{} / {} dBFS",
                format_peak_compact(f64::from(record.peak_left)),
                format_peak_compact(f64::from(record.peak_right))
            ),
        ),
    ]
}

/// Returns the musical details and tags that are set.
/// The tempo prefers Serato's analyzed value over the tag and the beat grid.
fn tag_rows(tags: &FileTags) -> Vec<(&'static str, String)> {
    let serato_bpm = tags.serato.as_ref().and_then(|serato| {
        serato
            .bpm()
            .or_else(|| serato.beatgrid.as_ref().and_then(BeatGrid::bpm))
    });
    let bpm = serato_bpm
        .map(f64::from)
        .or_else(|| tags.bpm.as_deref().and_then(|bpm| bpm.trim().parse::<f64>().ok()))
        .filter(|bpm| bpm.is_finite() && *bpm > 0.0)
        .map(|bpm| format!("{bpm:.2}"));
    [
        ("BPM", bpm),
        ("Key", tags.key.clone()),
        ("Album", tags.album.clone()),
        ("Genre", tags.genre.clone()),
        ("Year", tags.year.map(|year| year.to_string())),
        ("Comment", tags.comment.clone()),
    ]
    .into_iter()
    .filter_map(|(field, value)| {
        value
            .filter(|value| !value.trim().is_empty())
            .map(|value| (field, value))
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
