#include "FxPanel.h"

namespace om
{

FxPanel::FxPanel (AudioEngine& e) : engine (e)
{
    setInterceptsMouseClicks (false, true);
    for (int side = 0; side < 2; ++side)
    {
        auto& button = slotButtons[(size_t) side];
        button.setIcon (Icon::chevronDown);
        button.setFitCaption (true);
        button.setTooltip ("Elegir el efecto del deck que querés controlar"_u);
        button.onClick = [this, side] { showSlotMenu (side); };
        addChildComponent (button);
    }
    for (int d = 0; d < AudioEngine::numDecks; ++d)
        for (int i = 0; i < AudioEngine::numDeckFx; ++i)
        {
            auto s = std::make_unique<Slot>();
            s->deck = d;
            s->index = i;
            auto& fx = engine.deckFx (d, i);
            auto* raw = s.get();

            s->type.setTextType (Type::caption, Weight::bold);
            s->type.setFitCaption (true);
            s->type.setTooltip ("Elegir efecto");
            s->type.onClick = [this, raw] { showTypeMenu (*raw); };

            s->on.setTextType (Type::caption, Weight::bold);
            s->on.setLitColour (Theme::get().deckColour (d));
            s->on.onClick = [&fx] { fx.enabled = ! fx.enabled.load(); };

            s->wet.setRange (0.0, 1.0);
            s->wet.setDefault (0.5);
            s->wet.setArcColour (Theme::get().deckColour (d));
            s->wet.formatter = [] (double v) { return juce::String ((int) std::round (v * 100)) + "%"; };
            s->wet.bind ({ [&fx] { return (double) fx.wet.load(); }, [&fx] (double v) { fx.wet = (float) v; } });

            s->param.setRange (0.0, 1.0);
            s->param.setDefault (0.5);
            s->param.setAlwaysShowValue (true);                 // BEAT 1/2, AMOUNT 90%… always in sight
            s->param.bind ({ [&fx] { return (double) fx.param.load(); }, [&fx] (double v) { fx.param = (float) v; } });

            for (auto* c : std::initializer_list<juce::Component*> { &s->type, &s->on, &s->wet, &s->param })
                addAndMakeVisible (c);
            slots.push_back (std::move (s));
        }
}

void FxPanel::showTypeMenu (Slot& s)
{
    auto& fx = engine.deckFx (s.deck, s.index);
    juce::PopupMenu m;
    m.addSectionHeader ("FX " + juce::String (s.index + 1) + " · DECK " + juce::String (s.deck + 1));
    using T = BeatFx::Type;
    if (presentation == ViewLayout::openMix) m.addSectionHeader ("BEAT FX");
    for (auto type : { T::delay, T::echo, T::pingPong, T::spiral, T::reverb, T::trans, T::filter, T::flanger, T::phaser,
                       T::pitch, T::slipRoll, T::roll, T::vinylBrake, T::lowCutEcho })
        m.addItem (BeatFx::typeName (type), true, fx.type.load() == (int) type, [&fx, type]
        {
            fx.enabled = false;
            fx.type = (int) type;
            fx.param = BeatFx::defaultParam (type);
            fx.wet = BeatFx::defaultLevel (type);
        });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&s.type));
}

void FxPanel::setPresentation (ViewLayout layout, int centreWidth)
{
    if (layout == presentation && centreWidth == mixerGap) return;
    presentation = layout;
    mixerGap = centreWidth;
    resized();
    repaint();
}

void FxPanel::showSlotMenu (int side)
{
    const int deck = engine.visibleDeck (side);
    juce::PopupMenu menu;
    for (int slot = 0; slot < AudioEngine::numDeckFx; ++slot)
    {
        const auto& fx = engine.deckFx (deck, slot);
        const auto label = juce::String (slot + 1) + " · "_u + BeatFx::typeName ((BeatFx::Type) fx.type.load())
                         + (fx.enabled.load() ? " · ON"_u : juce::String());
        menu.addItem (label, true, slot == selectedSlots[(size_t) deck],
            [safe = juce::Component::SafePointer<FxPanel> (this), deck, slot]
            {
                if (auto* panel = safe.getComponent())
                {
                    panel->selectedSlots[(size_t) deck] = slot;
                    panel->resized();
                }
            });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slotButtons[(size_t) side]));
}

void FxPanel::refresh()
{
    int state = 0, bit = 0;
    for (auto& sp : slots)
    {
        auto& s = *sp;
        auto& fx = engine.deckFx (s.deck, s.index);
        const auto t = (BeatFx::Type) fx.type.load();
        if ((int) t != s.lastType)
        {
            s.lastType = (int) t;
            s.type.setButtonText (BeatFx::typeName (t));
            s.param.setLabel (BeatFx::paramName (t));
            s.param.formatter = [t] (double v) { return BeatFx::paramText (t, (float) v); };
            s.on.setButtonText ("ON");
            s.on.setTooltip ("Activar / desactivar");
        }
        s.on.setToggleState (fx.enabled.load(), juce::dontSendNotification);
        s.wet.syncFromEngine();
        s.param.syncFromEngine();
        state |= (fx.enabled.load() ? 1 : 0) << bit++;
    }
    if (state != lastState) { lastState = state; repaint(); }
    if (shown != std::pair { engine.visibleDeck (0), engine.visibleDeck (1) }) { resized(); repaint(); }
}

