#include "MixerView.h"
#include <BinaryData.h>

namespace om
{

static juce::String dbText (double db)
{
    if (db <= -99.0) return "KILL";
    return (db > 0.05 ? "+" : "") + juce::String (db, 1) + " dB";
}

MixerView::Strip::Strip (int i) : index (i) {}

//==============================================================================
int XPad::segments() const
{
    const auto t = (BeatFx::Type) engine.beatFx().type.load();
    return juce::jmin (juce::jlimit (4, 8, getWidth() / 20), BeatFx::beatSteps (t));
}

int XPad::indexAt (int seg) const
{
    // The X-PAD's positions: consecutive BEAT values around the effect's usual one.
    const auto t = (BeatFx::Type) engine.beatFx().type.load();
    const int n = BeatFx::beatSteps (t), k = segments();
    int def = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs (BeatFx::paramAt (t, i) - BeatFx::defaultParam (t)) < 1.0e-6f) def = i;
    const int first = juce::jlimit (0, juce::jmax (0, n - k), def - k / 2);
    return juce::jmin (n - 1, first + seg);
}

int XPad::segmentAt (int x) const
{
    return juce::jlimit (0, segments() - 1, x * segments() / juce::jmax (1, getWidth()));
}

void XPad::touch (int x)
{
    auto& b = engine.beatFx();
    const int seg = segmentAt (x);
    if (seg == held) return;
    held = seg;
    b.timeMs = 0.0f;
    b.param = BeatFx::paramAt ((BeatFx::Type) b.type.load(), indexAt (seg));
    b.on = true;
    repaint();
}

void XPad::mouseDown (const juce::MouseEvent& e)
{
    auto& b = engine.beatFx();
    savedParam = b.param.load();
    savedTime = b.timeMs.load();
    savedOn = b.on.load();
    held = -1;
    touch (e.x);
}

void XPad::mouseDrag (const juce::MouseEvent& e)     { touch (e.x); }

void XPad::mouseUp (const juce::MouseEvent&)
{
    auto& b = engine.beatFx();
    b.param = savedParam;
    b.timeMs = savedTime;
    b.on = savedOn;
    held = -1;
    repaint();
}

