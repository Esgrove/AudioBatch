//! The information panel of the selected file, beside the waveform.
//!
//! It shows the file name split into artist and title at the first " - ", beside the embedded album artwork,
//! actionable error details when analysis fails,
//! four metric cards for the levels, and a scrollable two-row, five-column grid of technical and musical details.
//! The first column aligns left and the other four align right, with field names in tooltips.
//! Genre truncates, while Album and Comment wrap without a line limit.
//! The wide variant puts the metric cards in one row and keeps the same stacked details.
//! The compact variant for the collapsed preview shows only the four metric cards, in one row.

use std::sync::Arc;

use audiobatch_core::format::{
    camelot_key, format_bitrate, format_clip_counts, format_dc_offsets, format_duration_tenths, format_file_size,
    format_loudness_compact, format_metric, format_peak_compact, format_sample_rate, format_true_peak_compact,
    format_type_label, gain_to_decibels,
};
use audiobatch_core::gain_loudness::{GainLoudness, estimate_limiter_integrated};
use audiobatch_core::record::is_silent_loudness;
use audiobatch_core::signal_metrics::SignalMetrics;
use audiobatch_core::{AnalysisRecord, FileTags};
use gpui_kit::{
    AnyElement, Div, InteractiveElement as _, IntoElement, ObjectFit, ParentElement as _, RenderImage, Stateful,
    StatefulInteractiveElement as _, Styled as _, StyledImage as _, TextAlign, TextRun, canvas, div, font, img, point,
    px, rems,
};

use crate::results::{ResultRow, RowState};
use crate::theme::{MONO_FONT, TEXT_FONT, color, palette, tooltip, white};

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

/// Detail line height in rems, preserving the panel's compact rhythm at the default theme size.
const DETAIL_ROW_HEIGHT: f32 = 4.0 / 3.0;

/// Width of the tag caption column in rems.
const DETAIL_FIELD_WIDTH: f32 = 64.0 / 15.0;

/// Detail value font size in rems, keeping its ratio to the line height when the interface zooms.
const DETAIL_VALUE_FONT_SIZE: f32 = 11.0 / 15.0;

/// Tag caption font size in rems.
const DETAIL_CAPTION_FONT_SIZE: f32 = 10.25 / 15.0;

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
    /// Unambiguous unit-only caption used when the full labels cannot fit.
    compact_unit: &'static str,
    /// Formatted value, or a dash when there is no result.
    value: String,
    /// Predicted level after custom gain, with approximate loudness values marked by "≈".
    adjusted_value: Option<String>,
}

/// Pure presentation of the selected file's details, independent of the window and panel width.
#[derive(Debug, Default, PartialEq, Eq)]
struct InfoDetails {
    /// Container, channel layout, duration, and decimal file size.
    format: Vec<InfoPart>,
    /// Source bit depth, sample rate, and average bitrate.
    audio: Vec<InfoPart>,
    /// Tempo, original key with Camelot mapping, and release year.
    track: Vec<InfoPart>,
    /// Single-line genre, when present.
    genre: Option<String>,
    /// Album title, displayed without a line limit.
    album: Option<String>,
    /// Comment, displayed without a line limit.
    comment: Option<String>,
}

/// One available value of a detail line, with its field name retained for the tooltip.
#[derive(Debug, PartialEq, Eq)]
struct InfoPart {
    field: &'static str,
    value: String,
}

