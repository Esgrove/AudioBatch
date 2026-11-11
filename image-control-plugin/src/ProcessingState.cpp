/// Captures ImageControl sound controls and reads or writes their strict, versioned portable XML representation.
/// Validation uses the actual parameter layout, without clamping invalid or incomplete preset data.

#include "ProcessingState.h"

#include "Parameters.h"
#include "UserPresets.h"
#include <system_error>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
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
/// Version and identity of the portable sound format, independent of host state XML.
constexpr int SOUND_STATE_VERSION {1};
constexpr auto SOUND_STATE_TAG {"SoundState"};
constexpr auto PLUGIN_IDENTITY {"com.esgrove.imagecontrol"};
/// Optional host-state siblings have independent version contracts.
constexpr auto COMPARISON_STATE_TAG {"ComparisonState"};
constexpr int COMPARISON_STATE_VERSION {1};
constexpr auto PRESET_PROVENANCE_STATE_TAG {"PresetProvenanceState"};
constexpr int PRESET_PROVENANCE_STATE_VERSION {1};
constexpr int UUID_HEX_CHARACTER_COUNT {32};
/// Comparison slots carry only their index, while provenance slots also carry preset identity and name.
constexpr int COMPARISON_SLOT_ATTRIBUTE_COUNT {1};
constexpr int PROVENANCE_SLOT_ATTRIBUTE_COUNT {3};

/// Finds exactly one root sibling, rejecting duplicates rather than choosing an arbitrary metadata revision.
const juce::XmlElement* uniqueChild(const juce::XmlElement& root, const juce::String& tag)
{
    const juce::XmlElement* result = nullptr;
    for (const auto* child : root.getChildIterator()) {
        if (child->hasTagName(tag)) {
            if (result != nullptr) {
                return nullptr;
            }
            result = child;
        }
    }
    return result;
}

/// Requires a canonical decimal slot index, without accepting numeric prefixes or out-of-range values.
std::optional<std::size_t> slotIndex(const juce::XmlElement& slot)
{
    const auto& identifier = slot.getStringAttribute("id");
    if (identifier != "0" && identifier != "1") {
        return std::nullopt;
    }
    return static_cast<std::size_t>(identifier.getIntValue());
}

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

/// Writes the unchanged version-one comparison contract, separately from optional provenance.
std::unique_ptr<juce::XmlElement> comparisonStateToXml(const ComparisonState& comparison)
{
    auto xml = std::make_unique<juce::XmlElement>(COMPARISON_STATE_TAG);
    xml->setAttribute("version", COMPARISON_STATE_VERSION);
    xml->setAttribute("active", comparison.active);
    for (std::size_t index = 0; index < comparison.slots.size(); ++index) {
        auto slot = std::make_unique<juce::XmlElement>("Slot");
        slot->setAttribute("id", static_cast<int>(index));
        slot->addChildElement(processingStateToXml(comparison.slots.at(index)).release());
        xml->addChildElement(slot.release());
    }
    return xml;
}

/// Validates the two comparison sounds as a unit, without rejecting independently usable host parameters.
std::optional<ComparisonState> comparisonStateFromXml(
    const juce::XmlElement& root,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    const auto* comparison = uniqueChild(root, COMPARISON_STATE_TAG);
    if (comparison == nullptr || comparison->getStringAttribute("version") != juce::String(COMPARISON_STATE_VERSION)
        || comparison->getNumAttributes() != 2)
    {
        return std::nullopt;
    }
    const auto active = comparison->getStringAttribute("active");
    if (active != "0" && active != "1") {
        return std::nullopt;
    }
    ComparisonState result;
    result.active = active.getIntValue();
    std::array<bool, 2> seen {};
    for (const auto* child : comparison->getChildIterator()) {
        const auto index = slotIndex(*child);
        const auto* sound = child->getFirstChildElement();
        if (!child->hasTagName("Slot") || child->getNumAttributes() != COMPARISON_SLOT_ATTRIBUTE_COUNT || !index
            || seen.at(*index) || sound == nullptr || sound->getNextElement() != nullptr)
        {
            return std::nullopt;
        }
        const auto snapshot = processingStateFromXml(*sound, parameters);
        if (!snapshot) {
            return std::nullopt;
        }
        result.slots.at(*index) = *snapshot;
        seen.at(*index) = true;
    }
    return seen[0] && seen[1] ? std::optional(result) : std::nullopt;
}

