/// Declares the spatial spectrogram's particles and how analyzed bins become them:
/// the frequency axis scales, the frequency and phase colours, and SpectrogramParticleBuilder.
/// None of it touches a component or the GPU, so the analysis worker thread can build particles
/// while the view and the renderer use the same axis definitions.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// How the particles are coloured.
/// Frequency colours run from red in the bass through yellow, green, cyan, and blue to violet in the highs,
/// and phase colours from blue for in-phase content to red for anti-phase content.
enum class SpectrogramColours : std::uint8_t {
    frequency,
    phase,
};

/// How frequency maps onto the horizontal axis.
/// The logarithmic scale gives every octave the same width,
/// and the mel scale, as in AudioBatch's spectrogram, gives the bass less room and the high frequencies more.
enum class FrequencyScale : std::uint8_t {
    logarithmic,
    mel,
};

/// Lowest auto-scale reference of the particle brightness, in tilted dB,
/// so silence and noise floors are not scaled up to full brightness.
constexpr float MIN_REFERENCE_DB {-90.0F};

/// A colour with float channels from 0 to 1 in linear light, for the additive particle buffer.
struct LinearColour {
    float red {0.0F};
    float green {0.0F};
    float blue {0.0F};
};

/// One analyzed bin, ready for the GPU.
/// The position is in shares of the plot, so it does not depend on the plot size or the display scale,
/// and the renderer turns it into physical pixels.
/// The layout is the instance layout of the renderer's particle shader.
struct SpectrogramParticle {
    /// Centre on the frequency axis, from 0 at the left edge to 1 at the right edge.
    float x {0.0F};
    /// Centre on the position axis, from 0 at the top (full left) to 1 at the bottom (full right).
    float y {0.0F};
    /// Horizontal spread that the distance to the neighbouring bins asks for, as a share of the plot width.
    /// The renderer never draws a particle narrower than its sharp core.
    float spread {0.0F};
    /// Red channel of the colour, in linear light, multiplied by the particle's weight.
    float red {0.0F};
    /// Green channel of the colour, in linear light, multiplied by the particle's weight.
    float green {0.0F};
    /// Blue channel of the colour, in linear light, multiplied by the particle's weight.
    float blue {0.0F};
    /// Size of the particle relative to the loudest, which the renderer scales its core and halo by.
    float size {1.0F};
    /// How strongly the contrast curve emphasises the particle, from 0 for the faintest to 1 for the strongest,
    /// which scales the brightness of its halo, so faint particles draw only their sharp cores.
    float emphasis {1.0F};
};

[[nodiscard]] double frequencyToShare(double frequency, FrequencyScale scale);
[[nodiscard]] double shareToFrequency(double share, FrequencyScale scale);
[[nodiscard]] double frequencyToMel(double frequency);
[[nodiscard]] double melToFrequency(double mel);
[[nodiscard]] LinearColour frequencyColour(float share);
[[nodiscard]] LinearColour toSrgb(const LinearColour& colour);
[[nodiscard]] float contrastCurve(float share, float contrast);
[[nodiscard]] float frequencyContrast(float frequency, float contrast);

/// Reusable storage for one tier at a common analyzer deadline.
struct PendingSpectralFrame {
    /// Valid display points, retained until all tiers due at the deadline have arrived.
    std::vector<SpectralPoint> points;
    /// Tilted point levels, in dB, cached for reference selection and particle weighting.
    std::vector<float> tiltedDecibels;
    /// This tier's hop duration, retained independently of the common reference clock.
    double frameSeconds {0.0};
};

/// Turns analyzed frames into particles and collects them until they are taken.
/// Not thread safe. The analysis worker owns it.
class SpectrogramParticleBuilder
{
public:
    SpectrogramParticleBuilder();

    [[nodiscard]] std::span<const SpectrogramParticle> particles() const noexcept;
    [[nodiscard]] float referenceLevel() const noexcept;

    void addFrame(std::span<const SpectralPoint> points, const SpectralFrameInfo& frame);
    void addFrame(std::span<const SpectralPoint> points, double frameSeconds);
    void clear() noexcept;
    void resetLevel() noexcept;

    void setColours(SpectrogramColours newColours) noexcept;
    void setContrast(double newContrast) noexcept;
    void setFrequencyScale(FrequencyScale newScale) noexcept;
    void setPersistence(double seconds) noexcept;

private:
    [[nodiscard]] LinearColour pointColour(const SpectralPoint& point) const;

    void appendParticles(const PendingSpectralFrame& frame);
    void finishDeadline();

    /// Pending tier frames, with reusable point storage.
    std::array<PendingSpectralFrame, ANALYSIS_TIER_COUNT> pendingFrames;
    /// Order in which tiers arrived at this deadline, preserved when their particles are emitted.
    std::array<std::size_t, ANALYSIS_TIER_COUNT> pendingOrder {};
    /// Latest tilted peak of each tier, retained until that tier reports again.
    std::array<float, ANALYSIS_TIER_COUNT> tierPeaks {};

    /// The particles added since the last clear.
    std::vector<SpectrogramParticle> collected;
    /// Frequency colour of evenly spaced shares of the logarithmic frequency axis.
    std::vector<LinearColour> frequencyColours;
    /// How new particles are coloured.
    SpectrogramColours colours {SpectrogramColours::frequency};
    /// How frequency maps onto the horizontal axis.
    FrequencyScale scale {FrequencyScale::logarithmic};

    /// Contrast of the level curve, from 0 for linear in dB to 1 for the steepest S-curve.
    double contrast {DEFAULT_CONTRAST_PERCENT / MAX_CONTRAST_PERCENT};
    /// End time of the last complete deadline, used once for shared reference release.
    double lastEndSeconds {0.0};
    /// End time shared by the pending frames.
    double pendingEndSeconds {0.0};
    /// Number of pending tiers at this deadline.
    std::size_t pendingCount {0};
    /// Time constant of the particle fade, in seconds, which scales the particle weight.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Auto-scale reference: the tilted level in dB that draws the brightest particles.
    float referenceDecibels {MIN_REFERENCE_DB};
};
}  // namespace esgrove::image_control
