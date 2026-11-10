/// Bounded Overview payload parsing and portable numeric cache serialization.
/// Every cache field is encoded explicitly rather than persisting native struct layout.

#include "serato/SeratoOverview.h"

#include "serato/SeratoBytes.h"
#include "serato/SeratoTagTypes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace audiobatch::serato
{
namespace
{
constexpr std::array<std::uint8_t, 4> CACHE_MAGIC {'S', 'O', 'V', 'R'};
constexpr std::array<std::uint8_t, 2> PAYLOAD_HEADER {0x01, 0x05};
constexpr std::size_t CACHE_HEADER_BYTES {16};
/// Raster layout is independent of the parser-result revision used by the tag cache.
constexpr std::uint32_t CACHE_FORMAT_VERSION {2};
constexpr std::size_t KIND_OFFSET {4};
constexpr std::size_t VERSION_OFFSET {8};
constexpr std::size_t COUNT_OFFSET {12};
constexpr std::size_t FIELD_BYTES {sizeof(std::uint32_t)};

/// Reads the four-byte big-endian header field at the given offset.
template<std::size_t Offset>
std::uint32_t readHeaderField(const std::span<const std::uint8_t> bytes)
{
    return readBigEndian<std::uint32_t>(bytes.subspan<Offset, FIELD_BYTES>());
}

/// Writes a four-byte big-endian field to an already allocated cache header.
void writeBigEndian(std::span<std::uint8_t> output, const std::uint32_t value)
{
    const auto bytes = std::bit_cast<std::array<std::uint8_t, FIELD_BYTES>>(juce::ByteOrder::swapIfLittleEndian(value));
    std::ranges::copy(bytes, output.begin());
}

/// Copies already validated complete slices into independent storage.
Overview copySlices(const std::span<const std::uint8_t> bands)
{
    Overview overview;
    overview.slices.resize(bands.size() / Overview::BAND_COUNT);
    for (std::size_t index = 0; index < overview.slices.size(); ++index) {
        std::ranges::copy(
            bands.subspan(index * Overview::BAND_COUNT, Overview::BAND_COUNT), overview.slices[index].begin()
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
    if (bytes.size() < PAYLOAD_HEADER.size() + BAND_COUNT || bytes.size() > MAX_PAYLOAD_BYTES
        || !std::ranges::equal(bytes.first(PAYLOAD_HEADER.size()), PAYLOAD_HEADER)
        || (bytes.size() - PAYLOAD_HEADER.size()) % BAND_COUNT != 0)
    {
        return std::nullopt;
    }
    return copySlices(bytes.subspan(PAYLOAD_HEADER.size()));
}

/// Encodes magic, kind, raster format version and count as big-endian fields followed by raw palette indices.
/// Returns an empty block when the public slice vector is empty or exceeds maxSlices.
juce::MemoryBlock Overview::serialize() const
{
    if (slices.empty() || slices.size() > MAX_SLICES) {
        return {};
    }
    juce::MemoryBlock output(CACHE_HEADER_BYTES + slices.size() * BAND_COUNT);
    const std::span bytes {static_cast<std::uint8_t*>(output.getData()), output.getSize()};
    std::ranges::copy(CACHE_MAGIC, bytes.begin());
    writeBigEndian(bytes.subspan(KIND_OFFSET, FIELD_BYTES), std::to_underlying(TagKind::overview));
    writeBigEndian(bytes.subspan(VERSION_OFFSET, FIELD_BYTES), CACHE_FORMAT_VERSION);
    writeBigEndian(bytes.subspan(COUNT_OFFSET, FIELD_BYTES), static_cast<std::uint32_t>(slices.size()));
    for (std::size_t index = 0; index < slices.size(); ++index) {
        std::ranges::copy(slices[index], bytes.subspan(CACHE_HEADER_BYTES + index * BAND_COUNT, BAND_COUNT).begin());
    }
    return output;
}

/// Validates the complete numeric cache representation before allocating owned slices.
/// Unknown magic, kind, version, count or an inexact length is a cache miss.
std::optional<Overview> Overview::deserialize(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < CACHE_HEADER_BYTES || !std::ranges::equal(bytes.first(CACHE_MAGIC.size()), CACHE_MAGIC)
        || readHeaderField<KIND_OFFSET>(bytes) != std::to_underlying(TagKind::overview)
        || readHeaderField<VERSION_OFFSET>(bytes) != CACHE_FORMAT_VERSION)
    {
        return std::nullopt;
    }
    const auto count = readHeaderField<COUNT_OFFSET>(bytes);
    if (count == 0 || count > MAX_SLICES
        || bytes.size() != CACHE_HEADER_BYTES + static_cast<std::size_t>(count) * BAND_COUNT)
    {
        return std::nullopt;
    }
    return copySlices(bytes.subspan(CACHE_HEADER_BYTES));
}
}  // namespace audiobatch::serato
