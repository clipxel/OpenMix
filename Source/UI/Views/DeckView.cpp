#include "DeckView.h"
#include "../../Library/Library.h"
#include <thread>

namespace om
{

static juce::String formatTime (double seconds, bool tenths = true)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return tenths ? juce::String::formatted ("%d:%04.1f", m, std::floor (s * 10.0) / 10.0)
                  : juce::String::formatted ("%d:%02d", m, (int) s);
}

static juce::String loopSizeLabel (double beats)
{
    if (beats < 0.99) return "1/" + juce::String ((int) std::round (1.0 / beats));
    return juce::String ((int) std::round (beats));
}

//==============================================================================
/** Beat-grid editor (call-out): nudge, BPM, ×2/÷2, downbeat, tap, reset — all live. */
class GridEditor : public juce::Component, private juce::Timer
{
public:
    explicit GridEditor (Deck& d) : deck (d)
    {
        auto add = [this] (const juce::String& text, const juce::String& tip, std::function<void()> fn)
        {
            auto b = std::make_unique<Button> (text);
            b->setTextType (Type::label, Weight::semibold);
            b->setTooltip (tip);
            b->onClick = std::move (fn);
            addAndMakeVisible (*b);
            buttons.push_back (std::move (b));
            return buttons.back().get();
        };

        bpmField.setFont (font (Type::title, Weight::semibold));
        bpmField.setJustification (juce::Justification::centred);
        bpmField.setInputRestrictions (7, "0123456789.,");
        bpmField.setTooltip ("Escribí el BPM correcto y apretá Enter"_u);
        bpmField.onReturnKey = [this] { applyTyped(); };
        addAndMakeVisible (bpmField);
        applyBtn = add ("APLICAR", "Usar el BPM escrito", [this] { applyTyped(); });
        add ("TAP", "Tocá al ritmo (4 o más toques) para fijar el BPM"_u, [this] { deck.tapTempo(); });

        // The usual analysis mistakes, with the result shown on the button.
        for (auto [f, label] : { std::pair<double, const char*> { 0.5, "÷2" }, { 2.0, "×2" }, { 1.5, "×1,5" }, { 2.0 / 3.0, "×⅔" } })
            factorBtns.push_back ({ add ({}, "Corregir el BPM: "_u + juce::String::fromUTF8 (label), [this, f = f] { bpmTimes (f); }), f, juce::String::fromUTF8 (label) });

        add ("-0.1",  "BPM -0.1",  [this] { bpmBy (-0.1); });
        add ("-0.01", "BPM -0.01", [this] { bpmBy (-0.01); });
        add ("+0.01", "BPM +0.01", [this] { bpmBy (0.01); });
        add ("+0.1",  "BPM +0.1",  [this] { bpmBy (0.1); });
        add ("<< 10 ms", "Mover la grilla 10 ms antes", [this] { deck.shiftGrid (-0.010); });
        add ("< 1 ms",   "Mover la grilla 1 ms antes",  [this] { deck.shiftGrid (-0.001); });
        add ("1 ms >",   "Mover la grilla 1 ms después", [this] { deck.shiftGrid (0.001); });
        add ("10 ms >>", "Mover la grilla 10 ms después", [this] { deck.shiftGrid (0.010); });
        autoBtn = add ("AJUSTE AUTOMÁTICO"_u, "Con el BPM de arriba, encuentra la grilla exacta sobre los bombos de todo el tema"_u,
                       [this] { autoFit(); });
        autoBtn->setLitColour (colour (Col::play));
        add ("½ BEAT"_u, "Correr toda la grilla medio beat (si quedó en el contratiempo, sobre el hi-hat)"_u,
             [this] { const auto g = deck.getGrid(); if (g.isValid()) deck.shiftGrid (g.beatLengthSeconds() * 0.5); });
        add ("1 AQUÍ"_u, "Marcar el beat 1 del compás en la posición actual"_u, [this] { deck.setDownbeatHere(); });
        add ("RESTAURAR", "Volver a la grilla del análisis"_u, [this] { deck.resetGrid(); });

        setSize (440, 226);
        refreshField();
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        const auto grid = deck.getGrid();
        auto r = getLocalBounds().toFloat().reduced (12.0f, 8.0f).removeFromTop (22.0f);
        Text::caption (g, "EDITOR DE BPM Y GRILLA", r.removeFromLeft (190.0f), colour (Col::textTertiary), juce::Justification::centredLeft);
        Text::drawTabular (g, busy ? "Ajustando…"_u
                                   : grid.isValid() ? juce::String (grid.bpm, 2) + " BPM  ·  beat 1 en " + juce::String (grid.firstBeatSeconds * 1000.0, 1) + " ms"
                                                    : juce::String ("Sin grilla"),
                           r, font (Type::body, Weight::semibold), colour (Col::textPrimary), juce::Justification::centredRight);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        r.removeFromTop (28);
        auto line = [&r] (int h) { auto l = r.removeFromTop (h); r.removeFromTop (Space::sm); return l; };
        auto spread = [] (juce::Rectangle<int> l, std::vector<juce::Component*> cs)
        {
            const int w = (l.getWidth() - ((int) cs.size() - 1) * Space::xs) / (int) cs.size();
            for (auto* c : cs) { c->setBounds (l.removeFromLeft (w)); l.removeFromLeft (Space::xs); }
        };
        size_t i = 0;
        auto next = [this, &i] { return (juce::Component*) buttons[i++].get(); };

        auto top = line (Size::controlLg);
        bpmField.setBounds (top.removeFromLeft (140));
        top.removeFromLeft (Space::sm);
        spread (top, { next(), next() });                              // APLICAR, TAP
        spread (line (Size::controlMd), { next(), next(), next(), next() });   // ÷2 ×2 ×1,5 ×⅔
        spread (line (Size::controlMd), { next(), next(), next(), next() });   // fine BPM
        spread (line (Size::controlMd), { next(), next(), next(), next() });   // grid shift
        auto last = line (Size::controlMd);
        auto* autoB = next();
        autoB->setBounds (last.removeFromLeft (last.getWidth() / 2));
        last.removeFromLeft (Space::xs);
        spread (last, { next(), next(), next() });                     // ½ BEAT, 1 AQUÍ, RESTAURAR
    }

private:
    void timerCallback() override
    {
        const auto g = deck.getGrid();
        if (std::abs (g.bpm - shownBpm) > 1.0e-6) refreshField();
        repaint();
    }

    void refreshField()
    {
        const auto g = deck.getGrid();
        shownBpm = g.bpm;
        if (! bpmField.hasKeyboardFocus (true))
            bpmField.setText (g.isValid() ? juce::String (g.bpm, 2) : juce::String(), false);
        for (auto& fb : factorBtns)
            fb.button->setButtonText (g.isValid() ? fb.label + "  " + juce::String (g.bpm * fb.factor, 2) : fb.label);
    }

    void applyTyped()
    {
        const double v = bpmField.getText().replaceCharacter (',', '.').getDoubleValue();
        if (v >= 40.0 && v <= 300.0)
        {
            if (deck.getGrid().isValid()) deck.setBpmKeepingPlayhead (std::round (v * 100.0) / 100.0);
            else deck.setGrid ({ std::round (v * 100.0) / 100.0, deck.getPosition() });
        }
        bpmField.giveAwayKeyboardFocus();
        refreshField();
    }

