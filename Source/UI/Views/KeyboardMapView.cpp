#include "KeyboardMapView.h"

namespace om
{

KeyboardMapView::KeyboardMapView (KeyboardControl& k, Actions& a) : keyboard (k), actions (a)
{
    buildKeys();
    layer.setCompact (true);
    layer.onChange = [this] (int) { repaint(); };
    addAndMakeVisible (layer);
    factory.setTooltip (juce::String::fromUTF8 ("Vuelve al mapeo de teclado de fábrica"));
    factory.onClick = [this]
    {
        keyboard.resetToFactory();
        repaint();
    };
    addAndMakeVisible (factory);
}

void KeyboardMapView::buildKeys()
{
    using KP = juce::KeyPress;
    auto row = [this] (std::initializer_list<std::pair<int, const char*>> ks, std::initializer_list<float> widths = {})
    {
        std::vector<float> w (widths);
        size_t i = 0;
        for (auto& [code, name] : ks)
        {
            keys.push_back ({ code, juce::String::fromUTF8 (name), i < w.size() ? w[i] : 1.0f, {} });
            ++i;
        }
        keys.push_back ({ -1, {}, 0.0f, {} });          // end of row
    };

    row ({ { KP::escapeKey, "ESCAPE" }, { KP::F1Key, "F1" }, { KP::F2Key, "F2" }, { KP::F3Key, "F3" }, { KP::F4Key, "F4" },
           { KP::F5Key, "F5" }, { KP::F6Key, "F6" }, { KP::F7Key, "F7" }, { KP::F8Key, "F8" }, { KP::F9Key, "F9" },
           { KP::F10Key, "F10" }, { KP::F11Key, "F11" }, { KP::F12Key, "F12" } },
         { 1.5f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f, 1.0833f });
    row ({ { '`', "`" }, { '1', "1" }, { '2', "2" }, { '3', "3" }, { '4', "4" }, { '5', "5" }, { '6', "6" }, { '7', "7" },
           { '8', "8" }, { '9', "9" }, { '0', "0" }, { '-', "-" }, { '=', "=" }, { KP::backspaceKey, "BACK" } },
         { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.5f });
    row ({ { KP::tabKey, "TAB" }, { 'q', "Q" }, { 'w', "W" }, { 'e', "E" }, { 'r', "R" }, { 't', "T" }, { 'y', "Y" },
           { 'u', "U" }, { 'i', "I" }, { 'o', "O" }, { 'p', "P" }, { '[', "[" }, { ']', "]" }, { '\\', "\\" } },
         { 1.5f });
    row ({ { 0, "CAPSLOCK" }, { 'a', "A" }, { 's', "S" }, { 'd', "D" }, { 'f', "F" }, { 'g', "G" }, { 'h', "H" },
           { 'j', "J" }, { 'k', "K" }, { 'l', "L" }, { ';', ";" }, { '\'', "'" }, { KP::returnKey, "RETURN" } },
         { 1.75f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.75f });
    row ({ { 0, "SHIFT" }, { 'z', "Z" }, { 'x', "X" }, { 'c', "C" }, { 'v', "V" }, { 'b', "B" }, { 'n', "N" },
           { 'm', "M" }, { ',', "," }, { '.', "." }, { '/', "/" }, { 0, "SHIFT" } },
         { 2.25f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.25f });
    row ({ { 0, "CTRL" }, { 0, "ALT" }, { 0, "CMD" }, { KP::spaceKey, "SPACE" }, { 0, "CMD" }, { 0, "ALT" },
           { KP::leftKey, "LEFT" }, { KP::rightKey, "RIGHT" }, { KP::upKey, "UP" }, { KP::downKey, "DOWN" } },
         { 1.5f, 1, 1, 5, 1, 1, 1, 1, 1, 1 });
}

void KeyboardMapView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (Size::controlMd);
    layer.setBounds (top.removeFromLeft (180));
    factory.setBounds (top.removeFromRight (220));
    r.removeFromTop (Space::lg);
    note = r.removeFromBottom (22).toFloat();
    r.removeFromBottom (Space::md);

    // 14.5 key units wide, 6 rows (the function row a little shorter).
    const float unit = juce::jmin ((float) r.getWidth() / 14.5f, (float) r.getHeight() / 5.7f * 0.92f);
    const float gap = juce::jmax (2.0f, unit * 0.05f);
    board = juce::Rectangle<float> (unit * 14.5f, unit * 5.7f).withCentre (r.toFloat().getCentre()).withY ((float) r.getY());
    float x = board.getX(), y = board.getY();
    int rowIndex = 0;
    for (auto& k : keys)
    {
        const float h = rowIndex == 0 ? unit * 0.7f : unit;
        if (k.code == -1)
        {
            x = board.getX();
            y += h;
            ++rowIndex;
            continue;
        }
        k.bounds = juce::Rectangle<float> (x, y, unit * k.width, h).reduced (gap * 0.5f);
        x += unit * k.width;
    }
}

