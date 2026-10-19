/// Internal SQL constants and value marshalling helpers shared by the AnalysisCache sources.
/// Holds the column and bind parameter indices of the cache statements, which must match the SQL text exactly,
/// and the helpers that convert between SQLite values and JUCE types.

#pragma once

#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <cstddef>
#include <cstdint>
#include <sqlite3.h>

namespace audiobatch
{
/// How long SQLite retries a locked database before failing a statement, in milliseconds.
constexpr int busyTimeoutMilliseconds {2000};

/// Maximum numeric tag cache size, including the explicit serializer header fields.
constexpr std::size_t maximumOverviewBytes {
    4 * sizeof(std::uint32_t) + serato::Overview::maxSlices * serato::Overview::bandCount
};

/// Zero-based columns for independent tag lookup, including the key's stored SQLite types.
namespace serato_column
{
constexpr int fileSize {0};
constexpr int modifiedTimeMs {1};
constexpr int parserVersion {2};
constexpr int resultState {3};
constexpr int decodedData {4};
constexpr int updatedAtMs {5};
constexpr int filePath {6};
constexpr int tagKind {7};
}  // namespace serato_column

/// One-based parameters for independent per-kind tag UPSERT.
namespace serato_parameter
{
constexpr int filePath {1};
constexpr int tagKind {2};
constexpr int fileSize {3};
constexpr int modifiedTimeMs {4};
constexpr int parserVersion {5};
constexpr int resultState {6};
constexpr int decodedData {7};
constexpr int updatedAtMs {8};
}  // namespace serato_parameter

/// Zero-based result column indices for the analysis record SELECT in AnalysisCache::getAnalysis.
/// The order must match the column list of that query exactly.
namespace analysis_column
{
constexpr int fileName {0};
constexpr int formatName {1};
constexpr int fileSize {2};
constexpr int modifiedTimeMs {3};
constexpr int lengthInSamples {4};
constexpr int durationSeconds {5};
constexpr int peakLeft {6};
constexpr int peakRight {7};
constexpr int overallPeak {8};
constexpr int truePeakLeft {9};
constexpr int truePeakRight {10};
constexpr int overallTruePeak {11};
constexpr int maxShortTermLufs {12};
constexpr int integratedLufs {13};
constexpr int sampleRate {14};
constexpr int channels {15};
constexpr int bitsPerSample {16};
constexpr int status {17};
constexpr int errorMessage {18};
constexpr int analysisVersion {19};
constexpr int customGainDb {20};
constexpr int hasCustomGain {21};
}  // namespace analysis_column

/// One-based bind parameter indices for the analysis record upsert in AnalysisCache::storeAnalysis.
/// The order must match the column list of that INSERT statement exactly.
namespace analysis_parameter
{
constexpr int filePath {1};
constexpr int fileName {2};
constexpr int formatName {3};
constexpr int fileSize {4};
constexpr int modifiedTimeMs {5};
constexpr int lengthInSamples {6};
constexpr int durationSeconds {7};
constexpr int peakLeft {8};
constexpr int peakRight {9};
constexpr int overallPeak {10};
constexpr int truePeakLeft {11};
constexpr int truePeakRight {12};
constexpr int overallTruePeak {13};
constexpr int maxShortTermLufs {14};
constexpr int integratedLufs {15};
constexpr int sampleRate {16};
constexpr int channels {17};
constexpr int bitsPerSample {18};
constexpr int status {19};
constexpr int errorMessage {20};
constexpr int waveformData {21};
constexpr int waveformVersion {22};
constexpr int analysisVersion {23};
constexpr int updatedAtMs {24};
constexpr int customGainDb {25};
constexpr int hasCustomGain {26};
}  // namespace analysis_parameter

/// One-based bind parameter indices for the waveform UPDATE in AnalysisCache::storeWaveformData.
/// The order must match the placeholders of that statement exactly.
namespace waveform_parameter
{
constexpr int waveformData {1};
constexpr int waveformVersion {2};
constexpr int updatedAtMs {3};
constexpr int filePath {4};
constexpr int fileSize {5};
constexpr int modifiedTimeMs {6};
constexpr int analysisVersion {7};
}  // namespace waveform_parameter

/// Reads a text column as a juce::String, mapping SQL NULL to an empty string.
inline juce::String columnText(sqlite3_stmt* statement, const int columnIndex)
{
    const auto* text = sqlite3_column_blob(statement, columnIndex);
    return text != nullptr ? juce::String::createStringFromData(text, sqlite3_column_bytes(statement, columnIndex))
                           : juce::String();
}

/// Copies a blob column into a juce::MemoryBlock, returning an empty block for NULL or empty data.
inline juce::MemoryBlock columnBlob(sqlite3_stmt* statement, const int columnIndex)
{
    juce::MemoryBlock data;

    if (const auto* blob = sqlite3_column_blob(statement, columnIndex)) {
        const auto size = sqlite3_column_bytes(statement, columnIndex);

        if (size > 0) {
            data.append(blob, static_cast<size_t>(size));
        }
    }

    return data;
}

/// Binds a juce::String as UTF-8 text.
///
/// SQLITE_TRANSIENT makes SQLite copy the buffer, so the string does not need to outlive the statement.
inline void bindText(sqlite3_stmt* statement, const int index, const juce::String& value)
{
    const auto* utf8 = value.toRawUTF8();
    sqlite3_bind_text(statement, index, utf8, -1, SQLITE_TRANSIENT);
}

/// Binds a memory block as a blob.
///
/// SQLITE_TRANSIENT makes SQLite copy the data, so the block does not need to outlive the statement.
inline void bindBlob(sqlite3_stmt* statement, const int index, const juce::MemoryBlock& data)
{
    sqlite3_bind_blob(statement, index, data.getData(), static_cast<int>(data.getSize()), SQLITE_TRANSIENT);
}

}  // namespace audiobatch
