/// Implements the parameter layout of ImageControl, the parameter ids,
/// the conversion of band gains from dB and of band pans from percent, and the crossover ordering rule.

#include "Parameters.h"

#include <system_error>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

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
/// Version hint of the parameters of the first version,
/// for hosts that use it to recognise parameters across plugin versions.
constexpr int PARAMETER_VERSION {1};
/// Version hint of the pan parameters, which came after the first version,
/// so hosts such as Audio Unit hosts tell them apart from the parameters they already know.
constexpr int PAN_PARAMETER_VERSION {2};
/// Version hint of the delta parameter, which came after the pan parameters.
constexpr int DELTA_PARAMETER_VERSION {3};
/// Version hint of the solo parameters, which came after the delta parameter.
constexpr int SOLO_PARAMETER_VERSION {4};
/// Version hint of the mono parameter, ordered after the existing host parameters.
constexpr int MONO_PARAMETER_VERSION {5};
/// Version hint of the independent Width parameters, appended after Mono.
constexpr int WIDTH_PARAMETER_VERSION {6};
/// Version hint of the Recover parameters, appended after every Width parameter.
constexpr int RECOVER_PARAMETER_VERSION {7};
/// Step of the independent linear side multiplier.
constexpr float WIDTH_STEP {0.01F};
/// Step of the host-facing recovery amount, in percent.
constexpr float RECOVER_STEP_PERCENT {1.0F};
/// Step of the pan parameters, in percent.
constexpr float PAN_STEP_PERCENT {1.0F};
/// Frequencies in kHz below this are shown with two decimals, and higher ones with one.
constexpr float TWO_DECIMALS_BELOW_KILOHERTZ {10.0F};
/// Tenths of a dB in one dB, for showing gains with one decimal.
constexpr float TENTHS_PER_DECIBEL {10.0F};
/// Step of the gain parameters in dB.
constexpr float GAIN_STEP_DB {0.01F};
/// Precomputed upper bounds for each ordered crossover so neighboring bands stay at least MIN_CROSSOVER_RATIO apart.
constexpr std::array<float, CROSSOVER_COUNT> CROSSOVER_UPPER_LIMITS {
    MAX_FREQUENCY / (MIN_CROSSOVER_RATIO * MIN_CROSSOVER_RATIO),
    MAX_FREQUENCY / MIN_CROSSOVER_RATIO,
    MAX_FREQUENCY,
};

/// Text grammar and host metadata of a finite parameter.
enum class ParameterKind : std::uint8_t { frequency, gain, pan, width, recovery, boolean };

