/// Defines portable, plain-unit sound snapshots shared by presets, A/B comparison, and local history.
/// Sound snapshots exclude audition controls and editor preferences.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <map>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
/// Parameter IDs mapped to their unnormalized, plain-unit values.
using ProcessingSnapshot = std::map<juce::String, float>;

/// Portable identity and immutable recall baseline, independent of a preset file's existence or exact revision.
struct PresetProvenance final {
    /// Canonical nonzero UUID of the explicitly selected preset.
    juce::String id;
    /// Validated display name retained even when the library entry is unavailable.
    juce::String name;
    /// Sound against which the selected slot's live sound is compared for its modified indicator.
    ProcessingSnapshot baseline;
    /// Exact live-parameter reference after controlled recalls, never substituted for the source snapshot.
    /// Absent in legacy version-one provenance, which uses the deterministic recall mapping until its next recall.
    std::optional<ProcessingSnapshot> effectiveBaseline;
};

/// Independent optional provenance for A and B, without filesystem paths or revision fingerprints.
using PresetProvenanceSlots = std::array<std::optional<PresetProvenance>, 2>;

/// Versioned comparison sounds and selection, independent of optional preset provenance.
struct ComparisonState final {
    /// Persistent A and B sounds, with the active sound refreshed from host parameters by the processor.
    std::array<ProcessingSnapshot, 2> slots;
    /// Zero selects A and one selects B.
    int active {0};
};

[[nodiscard]] std::unique_ptr<juce::XmlElement> comparisonStateToXml(const ComparisonState& comparison);
[[nodiscard]] std::optional<ComparisonState> comparisonStateFromXml(
    const juce::XmlElement& root,
    const juce::AudioProcessorValueTreeState& parameters
);
[[nodiscard]] std::unique_ptr<juce::XmlElement> presetProvenanceStateToXml(const PresetProvenanceSlots& slots);
[[nodiscard]] PresetProvenanceSlots presetProvenanceStateFromXml(
    const juce::XmlElement& root,
    const juce::AudioProcessorValueTreeState& parameters
);
[[nodiscard]] bool samePresetProvenance(
    const std::optional<PresetProvenance>& first,
    const std::optional<PresetProvenance>& second
);
[[nodiscard]] bool validPresetProvenance(
    const PresetProvenance& provenance,
    const juce::AudioProcessorValueTreeState& parameters
);

[[nodiscard]] ProcessingSnapshot captureProcessingState(
    const juce::AudioProcessorValueTreeState& parameters,
    bool soundOnly = true
);
[[nodiscard]] std::unique_ptr<juce::XmlElement> processingStateToXml(const ProcessingSnapshot& snapshot);
[[nodiscard]] std::optional<ProcessingSnapshot> processingStateFromXml(
    const juce::XmlElement& xml,
    const juce::AudioProcessorValueTreeState& parameters
);
[[nodiscard]] bool sameProcessingState(const ProcessingSnapshot& first, const ProcessingSnapshot& second);
void advancePresetRecallBaseline(
    PresetProvenance& provenance,
    const ProcessingSnapshot& target,
    const juce::AudioProcessorValueTreeState& parameters
);
[[nodiscard]] ProcessingSnapshot processingStateAfterRecall(
    const ProcessingSnapshot& snapshot,
    const juce::AudioProcessorValueTreeState& parameters
);
[[nodiscard]] bool matchesRecalledProcessingState(
    const ProcessingSnapshot& current,
    const ProcessingSnapshot& baseline,
    const juce::AudioProcessorValueTreeState& parameters
);
}  // namespace esgrove::image_control