    void autoFit()
    {
        auto t = deck.getTrack();
        const auto g = deck.getGrid();
        if (t == nullptr || ! g.isValid() || busy) return;
        busy = true;
        autoBtn->setToggleState (true, juce::dontSendNotification);
        juce::Component::SafePointer<GridEditor> self (this);
        Deck* d = &deck;
        std::thread ([t, g, d, self]
        {
            const auto refined = Analysis::refineGrid (t->audio, t->sampleRate, g);
            juce::MessageManager::callAsync ([t, refined, d, self]
            {
                if (d->getTrack() == t) d->setGrid (refined);     // still the same track
                if (self != nullptr) { self->busy = false; self->autoBtn->setToggleState (false, juce::dontSendNotification); self->refreshField(); }
            });
        }).detach();
    }

    void bpmBy (double d)    { const auto g = deck.getGrid(); if (g.isValid()) deck.setBpmKeepingPlayhead (std::round ((g.bpm + d) * 100.0) / 100.0); }
    void bpmTimes (double f) { const auto g = deck.getGrid(); if (g.isValid()) deck.setBpmKeepingPlayhead (std::round (g.bpm * f * 100.0) / 100.0); }

    struct Factor { Button* button; double factor; juce::String label; };

    Deck& deck;
    juce::TextEditor bpmField;
    std::vector<std::unique_ptr<Button>> buttons;
    std::vector<Factor> factorBtns;
    Button* applyBtn = nullptr;
    Button* autoBtn = nullptr;
    double shownBpm = -1.0;
    bool busy = false;
};

static constexpr double padLoopSizes[8] = { 0.25, 0.5, 1, 2, 4, 8, 16, 32 };

//==============================================================================
JogWheel::JogWheel (Deck& d) : deck (d)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void JogWheel::refresh()
{
    const auto p = deck.getPosition();
    if (std::abs (p - lastPos) > 1.0e-9)
    {
        lastPos = p;
        repaint();
    }
}

float JogWheel::angleAt (juce::Point<float> p) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    return std::atan2 (p.x - c.x, c.y - p.y);
}

void JogWheel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 2.0f;
    auto disc = juce::Rectangle<float> (d, d).withCentre (bounds.getCentre());
    const auto deckCol = Theme::get().deckColour (deck.getIndex());
    const bool has = deck.hasTrack();
    const double len = deck.getLength();
    const double pos = deck.getPosition();

    // Outer body
    g.setColour (colour (Col::bgRaised));
    g.fillEllipse (disc);
    g.setColour (colour (Col::line));
    g.drawEllipse (disc.reduced (0.5f), 1.0f);

    // Progress ring (track position) in deck colour
    const float ringW = juce::jmax (4.0f, d * 0.035f);
    auto ring = disc.reduced (ringW * 0.5f + 3.0f);
    juce::Path track;
    track.addCentredArc (ring.getCentreX(), ring.getCentreY(), ring.getWidth() * 0.5f, ring.getHeight() * 0.5f, 0.0f, 0.0f,
                         juce::MathConstants<float>::twoPi, true);
    g.setColour (deckCol.withAlpha (has ? 0.18f : 0.08f));
    g.strokePath (track, juce::PathStrokeType (ringW));

    if (has && len > 0.0)
    {
        const float frac = (float) juce::jlimit (0.0, 1.0, pos / len);
        juce::Path prog;
        prog.addCentredArc (ring.getCentreX(), ring.getCentreY(), ring.getWidth() * 0.5f, ring.getHeight() * 0.5f, 0.0f, 0.0f,
                            juce::MathConstants<float>::twoPi * frac, true);
        const bool ending = deck.isPlaying() && len - pos < 30.0;
        g.setColour (ending ? colour (Col::warning) : deckCol);
        g.strokePath (prog, juce::PathStrokeType (ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Platter
    auto platter = disc.reduced (ringW + 9.0f);
    g.setColour (colour (Col::bgPanel));
    g.fillEllipse (platter);
    g.setColour (colour (Col::line));
    for (float k = 0.25f; k < 0.95f; k += 0.18f)
        g.drawEllipse (platter.reduced (platter.getWidth() * 0.5f * k), 1.0f);

    // Rotating marker: 33⅓ rpm with the real playhead.
    if (has)
    {
        const float a = (float) std::fmod (pos * (33.333 / 60.0), 1.0) * juce::MathConstants<float>::twoPi;
        const auto c = platter.getCentre();
        const float rOut = platter.getWidth() * 0.5f - 3.0f, rIn = rOut - platter.getWidth() * 0.12f;
        g.setColour (colour (Col::textPrimary).withAlpha (0.9f));
        g.drawLine (c.x + std::sin (a) * rIn, c.y - std::cos (a) * rIn, c.x + std::sin (a) * rOut, c.y - std::cos (a) * rOut, 2.5f);
    }

    // Centre: state + remaining
    auto hub = platter.reduced (platter.getWidth() * 0.27f);
    g.setColour (colour (Col::bgRaised));
    g.fillEllipse (hub);
    g.setColour (deckCol.withAlpha (0.5f));
    g.drawEllipse (hub, 1.0f);

    if (has)
    {
        auto inner = hub.reduced (4.0f);
        const auto big = hub.getWidth() > 70.0f ? Type::title : Type::label;
        Text::drawTabular (g, "-" + formatTime (len - pos, false), inner.removeFromTop (inner.getHeight() * 0.58f),
                           font (big, Weight::semibold), colour (Col::textPrimary), juce::Justification::centredBottom);
        Icons::draw (g, deck.isPlaying() ? Icon::play : Icon::pause, inner.withSizeKeepingCentre (9, 9).translated (0, 2),
                     deck.isPlaying() ? colour (Col::play) : colour (Col::textTertiary));
    }
}

void JogWheel::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;
    wasPlaying = deck.isPlaying();
    lastAngle = angleAt (e.position);
    if (deck.isVinylMode())
        deck.setScratching (true);
}

void JogWheel::mouseDrag (const juce::MouseEvent& e)
{
    const float a = angleAt (e.position);
    float delta = a - lastAngle;
    if (delta > juce::MathConstants<float>::pi)  delta -= juce::MathConstants<float>::twoPi;
    if (delta < -juce::MathConstants<float>::pi) delta += juce::MathConstants<float>::twoPi;
    lastAngle = a;

    // One revolution = 1.8 s of audio, like a 33⅓ platter.
    const double seconds = (double) delta / juce::MathConstants<double>::twoPi * 1.8;
    if (deck.isScratching()) deck.scratchMove (seconds);
    else if (wasPlaying)     deck.jogBend (juce::jlimit (-1.0, 1.0, seconds * 12.0));
    else                     deck.jogScrub (seconds);
}

void JogWheel::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    deck.jogBend (0.0);
    deck.setScratching (false);
}

