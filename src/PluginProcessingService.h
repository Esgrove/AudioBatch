/// Offline rendering of audio files through a plugin chain.
/// Declares PluginProcessingService, a stateless service
/// that processes one file at a time through a caller-owned sequence of plugin instances
/// and writes the result as `<name>.aif` next to the original file.

#pragma once

#include "AudioAnalysisService.h"
#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

/// Stateless audio-file processing through a chain of plugin instances.
/// Output is always written as `<name>.aif` next to the original file, at the source bit depth up to 24 bits.
class PluginProcessingService
{
public:
    /// Returns true when the caller wants processing to stop as soon as possible.
    /// Polled from the processing thread, so it must be thread-safe and cheap.
    using CancellationCheck = std::function<bool()>;

    /// Returns true when the file's format can be read for processing.
    static bool canProcessFile(const juce::File& file);

    /// Returns the on-disk output path that processing would produce for the given input.
    static juce::File getProcessingOutputFile(const juce::File& input);

    /// Processes a single file through the given plugin instances in order.
    /// The instances must align index-for-index with options.plugins.
    /// The function restores state on each plugin, prepares it in offline mode, and resets it before processing,
    /// compensates for the chain's total reported latency,
    /// and releases the plugins' resources afterwards, also when processing fails.
    /// Intended to run on a worker thread.
    /// Plugin setup and release are marshalled to the message thread,
    /// because VST3 restricts bus arrangement, setupProcessing, setActive, and setState to the UI thread,
    /// so only processBlock runs on the calling thread.
    /// The message loop must keep running while a worker is inside this call,
    /// unless isCancelled starts returning true:
    /// a worker waiting for the message thread then gives up instead of blocking,
    /// so the message thread may wait for workers after requesting cancellation without deadlocking.
    /// A call cancelled before its output is committed returns a failure result and leaves the original file untouched.
    /// It may leave the chain prepared, in which case the plugin instances release their resources when destroyed.
    /// The caller owns the plugin instances and is responsible for thread-safety:
    /// a chain of instances must only be in use by one thread at a time.
    /// A mono file running through a stereo chain feeds both plugin inputs and is written back as mono,
    /// using the average of both output channels.
    /// The chain renders into an unquantized intermediate file first.
    /// When the rendered peak exceeds full scale, the whole output is turned down just enough to avoid clipping,
    /// which is reported in the result notice.
    /// Trailing silence from the tail allowance is trimmed, and 16-bit output is dithered.
    /// When the file changed on disk since sourceRecord was analyzed, it is analyzed again first.
    static PluginProcessingResult processFile(
        const AudioAnalysisRecord& sourceRecord,
        const PluginProcessingOptions& options,
        const std::vector<juce::AudioPluginInstance*>& chainInstances,
        const CancellationCheck& isCancelled = {}
    );

private:
    /// Returns a format manager private to the calling thread, with the basic formats registered.
    /// Thread-local storage lets worker threads read files concurrently without sharing a manager or locking.
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
