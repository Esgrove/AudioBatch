/// AudioBatchComponent member functions for the audio preview:
/// loading files into the preview player and waveform, the play button state, zoom, the info panel,
/// and the custom gain controls.

#include "gui/AudioBatchComponent.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/CustomLookAndFeel.h"
#include "utils/Log.h"

#include <cmath>

namespace audiobatch
{
/// Reports the active spectral-colour preference without requiring a selected file.
bool AudioBatchComponent::isSeratoWaveformColouringEnabled() const noexcept
{
    return previewPreferences.seratoWaveformColours;
}

/// Switches the spectral-colour brush without altering cues, metadata, playback, zoom, or cached peaks.
void AudioBatchComponent::toggleSeratoWaveformColouring()
{
    auto preferences = previewPreferences;
    preferences.seratoWaveformColours = !preferences.seratoWaveformColours;
    setPreviewPreferences(preferences);
}

/// Reports the canonical live-chain setting to menu command descriptions.
bool AudioBatchComponent::isPluginPreviewEnabled() const noexcept
{
    return previewPreferences.pluginChainEnabled;
}

/// Reports compensation independently of whether live preview is currently enabled.
bool AudioBatchComponent::isPreviewLatencyCompensated() const noexcept
{
    return previewPreferences.latencyCompensated;
}

/// Reports whether active playback should continue after a user selects a different file.
bool AudioBatchComponent::isContinuePreviewPlaybackEnabled() const noexcept
{
    return previewPreferences.continuePlayback;
}

/// Reports whether newly loaded files use an eligible first cue as their initial position.
bool AudioBatchComponent::isPlayFromFirstCuePointEnabled() const noexcept
{
    return previewPreferences.playFromFirstCuePoint;
}

/// Toggles live processing through the same setter used by Audio Settings.
void AudioBatchComponent::togglePluginPreview()
{
    auto preferences = previewPreferences;
    preferences.pluginChainEnabled = !preferences.pluginChainEnabled;
    setPreviewPreferences(preferences);
}

/// Re-primes playback after changing the compensation policy.
void AudioBatchComponent::togglePreviewLatencyCompensation()
{
    auto preferences = previewPreferences;
    preferences.latencyCompensated = !preferences.latencyCompensated;
    setPreviewPreferences(preferences);
}

/// Toggles selection continuation without starting a currently stopped preview.
void AudioBatchComponent::toggleContinuePreviewPlayback()
{
    auto preferences = previewPreferences;
    preferences.continuePlayback = !preferences.continuePlayback;
    setPreviewPreferences(preferences);
}

/// Toggles cue-based initial positioning without seeking or starting an already loaded preview.
void AudioBatchComponent::togglePlayFromFirstCuePoint()
{
    auto preferences = previewPreferences;
    preferences.playFromFirstCuePoint = !preferences.playFromFirstCuePoint;
    setPreviewPreferences(preferences);
}

/// Applies requested changes before publishing actual successful state to every preference surface.
void AudioBatchComponent::setPreviewPreferences(const AudioPreviewPreferences& preferences)
{
    const auto previous = previewPreferences;
    previewPreferences = preferences;
    if (!preferences.playFromFirstCuePoint) {
        previewPlayer.completeInitialPosition(0.0);
    }
    if (previous.latencyCompensated != preferences.latencyCompensated) {
        previewPlayer.setLatencyCompensated(preferences.latencyCompensated);
    }
    if (previous.pluginChainEnabled != preferences.pluginChainEnabled) {
        const auto enabled = previewPlayer.setPluginPreviewEnabled(preferences.pluginChainEnabled);
        juce::ignoreUnused(enabled);
    }
    previewPreferences.pluginChainEnabled = previewPlayer.isPluginPreviewEnabled();
    thumbnail->setSeratoWaveformColouringEnabled(previewPreferences.seratoWaveformColours);
    updatePreviewInputGain(true);
    synchronizePreviewPreferences();
}

/// Persists the canonical preferences and updates check marks without generating recursive control callbacks.
void AudioBatchComponent::synchronizePreviewPreferences()
{
    audioSettingsComponent.setPreferences(previewPreferences);
    const auto saved = saveAudioPreviewPreferences(*pluginAppProperties.getUserSettings(), previewPreferences);
    juce::ignoreUnused(saved);
    if (onViewOptionsChanged) {
        onViewOptionsChanged();
    }
}

/// Updates the fixed analysis cards and scrolling details, including pending and failed record states.
void AudioBatchComponent::updateAudioInfo(const AudioAnalysisRecord& record)
{
    if (record.file == currentAudioFile && seratoPreviewIdentity && !seratoPreviewIdentity->matches(record.file)) {
        requestSeratoPreview();
    }
    audioInfo->showRecord(
        record,
        fileActivity.getActiveLabel(record.fullPath),
        record.file == currentAudioFile ? seratoPreviewData.get() : nullptr
    );
}

/// Loads a file URL into the preview player and waveform, preferring cached waveform data over decoding the file again.
void AudioBatchComponent::showAudioResource(juce::URL resource)
{
    // Both branches below clear the remaining Serato presentation state.
    if (seratoPreviewLoader) {
        seratoPreviewLoader->cancel();
    }
    audioInfo->clear();

    if (previewPlayer.load(resource, previewPreferences.playFromFirstCuePoint)) {
        updatePreviewInputGain(true);
        currentAudioUrl = std::move(resource);
        currentWaveformLoadedFromCache = false;
        startStopButton.setEnabled(true);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green());
        zoomSlider.setValue(100.0, juce::dontSendNotification);

        juce::MemoryBlock waveformData;
        if (currentAudioFile.existsAsFile() && analysisCache.getWaveformData(currentAudioFile, waveformData)
            && thumbnail->loadFromCacheData(waveformData))
        {
            currentWaveformLoadedFromCache = true;
        } else {
            thumbnail->setURL(currentAudioUrl);
        }

        requestSeratoPreview();
    } else {
        currentAudioUrl = {};
        releasePreviewPlayback();
    }
}