//==============================================================================
DeckView::DeckView (AudioEngine& e, int i)
    : engine (e), deck (e.deck (i)), index (i), overview (deck), jog (deck)
{
    const auto deckCol = Theme::get().deckColour (i);

    for (auto* c : std::initializer_list<juce::Component*> { &overview, &jog, &sync, &cue, &play, &quantize, &keyLock, &slip,
                                                             &pitch, &pitchRange, &pitchZero, &bendDown, &bendUp, &loopHalf, &loopDouble,
                                                             &loopToggle, &loopIn, &loopOut, &jumpBack, &jumpFwd })
        addAndMakeVisible (c);

    // Transport ------------------------------------------------------------
    play.setLitColour (colour (Col::play));
    play.setTooltip ("Reproducir / pausa");
    play.onClick = [this] { deck.togglePlay(); };

    cue.setLitColour (colour (Col::cue));
    cue.setTextType (Type::body, Weight::bold);
    cue.setTooltip ("Cue: en pausa fija el punto (o lo previsualiza mientras se mantiene); en reproducción vuelve al cue"_u);
    cue.onPress   = [this] { deck.cueDown(); };
    cue.onRelease = [this] { deck.cueUp(); };

    sync.setLitColour (colour (Col::sync));
    sync.setTextType (Type::label, Weight::bold);
    sync.setTooltip ("Sincroniza tempo y fase con el otro deck");
    sync.onClick = [this] { deck.setSync (! deck.isSyncOn()); };
    sync.onRightClick = [this] (const juce::MouseEvent&)
    {
        juce::PopupMenu m;
        const bool isMaster = engine.masterDeck.load() == index;
        m.addItem ("Deck maestro", true, isMaster, [this, isMaster] { engine.masterDeck = isMaster ? -1 : index; });
        m.addItem ("Maestro automático", true, engine.masterDeck.load() < 0, [this] { engine.masterDeck = -1; });
        m.addSeparator();
        m.addItem (juce::String::fromUTF8 ("Instant doubles: copiar el deck ") + juce::String (2 - index), ! deck.isPlaying() && engine.deck (1 - index).hasTrack(),
                   false, [this] { deck.cloneFrom (engine.deck (1 - index)); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sync));
    };

    quantize.setTooltip ("Quantize: cues, loops y saltos caen en tiempo (clic derecho: resolución)"_u);
    quantize.onRightClick = [this] (const juce::MouseEvent&)
    {
        juce::PopupMenu m;
        m.addSectionHeader (juce::String::fromUTF8 ("Resolución del quantize"));
        for (auto [label, beats] : { std::pair<const char*, double> { "1 beat", 1.0 }, { "1/2 beat", 0.5 }, { "1/4 beat", 0.25 }, { "1/8 beat", 0.125 } })
            m.addItem (label, true, std::abs (deck.getQuantizeBeats() - beats) < 1e-9, [this, beats] { deck.setQuantizeBeats (beats); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&quantize));
    };
    quantize.onClick = [this] { deck.setQuantize (! deck.isQuantizeOn()); };

    keyLock.setTooltip ("Key lock: cambia el tempo sin cambiar el tono");
    keyLock.onClick = [this] { deck.setKeyLock (! deck.isKeyLock()); };
    slip.setTextType (Type::micro, Weight::bold);
    slip.setTooltip ("Slip: tras un scratch, loop o reverse el tema sigue como si no lo hubieras tocado");
    slip.onClick = [this] { deck.setSlip (! deck.isSlip()); };

    for (auto* b : { &reverseBtn, &brakeBtn, &censorBtn })
    {
        b->setTextType (Type::micro, Weight::bold);
        addAndMakeVisible (*b);
    }
    gridBtn.setTextType (Type::label, Weight::bold);
    gridBtn.setTooltip ("Editor de beatgrid: corregir BPM, fase y beat 1");
    gridBtn.onClick = [this] { openGridEditor(); };
    addAndMakeVisible (gridBtn);

    vinylBtn.setTextType (Type::micro, Weight::bold);
    vinylBtn.setTooltip ("VINYL encendido: tocar el jog hace scratch. Apagado (modo CD): el jog solo empuja o busca");
    vinylBtn.onClick = [this] { deck.setVinylMode (! deck.isVinylMode()); };
    addAndMakeVisible (vinylBtn);
    reverseBtn.setTooltip ("Reverse");
    reverseBtn.onClick = [this] { deck.setReverse (! deck.isReverse()); };
    brakeBtn.setTooltip ("Brake: frena como un tocadiscos al apagarlo");
    brakeBtn.onClick = [this] { deck.brakeStop(); };
    censorBtn.setTooltip ("Censor: reverse mientras se mantiene; al soltar sigue donde estaría"_u);
    censorBtn.onPress = [this] { deck.setCensor (true); };
    censorBtn.onRelease = [this] { deck.setCensor (false); };

    // Pitch -----------------------------------------------------------------
    pitch.setRange (-1.0, 1.0);
    pitch.setDefault (0.0);
    pitch.setTicks (9);
    pitch.setCapColour (deckCol);
    // Club player convention: fader up = slower (−), down = faster (+).
    pitch.bind ({ [this] { return -deck.getPitch() / deck.getPitchRange(); },
                  [this] (double v) { if (! deck.isSyncOn()) deck.setPitch (-v * deck.getPitchRange()); } });
    pitch.setTooltip ("Pitch: hacia abajo más rápido, hacia arriba más lento (doble clic: 0 %)");

    pitchRange.setTextType (Type::caption, Weight::semibold);
    pitchRange.setTooltip ("Rango de pitch (clic para cambiar)");
    pitchRange.onClick = [this] { cyclePitchRange(); };

    pitchZero.setTooltip ("Pitch a 0 %: un toque vuelve de a poco, dos toques vuelve de una"_u);
    pitchZero.onClick = [this] { deck.pitchResetPressed(); };

    bendDown.setTooltip ("Pitch bend −"_u);
    bendUp.setTooltip ("Pitch bend +");
    bendDown.onPress = [this] { deck.jogBend (-0.35); };
    bendUp.onPress   = [this] { deck.jogBend (0.35); };
    bendDown.onRelease = bendUp.onRelease = [this] { deck.jogBend (0.0); };

    // Loops ------------------------------------------------------------------
    loopToggle.setTextType (Type::label, Weight::bold);
    loopToggle.setTooltip ("Auto loop / salir del loop");
    loopToggle.onClick = [this]
    {
        if (deck.isLoopActive()) deck.exitLoop();
        else                     deck.setAutoLoop (deck.getAutoLoopSize());
    };
    loopHalf.onClick = [this]
    {
        if (deck.isLoopActive()) deck.scaleLoop (0.5);
        else deck.setAutoLoopSize (juce::jmax (1.0 / 32.0, deck.getAutoLoopSize() * 0.5));
    };
    loopDouble.onClick = [this]
    {
        if (deck.isLoopActive()) deck.scaleLoop (2.0);
        else deck.setAutoLoopSize (juce::jmin (32.0, deck.getAutoLoopSize() * 2.0));
    };
    loopIn.onClick  = [this] { deck.loopIn(); };
    loopOut.onClick = [this] { deck.loopOut(); };
    loopIn.setTooltip ("Loop in");
    loopOut.setTooltip ("Loop out");
    jumpBack.onClick = [this] { deck.beatJump (-deck.getAutoLoopSize()); };
    jumpFwd.onClick  = [this] { deck.beatJump (deck.getAutoLoopSize()); };
    jumpBack.setTooltip ("Beat jump atrás (tamaño del loop)"_u);
    jumpFwd.setTooltip ("Beat jump adelante (tamaño del loop)"_u);

    // Pads -------------------------------------------------------------------
    const char* tabNames[] = { "HOT CUE", "ROLL", "LOOP", "S.LOOP", "JUMP", "SAMPLES/RMX" };
    for (int t = 0; t < numPadModes; ++t)
    {
        auto b = std::make_unique<Button> (tabNames[t]);
        b->setTextType (Type::micro, Weight::bold);
        b->onClick = [this, t] { setPadMode ((PadMode) t); };
        addAndMakeVisible (*b);
        padTabs[(size_t) t] = std::move (b);
    }

    for (int p = 0; p < 8; ++p)
    {
        auto b = std::make_unique<Button> (juce::String(), Icon::none, Button::Style::pad);
        b->setTextType (Type::label, Weight::bold);
        // Pads act on press (like hardware); release matters for rolls.
        b->onPress   = [this, p] { padDown (p, juce::ModifierKeys::currentModifiers.isShiftDown()); };
        b->onRelease = [this, p] { padUp (p); };
        b->onRightClick = [this, p] (const juce::MouseEvent&) { padRightClick (p); };
        // SAMPLES/RMX: while an X-PAD is held, up / down = tone, left / right = roll speed.
        b->onDrag = [this, p] (const juce::MouseEvent& ev)
        {
            if (padMode != PadMode::sampler || p >= RmxUnit::numDrums) return;
            const float semis = (float) juce::jlimit (-12, 12, juce::roundToInt (-ev.getDistanceFromDragStartY() / 8.0));
            const int steps = juce::jlimit (-2, 2, juce::roundToInt (ev.getDistanceFromDragStartX() / 28.0));
            engine.rmx (index).padGesture (p, semis, steps);
        };
        addAndMakeVisible (*b);
        pads[(size_t) p] = std::move (b);
    }

    auto rollName = [] (double beats) { return "1/" + juce::String (juce::roundToInt (4.0 / beats)); };
    padTone.setRange (-12.0, 12.0, 1.0);
    padTone.setDefault (0.0);
    padTone.setArcColour (Theme::get().deckColour (index));
    padTone.formatter = [] (double v) { return v == 0.0 ? juce::String ("0") : (v > 0 ? "+" : "") + juce::String ((int) v) + " st"; };
    padTone.bind ({ [this] { return (double) engine.rmx (index).getTone(); },
                    [this] (double v) { if (std::abs (v - engine.rmx (index).getTone()) > 0.01) engine.setRmxToneSpeed (index, (float) v, 1.0f); } });
    padTone.setTooltip (juce::String::fromUTF8 ("Tono de los X-PAD (semitonos, sin cambiar la duración). Mientras mantenés un pad: arrastrá arriba / abajo"));
    padRoll.setRange (0.0, 4.0, 1.0);
    padRoll.setDefault (2.0);
    padRoll.setArcColour (Theme::get().deckColour (index));
    padRoll.formatter = [rollName] (double v) { return rollName (std::pow (0.5, v)); };
    padRoll.bind ({ [this] { return std::round (std::log2 (1.0 / engine.rmx (index).getBaseRoll())); },
                    [this] (double v) { engine.rmx (index).setBaseRoll (std::pow (0.5, v)); } });
    padRoll.setTooltip (juce::String::fromUTF8 ("Velocidad del roll al mantener un X-PAD (1/4 … 1/64). Mientras lo mantenés: arrastrá izquierda / derecha"));
    padVol.setRange (0.0, 1.0);
    padVol.setDefault (0.8);
    padVol.setArcColour (Theme::get().deckColour (index));
    padVol.formatter = [] (double v) { return juce::String ((int) std::round (v * 100)) + "%"; };
    padVol.bind ({ [this] { return (double) engine.rmx (index).drumLevel.load(); },
                   [this] (double v) { engine.rmx (index).drumLevel = (float) v; } });
    padVol.setTooltip (juce::String::fromUTF8 ("Volumen de los X-PAD (el tema no cambia)"));
    addChildComponent (padTone);
    addChildComponent (padRoll);
    addChildComponent (padVol);
    setPadMode (PadMode::hotCue);
}

