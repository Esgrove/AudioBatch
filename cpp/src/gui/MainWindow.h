/// Top-level window of the GUI application.
/// Declares MainWindow, which hosts the AudioBatchComponent, installs the custom look-and-feel,
/// and provides the platform menu bar.

#pragma once

#include "gui/AudioBatchComponent.h"
#include "gui/components/CustomLookAndFeel.h"

#include <JuceHeader.h>

#include <memory>

namespace audiobatch
{
/// Top-level application window that hosts the main AudioBatch component.
/// The menu items that have a keyboard shortcut are application commands,
/// because the menu only displays shortcuts for items that are backed by a command manager.
class MainWindow : public juce::DocumentWindow, juce::MenuBarModel, juce::ApplicationCommandTarget
{
public:
    explicit MainWindow(const juce::String& name);

    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    void closeButtonPressed() override;
    void openPaths(const juce::StringArray& paths) const;
    void showAndActivate();

private:
    ApplicationCommandTarget* getNextCommandTarget() override;
    bool perform(const InvocationInfo& info) override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    juce::StringArray getMenuBarNames() override;
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    AudioBatchComponent& getAudioBatch() const;
    void appendPluginOptionsMenuItems(juce::PopupMenu& menu);
    void handlePluginChoiceMenuItem(int menuItemID) const;

    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<CustomLookAndFeel> lookAndFeel {std::make_unique<CustomLookAndFeel>(true)};
    /// The plugins the Add Plugin submenu of the menu bar was last built from, in menu id order.
    juce::Array<juce::PluginDescription> pluginMenuTypes;

    /// The window content, shown without transferring ownership to the window.
    /// Declared after the look-and-feel it paints with, and destroyed explicitly before it in the destructor.
    std::unique_ptr<AudioBatchComponent> content;

    JUCE_LEAK_DETECTOR(MainWindow)
};
}  // namespace audiobatch
