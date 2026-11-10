//! Iced shader widget for bounded worker observations and GPU-only persistent spatial energy.
//!
//! Only the editor owns the analysis worker. The audio callback never touches a widget, GPU object, or display mutex.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, Weak};
use std::time::Instant;

use iced_baseview::widget::{Shader, shader};
use iced_baseview::{Element, Length, Rectangle, mouse, wgpu};
use image_control_dsp::{AnalysisShutdown, AnalysisWorker, SharedState, SpatialParticle};

const MAX_PARTICLES: usize = 65_536;
const PARTICLE_STRIDE: u64 = 24;
const MAX_TEXTURE_DIMENSION: u32 = 4_096;
const PERSISTENCE_SECONDS: f32 = 0.7;
const ENERGY_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

/// Owns exactly one analysis consumer for an open editor and publishes only bounded GPU instance work.
/// Construct, tick, and destroy this on the editor thread, never in a processing callback.
pub struct Spectrogram {
    worker: AnalysisWorker,
    scratch: Vec<SpatialParticle>,
    program: SpectrogramProgram,
}

/// Shares pending observations between immutable Iced views and renderer preparation, never with the audio callback.
#[derive(Debug)]
struct PendingParticles {
    points: Vec<SpatialParticle>,
}

/// A stable identity keeps cached GPU history separate for every spectrogram using the same Iced renderer.
#[derive(Clone, Debug)]
struct SpectrogramProgram {
    id: u64,
    pending: Arc<Mutex<PendingParticles>>,
}

/// Lightweight per-frame command description. Its pipeline retains all expensive GPU allocations.
#[derive(Debug)]
struct SpectrogramPrimitive {
    program: SpectrogramProgram,
}

/// Shared immutable pipelines and per-widget GPU histories, owned and destroyed by Iced's renderer.
struct SpectrogramPipeline {
    uniform_layout: wgpu::BindGroupLayout,
    texture_layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    fade: wgpu::RenderPipeline,
    particles: wgpu::RenderPipeline,
    bloom: wgpu::RenderPipeline,
    composite: wgpu::RenderPipeline,
    encode_output: bool,
    widgets: HashMap<u64, GpuHistory>,
}

/// Persistent ping-pong targets and upload storage for one widget. Reallocated only on physical size changes.
struct GpuHistory {
    owner: Weak<Mutex<PendingParticles>>,
    accumulation: [wgpu::TextureView; 2],
    bloom: wgpu::TextureView,
    accumulation_inputs: [wgpu::BindGroup; 2],
    composite_inputs: [wgpu::BindGroup; 2],
    uniform_buffer: wgpu::Buffer,
    uniform_group: wgpu::BindGroup,
    particle_buffer: wgpu::Buffer,
    upload: Vec<u8>,
    particle_count: u32,
    size: [u32; 2],
    bounds: Rectangle,
    current: AtomicUsize,
    update_pending: AtomicBool,
    last_frame: Instant,
}

impl Spectrogram {
    /// Starts the bounded worker and leaves all GPU allocation to Iced's active renderer.
    ///
    /// # Errors
    /// Returns the worker creation error, including an already-owned analysis consumer.
    pub fn new(shared: Arc<SharedState>) -> Result<Self, std::io::Error> {
        Ok(Self {
            worker: AnalysisWorker::new(shared)?,
            scratch: Vec::with_capacity(MAX_PARTICLES),
            program: SpectrogramProgram {
                id: NEXT_ID.fetch_add(1, Ordering::Relaxed),
                pending: Arc::new(Mutex::new(PendingParticles {
                    points: Vec::with_capacity(MAX_PARTICLES),
                })),
            },
        })
    }

    /// Returns a GUI-bridge shutdown handle that does not borrow this spectrogram or its editor state.
    ///
    /// Retain it outside the Iced program so editor removal can join analysis during an active GUI poll.
    /// Call its shutdown only off the audio thread. Dropping this spectrogram also joins the worker.
    #[must_use]
    pub fn shutdown_handle(&self) -> AnalysisShutdown {
        self.worker.shutdown_handle()
    }

