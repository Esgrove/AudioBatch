/// Implementation of the functions that format analysis values for display in the GUI and CLI:
/// peak, true peak, and loudness levels, bitrate, sample rate, bit depth, per-file gain, duration, file size,
/// file type, and status. Musical key mapping recognizes common tag spellings and DJ wheel notation.

#include "analysis/AudioAnalysisFormatting.h"

#include "analysis/AudioAnalysisTypes.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace audiobatch
{
/// Sums channel sample counts rather than counting affected frames.
std::uint64_t SignalMetrics::totalClips() const
{
    std::uint64_t total = 0;
    for (const auto count : clipCounts) {
        total += count;
    }
    return total;
}

/// Returns the largest absolute channel mean for the compact table column.
std::optional<double> SignalMetrics::maximumDcOffset() const
{
    std::optional<double> maximum;
    for (const auto value : dcOffsets) {
        maximum = std::max(maximum.value_or(0.0), std::abs(value));
    }
    return maximum;
}

/// Formats a source ratio or coefficient, with a dash for an undefined result.
juce::String formatMetric(const std::optional<double> value, const bool signedValue)
{
    if (!value || !std::isfinite(*value)) {
        return "-";
    }
    const auto rounded = std::round(*value * 100.0) / 100.0;
    const auto normalized = juce::approximatelyEqual(rounded, 0.0) ? 0.0 : rounded;
    return signedValue ? utils::format("{:+.2f}", normalized) : utils::format("{:.2f}", normalized);
}

/// Formats signed source means in percent full scale, one labelled value per channel.
juce::String formatDcOffsets(const SignalMetrics& metrics)
{
    juce::StringArray values;
    for (std::size_t channel = 0; channel < metrics.dcOffsets.size(); ++channel) {
        values.add(utils::format("Ch {}: {:+.4f}%", channel + 1, metrics.dcOffsets[channel] * 100.0));
    }
    return values.isEmpty() ? juce::String("-") : values.joinIntoString(", ");
}

/// Formats source clip sample counts by channel, retaining zeros as valid measurements.
juce::String formatClipCounts(const SignalMetrics& metrics)
{
    juce::StringArray values;
    for (std::size_t channel = 0; channel < metrics.clipCounts.size(); ++channel) {
        values.add(utils::format("Ch {}: {}", channel + 1, metrics.clipCounts[channel]));
    }
    return values.isEmpty() ? juce::String("-") : values.joinIntoString(", ");
}

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

/// Number of pitch classes in a chromatic octave and entries on each Camelot wheel.
constexpr int KEY_COUNT {12};
/// Camelot wheel numbers indexed by chromatic pitch class from C to B.
constexpr std::array<int, KEY_COUNT> MINOR_CAMELOT {5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};
constexpr std::array<int, KEY_COUNT> MAJOR_CAMELOT {8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
/// Decimal byte units used for the detailed file size.
constexpr std::int64_t BYTES_PER_KILOBYTE {1000};
constexpr std::int64_t BYTES_PER_MEGABYTE {1'000'000};

/// Parses numbered DJ key notation, allowing a single leading zero and whitespace before its suffix.
std::optional<juce::String> numberedCamelotKey(const juce::String& text)
{
    int digitCount = 0;
    while (digitCount < text.length() && text[digitCount] >= '0' && text[digitCount] <= '9') {
        ++digitCount;
    }
    const auto rawDigits = text.substring(0, digitCount);
    const auto digits = rawDigits.startsWith("0") ? rawDigits.substring(1) : rawDigits;
    if (digits.isEmpty() || digits.length() > 2 || digits.startsWith("0")) {
        return std::nullopt;
    }
    const auto number = digits.getIntValue();
    const auto suffix = text.substring(digitCount).trim();
    if (number < 1 || number > KEY_COUNT || suffix.length() != 1) {
        return std::nullopt;
    }
    if (suffix == "a" || suffix == "b") {
        return utils::format("{}{}", number, suffix.toUpperCase());
    }
    if (suffix == "m" || suffix == "d") {
        constexpr int OPEN_KEY_OFFSET {6};
        return utils::format("{}{}", (number + OPEN_KEY_OFFSET) % KEY_COUNT + 1, suffix == "m" ? "A" : "B");
    }
    return std::nullopt;
}

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

/// Shortens macOS home folder prefixes for display, leaving shared and non-home folders absolute.
juce::String formatFolderText(const juce::String& folder, const bool macos)
{
    const juce::String usersPrefix {"/Users/"};
    if (!macos || !folder.startsWith(usersPrefix)) {
        return folder;
    }
    const auto remainder = folder.substring(usersPrefix.length());
    const auto user = remainder.upToFirstOccurrenceOf("/", false, false);
    if (user.isEmpty() || user == "Shared") {
        return folder;
    }
    const auto inside = remainder.fromFirstOccurrenceOf("/", false, false);
    return inside.isEmpty() ? juce::String("~") : "~/" + inside;
}

/// Formats a file's parent directory, shortening macOS home prefixes without changing stored paths.
juce::String formatFolder(const juce::File& file)
{
    return formatFolderText(file.getParentDirectory().getFullPathName(), JUCE_MAC != 0);
}

/// Maps a recognized musical key to its canonical Camelot notation.
std::optional<juce::String> camelotKey(const juce::String& key)
{
    auto text = key.trim().toLowerCase();
    if (text.isEmpty()) {
        return std::nullopt;
    }
    if (text[0] >= '0' && text[0] <= '9') {
        return numberedCamelotKey(text);
    }

    const auto tonicIndex = juce::String("cdefgab").indexOfChar(text[0]);
    if (tonicIndex < 0) {
        return std::nullopt;
    }
    // Natural-note pitch classes ordered C, D, E, F, G, A, B in the chromatic octave.
    constexpr std::array NATURAL_PITCH_CLASSES {0, 2, 4, 5, 7, 9, 11};
    auto pitchClass = NATURAL_PITCH_CLASSES[static_cast<std::size_t>(tonicIndex)];
    text = text.substring(1).trim();
    if (text.startsWith("#") || text.startsWith(juce::String::fromUTF8("♯"))) {
        ++pitchClass;
        text = text.substring(1).trim();
    } else if (text.startsWith("b") || text.startsWith(juce::String::fromUTF8("♭"))) {
        --pitchClass;
        text = text.substring(1).trim();
    } else if (text.startsWith("sharp")) {
        ++pitchClass;
        text = text.substring(juce::String("sharp").length()).trim();
    } else if (text.startsWith("flat")) {
        --pitchClass;
        text = text.substring(juce::String("flat").length()).trim();
    }
    const bool minor = text == "m" || text == "min" || text == "minor";
    if (!minor && text.isNotEmpty() && text != "maj" && text != "major") {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>((pitchClass + KEY_COUNT) % KEY_COUNT);
    return utils::format("{}{}", minor ? MINOR_CAMELOT[index] : MAJOR_CAMELOT[index], minor ? "A" : "B");
}

/// Formats a known file size in decimal KB or MB with one decimal place, or "-" when unavailable.
juce::String formatFileSizeDisplay(const AudioAnalysisRecord& record)
{
    if (record.fileSize <= 0) {
        return "-";
    }
    const bool megabytes = record.fileSize >= BYTES_PER_MEGABYTE;
    const auto divisor = megabytes ? BYTES_PER_MEGABYTE : BYTES_PER_KILOBYTE;
    return utils::format(
        "{:.1f} {}", static_cast<double>(record.fileSize) / static_cast<double>(divisor), megabytes ? "MB" : "KB"
    );
}

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

/// Formats measurement provenance for GUI settled statuses, without changing CLI status text.
juce::String formatGuiStatus(const AudioAnalysisRecord& record)
{
    if (record.status == AudioAnalysisStatus::cached && record.measurementProducer == MeasurementProducer::rust) {
        return "Cached (Rust)";
    }
    return formatStatus(record);
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
