#include "MixerView.h"

namespace om
{

static juce::String dbText (double db)
{
    if (db <= -99.0) return "KILL";
    return (db > 0.05 ? "+" : "") + juce::String (db, 1) + " dB";
}

MixerView::Strip::Strip (int i) : index (i) {}

MixerView::MixerView (AudioEngine& e) : engine (e)
{
    for (int i = 0; i < 2; ++i)
    {
        strips[(size_t) i] = std::make_unique<Strip> (i);
        bindStrip (*strips[(size_t) i]);
    }

    masterKnob.setRange (-40.0, 6.0);
    masterKnob.setSkewFactorFromMidPoint (-6.0);
    masterKnob.setDefault (0.0);
    masterKnob.formatter = [] (double v) { return v <= -39.9 ? juce::String ("MUTE") : dbText (v); };
    masterKnob.bind ({ [this] { return (double) engine.masterDb.load(); }, [this] (double v) { engine.masterDb = (float) v; } });
    masterMeter.setSource (&engine.masterMeter());

    cueMix.setRange (0.0, 1.0);
    cueMix.setDefault (0.5);
    cueMix.formatter = [] (double v) { return juce::String ((int) std::round (v * 100)) + "% MST"; };
    cueMix.bind ({ [this] { return (double) engine.cueMix.load(); }, [this] (double v) { engine.cueMix = (float) v; } });

    phones.setRange (-40.0, 6.0);
    phones.setSkewFactorFromMidPoint (-6.0);
    phones.setDefault (0.0);
    phones.formatter = [] (double v) { return dbText (v); };
    phones.bind ({ [this] { return (double) engine.phonesDb.load(); }, [this] (double v) { engine.phonesDb = (float) v; } });

    crossfader.setRange (-1.0, 1.0);
    crossfader.setDefault (0.0);
    crossfader.setTicks (11);
    crossfader.bind ({ [this] { return (double) engine.crossfader.load(); }, [this] (double v) { engine.crossfader = (float) v; } });
    crossfader.setTooltip ("Crossfader (doble clic: centro)");

    curve.setCompact (true);
    curve.onChange = [this] (int c) { engine.crossfaderCurve = c; };

    limBtn.setTextType (Type::micro, Weight::bold);
    limBtn.setLitColour (colour (Col::play));
    limBtn.setTooltip ("Limitador master true-peak (look-ahead). Techo en Configuración › Interfaz");
    limBtn.onClick = [this] { engine.limiter().enabled = ! engine.limiter().enabled.load(); };

    for (auto* c : std::initializer_list<juce::Component*> { &masterKnob, &masterMeter, &cueMix, &phones, &crossfader, &curve, &limBtn })
        addAndMakeVisible (c);
}

void MixerView::bindStrip (Strip& s)
{
    auto& ch = engine.channel (s.index);
    const auto deckCol = Theme::get().deckColour (s.index);

    s.trim.setRange (-12.0, 12.0);
    s.trim.setDefault (0.0);
    s.trim.formatter = [] (double v) { return dbText (v); };
    s.trim.bind ({ [&ch] { return (double) ch.trimDb.load(); }, [&ch] (double v) { ch.trimDb = (float) v; } });

    auto eq = [] (Knob& k, std::atomic<float>& p)
    {
        k.setRange (0.0, 1.0);
        k.setDefault (0.5);
        k.setShowsKill (true);
        k.formatter = [] (double v) { return dbText (ChannelStrip::eqKnobToDb ((float) v)); };
        k.bind ({ [&p] { return (double) p.load(); }, [&p] (double v) { p = (float) v; } });
    };
    eq (s.hi, ch.eqHigh);
    eq (s.mid, ch.eqMid);
    eq (s.low, ch.eqLow);

    s.filter.setRange (-1.0, 1.0);
    s.filter.setDefault (0.0);
    s.filter.setArcColour (deckCol);
    s.filter.formatter = [&ch] (double v)
    {
        const auto c = (ChannelStrip::Color) ch.colorType.load();
        if (std::abs (v) <= 0.03) return juce::String ("OFF");
        const auto pct = juce::String ((int) std::round (std::abs (v) * 100)) + "%";
        if (c == ChannelStrip::Color::filter) return juce::String (v < 0 ? "LPF " : "HPF ") + pct;
        return juce::String (v < 0 ? "< " : "> ") + pct;
    };
    s.filter.setTooltip ("Sound Color FX (clic derecho: elegir FILTER, SPACE, DUB ECHO, SWEEP, NOISE o CRUSH)");
    s.filter.onRightClick = [&ch, knob = &s.filter]
    {
        juce::PopupMenu m;
        m.addSectionHeader ("SOUND COLOR FX");
        for (int c = 0; c < (int) ChannelStrip::Color::numColors; ++c)
            m.addItem (ChannelStrip::colorName ((ChannelStrip::Color) c), true, ch.colorType.load() == c, [&ch, c] { ch.colorType = c; });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (knob));
    };
    s.filter.bind ({ [&ch] { return (double) ch.filter.load(); }, [&ch] (double v) { ch.filter = (float) v; } });