/// Parses a whole finite numeric token, rejecting empty input, trailing junk, and overflow.
std::optional<float> finiteNumber(const juce::String& text)
{
    const auto token = text.trim().toStdString();
    if (token.empty()) {
        return std::nullopt;
    }
    const std::span<const char> characters(token.data(), token.size());
    const auto number = characters.front() == '+' ? characters.subspan(1) : characters;
    const auto* last = std::to_address(number.end());
    float result = 0.0F;
    const auto parsed = std::from_chars(number.data(), last, result);
    if (parsed.ec != std::errc {} || parsed.ptr != last || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

/// Parses an optional pan side or percent suffix without accepting trailing junk.
std::optional<float> panValueFromText(juce::String token)
{
    if (token == "c") {
        return 0.0F;
    }
    if (token.endsWithChar('l') || token.endsWithChar('r')) {
        const bool left = token.endsWithChar('l');
        const auto value = finiteNumber(token.dropLastCharacters(1));
        if (!value) {
            return std::nullopt;
        }
        return left ? -std::abs(*value) : std::abs(*value);
    }
    if (token.endsWithChar('%')) {
        token = token.dropLastCharacters(1);
    }
    return finiteNumber(token);
}

/// Parses independent Width and Recover amounts, with percent units and an explicit recovery Off value.
std::optional<float> widthRecoveryValueFromText(juce::String token, const ParameterKind kind)
{
    if (kind == ParameterKind::recovery && token == "off") {
        return 0.0F;
    }
    float multiplier = 1.0F;
    if (token.endsWithChar('%')) {
        token = token.dropLastCharacters(1);
        if (kind == ParameterKind::width) {
            multiplier = 1.0F / PERCENT_PER_UNIT;
        }
    }
    const auto value = finiteNumber(token);
    if (!value || !std::isfinite(*value * multiplier)) {
        return std::nullopt;
    }
    return *value * multiplier;
}

/// Parses the supported units and pan notation without letting invalid text become a zero value.
std::optional<float> parameterValueFromText(const juce::String& text, const ParameterKind kind)
{
    auto token = text.trim().toLowerCase();
    float multiplier = 1.0F;
    if (kind == ParameterKind::frequency) {
        if (token.endsWith("khz")) {
            multiplier = HERTZ_PER_KILOHERTZ;
            token = token.dropLastCharacters(3);
        } else if (token.endsWith("hz")) {
            token = token.dropLastCharacters(2);
        }
    } else if (kind == ParameterKind::gain) {
        if (token.endsWith("db")) {
            token = token.dropLastCharacters(2).trim();
        }
        if (token == "-inf" || token == "-infinity") {
            return BAND_GAIN_MIN_DB;
        }
    } else if (kind == ParameterKind::pan) {
        return panValueFromText(token);
    } else if (kind == ParameterKind::width || kind == ParameterKind::recovery) {
        return widthRecoveryValueFromText(token, kind);
    } else {
        if (token == "on" || token == "true") {
            return 1.0F;
        }
        if (token == "off" || token == "false") {
            return 0.0F;
        }
    }
    const auto value = finiteNumber(token);
    if (!value || !std::isfinite(*value * multiplier)) {
        return std::nullopt;
    }
    return *value * multiplier;
}

/// Parameter boundary that rejects invalid host values and preserves the current value on invalid text.
/// The raw value read by APVTS always comes from this finite atomic, including after a rejected notification.
class FiniteParameter final : public juce::RangedAudioParameter
{
public:
    /// Keeps the public ids, version hints, ranges, and display attributes of the host parameter.
    FiniteParameter(
        const juce::ParameterID& parameterId,
        const juce::String& parameterName,
        juce::NormalisableRange<float> range,
        const float defaultValue,
        const juce::AudioParameterFloatAttributes& attributes,
        const ParameterKind parameterKind
    ) :
        juce::RangedAudioParameter(parameterId, parameterName, attributes.getAudioProcessorParameterWithIDAttributes()),
        valueRange(std::move(range)),
        formatter(attributes.getStringFromValueFunction()),
        value(defaultValue),
        kind(parameterKind),
        initialValue(defaultValue)
    {}

    /// Exposes the original range for attachments and host normalization.
    const juce::NormalisableRange<float>& getNormalisableRange() const override
    {
        return valueRange;
    }

    /// Reports boolean host metadata without exposing an unsafe bool conversion of NaN.
    bool isBoolean() const override
    {
        return kind == ParameterKind::boolean;
    }
    /// Boolean parameters are discrete. Continuous controls preserve their original host metadata.
    bool isDiscrete() const override
    {
        return isBoolean();
    }
    /// Boolean parameters have exactly two host steps.
    int getNumSteps() const override
    {
        return isBoolean() ? 2 : getDefaultNumParameterSteps();
    }

    /// Returns the finite value normalized for the host and APVTS.
    float getValue() const override
    {
        return convertTo0to1(value.load(std::memory_order_relaxed));
    }
    /// Non-finite automation is ignored before range conversion can assert or poison DSP state.
    void setValue(const float normalized) override
    {
        if (!std::isfinite(normalized)) {
            return;
        }
        const auto plain = convertFrom0to1(std::clamp(normalized, 0.0F, 1.0F));
        if (isBoolean()) {
            value.store(plain >= 0.5F ? 1.0F : 0.0F, std::memory_order_relaxed);
        } else {
            value.store(plain, std::memory_order_relaxed);
        }
    }
    /// Keeps the default independent of the current parameter state.
    float getDefaultValue() const override
    {
        return convertTo0to1(initialValue);
    }
    /// Formats a finite host value using the control's units.
    juce::String getText(const float normalized, const int length) const override
    {
        const auto plain
            = std::isfinite(normalized) ? convertFrom0to1(normalized) : value.load(std::memory_order_relaxed);
        if (isBoolean()) {
            return plain >= 0.5F ? "On" : "Off";
        }
        return formatter ? formatter(plain, length) : juce::String(plain, 2);
    }
    /// Returns the current normalized value on rejection, so text edits do not jump to zero.
    float getValueForText(const juce::String& text) const override
    {
        const auto plain = parameterValueFromText(text, kind);
        return plain ? convertTo0to1(*plain) : getValue();
    }

private:
    /// Range shared by host normalization and attachments.
    juce::NormalisableRange<float> valueRange;
    /// Unit-specific formatting without a permissive default parser.
    std::function<juce::String(float, int)> formatter;
    /// Finite plain value read by the host and APVTS.
    std::atomic<float> value;
    /// Selects units and boolean metadata.
    ParameterKind kind;
    /// Declared default independent of current edits.
    float initialValue;
};

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

/// Formats a pan parameter value for the host.
juce::String formatPanValue(const float percent, const int /*maximumLength*/)
{
    return formatPan(percent);
}

/// Formats Width as the independent linear multiplier, preserving its hundredth-step precision.
juce::String formatWidthValue(const float width, const int /*maximumLength*/)
{
    return {width, 2};
}

/// Formats recovery as a whole percentage with an adjacent percent sign, including its zero default.
juce::String formatRecoveryValue(const float percent, const int /*maximumLength*/)
{
    return juce::String(juce::roundToInt(percent)) + "%";
}

/// Converts a gain in dB to linear, treating values within half a parameter step of 0 dB as exactly unity.
/// The parameter grid starts at the bottom of the range, so its 0 dB step comes back with a rounding error,
/// and an exact unity gain is what lets the imager pass the input through bit for bit.
float gainFromDecibels(const float decibels, const float minusInfinityDecibels)
{
    if (!std::isfinite(decibels) || std::abs(decibels) < GAIN_STEP_DB * 0.5F) {
        return 1.0F;
    }
    return juce::Decibels::decibelsToGain(decibels, minusInfinityDecibels);
}

/// Creates a gain parameter in dB with a 0 dB default.
std::unique_ptr<FiniteParameter> makeGainParameter(
    const juce::String& parameterId,
    const juce::String& name,
    const float minDecibels,
    const float maxDecibels
)
{
    return std::make_unique<FiniteParameter>(
        juce::ParameterID {parameterId, PARAMETER_VERSION},
        name,
        gainRange(minDecibels, maxDecibels),
        0.0F,
        juce::AudioParameterFloatAttributes().withLabel("dB").withStringFromValueFunction(formatBandGain),
        ParameterKind::gain
    );
}
}  // namespace

/// Creates the crossover, band gain, pan, solo, output gain, bypass, Delta, Mono, Width, and Recover parameters.
/// All Width parameters follow Mono, then all Recover parameters, preserving existing host indices.
/// The band gains are in dB, and their lowest value turns the signal off.
/// The band pans are in percent, from fully left at -100 through the centre at 0 to fully right at 100.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        layout.add(
            std::make_unique<FiniteParameter>(
                juce::ParameterID {crossoverParameterId(crossover), PARAMETER_VERSION},
                CROSSOVER_NAMES.at(crossover),
                crossoverRange(),
                DEFAULT_CROSSOVERS.at(crossover),
                juce::AudioParameterFloatAttributes().withLabel("Hz").withStringFromValueFunction(formatCrossoverValue),
                ParameterKind::frequency
            )
        );
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        layout.add(
            makeGainParameter(midGainParameterId(band), bandName(band) + ": Mid", BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB),
            makeGainParameter(sideGainParameterId(band), bandName(band) + ": Side", BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB),
            std::make_unique<FiniteParameter>(
                juce::ParameterID {panParameterId(band), PAN_PARAMETER_VERSION},
                bandName(band) + ": Pan",
                panRange(),
                0.0F,
                juce::AudioParameterFloatAttributes().withStringFromValueFunction(formatPanValue),
                ParameterKind::pan
            )
        );
        layout.add(
            std::make_unique<FiniteParameter>(
                juce::ParameterID {soloParameterId(band), SOLO_PARAMETER_VERSION},
                bandName(band) + ": Solo",
                juce::NormalisableRange<float>(0.0F, 1.0F, 1.0F),
                0.0F,
                juce::AudioParameterFloatAttributes(),
                ParameterKind::boolean
            )
        );
    }
    layout.add(makeGainParameter(OUTPUT_GAIN_ID, "Output Gain", OUTPUT_GAIN_MIN_DB, OUTPUT_GAIN_MAX_DB));
    layout.add(
        std::make_unique<FiniteParameter>(
            juce::ParameterID {BYPASS_ID, PARAMETER_VERSION},
            "Bypass",
            juce::NormalisableRange<float>(0.0F, 1.0F, 1.0F),
            0.0F,
            juce::AudioParameterFloatAttributes(),
            ParameterKind::boolean
        )
    );
    layout.add(
        std::make_unique<FiniteParameter>(
            juce::ParameterID {DELTA_ID, DELTA_PARAMETER_VERSION},
            "Delta",
            juce::NormalisableRange<float>(0.0F, 1.0F, 1.0F),
            0.0F,
            juce::AudioParameterFloatAttributes(),
            ParameterKind::boolean
        )
    );
    layout.add(
        std::make_unique<FiniteParameter>(
            juce::ParameterID {MONO_ID, MONO_PARAMETER_VERSION},
            "Mono",
            juce::NormalisableRange<float>(0.0F, 1.0F, 1.0F),
            0.0F,
            juce::AudioParameterFloatAttributes(),
            ParameterKind::boolean
        )
    );
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        layout.add(
            std::make_unique<FiniteParameter>(
                juce::ParameterID {widthParameterId(band), WIDTH_PARAMETER_VERSION},
                bandName(band) + ": Width",
                juce::NormalisableRange<float>(0.0F, WIDTH_MAX, WIDTH_STEP),
                1.0F,
                juce::AudioParameterFloatAttributes().withStringFromValueFunction(formatWidthValue),
                ParameterKind::width
            )
        );
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        layout.add(
            std::make_unique<FiniteParameter>(
                juce::ParameterID {recoverParameterId(band), RECOVER_PARAMETER_VERSION},
                bandName(band) + ": Recover",
                juce::NormalisableRange<float>(0.0F, RECOVER_MAX_PERCENT, RECOVER_STEP_PERCENT),
                0.0F,
                juce::AudioParameterFloatAttributes().withLabel("%").withStringFromValueFunction(formatRecoveryValue),
                ParameterKind::recovery
            )
        );
    }
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

