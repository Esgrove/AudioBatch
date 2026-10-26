/// Big-endian field reading shared by the Serato tag and overview parsers.
/// The width of a field is part of its span type,
/// so a call site cannot hand a reader fewer bytes than the field needs.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace audiobatch::serato
{
/// Width in bytes of an RGB colour field.
inline constexpr std::size_t rgbBytes {3};

/// Reads an unsigned big-endian integer from exactly sizeof(Value) bytes, without alignment assumptions.
template<std::unsigned_integral Value>
[[nodiscard]] constexpr Value readBigEndian(const std::span<const std::uint8_t, sizeof(Value)> bytes) noexcept
{
    constexpr unsigned int bitsPerByte {8};
    Value value {0};

    for (const auto byte : bytes) {
        value = static_cast<Value>(static_cast<Value>(value << bitsPerByte) | byte);
    }

    return value;
}
}  // namespace audiobatch::serato
