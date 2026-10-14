#include "LookAndFeel.h"
#include "Icons.h"

namespace om
{

LookAndFeel::LookAndFeel()
{
    setDefaultSansSerifTypefaceName (Theme::get().fontFamily());
    refreshColours();
    Theme::get().addChangeListener (this);
}

LookAndFeel::~LookAndFeel()
{
    Theme::get().removeChangeListener (this);
}

void LookAndFeel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshColours();
}

void LookAndFeel::refreshColours()
{
    using C = Col;
    const auto acc = accent();

    getCurrentColourScheme().setUIColour (ColourScheme::windowBackground,   colour (C::bgApp));
    getCurrentColourScheme().setUIColour (ColourScheme::widgetBackground,   colour (C::bgControl));
    getCurrentColourScheme().setUIColour (ColourScheme::menuBackground,     colour (C::bgMenu));
    getCurrentColourScheme().setUIColour (ColourScheme::outline,            colour (C::lineStrong));
    getCurrentColourScheme().setUIColour (ColourScheme::defaultText,        colour (C::textPrimary));
    getCurrentColourScheme().setUIColour (ColourScheme::defaultFill,        acc);
    getCurrentColourScheme().setUIColour (ColourScheme::highlightedText,    colour (C::textPrimary));
    getCurrentColourScheme().setUIColour (ColourScheme::highlightedFill,    colour (C::bgControlHover));
    getCurrentColourScheme().setUIColour (ColourScheme::menuText,           colour (C::textPrimary));
    setColourScheme (getCurrentColourScheme());

    setColour (juce::ResizableWindow::backgroundColourId, colour (C::bgApp));
    setColour (juce::DocumentWindow::textColourId, colour (C::textSecondary));

    setColour (juce::PopupMenu::backgroundColourId, colour (C::bgMenu));
    setColour (juce::PopupMenu::textColourId, colour (C::textPrimary));
    setColour (juce::PopupMenu::headerTextColourId, colour (C::textTertiary));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colour (C::bgControlHover));
    setColour (juce::PopupMenu::highlightedTextColourId, colour (C::textPrimary));

    setColour (juce::TextEditor::backgroundColourId, colour (C::bgField));
    setColour (juce::TextEditor::textColourId, colour (C::textPrimary));
    setColour (juce::TextEditor::highlightColourId, acc.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, colour (C::textPrimary));
    setColour (juce::TextEditor::outlineColourId, colour (C::line));
    setColour (juce::TextEditor::focusedOutlineColourId, acc.withAlpha (0.7f));
    setColour (juce::CaretComponent::caretColourId, acc);

    setColour (juce::Label::textColourId, colour (C::textPrimary));
    setColour (juce::Label::textWhenEditingColourId, colour (C::textPrimary));
    setColour (juce::Label::backgroundWhenEditingColourId, colour (C::bgField));
    setColour (juce::Label::outlineWhenEditingColourId, acc);

    setColour (juce::ComboBox::backgroundColourId, colour (C::bgField));
    setColour (juce::ComboBox::textColourId, colour (C::textPrimary));
    setColour (juce::ComboBox::outlineColourId, colour (C::line));
    setColour (juce::ComboBox::arrowColourId, colour (C::textSecondary));
    setColour (juce::ComboBox::focusedOutlineColourId, acc);

    setColour (juce::TextButton::buttonColourId, colour (C::bgControl));
    setColour (juce::TextButton::buttonOnColourId, acc);
    setColour (juce::TextButton::textColourOffId, colour (C::textPrimary));
    setColour (juce::TextButton::textColourOnId, colour (C::textOnAccent));
    setColour (juce::ToggleButton::textColourId, colour (C::textPrimary));
    setColour (juce::ToggleButton::tickColourId, acc);
    setColour (juce::ToggleButton::tickDisabledColourId, colour (C::textTertiary));

    setColour (juce::ListBox::backgroundColourId, colour (C::bgPanel));
    setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ListBox::textColourId, colour (C::textPrimary));

    setColour (juce::TableHeaderComponent::backgroundColourId, colour (C::bgRaised));
    setColour (juce::TableHeaderComponent::textColourId, colour (C::textSecondary));
    setColour (juce::TableHeaderComponent::outlineColourId, colour (C::line));
    setColour (juce::TableHeaderComponent::highlightColourId, colour (C::bgControl));

    setColour (juce::TreeView::backgroundColourId, colour (C::bgPanel));
    setColour (juce::TreeView::linesColourId, colour (C::line));
    setColour (juce::TreeView::selectedItemBackgroundColourId, colour (C::bgControl));

    setColour (juce::ScrollBar::thumbColourId, colour (C::lineStrong));
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    setColour (juce::TooltipWindow::backgroundColourId, colour (C::bgMenu));
    setColour (juce::TooltipWindow::textColourId, colour (C::textPrimary));
    setColour (juce::TooltipWindow::outlineColourId, colour (C::lineStrong));

    setColour (juce::Slider::backgroundColourId, colour (C::bgField));
    setColour (juce::Slider::trackColourId, acc);
    setColour (juce::Slider::thumbColourId, colour (C::textPrimary));
    setColour (juce::Slider::textBoxTextColourId, colour (C::textPrimary));
    setColour (juce::Slider::textBoxBackgroundColourId, colour (C::bgField));
    setColour (juce::Slider::textBoxOutlineColourId, colour (C::line));

    setColour (juce::AlertWindow::backgroundColourId, colour (C::bgMenu));
    setColour (juce::AlertWindow::textColourId, colour (C::textPrimary));
    setColour (juce::AlertWindow::outlineColourId, colour (C::lineStrong));

    setColour (juce::GroupComponent::outlineColourId, colour (C::line));
    setColour (juce::GroupComponent::textColourId, colour (C::textSecondary));

    setColour (juce::ListBox::backgroundColourId, colour (C::bgField));
}

