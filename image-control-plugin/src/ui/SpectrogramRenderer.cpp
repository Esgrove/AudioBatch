/// Implements SpectrogramRenderer: the shaders, the float render targets and the glow chain,
/// the hand-off of particles from the analysis worker, and the passes of one frame:
/// fade, particles, glow, and the tone-mapped composite into the plot.
/// The particles add up in linear light, and the composite encodes the result as sRGB for the display.

#include "ui/SpectrogramRenderer.h"

#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Sharp core of the loudest particles, in logical pixels, so dots look the same size on every display scale.
/// Quieter particles are smaller, down to the builder's smallest particle size.
constexpr float CORE_SIGMA_PIXELS {1.3F};
/// Widest horizontal spread of a particle, in logical pixels, which bounds the widest low-frequency bins,
/// so a large window does not smear the bass into wide blurs.
constexpr float MAX_SIGMA_PIXELS {6.0F};
/// Spread of the faint halo around the loudest particles, in logical pixels,
/// so neighbouring dots join softly without losing their sharp cores.
constexpr float HALO_SIGMA_PIXELS {4.0F};
/// Brightness of the halo of the most emphasised particles relative to the core.
/// The halo scales with the particle's emphasis, so faint particles draw only their cores instead of a haze.
constexpr float HALO_WEIGHT {0.2F};
/// How many spreads a sprite reaches before it is cut off.
constexpr float SPRITE_REACH_SIGMAS {3.0F};
/// Scale from the accumulated particle energy to the tone curve.
constexpr float EXPOSURE {1.2F};
/// Strength of the glow relative to the sharp particles, for a soft bloom around dense content.
constexpr float GLOW_STRENGTH {0.8F};
/// Exposed energy where the brightest colours start to lighten towards white.
constexpr float HIGHLIGHT_START {2.0F};
/// Exposed energy where the lightening towards white is complete.
constexpr float HIGHLIGHT_END {8.0F};
/// Share of white mixed into the brightest colours, so dense clusters get white-hot cores.
/// Single dots stay coloured.
constexpr float HIGHLIGHT_MIX {0.6F};
/// Spread of the tent filter of the glow upsampling, in source texels.
constexpr float UPSAMPLE_RADIUS {1.0F};
/// Most particles kept waiting for the OpenGL thread.
/// When it stalls, such as while the window is hidden, older particles are dropped, since they would have faded.
constexpr std::size_t MAX_PENDING_PARTICLES {1'000'000};
/// Longest pending fade, in seconds. A longer fade leaves nothing visible anyway.
constexpr double MAX_PENDING_FADE_SECONDS {10.0};
/// Floats per particle instance, matching SpectrogramParticle.
constexpr int PARTICLE_FLOATS {8};
/// Glow chain level the solid mode draws its layers from: a quarter of the plot size,
/// smooth enough for calm outlines while still following the shapes of the particles.
/// The field copies the level before the upsampling adds the smaller levels into it,
/// since their wide blurs would spread a narrow mono line into wide layers.
constexpr std::size_t SOLID_FIELD_LEVEL {1};
/// Scale from the glow chain level to the exposed density of the solid layers.
/// The blur spreads the particles' energy, so the field needs more gain than the sharp picture.
constexpr float SOLID_FIELD_GAIN {1.5F};
/// Tone-mapped density from 0 to 1 at which each solid layer starts, from the outermost to the innermost.
constexpr std::array<float, 4> SOLID_LAYER_LEVELS {0.06F, 0.18F, 0.38F, 0.62F};
/// Opacity of each solid layer, from the outermost to the innermost,
/// so weak content draws faint shapes and strong content dense stacked solids.
constexpr std::array<float, 4> SOLID_LAYER_ALPHAS {0.16F, 0.26F, 0.38F, 0.52F};
/// Brightness of the outline of each solid layer, from the outermost to the innermost.
constexpr std::array<float, 4> SOLID_OUTLINE_STRENGTHS {0.25F, 0.4F, 0.6F, 0.85F};
/// Width of the solid layer outlines, in logical pixels.
constexpr float SOLID_OUTLINE_PIXELS {1.25F};
/// Tone-mapped brightness of the sharp particles where they start and finish showing over the solid layers,
/// so only the strongest content draws as bright traces on top.
constexpr float SOLID_HIGHLIGHT_START {0.35F};
/// Tone-mapped brightness of the sharp particles where they show fully over the solid layers.
constexpr float SOLID_HIGHLIGHT_END {0.75F};
/// Strength of the glow in the solid mode relative to the particle mode.
constexpr float SOLID_GLOW_SHARE {0.5F};
#if JUCE_DEBUG
/// Nanoseconds in a millisecond, for the GPU timer.
constexpr double NANOSECONDS_PER_MILLISECOND {1.0e6};
#endif
/// Background of the plot, as ARGB.
constexpr juce::uint32 BACKGROUND {0xff050608};

