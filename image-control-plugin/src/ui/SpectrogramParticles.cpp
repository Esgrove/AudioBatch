/// Implements the frequency axis scales, the frequency and phase colours with their sRGB conversion,
/// and SpectrogramParticleBuilder:
/// the auto-scaled brightness, size, spread, and colour of the particle of each analyzed bin.

#include "ui/SpectrogramParticles.h"

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Level range below the auto-scale reference from the faintest to the brightest particle, in dB.
constexpr float DISPLAY_RANGE_DB {36.0F};
/// How much steeper the contrast curve gets from no contrast to full contrast, as an addition to its exponent.
constexpr float CONTRAST_EXPONENT_RANGE {3.0F};
/// How far the pivot of the contrast curve moves up from the middle of the display range at full contrast,
/// so more of the quieter range darkens and only the strongest content stays bright.
constexpr float CONTRAST_PIVOT_SHIFT {0.2F};
/// Extra contrast at and above HIGH_CONTRAST_FULL_FREQUENCY, where dense, noisy bins would otherwise blur together.
constexpr float HIGH_CONTRAST_BOOST {0.25F};
/// Frequency from which the extra contrast of the highs rises, in Hz.
constexpr float HIGH_CONTRAST_START_FREQUENCY {1000.0F};
/// Frequency where the extra contrast of the highs is complete, in Hz.
constexpr float HIGH_CONTRAST_FULL_FREQUENCY {16'000.0F};
/// Tilt added to the displayed level per octave above the pivot, in dB.
/// Music falls by about this much per octave, so the tilt shows the bass, the mids,
/// and the highs about equally bright instead of leaving the highs faint.
/// It changes only the brightness and size of the particles, not their positions or colours.
constexpr float TILT_DB_PER_OCTAVE {3.0F};
/// Frequency where the tilt is 0 dB, in Hz.
constexpr float TILT_PIVOT_FREQUENCY {1000.0F};
/// Rate at which the auto-scale reference falls after the loudest bin gets quieter, in dB per second.
/// It rises at once, so a loud onset never saturates the picture.
constexpr double AUTO_SCALE_RELEASE_DB_PER_SECOND {12.0};
/// Size of the faintest particle relative to the loudest, so louder content draws larger dots.
constexpr float MIN_PARTICLE_SIZE {0.5F};
/// Weakest particle weight that is still drawn. Fainter particles would not show.
constexpr float MIN_PARTICLE_WEIGHT {1.0e-4F};
/// Time constant over which a new particle reaches its brightness, in seconds.
/// It is short and independent of the persistence, so a single frame already draws a bright dot.
constexpr double FLASH_SECONDS {0.04};
/// Horizontal spread of a particle relative to the distance to its neighbours,
/// so neighbouring bins blend into a continuous trace at the low frequencies.
constexpr float PARTICLE_SIGMA_PER_SPACING {0.35F};
/// Number of steps in the frequency colour table.
constexpr std::size_t FREQUENCY_COLOUR_STEPS {1024};
/// Quietest level the brightness is computed for, which keeps the logarithm finite.
constexpr float SILENT_LEVEL {1.0e-12F};
/// Decibels per decade of a power level.
constexpr float DECIBELS_PER_POWER_DECADE {10.0F};

/// Colour of in-phase content in the phase colours, in linear light: a light blue.
constexpr LinearColour IN_PHASE_COLOUR {.red = 0.1F, .green = 0.45F, .blue = 1.0F};
/// Colour of anti-phase content in the phase colours, in linear light: a warm red.
constexpr LinearColour ANTI_PHASE_COLOUR {.red = 1.0F, .green = 0.07F, .blue = 0.1F};

/// Channel value up to which the sRGB transfer curve is linear, in encoded sRGB.
constexpr float SRGB_LINEAR_LIMIT {0.04045F};
/// Channel value up to which the sRGB transfer curve is linear, in linear light.
constexpr float LINEAR_SRGB_LIMIT {0.0031308F};
/// Slope of the linear part of the sRGB transfer curve.
constexpr float SRGB_LINEAR_SLOPE {12.92F};
/// Offset of the power part of the sRGB transfer curve.
constexpr float SRGB_OFFSET {0.055F};
/// Exponent of the power part of the sRGB transfer curve.
constexpr float SRGB_EXPONENT {2.4F};

/// HTK mel scale factor, as in AudioBatch's spectrogram.
constexpr double MEL_SCALE {2595.0};
/// HTK mel corner frequency, in Hz.
constexpr double MEL_CORNER_FREQUENCY {700.0};
/// Base of the logarithm in the HTK mel definition.
constexpr double MEL_LOGARITHM_BASE {10.0};

/// A colour of the frequency palette at a frequency, as sRGB channels from 0 to 1.
struct FrequencyColourStop {
    float frequency {0.0F};
    float red {0.0F};
    float green {0.0F};
    float blue {0.0F};
};

/// The frequency palette from the bass to the highs, interpolated in linear light on the logarithmic axis.
/// The bass is red, the low mids yellow, 1 kHz green, the presence range cyan, the highs blue,
/// and the air band violet towards magenta.
constexpr std::array FREQUENCY_COLOUR_STOPS {
    FrequencyColourStop {.frequency = 20.0F, .red = 1.0F, .green = 0.06F, .blue = 0.04F},
    FrequencyColourStop {.frequency = 50.0F, .red = 1.0F, .green = 0.1F, .blue = 0.04F},
    FrequencyColourStop {.frequency = 75.0F, .red = 1.0F, .green = 0.6F, .blue = 0.05F},
    FrequencyColourStop {.frequency = 150.0F, .red = 1.0F, .green = 0.92F, .blue = 0.1F},
    FrequencyColourStop {.frequency = 400.0F, .red = 0.92F, .green = 1.0F, .blue = 0.1F},
    FrequencyColourStop {.frequency = 1000.0F, .red = 0.2F, .green = 1.0F, .blue = 0.15F},
    FrequencyColourStop {.frequency = 2500.0F, .red = 0.1F, .green = 1.0F, .blue = 0.7F},
    FrequencyColourStop {.frequency = 4000.0F, .red = 0.1F, .green = 0.9F, .blue = 1.0F},
    FrequencyColourStop {.frequency = 8000.0F, .red = 0.2F, .green = 0.45F, .blue = 1.0F},
    FrequencyColourStop {.frequency = 13'000.0F, .red = 0.6F, .green = 0.3F, .blue = 1.0F},
    FrequencyColourStop {.frequency = 20'000.0F, .red = 0.95F, .green = 0.3F, .blue = 1.0F},
};

/// Returns an sRGB channel value from 0 to 1 in linear light.
float srgbToLinear(const float value)
{
    return value <= SRGB_LINEAR_LIMIT ? value / SRGB_LINEAR_SLOPE
                                      : std::pow((value + SRGB_OFFSET) / (1.0F + SRGB_OFFSET), SRGB_EXPONENT);
}

/// Returns a linear channel value from 0 to 1 encoded with the sRGB transfer curve.
float linearToSrgb(const float value)
{
    return value <= LINEAR_SRGB_LIMIT ? value * SRGB_LINEAR_SLOPE
                                      : (1.0F + SRGB_OFFSET) * std::pow(value, 1.0F / SRGB_EXPONENT) - SRGB_OFFSET;
}

/// Returns the colour of a palette stop in linear light.
LinearColour linearStopColour(const FrequencyColourStop& stop)
{
    return {.red = srgbToLinear(stop.red), .green = srgbToLinear(stop.green), .blue = srgbToLinear(stop.blue)};
}

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

/// Returns the frequency colour, in linear light, of a share of the logarithmic frequency axis:
/// red in the bass through yellow, green, cyan, and blue to violet in the highs.
LinearColour frequencyColour(const float share)
{
    const double frequency
        = shareToFrequency(std::clamp(static_cast<double>(share), 0.0, 1.0), FrequencyScale::logarithmic);
    const auto upperIndex = static_cast<std::size_t>(std::ranges::distance(
        FREQUENCY_COLOUR_STOPS.begin(),
        std::ranges::find_if(FREQUENCY_COLOUR_STOPS, [frequency](const FrequencyColourStop& stop) {
            return static_cast<double>(stop.frequency) >= frequency;
        })
    ));
    if (upperIndex == 0) {
        return linearStopColour(FREQUENCY_COLOUR_STOPS.front());
    }
    if (upperIndex == FREQUENCY_COLOUR_STOPS.size()) {
        return linearStopColour(FREQUENCY_COLOUR_STOPS.back());
    }
    const auto& lower = FREQUENCY_COLOUR_STOPS.at(upperIndex - 1);
    const auto& upper = FREQUENCY_COLOUR_STOPS.at(upperIndex);
    const auto position = static_cast<float>(
        std::log(frequency / static_cast<double>(lower.frequency))
        / std::log(static_cast<double>(upper.frequency) / static_cast<double>(lower.frequency))
    );
    const LinearColour low = linearStopColour(lower);
    const LinearColour high = linearStopColour(upper);
    return {
        .red = low.red + (high.red - low.red) * position,
        .green = low.green + (high.green - low.green) * position,
        .blue = low.blue + (high.blue - low.blue) * position,
    };
}

/// Returns a colour in linear light encoded with the sRGB transfer curve,
/// for drawing it with JUCE outside the renderer, which works in sRGB.
LinearColour toSrgb(const LinearColour& colour)
{
    return {.red = linearToSrgb(colour.red), .green = linearToSrgb(colour.green), .blue = linearToSrgb(colour.blue)};
}

/// Returns the brightness of a level from its share of the display range, both from 0 to 1,
/// through an S-curve set by the contrast.
/// The curve is s^g / (s^g + (p · (1 − s) / (1 − p))^g), which passes through 0, ½ at the pivot p, and 1.
/// The contrast raises the exponent g from 1 and moves the pivot up from the middle of the range,
/// so quiet content darkens, loud content brightens, and the steep part between them separates the two.
/// No contrast leaves the share unchanged, linear in dB.
float contrastCurve(const float share, const float contrast)
{
    const float clampedShare = std::clamp(share, 0.0F, 1.0F);
    const float clampedContrast = std::clamp(contrast, 0.0F, 1.0F + HIGH_CONTRAST_BOOST);
    const float exponent = 1.0F + CONTRAST_EXPONENT_RANGE * clampedContrast;
    const float pivot = 0.5F + CONTRAST_PIVOT_SHIFT * clampedContrast;
    const float rising = std::pow(clampedShare, exponent);
    const float falling = std::pow(pivot * (1.0F - clampedShare) / (1.0F - pivot), exponent);
    return rising / (rising + falling);
}

/// Returns the setting plus a high-frequency contrast boost, rising along the logarithmic frequency axis.
/// The ramp starts at HIGH_CONTRAST_START_FREQUENCY and reaches HIGH_CONTRAST_BOOST at HIGH_CONTRAST_FULL_FREQUENCY.
/// This makes the strongest content clearer in dense, noisy highs.
float frequencyContrast(const float frequency, const float contrast)
{
    const float ramp = std::log2(std::max(frequency, MIN_FREQUENCY) / HIGH_CONTRAST_START_FREQUENCY)
        / std::log2(HIGH_CONTRAST_FULL_FREQUENCY / HIGH_CONTRAST_START_FREQUENCY);
    return contrast + HIGH_CONTRAST_BOOST * std::clamp(ramp, 0.0F, 1.0F);
}

/// Creates a builder with no particles and the frequency colour table.
SpectrogramParticleBuilder::SpectrogramParticleBuilder() : frequencyColours(buildFrequencyColours()) {}

/// Returns the particles added since the last clear.
std::span<const SpectrogramParticle> SpectrogramParticleBuilder::particles() const noexcept
{
    return collected;
}

/// Turns the points of one analyzed frame into particles.
/// A particle's level in dB gets the spectral tilt,
/// and its share of the display range below the auto-scale reference goes through the contrast curve,
/// which keeps the noise floor dark and sets the particle's brightness, size, and halo.
/// A particle sits at its bin's reassigned frequency, so a sine draws as a thin line at any plot width.
/// The reference follows the loudest tilted bin of every tier, rising at once and falling slowly,
/// so the picture keeps its brightness for quiet and loud material alike.
/// A particle's weight is the share of a short flash its frame stands for,
/// so a single frame draws a bright dot and the brightness does not depend on the analyzer's frame rate.
/// Long persistence lets steady content build up for longer,
/// so the weight shrinks with the square root of the persistence to keep the picture from saturating.
void SpectrogramParticleBuilder::addFrame(const std::span<const SpectralPoint> points, const double frameSeconds)
{
    const auto flash = static_cast<float>(
        (1.0 - std::exp(-frameSeconds / FLASH_SECONDS)) * std::sqrt(DEFAULT_PERSISTENCE_SECONDS / persistenceSeconds)
    );

    tiltedDecibels.resize(points.size());
    float loudest = MIN_REFERENCE_DB;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        const float tilt
            = TILT_DB_PER_OCTAVE * std::log2(std::max(point.frequency, MIN_FREQUENCY) / TILT_PIVOT_FREQUENCY);
        tiltedDecibels[index] = DECIBELS_PER_POWER_DECADE * std::log10(std::max(point.level, SILENT_LEVEL)) + tilt;
        loudest = std::max(loudest, tiltedDecibels[index]);
    }
    const double released = static_cast<double>(referenceDecibels) - AUTO_SCALE_RELEASE_DB_PER_SECOND * frameSeconds;
    referenceDecibels = std::max(static_cast<float>(released), loudest);
    const float floorDecibels = referenceDecibels - DISPLAY_RANGE_DB;

    collected.reserve(collected.size() + points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        const float share = std::clamp((tiltedDecibels[index] - floorDecibels) / DISPLAY_RANGE_DB, 0.0F, 1.0F);
        const float emphasis = contrastCurve(share, frequencyContrast(point.frequency, static_cast<float>(contrast)));
        const float weight = emphasis * flash;
        if (weight < MIN_PARTICLE_WEIGHT) {
            continue;
        }

        const auto frequency = static_cast<double>(point.reassignedFrequency);
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
            .size = MIN_PARTICLE_SIZE + (1.0F - MIN_PARTICLE_SIZE) * emphasis,
            .emphasis = emphasis,
        });
    }
}

/// Drops the collected particles, keeping their memory for the next ones.
void SpectrogramParticleBuilder::clear() noexcept
{
    collected.clear();
}

/// Returns the auto-scale reference, the tilted level in dB that draws the brightest particles.
float SpectrogramParticleBuilder::referenceLevel() const noexcept
{
    return referenceDecibels;
}

/// Drops the auto-scale reference to its lowest level, so it starts over from the next frame,
/// such as after the analysis is cleared.
void SpectrogramParticleBuilder::resetLevel() noexcept
{
    referenceDecibels = MIN_REFERENCE_DB;
}

/// Sets the contrast of the level curve for new particles, from 0 for linear in dB to 1 for the steepest S-curve.
void SpectrogramParticleBuilder::setContrast(const double newContrast) noexcept
{
    contrast = std::clamp(newContrast, 0.0, 1.0);
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
