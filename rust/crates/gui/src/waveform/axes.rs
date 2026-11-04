//! Relative-scale axis typography and collision-filtered linear waveform magnitude references.

use gpui_kit::{
    App, Bounds, ContentMask, Hsla, Pixels, SharedString, TextAlign, TextRun, Window, fill, font, point, px, size,
};

use super::geometry::{PreviewGeometry, amplitude_labels, relative};
use crate::theme::{TEXT_FONT, white};

/// Paints a shaped source-axis caption inside its reserved label lane.
pub(super) fn paint_label(
    text: SharedString,
    bounds: Bounds<Pixels>,
    left_align: bool,
    color: Hsla,
    window: &mut Window,
    cx: &mut App,
) {
    let run = TextRun {
        len: text.len(),
        font: font(TEXT_FONT),
        color,
        background_color: None,
        underline: None,
        strikethrough: None,
    };
    let line = window
        .text_system()
        .shape_line(text, px(relative(10.0, window.rem_size())), &[run], None);
    let x = if left_align {
        bounds.origin.x
    } else {
        bounds.right() - line.width()
    };
    if let Err(error) = line.paint(
        point(x, bounds.origin.y),
        bounds.size.height,
        TextAlign::Left,
        None,
        window,
        cx,
    ) {
        tracing::warn!("Failed to paint preview axis label: {error:#}");
    }
}

/// Labels both signs of each native amplitude lane, without adding a grid over the loudness grid.
pub(super) fn paint_amplitude_axis(
    geometry: PreviewGeometry,
    channels: u32,
    loudness: bool,
    window: &mut Window,
    cx: &mut App,
) {
    for label in amplitude_labels(geometry, channels, window.rem_size()) {
        let text = format!("{:.0}", label.decibels);
        paint_label(
            text.into(),
            label.bounds,
            geometry.amplitude_axis.origin.x >= geometry.waveform.right(),
            white(0.8),
            window,
            cx,
        );
        if !loudness {
            window.with_content_mask(
                Some(ContentMask {
                    bounds: geometry.waveform,
                }),
                |window| {
                    window.paint_quad(fill(
                        Bounds::new(
                            point(geometry.waveform.origin.x, px(label.y.round())),
                            size(geometry.waveform.size.width, px(1.0)),
                        ),
                        white(0.12),
                    ));
                },
            );
        }
    }
}