static_assert(sizeof(SpectrogramParticle) == sizeof(float) * PARTICLE_FLOATS);

/// Makes a triangle that covers the viewport from the vertex index, with texture coordinates from 0 to 1.
constexpr auto SCREEN_VERTEX_SHADER = R"(#version 330 core
out vec2 uv;
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

/// Outputs white, which the blend function turns into a multiplication of the target by the blend colour.
constexpr auto FADE_FRAGMENT_SHADER = R"(#version 330 core
out vec4 colour;
void main()
{
    colour = vec4(1.0);
}
)";

/// Places a sprite around a particle: the centre from its plot shares, the spread from its bin spacing,
/// never narrower than the sharp core, and the size from the wider of the core and the halo.
/// The core and the halo scale with the particle's size, so louder content draws larger dots,
/// and the halo's brightness scales with the particle's emphasis.
constexpr auto PARTICLE_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 placement;
layout(location = 2) in vec3 tint;
layout(location = 3) in float particleSize;
layout(location = 4) in float particleEmphasis;
uniform vec2 targetSize;
uniform float coreSigma;
uniform float maxSigma;
uniform float haloSigma;
uniform float reach;
out vec2 offset;
flat out vec2 core;
flat out vec2 halo;
flat out vec3 particleColour;
flat out float emphasis;
void main()
{
    vec2 centre = vec2(placement.x, 1.0 - placement.y) * targetSize;
    emphasis = particleEmphasis;
    float sizedCore = coreSigma * particleSize;
    float sizedHalo = haloSigma * particleSize;
    core = vec2(clamp(placement.z * targetSize.x, sizedCore, maxSigma), sizedCore);
    halo = vec2(max(core.x, sizedHalo), sizedHalo);
    offset = corner * halo * reach;
    particleColour = tint;
    gl_Position = vec4((centre + offset) / targetSize * 2.0 - 1.0, 0.0, 1.0);
}
)";

/// A Gaussian core with a faint wider halo, evaluated at the exact sub-pixel offset from the centre.
constexpr auto PARTICLE_FRAGMENT_SHADER = R"(#version 330 core
in vec2 offset;
flat in vec2 core;
flat in vec2 halo;
flat in vec3 particleColour;
flat in float emphasis;
uniform float haloWeight;
out vec4 colour;
void main()
{
    vec2 inCore = offset / core;
    vec2 inHalo = offset / halo;
    float weight = exp(-0.5 * dot(inCore, inCore)) + haloWeight * emphasis * exp(-0.5 * dot(inHalo, inHalo));
    colour = vec4(particleColour * weight, 0.0);
}
)";

/// Halves a picture with the 13-tap filter of the Call of Duty bloom, which does not flicker as particles move.
constexpr auto DOWNSAMPLE_FRAGMENT_SHADER = R"(#version 330 core
in vec2 uv;
uniform sampler2D source;
uniform vec2 sourceTexel;
out vec4 colour;
vec3 tap(float x, float y)
{
    return texture(source, uv + vec2(x, y) * sourceTexel).rgb;
}
void main()
{
    vec3 outer = (tap(-2.0, 2.0) + tap(2.0, 2.0) + tap(-2.0, -2.0) + tap(2.0, -2.0)) * 0.03125;
    vec3 edges = (tap(0.0, 2.0) + tap(-2.0, 0.0) + tap(2.0, 0.0) + tap(0.0, -2.0)) * 0.0625;
    vec3 inner = (tap(-1.0, 1.0) + tap(1.0, 1.0) + tap(-1.0, -1.0) + tap(1.0, -1.0)) * 0.125;
    colour = vec4(tap(0.0, 0.0) * 0.125 + outer + edges + inner, 1.0);
}
)";

