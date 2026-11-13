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
/// Extra contrast four octaves above HIGH_CONTRAST_START_FREQUENCY (16 kHz), where dense highs would blur together.
constexpr float HIGH_CONTRAST_BOOST {0.25F};
/// Frequency from which the extra contrast of the highs rises, in Hz.
constexpr float HIGH_CONTRAST_START_FREQUENCY {1000.0F};
/// Inverse octave span of the high-frequency contrast ramp (four octaves from 1 kHz to 16 kHz).
constexpr float HIGH_CONTRAST_INV_OCTAVES {0.25F};
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
/// Time constant of the hop-weighting flash, in seconds, independent of particle persistence.
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
/// Ratio between the upper and lower frequency bounds on the logarithmic axis.
constexpr double LOG_FREQUENCY_RATIO {static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY)};
/// Cached natural logarithm of LOG_FREQUENCY_RATIO, initialized once at first axis query.
double logFrequencySpan()
{
    static const double span = std::log(LOG_FREQUENCY_RATIO);
    return span;
}

/// Cached HTK mel value of MIN_FREQUENCY.
double minimumMel()
{
    static const double lowest = frequencyToMel(static_cast<double>(MIN_FREQUENCY));
    return lowest;
}

/// Cached HTK mel span between MIN_FREQUENCY and MAX_FREQUENCY.
double melSpan()
{
    static const double span = frequencyToMel(static_cast<double>(MAX_FREQUENCY)) - minimumMel();
    return span;
}

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
/// Rejects malformed display results before logarithms, axis transforms, or palette indexing can consume them.
/// Finite positions and coherence are clamped later, but invalid geometry and power are discarded.
bool validPoint(const SpectralPoint& point)
{
    return std::isfinite(point.frequency) && std::isfinite(point.reassignedFrequency) && std::isfinite(point.spacing)
        && std::isfinite(point.position) && std::isfinite(point.level) && std::isfinite(point.coherence)
        && point.frequency >= MIN_FREQUENCY && point.frequency <= MAX_FREQUENCY
        && point.reassignedFrequency >= MIN_FREQUENCY && point.reassignedFrequency <= MAX_FREQUENCY
        && point.spacing >= 0.0F && point.level > 0.0F;
}
}  // namespace

/// Returns where a frequency lies on the frequency axis, from 0 at MIN_FREQUENCY to 1 at MAX_FREQUENCY.
double frequencyToShare(const double frequency, const FrequencyScale scale)
{
    switch (scale) {
        case FrequencyScale::logarithmic: {
            return std::log(frequency / static_cast<double>(MIN_FREQUENCY)) / logFrequencySpan();
        }
        case FrequencyScale::mel: {
            return (frequencyToMel(frequency) - minimumMel()) / melSpan();
        }
    }
    return 0.0;
}

