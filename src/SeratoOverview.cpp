/// Bounded Overview payload parsing and portable numeric cache serialization.
/// Every cache field is encoded explicitly rather than persisting native struct layout.

#include "SeratoOverview.h"

#include "SeratoTagTypes.h"

#include <algorithm>
#include <bit>

namespace audiobatch::serato
{
namespace
{
constexpr std::array<std::uint8_t, 4> cacheMagic {'S', 'O', 'V', 'R'};
constexpr std::array<std::uint8_t, 2> payloadHeader {0x01, 0x05};
constexpr std::size_t cacheHeaderBytes = 16;
constexpr std::size_t kindOffset = 4;
constexpr std::size_t versionOffset = 8;
constexpr std::size_t countOffset = 12;
constexpr std::size_t fieldBytes = sizeof(std::uint32_t);

/// Reads a validated four-byte big-endian field without alignment assumptions.
std::uint32_t readBigEndian(std::span<const std::uint8_t> bytes)
{
    return juce::ByteOrder::bigEndianInt(bytes.data());
}

/// Writes a four-byte big-endian field to an already allocated cache header.
void writeBigEndian(std::span<std::uint8_t> output, std::uint32_t value)
{
    const auto bytes = std::bit_cast<std::array<std::uint8_t, fieldBytes>>(juce::ByteOrder::swapIfLittleEndian(value));
    std::ranges::copy(bytes, output.begin());
}

/// Copies already validated complete slices into independent storage.
Overview copySlices(std::span<const std::uint8_t> bands)
{
    Overview overview;
    overview.slices.resize(bands.size() / Overview::bandCount);
    for (std::size_t index = 0; index < overview.slices.size(); ++index) {
        std::ranges::copy(
            bands.subspan(index * Overview::bandCount, Overview::bandCount), overview.slices[index].begin()
        );
    }
    return overview;
}
}  // namespace

std::optional<Overview> Overview::parse(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < payloadHeader.size() + bandCount || bytes.size() > maxPayloadBytes
        || !std::ranges::equal(bytes.first(payloadHeader.size()), payloadHeader)
        || (bytes.size() - payloadHeader.size()) % bandCount != 0)
    {
        return std::nullopt;
    }
    return copySlices(bytes.subspan(payloadHeader.size()));
}

juce::MemoryBlock Overview::serialize() const
{
    if (slices.empty() || slices.size() > maxSlices) {
        return {};
    }
    juce::MemoryBlock output(cacheHeaderBytes + slices.size() * bandCount);
    const std::span bytes {static_cast<std::uint8_t*>(output.getData()), output.getSize()};
    std::ranges::copy(cacheMagic, bytes.begin());
    writeBigEndian(bytes.subspan(kindOffset, fieldBytes), static_cast<std::uint32_t>(TagKind::overview));
    writeBigEndian(bytes.subspan(versionOffset, fieldBytes), overviewParserVersion);
    writeBigEndian(bytes.subspan(countOffset, fieldBytes), static_cast<std::uint32_t>(slices.size()));
    for (std::size_t index = 0; index < slices.size(); ++index) {
        std::ranges::copy(slices[index], bytes.subspan(cacheHeaderBytes + index * bandCount, bandCount).begin());
    }
    return output;
}

std::optional<Overview> Overview::deserialize(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < cacheHeaderBytes || !std::ranges::equal(bytes.first(cacheMagic.size()), cacheMagic)
        || readBigEndian(bytes.subspan(kindOffset, fieldBytes)) != static_cast<std::uint32_t>(TagKind::overview)
        || readBigEndian(bytes.subspan(versionOffset, fieldBytes)) != overviewParserVersion)
    {
        return std::nullopt;
    }
    const auto count = readBigEndian(bytes.subspan(countOffset, fieldBytes));
    if (count == 0 || count > maxSlices
        || bytes.size() != cacheHeaderBytes + static_cast<std::size_t>(count) * bandCount)
    {
        return std::nullopt;
    }
    return copySlices(bytes.subspan(cacheHeaderBytes));
}
}  // namespace audiobatch::serato