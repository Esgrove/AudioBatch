/// Tone maps the particles and their glow into the plot.
/// The curve maps the brightest channel with 1 − exp(−energy) and scales the others with it,
/// so colours keep their hue and saturation instead of washing out,
/// and only the brightest clusters lighten towards white.
/// The result goes over the plot background in linear light and is encoded as sRGB,
/// since the framebuffer stores the encoded values the display shows,
/// so faint particles get their full perceived brightness instead of sinking into the background.
/// It is drawn inside rounded corners with an antialiased edge,
/// and a little noise keeps the dark gradients from banding.
///
/// The solid mode reads a smooth level of the glow chain as a density field, with a bicubic B-spline sample,
/// so its contours have no bilinear kinks.
/// Each layer fills where the tone-mapped density passes its level, tinted by the field's colour,
/// and is outlined with an antialiased line one to two pixels wide.
/// The sharp particles show over the layers only where they are bright, so the strongest content draws as traces.
#version 330 core
uniform sampler2D accumulation;
uniform sampler2D bloom;
uniform sampler2D field;
uniform vec2 fieldSize;
uniform vec2 plotOrigin;
uniform vec2 plotSize;
uniform float cornerRadius;
uniform float exposure;
uniform float glowStrength;
uniform vec3 highlight;
uniform vec3 background;
uniform float solid;
uniform float fieldGain;
uniform vec4 layerLevels;
uniform vec4 layerAlphas;
uniform vec4 outlineStrengths;
uniform float outlineWidth;
uniform vec2 solidHighlight;
out vec4 colour;
vec3 bicubic(sampler2D source, vec2 uv, vec2 size)
{
    vec2 position = uv * size - 0.5;
    vec2 cell = floor(position);
    vec2 f = position - cell;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    vec2 h0 = (cell - 0.5 + w1 / g0) / size;
    vec2 h1 = (cell + 1.5 + w3 / g1) / size;
    return g0.y * (g0.x * texture(source, vec2(h0.x, h0.y)).rgb + g1.x * texture(source, vec2(h1.x, h0.y)).rgb)
        + g1.y * (g0.x * texture(source, vec2(h0.x, h1.y)).rgb + g1.x * texture(source, vec2(h1.x, h1.y)).rgb);
}
vec3 toneMap(vec3 energy)
{
    float peak = max(max(energy.r, energy.g), energy.b);
    if (peak <= 1.0e-6) {
        return vec3(0.0);
    }
    float toned = 1.0 - exp(-peak);
    vec3 mapped = energy * (toned / peak);
    return mix(mapped, vec3(toned), smoothstep(highlight.x, highlight.y, peak) * highlight.z);
}
vec3 solidLayers(vec2 uv)
{
    vec3 fieldEnergy = exposure * fieldGain * max(bicubic(field, uv, fieldSize), vec3(0.0));
    float fieldPeak = max(max(fieldEnergy.r, fieldEnergy.g), fieldEnergy.b);
    if (fieldPeak <= 1.0e-6) {
        return vec3(0.0);
    }
    vec3 hue = fieldEnergy / fieldPeak;
    float density = 1.0 - exp(-fieldPeak);
    float fieldStep = max(fwidth(density), 1.0e-5);
    vec3 result = vec3(0.0);
    for (int layer = 0; layer < 4; ++layer) {
        float level = layerLevels[layer];
        float inside = smoothstep(level - fieldStep, level + fieldStep, density);
        float shade = 0.35 + 0.65 * float(layer) / 3.0;
        result = mix(result, hue * shade, inside * layerAlphas[layer]);
        float lineDistance = abs(density - level) / fieldStep;
        float line = 1.0 - smoothstep(outlineWidth * 0.5 - 0.5, outlineWidth * 0.5 + 0.5, lineDistance);
        result += hue * line * outlineStrengths[layer];
    }
    return result;
}
float roundedBoxDistance(vec2 position, vec2 halfSize, float radius)
{
    vec2 outside = abs(position) - halfSize + radius;
    return length(max(outside, 0.0)) + min(max(outside.x, outside.y), 0.0) - radius;
}
float noise(vec2 position)
{
    return fract(sin(dot(position, vec2(12.9898, 78.233))) * 43758.5453);
}
vec3 srgbToLinear(vec3 value)
{
    return mix(value / 12.92, pow((value + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), value));
}
vec3 linearToSrgb(vec3 value)
{
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), value));
}
void main()
{
    vec2 local = gl_FragCoord.xy - plotOrigin;
    vec2 uv = local / plotSize;
    vec3 mapped;
    if (solid > 0.5) {
        vec3 sharp = toneMap(exposure * texture(accumulation, uv).rgb);
        float sharpPeak = max(max(sharp.r, sharp.g), sharp.b);
        vec3 glow = toneMap(exposure * glowStrength * texture(bloom, uv).rgb);
        vec3 layers = solidLayers(uv);
        mapped = layers + glow + sharp * smoothstep(solidHighlight.x, solidHighlight.y, sharpPeak);
        mapped = min(mapped, vec3(1.0));
    } else {
        mapped = toneMap(exposure * (texture(accumulation, uv).rgb + glowStrength * texture(bloom, uv).rgb));
    }
    vec3 linearBackground = srgbToLinear(background);
    vec3 result = linearToSrgb(clamp(linearBackground + mapped * (1.0 - linearBackground), 0.0, 1.0));
    result += (noise(gl_FragCoord.xy) - 0.5) / 255.0;
    float distance = roundedBoxDistance(local - plotSize * 0.5, plotSize * 0.5, cornerRadius);
    colour = vec4(result, clamp(0.5 - distance, 0.0, 1.0));
}