/// Returns the frequency at a share of the frequency axis, the inverse of frequencyToShare.
double shareToFrequency(const double share, const FrequencyScale scale)
{
    switch (scale) {
        case FrequencyScale::logarithmic: {
            return static_cast<double>(MIN_FREQUENCY) * std::pow(LOG_FREQUENCY_RATIO, share);
        }
        case FrequencyScale::mel: {
            return melToFrequency(minimumMel() + share * melSpan());
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
    const float ramp
        = std::log2(std::max(frequency, MIN_FREQUENCY) / HIGH_CONTRAST_START_FREQUENCY) * HIGH_CONTRAST_INV_OCTAVES;
    return contrast + HIGH_CONTRAST_BOOST * std::clamp(ramp, 0.0F, 1.0F);
}

/// Creates a builder with no particles and the frequency colour table.
SpectrogramParticleBuilder::SpectrogramParticleBuilder() : frequencyColours(buildFrequencyColours())
{
    resetLevel();
}

/// Returns the particles added since the last clear.
std::span<const SpectrogramParticle> SpectrogramParticleBuilder::particles() const noexcept
{
    return collected;
}

/// Turns the points of one analyzed frame into particles.
/// A particle's level in dB gets the spectral tilt,
/// and its share of the display range below the auto-scale reference goes through the contrast curve,
/// which keeps the noise floor dark and sets the particle's brightness, size, and halo.
/// A particle sits at its bin's reassigned frequency, so a sine's main lobe shares one horizontal position.
/// All tiers at one deadline use a common reference based on the latest peak retained for every tier.
/// The reference rises at once and releases by 12 dB per second of shared audio time.
/// A particle's weight is the share of a short flash its own tier's hop stands for.
/// This reduces brightness differences from tier cadence, but the nonlinear flash is not exactly cadence invariant.
/// Long persistence lets steady content build up for longer,
/// so the weight shrinks with the square root of the persistence to keep the picture from saturating.
void SpectrogramParticleBuilder::addFrame(const std::span<const SpectralPoint> points, const SpectralFrameInfo& frame)
{
    if (frame.tierIndex >= ANALYSIS_TIER_COUNT || !std::isfinite(frame.frameSeconds) || frame.frameSeconds <= 0.0
        || !std::isfinite(frame.endSeconds) || frame.endSeconds < lastEndSeconds)
    {
        return;
    }
    if (pendingCount > 0 && !juce::approximatelyEqual(frame.endSeconds, pendingEndSeconds)) {
        if (frame.endSeconds < pendingEndSeconds) {
            return;
        }
        finishDeadline();
    }
    auto& pending = pendingFrames[frame.tierIndex];
    // A tier contributes only once per deadline. Repeated or malformed metadata cannot overrun the group.
    for (std::size_t index = 0; index < pendingCount; ++index) {
        if (pendingOrder[index] == frame.tierIndex) {
            return;
        }
    }
    pending.points.clear();
    pending.tiltedDecibels.clear();
    pending.frameSeconds = frame.frameSeconds;
    float loudest = MIN_REFERENCE_DB;
    for (const auto& point : points) {
        if (!validPoint(point)) {
            continue;
        }
        const float tilt = TILT_DB_PER_OCTAVE * std::log2(point.frequency / TILT_PIVOT_FREQUENCY);
        const float decibels = DECIBELS_PER_POWER_DECADE * std::log10(std::max(point.level, SILENT_LEVEL)) + tilt;
        pending.points.push_back(point);
        pending.tiltedDecibels.push_back(decibels);
        loudest = std::max(loudest, decibels);
    }
    tierPeaks[frame.tierIndex] = loudest;
    pendingOrder[pendingCount++] = frame.tierIndex;
    pendingEndSeconds = frame.endSeconds;
    if (frame.lastAtTime) {
        finishDeadline();
    }
}

/// Accepts a sequential, single-tier stream whose hop also advances its audio clock.
/// Multi-tier analyzer clients must use the metadata overload so simultaneous hops are not counted twice.
void SpectrogramParticleBuilder::addFrame(const std::span<const SpectralPoint> points, const double frameSeconds)
{
    addFrame(points, SpectralFrameInfo {.frameSeconds = frameSeconds, .endSeconds = lastEndSeconds + frameSeconds});
}

/// Advances the shared reference once, after all retained tier peaks at this deadline have been updated.
void SpectrogramParticleBuilder::finishDeadline()
{
    const double elapsed = pendingEndSeconds - lastEndSeconds;
    const double released = static_cast<double>(referenceDecibels) - AUTO_SCALE_RELEASE_DB_PER_SECOND * elapsed;
    const float loudest = *std::ranges::max_element(tierPeaks);
    referenceDecibels = std::max(static_cast<float>(released), loudest);
    lastEndSeconds = pendingEndSeconds;
    for (std::size_t index = 0; index < pendingCount; ++index) {
        appendParticles(pendingFrames[pendingOrder[index]]);
    }
    pendingCount = 0;
}

/// Colours a pending tier with the common reference, retaining its own hop's flash weight.
void SpectrogramParticleBuilder::appendParticles(const PendingSpectralFrame& frame)
{
    const auto flash = static_cast<float>(
        (1.0 - std::exp(-frame.frameSeconds / FLASH_SECONDS))
        * std::sqrt(DEFAULT_PERSISTENCE_SECONDS / persistenceSeconds)
    );
    const float floorDecibels = referenceDecibels - DISPLAY_RANGE_DB;
    for (std::size_t index = 0; index < frame.points.size(); ++index) {
        const auto& point = frame.points[index];
        const float share = std::clamp((frame.tiltedDecibels[index] - floorDecibels) / DISPLAY_RANGE_DB, 0.0F, 1.0F);
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
            .y = (1.0F - std::clamp(point.position, -1.0F, 1.0F)) * 0.5F,
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
    tierPeaks.fill(MIN_REFERENCE_DB);
    lastEndSeconds = 0.0;
    pendingEndSeconds = 0.0;
    pendingCount = 0;
    for (auto& frame : pendingFrames) {
        frame.points.clear();
        frame.tiltedDecibels.clear();
    }
}

/// Sets the contrast of the level curve for new particles, from 0 for linear in dB to 1 for the steepest S-curve.
void SpectrogramParticleBuilder::setContrast(const double newContrast) noexcept
{
    if (std::isfinite(newContrast)) {
        contrast = std::clamp(newContrast, 0.0, 1.0);
    }
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
    if (std::isfinite(seconds)) {
        persistenceSeconds = std::clamp(seconds, MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS);
    }
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
