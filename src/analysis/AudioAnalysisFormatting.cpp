/// Implementation of the functions that format analysis values for display in the GUI and CLI:
/// peak, true peak, and loudness levels, bitrate, sample rate, bit depth, file type, and status.

#include "analysis/AudioAnalysisFormatting.h"

#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

namespace audiobatch
{
namespace
{
/// Lowest level shown in dB, which very quiet peaks are floored at.
constexpr float minimumDisplayDecibels {-100.0f};
/// Number of bits per second in a kilobit per second, used for the average bitrate.
constexpr double kilobitsPerSecondDivisor {1000.0};
/// Number of bits in a byte, used to turn a file size into a bit count for the average bitrate.
constexpr double bitsPerByte {8.0};
/// Number of hertz in a kilohertz, used when displaying sample rates.
constexpr int hertzPerKilohertz {1000};

/// Converts a linear amplitude into a decibel string with the given unit suffix.
/// Silence renders as "-INF" so it never shows a misleading finite value.
juce::String formatAmplitudeDisplay(const double amplitude, const juce::String& unitSuffix)
{
    if (amplitude <= 0.0) {
        return utils::format("-INF{}", unitSuffix);
    }

    const auto decibels = juce::Decibels::gainToDecibels(static_cast<float>(amplitude), minimumDisplayDecibels);
    return utils::format("{:.2f}{}", decibels, unitSuffix);
}

/// Formats a LUFS loudness value with the given unit suffix, rendering the sentinel value as "-INF".
juce::String formatLoudnessValue(const double loudness, const juce::String& unitSuffix)
{
    if (loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return utils::format("-INF{}", unitSuffix);
    }

    return utils::format("{:.2f}{}", loudness, unitSuffix);
}
}  // namespace

/// Formats a peak value for display in the GUI and CLI.
juce::String formatPeakDisplay(const float peak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(static_cast<double>(peak)), " dBFS");
}

/// Formats a true-peak value for display in the GUI.
juce::String formatTruePeakDisplay(const double truePeak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(truePeak), " dBTP");
}

/// Formats a loudness value for display in the GUI.
juce::String formatLoudnessDisplay(const double loudness)
{
    return formatLoudnessValue(loudness, " LUFS");
}

/// Formats a peak value for compact CLI display without a unit suffix.
juce::String formatPeakCompact(const double peak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(peak), {});
}

/// Formats a true-peak value for compact CLI display without a unit suffix.
juce::String formatTruePeakCompact(const double truePeak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(truePeak), {});
}

/// Formats a loudness value for compact CLI display without a unit suffix.
juce::String formatLoudnessCompact(const double loudness)
{
    return formatLoudnessValue(loudness, {});
}

/// Returns the average file bitrate in kbps when it can be derived from the analyzed file.
double getAverageBitrateKbps(const AudioAnalysisRecord& record)
{
    if (record.durationSeconds <= 0.0 || record.fileSize <= 0) {
        return 0.0;
    }

    return static_cast<double>(record.fileSize) * bitsPerByte / record.durationSeconds / kilobitsPerSecondDivisor;
}

/// Formats the reported source bit depth for display, or "-" when unavailable.
juce::String formatBitsPerSampleDisplay(const AudioAnalysisRecord& record)
{
    if (record.bitsPerSample <= 0) {
        return utils::isMp3File(record.file) ? "Unknown" : "-";
    }

    return juce::String(record.bitsPerSample);
}

/// Formats the average bitrate for display in the GUI.
juce::String formatBitrateDisplay(const AudioAnalysisRecord& record)
{
    const auto bitrateKbps = getAverageBitrateKbps(record);

    if (bitrateKbps <= 0.0) {
        return "-";
    }

    return utils::format("{} kbps", juce::roundToInt(bitrateKbps));
}

/// Formats the per-file gain with an explicit sign for display in the GUI, or an empty string for a file without one.
juce::String formatCustomGainDisplay(const AudioAnalysisRecord& record)
{
    if (!record.hasCustomGain) {
        return {};
    }

    return utils::format("{}{:.1f} dB", record.customGainDb >= 0.0f ? "+" : "", record.customGainDb);
}

/// Formats the sample rate for display in the GUI.
juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record)
{
    if (record.sampleRate <= 0) {
        return "-";
    }

    if (record.sampleRate % hertzPerKilohertz == 0) {
        return utils::format("{} kHz", record.sampleRate / hertzPerKilohertz);
    }

    return utils::format("{:.1f} kHz", static_cast<double>(record.sampleRate) / hertzPerKilohertz);
}

/// Returns the short file type label for display: the uppercase extension, for example "FLAC",
/// then the reader's format name without its " file" suffix, and "Unknown" when neither is known.
juce::String formatTypeLabel(const AudioAnalysisRecord& record)
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

/// Formats the human-readable status text for a record.
juce::String formatStatus(const AudioAnalysisRecord& record)
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
}  // namespace audiobatch
