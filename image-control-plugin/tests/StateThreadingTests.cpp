/// Tests host state callbacks against message-thread editor edits.
/// Detached snapshots must save immediately, queued restores must retain tree identity,
/// and no bound ValueTree listener may run on a host worker.

#include "ImageControlProcessor.h"
#include "Parameters.h"

#include <gtest/gtest.h>

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <utility>

namespace esgrove::image_control::test
{
namespace
{
/// Records dirty notifications and the thread used for bound editor-tree notifications.
class ThreadingStateListener final : public juce::AudioProcessorListener, public juce::ValueTree::Listener
{
public:
    /// Parameter automation does not affect this listener's editor-state assertions.
    void audioProcessorParameterChanged(
        juce::AudioProcessor* /*processor*/,
        int /*parameterIndex*/,
        float /*value*/
    ) override
    {}

    /// Counts only non-parameter dirty changes and verifies that they remain message-thread-owned.
    void audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged) {
            dirtyChanges.fetch_add(1);
            recordThread();
        }
    }

    /// Observes all property callbacks, including the callbacks caused by restores.
    void valueTreePropertyChanged(juce::ValueTree& /*tree*/, const juce::Identifier& /*property*/) override
    {
        treeChanges.fetch_add(1);
        recordThread();
    }

    /// Number of dirty notifications sent to the host.
    std::atomic<int> dirtyChanges {0};
    /// Number of property notifications sent to live tree bindings.
    std::atomic<int> treeChanges {0};
    /// Whether any live editor callback ran on a host worker.
    std::atomic<bool> workerNotification {false};

private:
    /// Publishes a failure without using GoogleTest from a callback.
    void recordThread()
    {
        if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
            workerNotification.store(true);
        }
    }
};

/// Requests a host save from the synchronous APVTS notification sent while a restore still holds its locks.
class ReentrantSaveListener final : public juce::AudioProcessorValueTreeState::Listener
{
public:
    /// Runs the save once even if copying parameter state causes another parameter notification.
    void parameterChanged(const juce::String& /*parameterId*/, float /*value*/) override
    {
        if (const auto action = std::exchange(save, {})) {
            ++saves;
            action();
        }
    }

    /// Save request installed before the restore and invoked on the restoring thread.
    std::function<void()> save;
    /// Number of reentrant host saves actually requested.
    int saves {0};
};

/// Requests another restore from a notification delivered to an existing editor-tree binding.
class ReentrantRestoreListener final : public juce::ValueTree::Listener
{
public:
    /// Restores once when the outer copy reaches the editor size, without depending on other property ordering.
    void valueTreePropertyChanged(juce::ValueTree& /*tree*/, const juce::Identifier& property) override
    {
        if (property == juce::Identifier(EDITOR_WIDTH_PROPERTY)) {
            if (const auto action = std::exchange(restore, {})) {
                ++restores;
                action();
            }
        }
    }

    /// Nested restore installed before the outer restore begins.
    std::function<void()> restore;
    /// Number of nested restores actually requested.
    int restores {0};
};

/// Saves independent settings so test workers never access a live source tree.
juce::MemoryBlock makeThreadingState(const int width, const bool output)
{
    ImageControlProcessor source;
    source.getEditorState().setProperty(EDITOR_WIDTH_PROPERTY, width, nullptr);
    source.getEditorState().setProperty(EDITOR_HEIGHT_PROPERTY, width - 300, nullptr);
    source.getEditorState().setProperty(ANALYZE_OUTPUT_PROPERTY, output, nullptr);
    source.getEditorState().setProperty(PERSISTENCE_PROPERTY, 2.5, nullptr);
    auto* gain = source.getParameters().getParameter(OUTPUT_GAIN_ID);
    gain->setValueNotifyingHost(gain->convertTo0to1(-6.0F));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        auto& bandWidth = *source.getParameters().getParameter(widthParameterId(band));
        auto& recovery = *source.getParameters().getParameter(recoverParameterId(band));
        bandWidth.setValueNotifyingHost(bandWidth.convertTo0to1(output ? 0.75F : 0.25F));
        recovery.setValueNotifyingHost(recovery.convertTo0to1(output ? 25.0F : 75.0F));
    }
    juce::MemoryBlock state;
    source.getStateInformation(state);
    return state;
}

/// Decodes a host snapshot without touching the processor's message-thread tree.
std::unique_ptr<juce::XmlElement> savedThreadingState(ImageControlProcessor& processor)
{
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    return juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
}

