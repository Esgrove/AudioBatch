#include "utils.h"

namespace utils
{

juce::StringArray system_info()
{
    auto compile_time = juce::Time::getCompilationDate();
    auto compile_time_in_utc = compile_time - juce::RelativeTime(compile_time.getUTCOffsetSeconds());
    return juce::StringArray {
        juce::String(version::VERSION_INFO),
        juce::SystemStats::getJUCEVersion() + " " + compile_time_in_utc.toString(true, true, false, true),
        "Branch:       " + juce::String(version::BRANCH),
        "OS:           " + juce::SystemStats::getOperatingSystemName()
            + (juce::SystemStats::isOperatingSystem64Bit() ? " 64 bit" : " 32 bit"),
        "Device:       " + juce::SystemStats::getDeviceDescription(),
        "Manufacturer: " + juce::SystemStats::getDeviceManufacturer(),
        "CPU model:    " + juce::SystemStats::getCpuModel(),
        "CPU cores:    " + juce::String(juce::SystemStats::getNumPhysicalCpus()) + "C/"
            + juce::String(juce::SystemStats::getNumCpus()) + "T",
        "Memory size:  " + juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB",
        "User region:  " + juce::SystemStats::getUserRegion(),
        "User lang:    " + juce::SystemStats::getUserLanguage(),
        "Display lang: " + juce::SystemStats::getDisplayLanguage()};
}

juce::String formatted_system_info()
{
    juce::String info {
        juce::JUCEApplication::getInstance()->getApplicationName() + " " + juce::String(version::VERSION_NUMBER)
        + juce::newLine};
    for (const auto& line : utils::system_info()) {
        for (auto tokens = juce::StringArray::fromTokens(line, " ", "\""); const auto& token : tokens) {
            if (token.containsNonWhitespaceChars()) {
                info += token + " ";
            }
        }
        info += juce::newLine;
    }
    return info;
}
}  // namespace utils
