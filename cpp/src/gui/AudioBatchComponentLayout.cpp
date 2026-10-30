/// Layout of the main view, with the status bar along the bottom edge,
/// and the split between the results table and the preview workspace above it.
/// Animates the workspace between its expanded and compact presentations,
/// preserving the expanded table divider independently of the compact row.

#include "gui/AudioBatchComponent.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/GuiLayout.h"
#include "utils/StringFormat.h"

#include <array>
#include <functional>
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

/// Sets the expanded workspace to the height that fits its controls at the current width, and lays the view out.
/// The window calls this once after it has chosen its startup bounds,
/// because the constructor lays the component out at a provisional size whose width can pick the other layout.
/// Later window resizes and divider drags keep the height the user sees instead.
void AudioBatchComponent::fitPreviewToControls()
{
    const auto workspaceWidth = getWidth() - layout::mainContentPadding * 2;
    if (workspaceWidth <= 0 || previewFitInitialized) {
        return;
    }
    previewFitInitialized = true;
    expandedPreviewHeight = layout::expandedPreviewFitHeight(workspaceWidth);
    restoreExpandedLayout = true;
    resized();
}

/// Reverses from the currently displayed height without interrupting preview playback or changing settings.
void AudioBatchComponent::togglePreviewSection()
{
    animationStartHeight = currentPreviewHeight;
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
/// The expanded workspace keeps its height when the window is resized, and the results table takes the rest.
/// Secondary controls appear only in the settled expanded layout.
void AudioBatchComponent::resized()
{
    auto bounds = getLocalBounds();
    statusBar.setBounds(bounds.removeFromBottom(layout::statusBar::height));
    bounds.reduce(layout::mainContentPadding, layout::mainContentPadding);
    workspaceAvailableHeight = juce::jmax(1, bounds.getHeight());
    if (expandedPreviewHeight == 0 && bounds.getHeight() > 0) {
        expandedPreviewHeight = layout::expandedPreviewFitHeight(bounds.getWidth());
    }

    const bool compact = !previewSectionExpanded || !previewAnimationAttachment.isEmpty();
    waveformResizeBar.setVisible(!compact);
    previewWorkspace.setCompact(compact);

    if (compact) {
        const auto targetHeight = previewSectionExpanded ? getExpandedPreviewHeight() : layout::compactPreview::height;
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
        const auto startHeight = static_cast<double>(animationStartHeight);
        currentPreviewHeight = juce::jlimit(
            layout::compactPreview::height,
            juce::jmax(layout::compactPreview::height, bounds.getHeight()),
            juce::roundToInt(startHeight + (targetHeight - startHeight) * easedProgress)
        );
        previewWorkspace.setBounds(bounds.removeFromBottom(currentPreviewHeight));
        bounds.removeFromBottom(static_cast<int>(layout::resizeBarHeight));
        resultsTable.setBounds(bounds);
    } else {
        // A layout at an unchanged height after the divider was dragged keeps the dragged height.
        const auto previewHeight = getExpandedPreviewHeight();
        const bool setsLayout = restoreExpandedLayout || workspaceAvailableHeight != expandedLayoutHeight;
        if (setsLayout) {
            setExpandedLayout(previewHeight);
            restoreExpandedLayout = false;
            expandedLayoutHeight = workspaceAvailableHeight;
        }
        std::array<juce::Component*, 3> verticalSections {&resultsTable, &waveformResizeBar, &previewWorkspace};
        mainVerticalLayout.layOutComponents(
            verticalSections.data(), 3, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), true, true
        );
        currentPreviewHeight = previewWorkspace.getHeight();
        if (!setsLayout && currentPreviewHeight != previewHeight) {
            expandedPreviewHeight = currentPreviewHeight;
        }
    }
    updateResultsTableColumnWidths();
}

/// Returns the expanded workspace height fitted into the current window,
/// within the workspace limits and leaving the results table at least its minimum height.
int AudioBatchComponent::getExpandedPreviewHeight() const
{
    return layout::constrainedPreviewHeight(expandedPreviewHeight, workspaceAvailableHeight);
}

/// Sets the preferred proportions of the table and the workspace
/// that give the expanded workspace the given height at the current shared height.
void AudioBatchComponent::setExpandedLayout(const int previewHeight)
{
    const auto height = static_cast<double>(workspaceAvailableHeight);
    mainVerticalLayout.setItemLayout(
        0,
        layout::resultsTableMinimumHeight,
        layout::resultsTableMaximumProportion,
        -(height - previewHeight - layout::resizeBarHeight) / height
    );
    mainVerticalLayout.setItemLayout(
        2, layout::previewMinimumHeight, layout::previewMaximumProportion, -previewHeight / height
    );
}
}  // namespace audiobatch
