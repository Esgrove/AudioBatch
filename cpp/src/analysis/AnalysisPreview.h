/// Transient selected-file previews produced by the foreground analysis decoder.
/// Snapshots carry source identity, run, and selection generation so consumers can reject stale work.
/// They are independent of completed analysis records and must never be persisted as complete waveforms.

#pragma once

#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <cstdint>
#include <optional>

namespace audiobatch
{
/// One bounded, replaceable waveform snapshot for the currently selected source.
struct AnalysisPreview {
    /// Strong source identity captured before the decoder opens the file.
    utils::FileIdentity identity;
    /// Run that decoded these samples, still current when the snapshot is taken.
    int runId {0};
    /// Selection request that owns this snapshot, including same-path reselections.
    std::uint64_t selectionGeneration {0};
    /// Number of decoded source frames represented, never beyond the source length.
    std::int64_t decodedFrames {0};
    /// Serialized partial native waveform, absent when optional snapshot construction is unavailable.
    std::optional<juce::MemoryBlock> waveform;
};
}  // namespace audiobatch
