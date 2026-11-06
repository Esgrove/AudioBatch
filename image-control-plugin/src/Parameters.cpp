/// Implements the parameter layout of ImageControl, the parameter ids,
/// the conversion of band gains from dB, and the crossover ordering rule.

#include "Parameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace esgrove::image_control
{
namespace
{
/// Display names of the bands, from the lowest to the highest.
constexpr std::array<const char*, BAND_COUNT> BAND_NAMES {"Low", "Low Mid", "High Mid", "High"};
/// Display names of the crossovers, from the lowest to the highest.
constexpr std::array<const char*, CROSSOVER_COUNT> CROSSOVER_NAMES {"Low Crossover", "Mid Crossover", "High Crossover"};
/// Id stems of the crossover parameters, from the lowest to the highest.
constexpr std::array<const char*, CROSSOVER_COUNT> CROSSOVER_IDS {"crossover_low", "crossover_mid", "crossover_high"};
/// Version hint of every parameter id, for hosts that use it to recognise parameters across plugin versions.
constexpr int PARAMETER_VERSION {1};
/// Frequencies in kHz below this are shown with two decimals, and higher ones with one.
constexpr float TWO_DECIMALS_BELOW_KILOHERTZ {10.0F};
/// Tenths of a dB in one dB, for showing gains with one decimal.
constexpr float TENTHS_PER_DECIBEL {10.0F};
/// Step of the gain parameters in dB.
constexpr float GAIN_STEP_DB {0.01F};

/// Formats a crossover parameter value for the host.
juce::String formatCrossoverValue(const float frequency, const int /*maximumLength*/)
{
    return formatFrequency(frequency);
}

/// Formats a band gain, showing the bottom of the range as off.
juce::String formatBandGain(const float decibels, const int /*maximumLength*/)
{
    if (decibels <= BAND_GAIN_MIN_DB) {
        return "-inf dB";
    }
    // Rounding to the shown tenth and adding zero turns a negative zero into a positive one,
    // so a gain a hair below 0 dB on the parameter grid shows as 0.0 dB, not -0.0 dB.
    const float shown = std::round(decibels * TENTHS_PER_DECIBEL) / TENTHS_PER_DECIBEL + 0.0F;
    return juce::String(shown, 1) + " dB";
}

/// Converts a gain in dB to linear, treating values within half a parameter step of 0 dB as exactly unity.
/// The parameter grid starts at the bottom of the range, so its 0 dB step comes back with a rounding error,
/// and an exact unity gain is what lets the imager pass the input through bit for bit.
float gainFromDecibels(const float decibels, const float minusInfinityDecibels)
{
    if (std::abs(decibels) < GAIN_STEP_DB * 0.5F) {
        return 1.0F;
    }
    return juce::Decibels::decibelsToGain(decibels, minusInfinityDecibels);
}

/// Creates a gain parameter in dB with a 0 dB default.
std::unique_ptr<juce::AudioParameterFloat> makeGainParameter(
    const juce::String& parameterId,
    const juce::String& name,
    const float minDecibels,
    const float maxDecibels
)
{
    return std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID {parameterId, PARAMETER_VERSION},
        name,
        gainRange(minDecibels, maxDecibels),
        0.0F,
        juce::AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(formatBandGain)
    );
}
}  // namespace

/// Creates the crossover, band gain, output gain, and bypass parameters.
/// The band gains are in dB, and their lowest value turns the signal off.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        layout.add(
            std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID {crossoverParameterId(crossover), PARAMETER_VERSION},
                CROSSOVER_NAMES.at(crossover),
                crossoverRange(),
                DEFAULT_CROSSOVERS.at(crossover),
                juce::AudioParameterFloatAttributes().withLabel("Hz").withStringFromValueFunction(formatCrossoverValue)
            )
        );
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        layout.add(
            makeGainParameter(midGainParameterId(band), bandName(band) + ": Mid", BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB),
            makeGainParameter(sideGainParameterId(band), bandName(band) + ": Side", BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB)
        );
    }
    layout.add(makeGainParameter(OUTPUT_GAIN_ID, "Output Gain", OUTPUT_GAIN_MIN_DB, OUTPUT_GAIN_MAX_DB));
    layout.add(
        std::make_unique<juce::AudioParameterBool>(juce::ParameterID {BYPASS_ID, PARAMETER_VERSION}, "Bypass", false)
    );
    return layout;
}

/// Returns the range of the crossover parameters: a logarithmically skewed range from MIN_FREQUENCY to MAX_FREQUENCY.
juce::NormalisableRange<float> crossoverRange()
{
    juce::NormalisableRange<float> range(MIN_FREQUENCY, MAX_FREQUENCY);
    range.setSkewForCentre(std::sqrt(MIN_FREQUENCY * MAX_FREQUENCY));
    return range;
}

