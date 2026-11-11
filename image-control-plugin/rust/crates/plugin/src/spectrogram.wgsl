// Reference linear-energy particles, density contours, five-level bloom, and rounded sRGB composition on the GPU.
struct Parameters {
    size: vec2<f32>,
    fade: f32,
    encode_output: f32,
    display_scale: f32,
    solid_rendering: f32,
    padding1: f32,
    padding2: f32,
};

const CORE_SIGMA_PIXELS: f32 = 1.3;
const MAX_SIGMA_PIXELS: f32 = 6.0;
const HALO_SIGMA_PIXELS: f32 = 4.0;
const HALO_WEIGHT: f32 = 0.2;
const SPRITE_REACH_SIGMAS: f32 = 3.0;
const EXPOSURE: f32 = 1.2;
// Five additive blur levels share the reference's total glow strength of 0.8.
const GLOW_PER_LEVEL: f32 = 0.16;
const HIGHLIGHT_START: f32 = 2.0;
const HIGHLIGHT_END: f32 = 8.0;
const HIGHLIGHT_MIX: f32 = 0.6;
const CORNER_RADIUS_PIXELS: f32 = 6.0;
const SOLID_FIELD_GAIN: f32 = 1.5;
const SOLID_LAYER_LEVELS = array<f32, 4>(0.06, 0.18, 0.38, 0.62);
const SOLID_LAYER_ALPHAS = array<f32, 4>(0.16, 0.26, 0.38, 0.52);
const SOLID_OUTLINE_STRENGTHS = array<f32, 4>(0.25, 0.4, 0.6, 0.85);
const SOLID_OUTLINE_PIXELS: f32 = 1.25;
const SOLID_HIGHLIGHT_START: f32 = 0.35;
const SOLID_HIGHLIGHT_END: f32 = 0.75;
const SOLID_GLOW_SHARE: f32 = 0.5;

@group(0) @binding(0) var<uniform> parameters: Parameters;
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var bloom: texture_2d<f32>;
@group(1) @binding(2) var linear_sampler: sampler;
@group(1) @binding(3) var field: texture_2d<f32>;

struct ScreenVertex {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn screen_vertex(@builtin(vertex_index) index: u32) -> ScreenVertex {
    let corner = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return ScreenVertex(vec4<f32>(corner * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0), corner);
}

@fragment
fn resample_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    return textureSample(source, linear_sampler, input.uv);
}

@fragment
fn fade_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    return vec4<f32>(textureSample(source, linear_sampler, input.uv).rgb * parameters.fade, 0.0);
}

struct ParticleVertex {
    @builtin(position) position: vec4<f32>,
    @location(0) offset: vec2<f32>,
    @location(1) @interpolate(flat) core: vec2<f32>,
    @location(2) @interpolate(flat) halo: vec2<f32>,
    @location(3) @interpolate(flat) color: vec3<f32>,
    @location(4) @interpolate(flat) emphasis: f32,
};

@vertex
fn particle_vertex(
    @builtin(vertex_index) index: u32,
    @location(0) position: vec2<f32>,
    @location(1) spread: f32,
    @location(2) color: vec3<f32>,
    @location(3) size: f32,
    @location(4) emphasis: f32,
) -> ParticleVertex {
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(-1.0, 1.0),
        vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),
    );
    let sized_core = CORE_SIGMA_PIXELS * parameters.display_scale * size;
    let sized_halo = HALO_SIGMA_PIXELS * parameters.display_scale * size;
    let core = vec2<f32>(clamp(spread * parameters.size.x, sized_core, MAX_SIGMA_PIXELS * parameters.display_scale), sized_core);
    let halo = vec2<f32>(max(core.x, sized_halo), sized_halo);
    let offset = corners[index] * halo * SPRITE_REACH_SIGMAS;
    let point = position * parameters.size + offset;
    let clip = point / parameters.size * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    return ParticleVertex(vec4<f32>(clip, 0.0, 1.0), offset, core, halo, color, emphasis);
}