//==============================================================================
juce::Font LookAndFeel::getPopupMenuFont()                    { return font (Type::body); }
juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox&)     { return font (Type::body); }
juce::Font LookAndFeel::getLabelFont (juce::Label& l)         { return l.getFont().getTypefaceName() == juce::Font::getDefaultSansSerifFontName() ? font (Type::body) : l.getFont(); }
juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int) { return font (Type::body, Weight::medium); }

//==============================================================================
void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (colour (Col::bgMenu));
    g.setColour (colour (Col::lineStrong));
    g.drawRect (0, 0, w, h, 1);
}

void LookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& w, int& h)
{
    if (isSeparator)
    {
        w = 50;
        h = 9;
        return;
    }
    h = 24;
    w = (int) juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), text) + 56;
}

void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                     bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                     const juce::String& shortcutKeyText, const juce::Drawable*, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (colour (Col::line));
        g.fillRect (area.reduced (8, 0).withHeight (1).withCentre (area.getCentre()));
        return;
    }

    auto r = area.reduced (3, 1).toFloat();
    if (isHighlighted && isActive)
    {
        g.setColour (colour (Col::bgControlHover));
        g.fillRoundedRectangle (r, Radius::sm);
    }

    const auto tc = ! isActive ? colour (Col::textDisabled)
                  : textColour != nullptr ? *textColour : colour (Col::textPrimary);

    auto content = r.reduced (8, 0);
    auto tickArea = content.removeFromLeft (16);
    if (isTicked)
        Icons::draw (g, Icon::check, tickArea.withSizeKeepingCentre (12, 12), accent());

    content.removeFromLeft (4);
    if (hasSubMenu)
        Icons::draw (g, Icon::chevronRight, content.removeFromRight (12).withSizeKeepingCentre (10, 10), colour (Col::textSecondary));

    if (shortcutKeyText.isNotEmpty())
        Text::draw (g, shortcutKeyText, content.removeFromRight (70), font (Type::label), colour (Col::textTertiary),
                    juce::Justification::centredRight);

    Text::draw (g, text, content, getPopupMenuFont(), tc);
}

void LookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    Text::caption (g, name, area.toFloat().reduced (12, 0), colour (Col::textTertiary), juce::Justification::bottomLeft);
}

