/// Main-process plugin scanning through an isolated child process.
/// OutOfProcessPluginScanner owns the scan worker and reports crashes, cancellation, and per-plugin timeouts.

#pragma once

#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>

namespace audiobatch
{
class PluginScanWorkerProcess;

/// Main-process side of out-of-process plugin scanning.
/// Install it with KnownPluginList::setCustomScanner.
/// Each plugin file is sent to a worker process launched from this executable,
/// and the worker is relaunched after a crash so the scan can continue with the next plugin.
class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    /// Called before the first plugin of a scan is loaded, with a check that turns true once the scan is abandoned.
    /// Returning false skips every plugin of that scan without blacklisting any of them.
    using ScanStartingCallback = std::function<bool(const utils::CancellationCheck& shouldStop)>;

    /// Called once a scan has finished, including scans that were skipped or cancelled,
    /// with true only for a scan that ran to its end.
    using ScanFinishedCallback = std::function<void(bool completed)>;

    explicit OutOfProcessPluginScanner(
        ScanStartingCallback scanStartingCallback = nullptr,
        ScanFinishedCallback scanFinishedCallback = nullptr
    );

    ~OutOfProcessPluginScanner() override;

    OutOfProcessPluginScanner(const OutOfProcessPluginScanner&) = delete;
    OutOfProcessPluginScanner& operator=(const OutOfProcessPluginScanner&) = delete;
    OutOfProcessPluginScanner(OutOfProcessPluginScanner&&) = delete;
    OutOfProcessPluginScanner& operator=(OutOfProcessPluginScanner&&) = delete;

    /// Returns the plugin files that took longer than the timeout to scan and were skipped,
    /// since the scanner was created.
    [[nodiscard]] const juce::StringArray& getTimedOutFiles() const noexcept
    {
        return timedOutFiles;
    }

    // juce::KnownPluginList::CustomScanner
    bool findPluginTypesFor(
        juce::AudioPluginFormat& format,
        juce::OwnedArray<juce::PluginDescription>& result,
        const juce::String& fileOrIdentifier
    ) override;

    void scanFinished() override;

private:
    /// The worker process that loads the plugins, launched when a plugin needs one,
    /// and dropped when a plugin crashes it or hangs, or the scan is cancelled.
    /// Keep the existing teardown order: the worker is destroyed after the scan callbacks and state.
    std::unique_ptr<PluginScanWorkerProcess> worker;
    /// The plugin files that took longer than the timeout and were skipped.
    juce::StringArray timedOutFiles;
    /// Called before the first plugin of a scan, which can abandon the scan.
    ScanStartingCallback onScanStarting;
    /// Called when the scan ends, with whether it ran to its end.
    ScanFinishedCallback onScanFinished;
    /// True when the starting callback abandoned the scan.
    bool scanSkipped {false};
    /// True once the first plugin of the scan was reached.
    bool scanStarted {false};

    JUCE_LEAK_DETECTOR(OutOfProcessPluginScanner)
};
}  // namespace audiobatch