void DeckView::setShowJog (bool s)
{
    showJog = s;
    jog.setVisible (s);
    resized();
    repaint();
}

void DeckView::setPadMode (PadMode m)
{
    padMode = m;
    for (int t = 0; t < numPadModes; ++t)
        padTabs[(size_t) t]->setToggleState (t == (int) m, juce::dontSendNotification);
    padTone.setVisible (m == PadMode::sampler);
    padRoll.setVisible (m == PadMode::sampler);
    padVol.setVisible (m == PadMode::sampler);
    updatePads();
    resized();
}

static constexpr double padRollSizes[8] = { 1.0 / 32, 1.0 / 16, 0.125, 0.25, 0.5, 1, 2, 4 };
static constexpr double padJumpSizes[4] = { 1, 4, 8, 16 };

void DeckView::padDown (int p, bool shift)
{
    switch (padMode)
    {
        case PadMode::hotCue:
            if (shift) deck.clearHotCue (p); else deck.hotCue (p);
            break;
        case PadMode::roll:
            deck.loopRoll (padRollSizes[p], true);
            break;
        case PadMode::loop:
        {
            const double size = padLoopSizes[p];
            if (deck.isLoopActive() && std::abs (deck.getLoopBeats() - size) < 0.01) deck.exitLoop();
            else deck.setAutoLoop (size);
            break;
        }
        case PadMode::savedLoop:
            if (shift) deck.clearSavedLoop (p); else deck.triggerSavedLoop (p);
            break;
        case PadMode::jump:
            deck.beatJump ((p < 4 ? -1.0 : 1.0) * padJumpSizes[p % 4]);
            break;
        case PadMode::sampler:
            engine.rmx (index).padDown (p, shift);
            break;
    }
}

void DeckView::padUp (int p)
{
    if (padMode == PadMode::roll)
        deck.loopRoll (padRollSizes[p], false);
    else if (padMode == PadMode::hotCue)
        deck.hotCueRelease (p);                 // gate cue: release returns to the cue
    else if (padMode == PadMode::sampler)
        engine.rmx (index).padUp (p);
}

