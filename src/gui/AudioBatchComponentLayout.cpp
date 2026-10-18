/// Layout and animated expansion of the bottom preview workspace.
/// Reuses live controls, selected-file metrics, and waveform in expanded and compact presentations,
/// preserving the expanded table divider independently of the compact row.

#include "gui/AudioBatchComponent.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/GuiLayout.h"
#include "utils/StringFormat.h"

namespace audiobatch
{
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
        && (controlsViewport.hasKeyboardFocus(true) || audioInfo->hasKeyboardFocus(true)
            || waveformResizeBar.hasKeyboardFocus(true)))
    {
        previewSectionButton.grabKeyboardFocus();
    }
    const auto* action = previewSectionExpanded ? "Collapse bottom section" : "Expand bottom section";
    previewSectionButton.setButtonText(action);
    previewSectionButton.setToggleState(!previewSectionExpanded, juce::dontSendNotification);
    previewSectionButton.setTooltip(utils::format("{} (Ctrl+Shift+Space / Cmd+Shift+Space).", action));
    startTimerHz(layout::compactPreview::animationFramesPerSecond);
    resized();
    if (onPreviewSectionChanged) {
        onPreviewSectionChanged();
    }
}

/// Ends on the exact target layout, restoring normal divider behavior only once expansion is complete.
void AudioBatchComponent::timerCallback()
{
    if (juce::Time::getMillisecondCounterHiRes() - animationStartedAtMilliseconds
        >= layout::compactPreview::animationDurationMilliseconds)
    {
        stopTimer();
        restoreExpandedLayout = previewSectionExpanded;
    }
    resized();
    repaint();
}

/// Allocates one shared workspace height so the results table and live preview move together.
/// Secondary controls appear only in the settled expanded layout.
void AudioBatchComponent::resized()
{
    auto bounds = getLocalBounds().reduced(layout::mainContentPadding);
    auto topBar = bounds.removeFromTop(layout::topBarHeight);
    statusLabel.setBounds(topBar.removeFromRight(layout::statusLabelWidth));
    topBar.removeFromRight(layout::sectionSpacing);
    currentRootLabel.setBounds(topBar);
    bounds.removeFromTop(layout::sectionSpacing);
    workspaceAvailableHeight = juce::jmax(1, bounds.getHeight());

    const bool compact = !previewSectionExpanded || isTimerRunning();
    waveformResizeBar.setVisible(!compact);
    controlsViewport.setVisible(!compact);
    if (compact) {
        audioInfo->setCompact(true);
    }

    juce::Rectangle<int> previewBounds;
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
        if (isTimerRunning()) {
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
        previewBounds = bounds.removeFromBottom(currentPreviewHeight);
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
        std::array<juce::Component*, 3> verticalSections {&resultsTable, &waveformResizeBar, thumbnail.get()};
        mainVerticalLayout.layOutComponents(
            verticalSections.data(), 3, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), true, true
        );
        previewBounds = thumbnail->getBounds();
        currentPreviewHeight = previewBounds.getHeight();
        expandedPreviewProportion = static_cast<double>(currentPreviewHeight) / workspaceAvailableHeight;
    }
    updateResultsTableColumnWidths();

    auto workspace = previewBounds.reduced(layout::compactPreview::padding);
    if (compact) {
        const auto toggleBounds = workspace.removeFromLeft(layout::previewToggle::size);
        previewSectionButton.setBounds(
            toggleBounds.withSizeKeepingCentre(layout::previewToggle::size, layout::previewToggle::size)
        );
        workspace.removeFromLeft(layout::compactPreview::spacing);
        startStopButton.setBounds(workspace.removeFromLeft(layout::compactPreview::playbackWidth));
        workspace.removeFromLeft(layout::compactPreview::spacing);
        processButton.setBounds(workspace.removeFromLeft(layout::compactPreview::processWidth));
        workspace.removeFromLeft(layout::compactPreview::spacing);
        const auto waveformWidth = juce::jlimit(
            layout::compactPreview::minimumWaveformWidth,
            juce::jmax(
                layout::compactPreview::minimumWaveformWidth,
                workspace.getWidth() - layout::compactPreview::metricsMinimumWidth - layout::compactPreview::spacing
            ),
            juce::roundToInt(workspace.getWidth() * layout::compactPreview::waveformWidthProportion)
        );
        thumbnail->setBounds(workspace.removeFromRight(waveformWidth));
        workspace.removeFromRight(layout::compactPreview::spacing);
        audioInfo->setBounds(workspace);
        return;
    }

    const auto columns = layout::previewColumns(workspace);
    audioInfo->setBounds(columns.information);
    audioInfo->setCompact(false);
    thumbnail->setBounds(columns.waveform);
    auto controls
        = columns.controls.reduced(layout::information::horizontalPadding, layout::information::verticalPadding);
    previewSectionButton.setBounds(
        controls.removeFromTop(layout::previewToggle::size).withWidth(layout::previewToggle::size)
    );
    controls.removeFromTop(layout::previewToggle::expandedSpacing);
    auto playbackRow = controls.removeFromBottom(layout::processButtonHeight);
    startStopButton.setBounds(playbackRow.removeFromLeft((playbackRow.getWidth() - layout::sectionSpacing) / 2));
    playbackRow.removeFromLeft(layout::sectionSpacing);
    processButton.setBounds(playbackRow);
    controls.removeFromBottom(layout::controlsSectionSpacing);
    controlsViewport.setBounds(controls);
    controlsContent.setSize(
        juce::jmax(0, controls.getWidth() - layout::scrollBarThickness),
        juce::jmax(controls.getHeight(), layout::controlsContentHeight)
    );
    auto content = controlsContent.getLocalBounds();
    zoomLabel.setBounds(content.removeFromTop(layout::sliderRowHeight));
    zoomSlider.setBounds(content.removeFromTop(layout::sliderRowHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    gainLabel.setBounds(content.removeFromTop(layout::sliderRowHeight));
    gainSlider.setBounds(content.removeFromTop(layout::sliderRowHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    auto normalizeRow = content.removeFromTop(layout::normalizeToggleHeight);
    normalizeBeforePluginToggle.setBounds(
        normalizeRow.removeFromLeft((normalizeRow.getWidth() - layout::sectionSpacing) / 2)
    );
    normalizeRow.removeFromLeft(layout::sectionSpacing);
    forceOverwriteToggle.setBounds(normalizeRow);
    content.removeFromTop(layout::normalizeTargetSpacing);
    normalizationTargetControl.setBounds(content.removeFromTop(layout::controlButtonHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    pluginButton.setBounds(content.removeFromTop(layout::controlButtonHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    settingsButton.setBounds(content.removeFromTop(layout::controlButtonHeight));
}
}  // namespace audiobatch