/// Doubles a picture with a 3×3 tent filter. The blend function adds it to the next larger level.
constexpr auto UPSAMPLE_FRAGMENT_SHADER = R"(#version 330 core
in vec2 uv;
uniform sampler2D source;
uniform vec2 sourceTexel;
uniform float radius;
out vec4 colour;
vec3 tap(float x, float y)
{
    return texture(source, uv + vec2(x, y) * sourceTexel * radius).rgb;
}
void main()
{
    vec3 sum = tap(0.0, 0.0) * 4.0;
    sum += (tap(0.0, 1.0) + tap(-1.0, 0.0) + tap(1.0, 0.0) + tap(0.0, -1.0)) * 2.0;
    sum += tap(-1.0, 1.0) + tap(1.0, 1.0) + tap(-1.0, -1.0) + tap(1.0, -1.0);
    colour = vec4(sum / 16.0, 1.0);
}
)";

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
constexpr auto COMPOSITE_FRAGMENT_SHADER = R"(#version 330 core
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
    if (peak <= 1.0e-6)
    {
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
    if (fieldPeak <= 1.0e-6)
    {
        return vec3(0.0);
    }
    vec3 hue = fieldEnergy / fieldPeak;
    float density = 1.0 - exp(-fieldPeak);
    float fieldStep = max(fwidth(density), 1.0e-5);
    vec3 result = vec3(0.0);
    for (int layer = 0; layer < 4; ++layer)
    {
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
    if (solid > 0.5)
    {
        vec3 sharp = toneMap(exposure * texture(accumulation, uv).rgb);
        float sharpPeak = max(max(sharp.r, sharp.g), sharp.b);
        vec3 glow = toneMap(exposure * glowStrength * texture(bloom, uv).rgb);
        vec3 layers = solidLayers(uv);
        mapped = layers + glow + sharp * smoothstep(solidHighlight.x, solidHighlight.y, sharpPeak);
        mapped = min(mapped, vec3(1.0));
    }
    else
    {
        mapped = toneMap(exposure * (texture(accumulation, uv).rgb + glowStrength * texture(bloom, uv).rgb));
    }
    vec3 linearBackground = srgbToLinear(background);
    vec3 result = linearToSrgb(clamp(linearBackground + mapped * (1.0 - linearBackground), 0.0, 1.0));
    result += (noise(gl_FragCoord.xy) - 0.5) / 255.0;
    float distance = roundedBoxDistance(local - plotSize * 0.5, plotSize * 0.5, cornerRadius);
    colour = vec4(result, clamp(0.5 - distance, 0.0, 1.0));
}
)";

/// Returns a byte offset into a vertex buffer in the form glVertexAttribPointer takes.
const void* bufferOffset(const std::size_t bytes)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    return reinterpret_cast<const void*>(bytes);
}

/// Compiles and links a shader program, or returns null and logs why it failed.
std::unique_ptr<juce::OpenGLShaderProgram> buildProgram(
    const juce::OpenGLContext& context,
    const char* vertexShader,
    const char* fragmentShader
)
{
    auto program = std::make_unique<juce::OpenGLShaderProgram>(context);
    if (!program->addVertexShader(vertexShader) || !program->addFragmentShader(fragmentShader) || !program->link()) {
        DBG("[ERROR] Spectrogram shader failed: " << program->getLastError());
        return nullptr;
    }
    return program;
}

/// Accumulation format: full float precision keeps long, faint fades free of banding.
/// Apple GPUs blend half floats natively, so macOS uses those.
GLint accumulationFormat()
{
#if JUCE_MAC
    return static_cast<GLint>(juce::gl::GL_RGBA16F);
#else
    return static_cast<GLint>(juce::gl::GL_RGBA32F);
#endif
}

/// Frees a render target's framebuffer and texture.
void releaseTarget(SpectrogramRenderTarget& target)
{
    if (target.framebuffer != 0) {
        juce::gl::glDeleteFramebuffers(1, &target.framebuffer);
    }
    if (target.texture != 0) {
        juce::gl::glDeleteTextures(1, &target.texture);
    }
    target = {};
}

/// Creates a cleared float render target with linear filtering and clamped edges.
/// Returns false when the framebuffer is not complete.
bool createTarget(SpectrogramRenderTarget& target, const int width, const int height, const GLint format)
{
    releaseTarget(target);
    target.width = width;
    target.height = height;
    juce::gl::glGenTextures(1, &target.texture);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, target.texture);
    juce::gl::glTexImage2D(
        juce::gl::GL_TEXTURE_2D, 0, format, width, height, 0, juce::gl::GL_RGBA, juce::gl::GL_FLOAT, nullptr
    );
    juce::gl::glTexParameteri(
        juce::gl::GL_TEXTURE_2D, juce::gl::GL_TEXTURE_MIN_FILTER, static_cast<GLint>(juce::gl::GL_LINEAR)
    );
    juce::gl::glTexParameteri(
        juce::gl::GL_TEXTURE_2D, juce::gl::GL_TEXTURE_MAG_FILTER, static_cast<GLint>(juce::gl::GL_LINEAR)
    );
    juce::gl::glTexParameteri(
        juce::gl::GL_TEXTURE_2D, juce::gl::GL_TEXTURE_WRAP_S, static_cast<GLint>(juce::gl::GL_CLAMP_TO_EDGE)
    );
    juce::gl::glTexParameteri(
        juce::gl::GL_TEXTURE_2D, juce::gl::GL_TEXTURE_WRAP_T, static_cast<GLint>(juce::gl::GL_CLAMP_TO_EDGE)
    );
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);

    juce::gl::glGenFramebuffers(1, &target.framebuffer);
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, target.framebuffer);
    juce::gl::glFramebufferTexture2D(
        juce::gl::GL_FRAMEBUFFER, juce::gl::GL_COLOR_ATTACHMENT0, juce::gl::GL_TEXTURE_2D, target.texture, 0
    );
    const bool complete
        = juce::gl::glCheckFramebufferStatus(juce::gl::GL_FRAMEBUFFER) == juce::gl::GL_FRAMEBUFFER_COMPLETE;
    juce::gl::glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT);
    return complete;
}