void DeckView::padRightClick (int p)
{
    if (padMode == PadMode::sampler && p < RmxUnit::numDrums)
    {
        juce::PopupMenu m;
        m.addSectionHeader ("X-PAD " + RmxUnit::padName (p));
        m.addItem (juce::String::fromUTF8 ("Cargar sample…"), [this, p]
        {
            sampleChooser = std::make_unique<juce::FileChooser> ("Sample para " + RmxUnit::padName (p),
                                                                  juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                                  "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.m4a");
            sampleChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                        [this, p] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f.existsAsFile())
                    engine.loadRmxSample (index, p, f, [this] (bool ok, const juce::String& err) { if (! ok) showMessage (err, true); });
            });
        });
        m.addItem ("Sonido original", engine.rmx (index).sampleName (p).isNotEmpty(), false,
                   [this, p] { engine.loadRmxSample (index, p, {}, nullptr); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pads[(size_t) p].get()));
        return;
    }

    juce::PopupMenu m;
    const auto letter = juce::String (p + 1);
    if (padMode == PadMode::hotCue && deck.getHotCue (p) >= 0.0)
    {
        m.addSectionHeader ("Hot cue " + letter);
        m.addItem ("Borrar", [this, p] { deck.clearHotCue (p); });
    }
    else if (padMode == PadMode::savedLoop && deck.getSavedLoop (p).second > deck.getSavedLoop (p).first)
    {
        m.addSectionHeader ("Loop guardado " + letter);
        m.addItem ("Reemplazar con el loop actual", [this, p] { deck.saveLoop (p); });
        m.addItem ("Borrar", [this, p] { deck.clearSavedLoop (p); });
    }
    else return;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pads[(size_t) p].get()));
}

static juce::String beatsLabel (double beats)
{
    if (beats < 1.0) return "1/" + juce::String ((int) std::round (1.0 / beats));
    return juce::String ((int) std::round (beats));
}

void DeckView::updatePads()
{
    const bool hasTrack = deck.hasTrack();
    const bool hasGrid = hasTrack && deck.getTrack()->grid().isValid();
    using C = std::optional<juce::Colour>;

    for (int p = 0; p < 8; ++p)
    {
        auto& b = *pads[(size_t) p];
        switch (padMode)
        {
            case PadMode::hotCue:
            {
                const auto t = deck.getHotCue (p);
                const auto n = juce::String (p + 1);
                b.setButtonText (n);
                b.setPadColour (t >= 0.0 ? C (Theme::hotCueColour (p)) : std::nullopt);
                padLeds[(size_t) p] = ! hasTrack ? 0 : t >= 0.0 ? 2 : 0;
                b.setSubText (t >= 0.0 ? formatTime (t) : juce::String());
                b.setEnabled (hasTrack);
                b.setTooltip (t >= 0.0 ? "Saltar al hot cue " + n + juce::String::fromUTF8 (" (⇧-clic o clic derecho: borrar)")
                                       : "Fijar hot cue " + n);
                break;
            }
            case PadMode::roll:
            {
                const double size = padRollSizes[p];
                const bool active = deck.isRolling() && std::abs (deck.getRollBeats() - size) < 1e-9;
                b.setButtonText (beatsLabel (size));
                b.setSubText ("ROLL");
                b.setPadColour (active ? C (accent()) : std::nullopt);
                padLeds[(size_t) p] = ! hasGrid ? 0 : active ? 2 : 1;
                b.setEnabled (hasGrid);
                b.setTooltip (hasGrid ? "Loop roll de " + beatsLabel (size) + juce::String::fromUTF8 (" beat mientras se mantiene; al soltar sigue como si nada")
                                      : "Requiere BPM analizado");
                break;
            }
            case PadMode::loop:
            {
                const double size = padLoopSizes[p];
                const bool active = deck.isLoopActive() && ! deck.isRolling() && std::abs (deck.getLoopBeats() - size) < 0.01;
                b.setButtonText (beatsLabel (size));
                b.setSubText ("BEATS");
                b.setPadColour (active ? C (accent()) : std::nullopt);
                padLeds[(size_t) p] = ! hasGrid ? 0 : active ? 3 : 1;
                b.setEnabled (hasGrid);
                b.setTooltip (hasGrid ? "Auto loop de " + beatsLabel (size) + " beats" : "Requiere BPM analizado");
                break;
            }
            case PadMode::savedLoop:
            {
                const auto [ls, le] = deck.getSavedLoop (p);
                const bool set = le > ls;
                const bool active = set && deck.isLoopActive() && std::abs (deck.getLoopStart() - ls) < 1e-6 && std::abs (deck.getLoopEnd() - le) < 1e-6;
                b.setButtonText ("L" + juce::String (p + 1));
                b.setSubText (set ? formatTime (ls) : juce::String());
                b.setPadColour (set ? C (active ? accent() : Theme::hotCueColour (p).withMultipliedSaturation (0.6f)) : std::nullopt);
                padLeds[(size_t) p] = ! hasTrack || ! set ? 0 : active ? 3 : 2;
                b.setEnabled (hasTrack);
                b.setTooltip (set ? juce::String::fromUTF8 ("Activar / salir del loop guardado (⇧-clic: borrar)")
                                  : juce::String ("Guardar el loop actual en este pad"));
                break;
            }
            case PadMode::jump:
            {
                const double size = padJumpSizes[p % 4];
                b.setButtonText ((p < 4 ? juce::String::fromUTF8 ("−") : juce::String ("+")) + beatsLabel (size));
                b.setSubText ("BEATS");
                b.setPadColour (std::nullopt);
                padLeds[(size_t) p] = hasGrid ? 2 : 0;
                b.setEnabled (hasGrid);
                b.setTooltip (hasGrid ? juce::String::fromUTF8 ("Saltar ") + beatsLabel (size) + (p < 4 ? juce::String::fromUTF8 (" beats atrás") : juce::String (" beats adelante"))
                                      : "Requiere BPM analizado");
                break;
            }
            case PadMode::sampler:
            {
                auto& rx = engine.rmx (index);
                const bool drum = p < RmxUnit::numDrums;
                const auto custom = drum ? rx.sampleName (p) : juce::String();
                b.setButtonText (RmxUnit::padName (p));
                const float liveSt = drum ? rx.getTone() + rx.liveTone (p) : 0.0f;
                const bool gesturing = drum && rx.isLit (p) && (std::abs (rx.liveTone (p)) > 0.01f || std::abs (rx.rollRate (p) - rx.getBaseRoll()) > 1.0e-6);
                b.setSubText (gesturing ? (liveSt > 0 ? "+" : "") + juce::String ((int) liveSt) + " st · 1/" + juce::String (juce::roundToInt (4.0 / rx.rollRate (p)))
                            : drum ? (custom.isNotEmpty() ? juce::File (custom).getFileNameWithoutExtension().substring (0, 14) : juce::String ("X-PAD"))
                                   : (p == RmxUnit::brake ? juce::String::fromUTF8 ("⇧ BACKSPIN") : juce::String ("SCENE")));
                const auto base = drum ? Theme::hotCueColour (p) : accent();
                b.setPadColour (rx.isLit (p) ? C (base) : C (base.withMultipliedAlpha (0.28f)));
                padLeds[(size_t) p] = rx.isLit (p) ? 2 : 1;
                b.setEnabled (true);
                b.setTooltip (drum ? juce::String::fromUTF8 ("Suena apenas lo apretás. Mantené: roll en la grilla del tema (VEL.). Mientras lo mantenés, arrastrá arriba/abajo = tono, izquierda/derecha = velocidad del roll. Clic derecho: cargar sample")
                                   : p == RmxUnit::buildUp   ? juce::String::fromUTF8 ("Mantené: el tema sube durante 8 beats (filtro, ruido, cortes en tiempo y eco); soltá para el drop")
                                   : p == RmxUnit::breakDown ? juce::String::fromUTF8 ("Mantené: el tema baja (filtro que se cierra y eco)")
                                   : p == RmxUnit::echoOut   ? juce::String::fromUTF8 ("Repite al instante lo último que sonó y vuelve solo a los 4 beats")
                                                             : juce::String::fromUTF8 ("Mantené: freno de vinilo (⇧: backspin). El tema sigue corriendo"));
                break;
            }
        }
    }
}

