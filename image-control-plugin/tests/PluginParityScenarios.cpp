/// PluginParityScenarios defines independent semantic parameter mappings and the ordered parity workload catalogue.

#include "tests/PluginParityScenarios.h"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Global semantic indices follow the independent crossover and per-band control specification.
constexpr int OUTPUT_GAIN {27};
constexpr int BYPASS {28};
constexpr int DELTA {29};
constexpr int MONO {30};

/// Returns the semantic band field index without using either plugin's exposed host indices.
constexpr int bandParameter(const int band, const int field)
{
    return 3 + band * 6 + field;
}

/// Specifies matching per-band controls within the explicitly reported shared plain-unit domain.
std::vector<Setting> allBands(const int field, const double value)
{
    std::vector<Setting> result;
    result.reserve(4);
    for (int band = 0; band < 4; ++band) {
        result.push_back({.parameter = bandParameter(band, field), .value = value});
    }
    return result;
}

/// Appends related plain-unit controls to a scenario without changing their explicit values.
void append(std::vector<Setting>& destination, const std::vector<Setting>& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}
}  // namespace

/// Owns one case's explicit controls and events, leaving omitted controls at independently specified neutrality.
Scenario::Scenario(
    std::string caseName,
    std::vector<Setting> controls,
    std::vector<Event> automation,
    const Oracle expectation,
    const Signal source
) :
    name(std::move(caseName)),
    settings(std::move(controls)),
    events(std::move(automation)),
    oracle(expectation),
    signal(source)
{}

/// Defines all 31 semantic controls independently of production parameter registries.
std::vector<ParameterSpec> parameterSpecs()
{
    std::vector<ParameterSpec> result;
    result.reserve(PARAMETER_COUNT);
    constexpr std::array cppCrossovers {"Low Crossover", "Mid Crossover", "High Crossover"};
    constexpr std::array frequencies {120.0, 1000.0, 6000.0};
    for (int index = 0; index < 3; ++index) {
        result.push_back({
            .cppName = cppCrossovers.at(static_cast<std::size_t>(index)),
            .rustName = fmt::format("Crossover {}", index + 1),
            .kind = Kind::frequency,
            .neutral = frequencies.at(static_cast<std::size_t>(index)),
        });
    }
    constexpr std::array bands {"Low", "Low Mid", "High Mid", "High"};
    constexpr std::array fields {"Mid", "Side", "Width", "Recover", "Pan", "Solo"};
    constexpr std::array kinds {Kind::gain, Kind::gain, Kind::width, Kind::recover, Kind::pan, Kind::toggle};
    for (std::size_t band = 0; band < bands.size(); ++band) {
        for (std::size_t field = 0; field < fields.size(); ++field) {
            result.push_back({
                .cppName = fmt::format("{}: {}", bands[band], fields[field]),
                .rustName = fmt::format("Band {} {}", band + 1, fields[field]),
                .kind = kinds[field],
                .neutral = field == 2 ? 1.0 : 0.0,
            });
        }
    }
    result.push_back({.cppName = "Output Gain", .rustName = "Output Gain", .kind = Kind::gain, .neutral = 0.0});
    result.push_back({.cppName = "Bypass", .rustName = "Bypass", .kind = Kind::toggle, .neutral = 0.0});
    result.push_back({.cppName = "Delta", .rustName = "Delta", .kind = Kind::toggle, .neutral = 0.0});
    result.push_back({.cppName = "Mono", .rustName = "Mono", .kind = Kind::toggle, .neutral = 0.0});
    if (result.size() != PARAMETER_COUNT) {
        throw std::runtime_error("Internal semantic parameter count must be 31");
    }
    return result;
}

