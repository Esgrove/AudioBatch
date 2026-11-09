/// Reusable GUI operations shared by application windows and components.
/// Declares foreground activation, keeping restored window bounds on screen, read-only label styling,
/// bounded failure-summary dialogs,
/// and the conversion of JUCE integer menu, command, and column ids back to scoped enums.
/// Native platform details remain in the implementation rather than application and component classes.

#pragma once

#include <type_traits>

#include <JuceHeader.h>

#include <optional>
#include <utility>

namespace audiobatch::utils
{
/// Result code for a dialog's confirmation action.
inline constexpr int DIALOG_RESULT_OK {1};
/// Result code for cancelling or dismissing a dialog.
inline constexpr int DIALOG_RESULT_CANCEL {0};
/// Result code returned when a popup menu closes without a selection.
inline constexpr int MENU_RESULT_DISMISSED {0};

[[nodiscard]] juce::Rectangle<int> boundsOnScreen(juce::Rectangle<int> bounds, juce::BorderSize<int> frame = {});

void allowExistingInstanceToActivate(const juce::String& applicationName);
void bringWindowToFront(juce::TopLevelWindow& window);
void showFailureSummary(
    const juce::StringArray& failures,
    const juce::String& title,
    const juce::String& failedAction,
    juce::Component& associatedComponent
);
void styleLabel(juce::Label& label, const juce::Font& font, juce::Colour colour);

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
