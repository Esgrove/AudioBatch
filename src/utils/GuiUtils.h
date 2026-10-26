/// Reusable GUI operations shared by application windows and components.
/// Declares foreground activation, read-only label styling, bounded failure-summary dialogs,
/// and the conversion of JUCE integer menu, command, and column ids back to scoped enums.
/// Native platform details remain in the implementation rather than application and component classes.

#pragma once

#include <type_traits>

#include <JuceHeader.h>

#include <optional>
#include <utility>

namespace audiobatch::utils
{
void allowExistingInstanceToActivate(const juce::String& applicationName);
void bringWindowToFront(juce::TopLevelWindow& window);
void styleLabel(juce::Label& label, const juce::Font& font, juce::Colour colour);
void showFailureSummary(
    const juce::StringArray& failures,
    const juce::String& title,
    const juce::String& failedAction,
    juce::Component* associatedComponent
);

/// Converts an integer id received from JUCE back to a scoped enum whose values run without gaps from first to last.
/// Returns nullopt for any other id, such as the zero JUCE uses for a dismissed menu or for no sort column.
template<typename Enum, Enum first, Enum last>
    requires std::is_scoped_enum_v<Enum>
[[nodiscard]] constexpr std::optional<Enum> enumFromId(const int juceId) noexcept
{
    if (juceId < std::to_underlying(first) || juceId > std::to_underlying(last)) {
        return std::nullopt;
    }
    return static_cast<Enum>(juceId);
}
}  // namespace audiobatch::utils