/// Binds a render target for drawing, with a viewport over all of it.
void bindTarget(const SpectrogramRenderTarget& target)
{
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, target.framebuffer);
    juce::gl::glViewport(0, 0, target.width, target.height);
}

/// Binds a render target's texture to texture unit 0 for sampling.
void bindTexture(const SpectrogramRenderTarget& target, const GLenum unit = juce::gl::GL_TEXTURE0)
{
    juce::gl::glActiveTexture(unit);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, target.texture);
}
}  // namespace

/// Creates the renderer for a context. The GL resources are created when the context starts.
SpectrogramRenderer::SpectrogramRenderer(juce::OpenGLContext& context) : openGLContext(context) {}

/// The context is detached before the renderer goes away, which already released the GL resources.
SpectrogramRenderer::~SpectrogramRenderer() = default;

/// Returns the GPU time of a recent frame in milliseconds, when it is measured.
std::optional<double> SpectrogramRenderer::gpuFrameMilliseconds() const
{
    const double milliseconds = gpuMilliseconds.load(std::memory_order_relaxed);
    return milliseconds < 0.0 ? std::nullopt : std::optional<double>(milliseconds);
}

/// Returns whether the shaders or framebuffers could not be created, such as without OpenGL 3.3.
bool SpectrogramRenderer::isUnavailable() const noexcept
{
    return unavailable.load(std::memory_order_relaxed);
}

/// Returns how many frames have been drawn, for the frame rate readout.
int SpectrogramRenderer::renderedFrameCount() const noexcept
{
    return renderedFrames.load(std::memory_order_relaxed);
}

/// Returns how many particles wait for the next frame, for the tests of the analysis worker.
std::size_t SpectrogramRenderer::pendingParticleCount()
{
    const std::scoped_lock lock(pendingMutex);
    return pending.particles.size();
}

/// Hands the time that passed, the persistence, the plot's place in the editor, and what to draw to the OpenGL thread.
/// Called on the message thread once per display refresh.
/// Fade times add up until the next frame takes them.
void SpectrogramRenderer::submitFrame(
    const double fadeSeconds,
    const double persistenceSeconds,
    const juce::Rectangle<float> plotInEditor,
    const SpectrogramRenderMode mode
)
{
    const std::scoped_lock lock(pendingMutex);
    pending.fadeSeconds = std::min(pending.fadeSeconds + fadeSeconds, MAX_PENDING_FADE_SECONDS);
    pending.persistenceSeconds = std::clamp(persistenceSeconds, MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS);
    pending.plotInEditor = plotInEditor;
    pending.mode = mode;
}

/// Hands new particles to the OpenGL thread. Called on the analysis worker thread.
/// Particles add up until the next frame takes them, and clearing first drops the picture
/// and the particles that were still waiting, which belong to what came before the clear.
void SpectrogramRenderer::submitParticles(const std::span<const SpectrogramParticle> particles, const bool clear)
{
    const std::scoped_lock lock(pendingMutex);
    if (clear) {
        pending.particles.clear();
        pending.clear = true;
    }
    if (pending.particles.size() + particles.size() > MAX_PENDING_PARTICLES) {
        pending.particles.clear();
    }
    pending.particles.insert(pending.particles.end(), particles.begin(), particles.end());
}