//==============================================================================
void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    auto fill = on ? accent() : colour (down ? Col::bgControlHover : over ? Col::bgControlHover : Col::bgControl);
    if (! b.isEnabled()) fill = colour (Col::bgRaised);
    g.setColour (fill);
    g.fillRoundedRectangle (r, Radius::md);
    g.setColour (colour (Col::lineStrong).withAlpha (on ? 0.0f : 1.0f));
    g.drawRoundedRectangle (r, Radius::md, 1.0f);
}

void LookAndFeel::drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h, bool ticked,
                               bool enabled, bool over, bool)
{
    auto r = juce::Rectangle<float> (x, y, w, h).reduced (1.0f);
    g.setColour (ticked ? accent() : colour (over ? Col::bgControlHover : Col::bgField));
    g.fillRoundedRectangle (r, Radius::sm);
    if (! ticked)
    {
        g.setColour (colour (Col::lineStrong));
        g.drawRoundedRectangle (r, Radius::sm, 1.0f);
    }
    if (ticked)
        Icons::draw (g, Icon::check, r.reduced (1.5f), colour (Col::textOnAccent).withAlpha (enabled ? 1.0f : 0.5f), 2.0f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool down)
{
    const float box = 14.0f;
    auto r = b.getLocalBounds().toFloat();
    drawTickBox (g, b, r.getX() + 2.0f, r.getCentreY() - box * 0.5f, box, box, b.getToggleState(), b.isEnabled(), over, down);
    Text::draw (g, b.getButtonText(), r.withTrimmedLeft (box + 10.0f), font (Type::body),
                colour (b.isEnabled() ? Col::textPrimary : Col::textDisabled));
}

//==============================================================================
void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (colour (Col::bgField));
    g.fillRoundedRectangle (r, Radius::md);
    g.setColour (box.hasKeyboardFocus (true) ? accent().withAlpha (0.7f) : colour (Col::line));
    g.drawRoundedRectangle (r, Radius::md, 1.0f);
    Icons::draw (g, Icon::chevronDown, juce::Rectangle<float> ((float) w - 20.0f, 0, 14.0f, (float) h).withSizeKeepingCentre (10, 10),
                 colour (box.isEnabled() ? Col::textSecondary : Col::textDisabled));
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 0, box.getWidth() - 26, box.getHeight());
    label.setFont (getComboBoxFont (box));
}

void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& te)
{
    g.setColour (te.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h), Radius::md);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& te)
{
    if (! te.isEnabled()) return;
    const bool focused = te.hasKeyboardFocus (true) && ! te.isReadOnly();
    g.setColour (te.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), Radius::md, focused ? 1.5f : 1.0f);
}

//==============================================================================
void LookAndFeel::drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& header)
{
    auto r = header.getLocalBounds();
    g.setColour (colour (Col::bgRaised));
    g.fillRect (r);
    g.setColour (colour (Col::line));
    g.fillRect (r.removeFromBottom (1));
}

void LookAndFeel::drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& name, int,
                                         int w, int h, bool over, bool, int flags)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    if (over)
    {
        g.setColour (colour (Col::bgControl));
        g.fillRect (r);
    }
    g.setColour (colour (Col::line));
    g.fillRect (r.removeFromRight (1.0f).reduced (0, 5));

    const bool sorted = (flags & (juce::TableHeaderComponent::sortedForwards | juce::TableHeaderComponent::sortedBackwards)) != 0;
    auto text = r.reduced (8, 0);
    if (sorted)
    {
        const bool fwd = (flags & juce::TableHeaderComponent::sortedForwards) != 0;
        Icons::draw (g, fwd ? Icon::chevronUp : Icon::chevronDown, text.removeFromRight (10).withSizeKeepingCentre (9, 9), accent());
    }
    Text::caption (g, name, text, colour (sorted ? Col::textPrimary : Col::textSecondary), juce::Justification::centredLeft);
}

void LookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                 int thumbStart, int thumbSize, bool over, bool down)
{
    juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float> ((float) x, (float) thumbStart, (float) w, (float) thumbSize)
                                            : juce::Rectangle<float> ((float) thumbStart, (float) y, (float) thumbSize, (float) h);
    thumb = thumb.reduced (vertical ? 2.5f : 1.0f, vertical ? 1.0f : 2.5f);
    g.setColour (colour (Col::lineStrong).brighter (over || down ? 0.35f : 0.0f));
    g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
}

