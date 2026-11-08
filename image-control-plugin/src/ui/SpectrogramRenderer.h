/// Declares SpectrogramRenderer, which draws the spatial spectrogram's particles on the GPU with OpenGL.
/// The analysis worker builds the particles on its own thread, the editor sets the fade
/// and the plot on the message thread, and the renderer draws on JUCE's OpenGL thread,
/// under the editor's components, which JUCE paints over the GL picture.
/// Only this class knows OpenGL, so another GPU backend can replace it without touching the view or the worker.

#pragma once

#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"
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

/// Draws the spectrogram particles with OpenGL.
///
/// Every frame it fades the accumulated picture by the time that passed.
/// It adds the new particles as instanced Gaussian sprites with additive blending.
/// The sprites go into a float framebuffer at the plot's full physical resolution,
/// which keeps them sharp on any display.
/// It then builds a soft glow from a chain of downsampled copies,
/// and tone maps the picture into the plot with rounded corners.
/// In the solid mode, the composite instead draws a smooth level of the glow chain as stacked translucent layers
/// with outlines, and keeps only the strongest particles as bright traces on top.
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

    void newOpenGLContextCreated() override;
    void openGLContextClosing() override;
    void renderOpenGL() override;

private:
    /// The work submitted since the last frame.
    struct PendingWork {
        std::vector<SpectrogramParticle> particles;
        double fadeSeconds {0.0};
        double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
        bool clear {false};
        juce::Rectangle<float> plotInEditor;
        SpectrogramRenderMode mode {SpectrogramRenderMode::hidden};
    };

    [[nodiscard]] bool createPrograms();
    [[nodiscard]] bool resizeTargets(int width, int height);

    void drawBloom();
    void drawComposite(juce::Rectangle<int> plotViewport, float exposure, bool solid);
    void drawParticles(float scale);
    void fade(double seconds, double persistenceSeconds);
    void measureGpuTimeBegin();
    void measureGpuTimeEnd();
    void releaseResources();

    /// The context this renderer is attached to, owned by the editor.
    juce::OpenGLContext& openGLContext;

    /// Guards pending.
    std::mutex pendingMutex;
    /// Work submitted on the message thread and not yet drawn.
    PendingWork pending;
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
    std::array<SpectrogramRenderTarget, 5> bloomLevels;

    /// GPU timer queries in flight, read a few frames later so reading them never stalls the pipeline.
    std::array<GLuint, 4> timerQueries {};
    /// Next timer query to start.
    std::size_t nextTimerQuery {0};
    /// Number of timer queries started, up to their count.
    std::size_t startedTimerQueries {0};

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