    s.fader.setRange (0.0, 1.0);
    s.fader.setDefault (1.0);
    s.fader.setTicks (11);
    s.fader.bind ({ [&ch] { return (double) ch.fader.load(); }, [&ch] (double v) { ch.fader = (float) v; } });
    s.meter.setSource (&ch.meter);

    s.pfl.setLitColour (colour (Col::cue));
    s.pfl.onClick = [&ch] { ch.pfl = ! ch.pfl.load(); };

    s.assign.setCompact (true);
    s.assign.setLitColour (deckCol);
    s.assign.onChange = [&ch] (int v) { ch.xfAssign = v; };

    int unit = 0;
    for (auto* b : { &s.fx1, &s.fx2, &s.fx3 })
    {
        b->setLitColour (deckCol);
        b->setTextType (Type::micro, Weight::bold);
        b->onClick = [this, idx = s.index, unit] { auto& f = engine.fx (idx, unit); f.enabled = ! f.enabled.load(); };
        ++unit;
    }

    for (auto* c : std::initializer_list<juce::Component*> { &s.trim, &s.hi, &s.mid, &s.low, &s.filter, &s.pfl, &s.fx1, &s.fx2, &s.fx3,
                                                             &s.fader, &s.meter, &s.assign })
        addAndMakeVisible (c);
}

void MixerView::refresh()
{
    const bool phonesOk = engine.hasHeadphoneOutput();

    for (auto& sp : strips)
    {
        auto& s = *sp;
        auto& ch = engine.channel (s.index);
        for (auto* k : { &s.trim, &s.hi, &s.mid, &s.low, &s.filter })
            k->syncFromEngine();
        s.filter.setLabel (ChannelStrip::colorName ((ChannelStrip::Color) ch.colorType.load()));
        s.fader.syncFromEngine();
        s.meter.update();
        s.assign.setSelected (ch.xfAssign.load());
        s.pfl.setToggleState (ch.pfl.load(), juce::dontSendNotification);
        s.pfl.setEnabled (phonesOk);
        s.pfl.setTooltip (phonesOk ? "Pre-escucha en auriculares (salidas 3/4)"
                                   : "Pre-escucha: requiere una interfaz con 4 salidas (activalas en Configuración › Audio)"_u);

        for (int u = 0; u < AudioEngine::numFxSlots; ++u)
        {
            auto& f = engine.fx (s.index, u);
            auto& b = u == 0 ? s.fx1 : u == 1 ? s.fx2 : s.fx3;
            b.setToggleState (f.enabled.load(), juce::dontSendNotification);
            b.setTooltip ("FX " + juce::String (u + 1) + ": " + BeatFx::typeName ((BeatFx::Type) f.type.load()) + " (clic: activar)");
        }
    }

    masterKnob.syncFromEngine();
    cueMix.syncFromEngine();
    phones.syncFromEngine();
    cueMix.setEnabled (phonesOk);
    phones.setEnabled (phonesOk);
    crossfader.syncFromEngine();
    curve.setSelected (engine.crossfaderCurve.load());
    masterMeter.update();

    limBtn.setToggleState (engine.limiter().enabled.load(), juce::dontSendNotification);
    const float gr = engine.limiter().drainReductionDb();
    const float newGr = juce::jmax (gr, grDb - 0.4f);            // ~24 dB/s fall
    if (std::abs (newGr - grDb) > 0.01f) { grDb = juce::jmax (0.0f, newGr); repaint (grArea); }

    // Master clip gets an explicit text label, not just a red segment.
    if (masterMeter.isClipLit() != masterClipShown)
    {
        masterClipShown = masterMeter.isClipLit();
        repaint (clipLabelArea);
    }
}

