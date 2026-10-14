#pragma once

#include "Theme.h"

namespace om
{

/** Styles every stock JUCE widget (menus, tables, scrollbars, text fields, combo
    boxes, tree views, device selector) with the OpenMix tokens. Custom controls
    (knobs, faders, meters, buttons) paint themselves from the same tokens. */
class LookAndFeel : public juce::LookAndFeel_V4,
                    private juce::ChangeListener
{
public:
    LookAndFeel();
    ~LookAndFeel() override;

    void refreshColours();

    juce::Font getPopupMenuFont() override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>&, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
    void getIdealPopupMenuItemSize (const juce::String&, bool isSeparator, int standardHeight, int& w, int& h) override;
    int  getPopupMenuBorderSize() override { return 4; }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h, bool ticked, bool enabled, bool over, bool down) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;

    void drawTableHeaderBackground (juce::Graphics&, juce::TableHeaderComponent&) override;
    void drawTableHeaderColumn (juce::Graphics&, juce::TableHeaderComponent&, const juce::String& name, int columnId,
                                int w, int h, bool over, bool down, int flags) override;

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override;
    int  getDefaultScrollbarWidth() override { return 10; }

    void drawTreeviewPlusMinusBox (juce::Graphics&, const juce::Rectangle<float>&, juce::Colour, bool isOpen, bool over) override;

    void drawTooltip (juce::Graphics&, const juce::String&, int w, int h) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String&, juce::Point<int>, juce::Rectangle<int>) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawStretchableLayoutResizerBar (juce::Graphics&, int w, int h, bool vertical, bool over, bool dragging) override;
    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image&) override;
    void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>&, juce::TextLayout&) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};

} // namespace om
