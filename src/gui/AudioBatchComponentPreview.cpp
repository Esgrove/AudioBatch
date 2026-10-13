/// AudioBatchComponent member functions for the audio preview:
/// loading files into the preview player and waveform, the play button state, zoom, the info panel,
/// and the custom gain controls.

#include "gui/AudioBatchComponent.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/CustomLookAndFeel.h"
#include "utils/Log.h"

namespace audiobatch
{
/// Rebuilds the info panel rows for a record, with dedicated layouts for pending and failed files.
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

    if (previewPlayer.load(resource)) {
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
        && duration.has_value())
    {
        seratoPreviewIdentity.emplace(currentAudioFile);
        seratoPreviewLoader->request(currentAudioFile, *duration);
    } else if (seratoPreviewLoader) {
        seratoPreviewLoader->cancel();
    }
}

/// Adopts immutable colours, cues and track info only while the generation, file and URL still match.
/// Never reloads playback or changes waveform geometry, gain, zoom or peak-cache data.
void AudioBatchComponent::adoptSeratoPreview(
    const FileIdentity& identity,
    const serato::SeratoPreviewLoader::Generation generation,
    serato::SeratoPreviewLoader::Data data
)
{
    if (seratoPreviewLoader && thumbnail != nullptr && previewPlayer.hasLoadedSource() && currentAudioUrl.isWellFormed()
        && currentAudioUrl.isLocalFile() && currentAudioUrl.getLocalFile() == currentAudioFile
        && seratoPreviewLoader->canAdopt(identity, generation, currentAudioFile))
    {
        seratoPreviewIdentity = identity;
        seratoPreviewData = std::move(data);
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
    if (source == &previewPlayer.getTransport()) {
        updatePlaybackButtonForTransportState();
        return;
    }

    if (source == thumbnail.get()) {
        const auto droppedFiles = thumbnail->getLastDroppedFiles();

        if (!droppedFiles.isEmpty()) {
            handleDroppedPaths(droppedFiles);
            return;
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
        gainClearButton.setEnabled(hasGain);
    }
}

/// Syncs the gain slider and clear button with the record shown for the current selection.
void AudioBatchComponent::updateGainControlsForSelection()
{
    const auto selectedRows = resultsTable.getSelectedRows();
    const bool hasSelection = selectedRows.size() > 0;

    gainSlider.setEnabled(hasSelection);
    gainClearButton.setEnabled(false);

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
    gainClearButton.setEnabled(record.hasCustomGain);
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
