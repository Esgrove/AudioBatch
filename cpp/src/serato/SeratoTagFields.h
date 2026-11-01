/// Field-level helpers shared by the Serato payload parsers and the cache block codec.
/// Holds the fail-closed Cursor over a bounded byte block, the zero padding and UTF-8 checks,
/// and the offsets of the name fields in the Markers2 cue and loop records.

#pragma once

#include "serato/SeratoBytes.h"
#include "serato/SeratoTags.h"

#include <JuceHeader.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace audiobatch::serato
{
/// Offset of the name in a Markers2 cue record.
inline constexpr std::size_t cueNameOffset {12};
/// Offset of the name in a Markers2 loop record.
inline constexpr std::size_t loopNameOffset {20};

/// Checks padding without accepting unparsed trailing records or text.
inline bool allZero(std::span<const std::uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](auto byte) { return byte == 0; });
}

/// Returns a validated UTF-8 string of bounded length without embedded nulls.
inline std::optional<juce::String> utf8(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxTagTextBytes || std::ranges::find(bytes, std::uint8_t {0}) != bytes.end()) {
        return std::nullopt;
    }
    const std::string text(bytes.begin(), bytes.end());
    if (!juce::CharPointer_UTF8::isValidString(text.c_str(), static_cast<int>(text.size()))) {
        return std::nullopt;
    }
    return juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size()));
}

/// A fail-closed reader for cache fields.
/// Every read returns nothing instead of a default value when its field is truncated or invalid,
/// and a failed read never advances outside the supplied span.
class Cursor
{
public:
    /// Borrows an already bounded block for this synchronous decode.
    explicit Cursor(const std::span<const std::uint8_t> input) : remaining(input) {}

    /// Takes a field of the given size, or nothing when fewer bytes remain.
    std::optional<std::span<const std::uint8_t>> take(const std::size_t count)
    {
        if (count > remaining.size()) {
            return std::nullopt;
        }
        const auto bytes = remaining.first(count);
        remaining = remaining.subspan(count);
        return bytes;
    }

    /// Reads an unsigned byte.
    std::optional<std::uint8_t> byte()
    {
        return take(1).transform([](const std::span<const std::uint8_t> bytes) { return bytes.front(); });
    }

    /// Reads a strict boolean, rejecting byte values other than zero and one.
    std::optional<bool> boolean()
    {
        const auto value = byte();
        if (!value || *value > 1) {
            return std::nullopt;
        }
        return *value != 0;
    }

    /// Reads a portable four-byte unsigned integer.
    std::optional<std::uint32_t> number()
    {
        return take(sizeof(std::uint32_t)).transform([](const std::span<const std::uint8_t> bytes) {
            return readBigEndian<std::uint32_t>(bytes.first<sizeof(std::uint32_t)>());
        });
    }

    /// Reads an IEEE binary64 value, rejecting infinities and NaNs.
    std::optional<double> real()
    {
        const auto bytes = take(sizeof(std::uint64_t));
        if (!bytes) {
            return std::nullopt;
        }
        const auto value = std::bit_cast<double>(readBigEndian<std::uint64_t>(bytes->first<sizeof(std::uint64_t)>()));
        return std::isfinite(value) ? std::optional(value) : std::nullopt;
    }

    /// Reads length-prefixed UTF-8, bounding the length before allocating the text.
    std::optional<juce::String> text()
    {
        const auto size = number();
        if (!size || *size > maxTagTextBytes) {
            return std::nullopt;
        }
        const auto bytes = take(*size);
        return bytes ? utf8(*bytes) : std::nullopt;
    }

    /// Reports whether every byte of the enclosing block has been consumed.
    [[nodiscard]] bool isExhausted() const
    {
        return remaining.empty();
    }

private:
    /// The bytes not read yet, a view into the block the cursor borrows.
    std::span<const std::uint8_t> remaining;
};
}  // namespace audiobatch::serato
