/// Implements the frequency axis scales, the frequency and phase colours,
/// and SpectrogramParticleBuilder: the brightness, spread, and colour of the particle of each analyzed bin.

#include "ui/SpectrogramParticles.h"

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Level shown as the faintest particle, in dB relative to a full-scale sine.
constexpr float DISPLAY_FLOOR_DB {-72.0F};
/// Level range from the faintest to the brightest particle, in dB.
constexpr float DISPLAY_RANGE_DB {72.0F};
/// Exponent from the level's share of the display range to the particle brightness.
/// Above 1 it keeps the noise floor dark, so quiet bins show as faint dots instead of a haze over the plot.
constexpr float LEVEL_GAMMA {2.2F};
/// Weakest particle weight that is still drawn. Fainter particles would not show.
constexpr float MIN_PARTICLE_WEIGHT {1.0e-4F};
/// Time constant over which a new particle reaches its brightness, in seconds.
/// It is short and independent of the persistence, so a single frame already draws a bright dot.
constexpr double FLASH_SECONDS {0.04};
/// Horizontal spread of a particle relative to the distance to its neighbours,
/// so neighbouring bins blend into a continuous trace at the low frequencies.
constexpr float PARTICLE_SIGMA_PER_SPACING {0.6F};
/// Number of steps in the frequency colour table.
constexpr std::size_t FREQUENCY_COLOUR_STEPS {1024};
/// Quietest level the brightness is computed for, which keeps the logarithm finite.
constexpr float SILENT_LEVEL {1.0e-12F};

/// Hue of the lowest frequency: red.
constexpr float LOW_FREQUENCY_HUE {0.0F};
/// Hue of the highest frequency: blue towards violet.
constexpr float HIGH_FREQUENCY_HUE {0.7F};
/// Exponent that bends the hue along the frequency axis, so the bass octaves stay red
/// and the hue moves through orange, yellow, and green in the mids before reaching blue in the highs.
constexpr float HUE_CURVE {1.5F};
/// Saturation of the frequency colours.
constexpr float FREQUENCY_SATURATION {0.85F};
/// Colour of in-phase content in the phase colours.
constexpr LinearColour IN_PHASE_COLOUR {.red = 0.35F, .green = 0.7F, .blue = 1.0F};
/// Colour of anti-phase content in the phase colours.
constexpr LinearColour ANTI_PHASE_COLOUR {.red = 1.0F, .green = 0.3F, .blue = 0.35F};

/// HTK mel scale factor, as in AudioBatch's spectrogram.
constexpr double MEL_SCALE {2595.0};
/// HTK mel corner frequency, in Hz.
constexpr double MEL_CORNER_FREQUENCY {700.0};
/// Base of the logarithm in the HTK mel definition.
constexpr double MEL_LOGARITHM_BASE {10.0};

/// Builds the frequency colour of evenly spaced shares of the frequency axis.
std::vector<LinearColour> buildFrequencyColours()
{
    std::vector<LinearColour> table(FREQUENCY_COLOUR_STEPS);
    for (std::size_t step = 0; step < FREQUENCY_COLOUR_STEPS; ++step) {
        table[step] = frequencyColour(static_cast<float>(step) / static_cast<float>(FREQUENCY_COLOUR_STEPS - 1));
    }
    return table;
}
}  // namespace

/// Returns where a frequency lies on the frequency axis, from 0 at MIN_FREQUENCY to 1 at MAX_FREQUENCY.
double frequencyToShare(const double frequency, const FrequencyScale scale)
{
    switch (scale) {
        case FrequencyScale::logarithmic: {
            const double ratio = static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY);
            return std::log(frequency / static_cast<double>(MIN_FREQUENCY)) / std::log(ratio);
        }
        case FrequencyScale::mel: {
            const double lowest = frequencyToMel(static_cast<double>(MIN_FREQUENCY));
            const double highest = frequencyToMel(static_cast<double>(MAX_FREQUENCY));
            return (frequencyToMel(frequency) - lowest) / (highest - lowest);
        }
    }
    return 0.0;
}

/// Returns the frequency at a share of the frequency axis, the inverse of frequencyToShare.
double shareToFrequency(const double share, const FrequencyScale scale)
{
    switch (scale) {
        case FrequencyScale::logarithmic: {
            const double ratio = static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY);
            return static_cast<double>(MIN_FREQUENCY) * std::pow(ratio, share);
        }
        case FrequencyScale::mel: {
            const double lowest = frequencyToMel(static_cast<double>(MIN_FREQUENCY));
            const double highest = frequencyToMel(static_cast<double>(MAX_FREQUENCY));
            return melToFrequency(lowest + share * (highest - lowest));
        }
    }
    return 0.0;
}

/// Returns the HTK mel value of a frequency, the same definition as AudioBatch's spectrogram.
double frequencyToMel(const double frequency)
{
    return MEL_SCALE * std::log10(1.0 + frequency / MEL_CORNER_FREQUENCY);
}

