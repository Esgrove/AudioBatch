/// SQLite-backed cache for audio analysis results.
/// AnalysisCache stores per-file analysis records and waveform preview data keyed by path, size, and modification time,
/// so unchanged files are not re-read on later runs.

#pragma once

#include "AudioAnalysisTypes.h"
#include "SeratoTagTypes.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <optional>

struct sqlite3;
struct sqlite3_stmt;

/// Persistent SQLite-backed cache for file analysis results and waveform previews.
class AnalysisCache
{
public:
    /// Captures the cache key before tag reading. Reuse this identity for lookup, delivery and persistence.
    /// Size and modification time cannot detect external edits that deliberately preserve both values.
    struct FileIdentity {
        explicit FileIdentity(const juce::File& file);

        [[nodiscard]] bool matches(const juce::File& file) const;

        juce::String path;
        std::int64_t fileSize;
        std::int64_t modifiedTimeMs;
    };

    AnalysisCache();

    explicit AnalysisCache(juce::File cacheDatabaseFile);

    ~AnalysisCache();

    AnalysisCache(const AnalysisCache&) = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;
    AnalysisCache(AnalysisCache&&) = delete;
    AnalysisCache& operator=(AnalysisCache&&) = delete;

    bool getAnalysis(const juce::File& file, AudioAnalysisRecord& record);

    bool getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData);

    bool storeSeratoTag(const FileIdentity& identity, const audiobatch::serato::TagReadResult& result);

    bool open();

    bool storeAnalysis(const AudioAnalysisRecord& record);

    bool storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData);

    bool getCustomGain(const juce::File& file, std::int64_t fileSize, std::int64_t modifiedTimeMs, float& customGainDb);

    bool storeCustomGain(const juce::File& file, float customGainDb, bool hasCustomGain);

    bool removeAnalysis(const juce::File& file);

    std::optional<audiobatch::serato::TagReadResult> getSeratoTag(
        const FileIdentity& identity,
        audiobatch::serato::TagKind kind = audiobatch::serato::TagKind::overview
    );

    juce::File getDatabaseFile() const;

    int pruneMissingFiles(const std::function<bool()>& shouldStop = {});

private:
    bool columnExists(const juce::String& tableName, const juce::String& columnName) const;

    bool addMissingColumns() const;

    bool execute(const juce::String& sql) const;

    bool prepare(const char* sql, sqlite3_stmt*& statement) const;

    bool stepToCompletion(sqlite3_stmt* statement) const;

    bool openUnlocked();

    bool removeRowVersion(
        const juce::String& path,
        std::int64_t fileSize,
        std::int64_t modifiedTimeMs,
        std::optional<std::int64_t> tagKind = std::nullopt
    ) const;

    void closeUnlocked();

    static juce::String normalizedPath(const juce::File& file);

    static constexpr int analysisVersion = 8;
    static constexpr int waveformVersion = 1;

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};