/// Clears stale tag presentation and requests the current preview's independent metadata.
/// Captures file identity so same-path changes detected during reanalysis trigger a fresh read.
void AudioBatchComponent::requestSeratoPreview()
{
    seratoPreviewData.reset();
    seratoPreviewIdentity.reset();
    thumbnail->setSeratoOverview({});
    thumbnail->setSeratoMarkers({});
    const auto duration = previewPlayer.getLoadedDurationSeconds();
    if (seratoPreviewLoader && currentAudioUrl.isLocalFile() && currentAudioUrl.getLocalFile() == currentAudioFile
        && duration.has_value() && std::isfinite(*duration) && *duration > 0.0)
    {
        seratoPreviewIdentity.emplace(currentAudioFile);
        if (seratoPreviewLoader->request(currentAudioFile, *duration) == 0) {
            previewPlayer.completeInitialPosition(0.0);
        }
    } else {
        if (seratoPreviewLoader) {
            seratoPreviewLoader->cancel();
        }
        previewPlayer.completeInitialPosition(0.0);
    }
}

/// Adopts immutable colours, cues and track info only while the generation, file and URL still match.
/// Resolves a new file's pending cue start without overriding a manual seek or later metadata refresh.
/// Never reloads playback or changes waveform geometry, gain, zoom or peak-cache data.
void AudioBatchComponent::adoptSeratoPreview(
    const FileIdentity& identity,
    const std::uint64_t generation,
    std::shared_ptr<const serato::SeratoData> data
)
{
    if (seratoPreviewLoader && thumbnail != nullptr && previewPlayer.hasLoadedSource() && currentAudioUrl.isWellFormed()
        && currentAudioUrl.isLocalFile() && currentAudioUrl.getLocalFile() == currentAudioFile
        && identity.path == currentAudioFile.getFullPathName() && seratoPreviewLoader->isCurrentGeneration(generation))
    {
        if (!seratoPreviewLoader->canAdopt(identity, generation, currentAudioFile)) {
            previewPlayer.completeInitialPosition(0.0);
            return;
        }
        seratoPreviewIdentity = identity;
        seratoPreviewData = std::move(data);
        previewPlayer.completeInitialPosition(getPreviewStartPositionSeconds(
            previewPreferences,
            seratoPreviewData && seratoPreviewData->markers ? &*seratoPreviewData->markers : nullptr,
            previewPlayer.getLoadedDurationSeconds().value_or(0.0)
        ));
        thumbnail->setSeratoOverview(
            seratoPreviewData && seratoPreviewData->overview
                ? std::shared_ptr<const serato::Overview>(seratoPreviewData, &*seratoPreviewData->overview)
                : nullptr
        );
        thumbnail->setSeratoMarkers(
            seratoPreviewData && seratoPreviewData->markers
                ? std::shared_ptr<const serato::Markers>(seratoPreviewData, &*seratoPreviewData->markers)
                : nullptr
        );
        const auto row = findRecordIndex(currentAudioFile.getFullPathName());
        if (juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
            updateAudioInfo(analysisResults[static_cast<std::size_t>(row)]);
        }
    }
}

/// Stores freshly computed waveform data in the analysis cache,
/// unless the current waveform was itself loaded from the cache.
void AudioBatchComponent::handleThumbnailFullyLoaded()
{
    if (currentWaveformLoadedFromCache || !currentAudioFile.existsAsFile()) {
        return;
    }

    const auto waveformData = thumbnail->saveToCacheData();
    if (waveformData.getSize() == 0) {
        return;
    }

    currentWaveformLoadedFromCache = analysisCache.storeWaveformData(currentAudioFile, waveformData);
}

/// Recolours the play button to match whether the transport is playing.
void AudioBatchComponent::updatePlaybackButtonForTransportState()
{
    if (!startStopButton.isEnabled()) {
        return;
    }

    const auto colour = previewPlayer.isPlaying() ? juce::CustomLookAndFeel::red() : juce::CustomLookAndFeel::green();
    startStopButton.setColour(juce::TextButton::buttonColourId, colour);
}

