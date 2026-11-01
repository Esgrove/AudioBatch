/// Implementation of the normalization functions.
/// Reads source files through per-thread JUCE format readers, applies the peak or loudness gain in chunks,
/// and writes AIFF while measuring the exact quantized output.
/// Carries source tags and embedded art across through Metadata handling.
/// The format support reporting lives in NormalizationFormatSupport.cpp.

#include "normalization/AudioNormalization.h"

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/AudioMeasurement.h"
#include "analysis/DecodeFailureTracker.h"
#include "metadata/AudioMetadata.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <unordered_set>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
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
/// Progress reserved for rendering and metering before metadata validation and commit.
constexpr float renderProgressEnd {0.95f};

/// Exact output measurements and the notice from tolerated source decode failures.
struct NormalizedRender {
    /// Measurements of the quantized AIFF samples.
    AudioAnalysisRecord measurements;
    /// Notice about tolerated read failures.
    juce::String decodeNotice;
};

/// Prepared destination and source metadata used while assigning deterministic output ownership.
struct OutputCandidate {
    /// Non-owning record from the caller's vector, valid for the duration of conflict detection.
    const AudioAnalysisRecord* record {};
    /// Destination the normalized file is written to.
    juce::File output;
    /// True when the destination is the source file itself.
    bool inPlace {};
};

/// Steps of the output bit depth that a full-scale sample may move by while a file still counts as at its target.
/// A 16-bit file whose peak is the largest positive sample, 32767 of 32768,
/// needs a gain one step above unity to reach 0 dBFS, and rewriting it would produce the same samples again.
constexpr double atTargetToleranceSteps {1.5};

/// Returns true when the gain is so close to unity
/// that rewriting a file of the given output bit depth would change no sample by more than about one step,
/// so an in-place file is left as it is.
bool isAtTarget(const LinearGain gain, const int outputBitDepth)
{
    const auto fullScaleSteps = std::ldexp(1.0, juce::jlimit(1, 32, outputBitDepth) - 1);
    return std::abs(static_cast<double>(gain.value) - 1.0) <= atTargetToleranceSteps / fullScaleSteps;
}

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
    const utils::OutputTemporaryFile& temporaryFile,
    const juce::File& file,
    const juce::File& outputFile,
    const utils::FileIdentity& expectedSource,
    const AudioNormalizationOptions& options
)
{
    if (!expectedSource.matches(file)) {
        return std::unexpected(
            juce::String(
                "The source file changed while audio was being rendered. The original file was left unchanged."
            )
        );
    }
    if (outputFile == file && !utils::disposeOfCopy(file, options.disposeFile)) {
        return std::unexpected(juce::String("Could not move a copy of the original file to the system trash"));
    }

    return utils::commitTemporaryOutput(
        temporaryFile, file, outputFile, options.overwriteExisting, expectedSource, options.disposeFile
    );
}

/// Picks a bit depth the writer format supports, preferring the requested depth.
/// Falls back to the format's highest supported depth to retain as much precision as the writer allows.
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
/// Returns the output measurements and the tracker's notice about tolerated failures,
/// or a user-facing error message, including when cancellation is requested between blocks.
/// The written fraction of the file is reported to onProgress after each block.
std::expected<NormalizedRender, juce::String> renderNormalizedAudio(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const float gain,
    const int bitsPerSample,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    juce::AudioBuffer<float> buffer(static_cast<int>(reader.numChannels), normalizationBlockSize);
    juce::AudioBuffer<int> pcm(buffer.getNumChannels(), normalizationBlockSize);
    std::vector<float*> channelPointers(reader.numChannels);
    std::vector<const int*> outputChannels(static_cast<std::size_t>(buffer.getNumChannels()) + 1, nullptr);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        channelPointers[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);
        outputChannels[static_cast<std::size_t>(channel)] = pcm.getReadPointer(channel);
    }
    AudioMeasurement measurement(buffer.getNumChannels(), reader.sampleRate, reader.lengthInSamples);
    if (const auto initialized = measurement.checkInitialization(); !initialized) {
        return std::unexpected(initialized.error());
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

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
            const auto frames = static_cast<std::size_t>(samplesThisBlock);
            auto* samples = channelPointers[static_cast<std::size_t>(channel)];
            if (const auto quantized = utils::quantizeNormalizedSamples(
                    std::span<const float>(samples, frames),
                    std::span<int>(pcm.getWritePointer(channel), frames),
                    std::span<float>(samples, frames),
                    gain,
                    bitsPerSample
                );
                !quantized)
            {
                return std::unexpected(quantized.error());
            }
        }
        if (!writer.write(outputChannels.data(), samplesThisBlock)) {
            return std::unexpected(juce::String("Failed while writing normalized audio data"));
        }
        if (const auto measured = measurement.push(buffer, 0, samplesThisBlock); !measured) {
            return std::unexpected(measured.error());
        }

        utils::reportProgress(
            onProgress, utils::progressFraction(samplePosition + samplesThisBlock, reader.lengthInSamples)
        );
    }

    auto measured = measurement.finish();
    if (!measured) {
        return std::unexpected(measured.error());
    }
    return NormalizedRender {.measurements = std::move(*measured), .decodeNotice = failureTracker.getNotice()};
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
    auto ordered = records | std::views::transform([](const AudioAnalysisRecord& record) {
                       auto output = getNormalizationOutputFile(record.file);
                       const auto inPlace = output == record.file;
                       return OutputCandidate {.record = &record, .output = std::move(output), .inPlace = inPlace};
                   })
        | std::ranges::to<std::vector>();

    // In-place sources claim their output first, then the rest in path order.
    std::ranges::sort(ordered, [](const OutputCandidate& lhs, const OutputCandidate& rhs) {
        if (lhs.inPlace != rhs.inPlace) {
            return lhs.inPlace;
        }

        return lhs.record->file.getFullPathName() < rhs.record->file.getFullPathName();
    });

    std::unordered_set<juce::String> claimedOutputs;
    juce::StringArray conflicts;

    std::unordered_set<juce::String> visitedSources;
    for (const auto& candidate : ordered) {
        const auto& sourcePath = candidate.record->file.getFullPathName();
        if (!visitedSources.insert(sourcePath).second) {
            continue;
        }
        const auto outputPath = utils::resolvedOutputKey(candidate.output);

        if (!claimedOutputs.insert(outputPath).second) {
            conflicts.add(sourcePath);
        }
    }

    return conflicts;
}

