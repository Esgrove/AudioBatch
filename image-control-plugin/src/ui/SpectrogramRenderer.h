/// Declares SpectrogramRenderer, which draws the spatial spectrogram's particles on the GPU with OpenGL.
/// The analysis worker builds the particles on its own thread, the editor sets the fade
/// and the plot on the message thread, and the renderer draws on JUCE's OpenGL thread,
/// under the editor's components, which JUCE paints over the GL picture.
/// Only this class knows OpenGL, so another GPU backend can replace it without touching the view or the worker.

#pragma once

#include "Parameters.h"
#include "ui/SpectrogramParticles.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace esgrove::image_control
{
namespace test
{
struct SpectrogramRendererTestAccess;
}

/// Number of downsampled textures in the spectrogram's glow chain.
constexpr std::size_t SPECTROGRAM_BLOOM_LEVEL_COUNT {5};
/// Most particles kept waiting for the OpenGL thread.
/// Overflow drops the waiting batches, and an oversized batch retains only its newest particles.
constexpr std::size_t SPECTROGRAM_MAX_PENDING_PARTICLES {1'000'000};
#if defined(JUCE_DEBUG) || defined(IMAGE_CONTROL_RENDER_BENCHMARK)
/// Delayed query storage. The benchmark holds a bounded batch while the editor retains its short debug ring.
#ifdef IMAGE_CONTROL_RENDER_BENCHMARK
constexpr std::size_t SPECTROGRAM_TIMER_QUERY_COUNT {32};
#else
constexpr std::size_t SPECTROGRAM_TIMER_QUERY_COUNT {4};
#endif
#endif

/// Cached locations for the particle program, valid only for that linked program and its context.
struct SpectrogramParticleUniforms {
    explicit SpectrogramParticleUniforms(const juce::OpenGLShaderProgram& program);

    /// Core spread in physical pixels, updated for the display scale.
    juce::OpenGLShaderProgram::Uniform coreSigma;
    /// Halo spread in physical pixels, updated for the display scale.
    juce::OpenGLShaderProgram::Uniform haloSigma;
    /// Constant halo brightness relative to the core.
    juce::OpenGLShaderProgram::Uniform haloWeight;
    /// Maximum horizontal spread in physical pixels, updated for the display scale.
    juce::OpenGLShaderProgram::Uniform maxSigma;
    /// Constant sprite reach in spreads.
    juce::OpenGLShaderProgram::Uniform reach;
    /// Current accumulation dimensions in physical pixels.
    juce::OpenGLShaderProgram::Uniform targetSize;
};

/// Cached locations for a downsampling pass, valid only for its linked program and context.
struct SpectrogramDownsampleUniforms {
    explicit SpectrogramDownsampleUniforms(const juce::OpenGLShaderProgram& program);

    /// Constant texture unit for the source image.
    juce::OpenGLShaderProgram::Uniform source;
    /// Inverse source dimensions, updated for each glow-chain level.
    juce::OpenGLShaderProgram::Uniform sourceTexel;
};

/// Cached locations for an upsampling pass, valid only for its linked program and context.
struct SpectrogramUpsampleUniforms {
    explicit SpectrogramUpsampleUniforms(const juce::OpenGLShaderProgram& program);

    /// Constant filter spread in source texels.
    juce::OpenGLShaderProgram::Uniform radius;
    /// Constant texture unit for the source image.
    juce::OpenGLShaderProgram::Uniform source;
    /// Inverse source dimensions, updated for each glow-chain level.
    juce::OpenGLShaderProgram::Uniform sourceTexel;
};

/// Cached locations for the composite program, valid only for that linked program and context.
struct SpectrogramCompositeUniforms {
    explicit SpectrogramCompositeUniforms(const juce::OpenGLShaderProgram& program);

    /// Constant texture unit for the accumulated particles.
    juce::OpenGLShaderProgram::Uniform accumulation;
    /// Constant texture unit for the glow.
    juce::OpenGLShaderProgram::Uniform bloom;
    /// Constant texture unit for the solid density field.
    juce::OpenGLShaderProgram::Uniform field;

    /// Plot corner radius in physical pixels, updated for the display scale.
    juce::OpenGLShaderProgram::Uniform cornerRadius;
    /// Current solid density field dimensions.
    juce::OpenGLShaderProgram::Uniform fieldSize;
    /// Outline width in physical pixels, updated for the display scale.
    juce::OpenGLShaderProgram::Uniform outlineWidth;
    /// Plot origin in the current framebuffer.
    juce::OpenGLShaderProgram::Uniform plotOrigin;
    /// Plot dimensions in physical pixels.
    juce::OpenGLShaderProgram::Uniform plotSize;

    /// Particle exposure, zero when rendering only the background.
    juce::OpenGLShaderProgram::Uniform exposure;
    /// Glow strength, updated for the render mode.
    juce::OpenGLShaderProgram::Uniform glowStrength;
    /// Whether solid layers are selected for this pass.
    juce::OpenGLShaderProgram::Uniform solid;

    /// Constant plot background colour.
    juce::OpenGLShaderProgram::Uniform background;
    /// Constant solid density gain.
    juce::OpenGLShaderProgram::Uniform fieldGain;
    /// Constant highlight colour curve.
    juce::OpenGLShaderProgram::Uniform highlight;
    /// Constant brightness thresholds for sharp traces over the solid layers.
    juce::OpenGLShaderProgram::Uniform solidHighlight;

    /// Constant layer opacities.
    juce::OpenGLShaderProgram::Uniform layerAlphas;
    /// Constant layer density thresholds.
    juce::OpenGLShaderProgram::Uniform layerLevels;
    /// Constant layer outline brightnesses.
    juce::OpenGLShaderProgram::Uniform outlineStrengths;
};

/// What the renderer draws in the plot.
enum class SpectrogramRenderMode : std::uint8_t {
    /// The particles over the plot background.
    particles,
    /// The particles as stacked translucent layers with outlines, with the strongest content as bright traces on top.
    solid,
    /// Only the plot background, while rendering is bypassed.
    background,
    /// Nothing, while the spectrogram is hidden.
    hidden,
};

/// A texture with a framebuffer that renders into it.
struct SpectrogramRenderTarget {
    GLuint framebuffer {0};
    GLuint texture {0};
    int width {0};
    int height {0};
};

/// The work submitted since the last frame, guarded by the renderer's pending-work mutex.
struct SpectrogramPendingWork {
    /// Plot rectangle in logical editor coordinates.
    juce::Rectangle<float> plotInEditor;
    /// Particle or background rendering selected by the editor.
    SpectrogramRenderMode mode {SpectrogramRenderMode::hidden};
    /// Bounded particles accumulated since the previous render.
    std::vector<SpectrogramParticle> particles;
    /// Accumulated elapsed fade time in seconds.
    double fadeSeconds {0.0};
    /// Time constant of the particle fade in seconds.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Whether accumulated graphics must be cleared before drawing.
    bool clear {false};
};

#ifdef IMAGE_CONTROL_RENDER_BENCHMARK
/// Native device identity queried only while the benchmark's real context is current.
struct SpectrogramBenchmarkDevice {
    juce::String gpu;
    juce::String vendor;
    juce::String driver;
};

/// One asynchronously resolved elapsed-time query, indexed within the benchmark phase.
struct SpectrogramBenchmarkGpuSample {
    std::size_t frameIndex {0};
    double milliseconds {0.0};
};
#endif

/// Draws the spectrogram particles with OpenGL.
///
/// Every frame it fades the accumulated picture by the time that passed.
/// It adds the new particles as instanced Gaussian sprites with additive blending.
/// The sprites go into a float framebuffer at the plot's full physical resolution,
/// which keeps them sharp on any display.
/// It then builds a soft glow from a chain of downsampled copies,
/// and tone maps the picture into the plot with rounded corners.
/// Solid mode draws a smooth glow-chain level as stacked translucent layers with outlines.
/// Only the strongest particles remain as bright traces on top.
///
/// submitParticles runs on the analysis worker thread, submitFrame on the message thread,
/// and the OpenGLRenderer callbacks on JUCE's OpenGL thread.
/// They share only the pending work behind a mutex and a few atomics.
class SpectrogramRenderer final : public juce::OpenGLRenderer
{
public:
    explicit SpectrogramRenderer(juce::OpenGLContext& context);

    ~SpectrogramRenderer() override;

    SpectrogramRenderer(const SpectrogramRenderer&) = delete;
    SpectrogramRenderer(SpectrogramRenderer&&) = delete;
    SpectrogramRenderer& operator=(const SpectrogramRenderer&) = delete;
    SpectrogramRenderer& operator=(SpectrogramRenderer&&) = delete;

#ifdef IMAGE_CONTROL_RENDER_BENCHMARK
    [[nodiscard]] static SpectrogramBenchmarkDevice benchmarkDevice();
#endif

    [[nodiscard]] std::optional<double> gpuFrameMilliseconds() const;
    [[nodiscard]] bool isUnavailable() const noexcept;
    [[nodiscard]] std::size_t pendingParticleCount();
    [[nodiscard]] int renderedFrameCount() const noexcept;

    void submitFrame(
        double fadeSeconds,
        double persistenceSeconds,
        juce::Rectangle<float> plotInEditor,
        SpectrogramRenderMode mode
    );
    void submitParticles(std::span<const SpectrogramParticle> particles, bool clear);

#ifdef IMAGE_CONTROL_RENDER_BENCHMARK
    [[nodiscard]] std::vector<SpectrogramBenchmarkGpuSample> takeBenchmarkGpuSamples();
    [[nodiscard]] bool drawBenchmarkFrame();
    [[nodiscard]] bool finishBenchmarkBatch();
    [[nodiscard]] bool prepareBenchmark(int physicalWidth, int physicalHeight, float scale, bool gpuTiming);
#endif

    void newOpenGLContextCreated() override;
    void openGLContextClosing() override;
    void renderOpenGL() override;

private:
    friend struct test::SpectrogramRendererTestAccess;

    [[nodiscard]] bool createPrograms();
    [[nodiscard]] bool resizeTargets(int width, int height);
    [[nodiscard]] bool renderFrame(float scale, std::optional<juce::Rectangle<int>> physicalPlot);

    void drawBloom(bool solid);
    void drawComposite(juce::Rectangle<int> plotViewport, float exposure, bool solid, float scale);
    void drawParticles(float scale);
    void fade(double seconds, double persistenceSeconds);
    void measureGpuTimeBegin();
    void measureGpuTimeEnd();
    void releaseResources();

    /// The context this renderer is attached to, owned by the editor.
    juce::OpenGLContext& openGLContext;

    /// Guards pending.
    std::mutex pendingMutex;
    /// Frame settings and analysis-worker particles submitted but not yet drawn.
    SpectrogramPendingWork pending;
    /// The particles being drawn this frame, swapped with the pending ones so their memory is reused.
    std::vector<SpectrogramParticle> drawing;

    /// Draws the particles as Gaussian sprites.
    std::unique_ptr<juce::OpenGLShaderProgram> particleProgram;
    /// Multiplies the accumulated picture by the fade.
    std::unique_ptr<juce::OpenGLShaderProgram> fadeProgram;
    /// Halves a picture with a 13-tap filter, for the glow chain.
    std::unique_ptr<juce::OpenGLShaderProgram> downsampleProgram;
    /// Doubles a picture with a tent filter and adds it to the next larger level of the glow chain.
    std::unique_ptr<juce::OpenGLShaderProgram> upsampleProgram;
    /// Tone maps the picture and its glow into the plot.
    std::unique_ptr<juce::OpenGLShaderProgram> compositeProgram;

    // Cached uniforms follow their programs so they are destroyed first.
    // releaseResources clears both groups while their context is current.
    /// Linked locations for the particle shader.
    std::unique_ptr<SpectrogramParticleUniforms> particleUniforms;
    /// Linked locations for glow downsampling.
    std::unique_ptr<SpectrogramDownsampleUniforms> downsampleUniforms;
    /// Linked locations for glow upsampling.
    std::unique_ptr<SpectrogramUpsampleUniforms> upsampleUniforms;
    /// Linked locations for the tone-mapped composite.
    std::unique_ptr<SpectrogramCompositeUniforms> compositeUniforms;

    /// Vertex array with the sprite corners and the particle instances.
    GLuint particleVertexArray {0};
    /// Empty vertex array for the full-screen passes, which make their triangle from the vertex index.
    GLuint screenVertexArray {0};
    /// The four corners of a sprite.
    GLuint cornerBuffer {0};
    /// The particles of the current frame, one instance each.
    GLuint instanceBuffer {0};

    /// The accumulated particles at the plot's physical size.
    SpectrogramRenderTarget accumulation;
    /// The glow chain, each level half the size of the one before.
    std::array<SpectrogramRenderTarget, SPECTROGRAM_BLOOM_LEVEL_COUNT> bloomLevels;
    /// Density field of the solid layers, copied from one glow-chain level before adding its smaller levels.
    SpectrogramRenderTarget solidField;

#if defined(JUCE_DEBUG) || defined(IMAGE_CONTROL_RENDER_BENCHMARK)
    /// GPU timer queries in flight, read later only after availability is confirmed.
    std::array<GLuint, SPECTROGRAM_TIMER_QUERY_COUNT> timerQueries {};
    /// Whether each query still has an unread result.
    std::array<bool, SPECTROGRAM_TIMER_QUERY_COUNT> pendingTimerQueries {};
    /// Next timer query to start.
    std::size_t nextTimerQuery {0};
    /// Whether this frame successfully acquired a free timer query.
    bool timerQueryActive {false};
#endif
#ifdef IMAGE_CONTROL_RENDER_BENCHMARK
    /// Explicit offscreen composite target, owned and used only on the context's GL thread.
    SpectrogramRenderTarget benchmarkOutput;
    /// Completed query values, consumed after a bounded batch completes.
    std::vector<SpectrogramBenchmarkGpuSample> benchmarkGpuSamples;
    /// Original frame identity for each pending query.
    std::array<std::size_t, SPECTROGRAM_TIMER_QUERY_COUNT> benchmarkQueryFrames {};
    /// Frame index within the current benchmark phase.
    std::size_t benchmarkFrameIndex {0};
    /// Physical-to-logical scale independent of the native helper window's display.
    float benchmarkScale {1.0F};
    /// Release timing is opt-in for the isolated benchmark executable only.
    bool benchmarkGpuTiming {false};
#endif

    /// Number of frames drawn, for the frame rate readout.
    std::atomic<int> renderedFrames {0};
    /// GPU time of a recent frame in milliseconds, or negative when it is not measured.
    std::atomic<double> gpuMilliseconds {-1.0};
    /// Whether the shaders or framebuffers could not be created.
    std::atomic<bool> unavailable {false};
    /// Whether the GL resources exist.
    bool ready {false};
};
}  // namespace esgrove::image_control
