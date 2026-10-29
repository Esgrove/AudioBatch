/// Implementation of the helpers that build the chains and processing options of the test plugin tests.

#include "ProcessingTestPlugins.h"

#include "plugins/PluginProcessing.h"

#include <cstddef>
#include <vector>

namespace audiobatch::tests
{
/// Returns a chain holding only the given plugin.
/// The services take the chain as a std::span, which a braced list cannot convert to.
std::vector<juce::AudioPluginInstance*> chainOf(juce::AudioPluginInstance& plugin)
{
    return {&plugin};
}

/// Builds options for the given chain that delete replaced files instead of moving them to the system trash.
/// Each plugin gets the saved state at its index in `states`, or no saved state.
PluginProcessingOptions makeTestPluginOptions(
    const std::vector<juce::AudioPluginInstance*>& chain,
    const std::vector<juce::MemoryBlock>& states
)
{
    PluginProcessingOptions options;
    options.disposeFile = [](const juce::File& file) { return file.deleteFile(); };

    for (std::size_t index = 0; index < chain.size(); ++index) {
        PluginDescriptorRef descriptor;
        descriptor.identifierString = "test-plugin";
        descriptor.name = chain[index]->getName();

        if (index < states.size()) {
            descriptor.state = states[index];
        }

        options.plugins.push_back(descriptor);
    }

    return options;
}
}  // namespace audiobatch::tests
