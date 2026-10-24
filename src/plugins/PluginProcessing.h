/// Offline rendering of audio files through a plugin chain.
/// Declares the free functions that process one file at a time through a caller-owned sequence of plugin instances
/// and write the result as 16-bit PCM `<name>.aif` next to the original file.
/// Plugins handle dithering and output level. No dither or post-chain gain adjustment is added.
/// Also defines the shared data types of the feature:
/// PluginDescriptorRef for identifying a plugin and carrying its saved state,
/// PluginChainSlot for one entry in the chain,
/// PluginProcessingOptions for configuring a batch run,
/// and PluginProcessingResult for reporting the outcome of processing one file.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "normalization/AudioNormalization.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <optional>
#include <span>
#include <vector>

namespace audiobatch
{
int processBlockThroughChain(
    juce::AudioBuffer<float>& buffer,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    int numChannels,
    juce::MidiBuffer& midi,
    std::span<juce::AudioBuffer<float>> preparedViews = {}
);

[[nodiscard]] bool isBlockFinite(const juce::AudioBuffer<float>& block, int numSamples);

/// Lightweight reference describing which plugin to instantiate and its saved state.
struct PluginDescriptorRef {
    /// "VST3", "AudioUnit", etc.
    juce::String pluginFormatName;
    /// juce::PluginDescription::createIdentifierString()
    juce::String identifierString;
    /// Display name of the plugin.
    juce::String name;
    /// Name of the plugin vendor.
    juce::String manufacturer;
    /// Result of AudioProcessor::getStateInformation()
    juce::MemoryBlock state;

    /// True when this descriptor identifies a plugin.
    [[nodiscard]] bool isValid() const noexcept
    {
        return identifierString.isNotEmpty();
    }
};

/// One entry in the plugin processing chain.
struct PluginChainSlot {
    PluginDescriptorRef plugin;
    bool enabled {true};
};

/// Options controlling a batch plugin processing run.
struct PluginProcessingOptions {
    /// Enabled plugins in chain order.
    /// Disabled slots are filtered out before a run starts,
    /// so entries here align index-for-index with the instantiated chain each worker holds.
    std::vector<PluginDescriptorRef> plugins;
    /// Normalization target applied before the first plugin to files without a custom gain,
    /// or no normalization when empty.
    /// Uses the same peak or loudness gain as normalizing the file, including the true peak ceiling.
    std::optional<AudioNormalizationOptions> normalizeBeforePlugin;
    /// Replace an existing output file that is not the source itself.
    /// When false, a file whose output path is already taken fails instead of overwriting it.
    bool overwriteExisting {false};
    /// Disposes of the files a processed output replaces:
    /// a converted source, an overwritten output, and a copy of an `.aif` source rewritten in place.
    /// Moves them to the system trash, unless a caller such as a test passes another function.
    utils::FileDisposer disposeFile {utils::moveToTrash};
};

/// Result payload for a single plugin-processing operation.
struct PluginProcessingResult {
    /// Source file path before processing.
    juce::File originalFile;
    /// Absolute path of the source file, which identifies its table row.
    juce::String originalFullPath;
    /// Final on-disk file after processing (.aif).
    juce::File outputFile;
    /// Name of the source file.
    juce::String fileName;
    /// Empty on success, otherwise the reason processing failed.
    juce::String errorMessage;
    /// Information about a successful run worth showing to the user,
    /// such as output clipping or source blocks that failed to decode.
    juce::String notice;
    /// Analysis of the output file, valid when the re-analysis succeeded.
    AudioAnalysisRecord analysisRecord;
    /// True when the plugin chain rendered the output file.
    bool succeeded {false};
    /// True once the output was moved into place, which can happen even when re-analysis then failed.
    /// A converted source is in the trash by then, so callers move the record to the output either way.
    bool committed {false};

    /// True when processing failed.
    [[nodiscard]] bool hasError() const noexcept
    {
        return !succeeded;
    }

    /// Creates a failure result for the given file and message.
    static PluginProcessingResult failure(const juce::File& file, const juce::String& message)
    {
        PluginProcessingResult result;
        result.originalFile = file;
        result.originalFullPath = file.getFullPathName();
        result.outputFile = file;
        result.fileName = file.getFileName();
        result.errorMessage = message;
        return result;
    }
};

bool canProcessFileWithPlugins(const juce::File& file);

juce::File getProcessingOutputFile(const juce::File& input);

PluginProcessingResult processFileWithPlugins(
    const AudioAnalysisRecord& sourceRecord,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const utils::CancellationCheck& isCancelled = {},
    const utils::ProgressCallback& onProgress = {}
);
}  // namespace audiobatch
