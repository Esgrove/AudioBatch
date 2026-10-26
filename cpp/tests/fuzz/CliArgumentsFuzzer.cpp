/// libFuzzer harness for the CLI argument parser.
/// The input is split into lines, which are parsed as the arguments that follow the executable name.
/// The parser must return a command or an error message for any arguments, without crashing.

#include "cli/AudioAnalysisCli.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>

namespace audiobatch
{
namespace
{
/// Longest input that is parsed, since longer command lines exercise no new code paths.
constexpr std::size_t maxInputBytes {4096};

/// Parses the lines of one fuzz input as command line arguments.
void fuzzArguments(const std::span<const std::uint8_t> input)
{
    if (input.size() > maxInputBytes) {
        return;
    }
    const auto text = juce::String::createStringFromData(input.data(), static_cast<int>(input.size()));
    std::ignore = cli::parseArguments(juce::ArgumentList("audiobatch", juce::StringArray::fromLines(text)));
}
}  // namespace
}  // namespace audiobatch

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

/// libFuzzer entry point, called once for each generated input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    audiobatch::fuzzArguments({data, size});
    return 0;
}
