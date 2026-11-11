/// Shares plain parameter access and bounded host-block rendering for processor and persistence tests.

#pragma once

#include "ImageControlProcessor.h"

#include <gtest/gtest.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>

namespace esgrove::image_control::test
{
/// Sets a parameter of a processor to a plain value.
inline void setParameter(ImageControlProcessor& processor, const juce::String& parameterId, const float value)
{
    auto* parameter = processor.getParameters().getParameter(parameterId);
    ASSERT_NE(parameter, nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

/// Returns the plain value of a parameter of a processor.
inline float getParameter(ImageControlProcessor& processor, const juce::String& parameterId)
{
    const auto* parameter = processor.getParameters().getParameter(parameterId);
    return parameter->convertFrom0to1(parameter->getValue());
}

/// Renders an existing buffer in bounded irregular host blocks, with no editor required.
inline void renderBuffer(ImageControlProcessor& processor, juce::AudioBuffer<float>& buffer, const int blockSize)
{
    juce::MidiBuffer midi;
    for (int start = 0; start < buffer.getNumSamples(); start += blockSize) {
        juce::AudioBuffer<float> block(
            buffer.getArrayOfWritePointers(),
            buffer.getNumChannels(),
            start,
            std::min(blockSize, buffer.getNumSamples() - start)
        );
        processor.processBlock(block, midi);
    }
}
}  // namespace esgrove::image_control::test