/// Returns the range of the pan parameters: -100 to 100 percent in whole steps, so the centre is exactly 0.
juce::NormalisableRange<float> panRange()
{
    return {-PAN_MAX_PERCENT, PAN_MAX_PERCENT, PAN_STEP_PERCENT};
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

/// Returns the id of the pan parameter of a band, counted from the lowest band, such as `band1_pan`.
juce::String panParameterId(const std::size_t band)
{
    return "band" + juce::String(band + 1) + "_pan";
}

/// Returns the id of the solo parameter of a band, counted from the lowest band, such as `band1_solo`.
juce::String soloParameterId(const std::size_t band)
{
    return "band" + juce::String(band + 1) + "_solo";
}

/// Returns the stable id of the band's independent Width parameter, such as `band1_width`.
juce::String widthParameterId(const std::size_t band)
{
    return "band" + juce::String(band + 1) + "_width";
}

/// Returns the stable id of the band's recovery amount, such as `band1_recover`.
juce::String recoverParameterId(const std::size_t band)
{
    return "band" + juce::String(band + 1) + "_recover";
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
        const float upperLimit = CROSSOVER_UPPER_LIMITS.at(crossover);
        const auto frequency
            = std::isfinite(requested.at(crossover)) ? requested.at(crossover) : DEFAULT_CROSSOVERS.at(crossover);
        ordered.at(crossover) = std::clamp(frequency, lowerLimit, upperLimit);
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

/// Formats a pan in percent as `C` at the centre, and otherwise as the rounded percent and the side,
/// such as `35L` or `100R`.
juce::String formatPan(const float percent)
{
    const int rounded = juce::roundToInt(percent);
    if (rounded == 0) {
        return "C";
    }
    return juce::String(std::abs(rounded)) + (rounded < 0 ? "L" : "R");
}

/// Converts a pan in percent to the pan the imager applies, from -1 for fully left to 1 for fully right.
/// Values within half a parameter step of the centre are exactly 0,
/// which lets the imager pass the input through bit for bit.
float panFromPercent(const float percent)
{
    if (!std::isfinite(percent) || std::abs(percent) < PAN_STEP_PERCENT * 0.5F) {
        return 0.0F;
    }
    return std::clamp(percent / PAN_MAX_PERCENT, -1.0F, 1.0F);
}

/// Parses a pan typed by the user into percent:
/// `C` is the centre, a number followed by `L` or `R` is that far to the left or right,
/// and a plain number is a signed percent, negative to the left.
/// Invalid text returns NaN for callers to reject. The parameter parser preserves the previous value instead.
float panPercentFromText(const juce::String& text)
{
    const auto percent = parameterValueFromText(text, ParameterKind::pan);
    return percent ? std::clamp(*percent, -PAN_MAX_PERCENT, PAN_MAX_PERCENT) : std::numeric_limits<float>::quiet_NaN();
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
/// Returns how much of each band is heard from the solo states:
/// every band when no band or every band is soloed, and otherwise only the soloed bands.
/// Soloing every band is the same as soloing none, while each band keeps its own solo state,
/// so unsoloing some of them afterwards leaves the others soloed.
BandGains soloGains(const BandSolos& soloed)
{
    BandGains gains {};
    const bool anySoloed = std::ranges::any_of(soloed, std::identity {});
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        gains.at(band) = !anySoloed || soloed.at(band) ? 1.0F : 0.0F;
    }
    return gains;
}

/// Returns whether two sets of crossovers are the same.
bool sameCrossovers(const Crossovers& first, const Crossovers& second)
{
    return std::ranges::equal(first, second, [](const float left, const float right) {
        return juce::approximatelyEqual(left, right);
    });
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
    if (!std::isfinite(sampleRate) || !std::isfinite(referenceSampleRate) || sampleRate <= 0.0
        || referenceSampleRate <= 0.0 || sampleRate <= referenceSampleRate)
    {
        return 0;
    }
    constexpr double maxDoublings {16.0};
    return static_cast<int>(std::min(maxDoublings, std::ceil(std::log2(sampleRate / referenceSampleRate))));
}

/// Sets a parameter to a plain value in a host change gesture of its own.
void setParameterInGesture(juce::RangedAudioParameter& parameter, const float value)
{
    if (!std::isfinite(value)) {
        return;
    }
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    parameter.endChangeGesture();
}
}  // namespace esgrove::image_control
