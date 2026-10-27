/// Implementation of the functions that format analysis values for display in the GUI and CLI:
/// peak, true peak, and loudness levels, bitrate, sample rate, bit depth, per-file gain, duration, file type,
/// and status.

#include "analysis/AudioAnalysisFormatting.h"

#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <cmath>
#include <cstdint>

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
/// Number of seconds in a minute, used when displaying durations.
constexpr std::int64_t secondsPerMinute {60};
/// Number of minutes in an hour, used when displaying durations.
constexpr std::int64_t minutesPerHour {60};
/// Number of seconds in an hour, used when displaying durations.
constexpr std::int64_t secondsPerHour {secondsPerMinute * minutesPerHour};
/// Number of tenths of a second in a second, the precision of the detailed duration.
constexpr std::int64_t tenthsPerSecond {10};

/// Converts a linear amplitude into decibels as text.
/// Silence renders as "-INF" so it never shows a misleading finite value.
juce::String formatAmplitudeDecibels(const double amplitude)
{
    if (amplitude <= 0.0) {
        return "-INF";
    }

    const auto decibels = juce::Decibels::gainToDecibels(static_cast<float>(amplitude), minimumDisplayDecibels);
    return utils::format("{:.2f}", decibels);
}

/// Formats whole seconds as "m:ss", or as "h:mm:ss" from one hour on.
juce::String formatClock(const std::int64_t totalSeconds)
{
    const auto hours = totalSeconds / secondsPerHour;
    const auto minutes = totalSeconds / secondsPerMinute % minutesPerHour;
    const auto seconds = totalSeconds % secondsPerMinute;

    if (hours > 0) {
        return utils::format("{}:{:02}:{:02}", hours, minutes, seconds);
    }

    return utils::format("{}:{:02}", minutes, seconds);
}

/// Formats a LUFS loudness value, rendering the sentinel value as "-INF".
juce::String formatLoudnessValue(const double loudness)
{
    if (loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return "-INF";
    }

    return utils::format("{:.2f}", loudness);
}
}  // namespace

/// Formats a peak value in dBFS without a unit suffix, for the CLI and the results table.
juce::String formatPeakCompact(const double peak)
{
    return formatAmplitudeDecibels(utils::peakMagnitude(peak));
}

/// Formats a true-peak value in dBTP without a unit suffix, for the CLI and the results table.
juce::String formatTruePeakCompact(const double truePeak)
{
    return formatAmplitudeDecibels(utils::peakMagnitude(truePeak));
}

/// Formats a loudness value in LUFS without a unit suffix, for the CLI and the results table.
juce::String formatLoudnessCompact(const double loudness)
{
    return formatLoudnessValue(loudness);
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

/// Formats the average bitrate in kbps without a unit suffix, or "-" when it cannot be derived.
juce::String formatBitrateCompact(const AudioAnalysisRecord& record)
{
    const auto bitrateKbps = getAverageBitrateKbps(record);

    if (bitrateKbps <= 0.0) {
        return "-";
    }

    return juce::String(juce::roundToInt(bitrateKbps));
}

/// Formats the average bitrate with its unit for display in the GUI, or "-" when it cannot be derived.
juce::String formatBitrateDisplay(const AudioAnalysisRecord& record)
{
    const auto bitrate = formatBitrateCompact(record);
    return bitrate == "-" ? bitrate : utils::format("{} kbps", bitrate);
}

/// Formats the per-file gain in dB with an explicit sign and without a unit suffix,
/// or an empty string for a file without one.
juce::String formatCustomGainCompact(const AudioAnalysisRecord& record)
{
    if (!record.customGain) {
        return {};
    }

    // Rounding to the shown precision first, and adding zero, turns a negative zero into a positive one,
    // so a gain that rounds to zero reads "+0.0" instead of "-0.0" or "+-0.0".
    const auto rounded = (std::round(record.customGain->value * 10.0f) / 10.0f) + 0.0f;
    return utils::format("{:+.1f}", rounded);
}

/// Formats a duration rounded to whole seconds as "m:ss", or "h:mm:ss" from one hour on, such as "3:15".
/// Returns "-" for a duration that is not known.
juce::String formatDurationClock(const double durationSeconds)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
        return "-";
    }

    return formatClock(std::llround(durationSeconds));
}

/// Formats a duration rounded to tenths of a second as "m:ss.t", or "h:mm:ss.t" from one hour on, such as "3:15.4".
/// Returns "-" for a duration that is not known.
juce::String formatDurationClockTenths(const double durationSeconds)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
        return "-";
    }

    const auto tenths = std::llround(durationSeconds * tenthsPerSecond);
    return utils::format("{}.{}", formatClock(tenths / tenthsPerSecond), tenths % tenthsPerSecond);
}

/// Formats the sample rate in kHz without a unit suffix, such as "48" or "44.1", or "-" when it is not known.
juce::String formatSampleRateCompact(const AudioAnalysisRecord& record)
{
    if (record.sampleRate <= 0) {
        return "-";
    }

    if (record.sampleRate % hertzPerKilohertz == 0) {
        return juce::String(record.sampleRate / hertzPerKilohertz);
    }

    return utils::format("{:.1f}", static_cast<double>(record.sampleRate) / hertzPerKilohertz);
}

/// Formats the sample rate with its unit for display in the GUI, or "-" when it is not known.
juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record)
{
    const auto sampleRate = formatSampleRateCompact(record);
    return sampleRate == "-" ? sampleRate : utils::format("{} kHz", sampleRate);
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
