/// Implements SpectrogramRenderer: the shaders, the float render targets and the glow chain,
/// the hand-off of particles from the message thread, and the passes of one frame:
/// fade, particles, glow, and the tone-mapped composite into the plot.

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
using namespace juce::gl;

/// Sharp core of a particle, in logical pixels, so dots look the same size on every display scale.
constexpr float CORE_SIGMA_PIXELS {0.9F};
/// Widest horizontal spread of a particle, in logical pixels, which bounds the widest low-frequency bins.
constexpr float MAX_SIGMA_PIXELS {24.0F};
/// Spread of the faint halo around every particle, in logical pixels,
/// so neighbouring dots join softly without losing their sharp cores.
constexpr float HALO_SIGMA_PIXELS {3.0F};
/// Brightness of the halo relative to the core.
constexpr float HALO_WEIGHT {0.06F};
/// How many spreads a sprite reaches before it is cut off.
constexpr float SPRITE_REACH_SIGMAS {3.0F};
/// Scale from the accumulated particle energy to the tone curve.
constexpr float EXPOSURE {1.6F};
/// Strength of the glow relative to the sharp particles. Kept low for a clear picture with a soft bloom.
constexpr float GLOW_STRENGTH {0.6F};
/// Exposed energy where the brightest colours start to lighten towards white.
constexpr float HIGHLIGHT_START {1.5F};
/// Exposed energy where the lightening towards white is complete.
constexpr float HIGHLIGHT_END {6.0F};
/// Share of white mixed into the brightest colours, so dense clusters get light cores and stay coloured.
constexpr float HIGHLIGHT_MIX {0.55F};
/// Spread of the tent filter of the glow upsampling, in source texels.
constexpr float UPSAMPLE_RADIUS {1.0F};
/// Most particles kept waiting for the OpenGL thread.
/// When it stalls, such as while the window is hidden, older particles are dropped, since they would have faded.
constexpr std::size_t MAX_PENDING_PARTICLES {1'000'000};
/// Longest pending fade, in seconds. A longer fade leaves nothing visible anyway.
constexpr double MAX_PENDING_FADE_SECONDS {10.0};
/// Floats per particle instance, matching SpectrogramParticle.
constexpr int PARTICLE_FLOATS {6};
/// Nanoseconds in a millisecond, for the GPU timer.
constexpr double NANOSECONDS_PER_MILLISECOND {1.0e6};
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
constexpr auto PARTICLE_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 placement;
layout(location = 2) in vec3 tint;
uniform vec2 targetSize;
uniform float coreSigma;
uniform float maxSigma;
uniform float haloSigma;
uniform float reach;
out vec2 offset;
flat out vec2 core;
flat out vec2 halo;
flat out vec3 particleColour;
void main()
{
    vec2 centre = vec2(placement.x, 1.0 - placement.y) * targetSize;
    core = vec2(clamp(placement.z * targetSize.x, coreSigma, maxSigma), coreSigma);
    halo = vec2(max(core.x, haloSigma), haloSigma);
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
uniform float haloWeight;
out vec4 colour;
void main()
{
    vec2 inCore = offset / core;
    vec2 inHalo = offset / halo;
    float weight = exp(-0.5 * dot(inCore, inCore)) + haloWeight * exp(-0.5 * dot(inHalo, inHalo));
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
/// The result goes over the plot background, inside rounded corners with an antialiased edge,
/// and a little noise keeps the dark gradients from banding.
constexpr auto COMPOSITE_FRAGMENT_SHADER = R"(#version 330 core
uniform sampler2D accumulation;
uniform sampler2D bloom;
uniform vec2 plotOrigin;
uniform vec2 plotSize;
uniform float cornerRadius;
uniform float exposure;
uniform float glowStrength;
uniform vec3 highlight;
uniform vec3 background;
out vec4 colour;
float roundedBoxDistance(vec2 position, vec2 halfSize, float radius)
{
    vec2 outside = abs(position) - halfSize + radius;
    return length(max(outside, 0.0)) + min(max(outside.x, outside.y), 0.0) - radius;
}
float noise(vec2 position)
{
    return fract(sin(dot(position, vec2(12.9898, 78.233))) * 43758.5453);
}
void main()
{
    vec2 local = gl_FragCoord.xy - plotOrigin;
    vec2 uv = local / plotSize;
    vec3 energy = exposure * (texture(accumulation, uv).rgb + glowStrength * texture(bloom, uv).rgb);
    float peak = max(max(energy.r, energy.g), energy.b);
    vec3 mapped = vec3(0.0);
    if (peak > 1.0e-6)
    {
        float toned = 1.0 - exp(-peak);
        mapped = energy * (toned / peak);
        mapped = mix(mapped, vec3(toned), smoothstep(highlight.x, highlight.y, peak) * highlight.z);
    }
    vec3 result = background + mapped * (1.0 - background);
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
    return static_cast<GLint>(GL_RGBA16F);
#else
    return static_cast<GLint>(GL_RGBA32F);
#endif
}

/// Frees a render target's framebuffer and texture.
void releaseTarget(SpectrogramRenderTarget& target)
{
    if (target.framebuffer != 0) {
        glDeleteFramebuffers(1, &target.framebuffer);
    }
    if (target.texture != 0) {
        glDeleteTextures(1, &target.texture);
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
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    return complete;
}

/// Binds a render target for drawing, with a viewport over all of it.
void bindTarget(const SpectrogramRenderTarget& target)
{
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, target.width, target.height);
}

/// Binds a render target's texture to texture unit 0 for sampling.
void bindTexture(const SpectrogramRenderTarget& target, const GLenum unit = GL_TEXTURE0)
{
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, target.texture);
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
    glGenVertexArrays(1, &particleVertexArray);
    glGenVertexArrays(1, &screenVertexArray);
    glGenBuffers(1, &cornerBuffer);
    glGenBuffers(1, &instanceBuffer);

    glBindVertexArray(particleVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, cornerBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(corners)), corners.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
    constexpr auto stride = static_cast<GLsizei>(sizeof(SpectrogramParticle));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, bufferOffset(offsetof(SpectrogramParticle, x)));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, bufferOffset(offsetof(SpectrogramParticle, red)));
    glVertexAttribDivisor(2, 1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

#if JUCE_DEBUG
    glGenQueries(static_cast<GLsizei>(timerQueries.size()), timerQueries.data());
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

    PendingWork work;
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
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &defaultFramebuffer);
    std::array<GLint, 4> viewport {};
    glGetIntegerv(GL_VIEWPORT, viewport.data());

    const auto background = juce::Colour(palette::GREY_SEMI_DARK);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glClearColor(background.getFloatRed(), background.getFloatGreen(), background.getFloatBlue(), 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

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
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    const bool drawingParticles = work.mode == SpectrogramRenderMode::particles;
    glEnable(GL_BLEND);
    if (drawingParticles) {
        fade(work.fadeSeconds, work.persistenceSeconds);
        drawParticles(scale);
        drawBloom();
    }

    // The default framebuffer has its origin at the bottom left.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(defaultFramebuffer));
    const juce::Rectangle<int> plotViewport(
        plot.getX(), viewport[3] - plot.getBottom(), plot.getWidth(), plot.getHeight()
    );
    drawComposite(plotViewport, drawingParticles ? EXPOSURE : 0.0F);
    measureGpuTimeEnd();

    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
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
        glBindFramebuffer(GL_READ_FRAMEBUFFER, accumulation.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resized.framebuffer);
        glBlitFramebuffer(
            0, 0, accumulation.width, accumulation.height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR
        );
    }
    releaseTarget(accumulation);
    accumulation = resized;

    int levelWidth = width;
    int levelHeight = height;
    for (auto& level : bloomLevels) {
        levelWidth = std::max(1, levelWidth / 2);
        levelHeight = std::max(1, levelHeight / 2);
        if (!createTarget(level, levelWidth, levelHeight, static_cast<GLint>(GL_RGBA16F))) {
            return false;
        }
    }
    return true;
}

