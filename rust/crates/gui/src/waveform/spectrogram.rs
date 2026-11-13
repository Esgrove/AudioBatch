//! Retained bounded source-power raster and its exact source-frame footprint.
//! Raster construction happens only on artifact adoption, never during painting or playback animation.

use std::sync::{Arc, OnceLock};

use audiobatch_core::spectrogram::{FREQUENCY_ROWS, SpectrogramData, frequency_to_mel};
use gpui_kit::{App, Bounds, Corners, Pixels, RenderImage, SharedString, Window, fill, point, px, rgb, size};
use image::{Frame, RgbaImage};

use super::axes::paint_label;
use super::geometry::{Viewport, has_area, relative};
use crate::theme::{color, white};

/// Audited source-intensity raster colors, independent of mutable interface themes.
const PALETTE_ANCHORS: [[u8; 3]; 9] = [
    [0x08, 0x0b, 0x16],
    [0x11, 0x1f, 0x55],
    [0x28, 0x3c, 0x96],
    [0x60, 0x44, 0xad],
    [0xa4, 0x51, 0x83],
    [0xde, 0x70, 0x4e],
    [0xf7, 0xaa, 0x45],
    [0xff, 0xdd, 0x83],
    [0xff, 0xf5, 0xd7],
];
/// Fixed frequency captions, independent of source sample rate.
const FREQUENCIES: [(f64, &str); 10] = [
    (0.0, "0"),
    (250.0, "250"),
    (500.0, "500"),
    (1000.0, "1k"),
    (2000.0, "2k"),
    (4000.0, "4k"),
    (8000.0, "8k"),
    (16_000.0, "16k"),
    (18_000.0, "18k"),
    (20_000.0, "20k"),
];
/// Immutable 256-entry BGRA lookup shared by every source artifact.
static PALETTE: OnceLock<[[u8; 4]; 256]> = OnceLock::new();

/// One artifact and its single full-grid raster, reused across pane changes, zoom, and animation.
#[derive(Clone)]
pub(super) struct SpectrogramRaster {
    source: Arc<SpectrogramData>,
    image: Arc<RenderImage>,
}

/// Distinct true-source clipping and padded-grid image footprints for the same raster projection.
struct RasterBounds {
    clip: Bounds<Pixels>,
    image: Bounds<Pixels>,
}

impl SpectrogramRaster {
    /// Converts low-frequency-first column-major intensities to top-down row-major BGRA storage.
    pub(super) fn new(source: Arc<SpectrogramData>) -> Self {
        let palette = PALETTE.get_or_init(make_palette);
        let width = source.column_count();
        let mut bytes = vec![0; width * FREQUENCY_ROWS * 4];
        for (column, intensities) in source.intensities().as_chunks::<FREQUENCY_ROWS>().0.iter().enumerate() {
            for (row, intensity) in intensities.iter().enumerate() {
                let offset = ((FREQUENCY_ROWS - 1 - row) * width + column) * 4;
                if let (Some(destination), Some(pixel)) =
                    (bytes.get_mut(offset..offset + 4), palette.get(usize::from(*intensity)))
                {
                    destination.copy_from_slice(pixel);
                }
            }
        }
        let buffer = RgbaImage::from_raw(width as u32, FREQUENCY_ROWS as u32, bytes)
            .expect("validated spectrogram has exact bounded raster storage");
        Self {
            source,
            image: Arc::new(RenderImage::new([Frame::new(buffer)])),
        }
    }

    /// Returns the immutable source metadata for adoption and projection checks.
    pub(super) const fn source(&self) -> &Arc<SpectrogramData> {
        &self.source
    }

    /// Releases ownership to the caller so GPUI can dispose its previously uploaded texture safely.
    pub(super) fn into_image(self) -> Arc<RenderImage> {
        self.image
    }

    /// Paints a cropped full grid without stretching the final partial column to source EOF.
    pub(super) fn paint(&self, viewport: Viewport, window: &mut Window) {
        let RasterBounds {
            clip,
            image: image_bounds,
        } = raster_bounds(&self.source, viewport);
        if let Err(error) = window.paint_image(
            clip,
            image_bounds,
            Corners::default(),
            Arc::clone(&self.image),
            0,
            false,
        ) {
            tracing::warn!("Failed to paint spectrogram: {error:#}");
        }
    }
}

