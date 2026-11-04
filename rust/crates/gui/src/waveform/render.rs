//! Preview element composition and pane-local paint orchestration.
//! The retained view owns artifacts and gestures, while these helpers compose an immutable frame snapshot.

use std::rc::Rc;

use gpui_kit::{
    App, Bounds, ContentMask, Context, DispatchPhase, Div, Hsla, InteractiveElement as _, IntoElement, MouseButton,
    MouseMoveEvent, ParentElement as _, Pixels, Render, StatefulInteractiveElement as _, Styled as _, Window, canvas,
    div, rems,
};

use super::geometry::{PreviewGeometry, Viewport, has_area};
use super::{
    PAUSED_PLAYHEAD_ALPHA, PENDING_PLAYHEAD_ALPHA, PLAYING_PLAYHEAD_ALPHA, PLOT_INSET, PaintData, PlayheadSource,
    SOURCE_OVERS_TOOLTIP, WaveformView, axes, loudness, overloads, paint_channels, paint_cues, paint_placeholder,
    paint_position, spectrogram,
};
use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::settings::PreviewViewMode;
use crate::theme::{color, palette, tooltip, white};

impl WaveformView {
    /// Captures source presentation and the independently advancing playback cursor for one frame.
    fn paint_data(&mut self, window: &Window) -> PaintData {
        if let Some(counter) = &mut self.frame_counter {
            counter.tick();
        }
        let playing = self.playhead.as_ref().is_some_and(PlayheadSource::is_playing);
        let position = self.playhead.as_ref().map(PlayheadSource::position_seconds);
        if playing && self.duration_seconds() > 0.0 {
            window.request_animation_frame();
            if let Some(position) = position {
                self.follow_playhead(position);
            }
        }
        let playhead = position.and_then(|position| {
            if playing || self.pending_seek.is_some() {
                Some((position, PLAYING_PLAYHEAD_ALPHA))
            } else if position > 0.0 {
                Some((position, PAUSED_PLAYHEAD_ALPHA))
            } else {
                None
            }
        });
        PaintData {
            overview: self.overview.clone(),
            spectrogram: self.spectrogram.clone(),
            spectrogram_completed: self.spectrogram_completed,
            view_mode: self.view_mode,
            source_rate: self.source_rate,
            source_channels: self.source_channels,
            source_duration: self.source_duration,
            placeholder: self.placeholder.clone(),
            overload_settings: self.overload_settings,
            pending_seek: self.pending_seek.map(|pending| pending.seconds),
            overloads: self.overloads.history().cloned(),
            serato_overview: self.serato_overview.clone().filter(|_| self.serato_colours),
            loudness: self.loudness.clone(),
            show_loudness: self.show_loudness && self.source_duration > 0.0,
            loudness_range: self.loudness_range,
            cues: self.cues.clone(),
            visible_start: self.visible_start,
            visible_length: self.visible_length(),
            display_gain: self.display_gain,
            playhead,
        }
    }

    /// Resolves canvas geometry each frame and keeps an active pointer gesture live beyond the panel hitbox.
    fn render_content(&self, data: PaintData, cx: &Context<Self>) -> Div {
        let geometry_cell = Rc::clone(&self.geometry);
        let mode = data.view_mode;
        let show_loudness = data.show_loudness;
        let dragging = self.pending_seek.is_some();
        let entity = cx.weak_entity();
        let body = canvas(
            move |bounds, window, _| {
                geometry_cell.set(PreviewGeometry::new(bounds, mode, show_loudness, window.rem_size()));
            },
            move |bounds, (), window, cx| {
                paint_preview(&data, bounds, window, cx);
                if dragging {
                    window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
                        if phase == DispatchPhase::Capture {
                            let _ = entity.update(cx, |view, cx| view.on_mouse_move(event, window, cx));
                        }
                    });
                }
            },
        )
        .size_full();
        let mut content = div().relative().size_full().child(body);
        if let Some(help) = self.render_warning_help() {
            content = content.child(help);
        }
        content
    }

    /// Attaches source-over help only to the waveform half, never spectral pixels or the frequency gutter.
    fn render_warning_help(&self) -> Option<Div> {
        if self.view_mode == PreviewViewMode::Spectrogram
            || !self.overloads.has_visible_overloads(self.overload_settings)
        {
            return None;
        }
        let waveform_help = div()
            .id("waveform-source-warning-help")
            .flex_1()
            .min_w_0()
            .min_h_0()
            .tooltip(tooltip(SOURCE_OVERS_TOOLTIP));
        let help = div().absolute().top_0().left_0().size_full().flex();
        Some(match self.view_mode {
            PreviewViewMode::SideBySide => help
                .gap(rems(2.0 / 15.0))
                .child(waveform_help)
                .child(div().flex_1().min_w_0()),
            PreviewViewMode::Stacked => help
                .flex_col()
                .gap(rems(2.0 / 15.0))
                .child(waveform_help)
                .child(div().flex_1().min_h_0()),
            PreviewViewMode::Waveform | PreviewViewMode::Spectrogram => help.child(waveform_help),
        })
    }
}

