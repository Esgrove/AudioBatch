/// Declares explicit local-history attachments for ImageControl sliders and processing toggles.
/// JUCE ParameterAttachment delivers host updates without recording them or running local editing callbacks.

#pragma once

#include "ui/ControlSlider.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace esgrove::image_control
{
class ParameterHistory;

/// Binds a slider to a parameter and records only local gestures, touching the parameter before each local write.
/// The APVTS, slider, and optional processor-owned history must outlive this attachment.
class HistorySliderAttachment final : private juce::Slider::Listener
{
public:
    HistorySliderAttachment(
        juce::AudioProcessorValueTreeState& parameters,
        const juce::String& parameterId,
        ControlSlider& control,
        std::optional<std::reference_wrapper<ParameterHistory>> history = std::nullopt
    );

    ~HistorySliderAttachment() override;

    HistorySliderAttachment(const HistorySliderAttachment&) = delete;
    HistorySliderAttachment(HistorySliderAttachment&&) = delete;
    HistorySliderAttachment& operator=(const HistorySliderAttachment&) = delete;
    HistorySliderAttachment& operator=(HistorySliderAttachment&&) = delete;

private:
    void setControlValue(float value);

    void sliderDragEnded(juce::Slider* changed) override;
    void sliderDragStarted(juce::Slider* changed) override;
    void sliderValueChanged(juce::Slider* changed) override;

    /// Processor-owned parameter, which outlives the attachment and control.
    juce::RangedAudioParameter& parameter;
    /// Control owned by the editor or band strip, destroyed after this attachment.
    ControlSlider& slider;
    /// Stable parameter identity used for explicit pre-write capture.
    juce::String id;
    /// Optional reference to processor-owned history, which outlives this attachment.
    std::optional<std::reference_wrapper<ParameterHistory>> history;
    /// JUCE handles only parameter delivery and balanced host notification, with no UndoManager.
    juce::ParameterAttachment attachment;
    /// Host-state generation when the current local gesture started.
    std::uint64_t gestureGeneration {0};
    /// Prevents parameter-originated UI synchronization from becoming a local edit.
    bool ignoreCallbacks {false};
    /// Whether this attachment has an open host gesture and a matching local history frame.
    bool gestureActive {false};
};

/// Binds a processing toggle to its parameter, recording each local click as one complete transaction.
/// The APVTS, button, and optional processor-owned history must outlive this attachment.
class HistoryButtonAttachment final : private juce::Button::Listener
{
public:
    HistoryButtonAttachment(
        juce::AudioProcessorValueTreeState& parameters,
        const juce::String& parameterId,
        juce::Button& control,
        std::optional<std::reference_wrapper<ParameterHistory>> history = std::nullopt
    );

    ~HistoryButtonAttachment() override;

    HistoryButtonAttachment(const HistoryButtonAttachment&) = delete;
    HistoryButtonAttachment(HistoryButtonAttachment&&) = delete;
    HistoryButtonAttachment& operator=(const HistoryButtonAttachment&) = delete;
    HistoryButtonAttachment& operator=(HistoryButtonAttachment&&) = delete;

private:
    void setControlValue(float value);

    void buttonClicked(juce::Button* changed) override;

    /// Editor-owned button, which outlives this attachment.
    juce::Button& button;
    /// Processor-owned parameter, which outlives this attachment.
    juce::RangedAudioParameter& parameter;
    /// Stable parameter identity used for explicit pre-write capture.
    juce::String id;
    /// Optional reference to processor-owned history, which outlives this attachment.
    std::optional<std::reference_wrapper<ParameterHistory>> history;
    /// Message-thread delivery of host values without any history listener.
    juce::ParameterAttachment attachment;
    /// Rejects callbacks caused by synchronizing a host value to the control.
    bool ignoreCallbacks {false};
};
}  // namespace esgrove::image_control
