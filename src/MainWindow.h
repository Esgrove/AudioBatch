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
    /// Builds the window, installs the custom look-and-feel as the global default,
    /// creates the owned AudioBatchComponent content, and configures the platform menu bar.
    /// The window is not made visible here, callers use showAndActivate for that.
    explicit MainWindow(const juce::String& name);

    /// Makes the window visible and pushes it to the front with keyboard focus.
    /// Safe to call repeatedly,
    /// the GUI startup path also invokes it again asynchronously to work around platform focus quirks on first launch.
    void showAndActivate();

    /// Detaches the menu bar model and clears the global look-and-feel
    /// before the owned look-and-feel object is destroyed.
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    /// Routes the window close button to a normal application quit request.
    void closeButtonPressed() override;

private:
    /// Returns the top-level menu names.
    /// On macOS the application menu is included as the first entry, other platforms omit it.
    juce::StringArray getMenuBarNames() override;

    /// Builds the popup menu for one top-level menu.
    /// The menu layout differs per platform because macOS moves About and Quit into the application menu.
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;

    /// Dispatches a chosen menu item to the matching AudioBatchComponent or plugin chain action.
    /// Unknown ids fall through to handlePluginChoiceMenuItem
    /// because the Add Plugin submenu generates its ids dynamically.
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    /// Returns nullptr, the window is the only command target and has no parent target to forward to.
    ApplicationCommandTarget* getNextCommandTarget() override;

    /// Lists the commands that appear in the menu bar with a keyboard shortcut.
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;

    /// Describes one command, including the default key presses the menu displays next to its item.
    /// The key presses are only displayed.
    /// The shortcuts themselves are handled by AudioBatchComponent::keyPressed.
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;

    /// Runs a command chosen from the menu.
    /// The menu also reports the choice through menuItemSelected,
    /// which ignores these commands so each choice runs once.
    bool perform(const InvocationInfo& info) override;

    /// Appends the plugin chain section into a menu, mirroring the popup attached to the Plugin toolbar button.
    void appendPluginOptionsMenuItems(juce::PopupMenu& menu);

    /// Decodes a menu result against the known plugin list and appends the chosen plugin to the chain.
    void handlePluginChoiceMenuItem(int menuItemID) const;

    /// Returns the owned content component as its concrete type.
    /// Valid for the whole window lifetime because the constructor always installs an AudioBatchComponent.
    AudioBatchComponent& getAudioBatch() const;

    /// Registry of the menu commands, used by the menu to look up and display their shortcuts.
    juce::ApplicationCommandManager commandManager;

    std::unique_ptr<juce::CustomLookAndFeel> lookAndFeel = std::make_unique<juce::CustomLookAndFeel>(true);

    JUCE_LEAK_DETECTOR(MainWindow)
};
