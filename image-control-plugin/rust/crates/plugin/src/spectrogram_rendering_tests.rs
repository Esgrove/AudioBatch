//! Independent layout contracts and native GPU passes without screenshots, raster readback, or FFT-derived input.

use std::time::Duration;

use iced_baseview::futures::executor::block_on;
use iced_baseview::widget::shader::{Pipeline, Primitive, Viewport};
use iced_baseview::{Rectangle, Size, wgpu};
use image_control_dsp::{
    ColorMode, DisplaySettings, FrequencyScale, Imager, ParticleBuilder, SpectralFrameInfo, SpectralPoint,
};

use super::{
    BLOOM_LEVEL_COUNT, Spectrogram, SpectrogramPipeline, SpectrogramPrimitive, bloom_sizes, physical_plot_bounds,
};

/// Keeps physical viewport, scale, and fractional logical placement explicit at each resource transition.
#[derive(Clone, Copy)]
struct RenderCase {
    bounds: Rectangle,
    scale: f32,
    invalidate_after_prepare: bool,
}

#[test]
fn physical_plot_enclosure_matches_reference_and_rejects_empty_layout() {
    let logical = Rectangle {
        x: 10.25,
        y: 20.75,
        width: 400.5,
        height: 200.5,
    };
    assert_eq!(
        physical_plot_bounds(logical, 2.0),
        Rectangle {
            x: 20.0,
            y: 41.0,
            width: 802.0,
            height: 402.0,
        }
    );
    for scale in [0.0, -1.0, f32::NAN, f32::INFINITY] {
        assert_eq!(physical_plot_bounds(logical, scale), Rectangle::default());
    }
    assert_eq!(
        physical_plot_bounds(Rectangle { width: 0.0, ..logical }, 2.0),
        Rectangle::default()
    );
}

#[test]
fn glow_chain_uses_five_floor_halvings_with_one_pixel_minimum() {
    assert_eq!(BLOOM_LEVEL_COUNT, 5);
    assert_eq!(
        bloom_sizes([401, 203]),
        [[200, 101], [100, 50], [50, 25], [25, 12], [12, 6]]
    );
    assert_eq!(bloom_sizes([3, 1]), [[1, 1]; 5]);
}

/// Supplies independent bass/treble, left/right, coherent/anti-phase, and wide/narrow-spacing observations.
/// These expose resource and pass contracts without depending on analyzer output or expected pixels.
fn independent_particles(scale: FrequencyScale, color: ColorMode) -> Vec<image_control_dsp::SpatialParticle> {
    let mut builder = ParticleBuilder::new();
    builder.set_settings(DisplaySettings::new(color, scale, 0.3, 0.6).expect("display settings"));
    let points = [
        SpectralPoint::new(20.0, 20.0, 40.0, 0.0, 1.0, 1.0).expect("wide bass"),
        SpectralPoint::new(1000.0, 1000.0, 23.4375, 0.5, 0.1, -1.0).expect("mono midrange"),
        SpectralPoint::new(20_000.0, 20_000.0, 1.0, 1.0, 0.05, 0.0).expect("treble edge"),
    ];
    builder.add_frame(
        &points,
        SpectralFrameInfo {
            tier_index: 0,
            frame_seconds: 0.04,
            end_seconds: 0.04,
            last_at_time: true,
        },
    );
    assert_eq!(builder.particles().len(), 3);
    builder.particles().to_vec()
}

/// Creates a real presentation-format attachment without involving any host window or installed plugin.
fn output_target(device: &wgpu::Device, format: wgpu::TextureFormat) -> wgpu::TextureView {
    device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some("independent spectrogram presentation target"),
            size: wgpu::Extent3d {
                width: 1024,
                height: 512,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        })
        .create_view(&wgpu::TextureViewDescriptor::default())
}