/// Creates the shaders, the vertex arrays, and the timer queries when the context starts.
/// The render targets follow the plot size, so the first frame creates them.
void SpectrogramRenderer::newOpenGLContextCreated()
{
    if (!createPrograms()) {
        unavailable = true;
        return;
    }

    constexpr std::array<float, 8> corners {-1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F};
    juce::gl::glGenVertexArrays(1, &particleVertexArray);
    juce::gl::glGenVertexArrays(1, &screenVertexArray);
    juce::gl::glGenBuffers(1, &cornerBuffer);
    juce::gl::glGenBuffers(1, &instanceBuffer);

    juce::gl::glBindVertexArray(particleVertexArray);
    juce::gl::glBindBuffer(juce::gl::GL_ARRAY_BUFFER, cornerBuffer);
    juce::gl::glBufferData(
        juce::gl::GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(corners)), corners.data(), juce::gl::GL_STATIC_DRAW
    );
    juce::gl::glEnableVertexAttribArray(0);
    juce::gl::glVertexAttribPointer(0, 2, juce::gl::GL_FLOAT, juce::gl::GL_FALSE, 0, nullptr);

    juce::gl::glBindBuffer(juce::gl::GL_ARRAY_BUFFER, instanceBuffer);
    constexpr auto stride = static_cast<GLsizei>(sizeof(SpectrogramParticle));
    juce::gl::glEnableVertexAttribArray(1);
    juce::gl::glVertexAttribPointer(
        1, 3, juce::gl::GL_FLOAT, juce::gl::GL_FALSE, stride, bufferOffset(offsetof(SpectrogramParticle, x))
    );
    juce::gl::glVertexAttribDivisor(1, 1);
    juce::gl::glEnableVertexAttribArray(2);
    juce::gl::glVertexAttribPointer(
        2, 3, juce::gl::GL_FLOAT, juce::gl::GL_FALSE, stride, bufferOffset(offsetof(SpectrogramParticle, red))
    );
    juce::gl::glVertexAttribDivisor(2, 1);
    juce::gl::glEnableVertexAttribArray(3);
    juce::gl::glVertexAttribPointer(
        3, 1, juce::gl::GL_FLOAT, juce::gl::GL_FALSE, stride, bufferOffset(offsetof(SpectrogramParticle, size))
    );
    juce::gl::glVertexAttribDivisor(3, 1);
    juce::gl::glEnableVertexAttribArray(4);
    juce::gl::glVertexAttribPointer(
        4, 1, juce::gl::GL_FLOAT, juce::gl::GL_FALSE, stride, bufferOffset(offsetof(SpectrogramParticle, emphasis))
    );
    juce::gl::glVertexAttribDivisor(4, 1);
    juce::gl::glBindVertexArray(0);
    juce::gl::glBindBuffer(juce::gl::GL_ARRAY_BUFFER, 0);

#if JUCE_DEBUG
    juce::gl::glGenQueries(static_cast<GLsizei>(timerQueries.size()), timerQueries.data());
#endif
    ready = true;
}

/// Frees every GL resource while the context is still current.
void SpectrogramRenderer::openGLContextClosing()
{
    releaseResources();
}

/// Draws one frame: takes the pending work, fades the picture, adds the new particles, builds the glow,
/// and composites the picture into the plot. JUCE paints the editor's components over it afterwards.
void SpectrogramRenderer::renderOpenGL()
{
    renderedFrames.fetch_add(1, std::memory_order_relaxed);

    SpectrogramPendingWork work;
    {
        const std::scoped_lock lock(pendingMutex);
        drawing.clear();
        std::swap(drawing, pending.particles);
        work.fadeSeconds = std::exchange(pending.fadeSeconds, 0.0);
        work.persistenceSeconds = pending.persistenceSeconds;
        work.clear = std::exchange(pending.clear, false);
        work.plotInEditor = pending.plotInEditor;
        work.mode = pending.mode;
    }

    GLint defaultFramebuffer {0};
    juce::gl::glGetIntegerv(juce::gl::GL_FRAMEBUFFER_BINDING, &defaultFramebuffer);
    std::array<GLint, 4> viewport {};
    juce::gl::glGetIntegerv(juce::gl::GL_VIEWPORT, viewport.data());

    const auto background = juce::Colour(palette::GREY_SEMI_DARK);
    juce::gl::glDisable(juce::gl::GL_SCISSOR_TEST);
    juce::gl::glDisable(juce::gl::GL_DEPTH_TEST);
    juce::gl::glClearColor(background.getFloatRed(), background.getFloatGreen(), background.getFloatBlue(), 1.0F);
    juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT);

    const auto scale = static_cast<float>(openGLContext.getRenderingScale());
    const auto plot = (work.plotInEditor * scale).getSmallestIntegerContainer();
    if (!ready || work.mode == SpectrogramRenderMode::hidden || plot.isEmpty()) {
        return;
    }
    if (!resizeTargets(plot.getWidth(), plot.getHeight())) {
        unavailable = true;
        releaseResources();
        return;
    }

    measureGpuTimeBegin();
    if (work.clear) {
        bindTarget(accumulation);
        juce::gl::glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        juce::gl::glClear(juce::gl::GL_COLOR_BUFFER_BIT);
    }
    const bool solid = work.mode == SpectrogramRenderMode::solid;
    const bool drawingParticles = work.mode == SpectrogramRenderMode::particles || solid;
    juce::gl::glEnable(juce::gl::GL_BLEND);
    if (drawingParticles) {
        fade(work.fadeSeconds, work.persistenceSeconds);
        drawParticles(scale);
        drawBloom(solid);
    }

    // The default framebuffer has its origin at the bottom left.
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, static_cast<GLuint>(defaultFramebuffer));
    const juce::Rectangle<int> plotViewport(
        plot.getX(), viewport[3] - plot.getBottom(), plot.getWidth(), plot.getHeight()
    );
    drawComposite(plotViewport, drawingParticles ? EXPOSURE : 0.0F, solid);
    measureGpuTimeEnd();

    juce::gl::glDisable(juce::gl::GL_BLEND);
    juce::gl::glBindVertexArray(0);
    juce::gl::glUseProgram(0);
    juce::gl::glActiveTexture(juce::gl::GL_TEXTURE2);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);
    juce::gl::glActiveTexture(juce::gl::GL_TEXTURE1);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);
    juce::gl::glActiveTexture(juce::gl::GL_TEXTURE0);
    juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);
    juce::gl::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

