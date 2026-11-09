/// Tests portable sound snapshots independently of host state, including strict XML rejection and scope boundaries.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>

namespace esgrove::image_control::test
{
TEST(ProcessingState, SoundScopeExcludesAuditionAndIncludesMonoAndOutput)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    const auto all = captureProcessingState(processor.getParameters(), false);
    EXPECT_EQ(sound.size(), 25U);
    EXPECT_EQ(all.size(), 31U);
    EXPECT_TRUE(sound.contains(MONO_ID));
    EXPECT_TRUE(sound.contains(OUTPUT_GAIN_ID));
    EXPECT_FALSE(sound.contains(BYPASS_ID));
    EXPECT_FALSE(sound.contains(DELTA_ID));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FALSE(sound.contains(soloParameterId(band)));
        EXPECT_TRUE(all.contains(soloParameterId(band)));
    }
}

TEST(ProcessingState, XmlRoundTripsEveryPlainValueExactly)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    const auto xml = processingStateToXml(sound);
    ASSERT_TRUE(xml->hasTagName("SoundState"));
    EXPECT_EQ(xml->getStringAttribute("plugin"), "com.esgrove.imagecontrol");
    const auto parsed = processingStateFromXml(*xml, processor.getParameters());
    ASSERT_TRUE(parsed.has_value());
    if (parsed.has_value()) {
        EXPECT_TRUE(sameProcessingState(sound, *parsed));
    }
}

TEST(ProcessingState, RejectsValuesBeforeRoundingAndFractionalBooleanStates)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    const auto xml = processingStateToXml(sound);
    auto* output = xml->getChildByAttribute("id", OUTPUT_GAIN_ID);
    ASSERT_NE(output, nullptr);
    output->setAttribute("value", "12.0000000001");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    output->setAttribute("value", "-24.0000000001");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    output->setAttribute("value", "0");
    auto* mono = xml->getChildByAttribute("id", MONO_ID);
    ASSERT_NE(mono, nullptr);
    mono->setAttribute("value", "0.5");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
}

TEST(ProcessingState, RejectsIncompleteUnknownDuplicateNonFiniteAndOutOfRangeData)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    for (const auto* invalid : {"nan", "inf", "-inf", "1garbage", "100000", "-100000", ""}) {
        const auto xml = processingStateToXml(sound);
        xml->getFirstChildElement()->setAttribute("value", invalid);
        EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    }
    auto xml = processingStateToXml(sound);
    xml->getFirstChildElement()->setAttribute("id", "unknown");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->addChildElement(new juce::XmlElement(*xml->getFirstChildElement()));
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->removeChildElement(xml->getFirstChildElement(), true);
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->setAttribute("version", 2);
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->setAttribute("plugin", "other.plugin");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    auto invalid = sound;
    invalid.at(OUTPUT_GAIN_ID) = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(processor.loadSoundSnapshot(invalid, "Invalid"));
}
}  // namespace esgrove::image_control::test