    /// Drains already-projected worker results on an editor timer, without FFTs or CPU image generation.
    /// Returns true because even silent frames must redraw to fade retained GPU energy.
    pub fn tick(&mut self) -> bool {
        self.worker.drain(&mut self.scratch);
        if !self.scratch.is_empty() {
            let mut pending = self
                .program
                .pending
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if pending.points.len() + self.scratch.len() > MAX_PARTICLES {
                pending.points.clear();
            }
            pending.points.extend(self.scratch.iter().take(MAX_PARTICLES).copied());
        }
        true
    }

    /// Returns a real Iced shader widget. The surrounding editor owns axes, labels, and refresh scheduling.
    pub fn view<'a, Message: 'a>(&'a self) -> Element<'a, Message> {
        Shader::new(self.program.clone())
            .width(Length::Fill)
            .height(Length::Fill)
            .into()
    }
}

impl Drop for Spectrogram {
    /// Releases bounded particle storage even if the upstream window shell retains its final widget cache.
    /// This owner is destroyed on the editor thread.
    /// Audio never accesses this mutex or vector.
    fn drop(&mut self) {
        let mut pending = self
            .program
            .pending
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        pending.points = Vec::new();
    }
}

impl<Message> shader::Program<Message> for SpectrogramProgram {
    type State = ();
    type Primitive = SpectrogramPrimitive;

    /// Publishes the stable GPU input identity without copying particle batches into the widget tree.
    fn draw(&self, _state: &Self::State, _cursor: mouse::Cursor, _bounds: Rectangle) -> Self::Primitive {
        SpectrogramPrimitive { program: self.clone() }
    }
}

impl shader::Primitive for SpectrogramPrimitive {
    type Pipeline = SpectrogramPipeline;

    /// Uploads bounded instance attributes and uniforms. No pixels are generated or transferred by the CPU.
    fn prepare(
        &self,
        pipeline: &mut Self::Pipeline,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        bounds: &Rectangle,
        viewport: &shader::Viewport,
    ) {
        let scale = viewport.scale_factor();
        let physical_bounds = Rectangle {
            x: bounds.x * scale,
            y: bounds.y * scale,
            width: bounds.width * scale,
            height: bounds.height * scale,
        };
        let Some(size) = target_size(physical_bounds, device.limits().max_texture_dimension_2d) else {
            pipeline.widgets.remove(&self.program.id);
            return;
        };
        let replace = pipeline
            .widgets
            .get(&self.program.id)
            .is_none_or(|history| history.size != size);
        if replace {
            let history = GpuHistory::new(device, pipeline, &self.program, size);
            pipeline.widgets.insert(self.program.id, history);
        }
        let Some(history) = pipeline.widgets.get_mut(&self.program.id) else {
            return;
        };
        history.bounds = physical_bounds;
        history.upload.clear();
        {
            let mut pending = self
                .program
                .pending
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            for point in pending.points.drain(..).take(MAX_PARTICLES) {
                if let Some(attributes) = particle_attributes(point) {
                    for value in attributes {
                        history.upload.extend_from_slice(&value.to_ne_bytes());
                    }
                }
            }
        }
        history.particle_count = (history.upload.len() as u64 / PARTICLE_STRIDE) as u32;
        if !history.upload.is_empty() {
            queue.write_buffer(&history.particle_buffer, 0, &history.upload);
        }
        let now = Instant::now();
        let fade = persistence_fade(now.saturating_duration_since(history.last_frame).as_secs_f32());
        history.last_frame = now;
        let [width, height] = size;
        let parameters = [width as f32, height as f32, fade, f32::from(pipeline.encode_output)];
        let mut bytes = [0_u8; 16];
        for (destination, value) in bytes.as_chunks_mut::<4>().0.iter_mut().zip(parameters) {
            destination.copy_from_slice(&value.to_ne_bytes());
        }
        queue.write_buffer(&history.uniform_buffer, 0, &bytes);
        history.update_pending.store(true, Ordering::Release);
    }

