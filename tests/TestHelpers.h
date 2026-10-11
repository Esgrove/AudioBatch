/// Shared helpers for the AudioBatch tests.
/// Declares TemporaryDirectory, which owns a scratch folder that is deleted with it,
/// and the functions that locate the committed audio fixtures in tests/fixtures.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::tests
{
/// Peak level of the fixture tones, which have a peak amplitude of 0.5, in dBFS.
constexpr double fixturePeakDecibels = -6.0206;
/// Integrated loudness of a stereo fixture tone, in LUFS.
/// A 1 kHz sine on both channels measures the same as its peak level, as in EBU Tech 3341.
constexpr double fixtureStereoLoudness = fixturePeakDecibels;
/// Integrated loudness of a mono fixture tone, in LUFS, which is 3 dB below the stereo one.
constexpr double fixtureMonoLoudness = fixturePeakDecibels - 3.0103;

/// A uniquely named folder in the system temp directory that is deleted recursively on destruction.
class TemporaryDirectory
{
public:
    /// Creates the folder.
    TemporaryDirectory();

    /// Deletes the folder and everything in it.
    ~TemporaryDirectory();

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    /// Returns a file inside the folder, which does not need to exist yet.
    [[nodiscard]] juce::File getChildFile(const juce::String& name) const;

    /// Copies a fixture into the folder, optionally under a new name, and returns the copy.
    /// Tests that modify files work on copies so the committed fixtures never change.
    [[nodiscard]] juce::File copyFixture(const juce::String& fixtureName, const juce::String& copyName = {}) const;

private:
    juce::File directory;
};

/// Returns the committed fixture with the given file name from tests/fixtures.
juce::File fixtureFile(const juce::String& name);
}  // namespace audiobatch::tests