/// Builds the glow: halves the picture through the chain, then adds each level back into the one above it,
/// so the first level holds a wide, smooth blur of the particles.
void SpectrogramRenderer::drawBloom()
{
    glBindVertexArray(screenVertexArray);
    glDisable(GL_BLEND);
    downsampleProgram->use();
    downsampleProgram->setUniform("source", 0);
    const SpectrogramRenderTarget* source = &accumulation;
    for (const auto& level : bloomLevels) {
        bindTarget(level);
        bindTexture(*source);
        downsampleProgram->setUniform(
            "sourceTexel", 1.0F / static_cast<float>(source->width), 1.0F / static_cast<float>(source->height)
        );
        glDrawArrays(GL_TRIANGLES, 0, 3);
        source = &level;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
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
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
}

/// Tone maps the accumulated picture and its glow into the plot, over the plot background.
/// An exposure of 0 draws only the background.
void SpectrogramRenderer::drawComposite(const juce::Rectangle<int> plotViewport, const float exposure)
{
    glViewport(plotViewport.getX(), plotViewport.getY(), plotViewport.getWidth(), plotViewport.getHeight());
    glBindVertexArray(screenVertexArray);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    bindTexture(accumulation, GL_TEXTURE0);
    bindTexture(bloomLevels.front(), GL_TEXTURE1);

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
    compositeProgram->setUniform("glowStrength", GLOW_STRENGTH / static_cast<float>(bloomLevels.size()));
    compositeProgram->setUniform("highlight", HIGHLIGHT_START, HIGHLIGHT_END, HIGHLIGHT_MIX);
    compositeProgram->setUniform(
        "background", background.getFloatRed(), background.getFloatGreen(), background.getFloatBlue()
    );
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/// Adds this frame's particles to the accumulated picture as sprites with additive blending.
/// Their sizes are in logical pixels times the display scale, so they look the same on every display.
void SpectrogramRenderer::drawParticles(const float scale)
{
    if (drawing.empty()) {
        return;
    }
    const auto bytes = static_cast<GLsizeiptr>(drawing.size() * sizeof(SpectrogramParticle));
    glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
    // Orphaning the buffer lets the driver hand out fresh memory instead of waiting for the previous frame.
    glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, drawing.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    bindTarget(accumulation);
    glBlendFunc(GL_ONE, GL_ONE);
    glBindVertexArray(particleVertexArray);
    particleProgram->use();
    particleProgram->setUniform(
        "targetSize", static_cast<float>(accumulation.width), static_cast<float>(accumulation.height)
    );
    particleProgram->setUniform("coreSigma", CORE_SIGMA_PIXELS * scale);
    particleProgram->setUniform("maxSigma", MAX_SIGMA_PIXELS * scale);
    particleProgram->setUniform("haloSigma", HALO_SIGMA_PIXELS * scale);
    particleProgram->setUniform("reach", SPRITE_REACH_SIGMAS);
    particleProgram->setUniform("haloWeight", HALO_WEIGHT);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(drawing.size()));
}

/// Fades the accumulated picture by the time that passed, with the persistence as time constant.
void SpectrogramRenderer::fade(const double seconds, const double persistenceSeconds)
{
    if (seconds <= 0.0) {
        return;
    }
    const auto factor = static_cast<float>(std::exp(-seconds / persistenceSeconds));
    bindTarget(accumulation);
    glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR);
    glBlendColor(factor, factor, factor, factor);
    glBindVertexArray(screenVertexArray);
    fadeProgram->use();
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/// Starts timing this frame's passes on the GPU, in Debug builds.
void SpectrogramRenderer::measureGpuTimeBegin()
{
#if JUCE_DEBUG
    // Read the oldest query, which finished frames ago, before reusing it.
    const GLuint query = timerQueries.at(nextTimerQuery);
    if (startedTimerQueries == timerQueries.size()) {
        GLint available {0};
        glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available != 0) {
            GLuint64 nanoseconds {0};
            glGetQueryObjectui64v(query, GL_QUERY_RESULT, &nanoseconds);
            gpuMilliseconds = static_cast<double>(nanoseconds) / NANOSECONDS_PER_MILLISECOND;
        }
    }
    glBeginQuery(GL_TIME_ELAPSED, query);
#endif
}

/// Stops timing this frame's passes on the GPU, in Debug builds.
void SpectrogramRenderer::measureGpuTimeEnd()
{
#if JUCE_DEBUG
    glEndQuery(GL_TIME_ELAPSED);
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
            glDeleteBuffers(1, buffer);
            *buffer = 0;
        }
    }
    for (auto* vertexArray : {&particleVertexArray, &screenVertexArray}) {
        if (*vertexArray != 0) {
            glDeleteVertexArrays(1, vertexArray);
            *vertexArray = 0;
        }
    }
    releaseTarget(accumulation);
    for (auto& level : bloomLevels) {
        releaseTarget(level);
    }
#if JUCE_DEBUG
    if (timerQueries.front() != 0) {
        glDeleteQueries(static_cast<GLsizei>(timerQueries.size()), timerQueries.data());
        timerQueries = {};
    }
    nextTimerQuery = 0;
    startedTimerQueries = 0;
#endif
    ready = false;
}
}  // namespace esgrove::image_control
