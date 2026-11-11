//! Independent layout contracts and native GPU passes without screenshots, raster readback, or FFT-derived input.

use std::sync::Arc;
use std::time::Duration;

use iced_baseview::futures::executor::block_on;
use iced_baseview::widget::shader::{Pipeline, Primitive, Viewport};
use iced_baseview::{Rectangle, Size, wgpu};
use image_control_dsp::{
    ColorMode, DisplayPreferences, DisplaySettings, FrequencyScale, Imager, ParticleBuilder, SpectralFrameInfo,
    SpectralPoint,
};

use super::{
    BLOOM_LEVEL_COUNT, SOLID_FIELD_LEVEL, Spectrogram, SpectrogramPipeline, SpectrogramPrimitive, bloom_sizes,
    physical_plot_bounds, solid_field_size,
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
    assert_eq!(SOLID_FIELD_LEVEL, 1);
    assert_eq!(solid_field_size([401, 203]), [100, 50]);
    assert_eq!(solid_field_size([3, 1]), [1, 1]);
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
        let shared = &spectrogram.program.shared;
        let preferences = shared.display_preferences();
        assert!(shared.set_display_preferences(DisplayPreferences {
            solid_rendering: !preferences.solid_rendering,
            ..preferences
        }));
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

/// Queues independent, already-weighted observations without involving the asynchronous analyzer.
fn queue_continuity_particles(spectrogram: &Spectrogram, points: &[image_control_dsp::SpatialParticle]) {
    spectrogram
        .program
        .pending
        .lock()
        .expect("pending")
        .points
        .extend_from_slice(points);
}

/// Keeps native continuity cases on identical physical bounds without a window or a pixel expectation.
fn continuity_bounds() -> Rectangle {
    Rectangle {
        x: 0.0,
        y: 0.0,
        width: 200.0,
        height: 100.0,
    }
}

/// Uses the same bounded scissor for each continuity case.
fn continuity_clip() -> Rectangle<u32> {
    Rectangle {
        x: 0,
        y: 0,
        width: 200,
        height: 100,
    }
}

#[test]
fn preference_publication_before_source_increment_discards_pending_and_prepared_gpu_work() {
    let (device, queue) = super::tests::native_device();
    device.push_error_scope(wgpu::ErrorFilter::Validation);
    let mut pipeline = SpectrogramPipeline::new(&device, &queue, wgpu::TextureFormat::Rgba16Float);
    let imager = Imager::new();
    let spectrogram = Spectrogram::new(imager.shared()).expect("analysis owner");
    let shared = &spectrogram.program.shared;
    let generation = shared.analysis_generation();
    let original = shared.display_preferences_snapshot();
    let replacement = Arc::new(
        image_control_dsp::DisplayConfiguration::new(DisplayPreferences {
            mel_scale: !original.mel_scale,
            ..**original
        })
        .expect("valid replacement"),
    );
    let points = independent_particles(FrequencyScale::Logarithmic, ColorMode::Frequency);
    let primitive = SpectrogramPrimitive {
        program: spectrogram.program.clone(),
    };
    let bounds = continuity_bounds();
    let clip = continuity_clip();
    let viewport = Viewport::with_physical_size(Size::new(1024, 512), 1.0);
    let target = output_target(&device, wgpu::TextureFormat::Rgba16Float);
    {
        let mut pending = spectrogram.program.pending.lock().expect("pending");
        pending.points.extend_from_slice(&points);
    }
    primitive.prepare(&mut pipeline, &device, &queue, &bounds, &viewport);
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    primitive.render(&pipeline, &mut encoder, &target, &clip);
    queue.submit([encoder.finish()]);
    let history = pipeline.widgets.get(&spectrogram.program.id).expect("rendered history");
    assert_eq!(history.current.load(super::Ordering::Acquire), 1);
    assert!(history.configuration_matches(generation, &original));

    {
        let mut pending = spectrogram.program.pending.lock().expect("pending");
        pending.points.extend_from_slice(&points);
    }
    primitive.prepare(&mut pipeline, &device, &queue, &bounds, &viewport);
    let history = pipeline.widgets.get(&spectrogram.program.id).expect("prepared history");
    assert_eq!(history.particle_count, 3);
    assert!(history.prepared_matches(generation, &original));

    // Freeze publication immediately after its immutable record swap, before the source increment.
    // The production render core receives exactly the pair sampled at that boundary, without timing sleeps.
    {
        let mut pending = spectrogram.program.pending.lock().expect("pending");
        pending.points.extend_from_slice(&points);
        pending.synchronize(generation, &replacement);
        assert!(
            pending.points.is_empty(),
            "old pending projection must clear with unchanged source"
        );
        assert_eq!(pending.generation, generation);
        assert!(Arc::ptr_eq(&pending.preferences, &replacement));
        drop(pending);
    }
    assert!(!history.prepared_matches(generation, &replacement));
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    primitive.render_configuration(
        &pipeline,
        &mut encoder,
        &target,
        &clip,
        (generation, Arc::clone(&replacement)),
    );
    assert!(history.configuration_matches(generation, &replacement));
    assert_eq!(history.generation.load(super::Ordering::Acquire), generation);
    assert_eq!(
        history.current.load(super::Ordering::Acquire),
        0,
        "clearing cannot run the stale prepared fade/particle pass or advance ping-pong history"
    );
    assert!(!history.update_pending.load(super::Ordering::Acquire));
    assert!(!history.resize_pending.load(super::Ordering::Acquire));
    queue.submit([encoder.finish()]);
    // A-to-B-to-A has equal values but a fresh epoch, including before any audio-source increment.
    let returned =
        Arc::new(image_control_dsp::DisplayConfiguration::new(**original).expect("valid returned projection"));
    assert!(!returned.analysis_matches(&original));
    assert!(!history.prepared_matches(generation, &returned));
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    primitive.render_configuration(
        &pipeline,
        &mut encoder,
        &target,
        &clip,
        (generation, Arc::clone(&returned)),
    );
    assert!(history.configuration_matches(generation, &returned));
    assert_eq!(history.current.load(super::Ordering::Acquire), 0);
    let submission = queue.submit([encoder.finish()]);
    device
        .poll(wgpu::PollType::Wait {
            submission_index: Some(submission),
            timeout: Some(Duration::from_secs(10)),
        })
        .expect("preference-only invalidation passes");
    assert!(block_on(device.pop_error_scope()).is_none(), "native GPU validation");
}

#[test]
fn compatible_live_edits_preserve_native_gpu_history_pending_energy_and_prepared_uploads() {
    let (device, queue) = super::tests::native_device();
    device.push_error_scope(wgpu::ErrorFilter::Validation);
    let mut pipeline = SpectrogramPipeline::new(&device, &queue, wgpu::TextureFormat::Rgba16Float);
    let imager = Imager::new();
    let spectrogram = Spectrogram::new(imager.shared()).expect("analysis owner");
    let shared = &spectrogram.program.shared;
    let generation = shared.analysis_generation();
    let original = shared.display_preferences_snapshot();
    let points = independent_particles(FrequencyScale::Logarithmic, ColorMode::Frequency);
    let primitive = SpectrogramPrimitive {
        program: spectrogram.program.clone(),
    };
    let bounds = continuity_bounds();
    let clip = continuity_clip();
    let viewport = Viewport::with_physical_size(Size::new(1024, 512), 1.0);
    let target = output_target(&device, wgpu::TextureFormat::Rgba16Float);
    queue_continuity_particles(&spectrogram, &points);
    primitive.prepare(&mut pipeline, &device, &queue, &bounds, &viewport);
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    primitive.render(&pipeline, &mut encoder, &target, &clip);
    queue.submit([encoder.finish()]);
    assert_eq!(
        pipeline
            .widgets
            .get(&spectrogram.program.id)
            .expect("history")
            .current
            .load(super::Ordering::Acquire),
        1
    );

    // Compatible changes before prepare retain original particle energy, then changes after prepare accept that upload.
    for index in 0..8 {
        let before = shared.display_preferences();
        assert!(shared.set_display_preferences(DisplayPreferences {
            persistence_seconds: if index % 2 == 0 { 0.1 } else { 1.2 },
            contrast: index as f32 * 0.1,
            pan_midpoint_db: 6.0,
            pan_edge_db: 18.0,
            solid_rendering: index % 2 == 0,
            smooth_correlation: index % 2 == 0,
            show_fps: index % 2 != 0,
            ..before
        }));
        let current = shared.display_preferences_snapshot();
        assert_eq!(shared.analysis_generation(), generation);
        assert!(current.analysis_matches(&original));
        {
            let mut pending = spectrogram.program.pending.lock().expect("pending");
            pending.points.extend_from_slice(&points);
            pending.synchronize(generation, &current);
            assert_eq!(
                pending.points, points,
                "compatible settings do not reweight queued energy"
            );
        }
        primitive.prepare(&mut pipeline, &device, &queue, &bounds, &viewport);
        let history = pipeline.widgets.get(&spectrogram.program.id).expect("prepared history");
        let previous = history.current.load(super::Ordering::Acquire);
        let upload = history.upload.clone();
        assert_eq!(history.particle_count, 3);
        assert!(shared.set_display_preferences(DisplayPreferences {
            persistence_seconds: 2.0,
            contrast: 0.99,
            show_fps: !current.show_fps,
            ..**current
        }));
        let latest = shared.display_preferences_snapshot();
        assert!(history.configuration_matches(generation, &latest));
        assert!(history.prepared_matches(generation, &latest));
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        primitive.render(&pipeline, &mut encoder, &target, &clip);
        assert_eq!(
            history.current.load(super::Ordering::Acquire),
            1 - previous,
            "compatible edits must execute fade/add without a history clear"
        );
        assert_eq!(
            history.upload, upload,
            "retained energy is never retroactively rescaled"
        );
        queue.submit([encoder.finish()]);
    }
    let resized = Rectangle { width: 199.0, ..bounds };
    primitive.prepare(&mut pipeline, &device, &queue, &resized, &viewport);
    let history = pipeline.widgets.get(&spectrogram.program.id).expect("resized history");
    assert!(
        history.resize_source.is_some(),
        "compatible settings preserve resize energy"
    );
    assert!(history.resize_pending.load(super::Ordering::Acquire));
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    primitive.render(&pipeline, &mut encoder, &target, &clip);
    let submission = queue.submit([encoder.finish()]);
    device
        .poll(wgpu::PollType::Wait {
            submission_index: Some(submission),
            timeout: Some(Duration::from_secs(10)),
        })
        .expect("compatible appearance passes");
    assert!(block_on(device.pop_error_scope()).is_none(), "native GPU validation");
}

#[test]
fn native_gpu_runs_particles_and_solid_density_at_dpi_odd_and_tiny_sizes_in_both_encodings() {
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
        for solid_rendering in [false, true] {
            for (case, (scale, color)) in cases.into_iter().zip([
                (FrequencyScale::Logarithmic, ColorMode::Frequency),
                (FrequencyScale::Mel, ColorMode::Phase),
                (FrequencyScale::Logarithmic, ColorMode::Frequency),
            ]) {
                let shared = &spectrogram.program.shared;
                assert!(shared.set_display_preferences(DisplayPreferences {
                    mel_scale: scale == FrequencyScale::Mel,
                    solid_rendering,
                    ..shared.display_preferences()
                }));
                let mut pending = spectrogram.program.pending.lock().expect("pending inputs");
                pending.generation = spectrogram.program.shared.analysis_generation();
                pending.preferences = shared.display_preferences_snapshot();
                pending.points.extend(independent_particles(scale, color));
                drop(pending);
                submit_case(&device, &queue, &mut pipeline, &spectrogram, &target, case);
                let history = pipeline.widgets.get(&spectrogram.program.id).expect("history");
                assert_eq!(history.bloom.len(), 5);
                assert_eq!(history.solid_rendering, solid_rendering);
                assert_eq!(history.particle_count, 3);
                assert_eq!(
                    history.generation.load(super::Ordering::Acquire),
                    spectrogram.program.shared.analysis_generation()
                );
                assert!(!history.resize_pending.load(super::Ordering::Acquire));
                if !case.invalidate_after_prepare {
                    let resized = RenderCase {
                        bounds: Rectangle {
                            width: case.bounds.width - 1.0,
                            ..case.bounds
                        },
                        ..case
                    };
                    submit_case(&device, &queue, &mut pipeline, &spectrogram, &target, resized);
                    let history = pipeline.widgets.get(&spectrogram.program.id).expect("resized history");
                    assert!(
                        history.resize_source.is_some(),
                        "same-generation resize retains GPU energy"
                    );
                    assert_eq!(history.particle_count, 0, "resize must not replay input instances");
                    assert_eq!(history.solid_rendering, solid_rendering);
                }
            }
        }
        // Invalidation plus resize must discard the retained source even when the next frame has no input.
        spectrogram.program.shared.invalidate_monitoring();
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
