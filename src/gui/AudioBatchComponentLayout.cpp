/// Layout of the main view, with the status bar along the bottom edge,
/// and the split between the results table and the preview workspace above it.
/// Animates the workspace between its expanded and compact presentations,
/// preserving the expanded table divider independently of the compact row.

#include "gui/AudioBatchComponent.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/GuiLayout.h"
#include "utils/StringFormat.h"

#include <utility>

namespace audiobatch
{
/// Replaces the callback used to refresh window commands when preview view options change.
void AudioBatchComponent::setViewOptionsChangedCallback(std::function<void()> callback)
{
    onViewOptionsChanged = std::move(callback);
}

/// Reports the intended state immediately, including while an animation is running.
bool AudioBatchComponent::isPreviewSectionExpanded() const noexcept
{
    return previewSectionExpanded;
}

/// Reverses from the currently displayed height without interrupting preview playback or changing settings.
void AudioBatchComponent::togglePreviewSection()
{
    animationStartProportion = static_cast<double>(currentPreviewHeight) / juce::jmax(1, workspaceAvailableHeight);
    previewSectionExpanded = !previewSectionExpanded;
    animationStartedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    if (!previewSectionExpanded
        && (previewWorkspace.controlsViewport.hasKeyboardFocus(true)
            || previewWorkspace.audioInfo.hasKeyboardFocus(true) || waveformResizeBar.hasKeyboardFocus(true)))
    {
        previewWorkspace.previewSectionButton.grabKeyboardFocus();
    }
    const auto* action = previewSectionExpanded ? "Collapse bottom section" : "Expand bottom section";
    previewWorkspace.previewSectionButton.setButtonText(action);
    previewWorkspace.previewSectionButton.setToggleState(!previewSectionExpanded, juce::dontSendNotification);
    previewWorkspace.previewSectionButton.setTooltip(utils::format("{} (Ctrl+Shift+Space / Cmd+Shift+Space).", action));
    if (previewAnimationAttachment.isEmpty()) {
        previewAnimationAttachment = juce::VBlankAttachment(this, [this](double) { updatePreviewAnimation(); });
    }
    resized();
    if (onViewOptionsChanged) {
        onViewOptionsChanged();
    }
}

/// Ends on the exact target layout, restoring normal divider behavior only once expansion is complete.
void AudioBatchComponent::updatePreviewAnimation()
{
    if (juce::Time::getMillisecondCounterHiRes() - animationStartedAtMilliseconds
        >= layout::compactPreview::animationDurationMilliseconds)
    {
        previewAnimationAttachment = {};
        restoreExpandedLayout = previewSectionExpanded;
    }
    resized();
    repaint();
}

/// Puts the status bar along the full width of the bottom edge,
/// and allocates one shared workspace height above it so the results table and live preview move together.
/// Secondary controls appear only in the settled expanded layout.
void AudioBatchComponent::resized()
{
    auto bounds = getLocalBounds();
    statusBar.setBounds(bounds.removeFromBottom(layout::statusBar::height));
    bounds.reduce(layout::mainContentPadding, layout::mainContentPadding);
    workspaceAvailableHeight = juce::jmax(1, bounds.getHeight());

    const bool compact = !previewSectionExpanded || !previewAnimationAttachment.isEmpty();
    waveformResizeBar.setVisible(!compact);
    previewWorkspace.setCompact(compact);

    if (compact) {
        const auto maximumExpandedHeight = juce::jmax(
            static_cast<int>(layout::previewMinimumHeight),
            bounds.getHeight() - static_cast<int>(layout::resultsTableMinimumHeight + layout::resizeBarHeight)
        );
        const auto expandedHeight = juce::jlimit(
            static_cast<int>(layout::previewMinimumHeight),
            maximumExpandedHeight,
            juce::roundToInt(expandedPreviewProportion * workspaceAvailableHeight)
        );
        const auto targetHeight = previewSectionExpanded ? expandedHeight : layout::compactPreview::height;
        double progress = 1.0;
        if (!previewAnimationAttachment.isEmpty()) {
            progress = juce::jlimit(
                0.0,
                1.0,
                (juce::Time::getMillisecondCounterHiRes() - animationStartedAtMilliseconds)
                    / layout::compactPreview::animationDurationMilliseconds
            );
        }
        const auto easedProgress = progress * progress * (3.0 - 2.0 * progress);
        const auto startHeight = animationStartProportion * workspaceAvailableHeight;
        currentPreviewHeight = juce::jlimit(
            layout::compactPreview::height,
            juce::jmax(layout::compactPreview::height, bounds.getHeight()),
            juce::roundToInt(startHeight + (targetHeight - startHeight) * easedProgress)
        );
        previewWorkspace.setBounds(bounds.removeFromBottom(currentPreviewHeight));
        bounds.removeFromBottom(static_cast<int>(layout::resizeBarHeight));
        resultsTable.setBounds(bounds);
    } else {
        if (restoreExpandedLayout) {
            mainVerticalLayout.setItemLayout(
                0,
                layout::resultsTableMinimumHeight,
                layout::resultsTableMaximumProportion,
                -(1.0 - expandedPreviewProportion - layout::resizeBarHeight / workspaceAvailableHeight)
            );
            mainVerticalLayout.setItemLayout(
                2, layout::previewMinimumHeight, layout::previewMaximumProportion, -expandedPreviewProportion
            );
            restoreExpandedLayout = false;
        }
        std::array<juce::Component*, 3> verticalSections {&resultsTable, &waveformResizeBar, &previewWorkspace};
        mainVerticalLayout.layOutComponents(
            verticalSections.data(), 3, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), true, true
        );
        currentPreviewHeight = previewWorkspace.getHeight();
        expandedPreviewProportion = static_cast<double>(currentPreviewHeight) / workspaceAvailableHeight;
    }
    updateResultsTableColumnWidths();
}
}  // namespace audiobatch