void MixerView::resized()
{
    auto r = getLocalBounds().reduced (Space::md, Space::sm);

    // Bottom: crossfader block
    auto xfBlock = r.removeFromBottom (52);
    r.removeFromBottom (Space::sm);

    const int masterW = juce::jlimit (64, 84, r.getWidth() / 4);
    masterArea = r.removeFromRight (masterW);
    r.removeFromRight (Space::sm);

    const int stripW = (r.getWidth() - Space::sm) / 2;
    const int knobCount = 5;

    for (int i = 0; i < 2; ++i)
    {
        auto& s = *strips[(size_t) i];
        auto col = i == 0 ? r.removeFromLeft (stripW) : r.removeFromRight (stripW);

        col.removeFromTop (16); // channel title painted

        // Knobs get a guaranteed usable size first; the fader takes what remains.
        const int fixedBelow = Size::controlXs + Size::controlSm + Space::xs * 3;
        const int kh = juce::jlimit (26, 46, juce::roundToInt ((float) (col.getHeight() - fixedBelow) * 0.62f) / knobCount);
        const int kw = juce::jmin (col.getWidth(), kh + 44);
        for (auto* k : { &s.trim, &s.hi, &s.mid, &s.low, &s.filter })
        {
            k->setLabelOnSide (true);
            k->setBounds (col.removeFromTop (kh).withSizeKeepingCentre (kw, kh));
        }
        col.removeFromTop (Space::xs);
        s.pfl.setBounds (col.removeFromTop (Size::controlSm).withSizeKeepingCentre (juce::jmin (44, col.getWidth()), Size::controlSm));
        col.removeFromTop (Space::xs);
        auto fxRow = col.removeFromTop (Size::controlXs);
        col.removeFromTop (Space::xs);
        auto faderArea = col;

        const int fw = (fxRow.getWidth() - 2 * Space::xs) / 3;
        s.fx1.setBounds (fxRow.removeFromLeft (fw));
        fxRow.removeFromLeft (Space::xs);
        s.fx2.setBounds (fxRow.removeFromLeft (fw));
        fxRow.removeFromLeft (Space::xs);
        s.fx3.setBounds (fxRow);

        // Fader with meter on the inner side
        auto meterArea = i == 0 ? faderArea.removeFromRight (10) : faderArea.removeFromLeft (10);
        s.meter.setBounds (meterArea.reduced (1, 4));
        s.fader.setBounds (faderArea.withSizeKeepingCentre (Size::faderWidth, faderArea.getHeight()));
    }

    // Master column
    {
        auto m = masterArea;
        m.removeFromTop (16);
        masterKnob.setBounds (m.removeFromTop (50).withSizeKeepingCentre (48, 48));
        m.removeFromTop (Space::xs);
        auto limRow = m.removeFromTop (18);
        limBtn.setBounds (limRow.removeFromLeft (30));
        limRow.removeFromLeft (3);
        grArea = limRow.reduced (0, 5);
        m.removeFromTop (Space::xs);
        clipLabelArea = m.removeFromTop (14);
        phonesArea = m.removeFromBottom (juce::jmin (120, m.getHeight() / 2));
        masterMeter.setBounds (m.withSizeKeepingCentre (14, m.getHeight() - 8));

        auto p = phonesArea;
        p.removeFromTop (14);
        const int kh = juce::jmin (46, p.getHeight() / 2);
        cueMix.setBounds (p.removeFromTop (kh).withSizeKeepingCentre (kh + 6, kh));
        phones.setBounds (p.removeFromTop (kh).withSizeKeepingCentre (kh + 6, kh));
    }

    // Crossfader block: assign switches on each side, curve under the fader
    {
        auto x = xfBlock;
        auto top = x.removeFromTop (30);
        strips[0]->assign.setBounds (top.removeFromLeft (96).withSizeKeepingCentre (96, 20));
        strips[1]->assign.setBounds (top.removeFromRight (96).withSizeKeepingCentre (96, 20));
        crossfader.setBounds (top.reduced (Space::sm, 0));
        curve.setBounds (x.withSizeKeepingCentre (juce::jmin (180, x.getWidth()), 18).translated (0, 2));
    }
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgRaised));
    auto r = getLocalBounds().reduced (Space::md, Space::sm);

    for (auto& sp : strips)
    {
        const auto b = sp->trim.getBounds();
        auto title = juce::Rectangle<int> (b.getX(), r.getY(), b.getWidth(), 14).toFloat();
        Text::draw (g, juce::String ("CH ") + juce::String (sp->index + 1), title, font (Type::caption, Weight::bold),
                    Theme::get().deckColour (sp->index), juce::Justification::centred);
    }

    Text::caption (g, "MASTER", masterArea.withHeight (14).toFloat(), colour (Col::textSecondary));

    // Limiter gain reduction: bar grows right-to-left, 0…12 dB.
    if (! grArea.isEmpty())
    {
        auto bar = grArea.toFloat();
        g.setColour (colour (Col::bgField));
        g.fillRoundedRectangle (bar, 1.5f);
        if (grDb > 0.05f && engine.limiter().enabled.load())
        {
            const float w = juce::jlimit (0.0f, 1.0f, grDb / 12.0f) * bar.getWidth();
            g.setColour (grDb > 6.0f ? colour (Col::warning) : colour (Col::play));
            g.fillRoundedRectangle (bar.withLeft (bar.getRight() - juce::jmax (2.0f, w)), 1.5f);
        }
    }
    if (masterClipShown)
    {
        auto c = clipLabelArea.toFloat().withSizeKeepingCentre (40, 13);
        g.setColour (colour (Col::clip));
        g.fillRoundedRectangle (c, 2.0f);
        Text::draw (g, "CLIP", c, font (Type::micro, Weight::bold), juce::Colours::white, juce::Justification::centred, false);
    }
    Text::caption (g, "AURICULARES", phonesArea.withHeight (14).toFloat(),
                   colour (engine.hasHeadphoneOutput() ? Col::textTertiary : Col::textDisabled));

    // Vertical hairlines separating master column
    g.setColour (colour (Col::line));
    g.fillRect (masterArea.getX() - Space::sm / 2 - 1, r.getY(), 1, masterArea.getHeight());
    g.fillRect (getLocalBounds().removeFromLeft (1));
    g.fillRect (getLocalBounds().removeFromRight (1));
}

} // namespace om
