/// Read-only extraction of supported Serato and standard track metadata through TagLib.
/// Call from a worker thread. No audio decoding, metadata writing or caching occurs here.

#pragma once

#include "Cancellation.h"
#include "SeratoTags.h"

namespace audiobatch::serato
{
/// Reads one requested kind from MPEG, AIFF or WAV ID3v2 GEOB, or native FLAC comments.
/// The first valid duplicate wins. Ogg and other containers are unsupported.
/// Cancellation is checked around TagLib operations, which cannot be interrupted internally.
/// Missing/unreadable files return ioError, never a stable absent result.
[[nodiscard]] TagReadResult readTag(
    const juce::File& file,
    TagKind kind = TagKind::overview,
    const utils::CancellationCheck& isCancelled = {}
);

/// Opens a supported file once and returns independent outcomes in requested order, including duplicates.
/// Valid later duplicates recover invalid earlier ones. Markers2 takes precedence over legacy fallback.
/// Requests above maxTagRecords are rejected with an empty vector. Empty requests perform no I/O.
/// Cancellation never turns a transient failure into a stable negative result.
[[nodiscard]] std::vector<TagReadResult> readTags(
    const juce::File& file,
    std::span<const TagKind> kinds,
    const utils::CancellationCheck& isCancelled = {}
);
}  // namespace audiobatch::serato
