/// libFuzzer harness for the Serato tag payload parsers and their cache encoding.
/// The first input byte selects the tag kind and the marker layout, and the remaining bytes are the payload.
/// A payload that parses must serialize, deserialize, and serialize again to the same cache bytes.

#include "serato/SeratoTags.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <tuple>

namespace audiobatch
{
namespace
{
/// Number of tag kinds the selector byte chooses from, which are numbered from one.
constexpr std::uint8_t tagKindCount {6};
/// Selector bit that chooses the legacy marker layout instead of Markers2.
constexpr std::uint8_t legacyMarkersBit {0x80};
/// Selector bits that choose the tag kind.
constexpr std::uint8_t tagKindBits {0x7f};

/// Runs the parsers and the cache codec on one fuzz input.
/// A cache round trip that loses or changes a parsed tag aborts, which libFuzzer reports as a crash.
void fuzzTagPayload(const std::span<const std::uint8_t> input)
{
    if (input.empty()) {
        return;
    }
    const auto selector = input.front();
    const auto payload = input.subspan(1);
    const auto legacyMarkers = (selector & legacyMarkersBit) != 0;
    const auto kind = static_cast<serato::TagKind>((selector & tagKindBits) % tagKindCount + 1);

    std::ignore = serato::decodeTagBase64(payload, legacyMarkers);

    // The cache decoder also reads bytes that did not come from the encoder, for example a corrupted database.
    std::ignore = serato::deserializeTag(kind, juce::MemoryBlock(payload.data(), payload.size()));

    const auto result = serato::parseTagPayload(kind, payload, legacyMarkers);
    const auto serialized = serato::serializeTag(result);
    if (serialized.isEmpty()) {
        return;
    }
    const auto restored = serato::deserializeTag(kind, serialized);
    if (!restored || serato::serializeTag(*restored) != serialized) {
        std::abort();
    }
}
}  // namespace
}  // namespace audiobatch

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

/// libFuzzer entry point, called once for each generated input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    audiobatch::fuzzTagPayload({data, size});
    return 0;
}