/// Checks detached editor settings and all restored imaging controls after a reentrant callback.
void expectThreadingSnapshot(const juce::XmlElement& snapshot, const int width, const bool output)
{
    const auto* editor = snapshot.getChildByName("EditorState");
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getIntAttribute(EDITOR_WIDTH_PROPERTY), width);
    EXPECT_EQ(editor->getIntAttribute(EDITOR_HEIGHT_PROPERTY), width - 300);
    EXPECT_EQ(editor->getBoolAttribute(ANALYZE_OUTPUT_PROPERTY), output);
    EXPECT_DOUBLE_EQ(editor->getDoubleAttribute(PERSISTENCE_PROPERTY), 2.5);
    const auto* gain = snapshot.getChildByAttribute("id", OUTPUT_GAIN_ID);
    ASSERT_NE(gain, nullptr);
    EXPECT_NEAR(gain->getDoubleAttribute("value"), -6.0, 0.01);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const auto* bandWidth = snapshot.getChildByAttribute("id", widthParameterId(band));
        const auto* recovery = snapshot.getChildByAttribute("id", recoverParameterId(band));
        ASSERT_NE(bandWidth, nullptr);
        ASSERT_NE(recovery, nullptr);
        EXPECT_NEAR(bandWidth->getDoubleAttribute("value"), output ? 0.75 : 0.25, 0.001);
        EXPECT_NEAR(recovery->getDoubleAttribute("value"), output ? 25.0 : 75.0, 0.001);
    }
}
}  // namespace

TEST(StateThreading, WorkerRestorePublishesSnapshotAndTapBeforeMessageDispatch)
{
    const auto state = makeThreadingState(1400, false);
    ImageControlProcessor processor;
    auto bound = processor.getEditorState();
    ThreadingStateListener listener;
    bound.addListener(&listener);
    processor.addListener(&listener);
    const auto previousEpoch = processor.getAnalyzerFifo().getEpoch();

    // Joining on the message thread proves the host worker never waits for message dispatch.
    std::thread worker([&] {
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const auto saved = savedThreadingState(processor);
        ASSERT_NE(saved, nullptr);
        const auto* editor = saved->getChildByName("EditorState");
        ASSERT_NE(editor, nullptr);
        EXPECT_EQ(editor->getIntAttribute(EDITOR_WIDTH_PROPERTY), 1400);
        EXPECT_FALSE(editor->getBoolAttribute(ANALYZE_OUTPUT_PROPERTY));
        const auto* gain = saved->getChildByAttribute("id", OUTPUT_GAIN_ID);
        ASSERT_NE(gain, nullptr);
        EXPECT_NEAR(gain->getDoubleAttribute("value"), -6.0, 0.01);
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            const auto* width = saved->getChildByAttribute("id", widthParameterId(band));
            const auto* recovery = saved->getChildByAttribute("id", recoverParameterId(band));
            ASSERT_NE(width, nullptr);
            ASSERT_NE(recovery, nullptr);
            EXPECT_NEAR(width->getDoubleAttribute("value"), 0.25, 0.001);
            EXPECT_NEAR(recovery->getDoubleAttribute("value"), 75.0, 0.001);
        }
        EXPECT_FALSE(StereoSampleFifo::analyzesOutput(processor.getAnalyzerFifo().getEpoch()));
    });
    worker.join();

    EXPECT_EQ(listener.treeChanges.load(), 0);
    EXPECT_FALSE(bound.hasProperty(EDITOR_WIDTH_PROPERTY));
    EXPECT_NE(processor.getAnalyzerFifo().getEpoch(), previousEpoch);
    EXPECT_EQ(processor.getEditorState(), bound);
    EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_WIDTH_PROPERTY)), 1400);
    EXPECT_GT(listener.treeChanges.load(), 0);
    EXPECT_EQ(listener.dirtyChanges.load(), 0);
    EXPECT_FALSE(listener.workerNotification.load());
    processor.removeListener(&listener);
    bound.removeListener(&listener);
}

