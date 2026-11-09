/// Captures ImageControl sound controls and reads or writes their strict, versioned portable XML representation.
/// Validation uses the actual parameter layout, without clamping invalid or incomplete preset data.

#include "ProcessingState.h"

#include "Parameters.h"
#include <system_error>

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace esgrove::image_control
{
namespace
{
/// Version and identity of the portable sound format, independent of host state XML.
constexpr int SOUND_STATE_VERSION {1};
constexpr auto SOUND_STATE_TAG {"SoundState"};
constexpr auto PLUGIN_IDENTITY {"com.esgrove.imagecontrol"};

/// Audition controls remain host parameters and local undo targets, but are never part of a stored sound.
bool isSoundParameter(const juce::String& parameterId)
{
    if (parameterId == BYPASS_ID || parameterId == DELTA_ID) {
        return false;
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        if (parameterId == soloParameterId(band)) {
            return false;
        }
    }
    return true;
}

/// Parses the entire attribute as a finite floating-point value, rather than accepting a numeric prefix.
std::optional<double> parseValue(const juce::String& text)
{
    const auto characters = text.toStdString();
    if (characters.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const std::span<const char> number(characters.data(), characters.size());
    const auto* end = std::to_address(number.end());
    const auto parsed = std::from_chars(number.data(), end, value);
    if (parsed.ec != std::errc {} || parsed.ptr != end || !std::isfinite(value)
        || value < -static_cast<double>(std::numeric_limits<float>::max())
        || value > static_cast<double>(std::numeric_limits<float>::max()))
    {
        return std::nullopt;
    }
    return value;
}
}  // namespace

/// Captures atomic plain-unit values without copying or listening to the mutable APVTS ValueTree.
/// The caller provides synchronization with host restores when a coherent multi-parameter snapshot is required.
ProcessingSnapshot captureProcessingState(const juce::AudioProcessorValueTreeState& parameters, const bool soundOnly)
{
    ProcessingSnapshot snapshot;
    for (const auto* parameter : parameters.processor.getParameters()) {
        const auto* identified = dynamic_cast<const juce::RangedAudioParameter*>(parameter);
        if (identified == nullptr || (soundOnly && !isSoundParameter(identified->paramID))) {
            continue;
        }
        if (const auto* value = parameters.getRawParameterValue(identified->paramID)) {
            snapshot.emplace(identified->paramID, value->load(std::memory_order_relaxed));
        }
    }
    return snapshot;
}

/// Serializes values with enough significant digits to round-trip every float exactly.
/// Returns null for non-finite input.
/// Callers validate the remaining snapshot against their processor before applying it.
std::unique_ptr<juce::XmlElement> processingStateToXml(const ProcessingSnapshot& snapshot)
{
    auto xml = std::make_unique<juce::XmlElement>(SOUND_STATE_TAG);
    xml->setAttribute("version", SOUND_STATE_VERSION);
    xml->setAttribute("plugin", PLUGIN_IDENTITY);
    for (const auto& [parameterId, value] : snapshot) {
        if (!std::isfinite(value)) {
            return nullptr;
        }
        auto child = std::make_unique<juce::XmlElement>("Param");
        child->setAttribute("id", parameterId);
        child->setAttribute("value", static_cast<double>(value));
        xml->addChildElement(child.release());
    }
    return xml;
}

/// Accepts only a complete known sound set in the current format, rejecting duplicates and unknown data.
/// Range checks precede float conversion, so a slightly out-of-range double cannot round into the legal range.
std::optional<ProcessingSnapshot> processingStateFromXml(
    const juce::XmlElement& xml,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    if (!xml.hasTagName(SOUND_STATE_TAG) || xml.getStringAttribute("version") != juce::String(SOUND_STATE_VERSION)
        || xml.getStringAttribute("plugin") != PLUGIN_IDENTITY || xml.getNumAttributes() != 2)
    {
        return std::nullopt;
    }
    const auto known = captureProcessingState(parameters);
    ProcessingSnapshot snapshot;
    for (const auto* child : xml.getChildIterator()) {
        if (!child->hasTagName("Param") || child->getNumAttributes() != 2 || child->getFirstChildElement() != nullptr
            || !child->hasAttribute("id") || !child->hasAttribute("value"))
        {
            return std::nullopt;
        }
        const auto parameterId = child->getStringAttribute("id");
        const auto value = parseValue(child->getStringAttribute("value"));
        if (!known.contains(parameterId) || !value.has_value()) {
            return std::nullopt;
        }
        const auto* parameter = parameters.getParameter(parameterId);
        const auto& range = parameter->getNormalisableRange();
        if (parameter->isBoolean() && !juce::exactlyEqual(*value, 0.0) && !juce::exactlyEqual(*value, 1.0)) {
            return std::nullopt;
        }
        if (*value < static_cast<double>(range.start) || *value > static_cast<double>(range.end)
            || !snapshot.emplace(parameterId, static_cast<float>(*value)).second)
        {
            return std::nullopt;
        }
    }
    if (snapshot.size() != known.size()) {
        return std::nullopt;
    }
    return snapshot;
}

/// Compares IDs and exact stored float values, so history and A/B never hide a small but real parameter change.
bool sameProcessingState(const ProcessingSnapshot& first, const ProcessingSnapshot& second)
{
    if (first.size() != second.size()) {
        return false;
    }
    auto other = second.begin();
    for (const auto& [parameterId, value] : first) {
        if (parameterId != other->first || !juce::exactlyEqual(value, other->second)) {
            return false;
        }
        ++other;
    }
    return true;
}
}  // namespace esgrove::image_control