/// Writes optional per-slot provenance without paths, fingerprints, library access, or derived modified flags.
std::unique_ptr<juce::XmlElement> presetProvenanceStateToXml(const PresetProvenanceSlots& slots)
{
    auto xml = std::make_unique<juce::XmlElement>(PRESET_PROVENANCE_STATE_TAG);
    xml->setAttribute("version", PRESET_PROVENANCE_STATE_VERSION);
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (const auto& provenance = slots.at(index)) {
            auto slot = std::make_unique<juce::XmlElement>("Slot");
            slot->setAttribute("id", static_cast<int>(index));
            slot->setAttribute("presetId", provenance->id);
            slot->setAttribute("name", provenance->name);
            slot->addChildElement(processingStateToXml(provenance->baseline).release());
            if (provenance->effectiveBaseline) {
                auto effective = std::make_unique<juce::XmlElement>("EffectiveBaseline");
                effective->addChildElement(processingStateToXml(*provenance->effectiveBaseline).release());
                slot->addChildElement(effective.release());
            }
            xml->addChildElement(slot.release());
        }
    }
    return xml;
}

/// Restores provenance only when the entire independent sibling is supported and strictly valid.
/// Missing, ambiguous, malformed, or future metadata yields two unnamed slots without changing audio or comparison.
PresetProvenanceSlots presetProvenanceStateFromXml(
    const juce::XmlElement& root,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    const auto* metadata = uniqueChild(root, PRESET_PROVENANCE_STATE_TAG);
    if (metadata == nullptr || metadata->getNumAttributes() != 1
        || metadata->getStringAttribute("version") != juce::String(PRESET_PROVENANCE_STATE_VERSION))
    {
        return {};
    }
    PresetProvenanceSlots result;
    for (const auto* child : metadata->getChildIterator()) {
        const auto index = slotIndex(*child);
        const auto* sound = child->getFirstChildElement();
        if (!child->hasTagName("Slot") || child->getNumAttributes() != PROVENANCE_SLOT_ATTRIBUTE_COUNT || !index
            || result.at(*index) || sound == nullptr)
        {
            return {};
        }
        const auto baseline = processingStateFromXml(*sound, parameters);
        if (!baseline) {
            return {};
        }
        PresetProvenance provenance {
            .id = child->getStringAttribute("presetId"),
            .name = child->getStringAttribute("name"),
            .baseline = *baseline,
            .effectiveBaseline = std::nullopt,
        };
        if (const auto* effective = sound->getNextElement()) {
            const auto* effectiveSound = effective->getFirstChildElement();
            if (!effective->hasTagName("EffectiveBaseline") || effective->getNumAttributes() != 0
                || effective->getNextElement() != nullptr || effectiveSound == nullptr
                || effectiveSound->getNextElement() != nullptr)
            {
                return {};
            }
            provenance.effectiveBaseline = processingStateFromXml(*effectiveSound, parameters);
            if (!provenance.effectiveBaseline) {
                return {};
            }
        }
        if (!validPresetProvenance(provenance, parameters)) {
            return {};
        }
        result.at(*index) = std::move(provenance);
    }
    return result;
}

