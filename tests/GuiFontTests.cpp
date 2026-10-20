/// Regression tests for the embedded Roboto UI fonts.
/// Checks inherited sans-serif resolution, popup measurement and painting,
/// and the increased column title size without relying on platform-specific glyph snapshots.

#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Exercises app-owned font selection through the actual look-and-feel drawing methods.
class GuiFontTests final : public juce::UnitTest
{
public:
    /// Registers the suite for CTest discovery.
    GuiFontTests() : juce::UnitTest("GUI fonts", "AudioBatch") {}

    /// Checks face identities and logical sizes on an offscreen graphics context.
    void runTest() override
    {
        juce::CustomLookAndFeel lookAndFeel(true);
        const auto textTypeface = juce::CustomLookAndFeel::getTextFont().getTypefacePtr();
        const auto monoTypeface = juce::CustomLookAndFeel::getMonoBaseFont().getTypefacePtr();

        beginTest("Embedded Roboto faces resolve without system font installation");
        expectEquals(textTypeface->getName(), juce::String("Roboto"));
        expectEquals(monoTypeface->getName(), juce::String("Roboto Mono"));
        expect(lookAndFeel.getTypefaceForFont(juce::Font(juce::FontOptions {})) == textTypeface);

        const juce::Image image(juce::Image::ARGB, 240, 40, true);
        juce::Graphics graphics(image);

        beginTest("Column titles use Roboto at half header height plus one logical pixel");
        juce::TableHeaderComponent header;
        header.setLookAndFeel(&lookAndFeel);
        for (const int height : {24, 28, 32}) {
            lookAndFeel.drawTableHeaderColumn(
                graphics, header, "True Peak", 1, 240, height, false, false, juce::TableHeaderComponent::sortedForwards
            );
            expectWithinAbsoluteError(
                graphics.getCurrentFont().getHeight(), static_cast<float>(height) * 0.5f + 1.0f, 0.001f
            );
            expect(graphics.getCurrentFont().getTypefacePtr() == textTypeface);
        }
        header.setLookAndFeel(nullptr);

        beginTest("Top-level menu labels use smaller Roboto text");
        juce::MenuBarComponent menuBar;
        const auto menuBarFont = lookAndFeel.getMenuBarFont(menuBar, 0, "File");
        expect(menuBarFont.getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(menuBarFont.getHeight(), 16.0f, 0.001f);

        beginTest("Popup measurement and painting share regular Roboto at the same height");
        const auto popupFont = lookAndFeel.getPopupMenuFont();
        expect(popupFont.getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(popupFont.getHeight(), layout::theme::popupMenuFontHeight, 0.001f);
        lookAndFeel.drawPopupMenuItem(
            graphics, {0, 0, 240, 30}, false, true, false, false, false, "Process Selected", {}, nullptr, nullptr
        );
        expect(graphics.getCurrentFont().getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(graphics.getCurrentFont().getHeight(), popupFont.getHeight(), 0.001f);

        beginTest("Popup rows have extra vertical space without changing separators or explicit heights");
        int idealWidth = 0;
        int idealHeight = 0;
        lookAndFeel.getIdealPopupMenuItemSize("Process Selected", false, 0, idealWidth, idealHeight);
        expectEquals(idealHeight, 28);
        expect(idealWidth > 0);
        lookAndFeel.getIdealPopupMenuItemSize({}, true, 0, idealWidth, idealHeight);
        expectEquals(idealHeight, 10);
        lookAndFeel.getIdealPopupMenuItemSize("Process Selected", false, 30, idealWidth, idealHeight);
        expectEquals(idealHeight, 30);

        beginTest("Buttons and combo boxes retain the existing Roboto font sizes");
        juce::TextButton button;
        juce::ComboBox comboBox;
        expect(lookAndFeel.getTextButtonFont(button, 30).getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(lookAndFeel.getTextButtonFont(button, 30).getHeight(), 16.0f, 0.001f);
        expect(lookAndFeel.getComboBoxFont(comboBox).getTypefacePtr() == monoTypeface);
    }
};

/// Creates the font suite without static font or test construction.
std::unique_ptr<juce::UnitTest> createGuiFontTests()
{
    return std::make_unique<GuiFontTests>();
}
}  // namespace audiobatch::tests