/// Paints the calibrated silence color, even before source pixels become available.
pub(super) fn paint_background(plot: Bounds<Pixels>, window: &mut Window) {
    // The silence background is itself part of the audited raster palette.
    window.paint_quad(fill(plot, color(rgb(0x0008_0b16))));
}

/// Projects the padded grid separately from the true decoded source extent.
fn raster_bounds(source: &SpectrogramData, viewport: Viewport) -> RasterBounds {
    let rate = f64::from(source.sample_rate());
    let duration = source.length_in_frames() as f64 / rate;
    let grid_duration = source.samples_per_column() as f64 * source.column_count() as f64 / rate;
    let start = viewport.time_to_x(0.0);
    let finish = viewport.time_to_x(duration);
    let grid_finish = viewport.time_to_x(grid_duration);
    let source_bounds = Bounds::new(
        point(px(start), viewport.plot.origin.y),
        size(px(finish - start), viewport.plot.size.height),
    );
    let nyquist = rate * 0.5;
    let fraction = frequency_to_mel(nyquist) / frequency_to_mel(nyquist.max(FREQUENCIES[9].0));
    let height = px(f32::from(viewport.plot.size.height) * fraction as f32);
    let image_bounds = Bounds::new(
        point(px(start), viewport.plot.bottom() - height),
        size(px(grid_finish - start), height),
    );
    RasterBounds {
        clip: viewport.plot.intersect(&source_bounds),
        image: image_bounds,
    }
}

/// Draws fixed reference ticks across a Mel range extending to at least 20 kHz.
pub(super) fn paint_frequency_axis(
    plot: Bounds<Pixels>,
    gutter: Bounds<Pixels>,
    rate: u32,
    window: &mut Window,
    cx: &mut App,
) {
    if !has_area(plot) || !has_area(gutter) || rate == 0 {
        return;
    }
    let nyquist = f64::from(rate) * 0.5;
    let maximum_mel = frequency_to_mel(nyquist.max(FREQUENCIES[9].0));
    let height = relative(10.0, window.rem_size());
    let spacing = relative(16.0, window.rem_size());
    let mut occupied: Vec<(f32, f32)> = Vec::new();
    for (frequency, caption) in FREQUENCIES {
        let y = (frequency_to_mel(frequency) / maximum_mel) as f32;
        let y = y.mul_add(-f32::from(plot.size.height), f32::from(plot.bottom()));
        let top = height.mul_add(-0.5, y).clamp(
            f32::from(gutter.origin.y),
            (f32::from(gutter.bottom()) - height).max(f32::from(gutter.origin.y)),
        );
        if occupied.iter().any(|&(start, end)| top < end && top + spacing > start) {
            continue;
        }
        occupied.push((top, top + spacing));
        let label = Bounds::new(point(gutter.origin.x, px(top)), size(gutter.size.width, px(height)));
        paint_label(
            SharedString::new_static(caption),
            label,
            gutter.origin.x >= plot.right(),
            white(0.8),
            window,
            cx,
        );
        window.paint_quad(fill(
            Bounds::new(point(plot.origin.x, px(y.round())), size(plot.size.width, px(1.0))),
            white(0.1),
        ));
    }
}

/// Builds the native smoothstep lookup, including the integer 8-bit color tween arithmetic.
fn make_palette() -> [[u8; 4]; 256] {
    std::array::from_fn(|intensity| {
        let normalized = intensity as f32 / 255.0;
        let display = normalized * normalized * 2.0f32.mul_add(-normalized, 3.0);
        let position = f64::from(display) * (PALETTE_ANCHORS.len() - 1) as f64;
        let first = position as usize;
        let last = (first + 1).min(PALETTE_ANCHORS.len() - 1);
        let first = PALETTE_ANCHORS.get(first).copied().expect("bounded palette position");
        let last = PALETTE_ANCHORS.get(last).copied().expect("bounded palette position");
        let fraction = (position - position.floor()) as f32;
        let amount = (fraction * 255.0).round() as i32;
        let interpolate = |start: u8, finish: u8| {
            if fraction >= 1.0 {
                finish
            } else {
                (i32::from(start) + (((i32::from(finish) - i32::from(start)) * amount) >> 8)) as u8
            }
        };
        let [red, green, blue] = std::array::from_fn(|channel| {
            interpolate(
                first.get(channel).copied().expect("RGB component"),
                last.get(channel).copied().expect("RGB component"),
            )
        });
        [blue, green, red, 255]
    })
}

