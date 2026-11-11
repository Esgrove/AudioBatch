// Persistent linear-energy spectrogram, instanced Gaussian sprites, and a short GPU bloom.
struct Parameters {
    size: vec2<f32>,
    fade: f32,
    encode_output: f32,
};

@group(0) @binding(0) var<uniform> parameters: Parameters;
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var bloom: texture_2d<f32>;
@group(1) @binding(2) var linear_sampler: sampler;

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
    let energy = textureSample(source, linear_sampler, input.uv).rgb;
    return vec4<f32>(min(energy * parameters.fade, vec3<f32>(8192.0)), 0.0);
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
    let scale = max(min(parameters.size.x / 900.0, parameters.size.y / 360.0), 0.6);
    let radius = 1.3 * max(size, 0.1) * scale;
    let core = vec2<f32>(max(radius, spread * parameters.size.x), radius);
    let halo = max(core, vec2<f32>((3.0 + emphasis * 2.0) * max(size, 0.1) * scale));
    let offset = corners[index] * halo * 3.0;
    let point = position * parameters.size + offset;
    let clip = point / parameters.size * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0);
    return ParticleVertex(vec4<f32>(clip, 0.0, 1.0), offset, core, halo, color, emphasis);
}

@fragment
fn particle_fragment(input: ParticleVertex) -> @location(0) vec4<f32> {
    let core = input.offset / input.core;
    let halo = input.offset / input.halo;
    let weight = exp(-0.5 * dot(core, core)) + 0.2 * input.emphasis * exp(-0.5 * dot(halo, halo));
    return vec4<f32>(input.color * weight * 0.12, 0.0);
}

@fragment
fn bloom_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    let texel = 2.0 / vec2<f32>(textureDimensions(source));
    var sum = vec3<f32>(0.0);
    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            let weight_x = select(1.0, 2.0, x == 0);
            let weight_y = select(1.0, 2.0, y == 0);
            sum += textureSample(source, linear_sampler, input.uv + vec2<f32>(f32(x), f32(y)) * texel).rgb
                * weight_x * weight_y;
        }
    }
    return vec4<f32>(min(sum * 0.0625, vec3<f32>(8192.0)), 0.0);
}

@fragment
fn composite_fragment(input: ScreenVertex) -> @location(0) vec4<f32> {
    var energy = max(textureSample(source, linear_sampler, input.uv).rgb, vec3<f32>(0.0));
    energy += textureSample(bloom, linear_sampler, input.uv).rgb * 0.3;
    let peak = max(max(energy.r, energy.g), energy.b);
    let toned = 1.0 - exp(-peak * 1.2);
    var mapped = energy * (toned / max(peak, 0.000001));
    mapped = mix(mapped, vec3<f32>(toned), smoothstep(2.0, 8.0, peak) * 0.6);
    let background = vec3<f32>(5.0, 6.0, 8.0) / (255.0 * 12.92);
    let linear = clamp(background + mapped * (vec3<f32>(1.0) - background), vec3<f32>(0.0), vec3<f32>(1.0));
    let encoded = select(linear * 12.92, 1.055 * pow(linear, vec3<f32>(1.0 / 2.4)) - 0.055,
        linear >= vec3<f32>(0.0031308));
    return vec4<f32>(select(linear, encoded, parameters.encode_output > 0.5), 1.0);
}