/// Compiles every shader program. Returns false when one fails.
bool SpectrogramRenderer::createPrograms()
{
    particleProgram = buildProgram(openGLContext, PARTICLE_VERTEX_SHADER, PARTICLE_FRAGMENT_SHADER);
    fadeProgram = buildProgram(openGLContext, SCREEN_VERTEX_SHADER, FADE_FRAGMENT_SHADER);
    downsampleProgram = buildProgram(openGLContext, SCREEN_VERTEX_SHADER, DOWNSAMPLE_FRAGMENT_SHADER);
    upsampleProgram = buildProgram(openGLContext, SCREEN_VERTEX_SHADER, UPSAMPLE_FRAGMENT_SHADER);
    compositeProgram = buildProgram(openGLContext, SCREEN_VERTEX_SHADER, COMPOSITE_FRAGMENT_SHADER);
    return particleProgram != nullptr && fadeProgram != nullptr && downsampleProgram != nullptr
        && upsampleProgram != nullptr && compositeProgram != nullptr;
}

/// Sizes the accumulation target to the plot in physical pixels and the glow chain to fractions of it.
/// The accumulated picture is scaled into the new size, so resizing the window keeps what is shown.
/// Returns false when a framebuffer cannot be created.
bool SpectrogramRenderer::resizeTargets(const int width, const int height)
{
    if (accumulation.width == width && accumulation.height == height) {
        return true;
    }
    SpectrogramRenderTarget resized;
    if (!createTarget(resized, width, height, accumulationFormat())) {
        releaseTarget(resized);
        return false;
    }
    if (accumulation.framebuffer != 0) {
        juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, accumulation.framebuffer);
        juce::gl::glBindFramebuffer(juce::gl::GL_DRAW_FRAMEBUFFER, resized.framebuffer);
        juce::gl::glBlitFramebuffer(
            0,
            0,
            accumulation.width,
            accumulation.height,
            0,
            0,
            width,
            height,
            juce::gl::GL_COLOR_BUFFER_BIT,
            juce::gl::GL_LINEAR
        );
    }
    releaseTarget(accumulation);
    accumulation = resized;

    int levelWidth = width;
    int levelHeight = height;
    for (auto& level : bloomLevels) {
        levelWidth = std::max(1, levelWidth / 2);
        levelHeight = std::max(1, levelHeight / 2);
        if (!createTarget(level, levelWidth, levelHeight, static_cast<GLint>(juce::gl::GL_RGBA16F))) {
            return false;
        }
    }
    const auto& fieldLevel = bloomLevels.at(SOLID_FIELD_LEVEL);
    return createTarget(solidField, fieldLevel.width, fieldLevel.height, static_cast<GLint>(juce::gl::GL_RGBA16F));
}

/// Builds the glow: halves the picture through the chain, then adds each level back into the one above it,
/// so the first level holds a wide, smooth blur of the particles.
/// When solid, it copies the field level into the solid field between the two,
/// so the layers have only that level's own blur.
void SpectrogramRenderer::drawBloom(const bool solid)
{
    juce::gl::glBindVertexArray(screenVertexArray);
    juce::gl::glDisable(juce::gl::GL_BLEND);
    downsampleProgram->use();
    downsampleProgram->setUniform("source", 0);
    const SpectrogramRenderTarget* source = &accumulation;
    for (const auto& level : bloomLevels) {
        bindTarget(level);
        bindTexture(*source);
        downsampleProgram->setUniform(
            "sourceTexel", 1.0F / static_cast<float>(source->width), 1.0F / static_cast<float>(source->height)
        );
        juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES, 0, 3);
        source = &level;
    }
    if (solid) {
        const auto& fieldLevel = bloomLevels.at(SOLID_FIELD_LEVEL);
        juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, fieldLevel.framebuffer);
        juce::gl::glBindFramebuffer(juce::gl::GL_DRAW_FRAMEBUFFER, solidField.framebuffer);
        juce::gl::glBlitFramebuffer(
            0,
            0,
            fieldLevel.width,
            fieldLevel.height,
            0,
            0,
            solidField.width,
            solidField.height,
            juce::gl::GL_COLOR_BUFFER_BIT,
            juce::gl::GL_NEAREST
        );
    }

    juce::gl::glEnable(juce::gl::GL_BLEND);
    juce::gl::glBlendFunc(juce::gl::GL_ONE, juce::gl::GL_ONE);
    upsampleProgram->use();
    upsampleProgram->setUniform("source", 0);
    upsampleProgram->setUniform("radius", UPSAMPLE_RADIUS);
    for (std::size_t level = bloomLevels.size() - 1; level > 0; --level) {
        const auto& smaller = bloomLevels.at(level);
        bindTarget(bloomLevels.at(level - 1));
        bindTexture(smaller);
        upsampleProgram->setUniform(
            "sourceTexel", 1.0F / static_cast<float>(smaller.width), 1.0F / static_cast<float>(smaller.height)
        );
        juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES, 0, 3);
    }
}

