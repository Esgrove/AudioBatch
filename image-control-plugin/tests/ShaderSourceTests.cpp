/// Tests that the renderer's embedded GLSL sources match their authoritative files and preserve explicit byte lengths.

#include "ImageControlBinaryData.h"
#include <string_view>

#include <gtest/gtest.h>

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <span>

namespace esgrove::image_control::test
{
namespace
{
/// A shader's source filename and its process-lifetime binary resource, excluding JUCE's terminator padding.
struct ShaderSource {
    /// Filename relative to the shader source directory supplied by CMake.
    const char* filename;
    /// Non-owning view of the bytes owned by ImageControlBinaryData for the process lifetime.
    std::span<const char> bytes;
};
}  // namespace

TEST(ShaderSources, EmbeddedBytesMatchAuthoritativeFiles)
{
    const std::array sources {
        ShaderSource {
            .filename="screen.vert",
            .bytes={ImageControlBinaryData::screen_vert, static_cast<std::size_t>(ImageControlBinaryData::screen_vertSize)},
        },
        ShaderSource {
            .filename="fade.frag",
            .bytes={ImageControlBinaryData::fade_frag, static_cast<std::size_t>(ImageControlBinaryData::fade_fragSize)},
        },
        ShaderSource {
            .filename="particle.vert",
            .bytes={ImageControlBinaryData::particle_vert, static_cast<std::size_t>(ImageControlBinaryData::particle_vertSize)},
        },
        ShaderSource {
            .filename="particle.frag",
            .bytes={ImageControlBinaryData::particle_frag, static_cast<std::size_t>(ImageControlBinaryData::particle_fragSize)},
        },
        ShaderSource {
            .filename="downsample.frag",
            .bytes={ImageControlBinaryData::downsample_frag,
             static_cast<std::size_t>(ImageControlBinaryData::downsample_fragSize),},
        },
        ShaderSource {
            .filename="upsample.frag",
            .bytes={ImageControlBinaryData::upsample_frag, static_cast<std::size_t>(ImageControlBinaryData::upsample_fragSize)},
        },
        ShaderSource {
            .filename="composite.frag",
            .bytes={ImageControlBinaryData::composite_frag,
             static_cast<std::size_t>(ImageControlBinaryData::composite_fragSize),},
        },
    };
    const juce::File sourceRoot(IMAGE_CONTROL_SHADER_SOURCE_ROOT);
    for (const auto& source : sources) {
        SCOPED_TRACE(source.filename);
        ASSERT_FALSE(source.bytes.empty());
        const std::string_view embedded(source.bytes.data(), source.bytes.size());
        EXPECT_NE(embedded.find("#version 330 core"), std::string_view::npos);
        EXPECT_EQ(embedded.find('\0'), std::string_view::npos);
        EXPECT_EQ(embedded.back(), '\n');

        juce::MemoryBlock diskBytes;
        ASSERT_TRUE(sourceRoot.getChildFile(source.filename).loadFileAsData(diskBytes));
        ASSERT_EQ(diskBytes.getSize(), source.bytes.size());
        EXPECT_EQ(std::string_view(static_cast<const char*>(diskBytes.getData()), diskBytes.getSize()), embedded);

        const auto text = juce::String::fromUTF8(source.bytes.data(), static_cast<int>(source.bytes.size()));
        EXPECT_EQ(text.getNumBytesAsUTF8(), source.bytes.size());
        EXPECT_EQ(std::string_view(text.toRawUTF8(), text.getNumBytesAsUTF8()), embedded);
    }
}
}  // namespace esgrove::image_control::test