void XPad::paint (juce::Graphics& g)
{
    const auto t = (BeatFx::Type) engine.beatFx().type.load();
    const int k = segments();
    auto r = getLocalBounds().toFloat();
    g.setColour (colour (Col::bgField));
    g.fillRoundedRectangle (r, 4.0f);
    const float w = r.getWidth() / (float) k;
    for (int i = 0; i < k; ++i)
    {
        auto cell = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f);
        if (i == held)
        {
            g.setColour (colour (Col::clip).withAlpha (0.85f));
            g.fillRoundedRectangle (cell, 3.0f);
        }
        Text::draw (g, BeatFx::paramText (t, BeatFx::paramAt (t, indexAt (i))), cell, font (Type::micro, Weight::bold),
                    i == held ? juce::Colours::white : colour (Col::textSecondary), juce::Justification::centred, false);
    }
    g.setColour (colour (Col::line));
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
}

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
    cueMix.formatter = [] (double v) { return v < 0.02 ? juce::String ("CUE") : v > 0.98 ? juce::String ("MASTER")
                                                                              : juce::String ((int) std::round (v * 100)) + "% MST"; };
    cueMix.setTooltip ("MIXING: CUE ↔ MASTER en los auriculares"_u);
    cueMix.bind ({ [this] { return (double) engine.cueMix.load(); }, [this] (double v) { engine.cueMix = (float) v; } });

    phones.setRange (-40.0, 6.0);
    phones.setSkewFactorFromMidPoint (-6.0);
    phones.setDefault (0.0);
    phones.formatter = [] (double v) { return dbText (v); };
    phones.setTooltip ("Volumen de los auriculares"_u);
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

    // SOUND COLOR FX: one button lit for the whole mixer; each channel's COLOR knob applies it.
    using C = ChannelStrip::Color;
    const C order[] = { C::space, C::dubEcho, C::sweep, C::noise, C::crush, C::filter };
    for (size_t k = 0; k < colorBtns.size(); ++k)
    {
        const int c = (int) order[k];
        auto b = std::make_unique<Button> (ChannelStrip::colorName (order[k]));
        b->setTextType (Type::micro, Weight::bold);
        b->setTooltip (juce::String::fromUTF8 ("SOUND COLOR FX: encendelo y girá la perilla COLOR de cada canal. Otra vez: se apaga"));
        b->onClick = [this, c] { selectColor (colorSelected == c ? -1 : c); };
        addAndMakeVisible (*b);
        colorBtns[k] = std::move (b);
    }
    colorParam.setRange (0.0, 1.0);
    colorParam.setDefault (0.5);
    colorParam.formatter = [] (double v) { return juce::String ((int) std::round (v * 100)) + "%"; };
    colorParam.setTooltip ("PARAMETER: ajusta el SOUND COLOR FX encendido"_u);
    colorParam.bind ({ [this] { return (double) engine.channel (0).colorParam.load(); },
                       [this] (double v) { for (int d = 0; d < 2; ++d) engine.channel (d).colorParam = (float) v; } });
    colorSelected = engine.channel (0).colorOn.load() ? engine.channel (0).colorType.load() : -1;

    // BEAT FX
    auto& bx = engine.beatFx();
    beatDown.setTooltip ("BEAT ◀: menos beats"_u);
    beatUp.setTooltip ("BEAT ▶: más beats"_u);
    beatDown.onClick = [this] { engine.beatFx().stepBeat (-1); };
    beatUp.onClick   = [this] { engine.beatFx().stepBeat (+1); };
    fxSelect.setTextType (Type::caption, Weight::bold);
    fxSelect.setTooltip ("BEAT FX SELECT: elegir el efecto"_u);
    fxSelect.onClick = [this] { showFxMenu(); };
    chSelect.setTextType (Type::caption, Weight::bold);
    chSelect.setTooltip ("FX CH SELECT: dónde se aplica el efecto (canal 1, canal 2, lado A o B del crossfader, o el master)"_u);
    chSelect.onClick = [this] { showTargetMenu(); };
    const char* bandNames[] = { "LOW", "MID", "HI" };
    for (int k = 0; k < 3; ++k)
    {
        auto b = std::make_unique<Button> (bandNames[k]);
        b->setTextType (Type::micro, Weight::bold);
        b->setTooltip ("FX FREQUENCY: la banda pasa por el efecto (apagada: queda sin efecto)"_u);
        b->onClick = [this, k] { auto& f = engine.beatFx().bands[(size_t) k]; f = ! f.load(); };
        addAndMakeVisible (*b);
        bandBtns[(size_t) k] = std::move (b);
    }
    fxLevel.setRange (0.0, 1.0);
    fxLevel.setDefault (0.5);
    fxLevel.formatter = [] (double v) { return juce::String ((int) std::round (v * 100)) + "%"; };
    fxLevel.bind ({ [&bx] { return (double) bx.level.load(); }, [&bx] (double v) { bx.level = (float) v; } });
    // TIME: milliseconds instead of the BEAT fraction (REVERB: its amount). Fully left = follow BEAT.
    fxTime.setRange (0.0, 1.0);
    fxTime.setDefault (0.0);
    fxTime.formatter = [this] (double v)
    {
        if ((BeatFx::Type) engine.beatFx().type.load() == BeatFx::Type::reverb) return BeatFx::paramText (BeatFx::Type::reverb, (float) v);
        return v < 0.02 ? juce::String ("BEAT") : juce::String (juce::roundToInt (10.0 * std::pow (1600.0, v))) + " ms";
    };
    fxTime.setTooltip ("TIME: tiempo del efecto en milisegundos (todo a la izquierda: sigue el BEAT; los botones BEAT vuelven al tempo)"_u);
    fxTime.bind ({ [this]
                   {
                       auto& b = engine.beatFx();
                       if ((BeatFx::Type) b.type.load() == BeatFx::Type::reverb) return (double) b.param.load();
                       const float ms = b.timeMs.load();
                       return ms <= 0.0f ? 0.0 : std::log ((double) ms / 10.0) / std::log (1600.0);
                   },
                   [this] (double v)
                   {
                       auto& b = engine.beatFx();
                       if ((BeatFx::Type) b.type.load() == BeatFx::Type::reverb) { b.param = (float) v; return; }
                       b.timeMs = v < 0.02 ? 0.0f : (float) (10.0 * std::pow (1600.0, v));
                   } });
    tapBtn.setTextType (Type::micro, Weight::bold);
    tapBtn.setTooltip ("TAP: tocá al ritmo para marcar el BPM del efecto"_u);
    tapBtn.onClick = [this] { engine.beatFx().tap(); };
    autoBtn.setTextType (Type::micro, Weight::bold);
    autoBtn.setTooltip ("AUTO: el efecto sigue el tempo del tema"_u);
    autoBtn.onClick = [this] { engine.beatFx().tapBpm = 0.0f; };
    xpad.setTooltip ("X-PAD: mantené una posición y el efecto suena con ese BEAT; al soltar vuelve como estaba"_u);

    fxOn.setTextType (Type::label, Weight::bold);
    fxOn.setLitColour (colour (Col::clip));
    fxOn.setTooltip ("BEAT FX ON/OFF"_u);
    fxOn.onClick = [this] { auto& b = engine.beatFx(); b.on = ! b.on.load(); };

    for (auto* c : std::initializer_list<juce::Component*> { &masterKnob, &masterMeter, &cueMix, &phones, &crossfader, &curve, &limBtn,
                                                             &colorParam, &beatDown, &beatUp, &fxSelect, &chSelect, &fxLevel, &fxOn,
                                                             &fxTime, &tapBtn, &autoBtn, &xpad })
        addAndMakeVisible (c);
}

