/// SQLite-backed cache for audio analysis results.
/// AnalysisCache stores per-file analysis records, short-term loudness history,
/// and independent waveform and spectrogram previews keyed by path, size, and modification time,
/// so unchanged files are not re-read on later runs.
/// It owns the database and its schema, including the Serato tag table that serato/SeratoTagCache reads and writes.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/Sqlite.h"
#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
struct SpectrogramData;

/// A user-controlled gain to store for one file.
struct CustomGainUpdate {
    // Preserve the file, gain, and captured version order used by aggregate edit requests.
    /// The file whose cached row gets the gain.
    juce::File file;
    /// The new gain, or nothing to clear it.
    std::optional<Decibels> customGain;
    /// Size captured when the edit was made, or -1 for the synchronous compatibility API.
    std::int64_t fileSize {-1};
    /// Modification time captured when the edit was made, or -1 for the synchronous compatibility API.
    std::int64_t modifiedTimeMs {-1};
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

    [[nodiscard]] juce::File getDatabaseFile() const;

    [[nodiscard]] std::optional<AudioAnalysisRecord> getAnalysis(const juce::File& file);
    [[nodiscard]] std::optional<Decibels> getCustomGain(
        const juce::File& file,
        std::int64_t fileSize,
        std::int64_t modifiedTimeMs
    );
    [[nodiscard]] std::shared_ptr<const SpectrogramData> getSpectrogramData(const juce::File& file);
    [[nodiscard]] std::optional<juce::MemoryBlock> getWaveformData(const juce::File& file);

    [[nodiscard]] bool open();
    [[nodiscard]] bool removeAnalysis(const juce::File& file);
    [[nodiscard]] bool removeAnalysis(const AudioAnalysisRecord& record);
    [[nodiscard]] bool storeAnalysis(const AudioAnalysisRecord& record);
    [[nodiscard]] bool storeCustomGain(const juce::File& file, std::optional<Decibels> customGain);
    [[nodiscard]] bool storeWaveformData(const utils::FileIdentity& identity, const juce::MemoryBlock& waveformData);
    [[nodiscard]] bool storeSpectrogramData(
        const utils::FileIdentity& identity,
        const SpectrogramData& spectrogramData
    );

    [[nodiscard]] int pruneMissingFiles(const std::function<bool()>& shouldStop = {});
    [[nodiscard]] int storeCustomGains(const std::vector<CustomGainUpdate>& updates);

    void withDatabase(const std::function<void(sqlite3& database, sqlite::StatementCache& statements)>& function);

private:
    static juce::String normalizedPath(const juce::File& file);

    [[nodiscard]] bool addMissingColumns() const;
    [[nodiscard]] bool columnExists(
        const juce::String& tableName,
        const juce::String& columnName,
        bool requireNotNull = false
    ) const;
    bool openUnlocked();
    [[nodiscard]] bool removeRowVersion(
        const juce::String& path,
        std::int64_t fileSize,
        std::int64_t modifiedTimeMs,
        std::optional<std::int64_t> tagKind = std::nullopt
    );

    void closeUnlocked();

    /// Raised when the analysis changed, so cached results of the older analysis are analyzed again.
    static constexpr int analysisVersion {16};
    /// Raised when waveform detail or source-version rules change, so older peaks are generated again.
    static constexpr int waveformVersion {3};

    /// Serializes every use of the connection, which is opened without SQLite's own mutex.
    juce::CriticalSection mutex;
    /// The SQLite database file of the cache.
    juce::File databaseFile;

    /// The open connection, or null until open succeeds and after the cache closed it.
    sqlite::Database database;
    /// The prepared statements of the connection, cleared before the connection closes.
    /// Declared after it, so they are also finalized first when the cache is destroyed.
    sqlite::StatementCache statements;
};
}  // namespace audiobatch