    /// Encodes fade, additive Gaussian instances, bloom, and clipped composition into Iced's command encoder.
    fn render(
        &self,
        pipeline: &Self::Pipeline,
        encoder: &mut wgpu::CommandEncoder,
        target: &wgpu::TextureView,
        clip_bounds: &Rectangle<u32>,
    ) {
        let Some(history) = pipeline.widgets.get(&self.program.id) else {
            return;
        };
        if history.update_pending.swap(false, Ordering::AcqRel) {
            let source = history.current.load(Ordering::Acquire);
            let destination = 1 - source;
            if let (Some(input), Some(output)) = (
                history.accumulation_inputs.get(source),
                history.accumulation.get(destination),
            ) {
                {
                    let mut pass = begin_pass(encoder, output, "ImageControl fade and particles", true);
                    pass.set_bind_group(0, &history.uniform_group, &[]);
                    pass.set_bind_group(1, input, &[]);
                    pass.set_pipeline(&pipeline.fade);
                    pass.draw(0..3, 0..1);
                    if history.particle_count > 0 {
                        pass.set_pipeline(&pipeline.particles);
                        pass.set_vertex_buffer(0, history.particle_buffer.slice(..));
                        pass.draw(0..6, 0..history.particle_count);
                    }
                }
                if let Some(input) = history.accumulation_inputs.get(destination) {
                    let mut pass = begin_pass(encoder, &history.bloom, "ImageControl bloom", true);
                    pass.set_pipeline(&pipeline.bloom);
                    pass.set_bind_group(0, &history.uniform_group, &[]);
                    pass.set_bind_group(1, input, &[]);
                    pass.draw(0..3, 0..1);
                }
                history.current.store(destination, Ordering::Release);
            }
        }
        let Some(input) = history.composite_inputs.get(history.current.load(Ordering::Acquire)) else {
            return;
        };
        if clip_bounds.width == 0 || clip_bounds.height == 0 {
            return;
        }
        let mut pass = begin_pass(encoder, target, "ImageControl composite", false);
        pass.set_pipeline(&pipeline.composite);
        pass.set_bind_group(0, &history.uniform_group, &[]);
        pass.set_bind_group(1, input, &[]);
        pass.set_viewport(
            history.bounds.x,
            history.bounds.y,
            history.bounds.width,
            history.bounds.height,
            0.0,
            1.0,
        );
        pass.set_scissor_rect(clip_bounds.x, clip_bounds.y, clip_bounds.width, clip_bounds.height);
        pass.draw(0..3, 0..1);
    }
}

impl shader::Pipeline for SpectrogramPipeline {
    /// Creates portable wgpu pipelines on Iced's existing device, including its native Metal device on macOS.
    fn new(device: &wgpu::Device, _queue: &wgpu::Queue, format: wgpu::TextureFormat) -> Self {
        let uniform_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("ImageControl parameters layout"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: wgpu::BufferSize::new(16),
                },
                count: None,
            }],
        });
        let texture_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let texture_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("ImageControl textures layout"),
            entries: &[
                texture_entry(0),
                texture_entry(1),
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("ImageControl spectrogram layout"),
            bind_group_layouts: &[&uniform_layout, &texture_layout],
            push_constant_ranges: &[],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("ImageControl spatial spectrogram"),
            source: wgpu::ShaderSource::Wgsl(include_str!("spectrogram.wgsl").into()),
        });
        let fade = render_pipeline(device, &layout, &module, "fade_fragment", ENERGY_FORMAT, false);
        let particles = render_pipeline(device, &layout, &module, "particle_fragment", ENERGY_FORMAT, true);
        let bloom = render_pipeline(device, &layout, &module, "bloom_fragment", ENERGY_FORMAT, false);
        let composite = render_pipeline(device, &layout, &module, "composite_fragment", format, false);
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("ImageControl linear clamp"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..wgpu::SamplerDescriptor::default()
        });
        Self {
            uniform_layout,
            texture_layout,
            sampler,
            fade,
            particles,
            bloom,
            composite,
            encode_output: !format.is_srgb(),
            widgets: HashMap::new(),
        }
    }

    /// Releases closed widgets' histories even when the host keeps its Iced renderer alive.
    fn trim(&mut self) {
        self.widgets.retain(|_, history| history.owner.strong_count() > 0);
    }
}