void MixerView::setShowBeatFx (bool s)
{
    showBeatFx = s;
    for (auto* c : std::initializer_list<juce::Component*> { &beatDown, &beatUp, &fxSelect, &chSelect, &fxLevel, &fxOn, &fxTime, &tapBtn, &autoBtn, &xpad })
        c->setVisible (s);
    for (auto& b : bandBtns) b->setVisible (s);
    resized();
    repaint();
}

void MixerView::selectColor (int c)
{
    colorSelected = c;
    for (int d = 0; d < 2; ++d)
    {
        auto& ch = engine.channel (d);
        if (c >= 0) ch.colorType = c;
        ch.colorOn = c >= 0;
    }
}

void MixerView::showFxMenu()
{
    auto& bx = engine.beatFx();
    juce::PopupMenu m;
    m.addSectionHeader ("BEAT FX");
    for (auto t : BeatFx::clubOrder())
        m.addItem (BeatFx::typeName (t), true, bx.type.load() == (int) t, [&bx, t] { bx.select (t); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fxSelect));
}

void MixerView::showTargetMenu()
{
    auto& bx = engine.beatFx();
    juce::PopupMenu m;
    m.addSectionHeader ("FX CH SELECT");
    for (int t = 0; t < AudioEngine::BeatFxSection::numTargets; ++t)
        m.addItem ((t <= 1 ? juce::String ("CANAL ") : juce::String()) + AudioEngine::BeatFxSection::targetName (t), true, bx.target.load() == t,
                   [&bx, t] { bx.target = t; });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chSelect));
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

    s.color.setRange (-1.0, 1.0);
    s.color.setDefault (0.0);
    s.color.setArcColour (deckCol);
    s.color.formatter = [&ch] (double v)
    {
        if (! ch.colorOn.load()) return juce::String ("SIN COLOR FX");
        const auto c = (ChannelStrip::Color) ch.colorType.load();
        if (std::abs (v) <= 0.03) return juce::String ("OFF");
        const auto pct = juce::String ((int) std::round (std::abs (v) * 100)) + "%";
        if (c == ChannelStrip::Color::filter) return juce::String (v < 0 ? "LPF " : "HPF ") + pct;
        return juce::String (v < 0 ? "< " : "> ") + pct;
    };
    s.color.setTooltip ("COLOR: aplica el SOUND COLOR FX encendido (al centro no hace nada)"_u);
    s.color.bind ({ [&ch] { return (double) ch.filter.load(); }, [&ch] (double v) { ch.filter = (float) v; } });

    s.fader.setRange (0.0, 1.0);
    s.fader.setDefault (1.0);
    s.fader.setTicks (11);
    s.fader.bind ({ [&ch] { return (double) ch.fader.load(); }, [&ch] (double v) { ch.fader = (float) v; } });
    s.meter.setSource (&ch.meter);

    s.cue.setTextType (Type::micro, Weight::bold);
    s.cue.setLitColour (colour (Col::cue));
    s.cue.onClick = [&ch] { ch.pfl = ! ch.pfl.load(); };

    s.assign.setCompact (true);
    s.assign.setLitColour (deckCol);
    s.assign.onChange = [&ch] (int v) { ch.xfAssign = v; };

    for (auto* c : std::initializer_list<juce::Component*> { &s.trim, &s.hi, &s.mid, &s.low, &s.color, &s.cue, &s.fader, &s.meter, &s.assign })
        addAndMakeVisible (c);
}

