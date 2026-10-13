/// Shared platform-correct filename conversion for TagLib readers and metadata services.
/// This helper does not change the metadata read or write contract.

#pragma once

#include <juce_core/juce_core.h>

#include <tfile.h>

namespace audiobatch::utils
{
/// Converts a JUCE path to TagLib's platform filename representation.
/// Windows copies the wide buffer. Elsewhere the returned pointer aliases fullPath,
/// which must remain alive for the entire TagLib operation.
[[nodiscard]] inline TagLib::FileName tagLibFileName(const juce::String& fullPath)
{
#if JUCE_WINDOWS
    return {fullPath.toWideCharPointer()};
#else
    return fullPath.toRawUTF8();
#endif
}
}  // namespace audiobatch::utils
