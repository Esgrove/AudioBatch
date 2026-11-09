/// Pins non-finite notification rejection with real JUCE attachments and stale correlation expiry.
/// Host lifecycle and display-generation transitions must not leave unsafe UI values or old meter readings.

#include "dsp/BandCorrelation.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <limits>

namespace esgrove::image_control::test
{
namespace
{
/// Observes the actual notification payload rather than APVTS's reread of the current value.
class ParameterValueListener final : public juce::AudioProcessorParameter::Listener
{
public:
    /// Records whether even one non-finite notification escaped the public parameter boundary.
    void parameterValueChanged(int /*index*/, const float value) override
    {
        ++notifications;
        allFinite = allFinite && std::isfinite(value);
    }

    /// Gesture boundaries are unrelated to invalid-value rejection.
    void parameterGestureChanged(int /*index*/, bool /*starting*/) override {}

    /// Number of value notifications received.
    int notifications {0};
    /// Whether all received payloads were finite.
    bool allFinite {true};
};

/// Fills and processes a coherent stereo high-frequency signal that yields a valid High-band reading.
void publishCorrelation(ImageControlProcessor& processor)
{
    juce::AudioBuffer<float> block(2, 256);
    juce::MidiBuffer midi;
    for (int pass = 0; pass < 64; ++pass) {
        for (int sample = 0; sample < 256; ++sample) {
            const float value = sample % 2 == 0 ? 0.25F : -0.25F;
            block.setSample(0, sample, value);
            block.setSample(1, sample, value);
        }
        processor.processBlock(block, midi);
    }
}

/// Checks the explicit unavailable state without treating NaN as a numeric correlation.
void expectUnavailable(const BandCorrelations& readings)
{
    for (const float value : readings) {
        EXPECT_TRUE(std::isnan(value));
    }
}
}  // namespace

TEST(ParameterNotifications, RejectedValuesNeverReachListenersOrAttachments)
{
    ImageControlProcessor processor;
    auto& pan = *processor.getParameters().getParameter(panParameterId(0));
    auto& solo = *processor.getParameters().getParameter(soloParameterId(0));
    juce::Slider slider;
    juce::ToggleButton button;
    const juce::SliderParameterAttachment sliderAttachment(pan, slider);
    const juce::ButtonParameterAttachment buttonAttachment(solo, button);
    ParameterValueListener panListener;
    ParameterValueListener soloListener;
    pan.addListener(&panListener);
    solo.addListener(&soloListener);
    pan.setValueNotifyingHost(pan.convertTo0to1(40.0F));
    solo.setValueNotifyingHost(1.0F);
    ASSERT_EQ(panListener.notifications, 1);
    ASSERT_EQ(soloListener.notifications, 1);
    for (const float invalid :
         {
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         })
    {
        pan.setValueNotifyingHost(invalid);
        solo.setValueNotifyingHost(invalid);
        pan.sendValueChangedMessageToListeners(invalid);
        solo.sendValueChangedMessageToListeners(invalid);
        EXPECT_NEAR(slider.getValue(), 40.0, 0.001);
        EXPECT_TRUE(button.getToggleState());
        EXPECT_TRUE(std::isfinite(pan.getValue()));
        EXPECT_FLOAT_EQ(solo.getValue(), 1.0F);
    }
    EXPECT_EQ(panListener.notifications, 1);
    EXPECT_EQ(soloListener.notifications, 1);
    EXPECT_TRUE(panListener.allFinite);
    EXPECT_TRUE(soloListener.allFinite);
    pan.removeListener(&panListener);
    solo.removeListener(&soloListener);
}

TEST(CorrelationLifecycle, StoppedAndHiddenViewsCannotShowAnOldReading)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
    processor.setCorrelationEnabled(false);
    expectUnavailable(processor.getBandCorrelations());
    processor.setCorrelationEnabled(true);
    expectUnavailable(processor.getBandCorrelations());
    juce::AudioBuffer<float> shortBlock(2, 1);
    shortBlock.clear();
    juce::MidiBuffer midi;
    processor.processBlock(shortBlock, midi);
    expectUnavailable(processor.getBandCorrelations());
    publishCorrelation(processor);
    EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
    processor.releaseResources();
    expectUnavailable(processor.getBandCorrelations());
}

TEST(CorrelationLifecycle, SnapshotsExposeTransitionsEvenWhenPublicationResumesBeforeTheNextRead)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    auto snapshot = processor.getBandCorrelationSnapshot();
    ASSERT_TRUE(std::isfinite(snapshot.readings.back()));
    processor.setCorrelationEnabled(true);
    EXPECT_EQ(processor.getBandCorrelationSnapshot().generation, snapshot.generation);

    processor.reset();
    publishCorrelation(processor);
    auto next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    EXPECT_NEAR(next.readings.back(), 1.0F, 1.0e-6F);
    snapshot = next;

    processor.prepareToPlay(96'000.0, 256);
    publishCorrelation(processor);
    next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    EXPECT_NEAR(next.readings.back(), 1.0F, 1.0e-6F);
    snapshot = next;

    // No callback or display read observes the disabled interval.
    processor.setCorrelationEnabled(false);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    EXPECT_NEAR(next.readings.back(), 1.0F, 1.0e-6F);
    snapshot = next;
    processor.releaseResources();
    next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    expectUnavailable(next.readings);
}

TEST(CorrelationLifecycle, MissingCallbacksExpireWithoutReadingOrMutatingAudioHistory)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    ASSERT_TRUE(std::isfinite(processor.getBandCorrelations().back()));
    juce::Thread::sleep(300);
    expectUnavailable(processor.getBandCorrelations());
    publishCorrelation(processor);
    EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
    processor.reset();
    expectUnavailable(processor.getBandCorrelations());
    processor.releaseResources();
}
}  // namespace esgrove::image_control::test