impl GpuHistory {
    /// Allocates bounded persistent float targets and reusable upload buffers off the audio thread.
    fn new(
        device: &wgpu::Device,
        pipeline: &SpectrogramPipeline,
        program: &SpectrogramProgram,
        size: [u32; 2],
    ) -> Self {
        let accumulation = [energy_target(device, size), energy_target(device, size)];
        let bloom = energy_target(device, [size[0].div_ceil(4), size[1].div_ceil(4)]);
        // Fade and bloom do not sample binding 1. Bind the source again rather than the active attachment.
        let accumulation_inputs = accumulation
            .each_ref()
            .map(|view| texture_group(device, pipeline, view, view));
        let composite_inputs = accumulation
            .each_ref()
            .map(|view| texture_group(device, pipeline, view, &bloom));
        let uniform_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ImageControl parameters"),
            size: 16,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let uniform_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("ImageControl parameters"),
            layout: &pipeline.uniform_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: uniform_buffer.as_entire_binding(),
            }],
        });
        let particle_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("ImageControl bounded particle instances"),
            size: MAX_PARTICLES as u64 * PARTICLE_STRIDE,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        Self {
            owner: Arc::downgrade(&program.pending),
            accumulation,
            bloom,
            accumulation_inputs,
            composite_inputs,
            uniform_buffer,
            uniform_group,
            particle_buffer,
            upload: Vec::with_capacity(MAX_PARTICLES * PARTICLE_STRIDE as usize),
            particle_count: 0,
            size,
            bounds: Rectangle::default(),
            current: AtomicUsize::new(0),
            update_pending: AtomicBool::new(false),
            last_frame: Instant::now(),
        }
    }
}

/// Validates only instance attributes, never rasterizes observations on the CPU.
fn particle_attributes(point: SpatialParticle) -> Option<[f32; 6]> {
    let [x, y] = point.position();
    let [red, green, blue] = point.color();
    let values = [x, y, red, green, blue, point.emphasis()];
    values
        .iter()
        .all(|value| value.is_finite() && (0.0..=1.0).contains(value))
        .then_some(values)
}

/// Rejects empty or non-finite layout and bounds GPU allocation independently of window dimensions.
fn target_size(bounds: Rectangle, device_limit: u32) -> Option<[u32; 2]> {
    if ![bounds.x, bounds.y, bounds.width, bounds.height]
        .iter()
        .all(|value| value.is_finite())
        || bounds.width <= 0.0
        || bounds.height <= 0.0
    {
        return None;
    }
    let limit = device_limit.clamp(1, MAX_TEXTURE_DIMENSION);
    Some([
        bounds.width.ceil().clamp(1.0, limit as f32) as u32,
        bounds.height.ceil().clamp(1.0, limit as f32) as u32,
    ])
}

/// Uses wall-clock decay so persistence does not depend on the host's editor refresh frequency.
fn persistence_fade(elapsed: f32) -> f32 {
    (-elapsed.max(0.0) / PERSISTENCE_SECONDS).exp()
}

/// Makes one float texture view. wgpu retains its texture through the view and clears first use safely.
fn energy_target(device: &wgpu::Device, size: [u32; 2]) -> wgpu::TextureView {
    device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some("ImageControl persistent energy"),
            size: wgpu::Extent3d {
                width: size[0],
                height: size[1],
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: ENERGY_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        })
        .create_view(&wgpu::TextureViewDescriptor::default())
}

/// Binds sampled targets independently of the output attachment to avoid texture feedback hazards.
fn texture_group(
    device: &wgpu::Device,
    pipeline: &SpectrogramPipeline,
    source: &wgpu::TextureView,
    bloom: &wgpu::TextureView,
) -> wgpu::BindGroup {
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("ImageControl sampled energy"),
        layout: &pipeline.texture_layout,
        entries: &[
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::TextureView(source),
            },
            wgpu::BindGroupEntry {
                binding: 1,
                resource: wgpu::BindingResource::TextureView(bloom),
            },
            wgpu::BindGroupEntry {
                binding: 2,
                resource: wgpu::BindingResource::Sampler(&pipeline.sampler),
            },
        ],
    })
}

