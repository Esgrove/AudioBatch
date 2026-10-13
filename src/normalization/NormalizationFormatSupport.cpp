/// Functions that report which formats normalization supports:
/// whether a file can be normalized, why not, and the summary of readable and writable formats in this build.
/// Writer support is checked by creating a probe writer in memory,
/// since some formats are registered for reading but cannot write.

#include "analysis/AudioAnalysisFormatting.h"
#include "normalization/AudioNormalization.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace audiobatch
{
/// Helpers for collecting and describing the normalization format support.
namespace
{
struct AudioNormalizationFormatSupport {
    juce::String formatName;
    juce::StringArray fileExtensions;
    bool canNormalize = false;
    juce::String detail;
};

/// Returns the one-line summary of normalization output behavior used in the support summary.
static juce::String getNormalizationStatusLine()
{
    return "Normalization output: same-name AIF files. "
           "Originals with a different extension are moved to the system trash.";
}

/// Builds minimal writer options from the format's advertised capabilities.
/// Returns default constructed options when the format advertises no usable configuration.
static juce::AudioFormatWriterOptions buildProbeWriterOptions(juce::AudioFormat& format)
{
    const auto sampleRates = format.getPossibleSampleRates();
    const auto bitDepths = format.getPossibleBitDepths();
    int numChannels = 0;
    if (format.canDoStereo()) {
        numChannels = 2;
    } else if (format.canDoMono()) {
        numChannels = 1;
    }

    if (sampleRates.isEmpty() || bitDepths.isEmpty() || numChannels <= 0) {
        return {};
    }

    return juce::AudioFormatWriterOptions()
        .withSampleRate(sampleRates[0])
        .withNumChannels(numChannels)
        .withBitsPerSample(bitDepths[0]);
}

/// Checks whether the format can actually create a writer by probing with an in-memory stream.
/// Some formats are read-only in JUCE even though they are registered with the format manager.
static bool canCreateProbeWriter(juce::AudioFormat& format)
{
    const auto options = buildProbeWriterOptions(format);

    if (options.getSampleRate() <= 0.0 || options.getNumChannels() <= 0 || options.getBitsPerSample() <= 0) {
        return false;
    }

    std::unique_ptr<juce::OutputStream> outputStream = std::make_unique<juce::MemoryOutputStream>();
    const auto writer = format.createWriterFor(outputStream, options);
    return writer != nullptr;
}

/// Describes what normalization does with files of the given format for the support summary.
/// Every extension of the format is checked, because JUCE lists `.aiff` before `.aif` for AIFF.
static juce::String getNormalizableFormatDetail(const juce::StringArray& extensions)
{
    const auto hasExtension = [&extensions](const char* extension) {
        return std::ranges::any_of(extensions, [extension](const juce::String& formatExtension) {
            return utils::normalizeExtension(formatExtension) == extension;
        });
    };

    if (hasExtension("aif")) {
        // The output path of a .aif source is the source path, so the existing file is rewritten in place.
        return "A .aif file is rewritten in place. "
               "Other AIFF extensions are converted to a same-name .aif file, "
               "and the original is moved to the system trash.";
    }

    if (hasExtension("mp3")) {
        return "Converted to a same-name AIFF file. The original MP3 is moved to the system trash.";
    }

    return "Converted to a same-name AIFF file. The original is moved to the system trash.";
}

/// Gathers per-format normalization support information for the support summary.
/// Normalizable formats sort before read-only ones, then alphabetically by name.
static std::vector<AudioNormalizationFormatSupport> collectFormatSupport()
{
    std::vector<AudioNormalizationFormatSupport> support;
    support.reserve(static_cast<std::size_t>(utils::threadLocalFormatManager().getNumKnownFormats()));

    // Normalization always writes AIFF output, so any readable format is normalizable
    // as long as an AIFF writer is available in this build.
    auto* aiffWriterFormat = utils::findAiffFormat(utils::threadLocalFormatManager());
    const bool aiffWriterAvailable = aiffWriterFormat != nullptr && canCreateProbeWriter(*aiffWriterFormat);

    for (int index = 0; index < utils::threadLocalFormatManager().getNumKnownFormats(); ++index) {
        const auto* format = utils::threadLocalFormatManager().getKnownFormat(index);

        if (format == nullptr) {
            continue;
        }

        AudioNormalizationFormatSupport entry;
        entry.formatName = format->getFormatName();
        entry.fileExtensions = format->getFileExtensions();
        entry.canNormalize = aiffWriterAvailable && !entry.fileExtensions.isEmpty();

        if (entry.canNormalize) {
            entry.detail = getNormalizableFormatDetail(entry.fileExtensions);
        } else {
            entry.detail = "AIFF writer is not available in this build, so this format cannot be normalized.";
        }

        support.push_back(std::move(entry));
    }

    std::ranges::sort(support, [](const auto& lhs, const auto& rhs) {
        if (lhs.canNormalize != rhs.canNormalize) {
            return lhs.canNormalize > rhs.canNormalize;
        }

        return lhs.formatName.compareNatural(rhs.formatName) < 0;
    });

    return support;
}

/// Joins the format's extensions into a lowercase, de-duplicated display string.
static juce::String formatExtensionsToText(const juce::StringArray& extensions)
{
    juce::StringArray normalizedExtensions;

    for (const auto& extension : extensions) {
        normalizedExtensions.addIfNotAlreadyThere(extension.toLowerCase());
    }

    return normalizedExtensions.joinIntoString(", ");
}
}  // namespace

/// Returns true when the file's format can be rewritten by this build during normalization.
bool canNormalizeFile(const juce::File& file)
{
    // Source must be readable by JUCE and AIFF output must be writable.
    const auto* readerFormat = utils::threadLocalFormatManager().findFormatForFileExtension(utils::fileExtension(file));

    if (readerFormat == nullptr) {
        return false;
    }

    return utils::findAiffFormat(utils::threadLocalFormatManager()) != nullptr;
}

/// True when the list is non-empty and every record's format can be normalized in this build.
bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records)
{
    if (records.empty()) {
        return false;
    }

    return std::ranges::all_of(records, [](const auto& record) { return canNormalizeFile(record.file); });
}

