/// Spectrogram geometry validation, mel conversion, and a strictly bounded little-endian codec.
/// The envelope pins every DSP profile field so incompatible artifacts become cache misses.

#include "analysis/SpectrogramData.h"

#include "analysis/AudioAnalysisTypes.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
namespace audiobatch
{
namespace
{
constexpr std::uint32_t MAGIC {0x43455053};
constexpr std::size_t HEADER_BYTES {72};
constexpr std::size_t MAXIMUM_PAYLOAD_BYTES {
    static_cast<std::size_t>(SpectrogramData::maximumColumns) * SpectrogramData::frequencyRows,
};
constexpr double MEL_SCALE {2595.0};
constexpr double MEL_FREQUENCY {700.0};
constexpr double DECIBEL_STEPS {255.0};
constexpr double LOGARITHM_BASE {10.0};
constexpr std::size_t FFT_OFFSET {12};
constexpr std::size_t ROWS_OFFSET {20};
constexpr std::size_t MAXIMUM_COLUMNS_OFFSET {24};
constexpr std::size_t SAMPLE_RATE_OFFSET {28};
constexpr std::size_t COLUMNS_OFFSET {36};
constexpr std::size_t LENGTH_OFFSET {40};
constexpr std::size_t STRIDE_OFFSET {48};
constexpr std::size_t FLOOR_OFFSET {56};

/// Finds the canonical stride without multiplying a possibly enormous source length.
std::int64_t canonicalStride(const std::int64_t length)
{
    std::int64_t stride = SpectrogramData::hopSize;
    while (length > 0 && 1 + (length - 1) / stride > SpectrogramData::maximumColumns) {
        stride *= 2;
    }
    return stride;
}

/// Writes an unsigned integer explicitly, independently of host byte order and alignment.
void writeLittleEndian(std::span<std::byte> bytes, const std::size_t offset, std::uint64_t value, const int count)
{
    constexpr std::uint64_t BYTE_MASK {0xff};
    for (int index = 0; index < count; ++index) {
        bytes[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>(value & BYTE_MASK);
        value >>= 8U;
    }
}

/// Reads a header integer after the caller has checked the complete fixed header size.
std::uint64_t readLittleEndian(const std::span<const std::byte> bytes, const std::size_t offset, const int count)
{
    std::uint64_t value = 0;
    for (int index = 0; index < count; ++index) {
        value |= std::to_integer<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)])
            << (8U * static_cast<unsigned int>(index));
    }
    return value;
}
}  // namespace

/// Returns the number of complete columns, without assuming that unvalidated bytes have legal geometry.
std::size_t SpectrogramData::columnCount() const noexcept
{
    return intensities.size() / static_cast<std::size_t>(frequencyRows);
}

/// Converts hertz using the HTK mel definition, with zero hertz at zero mel.
double frequencyToMel(const double frequency) noexcept
{
    return MEL_SCALE * std::log10(1.0 + frequency / MEL_FREQUENCY);
}

/// Inverts the HTK mel definition used by every spectrogram row boundary.
double melToFrequency(const double mel) noexcept
{
    return MEL_FREQUENCY * (std::pow(LOGARITHM_BASE, mel / MEL_SCALE) - 1.0);
}

/// Decodes a byte to the center of its fixed linear decibel quantization level.
double spectrogramDecibels(const std::uint8_t intensity) noexcept
{
    return SpectrogramData::minimumDecibels
        + static_cast<double>(intensity) * (SpectrogramData::maximumDecibels - SpectrogramData::minimumDecibels)
        / DECIBEL_STEPS;
}

/// Accepts only nonempty mono/stereo artifacts with exactly the canonical actual-length geometry.
bool isValidSpectrogramData(const SpectrogramData& data)
{
    if (data.sampleRate <= 0 || data.channelCount < 1 || data.channelCount > 2 || data.lengthInSamples <= 0
        || data.samplesPerColumn != canonicalStride(data.lengthInSamples)
        || data.intensities.size() > MAXIMUM_PAYLOAD_BYTES)
    {
        return false;
    }
    const auto columns = 1 + (data.lengthInSamples - 1) / data.samplesPerColumn;
    return data.intensities.size() == static_cast<std::size_t>(columns) * SpectrogramData::frequencyRows;
}

/// Checks decoded geometry rather than presentation duration or producer-specific loudness values.
bool spectrogramMatches(const SpectrogramData& data, const AudioAnalysisRecord& record)
{
    return isValidSpectrogramData(data) && data.sampleRate == record.sampleRate && data.channelCount == record.channels
        && data.lengthInSamples == record.lengthInSamples;
}

