/// SQLite-backed cache for audio analysis results.
/// AnalysisCache stores per-file analysis records, short-term loudness history,
/// and waveform preview data keyed by path, size, and modification time,
/// so unchanged files are not re-read on later runs.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/Sqlite.h"
#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace audiobatch
{
/// Captures the cache key before tag reading. Reuse this identity for lookup, delivery and persistence.
/// Size and modification time cannot detect external edits that deliberately preserve both values.
struct FileIdentity {
    explicit FileIdentity(const juce::File& file);

    [[nodiscard]] bool matches(const juce::File& file) const;

    juce::String path;
    std::int64_t fileSize;
    std::int64_t modifiedTimeMs;
};

/// A user-controlled gain to store for one file.
struct CustomGainUpdate {
    juce::File file;
    float customGainDb {0.0f};
    bool hasCustomGain {false};
};

/// Persistent SQLite-backed cache for file analysis results and waveform previews.
class AnalysisCache
{
public:
    AnalysisCache();

    explicit AnalysisCache(juce::File cacheDatabaseFile);

    ~AnalysisCache();

    AnalysisCache(const AnalysisCache&) = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;
    AnalysisCache(AnalysisCache&&) = delete;
    AnalysisCache& operator=(AnalysisCache&&) = delete;

    std::optional<serato::TagReadResult> getSeratoTag(
        const FileIdentity& identity,
        serato::TagKind kind = serato::TagKind::overview
    );

    [[nodiscard]] juce::File getDatabaseFile() const;

    [[nodiscard]] std::optional<AudioAnalysisRecord> getAnalysis(const juce::File& file);
    [[nodiscard]] std::optional<float> getCustomGain(
        const juce::File& file,
        std::int64_t fileSize,
        std::int64_t modifiedTimeMs
    );
    [[nodiscard]] std::optional<juce::MemoryBlock> getWaveformData(const juce::File& file);

    int pruneMissingFiles(const std::function<bool()>& shouldStop = {});
    [[nodiscard]] int storeCustomGains(const std::vector<CustomGainUpdate>& updates);

    [[nodiscard]] bool open();
    [[nodiscard]] bool removeAnalysis(const juce::File& file);
    [[nodiscard]] bool storeAnalysis(const AudioAnalysisRecord& record);
    [[nodiscard]] bool storeCustomGain(const juce::File& file, float customGainDb, bool hasCustomGain);
    [[nodiscard]] bool storeSeratoTag(const FileIdentity& identity, const serato::TagReadResult& result);
    [[nodiscard]] bool storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData);

private:
    [[nodiscard]] bool addMissingColumns() const;
    [[nodiscard]] bool columnExists(const juce::String& tableName, const juce::String& columnName) const;
    bool openUnlocked();

    [[nodiscard]] bool removeRowVersion(
        const juce::String& path,
        std::int64_t fileSize,
        std::int64_t modifiedTimeMs,
        std::optional<std::int64_t> tagKind = std::nullopt
    ) const;

    void closeUnlocked();

    static juce::String normalizedPath(const juce::File& file);

    static constexpr int analysisVersion {10};
    static constexpr int waveformVersion {1};

    /// Serializes every use of the connection, which is opened without SQLite's own mutex.
    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite::Database database;
};
}  // namespace audiobatch
