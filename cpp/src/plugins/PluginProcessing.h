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
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch
{
/// Lightweight reference describing which plugin to instantiate and its saved state.
struct PluginDescriptorRef {
    /// True when this descriptor identifies a plugin.
    [[nodiscard]] bool isValid() const noexcept
    {
        return identifierString.isNotEmpty();
    }

    /// Result of AudioProcessor::getStateInformation()
    juce::MemoryBlock state;
    /// juce::PluginDescription::createIdentifierString()
    juce::String identifierString;
    /// Name of the plugin vendor.
    juce::String manufacturer;
    /// Display name of the plugin.
    juce::String name;
    /// "VST3", "AudioUnit", etc.
    juce::String pluginFormatName;
};

/// One entry in the plugin processing chain.
struct PluginChainSlot {
    // Preserve the enabled/descriptor aggregate order shared by saved-chain producers.
    /// True when the slot takes part in processing.
    bool enabled {true};
    /// The plugin of the slot and its saved state.
    PluginDescriptorRef plugin;
};

/// Options controlling a batch plugin processing run.
struct PluginProcessingOptions {
    // Preserve the designated option-construction order shared by GUI and test callers.
    /// Replace an existing output file that is not the source itself.
    /// When false, a file whose output path is already taken fails instead of overwriting it.
    bool overwriteExisting {false};
    /// Disposes of the files a processed output replaces:
    /// a converted source, an overwritten output, and a copy of an `.aif` source rewritten in place.
    /// Moves them to the system trash, unless a caller such as a test passes another function.
    utils::FileDisposer disposeFile {utils::trashFile};
    /// Normalization target applied before the first plugin to files without a custom gain,
    /// or no normalization when empty.
    /// Uses the same peak or loudness gain as normalizing the file, including the true peak ceiling.
    std::optional<AudioNormalizationOptions> normalizeBeforePlugin;
    /// Enabled plugins in chain order.
    /// Disabled slots are filtered out before a run starts,
    /// so entries here align index-for-index with the instantiated chain each worker holds.
    std::vector<PluginDescriptorRef> plugins;
};

/// Result payload for a single plugin-processing operation.
struct PluginProcessingResult {
    /// Creates the result of a file the user cancelled before it was changed.
    static PluginProcessingResult cancelled(const juce::File& file)
    {
        return forFile(file, OperationFailed {.error = "Processing was cancelled", .cancelled = true});
    }

    /// Creates a failure result for the given file and message.
    static PluginProcessingResult failure(const juce::File& file, const juce::String& message)
    {
        return forFile(file, OperationFailed {.error = message, .cancelled = false});
    }

    /// Creates the result of an operation on the given source file.
    static PluginProcessingResult forFile(const juce::File& file, OperationOutcome outcome)
    {
        PluginProcessingResult result;
        result.originalFile = file;
        result.originalFullPath = file.getFullPathName();
        result.fileName = file.getFileName();
        result.outcome = std::move(outcome);
        return result;
    }

    /// Returns true unless the output was written and analyzed.
    [[nodiscard]] bool hasError() const noexcept
    {
        return !std::holds_alternative<OperationSucceeded>(outcome);
    }

    /// What the operation did with the file.
    OperationOutcome outcome;
    /// Source file path before processing.
    juce::File originalFile;
    /// Name of the source file.
    juce::String fileName;
    /// Absolute path of the source file, which identifies its table row.
    juce::String originalFullPath;
};

juce::File getProcessingOutputFile(const juce::File& input);
[[nodiscard]] juce::String describeFailedProcessCall(const juce::AudioPluginInstance& plugin);

PluginProcessingResult processFileWithPlugins(
    const AudioAnalysisRecord& sourceRecord,
    const PluginProcessingOptions& options,
    std::span<juce::AudioPluginInstance* const> chainInstances,
    const utils::CancellationCheck& isCancelled = {},
    const utils::ProgressCallback& onProgress = {}
);

bool canProcessFileWithPlugins(const juce::File& file);
[[nodiscard]] bool isBlockFinite(const juce::AudioBuffer<float>& block, int numSamples);
[[nodiscard]] int findFailedProcessCall(std::span<juce::AudioPluginInstance* const> chainInstances) noexcept;
int processBlockThroughChain(
    juce::AudioBuffer<float>& buffer,
    std::span<juce::AudioPluginInstance* const> chainInstances,
    int numChannels,
    juce::MidiBuffer& midi,
    std::span<juce::AudioBuffer<float>> preparedViews = {}
);

void foldStereoToMono(juce::AudioBuffer<float>& buffer, int firstSample, int numSamples);
}  // namespace audiobatch