void MixerView::refresh()
{
    const bool phonesOk = engine.hasHeadphoneOutput();

    for (auto& sp : strips)
    {
        auto& s = *sp;
        auto& ch = engine.channel (s.index);
        for (auto* k : { &s.trim, &s.hi, &s.mid, &s.low, &s.color })
            k->syncFromEngine();
        s.fader.syncFromEngine();
        s.meter.update();
        s.assign.setSelected (ch.xfAssign.load());
        s.cue.setToggleState (ch.pfl.load(), juce::dontSendNotification);
        s.cue.setEnabled (phonesOk);
        s.cue.setTooltip (phonesOk ? "CUE: pre-escucha en auriculares"
                                   : "Pre-escucha: requiere una interfaz con 4 salidas (activalas en Configuración › Audio)"_u);
    }

    // SOUND COLOR FX: the lit button follows the engine (a controller may change it too).
    {
        const auto& ch = engine.channel (0);
        colorSelected = ch.colorOn.load() ? ch.colorType.load() : -1;
        using C = ChannelStrip::Color;
        const C order[] = { C::space, C::dubEcho, C::sweep, C::noise, C::crush, C::filter };
        for (size_t k = 0; k < colorBtns.size(); ++k)
            colorBtns[k]->setToggleState ((int) order[k] == colorSelected, juce::dontSendNotification);
        colorParam.syncFromEngine();
    }

    // BEAT FX
    {
        auto& bx = engine.beatFx();
        const auto t = (BeatFx::Type) bx.type.load();
        const auto name = BeatFx::typeName (t), beat = BeatFx::paramText (t, bx.param.load());
        const auto target = AudioEngine::BeatFxSection::targetName (bx.target.load());
        if (name != shownFx || beat != shownBeat || target != shownTarget)
        {
            shownFx = name; shownBeat = beat; shownTarget = target;
            fxSelect.setButtonText (name);
            chSelect.setButtonText (bx.target.load() <= 1 ? "CH " + target : target);
            repaint (fxDisplay);
        }
        for (int k = 0; k < 3; ++k) bandBtns[(size_t) k]->setToggleState (bx.bands[(size_t) k].load(), juce::dontSendNotification);
        fxLevel.syncFromEngine();
        fxTime.syncFromEngine();
        const float tapped = bx.tapBpm.load();
        autoBtn.setToggleState (tapped <= 0.0f, juce::dontSendNotification);
        tapBtn.setToggleState (tapped > 0.0f, juce::dontSendNotification);
        tapBtn.setButtonText (tapped > 0.0f ? juce::String (tapped, 1) : juce::String ("TAP"));
        if ((int) t != lastFxType) { lastFxType = (int) t; xpad.refresh(); }
        // ON/OFF flashes while the effect is on, like the hardware button.
        const bool on = bx.on.load();
        const bool flash = on && (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
        fxOn.setToggleState (on && ! flash ? true : on, juce::dontSendNotification);
        fxOn.setLitColour (flash ? colour (Col::clip).brighter (0.35f) : colour (Col::clip));
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
    auto r = getLocalBounds().reduced (Space::sm, Space::sm);

    // BEAT FX column on the right, full height (as on the mixer).
    const int fxW = juce::jlimit (96, 124, r.getWidth() / 4);
    fxArea = showBeatFx ? r.removeFromRight (fxW) : juce::Rectangle<int>();
    if (showBeatFx) r.removeFromRight (Space::xs);

    // Under the channels, across: crossfader block, and above it SOUND COLOR FX.
    auto xfBlock = r.removeFromBottom (66);
    r.removeFromBottom (Space::xs);
    colorArea = r.removeFromBottom (juce::jlimit (50, 60, r.getHeight() / 6));
    r.removeFromBottom (Space::xs);

    const int sideW = juce::jlimit (54, 66, r.getWidth() / 5);
    masterArea = r.removeFromLeft (sideW);
    r.removeFromLeft (Space::xs);

    const int stripW = (r.getWidth() - Space::xs) / 2;
    const int knobCount = 5;

    for (int i = 0; i < 2; ++i)
    {
        auto& s = *strips[(size_t) i];
        auto col = i == 0 ? r.removeFromLeft (stripW) : r.removeFromRight (stripW);

        col.removeFromTop (16); // channel title painted

        // Fader (with its meter) beside the knobs, on the inner side: the knobs get the full height.
        const int faderColW = Size::faderWidth + 12;
        auto faderCol = i == 0 ? col.removeFromRight (faderColW) : col.removeFromLeft (faderColW);
        faderCol = faderCol.removeFromBottom (juce::jmin (faderCol.getHeight(), juce::jmax (110, faderCol.getHeight() * 11 / 20)));
        auto meterArea = i == 0 ? faderCol.removeFromRight (8) : faderCol.removeFromLeft (8);
        s.meter.setBounds (meterArea.reduced (1, 4));
        s.fader.setBounds (faderCol.withSizeKeepingCentre (Size::faderWidth, faderCol.getHeight()));

        auto cueRow = col.removeFromBottom (Size::controlSm);
        s.cue.setBounds (cueRow.withSizeKeepingCentre (juce::jmin (60, cueRow.getWidth()), Size::controlSm));
        col.removeFromBottom (Space::xs);
        const int kh = juce::jlimit (24, 64, col.getHeight() / knobCount);
        for (auto* k : { &s.trim, &s.hi, &s.mid, &s.low, &s.color })
        {
            k->setLabelOnSide (false);
            k->setBounds (col.removeFromTop (kh).withSizeKeepingCentre (col.getWidth(), kh));
        }
    }

    // The free space above the faders, between the channels: the OpenMix mark.
    {
        const auto& f0 = strips[0]->fader;
        const auto& f1 = strips[1]->fader;
        const int left = f0.getX() - 6, right = f1.getRight() + 6;
        logoArea = juce::Rectangle<int> (left, strips[0]->trim.getY(), right - left, juce::jmin (f0.getY(), f1.getY()) - strips[0]->trim.getY() - Space::sm);
    }

    // SOUND COLOR FX: six buttons (two rows) and PARAMETER
    {
        auto c = colorArea;
        c.removeFromTop (12);                                   // caption
        const int kw = juce::jmin (78, c.getWidth() / 4);
        colorParam.setBounds (c.removeFromRight (kw));
        c.removeFromRight (Space::xs);
        const int bh = (c.getHeight() - 2) / 2, bw = (c.getWidth() - 2 * 2) / 3;
        for (size_t k = 0; k < colorBtns.size(); ++k)
        {
            const int row = (int) k / 3, colIx = (int) k % 3;
            colorBtns[k]->setBounds (c.getX() + colIx * (bw + 2), c.getY() + row * (bh + 2), bw, bh);
        }
    }

    // MASTER / HEADPHONES column
    {
        auto m = masterArea;
        m.removeFromTop (16);
        masterKnob.setBounds (m.removeFromTop (46).withSizeKeepingCentre (m.getWidth(), 44));
        m.removeFromTop (Space::xs);
        auto limRow = m.removeFromTop (18);
        limBtn.setBounds (limRow.removeFromLeft (28));
        limRow.removeFromLeft (3);
        grArea = limRow.reduced (0, 5);
        m.removeFromTop (Space::xs);
        clipLabelArea = m.removeFromTop (14);
        phonesArea = m.removeFromBottom (juce::jlimit (14 + 2 * 24, 14 + 2 * 44, m.getHeight() - 60));
        masterMeter.setBounds (m.withSizeKeepingCentre (14, m.getHeight() - 8));

        auto p = phonesArea;
        p.removeFromTop (14);
        const int kh = juce::jlimit (20, 46, p.getHeight() / 2);
        phones.setBounds (p.removeFromTop (kh).withSizeKeepingCentre (p.getWidth(), kh));
        cueMix.setBounds (p.removeFromTop (kh).withSizeKeepingCentre (p.getWidth(), kh));
    }

    // BEAT FX column
    if (showBeatFx)
    {
        auto f = fxArea;
        f.removeFromTop (16);                                   // caption
        fxDisplay = f.removeFromTop (46);
        f.removeFromTop (Space::xs);
        auto beatRow = f.removeFromTop (Size::controlSm);
        beatDown.setBounds (beatRow.removeFromLeft (beatRow.getWidth() / 2 - 1));
        beatRow.removeFromLeft (2);
        beatUp.setBounds (beatRow);
        f.removeFromTop (Space::xs);
        fxSelect.setBounds (f.removeFromTop (Size::controlSm));
        f.removeFromTop (Space::xs);
        chSelect.setBounds (f.removeFromTop (Size::controlSm));
        f.removeFromTop (Space::sm);
        auto bands = f.removeFromTop (Size::controlXs);
        const int bw = (bands.getWidth() - 4) / 3;
        for (int k = 0; k < 3; ++k)
        {
            bandBtns[(size_t) k]->setBounds (bands.removeFromLeft (bw));
            bands.removeFromLeft (2);
        }
        f.removeFromTop (Space::xs);
        auto tapRow = f.removeFromTop (Size::controlXs);
        autoBtn.setBounds (tapRow.removeFromLeft (tapRow.getWidth() / 2 - 1));
        tapRow.removeFromLeft (2);
        tapBtn.setBounds (tapRow);

        auto onRow = f.removeFromBottom (Size::controlLg + 4);
        fxOn.setBounds (onRow);
        f.removeFromBottom (Space::xs);
        xpad.setBounds (f.removeFromBottom (28));
        f.removeFromBottom (Space::xs);
        // TIME above LEVEL/DEPTH (full width, so the labels fit).
        const int kh = juce::jlimit (36, 62, f.getHeight() / 2);
        auto knobs = f.withSizeKeepingCentre (f.getWidth(), juce::jmin (f.getHeight(), 2 * kh));
        fxTime.setBounds (knobs.removeFromTop (knobs.getHeight() / 2));
        fxLevel.setBounds (knobs);
    }

    // Crossfader block: each channel's assign switch above, the crossfader across, curve below
    {
        auto x = xfBlock;
        auto assignRow = x.removeFromTop (20);
        const int aw = juce::jmin (110, assignRow.getWidth() / 2 - Space::xs);
        strips[0]->assign.setBounds (assignRow.removeFromLeft (assignRow.getWidth() / 2).withSizeKeepingCentre (aw, 20));
        strips[1]->assign.setBounds (assignRow.withSizeKeepingCentre (aw, 20));
        x.removeFromTop (Space::xs);
        crossfader.setBounds (x.removeFromTop (24).reduced (Space::md, 0));
        curve.setBounds (x.withSizeKeepingCentre (juce::jmin (190, x.getWidth()), 16));
    }
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgRaised));
    auto r = getLocalBounds().reduced (Space::sm, Space::sm);

    for (auto& sp : strips)
    {
        const auto b = sp->trim.getBounds();
        auto title = juce::Rectangle<int> (b.getCentreX() - 30, r.getY(), 60, 14).toFloat();
        Text::draw (g, juce::String ("CH ") + juce::String (sp->index + 1), title, font (Type::caption, Weight::bold),
                    Theme::get().deckColour (sp->index), juce::Justification::centred);
    }

    if (logoArea.getWidth() > 20 && logoArea.getHeight() > 20)
    {
        static const auto logo = juce::ImageCache::getFromMemory (BinaryData::logomark_png, BinaryData::logomark_pngSize);
        if (logo.isValid())
        {
            const float side = (float) juce::jmin (logoArea.getWidth(), logoArea.getHeight(), 96) * 0.85f;
            const float w = side * (float) logo.getWidth() / (float) juce::jmax (1, logo.getHeight());
            g.setOpacity (0.9f);
            g.drawImage (logo, juce::Rectangle<float> (w, side).withCentre (logoArea.toFloat().getCentre()), juce::RectanglePlacement::centred);
            g.setOpacity (1.0f);
        }
    }

    Text::caption (g, "MASTER", masterArea.withHeight (14).toFloat(), colour (Col::textSecondary));
    if (showBeatFx)
        Text::caption (g, "BEAT FX", fxArea.withHeight (14).toFloat(), colour (Col::textSecondary));
    Text::caption (g, "SOUND COLOR FX", colorArea.withHeight (12).toFloat(), colour (Col::textTertiary));

    // BEAT FX display: effect, beat and where it sits, like the mixer's screen.
    if (showBeatFx && ! fxDisplay.isEmpty())
    {
        auto d = fxDisplay.toFloat();
        g.setColour (colour (Col::bgField));
        g.fillRoundedRectangle (d, 4.0f);
        g.setColour (colour (Col::line));
        g.drawRoundedRectangle (d.reduced (0.5f), 4.0f, 1.0f);
        auto in = d.reduced (6.0f, 4.0f);
        const bool on = engine.beatFx().on.load();
        Text::draw (g, shownFx, in.removeFromTop (16.0f), font (Type::label, Weight::bold),
                    on ? colour (Col::clip) : colour (Col::textPrimary), juce::Justification::centredLeft);
        auto row = in.removeFromTop (18.0f);
        Text::draw (g, shownBeat, row.removeFromLeft (row.getWidth() * 0.5f), font (Type::deckTitle, Weight::bold),
                    colour (Col::textPrimary), juce::Justification::centredLeft);
        Text::draw (g, shownTarget, row, font (Type::caption, Weight::bold), colour (Col::textSecondary), juce::Justification::centredRight);
    }

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
    Text::caption (g, "AURIC.", phonesArea.withHeight (14).toFloat(),
                   colour (engine.hasHeadphoneOutput() ? Col::textTertiary : Col::textDisabled));

    // Hairlines separating the master and BEAT FX columns
    g.setColour (colour (Col::line));
    g.fillRect (masterArea.getRight() + Space::xs / 2, r.getY(), 1, masterArea.getBottom() - r.getY());
    if (showBeatFx) g.fillRect (fxArea.getX() - Space::xs / 2 - 1, r.getY(), 1, r.getHeight());
    g.fillRect (getLocalBounds().removeFromLeft (1));
    g.fillRect (getLocalBounds().removeFromRight (1));
}

} // namespace om