/// Creates one pass pipeline with a shared resource layout and optional additive instance blending.
fn render_pipeline(
    device: &wgpu::Device,
    layout: &wgpu::PipelineLayout,
    module: &wgpu::ShaderModule,
    fragment: &str,
    format: wgpu::TextureFormat,
    particles: bool,
) -> wgpu::RenderPipeline {
    let attributes = wgpu::vertex_attr_array![0 => Float32x2, 1 => Float32x3, 2 => Float32];
    let buffers = [wgpu::VertexBufferLayout {
        array_stride: PARTICLE_STRIDE,
        step_mode: wgpu::VertexStepMode::Instance,
        attributes: &attributes,
    }];
    let additive = wgpu::BlendComponent {
        src_factor: wgpu::BlendFactor::One,
        dst_factor: wgpu::BlendFactor::One,
        operation: wgpu::BlendOperation::Add,
    };
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(fragment),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module,
            entry_point: Some(if particles { "particle_vertex" } else { "screen_vertex" }),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: if particles { &buffers } else { &[] },
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module,
            entry_point: Some(fragment),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format,
                blend: particles.then_some(wgpu::BlendState {
                    color: additive,
                    alpha: additive,
                }),
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview: None,
        cache: None,
    })
}

/// Starts a private GPU pass while preserving Iced's existing target pixels for final composition.
fn begin_pass<'a>(
    encoder: &'a mut wgpu::CommandEncoder,
    target: &'a wgpu::TextureView,
    label: &'a str,
    clear: bool,
) -> wgpu::RenderPass<'a> {
    encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some(label),
        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
            view: target,
            depth_slice: None,
            resolve_target: None,
            ops: wgpu::Operations {
                load: if clear {
                    wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT)
                } else {
                    wgpu::LoadOp::Load
                },
                store: wgpu::StoreOp::Store,
            },
        })],
        ..wgpu::RenderPassDescriptor::default()
    })
}

#[cfg(test)]
mod tests {
    use super::{PERSISTENCE_SECONDS, persistence_fade, target_size};
    use iced_baseview::Rectangle;

    #[test]
    fn gpu_dimensions_are_bounded_and_empty_layout_is_rejected() {
        let bounds = Rectangle {
            x: 0.0,
            y: 0.0,
            width: 100_000.0,
            height: 500.1,
        };
        assert_eq!(target_size(bounds, 2_048), Some([2_048, 501]));
        assert!(
            target_size(
                Rectangle {
                    width: f32::NAN,
                    ..bounds
                },
                4_096
            )
            .is_none()
        );
        assert!(target_size(Rectangle { height: 0.0, ..bounds }, 4_096).is_none());
    }

    #[test]
    fn closing_owner_releases_particles_even_if_a_widget_cache_survives() {
        let imager = image_control_dsp::Imager::new();
        let spectrogram = super::Spectrogram::new(imager.shared()).expect("analysis worker");
        let cached = spectrogram.program.clone();
        assert!(cached.pending.lock().expect("pending").points.capacity() >= super::MAX_PARTICLES);
        drop(spectrogram);
        assert_eq!(cached.pending.lock().expect("pending").points.capacity(), 0);
        let reopened = super::Spectrogram::new(imager.shared()).expect("previous worker released ownership");
        drop(reopened);
    }

    #[test]
    fn external_shutdown_allows_reopening_before_retained_spectrogram_is_dropped() {
        let imager = image_control_dsp::Imager::new();
        let spectrogram = super::Spectrogram::new(imager.shared()).expect("analysis worker");
        let shutdown = spectrogram.shutdown_handle();
        shutdown.shutdown();
        let reopened = super::Spectrogram::new(imager.shared()).expect("external shutdown joined worker");
        drop(spectrogram);
        shutdown.shutdown();
        assert!(
            super::Spectrogram::new(imager.shared()).is_err(),
            "replacement retains ownership"
        );
        drop(reopened);
        assert!(super::Spectrogram::new(imager.shared()).is_ok());
    }