void FxPanel::resized()
{
    auto r = getLocalBounds().reduced (Space::md, Space::xs);
    const int separation = presentation == ViewLayout::openMix ? Space::xl : mixerGap;
    const int half = juce::jmax (0, (r.getWidth() - separation) / 2);
    deckAreas[0] = r.removeFromLeft (half);
    r.removeFromLeft (separation);
    deckAreas[1] = r;

    shown = { engine.visibleDeck (0), engine.visibleDeck (1) };
    for (auto& sp : slots)
    {
        const bool visible = engine.isVisible (sp->deck)
                          && (! focused() || sp->index == selectedSlots[(size_t) sp->deck]);
        for (auto* c : std::initializer_list<juce::Component*> { &sp->type, &sp->on, &sp->wet, &sp->param })
            c->setVisible (visible);
    }

    for (int side = 0; side < 2; ++side)
    {
        const int deck = engine.visibleDeck (side);
        auto area = deckAreas[(size_t) side];
        auto& slotButton = slotButtons[(size_t) side];
        slotButton.setVisible (focused());
        if (focused())
        {
            const int index = selectedSlots[(size_t) deck];
            auto& slot = *slots[(size_t) (deck * AudioEngine::numDeckFx + index)];
            slotButton.setButtonText ("FX " + juce::String (index + 1));
            slotButton.setBounds (area.removeFromLeft (56).withSizeKeepingCentre (56, 26));
            area.removeFromLeft (Space::xs);
            if (presentation == ViewLayout::rekordbox)
            {
                slot.on.setBounds (area.removeFromRight (40).withSizeKeepingCentre (40, 26));
                area.removeFromRight (Space::xs);
            }
            const int controlWidth = juce::jlimit (40, 78, area.getWidth() / 4);
            slot.param.setBounds (area.removeFromRight (controlWidth));
            slot.wet.setBounds (area.removeFromRight (controlWidth));
            if (presentation == ViewLayout::virtualDj)
            {
                slot.on.setBounds (area.removeFromLeft (38).withSizeKeepingCentre (38, 26));
                area.removeFromLeft (Space::xs);
            }
            slot.type.setBounds (area.withSizeKeepingCentre (area.getWidth(), 26));
            continue;
        }

        if (presentation == ViewLayout::openMix)
            area.removeFromLeft (44);
        const int gap = presentation == ViewLayout::openMix ? Space::md : Space::xs;
        const int width = juce::jmax (0, (area.getWidth() - 2 * gap) / 3);
        for (int index = 0; index < AudioEngine::numDeckFx; ++index)
        {
            auto& slot = *slots[(size_t) (deck * AudioEngine::numDeckFx + index)];
            auto cell = area.removeFromLeft (width);
            area.removeFromLeft (gap);
            slot.area = cell;

            if (presentation == ViewLayout::openMix)
            {
                auto left = cell.removeFromLeft (juce::jmin (92, cell.getWidth() / 2));
                slot.type.setBounds (left.removeFromTop (left.getHeight() / 2).reduced (0, 2));
                slot.on.setBounds (left.reduced (0, 2).withWidth (juce::jmin (left.getWidth(), 44)));
            }
            else
            {
                auto header = cell.removeFromTop (22);
                auto on = presentation == ViewLayout::serato ? header.removeFromLeft (28) : header.removeFromRight (28);
                slot.on.setBounds (on);
                header.reduce (2, 0);
                slot.type.setBounds (header);
            }
            slot.wet.setBounds (cell.removeFromLeft (cell.getWidth() / 2));
            slot.param.setBounds (cell);
        }
    }
}

void FxPanel::paint (juce::Graphics& g)
{
    if (presentation != ViewLayout::openMix)
    {
        g.setColour (colour (Col::bgRaised));
        for (const auto& area : deckAreas)
            g.fillRoundedRectangle (area.expanded (3, 2).toFloat(), Radius::md);
        return;
    }

    g.fillAll (colour (Col::bgRaised));
    g.setColour (colour (Col::line));
    g.fillRect (getLocalBounds().removeFromBottom (1));
    g.fillRect (getWidth() / 2, Space::sm, 1, getHeight() - Space::sm * 2);

    for (int side = 0; side < 2; ++side)
    {
        const int d = engine.visibleDeck (side);
        auto label = deckAreas[(size_t) side].withWidth (40).toFloat();
        bool any = false;
        for (int i = 0; i < AudioEngine::numDeckFx; ++i) any = any || engine.deckFx (d, i).enabled.load();
        Icons::draw (g, Icon::fx, label.removeFromTop (label.getHeight() * 0.55f).withSizeKeepingCentre (16, 16).translated (0, 4),
                     any ? Theme::get().deckColour (d) : colour (Col::textTertiary));
        Text::caption (g, "FX " + juce::String (d + 1), label, any ? Theme::get().deckColour (d) : colour (Col::textTertiary),
                       juce::Justification::centredTop);
    }
}

} // namespace om