impl Render for WaveformView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let focus_handle = self
            .focus_handle
            .get_or_insert_with(|| cx.focus_handle().tab_stop(true))
            .clone();
        let data = self.paint_data(window);
        let content = self.render_content(data, cx);
        div()
            .id("waveform")
            .track_focus(&focus_handle)
            .focus_visible(|style| style.border_color(color(palette::BLUE)))
            .size_full()
            .p(rems(PLOT_INSET / 15.0))
            .rounded(rems(CORNER_RADIUS / 15.0))
            .bg(color(palette::GREY_MEDIUM_DARK))
            .border_1()
            .border_color(white(OUTLINE_ALPHA))
            .overflow_hidden()
            .child(content)
            .on_mouse_down(
                MouseButton::Right,
                cx.listener(|view, _, _, cx| view.cancel_pending_seek(cx)),
            )
            .on_mouse_down(MouseButton::Left, cx.listener(Self::on_mouse_down))
            .on_mouse_up(MouseButton::Left, cx.listener(Self::on_mouse_up))
            .on_mouse_up_out(MouseButton::Left, cx.listener(Self::on_mouse_up))
            .on_scroll(cx.listener(Self::on_scroll))
    }
}

/// Paints each source representation with the shared time range and its own axes and overlays.
fn paint_preview(data: &PaintData, bounds: Bounds<Pixels>, window: &mut Window, cx: &mut App) {
    if data.source_duration <= 0.0 {
        paint_placeholder(data.placeholder.clone(), bounds, window, cx);
        return;
    }
    let geometry = PreviewGeometry::new(bounds, data.view_mode, data.show_loudness, window.rem_size());
    paint_waveform_pane(data, geometry, window, cx);
    paint_spectrogram_pane(data, geometry, window, cx);
    paint_warning_strip(data, geometry, window);
    paint_loudness_overlay(data, geometry, window, cx);
    for plot in [geometry.waveform, geometry.spectrogram]
        .into_iter()
        .filter(|plot| has_area(*plot))
    {
        paint_pane_markers(data, plot, window);
    }
}

/// Paints decoded channel envelopes and source warnings before amplitude labels.
fn paint_waveform_pane(data: &PaintData, geometry: PreviewGeometry, window: &mut Window, cx: &mut App) {
    if !has_area(geometry.waveform) {
        return;
    }
    let viewport = viewport(data, geometry.waveform);
    window.with_content_mask(
        Some(ContentMask {
            bounds: geometry.waveform,
        }),
        |window| {
            if data.overview.is_some() {
                paint_channels(data, &viewport, window);
            } else {
                paint_placeholder(data.placeholder.clone(), geometry.waveform, window, cx);
            }
            overloads::paint(data, &viewport, window);
        },
    );
    axes::paint_amplitude_axis(
        geometry,
        data.source_channels.clamp(1, 2),
        data.show_loudness,
        window,
        cx,
    );
}

/// Paints the retained source raster or its real availability state, followed by the independent frequency axis.
fn paint_spectrogram_pane(data: &PaintData, geometry: PreviewGeometry, window: &mut Window, cx: &mut App) {
    if !has_area(geometry.spectrogram) {
        return;
    }
    let viewport = viewport(data, geometry.spectrogram);
    window.with_content_mask(
        Some(ContentMask {
            bounds: geometry.spectrogram,
        }),
        |window| {
            spectrogram::paint_background(geometry.spectrogram, window);
            if let Some(raster) = &data.spectrogram {
                raster.paint(viewport, window);
            } else {
                let text = if data.spectrogram_completed {
                    "Spectrogram unavailable"
                } else {
                    "Computing spectrogram…"
                };
                paint_placeholder(text.into(), geometry.spectrogram, window, cx);
            }
        },
    );
    spectrogram::paint_frequency_axis(
        geometry.spectrogram,
        geometry.frequency_axis,
        data.source_rate,
        window,
        cx,
    );
}

/// Keeps spectral-only source warnings outside all frequency rows.
fn paint_warning_strip(data: &PaintData, geometry: PreviewGeometry, window: &mut Window) {
    if has_area(geometry.warning) {
        let viewport = viewport(data, geometry.warning);
        window.with_content_mask(
            Some(ContentMask {
                bounds: geometry.warning,
            }),
            |window| {
                overloads::paint_warning_strip(data, &viewport, window);
            },
        );
    }
}

/// Hosts source loudness on the waveform when visible, otherwise on the spectral pane.
fn paint_loudness_overlay(data: &PaintData, geometry: PreviewGeometry, window: &mut Window, cx: &mut App) {
    if !has_area(geometry.loudness) {
        return;
    }
    loudness::paint_axis(
        geometry.loudness_axis,
        geometry.loudness,
        data.loudness_range,
        window,
        cx,
    );
    if let Some(history) = data.loudness.as_deref() {
        let viewport = viewport(data, geometry.loudness);
        window.with_content_mask(
            Some(ContentMask {
                bounds: geometry.loudness,
            }),
            |window| {
                loudness::paint_curve(history, &viewport, data.loudness_range, window);
            },
        );
    }
}

/// Mirrors cues and the grey pending target behind the independently advancing white cursor in each pane.
fn paint_pane_markers(data: &PaintData, plot: Bounds<Pixels>, window: &mut Window) {
    let viewport = viewport(data, plot);
    window.with_content_mask(Some(ContentMask { bounds: plot }), |window| {
        paint_cues(&data.cues, data.source_duration, &viewport, window);
        if let Some(seconds) = data.pending_seek {
            let pending_color = Hsla {
                a: PENDING_PLAYHEAD_ALPHA,
                ..color(palette::GREY_MIDDLE_LIGHT)
            };
            paint_position(seconds, pending_color, viewport, window);
        }
        if let Some((seconds, alpha)) = data.playhead {
            paint_position(seconds, white(alpha), viewport, window);
        }
    });
}

/// Projects every pane through the same immutable frame interval.
const fn viewport(data: &PaintData, plot: Bounds<Pixels>) -> Viewport {
    Viewport {
        start: data.visible_start,
        length: data.visible_length,
        plot,
    }
}