/// Generates both fast smoke cases and the extended fixed-control and automation contracts.
std::vector<Scenario> scenarios()
{
    std::vector<Scenario> result {
        {"neutral", {}, {}, Oracle::delayed},
        {
            "active",
            {
                {.parameter = 0, .value = 180.0},
                {.parameter = 1, .value = 1200.0},
                {.parameter = 2, .value = 6500.0},
                {.parameter = 3, .value = -3.0},
                {.parameter = 4, .value = 2.0},
                {.parameter = 11, .value = 0.4},
                {.parameter = 12, .value = 75.0},
                {.parameter = 19, .value = -45.0},
                {.parameter = 26, .value = 1.0},
                {.parameter = OUTPUT_GAIN, .value = -2.0},
            },
        },
        {
            "automation",
            {},
            {
                {.frame = 4096,
                 .settings
                 = {{.parameter = 5, .value = 0.25},
                    {.parameter = 6, .value = 100.0},
                    {.parameter = 19, .value = 35.0},
                    {.parameter = OUTPUT_GAIN, .value = -3.0},},},
                {.frame = 8192,
                 .settings
                 = {{.parameter = 0, .value = 240.0},
                    {.parameter = 1, .value = 1800.0},
                    {.parameter = 2, .value = 8000.0},
                    {.parameter = 20, .value = 1.0},
                    {.parameter = DELTA, .value = 1.0},},},
                {.frame = 12'288, .settings = {{.parameter = BYPASS, .value = 1.0}, {.parameter = MONO, .value = 1.0}}},
                {.frame = 16'384,
                 .settings
                 = {{.parameter = BYPASS, .value = 0.0},
                    {.parameter = DELTA, .value = 0.0},
                    {.parameter = MONO, .value = 0.0},
                    {.parameter = 5, .value = 1.0},
                    {.parameter = 20, .value = 0.0},},},
            },
        },
        {"output_gain", {{.parameter = OUTPUT_GAIN, .value = -6.0}}, {}, Oracle::gain},
        {"pan_left", allBands(4, -100.0), {}, Oracle::panLeft},
        {"pan_right", allBands(4, 100.0), {}, Oracle::panRight},
        {"final_mono", {{.parameter = MONO, .value = 1.0}}, {}, Oracle::mono},
        {"neutral_delta", {{.parameter = DELTA, .value = 1.0}}, {}, Oracle::silence},
        {"neutral_impulse", {}, {}, Oracle::delayed, Signal::impulse},
        {"neutral_silence", {}, {}, Oracle::delayed, Signal::silence},
        {"width_zero", allBands(2, 0.0)},
        {"width_double", allBands(2, 2.0)},
        {"recover_100", allBands(2, 0.25)},
        {"recover_150", allBands(2, 0.25)},
        {"recover_200", allBands(2, 0.25)},
        {"recovery_automation_200",
         allBands(2, 0.0),
         {{.frame = 4096, .settings = allBands(3, 100.0)},
          {.frame = 8192, .settings = allBands(3, RECOVER_COMMON_MAX_PERCENT)},
          {.frame = 12'288, .settings = allBands(3, 0.0)},},
         Oracle::none,
         Signal::antiPhase,},
        {"recover_anti_phase", allBands(2, 0.0), {}, Oracle::none, Signal::antiPhase},
        {"recover_inverted", allBands(2, 0.4), {}, Oracle::none, Signal::inverted},
        {"recover_swapped", allBands(2, 0.4), {}, Oracle::none, Signal::swapped},
        {"mid_off_recovery", allBands(0, -60.0)},
        {"side_off_recovery", allBands(1, -60.0)},
        {
            "solo_delta",
            {
                {.parameter = bandParameter(1, 5), .value = 1.0},
                {.parameter = bandParameter(2, 5), .value = 1.0},
                {.parameter = bandParameter(1, 2), .value = 0.5},
                {.parameter = bandParameter(1, 3), .value = 100.0},
                {.parameter = DELTA, .value = 1.0},
            },
        },
        {
            "bypass_active",
            {{.parameter = bandParameter(0, 2), .value = 0.0},
             {.parameter = bandParameter(0, 3), .value = 100.0},
             {.parameter = OUTPUT_GAIN, .value = -12.0},
             {.parameter = DELTA, .value = 1.0},
             {.parameter = BYPASS, .value = 1.0},},
            {},
            Oracle::delayed,
        },
        {"custom_crossovers",
         {{.parameter = 0, .value = 70.0},
          {.parameter = 1, .value = 850.0},
          {.parameter = 2, .value = 10'000.0},
          {.parameter = 5, .value = 0.2},
          {.parameter = 6, .value = 100.0},
          {.parameter = 19, .value = -35.0},},},
    };
    for (auto& scenario : result) {
        if (scenario.name.starts_with("recover_") || scenario.name.ends_with("_off_recovery")) {
            auto recovery = RECOVER_COMMON_MAX_PERCENT;
            if (scenario.name == "recover_100") {
                recovery = 100.0;
            } else if (scenario.name == "recover_150") {
                recovery = 150.0;
            }
            append(scenario.settings, allBands(3, recovery));
        }
        if (scenario.name.ends_with("_off_recovery")) {
            append(scenario.settings, allBands(2, 0.0));
        }
    }
    return result;
}
}  // namespace esgrove::image_control::test
