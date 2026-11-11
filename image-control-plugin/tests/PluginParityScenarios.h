/// PluginParityScenarios declares the independent semantic controls and ordered workloads for the parity host.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esgrove::image_control::test
{
/// Both implementations map normalized automation onto the same Recover percentage range.
inline constexpr double RECOVER_COMMON_MAX_PERCENT {200.0};
/// Number of semantic sound controls, independent of either plugin's exposed host index order.
inline constexpr int PARAMETER_COUNT {31};

/// The independently specified shared plain-unit parameter domain.
enum class Kind : std::uint8_t { frequency, gain, width, recover, pan, toggle };
/// Independent expected output laws, deliberately separate from production DSP code.
enum class Oracle : std::uint8_t { none, delayed, silence, gain, panLeft, panRight, mono };
/// Input transforms cover polarity consistency and distinct channel ordering without assuming recovery symmetry.
enum class Signal : std::uint8_t { ordinary, inverted, swapped, antiPhase, impulse, silence };

/// One semantic parameter maps differing host-visible names without relying on either host index order.
struct ParameterSpec {
    std::string cppName;
    std::string rustName;
    Kind kind {Kind::gain};
    double neutral {0.0};
};
/// One plain-unit target is converted through the actual hosted controller's text interface.
struct Setting {
    int parameter {0};
    double value {0.0};
};
/// Block-sampled automation is delivered at identical absolute input-frame boundaries in both hosts.
struct Event {
    int frame {0};
    std::vector<Setting> settings;
};
/// A bounded workload includes an independent oracle where the fullband law is known analytically.
struct Scenario {
    Scenario(
        std::string caseName,
        std::vector<Setting> controls,
        std::vector<Event> automation = {},
        Oracle expectation = Oracle::none,
        Signal source = Signal::ordinary
    );

    std::string name;
    std::vector<Setting> settings;
    std::vector<Event> events;
    Oracle oracle {Oracle::none};
    Signal signal {Signal::ordinary};
};

std::vector<ParameterSpec> parameterSpecs();
std::vector<Scenario> scenarios();
}  // namespace esgrove::image_control::test
