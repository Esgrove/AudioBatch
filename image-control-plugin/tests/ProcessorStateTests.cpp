/// Tests processor state round trips, legacy and invalid state handling, editor preferences, and saved editor size.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "tests/ProcessorTestSupport.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstddef>
#include <memory>

namespace esgrove::image_control::test
{
namespace
{
/// Records host dirty notifications independently of parameter automation.
class StateChangeListener final : public juce::AudioProcessorListener
{
public:
    /// Parameter edits are outside the non-parameter dirty contract.
    void audioProcessorParameterChanged(
        juce::AudioProcessor* /*processor*/,
        int /*parameterIndex*/,
        float /*value*/
    ) override
    {}
    /// Counts notifications that ask the host to persist editor-only state.
    void audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged) {
            ++dirtyChanges;
        }
    }
    int dirtyChanges {0};
};
}  // namespace

TEST(Processor, WidthAndRecoverSavedStateRendersTheSameHeadlessOutput)
{
    ImageControlProcessor source;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        setParameter(source, widthParameterId(band), static_cast<float>(band) * 0.25F);
        setParameter(source, recoverParameterId(band), 50.0F + static_cast<float>(band) * 50.0F);
    }
    setParameter(source, midGainParameterId(1), -3.0F);
    setParameter(source, sideGainParameterId(2), -6.0F);
    setParameter(source, panParameterId(3), -25.0F);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FLOAT_EQ(getParameter(restored, recoverParameterId(band)), 50.0F + static_cast<float>(band) * 50.0F);
    }
    source.setNonRealtime(false);
    restored.setNonRealtime(true);
    source.prepareToPlay(TEST_SAMPLE_RATE, 37);
    restored.prepareToPlay(TEST_SAMPLE_RATE, 511);
    const auto left = makeNoise(0.3F, 16'384, 114);
    const auto right = makeNoise(0.3F, 16'384, 115);
    auto realtime = makeBuffer({left, right});
    auto offline = makeBuffer({left, right});
    renderBuffer(source, realtime, 37);
    renderBuffer(restored, offline, 511);
    for (int sample = 0; sample < realtime.getNumSamples(); ++sample) {
        ASSERT_EQ(realtime.getSample(0, sample), offline.getSample(0, sample));
        ASSERT_EQ(realtime.getSample(1, sample), offline.getSample(1, sample));
    }
    source.releaseResources();
    restored.releaseResources();
}

/// Fresh instances smooth correlation numbers, and each saved readout preference replaces the prior setting.
TEST(Processor, SmoothCorrelationReadoutDefaultsOnAndRoundTripsBothValues)
{
    ImageControlProcessor source;
    EXPECT_TRUE(static_cast<bool>(source.getEditorState().getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    EXPECT_STREQ(SMOOTH_CORRELATION_READOUT_PROPERTY, "smoothCorrelationReadout");
    for (const bool enabled : {false, true}) {
        source.getEditorState().setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, enabled, nullptr);
        juce::MemoryBlock state;
        source.getStateInformation(state);
        ImageControlProcessor restored;
        restored.getEditorState().setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, !enabled, nullptr);
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_EQ(
            static_cast<bool>(restored.getEditorState().getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)), enabled
        );
    }
}

/// Toolbar disclosure is editor-only state, defaults expanded, and round-trips both visibility choices.
TEST(Processor, ProcessingToolbarDefaultsExpandedAndRoundTripsBothValues)
{
    ImageControlProcessor source;
    EXPECT_STREQ(SHOW_PROCESSING_TOOLBAR_PROPERTY, "showProcessingToolbar");
    EXPECT_TRUE(static_cast<bool>(source.getEditorState().getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)));
    for (const bool shown : {false, true}) {
        source.getEditorState().setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, shown, nullptr);
        juce::MemoryBlock state;
        source.getStateInformation(state);
        ImageControlProcessor restored;
        restored.getEditorState().setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, !shown, nullptr);
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_EQ(static_cast<bool>(restored.getEditorState().getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)), shown);
        EXPECT_FALSE(restored.getHistory().canUndo());
    }
}

