/// AudioAnalysisService member functions that format analysis values for display in the GUI and CLI:
/// peak, true peak, and loudness levels, bitrate, sample rate, bit depth, file type, and status.

#include "AudioAnalysisService.h"
#include "AudioFormatUtils.h"
#include "FileUtils.h"
#include "StringFormat.h"

/// Helpers for formatting levels and file properties.
namespace audiobatch::analysis_formatting
{
constexpr float minimumDisplayDecibels = -100.0f;
constexpr double kilobitsPerSecondDivisor = 1000.0;
/// Number of bits in a byte, used to turn a file size into a bit count for the average bitrate.
constexpr double bitsPerByte = 8.0;
/// Number of hertz in a kilohertz, used when displaying sample rates.
constexpr int hertzPerKilohertz = 1000;

/// Converts a linear amplitude into a decibel string with the given unit suffix.
/// Silence renders as "-INF" so it never shows a misleading finite value.
static juce::String formatAmplitudeDisplay(const double amplitude, const juce::String& unitSuffix)
{
    if (amplitude <= 0.0) {
        return utils::format("-INF{}", unitSuffix);
    }

    const auto decibels = juce::Decibels::gainToDecibels(static_cast<float>(amplitude), minimumDisplayDecibels);
    return utils::format("{:.2f}{}", decibels, unitSuffix);
}

/// Formats a LUFS loudness value with the given unit suffix, rendering the sentinel value as "-INF".
static juce::String formatLoudnessValue(const double loudness, const juce::String& unitSuffix)
{
    if (loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return utils::format("-INF{}", unitSuffix);
    }

    return utils::format("{:.2f}{}", loudness, unitSuffix);
}
}  // namespace audiobatch::analysis_formatting

using namespace audiobatch::analysis_formatting;

juce::String AudioAnalysisService::formatPeakDisplay(const float peak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(static_cast<double>(peak)), " dBFS");
}

juce::String AudioAnalysisService::formatTruePeakDisplay(const double truePeak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(truePeak), " dBTP");
}

juce::String AudioAnalysisService::formatLoudnessDisplay(const double loudness)
{
    return formatLoudnessValue(loudness, " LUFS");
}

juce::String AudioAnalysisService::formatPeakCompact(const double peak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(peak), {});
}

juce::String AudioAnalysisService::formatTruePeakCompact(const double truePeak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(truePeak), {});
}

juce::String AudioAnalysisService::formatLoudnessCompact(const double loudness)
{
    return formatLoudnessValue(loudness, {});
}

double AudioAnalysisService::getAverageBitrateKbps(const AudioAnalysisRecord& record)
{
    if (record.durationSeconds <= 0.0 || record.fileSize <= 0) {
        return 0.0;
    }

    return static_cast<double>(record.fileSize) * bitsPerByte / record.durationSeconds / kilobitsPerSecondDivisor;
}

juce::String AudioAnalysisService::formatBitsPerSampleDisplay(const AudioAnalysisRecord& record)
{
    if (record.bitsPerSample <= 0) {
        return utils::isMp3File(record.file) ? "Unknown" : "-";
    }

    return juce::String(record.bitsPerSample);
}

juce::String AudioAnalysisService::formatBitrateDisplay(const AudioAnalysisRecord& record)
{
    const auto bitrateKbps = getAverageBitrateKbps(record);

    if (bitrateKbps <= 0.0) {
        return "-";
    }

    return utils::format("{} kbps", juce::roundToInt(bitrateKbps));
}

juce::String AudioAnalysisService::formatSampleRateDisplay(const AudioAnalysisRecord& record)
{
    if (record.sampleRate <= 0) {
        return "-";
    }

    if (record.sampleRate % hertzPerKilohertz == 0) {
        return utils::format("{} kHz", record.sampleRate / hertzPerKilohertz);
    }

    return utils::format("{:.1f} kHz", static_cast<double>(record.sampleRate) / hertzPerKilohertz);
}

juce::String AudioAnalysisService::formatTypeLabel(const AudioAnalysisRecord& record)
{
    if (const auto extension = utils::fileExtension(record.file); extension.isNotEmpty()) {
        return extension.toUpperCase();
    }

    // JUCE format names end with " file", such as "WAV file", which the label leaves out.
    const juce::String formatNameFileSuffix {" file"};
    auto formatName = record.formatName.trim();

    if (formatName.endsWithIgnoreCase(formatNameFileSuffix)) {
        formatName = formatName.dropLastCharacters(formatNameFileSuffix.length()).trimEnd();
    }

    return formatName.isNotEmpty() ? formatName : juce::String("Unknown");
}

juce::String AudioAnalysisService::formatStatus(const AudioAnalysisRecord& record)
{
    switch (record.status) {
        case AudioAnalysisStatus::cached:
            return "Cached";
        case AudioAnalysisStatus::analyzed:
            return "Analyzed";
        case AudioAnalysisStatus::failed:
            return "Failed";
        case AudioAnalysisStatus::pending:
            return "Pending";
    }

    std::unreachable();
}