/// Returns the range of a gain parameter in dB, in steps of a hundredth of a dB.
juce::NormalisableRange<float> gainRange(const float minDecibels, const float maxDecibels)
{
    return {minDecibels, maxDecibels, GAIN_STEP_DB};
}

/// Returns the id of the crossover with the given index, counted from the lowest.
juce::String crossoverParameterId(const std::size_t crossover)
{
    return CROSSOVER_IDS.at(crossover);
}

/// Returns the id of the mid gain parameter of a band, counted from the lowest band, such as `band1_mid`.
juce::String midGainParameterId(const std::size_t band)
{
    return "band" + juce::String(band + 1) + "_mid";
}

/// Returns the id of the side gain parameter of a band, counted from the lowest band, such as `band1_side`.
juce::String sideGainParameterId(const std::size_t band)
{
    return "band" + juce::String(band + 1) + "_side";
}

/// Returns the display name of a band, counted from the lowest.
juce::String bandName(const std::size_t band)
{
    return BAND_NAMES.at(band);
}

/// Returns the name of the editor setting that holds whether a band's Link keeps Side moving with Mid.
juce::Identifier linkPropertyName(const std::size_t band)
{
    return "link" + juce::String(band + 1);
}

/// Orders requested crossover frequencies so the bands keep their order and a minimum width.
/// Each crossover stays within the frequency range and at least MIN_CROSSOVER_RATIO above the one below it,
/// leaving room for the crossovers above it.
/// The DSP and the editor both apply this rule, so the display matches what is processed.
Crossovers orderCrossovers(const Crossovers& requested)
{
    Crossovers ordered {};
    float lowerLimit = MIN_FREQUENCY;
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const auto crossoversAbove = static_cast<float>(CROSSOVER_COUNT - 1 - crossover);
        const float upperLimit = MAX_FREQUENCY / std::pow(MIN_CROSSOVER_RATIO, crossoversAbove);
        ordered.at(crossover) = std::clamp(requested.at(crossover), lowerLimit, upperLimit);
        lowerLimit = ordered.at(crossover) * MIN_CROSSOVER_RATIO;
    }
    return ordered;
}

/// Formats a frequency in Hz below 1 kHz and in kHz above, with two decimals below 10 kHz and one above.
juce::String formatFrequency(const float frequency)
{
    // Comparing the rounded value keeps a frequency just below 1 kHz from showing as 1000 Hz.
    if (static_cast<float>(juce::roundToInt(frequency)) < HERTZ_PER_KILOHERTZ) {
        return juce::String(juce::roundToInt(frequency)) + " Hz";
    }
    const float kilohertz = frequency / HERTZ_PER_KILOHERTZ;
    return juce::String(kilohertz, kilohertz < TWO_DECIMALS_BELOW_KILOHERTZ ? 2 : 1) + " kHz";
}

/// Converts a band gain from dB to a linear gain, with the lowest value of the range turning the signal off.
float bandGainFromDecibels(const float decibels)
{
    return gainFromDecibels(decibels, BAND_GAIN_MIN_DB);
}

/// Converts the output gain from dB to a linear gain.
float outputGainFromDecibels(const float decibels)
{
    return gainFromDecibels(decibels, OUTPUT_GAIN_MIN_DB - 1.0F);
}
/// Returns whether two sets of crossovers are the same.
bool sameCrossovers(const Crossovers& first, const Crossovers& second)
{
    return std::ranges::equal(first, second, [](const float left, const float right) {
        return juce::approximatelyEqual(left, right);
    });
}

/// Returns the Width of a band, its side gain relative to its mid gain as a linear ratio,
/// from the two gains in dB, or 0 when the side is off.
double bandWidthFromGains(const double midDecibels, const double sideDecibels)
{
    if (sideDecibels <= static_cast<double>(BAND_GAIN_MIN_DB)) {
        return 0.0;
    }
    return juce::Decibels::decibelsToGain(sideDecibels - midDecibels);
}

/// Returns the plain value of a parameter, such as a frequency in Hz or a gain in dB.
float plainValue(const juce::RangedAudioParameter& parameter)
{
    return parameter.convertFrom0to1(parameter.getValue());
}

/// Returns how many times a sample rate doubles above a reference rate, or 0 at or below it.
/// Processing and analysis lengths double with each doubling, so their frequency resolution stays the same.
int sampleRateDoublings(const double sampleRate, const double referenceSampleRate)
{
    return std::max(0, static_cast<int>(std::ceil(std::log2(sampleRate / referenceSampleRate))));
}

/// Sets a parameter to a plain value in a host change gesture of its own.
void setParameterInGesture(juce::RangedAudioParameter& parameter, const float value)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    parameter.endChangeGesture();
}
}  // namespace esgrove::image_control