/// Reacts to transport state changes and to files dropped onto the waveform preview.
void AudioBatchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &previewPlayer) {
        updatePlaybackButtonForTransportState();
        return;
    }

    if (source == thumbnail.get()) {
        const auto droppedFiles = thumbnail->getLastDroppedFiles();

        if (!droppedFiles.isEmpty()) {
            handleDroppedPaths(droppedFiles);
        }
    }
}

/// Stops playback and releases the preview file, transport source, and waveform so the file can be replaced.
void AudioBatchComponent::clearCurrentAudioPreview()
{
    currentAudioFile = juce::File();
    currentAudioUrl = {};
    releasePreviewPlayback();
}

/// Stops playback, closes the preview file, and clears the waveform, keeping the current file selected,
/// so the file on disk can be replaced or the preview reset.
void AudioBatchComponent::releasePreviewPlayback()
{
    if (seratoPreviewLoader) {
        seratoPreviewLoader->cancel();
    }
    seratoPreviewData.reset();
    seratoPreviewIdentity.reset();
    thumbnail->setSeratoOverview({});
    thumbnail->setSeratoMarkers({});
    audioInfo->clear();
    previewPlayer.release();
    thumbnail->setURL(juce::URL());
    currentWaveformLoadedFromCache = false;
    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
}

/// Maps pinch-zoom gestures onto the waveform zoom slider.
void AudioBatchComponent::mouseMagnify(const juce::MouseEvent& /*event*/, const float scaleFactor)
{
    const auto newZoom = zoomSlider.getValue() * static_cast<double>(scaleFactor * scaleFactor);
    zoomSlider.setValue(newZoom);
}

/// Forwards the zoom slider value to the waveform preview.
void AudioBatchComponent::zoomLevelChanged(const double zoomLevel) const
{
    thumbnail->setZoom(zoomLevel);
}

/// Updates the current selection's custom gain in-memory, in the cache, and the waveform preview.
void AudioBatchComponent::applyCustomGainToSelection(const float gainDb, const bool hasGain)
{
    const auto selectedRows = resultsTable.getSelectedRows();
    if (selectedRows.size() == 0) {
        return;
    }

    bool anyChanged = false;

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];
        const auto previousHadGain = record.hasCustomGain;
        const auto previousValue = record.customGainDb;

        record.customGainDb = hasGain ? gainDb : 0.0f;
        record.hasCustomGain = hasGain;

        if (previousHadGain != record.hasCustomGain || !juce::approximatelyEqual(previousValue, record.customGainDb)) {
            analysisCache.storeCustomGain(record.file, record.customGainDb, record.hasCustomGain);
            anyChanged = true;
        }
    }

    if (anyChanged) {
        resultsTable.repaint();
        updateThumbnailDisplayGain();
        updatePreviewInputGain();
    }
}

/// Applies only the loaded file's gain, not another selected row's slider value or any normalization target.
void AudioBatchComponent::updatePreviewInputGain(const bool immediately)
{
    float gain = 1.0f;
    if (const auto row = findRecordIndex(currentAudioFile.getFullPathName());
        juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size())))
    {
        const auto& record = analysisResults[static_cast<std::size_t>(row)];
        if (record.hasCustomGain) {
            gain = juce::Decibels::decibelsToGain(record.customGainDb);
        }
    }
    previewPlayer.setInputGain(gain, immediately);
}

/// Syncs the gain slider with the record shown for the current selection.
void AudioBatchComponent::updateGainControlsForSelection()
{
    const auto selectedRows = resultsTable.getSelectedRows();
    const bool hasSelection = selectedRows.size() > 0;

    gainSlider.setEnabled(hasSelection);

    if (!hasSelection) {
        gainSlider.setValue(0.0, juce::dontSendNotification);
        return;
    }

    // Use the currently selected display row to drive the slider value.
    int referenceRow = selectedRows[0];
    if (currentAudioFile.existsAsFile()) {
        if (const auto currentRow = findRecordIndex(currentAudioFile.getFullPathName());
            currentRow >= 0 && selectedRows.contains(currentRow))
        {
            referenceRow = currentRow;
        }
    }

    if (!juce::isPositiveAndBelow(referenceRow, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(referenceRow)];
    gainSlider.setValue(
        record.hasCustomGain ? static_cast<double>(record.customGainDb) : 0.0, juce::dontSendNotification
    );
}

/// Applies the current file's custom gain to the waveform preview as a display-only scale factor.
void AudioBatchComponent::updateThumbnailDisplayGain() const
{
    if (thumbnail == nullptr) {
        return;
    }

    if (!currentAudioFile.existsAsFile()) {
        thumbnail->setDisplayGain(1.0f);
        return;
    }

    const auto currentRow = findRecordIndex(currentAudioFile.getFullPathName());
    if (!juce::isPositiveAndBelow(currentRow, static_cast<int>(analysisResults.size()))) {
        thumbnail->setDisplayGain(1.0f);
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(currentRow)];
    const auto gain = record.hasCustomGain ? juce::Decibels::decibelsToGain(record.customGainDb) : 1.0f;
    thumbnail->setDisplayGain(gain);
}
}  // namespace audiobatch
