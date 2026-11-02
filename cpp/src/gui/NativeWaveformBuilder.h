/// Bounded waveform-only decoding for the GUI enrichment worker.
/// Produces the native JUCE thumbnail encoding without an AudioThumbnail source or hidden time-slice decoder.
/// Geometry, quantization and the unfilled partial-final bucket follow JUCE's source-backed thumbnail format.

#pragma once

#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <optional>

namespace audiobatch
{
[[nodiscard]] std::optional<juce::MemoryBlock> buildNativeWaveform(
    juce::AudioFormatReader& reader,
    const utils::CancellationCheck& isCancelled = {}
);
[[nodiscard]] std::optional<juce::MemoryBlock> buildNativeWaveform(
    const utils::FileIdentity& identity,
    const utils::CancellationCheck& isCancelled = {}
);

[[nodiscard]] int samplesPerThumbnailPoint(juce::int64 lengthInSamples);
}  // namespace audiobatch