    /// Exercises the actual wgpu shader and passes with real worker observations, without pixel readback.
    #[test]
    fn native_gpu_executes_particles_fade_bloom_resize_and_separate_histories() {
        use std::sync::Arc;
        use std::time::{Duration, Instant};

        use iced_baseview::futures::executor::block_on;
        use iced_baseview::widget::shader::{Pipeline, Primitive, Viewport};
        use iced_baseview::{Size, wgpu};
        use image_control_dsp::Imager;

        use super::{Spectrogram, SpectrogramPipeline, SpectrogramPrimitive, energy_target};

        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: if cfg!(target_os = "macos") {
                wgpu::Backends::METAL
            } else {
                wgpu::Backends::DX12
            },
            ..wgpu::InstanceDescriptor::default()
        });
        let adapter =
            block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default())).expect("native GPU adapter");
        let (device, queue) =
            block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).expect("native GPU device");
        device.push_error_scope(wgpu::ErrorFilter::Validation);
        let mut pipeline = SpectrogramPipeline::new(&device, &queue, wgpu::TextureFormat::Rgba16Float);
        let mut imager = Imager::new();
        assert!(imager.prepare(48_000.0));
        let mut spectrogram = Spectrogram::new(imager.shared()).expect("analysis worker");
        imager.begin_block();
        for index in 0..8_192 {
            let sine = (std::f32::consts::TAU * 750.0 * index as f32 / 48_000.0).sin() * 0.5;
            let _ = imager.process_frame([sine, sine]);
        }
        let deadline = Instant::now() + Duration::from_secs(2);
        while spectrogram.program.pending.lock().expect("pending").points.is_empty() && Instant::now() < deadline {
            spectrogram.tick();
            std::thread::yield_now();
        }
        assert!(
            !spectrogram.program.pending.lock().expect("pending").points.is_empty(),
            "worker observations"
        );
        let primitive = SpectrogramPrimitive {
            program: spectrogram.program.clone(),
        };
        let target = energy_target(&device, [512, 256]);
        let viewport = Viewport::with_physical_size(Size::new(512, 256), 1.0);
        let bounds = Rectangle {
            x: 10.0,
            y: 20.0,
            width: 400.0,
            height: 200.0,
        };
        let clip = Rectangle {
            x: 10,
            y: 20,
            width: 400,
            height: 200,
        };
        for frame in 0..3 {
            let bounds = if frame == 2 {
                Rectangle { width: 380.0, ..bounds }
            } else {
                bounds
            };
            primitive.prepare(&mut pipeline, &device, &queue, &bounds, &viewport);
            let history = pipeline.widgets.get(&spectrogram.program.id).expect("history");
            if frame == 0 {
                assert!(history.particle_count > 0);
            } else {
                assert_eq!(history.particle_count, 0, "observations must not be added twice");
            }
            let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
            primitive.render(&pipeline, &mut encoder, &target, &clip);
            let submission = queue.submit([encoder.finish()]);
            device
                .poll(wgpu::PollType::Wait {
                    submission_index: Some(submission),
                    timeout: Some(Duration::from_secs(10)),
                })
                .expect("bounded GPU completion");
        }
        let other_imager = Imager::new();
        let other = Spectrogram::new(other_imager.shared()).expect("independent analysis worker");
        SpectrogramPrimitive {
            program: other.program.clone(),
        }
        .prepare(&mut pipeline, &device, &queue, &bounds, &viewport);
        assert_eq!(pipeline.widgets.len(), 2);
        drop(other);
        pipeline.trim();
        assert_eq!(pipeline.widgets.len(), 1, "closed widgets release cached targets");
        assert!(Arc::strong_count(&spectrogram.program.pending) >= 2);
        assert!(block_on(device.pop_error_scope()).is_none(), "wgpu validation");
    }

    #[test]
    fn persistence_is_independent_of_display_refresh_rate() {
        let whole = persistence_fade(PERSISTENCE_SECONDS);
        let quarters = persistence_fade(PERSISTENCE_SECONDS * 0.25).powi(4);
        assert!((whole - (-1.0_f32).exp()).abs() < 1.0e-6);
        assert!((whole - quarters).abs() < 1.0e-6);
        assert!(persistence_fade(60.0) < 1.0e-30);
    }
}
