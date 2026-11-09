/// Defines portable, plain-unit sound snapshots shared by presets, A/B comparison, and local history.
/// Sound snapshots exclude audition controls and editor preferences.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
/// Parameter IDs mapped to their unnormalized, plain-unit values.
using ProcessingSnapshot = std::map<juce::String, float>;

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
}  // namespace esgrove::image_control
