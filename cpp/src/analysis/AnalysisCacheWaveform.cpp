/// Native waveform persistence and validation for AnalysisCache.
/// Validates bounded JUCE thumbnail geometry before allocating preview storage,
/// and publishes previews only against the source version and measurements they describe.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCacheSql.h"
#include "analysis/Sqlite.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <utility>

namespace audiobatch
{
namespace
{
/// Validated native thumbnail geometry, read without allocating preview storage.
struct WaveformGeometry {
    /// Complete source length, including the final partial point.
    std::int64_t lengthInSamples {0};
    /// Source rate from the native serializer.
    int sampleRate {0};
    /// Source channel count from the native serializer.
    int channels {0};
};

constexpr int maximumWaveformBytes {52 + 4'194'304 * 2 * 2};

/// Validates a borrowed SQLite blob before copying it or allowing JUCE to allocate native thumbnail arrays.
std::optional<WaveformGeometry> waveformGeometry(const std::span<const std::byte> data)
{
    constexpr std::size_t headerBytes {52};
    constexpr int waveformMagic {0x6d74616a};
    constexpr int minimumDetail {16};
    constexpr int maximumDetail {1024};
    constexpr int maximumWaveformSampleRate {768'000};
    constexpr std::int64_t maximumPoints {4'194'304};
    if (data.size() < headerBytes || data.size() > static_cast<std::size_t>(maximumWaveformBytes)) {
        return std::nullopt;
    }
    juce::MemoryInputStream input(data.data(), data.size(), false);
    if (input.readInt() != waveformMagic) {
        return std::nullopt;
    }
    const auto detail = input.readInt();
    const auto total = input.readInt64();
    const auto finished = input.readInt64();
    const auto points = input.readInt();
    const auto channels = input.readInt();
    const auto rate = input.readInt();
    if (detail < minimumDetail || detail > maximumDetail || !std::has_single_bit(static_cast<unsigned int>(detail))
        || total <= 0 || finished < 0 || finished > total || total - finished > detail || points <= 0
        || points > maximumPoints || (channels != 1 && channels != 2) || rate <= 0 || rate > maximumWaveformSampleRate
        || total / detail != static_cast<std::int64_t>(points) - 1
        || data.size() - headerBytes != static_cast<std::size_t>(points) * static_cast<std::size_t>(channels) * 2)
    {
        return std::nullopt;
    }
    return WaveformGeometry {.lengthInSamples = total, .sampleRate = rate, .channels = channels};
}
}  // namespace

/// Validates the complete mono/stereo thumbnail payload before the JUCE deserializer can allocate.
/// Cached thumbnails use bounded power-of-two detail and complete source geometry, including the final partial point.
bool isValidWaveformData(const juce::MemoryBlock& data)
{
    return waveformGeometry(std::span(static_cast<const std::byte*>(data.getData()), data.getSize())).has_value();
}

/// Keys in-memory thumbnail reuse by the captured source version, detail and waveform format version.
juce::int64 waveformCacheKey(const utils::FileIdentity& identity, const int samplesPerPoint)
{
    const auto [volume, file] = identity.platformId.value_or(std::pair<std::uint64_t, std::uint64_t> {});
    return utils::format(
               "{}:{}:{}:{}:{}:{}:{}:{}:3",
               identity.path,
               identity.fileSize,
               identity.modifiedTimeMs,
               volume,
               file,
               identity.preciseTimes.first,
               identity.preciseTimes.second,
               samplesPerPoint
    )
        .hashCode64();
}

/// Loads the cached thumbnail waveform data for the given file,
/// or returns an empty optional when there is none for the file's current size, modification time and version.
std::optional<juce::MemoryBlock> AnalysisCache::getWaveformData(const juce::File& file)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    const auto fileSize = file.getSize();
    const auto modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return std::nullopt;
    }

    constexpr auto sql = R"SQL(
        SELECT file_size, modified_time_ms, waveform_data, waveform_version, analysis_version,
               length_in_samples, sample_rate, channels
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    const auto prepared = statements.borrow(*database, sql);
    if (!prepared.isValid() || !sqlite::bindText(*prepared, 1, normalizedPath(file))) {
        return std::nullopt;
    }

    auto* const statement = prepared.get();

    if (sqlite3_step(statement) != SQLITE_ROW) {
        return std::nullopt;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, 0);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, 1);
    const auto cachedWaveformVersion = sqlite3_column_int(statement, 3);
    const auto cachedAnalysisVersion = sqlite3_column_int(statement, 4);

    if (cachedFileSize != fileSize || cachedModifiedTime != modifiedTimeMs || cachedWaveformVersion != waveformVersion
        || cachedAnalysisVersion != analysisVersion)
    {
        return std::nullopt;
    }

    if (sqlite3_column_type(statement, 2) != SQLITE_BLOB || sqlite3_column_bytes(statement, 2) > maximumWaveformBytes) {
        return std::nullopt;
    }
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement, 2));
    if (data == nullptr) {
        return std::nullopt;
    }
    const auto geometry
        = waveformGeometry(std::span(data, static_cast<std::size_t>(sqlite3_column_bytes(statement, 2))));
    constexpr int lengthColumn {5};
    constexpr int rateColumn {6};
    constexpr int channelsColumn {7};
    if (!geometry || geometry->lengthInSamples != sqlite3_column_int64(statement, lengthColumn)
        || geometry->sampleRate != sqlite3_column_int64(statement, rateColumn)
        || geometry->channels != sqlite3_column_int64(statement, channelsColumn))
    {
        return std::nullopt;
    }
    return sqlite::columnBlob(*statement, 2);
}

/// Stores serialized waveform preview data for a file.
bool AnalysisCache::storeWaveformData(const utils::FileIdentity& identity, const juce::MemoryBlock& waveformData)
{
    const auto geometry
        = waveformGeometry(std::span(static_cast<const std::byte*>(waveformData.getData()), waveformData.getSize()));
    if (!geometry || !identity.matches(juce::File(identity.path))) {
        return false;
    }
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        UPDATE file_analysis
        SET waveform_data = ?,
            waveform_version = ?,
            updated_at_ms = ?
        WHERE file_path = ?
          AND file_size = ?
          AND modified_time_ms = ?
          AND analysis_version = ?
          AND length_in_samples = ? AND sample_rate = ? AND channels = ? AND status IN (1, 2);
    )SQL";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return false;
    }

    const bool bound = sqlite::bindBlob(*statement, waveform_parameter::waveformData, waveformData)
        && sqlite::bindInt64(*statement, waveform_parameter::waveformVersion, waveformVersion)
        && sqlite::bindInt64(*statement, waveform_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds())
        && sqlite::bindText(*statement, waveform_parameter::filePath, identity.path)
        && sqlite::bindInt64(*statement, waveform_parameter::fileSize, identity.fileSize)
        && sqlite::bindInt64(*statement, waveform_parameter::modifiedTimeMs, identity.modifiedTimeMs)
        && sqlite::bindInt64(*statement, waveform_parameter::analysisVersion, analysisVersion)
        && sqlite::bindInt64(*statement, waveform_parameter::lengthInSamples, geometry->lengthInSamples)
        && sqlite::bindInt64(*statement, waveform_parameter::sampleRate, geometry->sampleRate)
        && sqlite::bindInt64(*statement, waveform_parameter::channels, geometry->channels);

    return bound && sqlite::stepToCompletion(*statement) && sqlite3_changes(database.get()) > 0;
}
}  // namespace audiobatch