TEST(StateThreading, LaterBoundPropertyEditsOverrideOnlyThatPartOfQueuedRestore)
{
    const auto state = makeThreadingState(1400, false);
    ImageControlProcessor processor;
    auto bound = processor.getEditorState();
    ThreadingStateListener listener;
    processor.addListener(&listener);
    std::thread restore([&] { processor.setStateInformation(state.getData(), static_cast<int>(state.getSize())); });
    restore.join();

    // Existing bindings can edit without going through getEditorState and flushing the pending restore first.
    bound.setProperty(PERSISTENCE_PROPERTY, 4.0, nullptr);
    bound.setProperty(SHOW_FPS_PROPERTY, true, nullptr);
    bound.setProperty(ANALYZE_OUTPUT_PROPERTY, false, nullptr);
    bound.setProperty(ANALYZE_OUTPUT_PROPERTY, true, nullptr);
    EXPECT_EQ(listener.dirtyChanges.load(), 4);
    EXPECT_TRUE(StereoSampleFifo::analyzesOutput(processor.getAnalyzerFifo().getEpoch()));
    const auto snapshot = savedThreadingState(processor);
    ASSERT_NE(snapshot, nullptr);
    const auto* editor = snapshot->getChildByName("EditorState");
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getIntAttribute(EDITOR_WIDTH_PROPERTY), 1400);
    EXPECT_DOUBLE_EQ(editor->getDoubleAttribute(PERSISTENCE_PROPERTY), 4.0);
    EXPECT_TRUE(editor->getBoolAttribute(SHOW_FPS_PROPERTY));
    EXPECT_TRUE(editor->getBoolAttribute(ANALYZE_OUTPUT_PROPERTY));

    EXPECT_EQ(processor.getEditorState(), bound);
    EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_WIDTH_PROPERTY)), 1400);
    EXPECT_DOUBLE_EQ(static_cast<double>(bound.getProperty(PERSISTENCE_PROPERTY)), 4.0);
    EXPECT_TRUE(static_cast<bool>(bound.getProperty(SHOW_FPS_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(bound.getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_EQ(listener.dirtyChanges.load(), 4);
    processor.removeListener(&listener);
}

TEST(StateThreading, LatestRestoreWinsAndMissingSettingsUseDefaultsBeforeDispatch)
{
    const auto first = makeThreadingState(1400, false);
    auto latest = makeThreadingState(1600, true);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(latest.getData(), static_cast<int>(latest.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* editor = xml->getChildByName("EditorState");
    ASSERT_NE(editor, nullptr);
    editor->removeAttribute(EDITOR_WIDTH_PROPERTY);
    editor->removeAttribute(PERSISTENCE_PROPERTY);
    juce::AudioProcessor::copyXmlToBinary(*xml, latest);

    ImageControlProcessor processor;
    auto bound = processor.getEditorState();
    bound.setProperty(PERSISTENCE_PROPERTY, 6.0, nullptr);
    std::thread worker([&] {
        processor.setStateInformation(first.getData(), static_cast<int>(first.getSize()));
        processor.setStateInformation(latest.getData(), static_cast<int>(latest.getSize()));
    });
    worker.join();
    const auto saved = savedThreadingState(processor);
    ASSERT_NE(saved, nullptr);
    editor = saved->getChildByName("EditorState");
    ASSERT_NE(editor, nullptr);
    EXPECT_FALSE(editor->hasAttribute(EDITOR_WIDTH_PROPERTY));
    EXPECT_DOUBLE_EQ(editor->getDoubleAttribute(PERSISTENCE_PROPERTY), DEFAULT_PERSISTENCE_SECONDS);
    EXPECT_TRUE(StereoSampleFifo::analyzesOutput(processor.getAnalyzerFifo().getEpoch()));

    EXPECT_EQ(processor.getEditorState(), bound);
    EXPECT_FALSE(bound.hasProperty(EDITOR_WIDTH_PROPERTY));
    EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_HEIGHT_PROPERTY)), 1300);
    EXPECT_DOUBLE_EQ(static_cast<double>(bound.getProperty(PERSISTENCE_PROPERTY)), DEFAULT_PERSISTENCE_SECONDS);
}

TEST(StateThreading, MessageThreadRestoreAppliesSynchronouslyWithoutRebinding)
{
    const auto state = makeThreadingState(1400, false);
    ImageControlProcessor processor;
    const auto bound = processor.getEditorState();
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    // Read the pre-existing binding, not the accessor that also flushes async work.
    EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_WIDTH_PROPERTY)), 1400);
    EXPECT_FALSE(static_cast<bool>(bound.getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_EQ(processor.getEditorState(), bound);
}

TEST(StateThreading, ParameterListenerCanSaveReentrantlyDuringMessageOrWorkerRestore)
{
    const auto state = makeThreadingState(1400, false);
    for (const bool fromWorker : {false, true}) {
        SCOPED_TRACE(fromWorker);
        ImageControlProcessor processor;
        auto bound = processor.getEditorState();
        ThreadingStateListener listener;
        bound.addListener(&listener);
        processor.addListener(&listener);
        const auto previousEpoch = processor.getAnalyzerFifo().getEpoch();
        std::unique_ptr<juce::XmlElement> reentrantSnapshot;
        bool callbackOnMessageThread = false;
        bool callbackTapOutput = true;
        ReentrantSaveListener saveListener;
        saveListener.save = [&] {
            callbackOnMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
            reentrantSnapshot = savedThreadingState(processor);
            callbackTapOutput = StereoSampleFifo::analyzesOutput(processor.getAnalyzerFifo().getEpoch());
        };
        processor.getParameters().addParameterListener(OUTPUT_GAIN_ID, &saveListener);
        if (fromWorker) {
            // Joining without dispatch ensures the reentrant save does not need a message-thread round trip.
            std::thread worker([&] {
                processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            });
            worker.join();
            EXPECT_EQ(listener.treeChanges.load(), 0);
            EXPECT_FALSE(bound.hasProperty(EDITOR_WIDTH_PROPERTY));
        } else {
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        processor.getParameters().removeParameterListener(OUTPUT_GAIN_ID, &saveListener);
        EXPECT_EQ(saveListener.saves, 1);
        EXPECT_EQ(callbackOnMessageThread, !fromWorker);
        EXPECT_FALSE(callbackTapOutput);
        EXPECT_NE(processor.getAnalyzerFifo().getEpoch(), previousEpoch);
        EXPECT_EQ(processor.getEditorState(), bound);
        EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_WIDTH_PROPERTY)), 1400);
        EXPECT_FALSE(static_cast<bool>(bound.getProperty(ANALYZE_OUTPUT_PROPERTY)));
        EXPECT_GT(listener.treeChanges.load(), 0);
        EXPECT_EQ(listener.dirtyChanges.load(), 0);
        EXPECT_FALSE(listener.workerNotification.load());
        processor.removeListener(&listener);
        bound.removeListener(&listener);

        ASSERT_NE(reentrantSnapshot, nullptr);
        expectThreadingSnapshot(*reentrantSnapshot, 1400, false);
        const auto latest = savedThreadingState(processor);
        ASSERT_NE(latest, nullptr);
        expectThreadingSnapshot(*latest, 1400, false);
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            EXPECT_NEAR(processor.getParameters().getRawParameterValue(widthParameterId(band))->load(), 0.25F, 0.001F);
            EXPECT_NEAR(
                processor.getParameters().getRawParameterValue(recoverParameterId(band))->load(), 75.0F, 0.001F
            );
        }
    }
}

