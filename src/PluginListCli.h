/// Plugin listing for the CLI executable.
/// Declares PluginListGrouping, the supported listing layouts,
/// and PluginListCli, which prints the known-plugins list shared with the GUI,
/// optionally after rescanning the plugin folders with the out-of-process scanner.

#pragma once

#include <JuceHeader.h>

#include <optional>

/// How the plugin listing is grouped.
enum class PluginListGrouping {
    /// One alphabetical table of all plugins.
    name,
    /// One section per manufacturer.
    manufacturer,
    /// One section per processing type, for example Dynamics, EQ, Reverb, or Instrument.
    type,
    /// One section per plugin format, for example VST3 or AU.
    format,
};

/// Command-line front end for listing the available audio plugins.
class PluginListCli
{
public:
    /// Parses a --group-by value. Returns an empty optional for an unknown value.
    [[nodiscard]] static std::optional<PluginListGrouping> parseGrouping(const juce::String& value);

    /// Prints the known plugins, grouped as requested, and returns the process exit code.
    /// With rescan set, scans the plugin folders for new or changed plugins first
    /// and saves the updated list for the GUI too.
    /// Each plugin is loaded in a worker process launched from this executable,
    /// so a plugin that crashes is blacklisted instead of ending the scan.
    static int run(bool rescan, PluginListGrouping grouping);
};
