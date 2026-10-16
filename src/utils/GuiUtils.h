/// Reusable GUI operations shared by application windows and components.
/// Declares foreground activation, read-only label styling, and bounded failure-summary dialogs.
/// Native platform details remain in the implementation rather than application and component classes.

#pragma once

#include <JuceHeader.h>

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
}  // namespace audiobatch::utils