TEST(StateThreading, EditorTreeListenerCanRestoreAgainWithoutOverwritingLatestSnapshotOrTap)
{
    const auto first = makeThreadingState(1400, false);
    const auto second = makeThreadingState(1600, true);
    ImageControlProcessor processor;
    auto bound = processor.getEditorState();
    ThreadingStateListener listener;
    bound.addListener(&listener);
    processor.addListener(&listener);
    std::unique_ptr<juce::XmlElement> nestedSnapshot;
    bool nestedIdentityPreserved = false;
    bool nestedTapOutput = false;
    ReentrantRestoreListener restoreListener;
    restoreListener.restore = [&] {
        processor.setStateInformation(second.getData(), static_cast<int>(second.getSize()));
        nestedIdentityPreserved = processor.getEditorState() == bound;
        nestedSnapshot = savedThreadingState(processor);
        nestedTapOutput = StereoSampleFifo::analyzesOutput(processor.getAnalyzerFifo().getEpoch());
    };
    bound.addListener(&restoreListener);
    processor.setStateInformation(first.getData(), static_cast<int>(first.getSize()));
    bound.removeListener(&restoreListener);
    EXPECT_EQ(restoreListener.restores, 1);
    EXPECT_TRUE(nestedIdentityPreserved);
    EXPECT_TRUE(nestedTapOutput);
    // Finishing the outer tree copy must not republish its stale snapshot or analyzer tap.
    const auto afterOuterCopy = savedThreadingState(processor);
    EXPECT_TRUE(StereoSampleFifo::analyzesOutput(processor.getAnalyzerFifo().getEpoch()));
    EXPECT_EQ(processor.getEditorState(), bound);
    EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_WIDTH_PROPERTY)), 1600);
    EXPECT_EQ(static_cast<int>(bound.getProperty(EDITOR_HEIGHT_PROPERTY)), 1300);
    EXPECT_TRUE(static_cast<bool>(bound.getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_GT(listener.treeChanges.load(), 0);
    EXPECT_EQ(listener.dirtyChanges.load(), 0);
    EXPECT_FALSE(listener.workerNotification.load());
    processor.removeListener(&listener);
    bound.removeListener(&listener);

    ASSERT_NE(nestedSnapshot, nullptr);
    expectThreadingSnapshot(*nestedSnapshot, 1600, true);
    ASSERT_NE(afterOuterCopy, nullptr);
    expectThreadingSnapshot(*afterOuterCopy, 1600, true);
    const auto latest = savedThreadingState(processor);
    ASSERT_NE(latest, nullptr);
    expectThreadingSnapshot(*latest, 1600, true);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_NEAR(processor.getParameters().getRawParameterValue(widthParameterId(band))->load(), 0.75F, 0.001F);
        EXPECT_NEAR(processor.getParameters().getRawParameterValue(recoverParameterId(band))->load(), 25.0F, 0.001F);
    }
}