int KeyboardMapView::keyAt (juce::Point<float> p) const
{
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i].code > 0 && keys[i].bounds.contains (p))
            return (int) i;
    return -1;
}

void KeyboardMapView::mouseMove (const juce::MouseEvent& e)
{
    const int k = keyAt (e.position);
    if (k != hover) { hover = k; repaint(); }
}

void KeyboardMapView::mouseDown (const juce::MouseEvent& e)
{
    if (const int k = keyAt (e.position); k >= 0)
        showAssignMenu (k);
}

void KeyboardMapView::showAssignMenu (int keyIndex)
{
    const auto key = keys[(size_t) keyIndex];
    const bool shift = layer.getSelected() == 1;
    juce::PopupMenu m;
    m.addSectionHeader ((shift ? "SHIFT + " : "") + key.name);
    if (keyboard.bindingFor (key.code, shift) != nullptr)
        m.addItem (juce::String::fromUTF8 ("Quitar asignación"), [this, key, shift] { keyboard.clear (key.code, shift); repaint(); });
    m.addSeparator();

    std::map<juce::String, juce::PopupMenu> cats;
    juce::StringArray order;
    for (auto& info : actions.list())
    {
        if (info.type != ControlType::button && info.id != "browser_scroll") continue;   // keys press buttons
        if (! order.contains (info.category)) order.add (info.category);
        auto& sub = cats[info.category];
        const auto label = KeyboardControl::capLabel (info.id) + "  —  "_u + info.label;
        if (info.perDeck)
        {
            juce::PopupMenu decks;
            for (int d = 0; d < 2; ++d)
                decks.addItem ("Deck " + juce::String (d + 1), [this, key, shift, id = info.id, d]
                {
                    keyboard.assign (key.code, shift, id, d);
                    repaint();
                });
            sub.addSubMenu (label, decks);
        }
        else
            sub.addItem (label, [this, key, shift, id = info.id] { keyboard.assign (key.code, shift, id, -1); repaint(); });
    }
    for (auto& c : order)
        m.addSubMenu (c, cats[c]);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (key.bounds.toNearestInt())));
}

void KeyboardMapView::paint (juce::Graphics& g)
{
    const bool shift = layer.getSelected() == 1;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const auto& k = keys[i];
        if (k.code == -1 || k.bounds.isEmpty()) continue;
        auto r = k.bounds;
        const auto* b = k.code > 0 ? keyboard.bindingFor (k.code, shift) : nullptr;
        const bool isHover = (int) i == hover;

        g.setColour (isHover ? colour (Col::bgControlHover) : colour (Col::bgControl));
        g.fillRoundedRectangle (r, Radius::md);
        if (b != nullptr)
        {
            // soft glow + deck stripe
            g.setGradientFill (juce::ColourGradient (colour (Col::textPrimary).withAlpha (0.07f), r.getCentreX(), r.getCentreY(),
                                                     juce::Colours::transparentBlack, r.getX(), r.getY(), true));
            g.fillRoundedRectangle (r, Radius::md);
            if (b->deck >= 0)
            {
                g.setColour (Theme::get().deckColour (b->deck));
                g.fillRoundedRectangle (r.withHeight (2.5f).reduced (r.getWidth() * 0.18f, 0), 1.0f);
            }
        }
        g.setColour (colour (Col::line));
        g.drawRoundedRectangle (r.reduced (0.5f), Radius::md, 1.0f);

        auto inner = r.reduced (3.0f, 2.0f);
        const float nameH = juce::jmin (16.0f, inner.getHeight() * 0.3f);
        auto nameArea = inner.removeFromBottom (nameH);
        Text::draw (g, k.name, nameArea, font (Type::micro), k.code > 0 ? colour (Col::textTertiary) : colour (Col::textDisabled),
                    juce::Justification::centred);
        if (b != nullptr)
        {
            g.setColour (colour (Col::textPrimary));
            g.setFont (font (Type::label, Weight::semibold));
            g.drawFittedText (KeyboardControl::capLabel (b->action), inner.toNearestInt(), juce::Justification::centred, 2, 0.8f);
        }
    }

    Text::draw (g, juce::String::fromUTF8 ("Clic en una tecla para asignarle una función · la franja de color indica el deck · "
                                           "con la tecla SHIFT se usa la capa SHIFT"),
                note, font (Type::label), colour (Col::textSecondary), juce::Justification::centred);
}

} // namespace om
