/// Internal SQL constants shared by the AnalysisCache sources.
/// Holds the column and bind parameter indices of the cache statements, which must match the SQL text exactly.
/// The SQLite handles and value marshalling helpers are in analysis/Sqlite.h.

#pragma once

namespace audiobatch
{
/// How long SQLite retries a locked database before failing a statement, in milliseconds.
constexpr int BUSY_TIMEOUT_MILLISECONDS {2000};

/// Zero-based result column indices for the analysis record SELECT in AnalysisCache::getAnalysis.
/// The order must match the column list of that query exactly.
namespace analysis_column
{
constexpr int FILE_NAME {0};
constexpr int FORMAT_NAME {1};
constexpr int FILE_SIZE {2};
constexpr int MODIFIED_TIME_MS {3};
constexpr int LENGTH_IN_SAMPLES {4};
constexpr int DURATION_SECONDS {5};
constexpr int PEAK_LEFT {6};
constexpr int PEAK_RIGHT {7};
constexpr int OVERALL_PEAK {8};
constexpr int TRUE_PEAK_LEFT {9};
constexpr int TRUE_PEAK_RIGHT {10};
constexpr int OVERALL_TRUE_PEAK {11};
constexpr int MAX_SHORT_TERM_LUFS {12};
constexpr int INTEGRATED_LUFS {13};
constexpr int SAMPLE_RATE {14};
constexpr int CHANNELS {15};
constexpr int BITS_PER_SAMPLE {16};
constexpr int STATUS {17};
constexpr int ERROR_MESSAGE {18};
constexpr int ANALYSIS_VERSION {19};
constexpr int CUSTOM_GAIN_DB {20};
constexpr int HAS_CUSTOM_GAIN {21};
constexpr int SHORT_TERM_LOUDNESS {22};
constexpr int OVERLOAD_HISTORY {23};
constexpr int MEASUREMENT_PRODUCER {24};
constexpr int PLR_DB {25};
constexpr int PSR_DB {26};
constexpr int CREST_FACTOR_DB {27};
constexpr int DC_OFFSETS {28};
constexpr int CLIP_COUNTS {29};
constexpr int STEREO_CORRELATION {30};
}  // namespace analysis_column

/// One-based bind parameter indices for the analysis record upsert in AnalysisCache::storeAnalysis.
/// The order must match the column list of that INSERT statement exactly.
namespace analysis_parameter
{
constexpr int FILE_PATH {1};
constexpr int FILE_NAME {2};
constexpr int FORMAT_NAME {3};
constexpr int FILE_SIZE {4};
constexpr int MODIFIED_TIME_MS {5};
constexpr int LENGTH_IN_SAMPLES {6};
constexpr int DURATION_SECONDS {7};
constexpr int PEAK_LEFT {8};
constexpr int PEAK_RIGHT {9};
constexpr int OVERALL_PEAK {10};
constexpr int TRUE_PEAK_LEFT {11};
constexpr int TRUE_PEAK_RIGHT {12};
constexpr int OVERALL_TRUE_PEAK {13};
constexpr int MAX_SHORT_TERM_LUFS {14};
constexpr int INTEGRATED_LUFS {15};
constexpr int SAMPLE_RATE {16};
constexpr int CHANNELS {17};
constexpr int BITS_PER_SAMPLE {18};
constexpr int STATUS {19};
constexpr int ERROR_MESSAGE {20};
constexpr int WAVEFORM_DATA {21};
constexpr int WAVEFORM_VERSION {22};
constexpr int ANALYSIS_VERSION {23};
constexpr int UPDATED_AT_MS {24};
constexpr int CUSTOM_GAIN_DB {25};
constexpr int HAS_CUSTOM_GAIN {26};
constexpr int SHORT_TERM_LOUDNESS {27};
constexpr int OVERLOAD_HISTORY {28};
constexpr int MEASUREMENT_PRODUCER {29};
constexpr int PLR_DB {30};
constexpr int PSR_DB {31};
constexpr int CREST_FACTOR_DB {32};
constexpr int DC_OFFSETS {33};
constexpr int CLIP_COUNTS {34};
constexpr int STEREO_CORRELATION {35};
constexpr int OBSERVED_SOURCE_PRESENT {36};
constexpr int OBSERVED_FILE_SIZE {37};
constexpr int OBSERVED_MODIFIED_TIME_MS {38};
}  // namespace analysis_parameter

/// One-based bind parameter indices for the waveform UPDATE in AnalysisCache::storeWaveformData.
/// The order must match the placeholders of that statement exactly.
namespace waveform_parameter
{
constexpr int WAVEFORM_DATA {1};
constexpr int WAVEFORM_VERSION {2};
constexpr int UPDATED_AT_MS {3};
constexpr int FILE_PATH {4};
constexpr int FILE_SIZE {5};
constexpr int MODIFIED_TIME_MS {6};
constexpr int ANALYSIS_VERSION {7};
constexpr int LENGTH_IN_SAMPLES {8};
constexpr int SAMPLE_RATE {9};
constexpr int CHANNELS {10};
}  // namespace waveform_parameter
}  // namespace audiobatch
