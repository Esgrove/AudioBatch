/// Child-process plugin scanning on the message thread.
/// PluginScanWorker receives scan requests from the main process and replies with plugin descriptions.

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <mutex>
#include <queue>

namespace audiobatch
{
/// Child side of out-of-process plugin scanning.
/// Receives requests on the connection thread and loads plugins on the message thread.
class PluginScanWorker final : juce::ChildProcessWorker, juce::AsyncUpdater
{
public:
    explicit PluginScanWorker(std::function<void()> connectionLostCallback);

    ~PluginScanWorker() override;

    PluginScanWorker(const PluginScanWorker&) = delete;
    PluginScanWorker& operator=(const PluginScanWorker&) = delete;
    PluginScanWorker(PluginScanWorker&&) = delete;
    PluginScanWorker& operator=(PluginScanWorker&&) = delete;

    bool initialiseFromCommandLine(const juce::String& commandLine);

private:
    [[nodiscard]] juce::OwnedArray<juce::PluginDescription> scan(const juce::MemoryBlock& request) const;

    void sendResults(const juce::OwnedArray<juce::PluginDescription>& results);

    // juce::ChildProcessWorker
    void handleConnectionLost() override;
    void handleMessageFromCoordinator(const juce::MemoryBlock& message) override;

    // juce::AsyncUpdater
    void handleAsyncUpdate() override;

    /// The formats the worker loads plugins with.
    juce::AudioPluginFormatManager formatManager;
    /// Called when the main process goes away, so the worker process can exit.
    std::function<void()> onConnectionLost;
    /// Guards the pending requests, which arrive on the connection thread and are scanned on the message thread.
    std::mutex pendingRequestsMutex;
    /// Scan requests waiting for the message thread.
    std::queue<juce::MemoryBlock> pendingRequests;

    JUCE_LEAK_DETECTOR(PluginScanWorker)
};
}  // namespace audiobatch