/// Submits the production passes and waits only inside the test, never in ordinary rendering.
fn submit_case(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    pipeline: &mut SpectrogramPipeline,
    spectrogram: &Spectrogram,
    target: &wgpu::TextureView,
    case: RenderCase,
) {
    let primitive = SpectrogramPrimitive {
        program: spectrogram.program.clone(),
    };
    let viewport = Viewport::with_physical_size(Size::new(1024, 512), case.scale);
    primitive.prepare(pipeline, device, queue, &case.bounds, &viewport);
    if case.invalidate_after_prepare {
        spectrogram
            .program
            .shared
            .set_analysis_tap(image_control_dsp::AnalysisTap::Pre);
    }
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    // A partially clipped plot must keep its own viewport and rounded local coordinates.
    let clip = Rectangle {
        x: 25,
        y: 30,
        width: 700,
        height: 380,
    };
    primitive.render(pipeline, &mut encoder, target, &clip);
    let submission = queue.submit([encoder.finish()]);
    device
        .poll(wgpu::PollType::Wait {
            submission_index: Some(submission),
            timeout: Some(Duration::from_secs(10)),
        })
        .expect("native reference pass completion");
}

#[test]
fn native_gpu_runs_reference_bloom_at_dpi_odd_and_tiny_sizes_in_both_output_encodings() {
    let (device, queue) = super::tests::native_device();
    device.push_error_scope(wgpu::ErrorFilter::Validation);
    for format in [wgpu::TextureFormat::Bgra8Unorm, wgpu::TextureFormat::Bgra8UnormSrgb] {
        let mut pipeline = SpectrogramPipeline::new(&device, &queue, format);
        assert_eq!(pipeline.encode_output, !format.is_srgb());
        let imager = Imager::new();
        let spectrogram = Spectrogram::new(imager.shared()).expect("independent analysis owner");
        let target = output_target(&device, format);
        let cases = [
            RenderCase {
                bounds: Rectangle {
                    x: 10.25,
                    y: 20.75,
                    width: 401.0,
                    height: 203.0,
                },
                scale: 2.0,
                invalidate_after_prepare: false,
            },
            RenderCase {
                bounds: Rectangle {
                    x: 10.0,
                    y: 20.0,
                    width: 399.0,
                    height: 201.0,
                },
                scale: 1.0,
                invalidate_after_prepare: false,
            },
            RenderCase {
                bounds: Rectangle {
                    x: 10.0,
                    y: 20.0,
                    width: 3.0,
                    height: 1.0,
                },
                scale: 1.0,
                invalidate_after_prepare: true,
            },
        ];
        for (case, (scale, color)) in cases.into_iter().zip([
            (FrequencyScale::Logarithmic, ColorMode::Frequency),
            (FrequencyScale::Mel, ColorMode::Phase),
            (FrequencyScale::Logarithmic, ColorMode::Frequency),
        ]) {
            let mut pending = spectrogram.program.pending.lock().expect("pending inputs");
            pending.generation = spectrogram.program.shared.analysis_generation();
            pending.points.extend(independent_particles(scale, color));
            drop(pending);
            submit_case(&device, &queue, &mut pipeline, &spectrogram, &target, case);
            let history = pipeline.widgets.get(&spectrogram.program.id).expect("history");
            assert_eq!(history.bloom.len(), 5);
            assert_eq!(history.particle_count, 3);
            assert_eq!(
                history.generation.load(super::Ordering::Acquire),
                spectrogram.program.shared.analysis_generation()
            );
            assert!(!history.resize_pending.load(super::Ordering::Acquire));
        }
        // Invalidation plus resize must discard the retained source even when the next frame has no input.
        spectrogram
            .program
            .shared
            .set_analysis_tap(image_control_dsp::AnalysisTap::Post);
        let [first_case, ..] = cases;
        submit_case(&device, &queue, &mut pipeline, &spectrogram, &target, first_case);
        let history = pipeline.widgets.get(&spectrogram.program.id).expect("empty history");
        assert_eq!(history.particle_count, 0);
        assert!(
            history.resize_source.is_none(),
            "stale generation cannot survive a resize binding"
        );
    }
    assert!(
        block_on(device.pop_error_scope()).is_none(),
        "real GPU resource validation"
    );
}