/// Tone maps the accumulated picture and its glow into the plot, over the plot background,
/// as particles or, when solid, as stacked layers with outlines under the strongest particles.
/// An exposure of 0 draws only the background.
void SpectrogramRenderer::drawComposite(const juce::Rectangle<int> plotViewport, const float exposure, const bool solid)
{
    juce::gl::glViewport(plotViewport.getX(), plotViewport.getY(), plotViewport.getWidth(), plotViewport.getHeight());
    juce::gl::glBindVertexArray(screenVertexArray);
    juce::gl::glEnable(juce::gl::GL_BLEND);
    juce::gl::glBlendFunc(juce::gl::GL_SRC_ALPHA, juce::gl::GL_ONE_MINUS_SRC_ALPHA);
    const auto& field = solidField;
    bindTexture(accumulation, juce::gl::GL_TEXTURE0);
    bindTexture(bloomLevels.front(), juce::gl::GL_TEXTURE1);
    bindTexture(field, juce::gl::GL_TEXTURE2);

    const auto background = juce::Colour(BACKGROUND);
    const auto scale = static_cast<float>(openGLContext.getRenderingScale());
    compositeProgram->use();
    compositeProgram->setUniform("accumulation", 0);
    compositeProgram->setUniform("bloom", 1);
    compositeProgram->setUniform(
        "plotOrigin", static_cast<float>(plotViewport.getX()), static_cast<float>(plotViewport.getY())
    );
    compositeProgram->setUniform(
        "plotSize", static_cast<float>(plotViewport.getWidth()), static_cast<float>(plotViewport.getHeight())
    );
    compositeProgram->setUniform("cornerRadius", SPECTROGRAM_PLOT_CORNER_RADIUS * scale);
    compositeProgram->setUniform("exposure", exposure);
    // The glow chain adds up one blurred copy per level, so its strength is shared between them.
    const float glowStrength = GLOW_STRENGTH / static_cast<float>(bloomLevels.size());
    compositeProgram->setUniform("glowStrength", solid ? glowStrength * SOLID_GLOW_SHARE : glowStrength);
    compositeProgram->setUniform("highlight", HIGHLIGHT_START, HIGHLIGHT_END, HIGHLIGHT_MIX);
    compositeProgram->setUniform("solid", solid ? 1.0F : 0.0F);
    compositeProgram->setUniform("field", 2);
    compositeProgram->setUniform("fieldSize", static_cast<float>(field.width), static_cast<float>(field.height));
    compositeProgram->setUniform("fieldGain", SOLID_FIELD_GAIN);
    compositeProgram->setUniform(
        "layerLevels", SOLID_LAYER_LEVELS[0], SOLID_LAYER_LEVELS[1], SOLID_LAYER_LEVELS[2], SOLID_LAYER_LEVELS[3]
    );
    compositeProgram->setUniform(
        "layerAlphas", SOLID_LAYER_ALPHAS[0], SOLID_LAYER_ALPHAS[1], SOLID_LAYER_ALPHAS[2], SOLID_LAYER_ALPHAS[3]
    );
    compositeProgram->setUniform(
        "outlineStrengths",
        SOLID_OUTLINE_STRENGTHS[0],
        SOLID_OUTLINE_STRENGTHS[1],
        SOLID_OUTLINE_STRENGTHS[2],
        SOLID_OUTLINE_STRENGTHS[3]
    );
    compositeProgram->setUniform("outlineWidth", SOLID_OUTLINE_PIXELS * scale);
    compositeProgram->setUniform("solidHighlight", SOLID_HIGHLIGHT_START, SOLID_HIGHLIGHT_END);
    compositeProgram->setUniform(
        "background", background.getFloatRed(), background.getFloatGreen(), background.getFloatBlue()
    );
    juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES, 0, 3);
}