/// Renders the panel for the selected row and its tags, or the empty state when nothing is selected.
/// The wide panel puts the four metric cards in one row and preserves the stacked details.
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
    let formatted = info_details(ready, tags);
    details = details.child(details_grid(&formatted));
    if let Some(record) = ready {
        for (field, value) in source_lines(&record.signal_metrics) {
            details = details.child(tag_block(field, value, true));
        }
    }
    for (field, value, wrap) in [
        ("Genre", formatted.genre, false),
        ("Album", formatted.album, true),
        ("Comment", formatted.comment, true),
    ] {
        if let Some(value) = value {
            details = details.child(tag_block(field, value, wrap));
        }
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
                .child(metric_labels(&metric, true))
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
            compact_unit: "dBFS",
            value: value(|record| format_peak_compact(f64::from(record.overall_peak))),
            adjusted_value: adjusted_record
                .and_then(|record| adjusted_peak(f64::from(record.overall_peak), record.custom_gain_db)),
        },
        Metric {
            name: "True peak",
            caption: "True peak",
            unit: "dBTP",
            compact_unit: "dBTP",
            value: value(|record| format_true_peak_compact(record.overall_true_peak)),
            adjusted_value: adjusted_record
                .and_then(|record| adjusted_peak(record.overall_true_peak, record.custom_gain_db)),
        },
        Metric {
            name: "Max short-term",
            caption: "Short-term",
            unit: "LUFS",
            compact_unit: "LUFS-S",
            value: value(|record| format_loudness_compact(record.max_short_term_lufs)),
            adjusted_value: adjusted_record.and_then(|record| {
                adjusted_loudness(gained_loudness.map(GainLoudness::max_short_term), record.custom_gain_db)
            }),
        },
        Metric {
            name: "Integrated loudness",
            caption: "Integrated",
            unit: "LUFS",
            compact_unit: "LUFS-I",
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

/// Renders a metric card, replacing labels with an unambiguous unit when they no longer fit.
fn metric_card(metric: Metric) -> Stateful<Div> {
    metric_card_frame(&metric)
        .h(px(METRIC_HEIGHT))
        .flex_col()
        .justify_between()
        .p(px(6.0))
        .child(metric_labels(&metric, false))
        .child(metric_value(metric.value, metric.adjusted_value))
}

/// Measures captions against their resolved lane, so resizing never leaves a partially truncated metric name.
/// The card owns the accessible full description through its tooltip, independent of the painted abbreviation.
fn metric_labels(metric: &Metric, stacked: bool) -> impl IntoElement + use<> {
    let labels = [metric.caption, metric.unit, metric.compact_unit];
    canvas(
        move |_, window, _| {
            labels.map(|text| {
                window.text_system().shape_line(
                    text.into(),
                    rems(CAPTION_FONT_SIZE / 15.0).to_pixels(window.rem_size()),
                    &[TextRun {
                        len: text.len(),
                        font: font(TEXT_FONT),
                        color: color(palette::GREY_MIDDLE_LIGHT),
                        background_color: None,
                        underline: None,
                        strikethrough: None,
                    }],
                    None,
                )
            })
        },
        move |bounds, [caption, unit, compact_unit], window, cx| {
            let line_height = rems(METRIC_CAPTION_LINE_HEIGHT / 15.0).to_pixels(window.rem_size());
            let gap = rems(4.0 / 15.0).to_pixels(window.rem_size());
            let required = if stacked {
                caption.width().max(unit.width())
            } else {
                caption.width() + gap + unit.width()
            };
            let full = metric_labels_fit(f32::from(bounds.size.width), f32::from(required));
            let labels = if full {
                let unit_position = if stacked {
                    point(bounds.origin.x, bounds.origin.y + line_height)
                } else {
                    point(bounds.right() - unit.width(), bounds.origin.y)
                };
                vec![(caption, bounds.origin), (unit, unit_position)]
            } else {
                let origin = if stacked {
                    point(bounds.origin.x, bounds.origin.y + line_height * 0.5)
                } else {
                    bounds.origin
                };
                vec![(compact_unit, origin)]
            };
            for (label, origin) in labels {
                if let Err(error) = label.paint(origin, line_height, TextAlign::Left, None, window, cx) {
                    tracing::warn!("Failed to paint metric label: {error:#}");
                }
            }
        },
    )
    .w_full()
    .min_w_0()
    .h(rems(
        METRIC_CAPTION_LINE_HEIGHT / 15.0 * if stacked { 2.0 } else { 1.0 },
    ))
    .flex_1()
    .overflow_hidden()
}

/// Keeps the full labels only when their measured glyphs and inter-label gap fit completely.
fn metric_labels_fit(available: f32, required: f32) -> bool {
    available >= required
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

/// Places properties in stable row-major slots, preserving empty cells for missing values.
fn info_grid(details: &InfoDetails) -> [(&'static str, &str); 10] {
    const FIELDS: [&str; 10] = [
        "Bit depth",
        "Sample rate",
        "Bitrate",
        "Duration",
        "Size",
        "Format",
        "Channels",
        "BPM",
        "Key",
        "Year",
    ];
    FIELDS.map(|field| {
        let value = details
            .format
            .iter()
            .chain(&details.audio)
            .chain(&details.track)
            .find(|part| part.field == field)
            .map_or("", |part| part.value.as_str());
        (field, value)
    })
}

/// Renders two shared five-column rows, with the first column leading and the other four trailing.
/// Each cell truncates independently and keeps its complete labelled value in a tooltip.
fn details_grid(details: &InfoDetails) -> Div {
    let cells = info_grid(details);
    let mut grid = div().flex().flex_col().flex_none().min_w_0();
    for row in cells.as_chunks::<5>().0 {
        if row.iter().all(|(_, value)| value.is_empty()) {
            continue;
        }
        let mut line = div()
            .flex()
            .flex_row()
            .flex_none()
            .min_w_0()
            .h(rems(DETAIL_ROW_HEIGHT))
            .items_center()
            .gap(rems(4.0 / 15.0))
            .text_size(rems(DETAIL_VALUE_FONT_SIZE))
            .text_color(color(palette::GREY_LIGHT));
        for (column, (field, value)) in row.iter().enumerate() {
            let content = div().min_w_0().truncate().child(value.replace(['\r', '\n'], " "));
            let content = if column == 1 {
                // Reserve a wider trailing gap without moving the remaining columns.
                content.mr_3()
            } else {
                content
            };
            let mut cell = div().id(*field).flex_1().min_w_0().overflow_hidden().child(content);
            if column > 0 {
                cell = cell.text_right();
            }
            if !value.is_empty() {
                cell = cell.tooltip(tooltip(format!("{field}: {value}")));
            }
            line = line.child(cell);
        }
        grid = grid.child(line);
    }
    grid
}

/// Renders a caption and left-aligned value, wrapping Album and Comment without a line limit.
fn tag_block(field: &'static str, value: String, wrap: bool) -> Stateful<Div> {
    let description = format!("{field}: {value}");
    let value = if wrap { value } else { value.replace(['\r', '\n'], " ") };
    let value = div()
        .flex_1()
        .min_w_0()
        .text_size(rems(DETAIL_VALUE_FONT_SIZE))
        .text_color(color(palette::GREY_LIGHT))
        .child(value);
    let value = if wrap {
        value.whitespace_normal()
    } else {
        value.truncate()
    };
    div()
        .id(field)
        .tooltip(tooltip(description))
        .min_h(rems(DETAIL_ROW_HEIGHT))
        .line_height(rems(DETAIL_ROW_HEIGHT))
        .flex_none()
        .flex()
        .flex_row()
        .items_start()
        .child(
            div()
                .w(rems(DETAIL_FIELD_WIDTH))
                .flex_none()
                .text_size(rems(DETAIL_CAPTION_FONT_SIZE))
                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                .child(field),
        )
        .child(value)
}

/// Builds the labelled source-statistics lines of a ready record, with a dash for each undefined value.
fn source_lines(metrics: &SignalMetrics) -> [(&'static str, String); 4] {
    [
        (
            "Source",
            format!(
                "PLR {} dB · PSR {} dB · Crest {} dB",
                format_metric(metrics.plr_db, false),
                format_metric(metrics.psr_db, false),
                format_metric(metrics.crest_factor_db, false)
            ),
        ),
        ("DC", format_dc_offsets(metrics)),
        (
            "Clips",
            format!("{} total, {}", metrics.total_clips(), format_clip_counts(metrics)),
        ),
        ("Corr", format_metric(metrics.stereo_correlation, true)),
    ]
}

/// Builds detail lines from ready technical data and independently available tags.
/// Missing values and their separators are omitted, while tags remain visible during pending or failed analysis.
fn info_details(record: Option<&AnalysisRecord>, tags: Option<&FileTags>) -> InfoDetails {
    let mut details = InfoDetails::default();
    if let Some(record) = record.filter(|record| record.is_ready()) {
        let channels = match record.channels {
            0 => String::new(),
            1 => String::from("Mono"),
            2 => String::from("Stereo"),
            count => format!("{count} ch"),
        };
        let format = format_type_label(record);
        details.format = info_parts([
            ("Format", if format == "Unknown" { String::new() } else { format }),
            ("Channels", channels),
            ("Duration", format_duration_tenths(record.duration_seconds)),
            ("Size", format_file_size(record)),
        ])
        .into_iter()
        .filter(|part| part.value != "-")
        .collect();
        details.audio = info_parts([
            (
                "Bit depth",
                if record.bits_per_sample == 0 {
                    String::new()
                } else {
                    format!("{}-bit", record.bits_per_sample)
                },
            ),
            ("Sample rate", format_sample_rate(record)),
            ("Bitrate", format_bitrate(record)),
        ])
        .into_iter()
        .filter(|part| part.value != "-")
        .collect();
    }
    if let Some(tags) = tags {
        let valid_bpm = |bpm: &f64| bpm.is_finite() && *bpm > 0.0;
        let serato = tags.serato.as_ref();
        let bpm = serato
            .and_then(|data| data.autotags.as_ref())
            .map(|autotags| autotags.bpm)
            .filter(valid_bpm)
            .or_else(|| tags.bpm.as_deref()?.trim().parse::<f64>().ok().filter(valid_bpm))
            .or_else(|| serato?.beatgrid.as_ref()?.bpm().map(f64::from).filter(valid_bpm));
        details.track = info_parts([
            ("BPM", bpm.map_or_else(String::new, |bpm| format!("{bpm:.2} BPM"))),
            ("Key", tags.key.as_deref().map_or_else(String::new, display_key)),
            ("Year", tags.year.map_or_else(String::new, |year| year.to_string())),
        ]);
        let present = |value: &Option<String>| value.as_ref().filter(|value| !value.trim().is_empty()).cloned();
        details.genre = present(&tags.genre);
        details.album = present(&tags.album);
        details.comment = present(&tags.comment);
    }
    details
}

/// Keeps populated values in their original order, preserving literal source tag text.
fn info_parts<const COUNT: usize>(parts: [(&'static str, String); COUNT]) -> Vec<InfoPart> {
    parts
        .into_iter()
        .filter(|(_, value)| !value.is_empty())
        .map(|(field, value)| InfoPart { field, value })
        .collect()
}

/// Appends the Camelot mapping without respelling the trimmed source or duplicating an existing Camelot code.
fn display_key(key: &str) -> String {
    let key = key.trim();
    let Some(mapped) = camelot_key(key) else {
        return key.to_string();
    };
    let already_camelot =
        key.starts_with(|character: char| character.is_ascii_digit()) && key.ends_with(['a', 'A', 'b', 'B']);
    if already_camelot {
        key.to_string()
    } else {
        format!("{key} / {mapped}")
    }
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
    fn narrow_metric_labels_use_distinct_units_and_switch_at_the_measured_width() {
        assert_eq!(
            metrics(None, false, None).map(|metric| metric.compact_unit),
            ["dBFS", "dBTP", "LUFS-S", "LUFS-I"]
        );
        assert!(!metric_labels_fit(99.0, 100.0));
        assert!(metric_labels_fit(100.0, 100.0));
        assert!(metric_labels_fit(101.0, 100.0));
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
mod test_info_details {
    use audiobatch_core::AnalysisStatus;
    use audiobatch_core::serato_tags::{AutoTags, BeatGrid, BeatGridMarker, SeratoData};

    use super::*;

    /// Returns a complete source record with predictable decimal size and average bitrate.
    fn record() -> AnalysisRecord {
        AnalysisRecord {
            path: "track.flac".into(),
            status: AnalysisStatus::Analyzed,
            channels: 2,
            duration_seconds: 205.4,
            file_size: 36_200_000,
            bits_per_sample: 24,
            sample_rate: 44_100,
            ..AnalysisRecord::default()
        }
    }

    /// Joins the production model's values in their display order for readable presentation assertions.
    fn line(parts: &[InfoPart]) -> String {
        parts
            .iter()
            .map(|part| part.value.as_str())
            .collect::<Vec<_>>()
            .join(" · ")
    }

    /// Supplies all three tempo sources to test their independent priority and validity rules.
    fn tempo_tags(autotags: f64, tag: &str, grid: f32) -> FileTags {
        FileTags {
            bpm: Some(tag.to_string()),
            serato: Some(SeratoData {
                autotags: Some(AutoTags {
                    bpm: autotags,
                    ..Default::default()
                }),
                beatgrid: Some(BeatGrid {
                    marker_count: 1,
                    markers: vec![BeatGridMarker::Terminal {
                        position: 0.0,
                        bpm: grid,
                    }],
                }),
                ..Default::default()
            }),
            ..Default::default()
        }
    }

    #[test]
    fn technical_lines_have_the_expected_order_units_and_tooltip_fields() {
        let details = info_details(Some(&record()), None);
        assert_eq!(line(&details.format), "FLAC · Stereo · 3:25.4 · 36.2 MB");
        assert_eq!(line(&details.audio), "24-bit · 44.1 kHz · 1410 kbps");
        assert_eq!(
            details.format.iter().map(|part| part.field).collect::<Vec<_>>(),
            ["Format", "Channels", "Duration", "Size"]
        );
        assert_eq!(details.track, []);
        assert!(
            !details
                .format
                .iter()
                .chain(&details.audio)
                .any(|part| part.field == "Samples")
        );
    }

    #[test]
    fn source_lines_label_ratios_channels_and_undefined_correlation() {
        let mut metrics = SignalMetrics::default();
        metrics.plr_db = Some(9.5);
        metrics.crest_factor_db = Some(3.01);
        metrics.dc_offsets = vec![0.001, -0.002];
        metrics.clip_counts = vec![2, 5];
        assert_eq!(
            source_lines(&metrics),
            [
                ("Source", String::from("PLR 9.50 dB · PSR - dB · Crest 3.01 dB")),
                ("DC", String::from("Ch 1: +0.1000%, Ch 2: -0.2000%")),
                ("Clips", String::from("7 total, Ch 1: 2, Ch 2: 5")),
                ("Corr", String::from("-")),
            ]
        );
    }

    #[test]
    fn grid_columns_share_stable_slots_across_both_rows() {
        let tags = FileTags {
            bpm: Some("128".into()),
            key: Some("Gm".into()),
            year: Some(2019),
            ..Default::default()
        };
        let details = info_details(Some(&record()), Some(&tags));
        assert_eq!(
            info_grid(&details),
            [
                ("Bit depth", "24-bit"),
                ("Sample rate", "44.1 kHz"),
                ("Bitrate", "1410 kbps"),
                ("Duration", "3:25.4"),
                ("Size", "36.2 MB"),
                ("Format", "FLAC"),
                ("Channels", "Stereo"),
                ("BPM", "128.00 BPM"),
                ("Key", "Gm / 6A"),
                ("Year", "2019"),
            ]
        );
        let tags_only = info_details(None, Some(&tags));
        let cells = info_grid(&tags_only);
        assert!(cells[..7].iter().all(|(_, value)| value.is_empty()));
        assert_eq!(&cells[7..], &info_grid(&details)[7..]);
        assert!(
            info_grid(&InfoDetails::default())
                .iter()
                .all(|(_, value)| value.is_empty())
        );
    }

    #[test]
    fn unknown_properties_are_omitted_without_placeholder_separators() {
        let mut record = record();
        record.bits_per_sample = 0;
        record.sample_rate = 0;
        record.duration_seconds = 0.0;
        record.file_size = 0;
        record.channels = 0;
        let details = info_details(Some(&record), None);
        assert_eq!(line(&details.format), "FLAC");
        assert_eq!(details.audio, []);
        record.path = "track.mp3".into();
        assert_eq!(info_details(Some(&record), None).audio, []);
        record.path = "no_extension".into();
        assert_eq!(info_details(Some(&record), None), InfoDetails::default());
    }

    #[test]
    fn channels_use_mono_stereo_or_a_count_with_a_unit() {
        let mut record = record();
        for (channels, expected) in [(1, "Mono"), (2, "Stereo"), (6, "6 ch")] {
            record.channels = channels;
            assert_eq!(info_details(Some(&record), None).format[1].value, expected);
        }
    }

    #[test]
    fn track_line_preserves_original_key_spelling_and_tag_order() {
        let tags = FileTags {
            bpm: Some(String::from("128")),
            key: Some(String::from(" Gm ")),
            year: Some(2019),
            ..Default::default()
        };
        let details = info_details(None, Some(&tags));
        assert_eq!(line(&details.track), "128.00 BPM · Gm / 6A · 2019");
        assert_eq!(
            details.track.iter().map(|part| part.field).collect::<Vec<_>>(),
            ["BPM", "Key", "Year"]
        );
        for (key, expected) in [
            (" F#m ", "F#m / 11A"),
            ("Abmin", "Abmin / 1A"),
            (" 08A ", "08A"),
            ("8a", "8a"),
            ("012A", "012A"),
            ("8 a", "8 a"),
            ("-", "-"),
            ("1d", "1d / 8B"),
            ("unknown key", "unknown key"),
        ] {
            assert_eq!(display_key(key), expected);
        }
    }

    #[test]
    fn tempo_priority_is_autotags_then_standard_tag_then_beatgrid() {
        for (autotags, tag, grid, expected) in [
            (130.0, "128", 125.0, "130.00 BPM"),
            (0.0, "128", 125.0, "128.00 BPM"),
            (f64::NAN, "128", 125.0, "128.00 BPM"),
            (0.0, "invalid", 125.0, "125.00 BPM"),
            (0.0, "NaN", 125.0, "125.00 BPM"),
            (f64::INFINITY, "-2", 125.0, "125.00 BPM"),
            (0.0, "", f32::INFINITY, ""),
        ] {
            let details = info_details(None, Some(&tempo_tags(autotags, tag, grid)));
            assert_eq!(line(&details.track), expected);
        }
    }

    #[test]
    fn tags_remain_available_for_pending_and_failed_analysis() {
        let tags = FileTags {
            key: Some(String::from("Gm")),
            genre: Some(String::from("Deep House")),
            album: Some(String::from("An album title long enough to wrap across multiple lines")),
            comment: Some("A long comment. ".repeat(100)),
            ..Default::default()
        };
        let mut record = record();
        for status in [AnalysisStatus::Pending, AnalysisStatus::Failed] {
            record.status = status;
            let details = info_details(Some(&record), Some(&tags));
            assert_eq!(details.format, []);
            assert_eq!(details.audio, []);
            assert_eq!(line(&details.track), "Gm / 6A");
            assert_eq!(details.genre, tags.genre);
            assert_eq!(details.album, tags.album);
            assert_eq!(details.comment, tags.comment);
        }
        assert_eq!(
            info_details(None, Some(&tags)),
            info_details(Some(&record), Some(&tags))
        );
    }

    #[test]
    fn absent_and_blank_metadata_produce_no_empty_lines_or_tag_blocks() {
        let tags = FileTags {
            key: Some(String::from("  ")),
            album: Some(String::from("\t")),
            genre: Some(String::new()),
            comment: Some(String::from("\n")),
            ..Default::default()
        };
        assert_eq!(info_details(None, Some(&tags)), InfoDetails::default());
        assert_eq!(info_details(None, None), InfoDetails::default());
    }
}