void LookAndFeel::drawTreeviewPlusMinusBox (juce::Graphics& g, const juce::Rectangle<float>& area, juce::Colour, bool isOpen, bool over)
{
    Icons::draw (g, isOpen ? Icon::chevronDown : Icon::chevronRight, area.withSizeKeepingCentre (10, 10),
                 colour (over ? Col::textPrimary : Col::textTertiary));
}

void LookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    g.setColour (colour (Col::bgMenu));
    g.fillRoundedRectangle (r, Radius::md);
    g.setColour (colour (Col::lineStrong));
    g.drawRoundedRectangle (r.reduced (0.5f), Radius::md, 1.0f);
    Text::draw (g, text, r.reduced (8, 0), font (Type::label), colour (Col::textPrimary), juce::Justification::centredLeft, false);
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    const int w = (int) juce::GlyphArrangement::getStringWidth (font (Type::label), text) + 18;
    const int h = 22;
    return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 12,
                                 pos.y > parent.getCentreY() ? pos.y - (h + 8) : pos.y + 18, w, h)
               .constrainedWithin (parent);
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                    juce::Slider::SliderStyle style, juce::Slider& s)
{
    // Used by stock sliders (settings). Flat track + accent fill + small round thumb.
    const bool horiz = style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearBar;
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    auto track = horiz ? r.withSizeKeepingCentre (r.getWidth(), 3.0f) : r.withSizeKeepingCentre (3.0f, r.getHeight());
    g.setColour (colour (Col::bgField));
    g.fillRoundedRectangle (track, 1.5f);
    g.setColour (accent().withAlpha (s.isEnabled() ? 1.0f : 0.4f));
    if (horiz) g.fillRoundedRectangle (track.withRight (pos), 1.5f);
    else       g.fillRoundedRectangle (track.withTop (pos), 1.5f);
    const auto c = horiz ? juce::Point<float> (pos, r.getCentreY()) : juce::Point<float> (r.getCentreX(), pos);
    g.setColour (colour (Col::textPrimary));
    g.fillEllipse (juce::Rectangle<float> (12, 12).withCentre (c));
}

void LookAndFeel::drawStretchableLayoutResizerBar (juce::Graphics& g, int w, int h, bool vertical, bool over, bool dragging)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    g.setColour (colour (Col::bgApp));
    g.fillRect (r);
    if (over || dragging)
    {
        g.setColour (accent().withAlpha (dragging ? 0.9f : 0.5f));
        g.fillRect (vertical ? r.withSizeKeepingCentre (2.0f, r.getHeight()) : r.withSizeKeepingCentre (r.getWidth(), 2.0f));
    }
    else
    {
        g.setColour (colour (Col::lineStrong));
        const auto c = r.getCentre();
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse (vertical ? juce::Rectangle<float> (2, 2).withCentre ({ c.x, c.y + (float) i * 5.0f })
                                    : juce::Rectangle<float> (2, 2).withCentre ({ c.x + (float) i * 5.0f, c.y }));
    }
}

void LookAndFeel::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillPath (path, juce::AffineTransform::translation (0.0f, 2.0f));
    g.setColour (colour (Col::bgMenu));
    g.fillPath (path);
    g.setColour (colour (Col::lineStrong));
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

juce::Font LookAndFeel::getAlertWindowTitleFont()   { return font (Type::title, Weight::semibold); }
juce::Font LookAndFeel::getAlertWindowMessageFont() { return font (Type::body); }

void LookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& layout)
{
    auto r = alert.getLocalBounds().toFloat();
    g.setColour (colour (Col::bgMenu));
    g.fillRoundedRectangle (r, Radius::lg);
    g.setColour (colour (Col::lineStrong));
    g.drawRoundedRectangle (r.reduced (0.5f), Radius::lg, 1.0f);
    g.setColour (colour (Col::textPrimary));
    layout.draw (g, textArea.toFloat().withX ((float) textArea.getX() - 0.0f));
}

} // namespace om
