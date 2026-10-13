/// AudioBatchComponent member functions for the audio preview:
/// audio device access, the transport and waveform, zoom, the info panel, and the custom gain controls.

#include "AudioBatchComponent.h"
#include "AudioInfoPanel.h"
#include "CustomLookAndFeel.h"
#include "Log.h"

void AudioBatchComponent::requestAudioDeviceAccess()
{
    utils::logDebug("AudioBatchComponent startup: requesting record-audio permission");
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](const bool granted) {
        const int numInputChannels = granted ? 2 : 0;
        const auto audioInitialiseStartedAtMs = juce::Time::getMillisecondCounterHiRes();

        utils::logDebug(
            "AudioBatchComponent startup: audio permission callback granted={}", granted ? "true" : "false"
        );
        utils::logDebug(
            "AudioBatchComponent startup: audio device init begin (inputs={}, outputs=2)", numInputChannels
        );

        const auto initialiseError = audioDeviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
        const auto audioInitialiseElapsedMs = juce::Time::getMillisecondCounterHiRes() - audioInitialiseStartedAtMs;

        if (initialiseError.isEmpty()) {
            utils::logDebug(
                "AudioBatchComponent startup: audio device init succeeded ({:.1f} ms)", audioInitialiseElapsedMs
            );
        } else {
            utils::logError(
                "AudioBatchComponent startup: audio device init failed ({:.1f} ms): {}",
                audioInitialiseElapsedMs,
                initialiseError
            );
        }
    });
}

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

void AudioBatchComponent::showAudioResource(juce::URL resource)
{
    // Both branches below clear the remaining Serato presentation state.
    if (seratoPreviewLoader) {
        seratoPreviewLoader->cancel();
    }
    audioInfo->clear();

    if (loadURLIntoTransport(resource)) {
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

void AudioBatchComponent::requestSeratoPreview()
{
    seratoPreviewData.reset();
    seratoPreviewIdentity.reset();
    thumbnail->setSeratoOverview({});
    thumbnail->setSeratoMarkers({});
    if (seratoPreviewLoader && currentAudioUrl.isLocalFile() && currentAudioUrl.getLocalFile() == currentAudioFile
        && currentAudioFileSource != nullptr)
    {
        const auto* reader = currentAudioFileSource->getAudioFormatReader();
        const auto duration
            = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
        seratoPreviewIdentity.emplace(currentAudioFile);
        seratoPreviewLoader->request(currentAudioFile, duration);
    } else if (seratoPreviewLoader) {
        seratoPreviewLoader->cancel();
    }
}

void AudioBatchComponent::adoptSeratoPreview(
    const AnalysisCache::FileIdentity& identity,
    const audiobatch::serato::SeratoPreviewLoader::Generation generation,
    audiobatch::serato::SeratoPreviewLoader::Data data
)
{
    if (seratoPreviewLoader && thumbnail != nullptr && currentAudioFileSource != nullptr
        && currentAudioUrl.isWellFormed() && currentAudioUrl.isLocalFile()
        && currentAudioUrl.getLocalFile() == currentAudioFile
        && seratoPreviewLoader->canAdopt(identity, generation, currentAudioFile))
    {
        seratoPreviewIdentity = identity;
        seratoPreviewData = std::move(data);
        thumbnail->setSeratoOverview(
            seratoPreviewData && seratoPreviewData->overview
                ? std::shared_ptr<const audiobatch::serato::Overview>(seratoPreviewData, &*seratoPreviewData->overview)
                : nullptr
        );
        thumbnail->setSeratoMarkers(
            seratoPreviewData && seratoPreviewData->markers
                ? std::shared_ptr<const audiobatch::serato::Markers>(seratoPreviewData, &*seratoPreviewData->markers)
                : nullptr
        );
        const auto row = findRecordIndex(currentAudioFile.getFullPathName());
        if (juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
            updateAudioInfo(analysisResults[static_cast<std::size_t>(row)]);
        }
    }
}

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

bool AudioBatchComponent::loadURLIntoTransport(const juce::URL& audioUrl)
{
    // Unload the previous file source and delete it.
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();

    auto* reader = formatManager.createReaderFor(
        audioUrl.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress))
    );

    if (reader != nullptr) {
        currentAudioFileSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        transportSource.setSource(currentAudioFileSource.get(), 32768, &thread, reader->sampleRate);

        return true;
    }

    return false;
}

void AudioBatchComponent::startOrStop()
{
    if (transportSource.isPlaying()) {
        transportSource.stop();
    } else {
        transportSource.start();
    }
}

void AudioBatchComponent::updatePlaybackButtonForTransportState()
{
    if (!startStopButton.isEnabled()) {
        return;
    }

    const auto colour = transportSource.isPlaying() ? juce::CustomLookAndFeel::red() : juce::CustomLookAndFeel::green();
    startStopButton.setColour(juce::TextButton::buttonColourId, colour);
}

void AudioBatchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &transportSource) {
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

void AudioBatchComponent::clearCurrentAudioPreview()
{
    currentAudioFile = juce::File();
    currentAudioUrl = {};
    releasePreviewPlayback();
}

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
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();
    thumbnail->setURL(juce::URL());
    currentWaveformLoadedFromCache = false;
    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
}

void AudioBatchComponent::mouseMagnify(const juce::MouseEvent& /*event*/, const float scaleFactor)
{
    const auto newZoom = zoomSlider.getValue() * static_cast<double>(scaleFactor * scaleFactor);
    zoomSlider.setValue(newZoom);
}

void AudioBatchComponent::zoomLevelChanged(const double zoomLevel) const
{
    thumbnail->setZoom(zoomLevel);
}

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