/// Adds this frame's particles to the accumulated picture as sprites with additive blending.
/// Their sizes are in logical pixels times the display scale, so they look the same on every display.
void SpectrogramRenderer::drawParticles(const float scale)
{
    if (drawing.empty()) {
        return;
    }
    const auto bytes = static_cast<GLsizeiptr>(drawing.size() * sizeof(SpectrogramParticle));
    juce::gl::glBindBuffer(juce::gl::GL_ARRAY_BUFFER, instanceBuffer);
    // Orphaning the buffer lets the driver hand out fresh memory instead of waiting for the previous frame.
    juce::gl::glBufferData(juce::gl::GL_ARRAY_BUFFER, bytes, nullptr, juce::gl::GL_STREAM_DRAW);
    juce::gl::glBufferSubData(juce::gl::GL_ARRAY_BUFFER, 0, bytes, drawing.data());
    juce::gl::glBindBuffer(juce::gl::GL_ARRAY_BUFFER, 0);

    bindTarget(accumulation);
    juce::gl::glBlendFunc(juce::gl::GL_ONE, juce::gl::GL_ONE);
    juce::gl::glBindVertexArray(particleVertexArray);
    particleProgram->use();
    particleProgram->setUniform(
        "targetSize", static_cast<float>(accumulation.width), static_cast<float>(accumulation.height)
    );
    particleProgram->setUniform("coreSigma", CORE_SIGMA_PIXELS * scale);
    particleProgram->setUniform("maxSigma", MAX_SIGMA_PIXELS * scale);
    particleProgram->setUniform("haloSigma", HALO_SIGMA_PIXELS * scale);
    particleProgram->setUniform("reach", SPRITE_REACH_SIGMAS);
    particleProgram->setUniform("haloWeight", HALO_WEIGHT);
    juce::gl::glDrawArraysInstanced(juce::gl::GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(drawing.size()));
}

/// Fades the accumulated picture by the time that passed, with the persistence as time constant.
void SpectrogramRenderer::fade(const double seconds, const double persistenceSeconds)
{
    if (seconds <= 0.0) {
        return;
    }
    const auto factor = static_cast<float>(std::exp(-seconds / persistenceSeconds));
    bindTarget(accumulation);
    juce::gl::glBlendFunc(juce::gl::GL_ZERO, juce::gl::GL_CONSTANT_COLOR);
    juce::gl::glBlendColor(factor, factor, factor, factor);
    juce::gl::glBindVertexArray(screenVertexArray);
    fadeProgram->use();
    juce::gl::glDrawArrays(juce::gl::GL_TRIANGLES, 0, 3);
}

/// Starts timing this frame's passes on the GPU, in Debug builds.
void SpectrogramRenderer::measureGpuTimeBegin()
{
#if JUCE_DEBUG
    // Read the oldest query, which finished frames ago, before reusing it.
    const GLuint query = timerQueries.at(nextTimerQuery);
    if (startedTimerQueries == timerQueries.size()) {
        GLint available {0};
        juce::gl::glGetQueryObjectiv(query, juce::gl::GL_QUERY_RESULT_AVAILABLE, &available);
        if (available != 0) {
            GLuint64 nanoseconds {0};
            juce::gl::glGetQueryObjectui64v(query, juce::gl::GL_QUERY_RESULT, &nanoseconds);
            gpuMilliseconds = static_cast<double>(nanoseconds) / NANOSECONDS_PER_MILLISECOND;
        }
    }
    juce::gl::glBeginQuery(juce::gl::GL_TIME_ELAPSED, query);
#endif
}

/// Stops timing this frame's passes on the GPU, in Debug builds.
void SpectrogramRenderer::measureGpuTimeEnd()
{
#if JUCE_DEBUG
    juce::gl::glEndQuery(juce::gl::GL_TIME_ELAPSED);
    nextTimerQuery = (nextTimerQuery + 1) % timerQueries.size();
    startedTimerQueries = std::min(startedTimerQueries + 1, timerQueries.size());
#endif
}

/// Frees the programs, buffers, vertex arrays, render targets, and timer queries.
void SpectrogramRenderer::releaseResources()
{
    particleProgram.reset();
    fadeProgram.reset();
    downsampleProgram.reset();
    upsampleProgram.reset();
    compositeProgram.reset();
    for (auto* buffer : {&cornerBuffer, &instanceBuffer}) {
        if (*buffer != 0) {
            juce::gl::glDeleteBuffers(1, buffer);
            *buffer = 0;
        }
    }
    for (auto* vertexArray : {&particleVertexArray, &screenVertexArray}) {
        if (*vertexArray != 0) {
            juce::gl::glDeleteVertexArrays(1, vertexArray);
            *vertexArray = 0;
        }
    }
    releaseTarget(accumulation);
    for (auto& level : bloomLevels) {
        releaseTarget(level);
    }
    releaseTarget(solidField);
#if JUCE_DEBUG
    if (timerQueries.front() != 0) {
        juce::gl::glDeleteQueries(static_cast<GLsizei>(timerQueries.size()), timerQueries.data());
        timerQueries = {};
    }
    nextTimerQuery = 0;
    startedTimerQueries = 0;
#endif
    ready = false;
}
}  // namespace esgrove::image_control