/// Builds a user-facing explanation of why the given files cannot be normalized,
/// listing each unsupported file with its reason.
/// Returns an empty string when every file is supported.
juce::String buildNormalizationUnavailableMessage(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray unsupportedLines;

    for (const auto& record : records) {
        const auto reason = getNormalizationSupportMessage(record.file);

        if (reason.isEmpty()) {
            continue;
        }

        unsupportedLines.add(utils::format("{} ({}): {}", record.fileName, formatTypeLabel(record), reason));
    }

    if (unsupportedLines.isEmpty()) {
        return {};
    }

    return utils::format(
        "The selected files cannot be normalized with the current build:\n\n{}\n\n{}",
        unsupportedLines.joinIntoString(juce::newLine),
        getFormatSupportSummary()
    );
}

/// Describes why normalization is unavailable for the given file format, or returns an empty string when supported.
juce::String getNormalizationSupportMessage(const juce::File& file)
{
    const auto* format = utils::threadLocalFormatManager().findFormatForFileExtension(utils::fileExtension(file));

    if (format == nullptr) {
        return "Unsupported audio format";
    }

    if (utils::findAiffFormat(utils::threadLocalFormatManager()) == nullptr) {
        return "AIFF writer is not available in this build";
    }

    return {};
}

/// Summarizes the readable and writable formats available for in-place normalization in this build.
juce::String getFormatSupportSummary()
{
    const auto support = collectFormatSupport();

    juce::StringArray writableLines;
    juce::StringArray readOnlyLines;

    for (const auto& [formatName, fileExtensions, canNormalize, detail] : support) {
        const auto line = detail.isNotEmpty()
            ? utils::format("- {} ({}): {}", formatName, formatExtensionsToText(fileExtensions), detail)
            : utils::format("- {} ({})", formatName, formatExtensionsToText(fileExtensions));

        if (canNormalize) {
            writableLines.add(line);
        } else {
            readOnlyLines.add(line);
        }
    }

    // The summary is only shown in GUI dialogs, so plain '\n' line breaks are fine.
    auto message = utils::format(
        "{}\n\n"
        "Normalization rewrites .aif files in place and converts every other supported file, "
        "including other AIFF extensions, to a .aif file of the same base name. "
        "Metadata (tags, album art, custom frames) is read from the source via TagLib "
        "and written back to the AIFF output as ID3v2.4.\n\n",
        getNormalizationStatusLine()
    );

    if (!writableLines.isEmpty()) {
        message += utils::format("Normalization available here:\n{}\n\n", writableLines.joinIntoString("\n"));
    }

    if (!readOnlyLines.isEmpty()) {
        message += utils::format("Readable but not normalizable here:\n{}", readOnlyLines.joinIntoString("\n"));
    }

    return message.trimEnd();
}
}  // namespace audiobatch
