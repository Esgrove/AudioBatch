/// Bounded optional waveform and spectrogram decoding for the GUI enrichment worker.
/// One PCM pass feeds the requested artifacts without a loudness meter or hidden decoder thread.
/// The streaming builders themselves live in analysis/, which fresh loudness analysis feeds directly.
/// Native thumbnail geometry, quantization and the empty final bucket retain JUCE's source-backed format.

#pragma once

#include "analysis/NativeWaveformBuilder.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <memory>
#include <optional>

namespace audiobatch
{
struct SpectrogramData;

/// Independently available audio artifacts, with no retained decoded PCM.
struct PreviewAudioArtifacts {
    /// Serialized native JUCE thumbnail, absent when not requested or when its optional build failed.
    std::optional<juce::MemoryBlock> waveform;
    /// Immutable bounded spectrogram, null when not requested or when its optional build failed.
    std::shared_ptr<const SpectrogramData> spectrogram;
};

[[nodiscard]] PreviewAudioArtifacts buildPreviewAudioArtifacts(
    juce::AudioFormatReader& reader,
    bool needWaveform,
    bool needSpectrogram,
    const utils::CancellationCheck& isCancelled = {}
);
[[nodiscard]] PreviewAudioArtifacts buildPreviewAudioArtifacts(
    const utils::FileIdentity& identity,
    bool needWaveform,
    bool needSpectrogram,
    const utils::CancellationCheck& isCancelled = {}
);

[[nodiscard]] std::optional<juce::MemoryBlock> buildNativeWaveform(
    juce::AudioFormatReader& reader,
    const utils::CancellationCheck& isCancelled = {}
);
[[nodiscard]] std::optional<juce::MemoryBlock> buildNativeWaveform(
    const utils::FileIdentity& identity,
    const utils::CancellationCheck& isCancelled = {}
);
}  // namespace audiobatch
