/// Top-level window of the GUI application.
/// Declares MainWindow, which hosts the AudioBatchComponent, installs the custom look-and-feel,
/// and provides the platform menu bar.

#pragma once

#include "AudioBatchComponent.h"
#include "CustomLookAndFeel.h"

#include <JuceHeader.h>

#include <memory>

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

    void showAndActivate();

    void closeButtonPressed() override;

private:
    juce::StringArray getMenuBarNames() override;

    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;

    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    ApplicationCommandTarget* getNextCommandTarget() override;

    void getAllCommands(juce::Array<juce::CommandID>& commands) override;

    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;

    bool perform(const InvocationInfo& info) override;

    void appendPluginOptionsMenuItems(juce::PopupMenu& menu) const;

    void handlePluginChoiceMenuItem(int menuItemID) const;

    AudioBatchComponent& getAudioBatch() const;

    /// Registry of the menu commands, used by the menu to look up and display their shortcuts.
    juce::ApplicationCommandManager commandManager;

    std::unique_ptr<juce::CustomLookAndFeel> lookAndFeel = std::make_unique<juce::CustomLookAndFeel>(true);

    JUCE_LEAK_DETECTOR(MainWindow)
};
