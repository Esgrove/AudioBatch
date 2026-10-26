/// Implementation of the normalization functions.
/// Reads source files through per-thread JUCE format readers, applies the peak or loudness gain in chunks,
/// and writes AIFF output. Carries source tags and embedded art across through Metadata handling.
/// The format support reporting lives in NormalizationFormatSupport.cpp.

#include "normalization/AudioNormalization.h"

#include "analysis/DecodeFailureTracker.h"
#include "utils/AudioFormatUtils.h"
#include "utils/AudioMetadata.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace audiobatch
{
/// Helpers for bit depth selection and chunked normalization.
namespace
{
constexpr int normalizationBlockSize {32'768};
constexpr auto normalizedAiffOutputExtension {".aif"};
constexpr int defaultMp3BitsPerSample {16};
constexpr auto cancelledMessage {"Normalization was cancelled"};

/// Share of a file's progress taken by analyzing a source that changed since its last analysis.
constexpr float sourceAnalysisProgressShare {0.3f};
/// Progress at which rendering the normalized audio ends and analyzing the output begins.
constexpr float renderProgressEnd {0.8f};

/// Logs the failure and builds a failure result for the given file.
AudioNormalizationResult failNormalization(const juce::File& file, const juce::String& message)
{
    utils::logError("Normalization failed for {}: {}", file.getFullPathName().quoted(), message);
    return AudioNormalizationResult::failure(file, message);
}

/// Moves the finished output into place, disposing of the files it replaces through options.disposeFile.
/// Normalizing again cannot restore the original exactly, because the output is quantized again,
/// so an original rewritten in place keeps a copy in the trash like a converted original does.
/// Returns the user-facing error on failure.
std::expected<void, juce::String> commitNormalizedOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& file,
    const juce::File& outputFile,
    const AudioNormalizationOptions& options
)
{
    if (outputFile == file && !utils::disposeOfCopy(file, options.disposeFile)) {
        return std::unexpected(juce::String("Could not move a copy of the original file to the system trash"));
    }

    return utils::commitTemporaryOutput(
        temporaryFile, file, outputFile, options.overwriteExisting, options.disposeFile
    );
}

/// Picks a bit depth the writer format supports, preferring the requested depth.
/// Falls back to the format's highest supported depth so no precision is lost.
int resolveWriterBitDepth(juce::AudioFormat& format, const int preferredBitsPerSample)
{
    const auto bitDepths = format.getPossibleBitDepths();

    if (bitDepths.isEmpty() || preferredBitsPerSample <= 0) {
        return preferredBitsPerSample;
    }

    if (bitDepths.contains(preferredBitsPerSample)) {
        return preferredBitsPerSample;
    }

    int resolvedBitsPerSample = bitDepths[0];

    for (const auto bitDepth : bitDepths) {
        resolvedBitsPerSample = std::max(bitDepth, resolvedBitsPerSample);
    }

    return resolvedBitsPerSample;
}

/// Chooses the output bit depth for the normalized file.
/// Prefers the analyzed source depth, then the MP3 default, then the reader's reported depth.
int resolvePreferredOutputBitDepth(
    const AudioAnalysisRecord& sourceRecord,
    const juce::AudioFormatReader& reader,
    const juce::File& sourceFile
)
{
    if (sourceRecord.bitsPerSample > 0) {
        return sourceRecord.bitsPerSample;
    }

    if (utils::isMp3File(sourceFile)) {
        return defaultMp3BitsPerSample;
    }

    return static_cast<int>(reader.bitsPerSample);
}

/// Streams the source through the writer block by block, applying the normalization gain.
/// Read failures follow the shared DecodeFailureTracker rules.
/// Returns the tracker's notice about tolerated failures, which is empty when there were none,
/// or a user-facing error message, including when cancellation is requested between blocks.
/// The written fraction of the file is reported to onProgress after each block.
std::expected<juce::String, juce::String> renderNormalizedAudio(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const float gain,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    juce::AudioBuffer<float> buffer(static_cast<int>(reader.numChannels), normalizationBlockSize);
    std::vector<float*> channelPointers(reader.numChannels);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        channelPointers[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);
    }

    DecodeFailureTracker failureTracker(reader.lengthInSamples);

    for (juce::int64 samplePosition = 0; samplePosition < reader.lengthInSamples;
         samplePosition += normalizationBlockSize)
    {
        if (utils::isCancellationRequested(isCancelled)) {
            return std::unexpected(juce::String(cancelledMessage));
        }

        const auto samplesThisBlock = static_cast<int>(
            juce::jmin<juce::int64>(normalizationBlockSize, reader.lengthInSamples - samplePosition)
        );

        // A failed read leaves the destination untouched, so without clearing, a
        // tolerated failure would write the previous, already gained block again.
        // The samples are zeroed through the channel pointers, because
        // AudioBuffer::clear marks the whole buffer as clear and applyGain would
        // then skip the samples the reader writes through those pointers.
        for (auto* const channelPointer : channelPointers) {
            juce::FloatVectorOperations::clear(channelPointer, samplesThisBlock);
        }

        const auto readSucceeded
            = utils::readFloatSamples(reader, std::span(channelPointers), samplePosition, samplesThisBlock);

        if (!failureTracker.recordBlock(readSucceeded, samplePosition, samplesThisBlock)) {
            return std::unexpected(failureTracker.getErrorMessage());
        }

        buffer.applyGain(gain);

        if (!writer.writeFromAudioSampleBuffer(buffer, 0, samplesThisBlock)) {
            return std::unexpected(juce::String("Failed while writing normalized audio data"));
        }

        utils::reportProgress(
            onProgress, utils::progressFraction(samplePosition + samplesThisBlock, reader.lengthInSamples)
        );
    }

    return failureTracker.getNotice();
}
}  // namespace

