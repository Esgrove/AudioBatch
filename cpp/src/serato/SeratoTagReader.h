/// Read-only extraction of supported Serato and standard track metadata through TagLib.
/// Call from a worker thread. No audio decoding, metadata writing or caching occurs here.

#pragma once

#include "serato/SeratoTags.h"
#include "utils/ThreadUtils.h"

namespace audiobatch::serato
{
[[nodiscard]] TagReadResult readTag(
    const juce::File& file,
    TagKind kind = TagKind::overview,
    const utils::CancellationCheck& isCancelled = {}
);

[[nodiscard]] std::vector<TagReadResult> readTags(
    const juce::File& file,
    std::span<const TagKind> kinds,
    const utils::CancellationCheck& isCancelled = {}
);
}  // namespace audiobatch::serato