TEST(Processor, RestoringAnOlderStateKeepsMissingEditorSettingsAtTheirDefaults)
{
    ImageControlProcessor source;
    source.getEditorState().setProperty(linkPropertyName(0), false, nullptr);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* editorSettings = xml->getChildByName("EditorState");
    ASSERT_NE(editorSettings, nullptr);
    // A state saved before these settings existed has none of them.
    for (const char* property :
         {
             ANALYZE_OUTPUT_PROPERTY,
             PERSISTENCE_PROPERTY,
             PAN_MIDPOINT_PROPERTY,
             PAN_EDGE_PROPERTY,
             SHOW_FPS_PROPERTY,
             SHOW_BAND_CORRELATION_PROPERTY,
             SMOOTH_CORRELATION_READOUT_PROPERTY,
             BYPASS_RENDERING_PROPERTY,
             HIDE_SPECTROGRAM_PROPERTY,
             SHOW_PROCESSING_TOOLBAR_PROPERTY,
         })
    {
        editorSettings->removeAttribute(property);
    }
    // A state from before the pan curve has the linear focus setting instead.
    editorSettings->setAttribute(LEGACY_FOCUS_PROPERTY, 24.0);
    juce::MemoryBlock olderState;
    juce::AudioProcessor::copyXmlToBinary(*xml, olderState);

    ImageControlProcessor restored;
    restored.getEditorState().setProperty(ANALYZE_OUTPUT_PROPERTY, false, nullptr);
    restored.getEditorState().setProperty(PERSISTENCE_PROPERTY, 2.0, nullptr);
    restored.getEditorState().setProperty(PAN_MIDPOINT_PROPERTY, 20.0, nullptr);
    restored.getEditorState().setProperty(PAN_EDGE_PROPERTY, 50.0, nullptr);
    restored.getEditorState().setProperty(SHOW_FPS_PROPERTY, true, nullptr);
    restored.getEditorState().setProperty(SHOW_BAND_CORRELATION_PROPERTY, false, nullptr);
    restored.getEditorState().setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, false, nullptr);
    restored.getEditorState().setProperty(BYPASS_RENDERING_PROPERTY, true, nullptr);
    restored.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    restored.getEditorState().setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, false, nullptr);
    restored.getEditorState().setProperty(EDITOR_WIDTH_PROPERTY, 1700, nullptr);
    restored.getEditorState().setProperty(EDITOR_HEIGHT_PROPERTY, 1200, nullptr);
    restored.getEditorState().setProperty(BAND_SECTION_HEIGHT_PROPERTY, 300, nullptr);
    const auto boundSettings = restored.getEditorState();
    restored.setStateInformation(olderState.getData(), static_cast<int>(olderState.getSize()));
    const auto& settings = restored.getEditorState();
    EXPECT_EQ(settings, boundSettings);
    EXPECT_FALSE(settings.hasProperty(EDITOR_WIDTH_PROPERTY));
    EXPECT_FALSE(settings.hasProperty(EDITOR_HEIGHT_PROPERTY));
    EXPECT_FALSE(settings.hasProperty(BAND_SECTION_HEIGHT_PROPERTY));
    EXPECT_TRUE(static_cast<bool>(settings.getProperty(SHOW_BAND_CORRELATION_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(settings.getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(linkPropertyName(0))));
    EXPECT_TRUE(static_cast<bool>(settings.getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_DOUBLE_EQ(static_cast<double>(settings.getProperty(PERSISTENCE_PROPERTY)), DEFAULT_PERSISTENCE_SECONDS);
    EXPECT_DOUBLE_EQ(static_cast<double>(settings.getProperty(PAN_MIDPOINT_PROPERTY)), DEFAULT_PAN_MIDPOINT_DB);
    EXPECT_DOUBLE_EQ(static_cast<double>(settings.getProperty(PAN_EDGE_PROPERTY)), DEFAULT_PAN_EDGE_DB);
    EXPECT_FALSE(settings.hasProperty(LEGACY_FOCUS_PROPERTY));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(SHOW_FPS_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(BYPASS_RENDERING_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(HIDE_SPECTROGRAM_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(settings.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)));
}

TEST(Processor, EditorChangesNotifyTheHostButRestoresDoNot)
{
    ImageControlProcessor source;
    juce::MemoryBlock state;
    source.getStateInformation(state);
    ImageControlProcessor processor;
    StateChangeListener listener;
    processor.addListener(&listener);
    for (const auto& property :
         {
             juce::Identifier(ANALYZE_OUTPUT_PROPERTY),
             juce::Identifier(SHOW_BAND_CORRELATION_PROPERTY),
             juce::Identifier(SMOOTH_CORRELATION_READOUT_PROPERTY),
             linkPropertyName(1),
         })
    {
        processor.getEditorState().setProperty(property, false, nullptr);
    }
    EXPECT_EQ(listener.dirtyChanges, 4);
    processor.getEditorState().setProperty(PERSISTENCE_PROPERTY, 2.0, nullptr);
    EXPECT_EQ(listener.dirtyChanges, 5);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_EQ(listener.dirtyChanges, 5);
    EXPECT_TRUE(static_cast<bool>(processor.getEditorState().getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(processor.getEditorState().getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    processor.getEditorState().setProperty(SHOW_FPS_PROPERTY, true, nullptr);
    EXPECT_EQ(listener.dirtyChanges, 6);
    processor.removeListener(&listener);
}

TEST(Processor, StateWithoutEditorSettingsReplacesCurrentSettingsWithDefaults)
{
    ImageControlProcessor source;
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    xml->removeChildElement(xml->getChildByName("EditorState"), true);
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(linkPropertyName(2), false, nullptr);
    processor.getEditorState().setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, false, nullptr);
    processor.getEditorState().setProperty(SHOW_BAND_CORRELATION_PROPERTY, false, nullptr);
    processor.getEditorState().setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, false, nullptr);
    processor.getEditorState().setProperty(PHASE_COLOURS_PROPERTY, true, nullptr);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_TRUE(static_cast<bool>(processor.getEditorState().getProperty(linkPropertyName(2))));
    EXPECT_TRUE(static_cast<bool>(processor.getEditorState().getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(processor.getEditorState().getProperty(SHOW_BAND_CORRELATION_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(processor.getEditorState().getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(processor.getEditorState().getProperty(PHASE_COLOURS_PROPERTY)));
}

TEST(Processor, NonFiniteSavedValuesPreservePriorValues)
{
    ImageControlProcessor processor;
    setParameter(processor, OUTPUT_GAIN_ID, -6.0F);
    setParameter(processor, recoverParameterId(0), 37.5F);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    for (const auto& parameterId :
         {
             juce::String(OUTPUT_GAIN_ID),
             crossoverParameterId(0),
             panParameterId(0),
             soloParameterId(0),
             widthParameterId(0),
             recoverParameterId(0),
         })
    {
        auto* child = xml->getChildByAttribute("id", parameterId);
        ASSERT_NE(child, nullptr);
        child->setAttribute("value", "nan");
    }
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(getParameter(processor, OUTPUT_GAIN_ID), -6.0F, 0.01F);
    EXPECT_FLOAT_EQ(getParameter(processor, recoverParameterId(0)), 38.0F);
    EXPECT_FLOAT_EQ(processor.getParameters().getRawParameterValue(recoverParameterId(0))->load(), 38.0F);
    for (auto* base : static_cast<juce::AudioProcessor&>(processor).getParameters()) {
        const auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(base);
        ASSERT_NE(parameter, nullptr);
        EXPECT_TRUE(std::isfinite(plainValue(*parameter)));
    }
}

TEST(Processor, RestoringAStateWithoutPansCentresThem)
{
    ImageControlProcessor source;
    setParameter(source, midGainParameterId(0), -3.0F);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    // A state saved before the pan parameters existed has none of them.
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        auto* pan = xml->getChildByAttribute("id", panParameterId(band));
        ASSERT_NE(pan, nullptr);
        xml->removeChildElement(pan, true);
    }
    juce::MemoryBlock olderState;
    juce::AudioProcessor::copyXmlToBinary(*xml, olderState);

    ImageControlProcessor restored;
    setParameter(restored, panParameterId(0), 70.0F);
    restored.setStateInformation(olderState.getData(), static_cast<int>(olderState.getSize()));
    EXPECT_NEAR(getParameter(restored, midGainParameterId(0)), -3.0F, 0.01F);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FLOAT_EQ(getParameter(restored, panParameterId(band)), 0.0F);
    }
}

TEST(Processor, RestoringAStateWithoutSolosLeavesThemOff)
{
    ImageControlProcessor source;
    setParameter(source, soloParameterId(2), 1.0F);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    // A state saved before the solo parameters existed has none of them.
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        auto* solo = xml->getChildByAttribute("id", soloParameterId(band));
        ASSERT_NE(solo, nullptr);
        xml->removeChildElement(solo, true);
    }
    juce::MemoryBlock olderState;
    juce::AudioProcessor::copyXmlToBinary(*xml, olderState);

    ImageControlProcessor restored;
    setParameter(restored, soloParameterId(2), 1.0F);
    restored.setStateInformation(olderState.getData(), static_cast<int>(olderState.getSize()));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FLOAT_EQ(getParameter(restored, soloParameterId(band)), 0.0F);
    }
}

TEST(Processor, StateRoundTripsParametersAndEditorSettings)
{
    ImageControlProcessor source;
    setParameter(source, crossoverParameterId(0), 200.0F);
    setParameter(source, midGainParameterId(2), -3.0F);
    setParameter(source, sideGainParameterId(0), BAND_GAIN_MIN_DB);
    setParameter(source, panParameterId(1), -35.0F);
    setParameter(source, soloParameterId(3), 1.0F);
    setParameter(source, widthParameterId(2), 0.35F);
    setParameter(source, recoverParameterId(2), 72.5F);
    EXPECT_FLOAT_EQ(getParameter(source, recoverParameterId(2)), 73.0F);
    setParameter(source, OUTPUT_GAIN_ID, 1.5F);
    source.getEditorState().setProperty(linkPropertyName(1), false, nullptr);
    source.getEditorState().setProperty(PERSISTENCE_PROPERTY, 2.5, nullptr);
    source.getEditorState().setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, false, nullptr);
    juce::MemoryBlock state;
    source.getStateInformation(state);

    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(getParameter(restored, crossoverParameterId(0)), 200.0F, 0.5F);
    EXPECT_NEAR(getParameter(restored, midGainParameterId(2)), -3.0F, 0.01F);
    EXPECT_NEAR(getParameter(restored, sideGainParameterId(0)), BAND_GAIN_MIN_DB, 0.01F);
    EXPECT_NEAR(getParameter(restored, panParameterId(1)), -35.0F, 0.01F);
    EXPECT_FLOAT_EQ(getParameter(restored, soloParameterId(3)), 1.0F);
    EXPECT_FLOAT_EQ(getParameter(restored, soloParameterId(0)), 0.0F);
    EXPECT_NEAR(getParameter(restored, widthParameterId(2)), 0.35F, 0.001F);
    EXPECT_FLOAT_EQ(getParameter(restored, recoverParameterId(2)), 73.0F);
    EXPECT_FLOAT_EQ(getParameter(restored, widthParameterId(0)), 1.0F);
    EXPECT_FLOAT_EQ(getParameter(restored, recoverParameterId(0)), 0.0F);
    EXPECT_NEAR(getParameter(restored, OUTPUT_GAIN_ID), 1.5F, 0.01F);
    EXPECT_FALSE(static_cast<bool>(restored.getEditorState().getProperty(linkPropertyName(1))));
    EXPECT_TRUE(static_cast<bool>(restored.getEditorState().getProperty(linkPropertyName(0))));
    EXPECT_DOUBLE_EQ(static_cast<double>(restored.getEditorState().getProperty(PERSISTENCE_PROPERTY)), 2.5);
    EXPECT_FALSE(static_cast<bool>(restored.getEditorState().getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
}

TEST(Processor, IgnoresForeignState)
{
    ImageControlProcessor processor;
    setParameter(processor, OUTPUT_GAIN_ID, -6.0F);
    const juce::String foreign = "<Other gain=\"1\"/>";
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(*juce::parseXML(foreign), state);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(getParameter(processor, OUTPUT_GAIN_ID), -6.0F, 0.01F);
}

TEST(Processor, EditorRestoresItsSavedSizeWhenReopened)
{
    ImageControlProcessor processor;
    auto& editorState = processor.getEditorState();
    editorState.setProperty(EDITOR_WIDTH_PROPERTY, 1500, nullptr);
    editorState.setProperty(EDITOR_HEIGHT_PROPERTY, 1100, nullptr);
    editorState.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 300, nullptr);

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditorAndMakeActive());
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getWidth(), 1500);
    EXPECT_EQ(editor->getHeight(), 1100);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(EDITOR_WIDTH_PROPERTY)), 1500);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY)), 1100);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 300);
    editor->setSize(1700, 1200);
    // Hosts tell the processor before they delete its editor.
    processor.editorBeingDeleted(editor.get());
    editor.reset();

    editor.reset(processor.createEditorAndMakeActive());
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getWidth(), 1700);
    EXPECT_EQ(editor->getHeight(), 1200);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 300);
    processor.editorBeingDeleted(editor.get());
}
}  // namespace esgrove::image_control::test