/// Returns the frequency of an HTK mel value, the inverse of frequencyToMel.
double melToFrequency(const double mel)
{
    return MEL_CORNER_FREQUENCY * (std::pow(MEL_LOGARITHM_BASE, mel / MEL_SCALE) - 1.0);
}

/// Returns the frequency colour of a share of the frequency axis: red at the left edge through to blue at the right.
LinearColour frequencyColour(const float share)
{
    const auto colour = juce::Colour::fromHSV(
        LOW_FREQUENCY_HUE + (HIGH_FREQUENCY_HUE - LOW_FREQUENCY_HUE) * std::pow(share, HUE_CURVE),
        FREQUENCY_SATURATION,
        1.0F,
        1.0F
    );
    return {.red = colour.getFloatRed(), .green = colour.getFloatGreen(), .blue = colour.getFloatBlue()};
}

/// Creates a builder with no particles and the frequency colour table.
SpectrogramParticleBuilder::SpectrogramParticleBuilder() : frequencyColours(buildFrequencyColours()) {}

/// Returns the particles added since the last clear.
std::span<const SpectrogramParticle> SpectrogramParticleBuilder::particles() const noexcept
{
    return collected;
}

/// Turns the points of one analyzed frame into particles.
/// A particle's brightness follows its level in dB through a power curve, which keeps the noise floor dark.
/// Its weight is the share of a short flash its frame stands for,
/// so a single frame draws a bright dot and the brightness does not depend on the analyzer's frame rate.
/// Long persistence lets steady content build up for longer,
/// so the weight shrinks with the square root of the persistence to keep the picture from saturating.
void SpectrogramParticleBuilder::addFrame(const std::span<const SpectralPoint> points, const double frameSeconds)
{
    const auto flash = static_cast<float>(
        (1.0 - std::exp(-frameSeconds / FLASH_SECONDS)) * std::sqrt(DEFAULT_PERSISTENCE_SECONDS / persistenceSeconds)
    );
    collected.reserve(collected.size() + points.size());
    for (const auto& point : points) {
        const float decibels = 10.0F * std::log10(std::max(point.level, SILENT_LEVEL));
        const float share = std::clamp((decibels - DISPLAY_FLOOR_DB) / DISPLAY_RANGE_DB, 0.0F, 1.0F);
        const float weight = std::pow(share, LEVEL_GAMMA) * flash;
        if (weight < MIN_PARTICLE_WEIGHT) {
            continue;
        }

        const auto frequency = static_cast<double>(point.frequency);
        // The distance to the neighbouring bins on the axis depends on the frequency and the scale.
        const double halfSpacing = static_cast<double>(point.spacing) * 0.5;
        const double spacingShare = frequencyToShare(frequency + halfSpacing, scale)
            - frequencyToShare(std::max(static_cast<double>(MIN_FREQUENCY), frequency - halfSpacing), scale);
        const LinearColour colour = pointColour(point);
        collected.push_back({
            .x = static_cast<float>(frequencyToShare(frequency, scale)),
            .y = (1.0F - point.position) * 0.5F,
            .spread = static_cast<float>(spacingShare) * PARTICLE_SIGMA_PER_SPACING,
            .red = colour.red * weight,
            .green = colour.green * weight,
            .blue = colour.blue * weight,
        });
    }
}

/// Drops the collected particles, keeping their memory for the next ones.
void SpectrogramParticleBuilder::clear() noexcept
{
    collected.clear();
}

/// Selects frequency or phase colours for new particles.
void SpectrogramParticleBuilder::setColours(const SpectrogramColours newColours) noexcept
{
    colours = newColours;
}

/// Sets the frequency scale new particles are placed on.
void SpectrogramParticleBuilder::setFrequencyScale(const FrequencyScale newScale) noexcept
{
    scale = newScale;
}

/// Sets the time constant of the particle fade, clamped to the persistence range.
void SpectrogramParticleBuilder::setPersistence(const double seconds) noexcept
{
    persistenceSeconds = std::clamp(seconds, MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS);
}

/// Returns the colour of a particle: its frequency colour, or its phase colour from its coherence.
LinearColour SpectrogramParticleBuilder::pointColour(const SpectralPoint& point) const
{
    if (colours == SpectrogramColours::frequency) {
        const double share
            = std::clamp(frequencyToShare(static_cast<double>(point.frequency), FrequencyScale::logarithmic), 0.0, 1.0);
        return frequencyColours[static_cast<std::size_t>(share * static_cast<double>(FREQUENCY_COLOUR_STEPS - 1))];
    }
    const float antiPhaseShare = std::clamp((1.0F - point.coherence) * 0.5F, 0.0F, 1.0F);
    return {
        .red = IN_PHASE_COLOUR.red + (ANTI_PHASE_COLOUR.red - IN_PHASE_COLOUR.red) * antiPhaseShare,
        .green = IN_PHASE_COLOUR.green + (ANTI_PHASE_COLOUR.green - IN_PHASE_COLOUR.green) * antiPhaseShare,
        .blue = IN_PHASE_COLOUR.blue + (ANTI_PHASE_COLOUR.blue - IN_PHASE_COLOUR.blue) * antiPhaseShare,
    };
}
}  // namespace esgrove::image_control
