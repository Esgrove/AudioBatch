/// Regression tests for the embedded Roboto UI fonts and the text geometry of the look-and-feel.
/// Checks inherited sans-serif resolution, the font sizes against the layout constants,
/// and the popup row heights, without painting or relying on platform-specific glyph rendering.

#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "TestSuites.h"

#include <memory>

namespace audiobatch::tests
{
/// Exercises app-owned font selection and text sizing through the look-and-feel.
class GuiFontTests final : public juce::UnitTest
{
public:
    /// Registers the suite for CTest discovery.
    GuiFontTests() : juce::UnitTest("GUI fonts", "AudioBatch") {}

    /// Checks face identities, font heights, and popup row heights.
    void runTest() override
    {
        CustomLookAndFeel lookAndFeel(true);
        const auto textTypeface = CustomLookAndFeel::getTextFont().getTypefacePtr();
        const auto monoTypeface = CustomLookAndFeel::getMonoBaseFont().getTypefacePtr();

        beginTest("Embedded Roboto faces resolve without system font installation");
        expectEquals(textTypeface->getName(), juce::String("Roboto"));
        expectEquals(monoTypeface->getName(), juce::String("Roboto Mono"));
        expect(lookAndFeel.getTypefaceForFont(juce::Font(juce::FontOptions {})) == textTypeface);

        beginTest("Column titles are one logical pixel larger than JUCE's proportional size and fit the header");
        for (const int height : {24, 28, 32}) {
            const auto font = CustomLookAndFeel::getTableHeaderFont(height);
            expect(font.getTypefacePtr() == textTypeface);
            expectWithinAbsoluteError(
                font.getHeight(), static_cast<float>(height) * layout::theme::tableHeaderFontHeightRatio + 1.0f, 0.001f
            );
            expect(font.getHeight() < static_cast<float>(height));
        }

        beginTest("Top-level menu labels and popup items use Roboto at their layout heights");
        juce::MenuBarComponent menuBar;
        const auto menuBarFont = lookAndFeel.getMenuBarFont(menuBar, 0, "File");
        expect(menuBarFont.getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(menuBarFont.getHeight(), layout::theme::menuBarFontHeight, 0.001f);
        const auto popupFont = lookAndFeel.getPopupMenuFont();
        expect(popupFont.getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(popupFont.getHeight(), layout::theme::popupMenuFontHeight, 0.001f);

        beginTest("Popup rows have extra vertical space without changing separators or explicit heights");
        juce::LookAndFeel_V4 juceLookAndFeel;
        int idealWidth = 0;
        int idealHeight = 0;
        lookAndFeel.getIdealPopupMenuItemSize("Process Selected", false, 0, idealWidth, idealHeight);
        expectEquals(idealHeight, layout::theme::popupMenuItemHeight);
        expect(static_cast<float>(idealHeight) > popupFont.getHeight());
        expect(idealWidth > 0);

        int juceWidth = 0;
        int juceHeight = 0;
        lookAndFeel.getIdealPopupMenuItemSize({}, true, 0, idealWidth, idealHeight);
        juceLookAndFeel.getIdealPopupMenuItemSize({}, true, 0, juceWidth, juceHeight);
        expectEquals(idealHeight, juceHeight);
        lookAndFeel.getIdealPopupMenuItemSize("Process Selected", false, 30, idealWidth, idealHeight);
        expectEquals(idealHeight, 30);

        beginTest("Button labels scale with the button height up to the maximum, and combo boxes use Roboto Mono");
        juce::TextButton button;
        juce::ComboBox comboBox;
        const auto shortButtonFont = lookAndFeel.getTextButtonFont(button, 20);
        expect(shortButtonFont.getTypefacePtr() == textTypeface);
        expectWithinAbsoluteError(shortButtonFont.getHeight(), 20.0f * layout::theme::buttonFontHeightRatio, 0.001f);
        expectWithinAbsoluteError(
            lookAndFeel.getTextButtonFont(button, 60).getHeight(), layout::theme::maximumButtonFontHeight, 0.001f
        );
        expect(lookAndFeel.getComboBoxFont(comboBox).getTypefacePtr() == monoTypeface);
    }
};

/// Creates the font suite without static font or test construction.
std::unique_ptr<juce::UnitTest> createGuiFontTests()
{
    return std::make_unique<GuiFontTests>();
}
}  // namespace audiobatch::tests