/// Returns the linear gain that brings the analyzed file to the target in the options,
/// or the user-facing reason when the file has no measurable level for that mode.
/// A gain above AudioNormalizationOptions::maximumGainDb, or one that is not finite, is refused instead of capped,
/// since a capped gain would leave the file short of the target without telling the caller.
std::expected<LinearGain, juce::String> getNormalizationGain(
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

    return LinearGain {static_cast<float>(juce::Decibels::decibelsToGain(gainDecibels, decibelFloor))};
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

/// Rewrites a file to the target level in the options and measures its quantized PCM while writing.
/// An in-place file already at its target reuses its source analysis instead of rewriting.
/// Fails without writing anything when the output path already exists and is not the source,
/// unless options.overwriteExisting is set, in which case the existing output is disposed of.
/// An `.aif` source rewritten in place first has a copy of it disposed of,
/// and a converted source is disposed of after the output is in place, both through options.disposeFile.
/// A call cancelled before its output is committed returns a failure result and leaves the original file untouched.
/// When the file changed on disk since sourceRecord was analyzed, it is analyzed again before computing the gain.
/// Tolerated read failures retain any samples the decoder supplied, with unwritten samples left silent,
/// and are reported in the result notice.
/// Progress through any source analysis, rendering and metering, and finalization is reported to onProgress.
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
    const utils::FileIdentity expectedSource(file);
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

    auto reader = utils::createAudioReader(file);

    if (reader == nullptr) {
        return failNormalization(file, "Unsupported or unreadable audio file");
    }

    const auto normalizationGain = getNormalizationGain(record, options);

    if (!normalizationGain) {
        return failNormalization(file, normalizationGain.error());
    }

    const auto gain = normalizationGain->value;

    if (outputFile == file && isAtTarget(*normalizationGain, getNormalizationOutputBitDepth(record, *reader))) {
        if (utils::isCancellationRequested(isCancelled)) {
            return failNormalization(file, cancelledMessage);
        }
        if (!expectedSource.matches(file)) {
            return failNormalization(file, "The source file changed while normalization was being checked.");
        }
        auto analysis = record;
        analysis.customGain.reset();
        utils::reportProgress(onProgress, 1.0f);

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
    const utils::OutputTemporaryFile temporaryFile(outputFile);
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

    auto rendered = renderNormalizedAudio(
        *reader,
        *writer,
        gain,
        writerBitDepth,
        isCancelled,
        utils::makeProgressRange(onProgress, renderProgressStart, renderProgressEnd)
    );

    if (!rendered) {
        return failNormalization(file, rendered.error());
    }

    writer.reset();

    if (!expectedSource.matches(file)) {
        return failNormalization(
            file, "The source file changed while audio was being rendered. The original file was left unchanged."
        );
    }
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

    reader.reset();
    const utils::FileIdentity renderedIdentity(temporaryFile.getFile());
    if (const auto commit = commitNormalizedOutput(temporaryFile, file, outputFile, expectedSource, options); !commit) {
        return failNormalization(file, commit.error());
    }

    auto analysis
        = completeRenderedAnalysis(std::move(rendered->measurements), outputFile, writerBitDepth, renderedIdentity);
    auto notice = std::move(rendered->decodeNotice);
    utils::reportProgress(onProgress, 1.0f);

    if (notice.isNotEmpty()) {
        utils::logWarn("Normalization warning: {}, {}", outputFile.getFullPathName().quoted(), notice);
    }

    if (analysis.hasError()) {
        auto error = utils::format("The file was normalized, but output analysis failed: {}", analysis.errorMessage);
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
