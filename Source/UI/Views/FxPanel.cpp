#include "FxPanel.h"

namespace om
{

FxPanel::FxPanel (AudioEngine& e) : engine (e)
{
    for (int d = 0; d < AudioEngine::numDecks; ++d)
        for (int i = 0; i < AudioEngine::numDeckFx; ++i)
        {
            auto s = std::make_unique<Slot>();
            s->deck = d;
            s->index = i;
            auto& fx = engine.deckFx (d, i);
            auto* raw = s.get();

            s->type.setTextType (Type::caption, Weight::bold);
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
    m.addSectionHeader ("BEAT FX");
    for (auto type : { T::delay, T::echo, T::pingPong, T::spiral, T::reverb, T::trans, T::filter, T::flanger, T::phaser,
                       T::pitch, T::slipRoll, T::roll, T::vinylBrake, T::helix, T::tripletFilter, T::tripletRoll, T::mobius })
        m.addItem (BeatFx::typeName (type), true, fx.type.load() == (int) type, [&fx, type]
        {
            fx.enabled = false;
            fx.type = (int) type;
            fx.param = BeatFx::defaultParam (type);
            fx.wet = BeatFx::defaultLevel (type);
        });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&s.type));
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
    const int half = (r.getWidth() - Space::xl) / 2;
    deckAreas[0] = r.removeFromLeft (half);
    r.removeFromLeft (Space::xl);
    deckAreas[1] = r;

    // One row of slots per side, for the deck that side shows.
    shown = { engine.visibleDeck (0), engine.visibleDeck (1) };
    for (auto& sp : slots)
        for (auto* c : std::initializer_list<juce::Component*> { &sp->type, &sp->on, &sp->wet, &sp->param })
            c->setVisible (engine.isVisible (sp->deck));
    for (int side = 0; side < 2; ++side)
    {
        const int d = engine.visibleDeck (side);
        auto a = deckAreas[(size_t) side];
        a.removeFromLeft (44);                                    // deck label
        const int w = (a.getWidth() - 2 * Space::md) / 3;
        for (int i = 0; i < AudioEngine::numDeckFx; ++i)
        {
            auto& s = *slots[(size_t) (d * AudioEngine::numDeckFx + i)];
            s.area = a.removeFromLeft (w);
            a.removeFromLeft (Space::md);
            auto sa = s.area;
            auto left = sa.removeFromLeft (juce::jmin (92, sa.getWidth() / 2));
            s.type.setBounds (left.removeFromTop (left.getHeight() / 2).reduced (0, 2));
            s.on.setBounds (left.reduced (0, 2).withWidth (juce::jmin (left.getWidth(), 44)));
            const int kw = sa.getWidth() / 2;
            s.wet.setBounds (sa.removeFromLeft (kw));
            s.param.setBounds (sa);
        }
    }
}

void FxPanel::paint (juce::Graphics& g)
{
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