TEST(StateThreading, ConcurrentHostSavesAndRestoresDoNotTouchBoundEditorTree)
{
    const auto first = makeThreadingState(1400, false);
    const auto second = makeThreadingState(1600, true);
    ImageControlProcessor processor;
    auto bound = processor.getEditorState();
    ThreadingStateListener listener;
    bound.addListener(&listener);
    processor.addListener(&listener);
    // Give every observable snapshot the same width/height invariant, including the initial one.
    processor.setStateInformation(first.getData(), static_cast<int>(first.getSize()));
    std::latch start(3);
    std::thread restores([&] {
        start.arrive_and_wait();
        for (int iteration = 0; iteration < 100; ++iteration) {
            const auto& state = iteration % 2 == 0 ? first : second;
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
    });
    std::thread saves([&] {
        start.arrive_and_wait();
        for (int iteration = 0; iteration < 100; ++iteration) {
            const auto saved = savedThreadingState(processor);
            ASSERT_NE(saved, nullptr);
            const auto* editor = saved->getChildByName("EditorState");
            ASSERT_NE(editor, nullptr);
            const int width = editor->getIntAttribute(EDITOR_WIDTH_PROPERTY);
            EXPECT_TRUE(width == 1400 || width == 1600);
            EXPECT_EQ(editor->getIntAttribute(EDITOR_HEIGHT_PROPERTY), width - 300);
            EXPECT_EQ(editor->getBoolAttribute(ANALYZE_OUTPUT_PROPERTY), width == 1600);
            for (std::size_t band = 0; band < BAND_COUNT; ++band) {
                const auto* bandWidth = saved->getChildByAttribute("id", widthParameterId(band));
                const auto* recovery = saved->getChildByAttribute("id", recoverParameterId(band));
                ASSERT_NE(bandWidth, nullptr);
                ASSERT_NE(recovery, nullptr);
                EXPECT_NEAR(bandWidth->getDoubleAttribute("value"), width == 1600 ? 0.75 : 0.25, 0.001);
                EXPECT_NEAR(recovery->getDoubleAttribute("value"), width == 1600 ? 25.0 : 75.0, 0.001);
            }
        }
    });
    start.arrive_and_wait();
    for (int iteration = 0; iteration < 100; ++iteration) {
        bound.setProperty(CONTRAST_PROPERTY, iteration + 1, nullptr);
        // Exercise message-thread copies while workers repeatedly replace and save their plain snapshots.
        const auto copy = processor.getEditorState().createCopy();
        EXPECT_TRUE(copy.isValid());
    }
    restores.join();
    saves.join();
    processor.getEditorState().setProperty(CONTRAST_PROPERTY, 99, nullptr);
    EXPECT_EQ(static_cast<int>(bound.getProperty(CONTRAST_PROPERTY)), 99);
    EXPECT_FALSE(listener.workerNotification.load());
    EXPECT_GE(listener.dirtyChanges.load(), 100);
    const auto final = savedThreadingState(processor);
    ASSERT_NE(final, nullptr);
    const auto* editor = final->getChildByName("EditorState");
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getIntAttribute(CONTRAST_PROPERTY), 99);
    EXPECT_EQ(editor->getIntAttribute(EDITOR_WIDTH_PROPERTY), 1600);
    EXPECT_EQ(processor.getEditorState(), bound);
    processor.removeListener(&listener);
    bound.removeListener(&listener);
}
}  // namespace esgrove::image_control::test
