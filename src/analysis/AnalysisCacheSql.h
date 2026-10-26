/// Internal SQL constants shared by the AnalysisCache sources.
/// Holds the column and bind parameter indices of the cache statements, which must match the SQL text exactly.
/// The SQLite handles and value marshalling helpers are in analysis/Sqlite.h.

#pragma once

namespace audiobatch
{
/// How long SQLite retries a locked database before failing a statement, in milliseconds.
constexpr int busyTimeoutMilliseconds {2000};

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
constexpr int shortTermLoudness {22};
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
constexpr int shortTermLoudness {27};
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
}  // namespace audiobatch