void DeckView::cyclePitchRange()
{
    const double ranges[] = { 0.06, 0.08, 0.16, 0.5, 1.0 };
    const auto cur = deck.getPitchRange();
    double next = ranges[0];
    for (int i = 0; i < 5; ++i)
        if (cur < ranges[i] - 1e-6) { next = ranges[i]; break; }
    deck.setPitchRange (next);
}

std::unique_ptr<juce::Component> DeckView::makeGridEditor() { return std::make_unique<GridEditor> (deck); }

void DeckView::openGridEditor()
{
    if (! deck.hasTrack()) return;
    juce::CallOutBox::launchAsynchronously (std::make_unique<GridEditor> (deck), gridBtn.getScreenBounds(), nullptr);
}

void DeckView::mouseDown (const juce::MouseEvent& e)
{
    if (! keyArea.contains (e.getPosition()) || ! deck.hasTrack())
        return;

    juce::PopupMenu m;
    m.addSectionHeader ("Tonalidad");
    m.addItem ("Subir un semitono", [this] { deck.setKeyShift (deck.getKeyShift() + 1); });
    m.addItem ("Bajar un semitono", [this] { deck.setKeyShift (deck.getKeyShift() - 1); });
    m.addItem ("Tonalidad original", deck.getKeyShift() != 0, false, [this] { deck.setKeyShift (0); });
    m.addSeparator();
    const auto other = engine.deck (1 - index).getEffectiveKey();
    m.addItem ("Igualar con el deck " + juce::String (2 - index) + (other.isNotEmpty() ? " (" + other + ")" : juce::String()),
               other.isNotEmpty() && deck.getTrack()->meta.key.isNotEmpty(), false, [this, other]
    {
        deck.setKeyShift (Analysis::semitonesToMatch (deck.getTrack()->meta.key, other));
    });
    m.addItem ("Key lock", true, deck.isKeyLock(), [this] { deck.setKeyLock (! deck.isKeyLock()); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (keyArea)));
}

void DeckView::showMessage (const juce::String& text, bool isError)
{
    message = text;
    messageIsError = isError;
    messageUntil = juce::Time::getMillisecondCounter() + 4000;
    repaint();
}

//==============================================================================
void DeckView::refresh()
{
    overview.refresh();
    if (showJog) jog.refresh();
    pitch.syncFromEngine();
    if (padMode == PadMode::sampler) { padTone.syncFromEngine(); padRoll.syncFromEngine(); padVol.syncFromEngine(); }

    play.setToggleState (deck.isPlaying(), juce::dontSendNotification);
    cue.setToggleState (deck.hasTrack() && ! deck.isPlaying() && std::abs (deck.getPosition() - deck.getCuePoint()) < 0.02,
                        juce::dontSendNotification);
    sync.setToggleState (deck.isSyncOn(), juce::dontSendNotification);
    quantize.setToggleState (deck.isQuantizeOn(), juce::dontSendNotification);
    {
        const double qb = deck.getQuantizeBeats();
        quantize.setButtonText (qb >= 1.0 ? "Q" : "Q" + juce::String ((int) std::round (1.0 / qb)));
    }
    loopToggle.setToggleState (deck.isLoopActive(), juce::dontSendNotification);
    slip.setToggleState (deck.isSlip(), juce::dontSendNotification);
    vinylBtn.setToggleState (deck.isVinylMode(), juce::dontSendNotification);
    keyLock.setToggleState (deck.isKeyLock(), juce::dontSendNotification);
    keyLock.setEnabled (deck.hasTrack());
    reverseBtn.setToggleState (deck.isReverse(), juce::dontSendNotification);
    brakeBtn.setToggleState (deck.isBraking(), juce::dontSendNotification);
    censorBtn.setToggleState (deck.isCensor(), juce::dontSendNotification);
    for (auto* b : { &slip, &reverseBtn, &brakeBtn, &censorBtn, &gridBtn })
        b->setEnabled (deck.hasTrack());

    const auto track = deck.getTrack();
    const bool hasGrid = track != nullptr && track->grid().isValid();
    sync.setEnabled (hasGrid);
    for (auto* b : { &loopToggle, &loopHalf, &loopDouble, &jumpBack, &jumpFwd })
        b->setEnabled (hasGrid);
    for (auto* b : { &cue, &play, &loopIn, &loopOut, &bendDown, &bendUp })
        b->setEnabled (track != nullptr);
    // 0 % dot: blinks while gliding back, dim when the pitch is already at 0 %.
    pitchZero.setState (deck.isPitchGliding() && (juce::Time::getMillisecondCounter() / 250) % 2 == 0,
                        std::abs (deck.getPitch()) < 1.0e-6, Theme::get().deckColour (index));

    const auto loopSize = deck.isLoopActive() ? deck.getLoopBeats() : deck.getAutoLoopSize();
    loopToggle.setButtonText ("LOOP " + loopSizeLabel (loopSize));
    pitchRange.setButtonText (juce::String::fromUTF8 ("±") + juce::String ((int) std::round (deck.getPitchRange() * 100.0)) + "%");

    updatePads();

    // Header repaint only when visible text changes (times at 0.1 s resolution).
    const auto pos = deck.getPosition();
    const auto key = juce::String (deck.getEffectiveBpm(), 2) + "|" + juce::String (deck.getPitch(), 5) + "|"
                   + juce::String ((int) (pos * 10.0)) + "|" + juce::String ((juce::pointer_sized_int) track.get())
                   + "|" + juce::String ((int) engine.isLoading (index)) + "|" + juce::String ((int) deck.isSyncOn()) + "|" + juce::String (deck.getKeyShift()) + "|" + juce::String (engine.masterDeck.load())
                   + "|" + juce::String ((int) (juce::Time::getMillisecondCounter() < messageUntil));
    if (key != lastHeaderKey)
    {
        lastHeaderKey = key;
        repaint (headerArea);
        repaint (pitchValueArea);
    }
}

