/// Read-only extraction of supported Serato Overview containers through TagLib.
/// Call from a worker thread. No audio decoding, metadata writing or caching occurs here.

#pragma once

#include "Cancellation.h"
#include "SeratoTagTypes.h"

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
}  // namespace audiobatch::serato