#[cfg(test)]
mod test_projection {
    use audiobatch_core::spectrogram::HOP_SIZE;

    use super::*;

    #[test]
    fn partial_final_column_keeps_its_padded_grid_footprint() {
        let source = SpectrogramData::new(1000, 1, 1100, HOP_SIZE as u64, vec![0; FREQUENCY_ROWS * 2])
            .expect("canonical source");
        let viewport = Viewport {
            start: 0.0,
            length: 1.1,
            plot: Bounds::new(point(px(10.0), px(20.0)), size(px(110.0), px(80.0))),
        };
        let RasterBounds { clip, image } = raster_bounds(&source, viewport);
        assert!((f32::from(clip.size.width) - 110.0).abs() < 0.001);
        assert!((f32::from(image.size.width) - 204.8).abs() < 0.001);
        let RasterBounds {
            clip: zoom_clip,
            image: zoom_image,
        } = raster_bounds(
            &source,
            Viewport {
                start: 1.0,
                length: 0.1,
                ..viewport
            },
        );
        assert!((f32::from(zoom_clip.right() - viewport.plot.right())).abs() < 0.001);
        assert!(zoom_image.origin.x < viewport.plot.origin.x);
        assert!(zoom_image.right() > viewport.plot.right());
    }

    #[test]
    fn low_rate_raster_leaves_space_for_fixed_twenty_kilohertz_axis() {
        let source =
            SpectrogramData::new(8000, 1, 1024, HOP_SIZE as u64, vec![0; FREQUENCY_ROWS]).expect("low-rate source");
        let viewport = Viewport {
            start: 0.0,
            length: 1024.0 / 8000.0,
            plot: Bounds::new(point(px(10.0), px(20.0)), size(px(110.0), px(80.0))),
        };
        let image = raster_bounds(&source, viewport).image;
        assert!(image.origin.y > viewport.plot.origin.y);
        assert!((f32::from(image.bottom() - viewport.plot.bottom())).abs() < 0.001);
        assert!((FREQUENCIES[9].0 - 20_000.0).abs() < f64::EPSILON);
    }

    #[test]
    fn high_rate_raster_retains_the_entire_frequency_height() {
        let source = SpectrogramData::new(96_000, 1, 1024, HOP_SIZE as u64, vec![0; FREQUENCY_ROWS])
            .expect("canonical high-rate source");
        let viewport = Viewport {
            start: 0.0,
            length: 1024.0 / 96_000.0,
            plot: Bounds::new(point(px(10.0), px(20.0)), size(px(110.0), px(80.0))),
        };
        let RasterBounds { clip, image } = raster_bounds(&source, viewport);
        assert!((f32::from(image.origin.y - viewport.plot.origin.y)).abs() < f32::EPSILON);
        assert!((f32::from(image.size.height - viewport.plot.size.height)).abs() < f32::EPSILON);
        assert!((f32::from(clip.size.height - viewport.plot.size.height)).abs() < f32::EPSILON);
        let label_fraction = frequency_to_mel(20_000.0) / frequency_to_mel(48_000.0);
        assert!(label_fraction < 1.0);
    }

    #[test]
    fn frequency_captions_cover_reference_range_without_redundant_zeroes() {
        assert_eq!(FREQUENCIES[0], (0.0, "0"));
        assert_eq!(FREQUENCIES[1], (250.0, "250"));
        assert_eq!(FREQUENCIES[3], (1000.0, "1k"));
        assert_eq!(FREQUENCIES[9], (20_000.0, "20k"));
    }
}