//==============================================================================
void DeckView::resized()
{
    auto r = getLocalBounds().reduced (Space::md, Space::sm);
    const int gap = Space::sm;

    headerArea = r.removeFromTop (dense ? 40 : 46);
    r.removeFromTop (Space::xs);
    overview.setBounds (r.removeFromTop (dense ? 22 : 28));
    r.removeFromTop (gap);

    // Pads block at the bottom: tabs + 2 rows
    const int padRowH = juce::jlimit (26, 40, (r.getHeight() - 150) / 3);
    padArea = r.removeFromBottom (16 + 4 + padRowH * 2 + 4);
    r.removeFromBottom (gap);
    auto loopRow = r.removeFromBottom (Size::controlMd);
    r.removeFromBottom (gap);
    bodyArea = r;

    // Pitch column (right)
    auto pitchCol = r.removeFromRight (48);
    pitchRange.setBounds (pitchCol.removeFromTop (18));
    auto bendRow = pitchCol.removeFromBottom (20);
    bendDown.setBounds (bendRow.removeFromLeft (19));
    bendUp.setBounds (bendRow.removeFromRight (19));
    pitchValueArea = pitchCol.removeFromBottom (16);
    pitch.setBounds (pitchCol.withTrimmedLeft (12).reduced (3, 2));
    // The 0 % dot: left of the fader, at its centre (the 0 % position).
    pitchZero.setBounds (juce::Rectangle<int> (14, 14).withCentre ({ pitchCol.getX() + 7, pitch.getBounds().getCentreY() }));
    r.removeFromRight (gap);

    if (showJog)
    {
        // Transport column (left), jog in the middle
        auto col = r.removeFromLeft (juce::jlimit (72, 96, r.getWidth() / 5));
        r.removeFromLeft (gap);
        const int tH = juce::jlimit (30, Size::transport + 4, (col.getHeight() - 52) / 2);
        play.setBounds (col.removeFromBottom (tH));
        col.removeFromBottom (gap);
        cue.setBounds (col.removeFromBottom (tH));
        col.removeFromBottom (gap);
        sync.setBounds (col.removeFromTop (Size::controlMd));
        col.removeFromTop (Space::xs);
        auto row3 = [&col] (Button& a, Button& b, Button& c)
        {
            auto small = col.removeFromTop (Size::controlSm);
            const int sw = (small.getWidth() - 2 * Space::xs) / 3;
            a.setBounds (small.removeFromLeft (sw));
            small.removeFromLeft (Space::xs);
            b.setBounds (small.removeFromLeft (sw));
            small.removeFromLeft (Space::xs);
            c.setBounds (small);
            col.removeFromTop (Space::xs);
        };
        row3 (quantize, keyLock, slip);

        // REV / BRAKE / CENSOR in the jog area's top-left corner (mirroring VINYL on the right),
        // wide enough for their full names.
        vinylBtn.setBounds (r.getRight() - 64, r.getY(), 64, Size::controlSm);
        {
            auto perf = juce::Rectangle<int> (r.getX(), r.getY(), 64, 3 * Size::controlSm + 2 * Space::xs);
            for (auto* b : { &reverseBtn, &brakeBtn, &censorBtn })
            {
                b->setBounds (perf.removeFromTop (Size::controlSm));
                perf.removeFromTop (Space::xs);
                b->setVisible (true);
            }
            vinylBtn.setVisible (true);
        }
        const int d = juce::jmin (r.getWidth() - (r.getWidth() > r.getHeight() + 110 ? 0 : 60), r.getHeight());
        jog.setBounds (juce::Rectangle<int> (d, d).withCentre (r.getCentre()));
    }
    else
    {
        // Compact: transport as a row, maximum room elsewhere for waveforms.
        auto row = r.removeFromBottom (juce::jmin (Size::transport, r.getHeight()));
        r.removeFromBottom (Space::xs);
        const int w = (row.getWidth() - gap * 2) / 3;
        cue.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (gap);
        play.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (gap);
        auto right = row;
        sync.setBounds (right.removeFromTop (right.getHeight() / 2 - 1));
        right.removeFromTop (2);
        const int sw = (right.getWidth() - 2 * Space::xs) / 3;
        quantize.setBounds (right.removeFromLeft (sw));
        right.removeFromLeft (Space::xs);
        keyLock.setBounds (right.removeFromLeft (sw));
        right.removeFromLeft (Space::xs);
        slip.setBounds (right);

        // Performance transport row above the main transport (only when it fits whole: on very short
        // screens these stay on the keyboard and the controller).
        const bool perfFits = r.getHeight() >= Size::controlSm;
        for (auto* b : { &reverseBtn, &brakeBtn, &censorBtn, &vinylBtn }) b->setVisible (perfFits);
        if (! perfFits) return layoutLoopAndPads (loopRow, padRowH);
        auto perf = r.removeFromBottom (Size::controlSm);
        const int pw = (perf.getWidth() - 3 * Space::xs) / 4;
        reverseBtn.setBounds (perf.removeFromLeft (pw));
        perf.removeFromLeft (Space::xs);
        brakeBtn.setBounds (perf.removeFromLeft (pw));
        perf.removeFromLeft (Space::xs);
        censorBtn.setBounds (perf.removeFromLeft (pw));
        perf.removeFromLeft (Space::xs);
        vinylBtn.setBounds (perf);
    }

    layoutLoopAndPads (loopRow, padRowH);
}

void DeckView::layoutLoopAndPads (juce::Rectangle<int> loopRow, int padRowH)
{
    const int gap = Space::sm;

    // Loop row
    {
        auto lr = loopRow;
        loopToggle.setBounds (lr.removeFromLeft (juce::jmin (84, lr.getWidth() / 4)));
        lr.removeFromLeft (Space::xs);
        loopHalf.setBounds (lr.removeFromLeft (30));
        lr.removeFromLeft (Space::xs);
        loopDouble.setBounds (lr.removeFromLeft (30));
        lr.removeFromLeft (gap);
        loopIn.setBounds (lr.removeFromLeft (38));
        lr.removeFromLeft (Space::xs);
        loopOut.setBounds (lr.removeFromLeft (38));
        jumpFwd.setBounds (lr.removeFromRight (30));
        lr.removeFromRight (Space::xs);
        jumpBack.setBounds (lr.removeFromRight (30));
        lr.removeFromRight (Space::sm);
        gridBtn.setBounds (lr.removeFromRight (juce::jmin (52, lr.getWidth())));
        loopLabelArea = {};
    }

    // Pads
    {
        auto pa = padArea;
        auto tabs = pa.removeFromTop (16);
        const int tw = (tabs.getWidth() - (numPadModes - 1) * Space::xs) / numPadModes;
        for (int t = 0; t < numPadModes; ++t)
        {
            padTabs[(size_t) t]->setBounds (tabs.removeFromLeft (tw));
            tabs.removeFromLeft (Space::xs);
        }
        pa.removeFromTop (4);
        if (padMode == PadMode::sampler)
        {
            // TONO · VEL. · VOL beside the pads, same size, one row.
            const int kw = juce::jlimit (34, 48, padRowH + 8);
            auto knobs = pa.removeFromRight (kw * 3 + Space::xs * 2);
            pa.removeFromRight (Space::sm);
            const int kh = juce::jmin (knobs.getHeight(), kw + 18);
            for (auto* k : { &padTone, &padRoll, &padVol })
            {
                k->setBounds (knobs.removeFromLeft (kw).withSizeKeepingCentre (kw, kh));
                knobs.removeFromLeft (Space::xs);
            }
        }
        const int pw = (pa.getWidth() - 3 * Space::sm) / 4;
        for (int row = 0; row < 2; ++row)
        {
            auto line = pa.removeFromTop (padRowH);
            for (int c = 0; c < 4; ++c)
            {
                pads[(size_t) (row * 4 + c)]->setBounds (line.removeFromLeft (pw));
                line.removeFromLeft (Space::sm);
            }
            pa.removeFromTop (4);
        }
    }
}