@fragment
fn particle_fragment(input: ParticleVertex) -> @location(0) vec4<f32> {
    let core = input.offset / input.core;
    let halo = input.offset / input.halo;
    let weight = exp(-0.5 * dot(core, core)) + HALO_WEIGHT * input.emphasis * exp(-0.5 * dot(halo, halo));
    return vec4<f32>(input.color * weight, 0.0);
}

// The reference's normalized 13-tap downsample avoids flicker as subpixel particles move.
@fragment
fn downsample_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    let texel = 1.0 / vec2<f32>(textureDimensions(source));
    let outer = (
        textureSample(source, linear_sampler, input.uv + vec2<f32>(-2.0, 2.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(2.0, 2.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(-2.0, -2.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(2.0, -2.0) * texel).rgb
    ) * 0.03125;
    let edges = (
        textureSample(source, linear_sampler, input.uv + vec2<f32>(0.0, 2.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(-2.0, 0.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(2.0, 0.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(0.0, -2.0) * texel).rgb
    ) * 0.0625;
    let inner = (
        textureSample(source, linear_sampler, input.uv + vec2<f32>(-1.0, 1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(1.0, 1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(-1.0, -1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(1.0, -1.0) * texel).rgb
    ) * 0.125;
    return vec4<f32>(textureSample(source, linear_sampler, input.uv).rgb * 0.125 + outer + edges + inner, 0.0);
}

// A one-source-texel 3x3 tent adds the smaller level into the existing larger level.
@fragment
fn upsample_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    let texel = 1.0 / vec2<f32>(textureDimensions(source));
    var sum = textureSample(source, linear_sampler, input.uv).rgb * 4.0;
    sum += (
        textureSample(source, linear_sampler, input.uv + vec2<f32>(0.0, 1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(-1.0, 0.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(1.0, 0.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(0.0, -1.0) * texel).rgb
    ) * 2.0;
    sum += textureSample(source, linear_sampler, input.uv + vec2<f32>(-1.0, 1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(1.0, 1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(-1.0, -1.0) * texel).rgb
        + textureSample(source, linear_sampler, input.uv + vec2<f32>(1.0, -1.0) * texel).rgb;
    return vec4<f32>(sum / 16.0, 0.0);
}

fn srgb_to_linear(value: vec3<f32>) -> vec3<f32> {
    return select(value / 12.92, pow((value + 0.055) / 1.055, vec3<f32>(2.4)), value >= vec3<f32>(0.04045));
}

fn linear_to_srgb(value: vec3<f32>) -> vec3<f32> {
    return select(value * 12.92, 1.055 * pow(value, vec3<f32>(1.0 / 2.4)) - 0.055, value >= vec3<f32>(0.0031308));
}

fn tone_map(energy: vec3<f32>) -> vec3<f32> {
    let peak = max(max(energy.r, energy.g), energy.b);
    let toned = 1.0 - exp(-peak);
    let mapped = energy * (toned / max(peak, 0.000001));
    return select(mix(mapped, vec3<f32>(toned), smoothstep(HIGHLIGHT_START, HIGHLIGHT_END, peak) * HIGHLIGHT_MIX),
        vec3<f32>(0.0), peak <= 0.000001);
}

// Four bilinear samples evaluate the reference's separable bicubic B-spline without bilinear contour kinks.
fn bicubic_field(uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(textureDimensions(field));
    let position = uv * size - 0.5;
    let cell = floor(position);
    let fraction = position - cell;
    let squared = fraction * fraction;
    let cubed = squared * fraction;
    let weight0 = (1.0 - 3.0 * fraction + 3.0 * squared - cubed) / 6.0;
    let weight1 = (4.0 - 6.0 * squared + 3.0 * cubed) / 6.0;
    let weight2 = (1.0 + 3.0 * fraction + 3.0 * squared - 3.0 * cubed) / 6.0;
    let weight3 = cubed / 6.0;
    let group0 = weight0 + weight1;
    let group1 = weight2 + weight3;
    let coordinate0 = (cell - 0.5 + weight1 / group0) / size;
    let coordinate1 = (cell + 1.5 + weight3 / group1) / size;
    return group0.y * (
        group0.x * textureSampleLevel(field, linear_sampler, vec2<f32>(coordinate0.x, coordinate0.y), 0.0).rgb
        + group1.x * textureSampleLevel(field, linear_sampler, vec2<f32>(coordinate1.x, coordinate0.y), 0.0).rgb
    ) + group1.y * (
        group0.x * textureSampleLevel(field, linear_sampler, vec2<f32>(coordinate0.x, coordinate1.y), 0.0).rgb
        + group1.x * textureSampleLevel(field, linear_sampler, vec2<f32>(coordinate1.x, coordinate1.y), 0.0).rgb
    );
}

// The copied quarter-size field excludes the broad energy added by the glow upsample passes.
fn solid_layers(uv: vec2<f32>) -> vec3<f32> {
    let energy = EXPOSURE * SOLID_FIELD_GAIN * max(bicubic_field(uv), vec3<f32>(0.0));
    let peak = max(max(energy.r, energy.g), energy.b);
    let hue = energy / max(peak, 0.000001);
    let density = 1.0 - exp(-peak);
    // Keep derivatives outside signal-dependent control flow, including an entirely silent field.
    let field_step = max(fwidth(density), 0.00001);
    let outline_width = SOLID_OUTLINE_PIXELS * parameters.display_scale;
    var result = vec3<f32>(0.0);
    for (var layer = 0u; layer < 4u; layer += 1u) {
        let level = SOLID_LAYER_LEVELS[layer];
        let inside = smoothstep(level - field_step, level + field_step, density);
        let shade = 0.35 + 0.65 * f32(layer) / 3.0;
        result = mix(result, hue * shade, inside * SOLID_LAYER_ALPHAS[layer]);
        let distance = abs(density - level) / field_step;
        let line = 1.0 - smoothstep(outline_width * 0.5 - 0.5, outline_width * 0.5 + 0.5, distance);
        result += hue * line * SOLID_OUTLINE_STRENGTHS[layer];
    }
    return select(result, vec3<f32>(0.0), peak <= 0.000001);
}

@fragment
fn composite_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    let sharp_energy = textureSample(source, linear_sampler, input.uv).rgb;
    let glow_energy = GLOW_PER_LEVEL * textureSample(bloom, linear_sampler, input.uv).rgb;
    // Exposure also controls highlight whitening, not only the exponential tone curve.
    var mapped = tone_map(EXPOSURE * (sharp_energy + glow_energy));
    if parameters.solid_rendering > 0.5 {
        let sharp = tone_map(EXPOSURE * sharp_energy);
        let sharp_peak = max(max(sharp.r, sharp.g), sharp.b);
        let glow = tone_map(EXPOSURE * SOLID_GLOW_SHARE * glow_energy);
        let layers = solid_layers(input.uv);
        mapped = min(layers + glow + sharp * smoothstep(SOLID_HIGHLIGHT_START, SOLID_HIGHLIGHT_END, sharp_peak), vec3<f32>(1.0));
    }
    let background = srgb_to_linear(vec3<f32>(5.0, 6.0, 8.0) / 255.0);
    let linear = clamp(background + mapped * (vec3<f32>(1.0) - background), vec3<f32>(0.0), vec3<f32>(1.0));
    let noise = fract(sin(dot(input.position.xy, vec2<f32>(12.9898, 78.233))) * 43758.5453);
    let encoded = linear_to_srgb(linear) + (noise - 0.5) / 255.0;
    let half_size = parameters.size * 0.5;
    let radius = CORNER_RADIUS_PIXELS * parameters.display_scale;
    let outside = abs(input.uv * parameters.size - half_size) - half_size + radius;
    let distance = length(max(outside, vec2<f32>(0.0))) + min(max(outside.x, outside.y), 0.0) - radius;
    return vec4<f32>(select(srgb_to_linear(encoded), encoded, parameters.encode_output > 0.5), clamp(0.5 - distance, 0.0, 1.0));
}
