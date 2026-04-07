#include "AudioAnalysisService.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace
{
constexpr float minimumDisplayDecibels = -100.0f;

float peakMagnitude(const float peak)
{
    return std::abs(peak);
}

float signedPeakFromExtrema(const float minimum, const float maximum)
{
    return peakMagnitude(minimum) > peakMagnitude(maximum) ? minimum : maximum;
}

float dominantPeak(const float firstPeak, const float secondPeak)
{
    return peakMagnitude(firstPeak) > peakMagnitude(secondPeak) ? firstPeak : secondPeak;
}

int compareAnalysisState(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.hasError() != rhs.hasError()) {
        return lhs.hasError() ? 1 : -1;
    }

    return 0;
}

int compareText(const juce::String& lhs, const juce::String& rhs)
{
    return lhs.compareNatural(rhs);
}
}  // namespace

juce::AudioFormatManager& AudioAnalysisService::getThreadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local const bool initialized = [] {
        formatManager.registerBasicFormats();
        return true;
    }();

    juce::ignoreUnused(initialized);
    return formatManager;
}

bool AudioAnalysisService::isSupportedAudioFile(const juce::File& file)
{
    if (!file.existsAsFile()) {
        return false;
    }

    auto& formatManager = getThreadLocalFormatManager();
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension.isNotEmpty() && formatManager.findFormatForFileExtension(extension) != nullptr;
}

juce::Array<juce::File> AudioAnalysisService::collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    bool recursive
)
{
    juce::Array<juce::File> files;
    std::set<juce::String> seenPaths;

    auto addFile = [&](const juce::File& file) {
        if (!isSupportedAudioFile(file)) {
            return;
        }

        auto normalizedPath = file.getFullPathName();
        if (seenPaths.insert(normalizedPath).second) {
            files.add(file);
        }
    };

    for (const auto& inputPath : inputPaths) {
        if (inputPath.isDirectory()) {
            juce::Array<juce::File> discoveredFiles;
            inputPath.findChildFiles(discoveredFiles, juce::File::findFiles, recursive, "*");

            for (const auto& file : discoveredFiles) {
                addFile(file);
            }
        } else {
            addFile(inputPath);
        }
    }

    std::sort(files.begin(), files.end(), [](const juce::File& lhs, const juce::File& rhs) {
        return lhs.getFullPathName().compareNatural(rhs.getFullPathName()) < 0;
    });

    return files;
}

AudioAnalysisRecord AudioAnalysisService::analyzeFile(const juce::File& file)
{
    auto record = AudioAnalysisRecord::fromFile(file);

    if (!file.existsAsFile()) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "File does not exist";
        return record;
    }

    auto& formatManager = getThreadLocalFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "Unsupported or unreadable audio file";
        return record;
    }

    float minLeft = 0.0f;
    float maxLeft = 0.0f;
    float minRight = 0.0f;
    float maxRight = 0.0f;

    reader->readMaxLevels(0, reader->lengthInSamples, minLeft, maxLeft, minRight, maxRight);

    const auto leftPeak = signedPeakFromExtrema(minLeft, maxLeft);
    const auto rightPeak = reader->numChannels > 1 ? signedPeakFromExtrema(minRight, maxRight) : leftPeak;

    record.formatName = reader->getFormatName();
    record.sampleRate = juce::roundToInt(reader->sampleRate);
    record.channels = static_cast<int>(reader->numChannels);
    record.bitsPerSample = reader->bitsPerSample;
    record.lengthInSamples = reader->lengthInSamples;
    record.durationSeconds
        = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
    record.peakLeft = leftPeak;
    record.peakRight = rightPeak;
    record.overallPeak = dominantPeak(leftPeak, rightPeak);
    record.status = AudioAnalysisStatus::analyzed;
    record.fromCache = false;
    return record;
}

juce::String AudioAnalysisService::formatPeakDisplay(float peak)
{
    peak = peakMagnitude(peak);

    if (peak <= 0.0f) {
        return "-INF dBFS";
    }

    const auto decibels = juce::Decibels::gainToDecibels(peak, minimumDisplayDecibels);
    return juce::String::formatted("%.2f dBFS", decibels);
}

juce::String AudioAnalysisService::formatStatus(const AudioAnalysisRecord& record)
{
    switch (record.status) {
        case AudioAnalysisStatus::cached:
            return "Cached";
        case AudioAnalysisStatus::analyzed:
            return "Analyzed";
        case AudioAnalysisStatus::failed:
            return record.errorMessage.isNotEmpty() ? "Failed" : "Failed";
        case AudioAnalysisStatus::pending:
        default:
            return "Pending";
    }
}

void AudioAnalysisService::sortRecords(
    std::vector<AudioAnalysisRecord>& records,
    AudioAnalysisSortMode sortMode,
    bool ascending
)
{
    std::sort(records.begin(), records.end(), [sortMode, ascending](const auto& lhs, const auto& rhs) {
        if (const auto stateComparison = compareAnalysisState(lhs, rhs); stateComparison != 0) {
            return stateComparison < 0;
        }

        auto comparePeaks = [ascending](float left, float right) {
            const auto leftMagnitude = peakMagnitude(left);
            const auto rightMagnitude = peakMagnitude(right);
            return ascending ? leftMagnitude < rightMagnitude : leftMagnitude > rightMagnitude;
        };

        auto compareStrings = [ascending](const juce::String& left, const juce::String& right) {
            return ascending ? compareText(left, right) < 0 : compareText(left, right) > 0;
        };

        switch (sortMode) {
            case AudioAnalysisSortMode::name:
                if (lhs.fileName != rhs.fileName) {
                    return compareStrings(lhs.fileName, rhs.fileName);
                }
                return compareStrings(lhs.fullPath, rhs.fullPath);

            case AudioAnalysisSortMode::path:
                if (lhs.fullPath != rhs.fullPath) {
                    return compareStrings(lhs.fullPath, rhs.fullPath);
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioAnalysisSortMode::peak:
            default:
                if (!juce::approximatelyEqual(peakMagnitude(lhs.overallPeak), peakMagnitude(rhs.overallPeak))) {
                    return comparePeaks(lhs.overallPeak, rhs.overallPeak);
                }
                return compareStrings(lhs.fileName, rhs.fileName);
        }
    });
}