/// Validates detached identity, display name, and the complete finite baseline without filesystem access.
bool validPresetProvenance(const PresetProvenance& provenance, const juce::AudioProcessorValueTreeState& parameters)
{
    if (provenance.id.length() != UUID_HEX_CHARACTER_COUNT || !provenance.id.containsOnly("0123456789abcdef")
        || juce::Uuid(provenance.id).isNull() || UserPresets::validateName(provenance.name).failed())
    {
        return false;
    }
    if (provenance.effectiveBaseline) {
        const auto effective = processingStateToXml(*provenance.effectiveBaseline);
        if (effective == nullptr || !processingStateFromXml(*effective, parameters)) {
            return false;
        }
    }
    const auto xml = processingStateToXml(provenance.baseline);
    return xml != nullptr && processingStateFromXml(*xml, parameters).has_value();
}

/// Compares durable provenance only, leaving transient file revision rebinding outside host dirty tracking.
bool samePresetProvenance(const std::optional<PresetProvenance>& first, const std::optional<PresetProvenance>& second)
{
    if (!first || !second) {
        return first.has_value() == second.has_value();
    }
    return first->id == second->id && first->name == second->name
        && sameProcessingState(first->baseline, second->baseline);
}

/// Advances only reference values that exactly match the sound being recalled, preserving genuine edits.
/// Untouched controls can round during the same recall as a modified control, so references advance per ID.
void advancePresetRecallBaseline(
    PresetProvenance& provenance,
    const ProcessingSnapshot& target,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    const auto recalled = processingStateAfterRecall(target, parameters);
    auto reference = provenance.effectiveBaseline.value_or(provenance.baseline);
    for (auto& [parameterId, value] : reference) {
        const auto targetValue = target.find(parameterId);
        if (targetValue == target.end()) {
            continue;
        }
        const ProcessingSnapshot actual {{parameterId, targetValue->second}};
        const ProcessingSnapshot expected {{parameterId, value}};
        const bool matches = provenance.effectiveBaseline
            ? sameProcessingState(actual, expected)
            : matchesRecalledProcessingState(actual, expected, parameters);
        if (matches) {
            value = recalled.at(parameterId);
        }
    }
    provenance.effectiveBaseline = std::move(reference);
}

/// Predicts the exact APVTS readback of a sound recalled through the finite parameter boundary.
/// Parameters already equal to their target are not written, matching the processor's recall command.
ProcessingSnapshot processingStateAfterRecall(
    const ProcessingSnapshot& snapshot,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    auto result = snapshot;
    for (auto& [parameterId, value] : result) {
        if (const auto* parameter = parameters.getParameter(parameterId);
            parameter != nullptr && !juce::exactlyEqual(plainValue(*parameter), value))
        {
            const auto stored = parameter->convertFrom0to1(parameter->convertTo0to1(value));
            value = parameter->convertFrom0to1(parameter->convertTo0to1(stored));
        }
    }
    return result;
}

/// Compares exact live values with the baseline or its effective recalled value, never with a tolerance.
/// Recall converts plain to normalized, and FiniteParameter stores the converted plain value.
/// APVTS then reads getValue and denormalizes again.
/// Each conversion may round, so source floats are not always live floats.
/// Direct equality also accepts captured baselines and untouched defaults without forcing needless recalls.
bool matchesRecalledProcessingState(
    const ProcessingSnapshot& current,
    const ProcessingSnapshot& baseline,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    if (current.size() != baseline.size()) {
        return false;
    }
    return std::ranges::all_of(baseline, [&](const auto& entry) {
        const auto& [parameterId, expected] = entry;
        const auto actual = current.find(parameterId);
        const auto* parameter = parameters.getParameter(parameterId);
        if (actual == current.end() || parameter == nullptr) {
            return false;
        }
        if (juce::exactlyEqual(actual->second, expected)) {
            return true;
        }
        const auto stored = parameter->convertFrom0to1(parameter->convertTo0to1(expected));
        const auto recalled = parameter->convertFrom0to1(parameter->convertTo0to1(stored));
        return juce::exactlyEqual(actual->second, recalled);
    });
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