/// Encodes SPEC followed by u32 revisions, FFT, hop, rows, maximum columns, rate, channels, and columns.
/// The remaining fields are u64 source length, stride, and IEEE-754 floor and ceiling bits, then raw intensity bytes.
/// The header is exactly 72 bytes and contains no native structs, padding, or compressed allocation sizes.
juce::MemoryBlock serializeSpectrogram(const SpectrogramData& data)
{
    if (!isValidSpectrogramData(data)) {
        return {};
    }
    std::array<std::byte, HEADER_BYTES> header {};
    writeLittleEndian(header, 0, MAGIC, 4);
    writeLittleEndian(header, 4, SpectrogramData::serializationVersion, 4);
    writeLittleEndian(header, 8, SpectrogramData::algorithmVersion, 4);
    writeLittleEndian(header, FFT_OFFSET, SpectrogramData::fftSize, 4);
    writeLittleEndian(header, 16, SpectrogramData::hopSize, 4);
    writeLittleEndian(header, ROWS_OFFSET, SpectrogramData::frequencyRows, 4);
    writeLittleEndian(header, MAXIMUM_COLUMNS_OFFSET, SpectrogramData::maximumColumns, 4);
    writeLittleEndian(header, SAMPLE_RATE_OFFSET, static_cast<std::uint32_t>(data.sampleRate), 4);
    writeLittleEndian(header, 32, static_cast<std::uint32_t>(data.channelCount), 4);
    writeLittleEndian(header, COLUMNS_OFFSET, data.columnCount(), 4);
    writeLittleEndian(header, LENGTH_OFFSET, static_cast<std::uint64_t>(data.lengthInSamples), 8);
    writeLittleEndian(header, STRIDE_OFFSET, static_cast<std::uint64_t>(data.samplesPerColumn), 8);
    writeLittleEndian(header, FLOOR_OFFSET, std::bit_cast<std::uint64_t>(SpectrogramData::minimumDecibels), 8);
    writeLittleEndian(header, 64, std::bit_cast<std::uint64_t>(SpectrogramData::maximumDecibels), 8);
    juce::MemoryBlock blob(header.data(), header.size());
    blob.append(data.intensities.data(), data.intensities.size());
    return blob;
}

/// Rejects future profiles, overflowing geometry, and any missing or extra bytes before allocating the payload.
std::shared_ptr<const SpectrogramData> deserializeSpectrogram(const std::span<const std::byte> blob)
{
    if (blob.size() < HEADER_BYTES || blob.size() > HEADER_BYTES + MAXIMUM_PAYLOAD_BYTES) {
        return nullptr;
    }
    const std::array<std::uint32_t, 7> profile {
        MAGIC,
        SpectrogramData::serializationVersion,
        SpectrogramData::algorithmVersion,
        SpectrogramData::fftSize,
        SpectrogramData::hopSize,
        SpectrogramData::frequencyRows,
        SpectrogramData::maximumColumns,
    };
    for (std::size_t index = 0; index < profile.size(); ++index) {
        if (readLittleEndian(blob, index * 4, 4) != profile[index]) {
            return nullptr;
        }
    }
    if (readLittleEndian(blob, FLOOR_OFFSET, 8) != std::bit_cast<std::uint64_t>(SpectrogramData::minimumDecibels)
        || readLittleEndian(blob, 64, 8) != std::bit_cast<std::uint64_t>(SpectrogramData::maximumDecibels))
    {
        return nullptr;
    }
    const auto sampleRate = readLittleEndian(blob, SAMPLE_RATE_OFFSET, 4);
    const auto channels = readLittleEndian(blob, 32, 4);
    const auto columns = readLittleEndian(blob, COLUMNS_OFFSET, 4);
    const auto length = readLittleEndian(blob, LENGTH_OFFSET, 8);
    const auto stride = readLittleEndian(blob, STRIDE_OFFSET, 8);
    if (sampleRate == 0 || sampleRate > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) || channels < 1
        || channels > 2 || columns == 0 || columns > static_cast<std::uint64_t>(SpectrogramData::maximumColumns)
        || length == 0 || length > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || std::cmp_not_equal(stride, canonicalStride(static_cast<std::int64_t>(length))))
    {
        return nullptr;
    }
    if (columns != 1 + (length - 1) / stride || blob.size() - HEADER_BYTES != columns * SpectrogramData::frequencyRows)
    {
        return nullptr;
    }
    auto data = std::make_shared<SpectrogramData>();
    data->sampleRate = static_cast<int>(sampleRate);
    data->channelCount = static_cast<int>(channels);
    data->lengthInSamples = static_cast<std::int64_t>(length);
    data->samplesPerColumn = static_cast<std::int64_t>(stride);
    data->intensities.resize(blob.size() - HEADER_BYTES);
    for (std::size_t index = 0; index < data->intensities.size(); ++index) {
        data->intensities[index] = std::to_integer<std::uint8_t>(blob[HEADER_BYTES + index]);
    }
    return data;
}
}  // namespace audiobatch
