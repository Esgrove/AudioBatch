/// Plugin listing for the CLI executable.
/// Declares PluginListGrouping, the supported listing layouts,
/// and PluginListCli, which prints the known-plugins list shared with the GUI,
/// optionally after rescanning the plugin folders with the out-of-process scanner.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <optional>

namespace audiobatch
{
/// How the plugin listing is grouped.
enum class PluginListGrouping : std::uint8_t {
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
    [[nodiscard]] static std::optional<PluginListGrouping> parseGrouping(const juce::String& value);

    static int run(bool rescan, PluginListGrouping grouping);
};
}  // namespace audiobatch