/// Returns the output path normalization writes for the given source, which is always a sibling `<name>.aif`.
juce::File getNormalizationOutputFile(const juce::File& sourceFile)
{
    return sourceFile.getSiblingFile(sourceFile.getFileNameWithoutExtension() + normalizedAiffOutputExtension);
}

/// Returns the full paths of the records that cannot be normalized in this batch
/// because an earlier record in the batch writes the same output file.
/// A source that is already the output file, such as `<name>.aif` normalized in place, always wins.
/// Otherwise the record that sorts first by path wins, so the result does not depend on input order.
juce::StringArray findBatchOutputConflicts(const std::vector<AudioAnalysisRecord>& records)
{
    auto ordered = records | std::views::transform([](const AudioAnalysisRecord& record) { return &record; })
        | std::ranges::to<std::vector>();

    // In-place sources claim their output first, then the rest in path order.
    std::ranges::sort(ordered, [](const AudioAnalysisRecord* lhs, const AudioAnalysisRecord* rhs) {
        const bool lhsInPlace = getNormalizationOutputFile(lhs->file) == lhs->file;
        const bool rhsInPlace = getNormalizationOutputFile(rhs->file) == rhs->file;

        if (lhsInPlace != rhsInPlace) {
            return lhsInPlace;
        }

        return lhs->file.getFullPathName() < rhs->file.getFullPathName();
    });

    juce::StringArray claimedOutputs;
    juce::StringArray conflicts;

    for (const auto* record : ordered) {
        const auto outputPath = getNormalizationOutputFile(record->file).getFullPathName();

        if (claimedOutputs.contains(outputPath, !juce::File::areFileNamesCaseSensitive())) {
            conflicts.add(record->file.getFullPathName());
        } else {
            claimedOutputs.add(outputPath);
        }
    }

    return conflicts;
}

/// Returns the linear gain that brings the analyzed file to the target in the options,
/// or the user-facing reason when the file has no measurable level for that mode.
/// A gain above AudioNormalizationOptions::maximumGainDb, or one that is not finite, is refused instead of capped,
/// since a capped gain would leave the file short of the target without telling the caller.
std::expected<float, juce::String> getNormalizationGain(
    const AudioAnalysisRecord& record,
    const AudioNormalizationOptions& options
)
{
    const auto peak = static_cast<double>(utils::peakMagnitude(record.overallPeak));

    if (!std::isfinite(peak) || peak <= 0.0) {
        return std::unexpected(juce::String("File contains no signal that can be normalized"));
    }

    // Decibels::gainToDecibels clamps at -100 dB by default, which would hide how quiet a nearly silent file is.
    constexpr double decibelFloor {-1000.0};
    double gainDecibels {};

    if (options.mode == AudioNormalizationMode::peak) {
        gainDecibels = options.targetPeakDbfs - juce::Decibels::gainToDecibels(peak, decibelFloor);
    } else {
        if (record.integratedLufs <= AudioAnalysisRecord::negativeInfinityLoudness) {
            return std::unexpected(juce::String("File has no measurable loudness to normalize"));
        }

        gainDecibels = options.targetLufs - record.integratedLufs;

        // The true peak scales with the gain, so the ceiling caps how far the file can be raised.
        if (const auto truePeak = juce::jmax(record.overallTruePeak, peak); truePeak > 0.0) {
            gainDecibels = juce::jmin(
                gainDecibels, options.truePeakCeilingDbtp - juce::Decibels::gainToDecibels(truePeak, decibelFloor)
            );
        }
    }

    if (!std::isfinite(gainDecibels) || gainDecibels > AudioNormalizationOptions::maximumGainDb) {
        return std::unexpected(
            utils::format(
                "Reaching the target would need more than {:.0f} dB of gain", AudioNormalizationOptions::maximumGainDb
            )
        );
    }

    return static_cast<float>(juce::Decibels::decibelsToGain(gainDecibels, decibelFloor));
}