void DeckView::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgPanel));

    const auto deckCol = Theme::get().deckColour (index);
    const auto track = deck.getTrack();
    auto h = headerArea.toFloat();

    // Right block: BPM (largest number on the deck), then time, then key.
    auto bpmBlock = h.removeFromRight (92.0f);
    const auto bpm = deck.getEffectiveBpm();
    const bool hasBpm = bpm > 0.0;
    const bool isMaster = engine.masterDeck.load() == index;
    Text::caption (g, isMaster ? "BPM · MASTER"_u : deck.isSyncOn() ? "BPM · SYNC"_u : juce::String ("BPM"), bpmBlock.removeFromTop (12.0f),
                   isMaster || deck.isSyncOn() ? colour (Col::sync) : colour (Col::textTertiary), juce::Justification::centredRight);
    Text::drawTabular (g, hasBpm ? juce::String (bpm, 2) : "--.--", bpmBlock.removeFromTop (26.0f),
                       font (Type::display, Weight::semibold), hasBpm ? colour (Col::textPrimary) : colour (Col::textDisabled),
                       juce::Justification::centredRight);
    if (track != nullptr)
    {
        const auto pct = deck.getPitch() * 100.0;
        Text::drawTabular (g, (pct >= 0 ? "+" : "") + juce::String (pct, 2) + "%", bpmBlock, font (Type::label, Weight::medium),
                           std::abs (pct) > 0.005 ? deckCol : colour (Col::textTertiary), juce::Justification::centredRight);
    }

    h.removeFromRight (16.0f);
    auto timeBlock = h.removeFromRight (78.0f);
    if (track != nullptr)
    {
        const auto pos = deck.getPosition();
        Text::caption (g, "RESTANTE", timeBlock.removeFromTop (12.0f), colour (Col::textTertiary), juce::Justification::centredRight);
        Text::drawTabular (g, "-" + formatTime (deck.getLength() - pos), timeBlock.removeFromTop (20.0f),
                           font (Type::timer, Weight::medium), colour (Col::textPrimary), juce::Justification::centredRight);
        Text::drawTabular (g, formatTime (pos), timeBlock, font (Type::label), colour (Col::textSecondary), juce::Justification::centredRight);
    }

    h.removeFromRight (16.0f);
    auto keyBlock = h.removeFromRight (56.0f);
    keyArea = keyBlock.toNearestInt();
    const auto shift = deck.getKeyShift();
    Text::caption (g, shift != 0 ? "KEY " + juce::String (shift > 0 ? "+" : "") + juce::String (shift) : juce::String ("KEY"),
                   keyBlock.removeFromTop (12.0f), shift != 0 ? accent() : colour (Col::textTertiary), juce::Justification::centredRight);
    const auto keyText = deck.getEffectiveKey().isNotEmpty() ? deck.getEffectiveKey() : juce::String ("—"_u);
    Text::draw (g, keyText, keyBlock.removeFromTop (20.0f), font (Type::body, Weight::semibold),
                colour (keyText == "—"_u ? Col::textDisabled : Col::textPrimary), juce::Justification::centredRight);
    if (keyText != "—"_u)
        Text::draw (g, Analysis::camelotToMusical (keyText), keyBlock, font (Type::label), colour (Col::textSecondary),
                    juce::Justification::centredRight);

    // Left block: deck label, title, artist
    h.removeFromRight (12.0f);
    auto label = h.removeFromTop (14.0f);
    Text::draw (g, juce::String ("DECK ") + (index == 0 ? "1" : "2"), label, font (Type::caption, Weight::bold), deckCol);

    if (engine.isLoading (index))
    {
        Text::draw (g, juce::String::fromUTF8 ("Cargando y analizando…"), h.removeFromTop (20.0f), font (Type::deckTitle, Weight::semibold),
                    colour (Col::textSecondary));
    }
    else if (track != nullptr)
    {
        const auto title = track->meta.title.isNotEmpty() ? track->meta.title : track->file.getFileNameWithoutExtension();
        Text::draw (g, title, h.removeFromTop (20.0f), font (Type::deckTitle, Weight::semibold), colour (Col::textPrimary));
        auto artistLine = track->meta.artist.isNotEmpty() ? track->meta.artist : juce::String ("Artista desconocido");
        if (! track->grid().isValid())
            artistLine << juce::String::fromUTF8 ("  ·  sin BPM detectado");
        Text::draw (g, artistLine, h, font (Type::label), colour (Col::textSecondary));
    }
    else
    {
        Text::draw (g, juce::String::fromUTF8 ("Vacío"), h.removeFromTop (20.0f), font (Type::deckTitle, Weight::semibold),
                    colour (Col::textDisabled));
        Text::draw (g, juce::String::fromUTF8 ("Arrastrá una pista o hacé doble clic en la biblioteca"), h, font (Type::label),
                    colour (Col::textTertiary));
    }

    // Pitch value under the fader
    if (! pitchValueArea.isEmpty())
        Text::drawTabular (g, juce::String (deck.getPitch() * 100.0, 1), pitchValueArea.toFloat(), font (Type::micro, Weight::semibold),
                           colour (Col::textSecondary), juce::Justification::centred);

    // Separator between deck and its surroundings
    g.setColour (colour (Col::line));
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

void DeckView::paintOverChildren (juce::Graphics& g)
{
    if (juce::Time::getMillisecondCounter() < messageUntil && message.isNotEmpty())
    {
        auto r = headerArea.toFloat().withTrimmedTop (16.0f).withHeight (24.0f);
        r = r.withWidth (juce::jmin (r.getWidth(), juce::GlyphArrangement::getStringWidth (font (Type::label, Weight::medium), message) + 34.0f));
        g.setColour (colour (Col::bgMenu));
        g.fillRoundedRectangle (r, Radius::md);
        const auto c = messageIsError ? colour (Col::error) : colour (Col::warning);
        g.setColour (c);
        g.drawRoundedRectangle (r.reduced (0.5f), Radius::md, 1.0f);
        auto inner = r.reduced (8.0f, 0.0f);
        Icons::draw (g, messageIsError ? Icon::error : Icon::warning, inner.removeFromLeft (12.0f).withSizeKeepingCentre (12, 12), c);
        inner.removeFromLeft (6.0f);
        Text::draw (g, message, inner, font (Type::label, Weight::medium), colour (Col::textPrimary));
    }

    if (dragOver)
    {
        g.setColour (Theme::get().deckColour (index).withAlpha (0.12f));
        g.fillRect (getLocalBounds());
        g.setColour (Theme::get().deckColour (index));
        g.drawRect (getLocalBounds(), 2);
    }
}

//==============================================================================
bool DeckView::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && Library::isAudioFile (juce::File (files[0]));
}

void DeckView::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint();
    if (onLoadRequest && files.size() > 0)
        onLoadRequest (juce::File (files[0]));
}

bool DeckView::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString().startsWith ("track:");
}

void DeckView::itemDropped (const SourceDetails& d)
{
    dragOver = false;
    repaint();
    if (onLoadRequest)
        onLoadRequest (juce::File (d.description.toString().fromFirstOccurrenceOf ("track:", false, false)));
}

} // namespace om
