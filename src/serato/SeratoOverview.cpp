/// Bounded Overview payload parsing and portable numeric cache serialization.
/// Every cache field is encoded explicitly rather than persisting native struct layout.

#include "serato/SeratoOverview.h"

#include "serato/SeratoBytes.h"
#include "serato/SeratoTagTypes.h"

#include <algorithm>
#include <bit>

namespace audiobatch::serato
{
namespace
{
constexpr std::array<std::uint8_t, 4> cacheMagic {'S', 'O', 'V', 'R'};
constexpr std::array<std::uint8_t, 2> payloadHeader {0x01, 0x05};
constexpr std::size_t cacheHeaderBytes {16};
constexpr std::size_t kindOffset {4};
constexpr std::size_t versionOffset {8};
constexpr std::size_t countOffset {12};
constexpr std::size_t fieldBytes {sizeof(std::uint32_t)};

/// Reads the four-byte big-endian header field at the given offset.
template<std::size_t Offset>
std::uint32_t readHeaderField(const std::span<const std::uint8_t> bytes)
{
    return readBigEndian<std::uint32_t>(bytes.subspan<Offset, fieldBytes>());
}

/// Writes a four-byte big-endian field to an already allocated cache header.
void writeBigEndian(std::span<std::uint8_t> output, const std::uint32_t value)
{
    const auto bytes = std::bit_cast<std::array<std::uint8_t, fieldBytes>>(juce::ByteOrder::swapIfLittleEndian(value));
    std::ranges::copy(bytes, output.begin());
}

/// Copies already validated complete slices into independent storage.
Overview copySlices(const std::span<const std::uint8_t> bands)
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

/// Parses the binary payload after the enclosing GEOB or FLAC wrapper has been removed.
/// Offsets and lengths below are hexadecimal. The final row illustrates a 240-slice payload.
///
/// | Offset | Length | Raw value     | Type         | Description                           |
/// | ------ | ------ | ------------- | ------------ | ------------------------------------- |
/// | 0x0000 | 0x0002 | 01 05         | two bytes    | Supported header, meaning unspecified |
/// | 0x0002 | 0x0010 | sixteen bytes | 16 * uint8_t | Palette indices for time slice 0      |
/// | 0x0012 | 0x0010 | sixteen bytes | 16 * uint8_t | Palette indices for time slice 1      |
/// | ...    | ...    | ...           | ...          | Subsequent consecutive time slices    |
/// | 0x0ef2 | 0x0010 | sixteen bytes | 16 * uint8_t | Palette indices for time slice 239    |
///
/// A payload with N slices occupies 2 + 16 * N bytes, or 3842 bytes for 240 slices.
/// Accepts header 01 05 and one to maxSlices complete slices, rejecting tails and unknown headers.
/// The returned slices own their data independently of the input buffer.
std::optional<Overview> Overview::parse(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < payloadHeader.size() + bandCount || bytes.size() > maxPayloadBytes
        || !std::ranges::equal(bytes.first(payloadHeader.size()), payloadHeader)
        || (bytes.size() - payloadHeader.size()) % bandCount != 0)
    {
        return std::nullopt;
    }
    return copySlices(bytes.subspan(payloadHeader.size()));
}

/// Encodes magic, kind, parser version and count as big-endian fields followed by raw palette indices.
/// Returns an empty block when the public slice vector is empty or exceeds maxSlices.
juce::MemoryBlock Overview::serialize() const
{
    if (slices.empty() || slices.size() > maxSlices) {
        return {};
    }
    juce::MemoryBlock output(cacheHeaderBytes + slices.size() * bandCount);
    const std::span bytes {static_cast<std::uint8_t*>(output.getData()), output.getSize()};
    std::ranges::copy(cacheMagic, bytes.begin());
    writeBigEndian(bytes.subspan(kindOffset, fieldBytes), std::to_underlying(TagKind::overview));
    writeBigEndian(bytes.subspan(versionOffset, fieldBytes), overviewParserVersion);
    writeBigEndian(bytes.subspan(countOffset, fieldBytes), static_cast<std::uint32_t>(slices.size()));
    for (std::size_t index = 0; index < slices.size(); ++index) {
        std::ranges::copy(slices[index], bytes.subspan(cacheHeaderBytes + index * bandCount, bandCount).begin());
    }
    return output;
}

/// Validates the complete numeric cache representation before allocating owned slices.
/// Unknown magic, kind, version, count or an inexact length is a cache miss.
std::optional<Overview> Overview::deserialize(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < cacheHeaderBytes || !std::ranges::equal(bytes.first(cacheMagic.size()), cacheMagic)
        || readHeaderField<kindOffset>(bytes) != std::to_underlying(TagKind::overview)
        || readHeaderField<versionOffset>(bytes) != overviewParserVersion)
    {
        return std::nullopt;
    }
    const auto count = readHeaderField<countOffset>(bytes);
    if (count == 0 || count > maxSlices
        || bytes.size() != cacheHeaderBytes + static_cast<std::size_t>(count) * bandCount)
    {
        return std::nullopt;
    }
    return copySlices(bytes.subspan(cacheHeaderBytes));
}
}  // namespace audiobatch::serato