/// Returns the AIFF bit depth for output written from the given source.
/// Keeps the analyzed source depth when the AIFF writer supports it,
/// uses 16 bits for MP3 sources, and falls back to the highest supported depth,
/// so for example 32-bit float sources are written as 24-bit.
int getNormalizationOutputBitDepth(const AudioAnalysisRecord& record, const juce::AudioFormatReader& reader)
{
    auto* writerFormat = utils::findAiffFormat(utils::threadLocalFormatManager());
    const auto preferredBitDepth = resolvePreferredOutputBitDepth(record, reader, record.file);
    return writerFormat != nullptr ? resolveWriterBitDepth(*writerFormat, preferredBitDepth) : preferredBitDepth;
}

/// Returns the user-facing error message for a record that lost a batch output conflict.
juce::String batchOutputConflictMessage(const juce::File& sourceFile)
{
    return utils::format(
        "Another selected file is also written to {}", getNormalizationOutputFile(sourceFile).getFileName()
    );
}

/// Rewrites a file to the target level in the options, then re-analyzes the result.
/// Fails without writing anything when the output path already exists and is not the source,
/// unless options.overwriteExisting is set, in which case the existing output is disposed of.
/// An `.aif` source rewritten in place first has a copy of it disposed of,
/// and a converted source is disposed of after the output is in place, both through options.disposeFile.
/// A call cancelled before its output is committed returns a failure result and leaves the original file untouched.
/// When the file changed on disk since sourceRecord was analyzed, it is analyzed again before computing the gain.
/// Isolated source blocks that fail to decode are written as decoded and reported in the result notice.
/// Progress of the whole file, through any source analysis, the render, and the output analysis,
/// is reported to onProgress.
AudioNormalizationResult normalizeFile(
    const AudioAnalysisRecord& sourceRecord,
    const AudioNormalizationOptions& options,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    const auto startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    const auto& file = sourceRecord.file;
    const auto outputFile = getNormalizationOutputFile(file);

    if (!file.existsAsFile()) {
        return failNormalization(file, "File does not exist");
    }

    if (const auto outputCheck = utils::checkOutputPath(file, outputFile, options.overwriteExisting); !outputCheck) {
        return failNormalization(file, outputCheck.error());
    }

    if (!sourceRecord.isReady()) {
        return failNormalization(file, "File analysis must finish before normalization");
    }

    // The gain comes from the analyzed peak, so a file that changed since its analysis is measured again first.
    const bool sourceChanged = !sourceRecord.matchesFileOnDisk();
    const auto renderProgressStart = sourceChanged ? sourceAnalysisProgressShare : 0.0f;
    const auto record = sourceChanged
        ? analyzeFile(file, isCancelled, utils::makeProgressRange(onProgress, 0.0f, renderProgressStart))
        : sourceRecord;

    if (record.hasError()) {
        return failNormalization(file, record.errorMessage);
    }

    auto& formatManager = utils::threadLocalFormatManager();
    auto* writerFormat = utils::findAiffFormat(formatManager);

    if (writerFormat == nullptr) {
        return failNormalization(file, "Unsupported audio format");
    }

    const auto reader = utils::createAudioReader(file);

    if (reader == nullptr) {
        return failNormalization(file, "Unsupported or unreadable audio file");
    }

    const auto normalizationGain = getNormalizationGain(record, options);

    if (!normalizationGain) {
        return failNormalization(file, normalizationGain.error());
    }

    const auto gain = *normalizationGain;

    if (juce::approximatelyEqual(gain, 1.0f) && outputFile == file) {
        auto analysis = analyzeFile(file, isCancelled, utils::makeProgressRange(onProgress, renderProgressStart, 1.0f));

        if (analysis.hasError()) {
            return AudioNormalizationResult::failure(file, analysis.errorMessage);
        }

        utils::logInfo(
            "Normalized: {:.2f} s, {} (already at target)",
            utils::elapsedSecondsSince(startedAtMilliseconds),
            file.getFileName().quoted()
        );
        return AudioNormalizationResult::forFile(
            file, OperationSucceeded {.output = std::move(analysis), .notice = {}}
        );
    }

    // The hidden name keeps a half-written output out of folder scans,
    // and out of later scans after a crash.
    const juce::TemporaryFile temporaryFile(outputFile, juce::TemporaryFile::useHiddenFile);
    std::unique_ptr<juce::OutputStream> outputStream = temporaryFile.getFile().createOutputStream();

    if (outputStream == nullptr) {
        return failNormalization(file, "Could not create temporary output file");
    }

    // The AIFF writer accepts any sample rate, and keeping the source rate avoids a pitch change,
    // since normalization does not resample.
    const auto writerSampleRate = reader->sampleRate;
    const auto writerBitDepth = getNormalizationOutputBitDepth(record, *reader);

    // Tags and pictures are copied separately through TagLib after the audio is rendered,
    // and the reader metadata passed here only covers what the JUCE AIFF writer itself stores.
    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(writerSampleRate)
                                   .withNumChannels(static_cast<int>(reader->numChannels))
                                   .withBitsPerSample(writerBitDepth)
                                   .withMetadataValues(utils::aiffWriterMetadata(reader->metadataValues));

    auto writer = writerFormat->createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return failNormalization(file, getNormalizationSupportMessage(file));
    }

    auto decodeNotice = renderNormalizedAudio(
        *reader,
        *writer,
        gain,
        isCancelled,
        utils::makeProgressRange(onProgress, renderProgressStart, renderProgressEnd)
    );

    if (!decodeNotice) {
        return failNormalization(file, decodeNotice.error());
    }

    writer.reset();

    if (!copyMetadata(file, temporaryFile.getFile())) {
        return failNormalization(file, "Could not preserve metadata while writing normalized audio");
    }

    if (const auto validation = utils::validateWrittenAudio(
            formatManager,
            temporaryFile.getFile(),
            static_cast<int>(reader->numChannels),
            reader->sampleRate,
            reader->lengthInSamples
        );
        !validation)
    {
        return failNormalization(
            file,
            utils::format(
                "Normalized output failed validation: {}. The original file was left unchanged.", validation.error()
            )
        );
    }

    // Check again before touching the original, since cancellation may arrive during the last block.
    // The coordinator drops results of a cancelled run, so committing here would change files on disk
    // without the caller ever learning about it.
    if (utils::isCancellationRequested(isCancelled)) {
        return failNormalization(file, cancelledMessage);
    }

    if (const auto commit = commitNormalizedOutput(temporaryFile, file, outputFile, options); !commit) {
        return failNormalization(file, commit.error());
    }

    auto analysis = analyzeFile(outputFile, {}, utils::makeProgressRange(onProgress, renderProgressEnd, 1.0f));
    auto notice = std::move(*decodeNotice);

    if (notice.isNotEmpty()) {
        utils::logWarn("Normalization warning: {}, {}", outputFile.getFullPathName().quoted(), notice);
    }

    if (analysis.hasError()) {
        auto error = utils::format("The file was normalized, but re-analysis failed: {}", analysis.errorMessage);
        utils::logError("Normalization failed for {}: {}", file.getFullPathName().quoted(), error);
        return AudioNormalizationResult::forFile(
            file, CommittedButUnanalyzed {.output = std::move(analysis), .error = std::move(error)}
        );
    }

    utils::logInfo(
        "Normalized: {:.2f} s, {}", utils::elapsedSecondsSince(startedAtMilliseconds), outputFile.getFileName().quoted()
    );
    return AudioNormalizationResult::forFile(
        file, OperationSucceeded {.output = std::move(analysis), .notice = std::move(notice)}
    );
}
}  // namespace audiobatch
