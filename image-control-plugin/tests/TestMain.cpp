/// Runs the ImageControl unit tests with GoogleTest inside an initialised JUCE environment,
/// so tests can create processors, value trees, and threads.

#include <gtest/gtest.h>

#include <juce_events/juce_events.h>

/// Initialises JUCE, then runs every registered test.
int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juce;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
