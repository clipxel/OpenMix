#include "Actions.h"
#include "../Common/Strings.h"

namespace om
{

Actions::Actions (AudioEngine& e, Hooks h) : engine (e), hooks (std::move (h))
{
    build();
}

const ActionInfo* Actions::find (const juce::String& id) const
{
    auto it = entries.find (id);
    return it != entries.end() ? &it->second.info : nullptr;
}

void Actions::add (ActionInfo info, std::function<void (int, float)> run, std::function<bool (int)> led,
                   std::function<float (int)> value)
{
    infos.push_back (info);
    entries[info.id] = { info, std::move (run), std::move (led), std::move (value) };
}

void Actions::trigger (const juce::String& id, int deck, float value)
{
    auto it = entries.find (id);
    if (it == entries.end())
        return;
    if (it->second.info.perDeck && ! juce::isPositiveAndBelow (deck, AudioEngine::numDecks))
        return;
    it->second.run (deck, value);
}

Actions::Led Actions::ledLevel (const juce::String& id, int deck) const
{
    auto it = entries.find (id);
    if (it == entries.end() || ! (it->second.led || it->second.level))
        return Led::off;
    if (it->second.info.perDeck && ! juce::isPositiveAndBelow (deck, AudioEngine::numDecks))
        return Led::off;
    if (it->second.level)
        return it->second.level (deck);
    return it->second.led (deck) ? Led::on : Led::off;
}

bool Actions::blinkPhase (int deck) const
{
    if (juce::isPositiveAndBelow (deck, AudioEngine::numDecks))
    {
        auto& k = engine.deck (deck);
        const auto grid = k.getGrid();
        if (k.isPlaying() && grid.isValid())
        {
            const double beat = grid.beatAt (k.getPosition());
            return beat - std::floor (beat) < 0.5;
        }
    }
    return (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
}

float Actions::meterLevel (int deck)
{
    auto& m = deck < 0 ? engine.masterMeter() : engine.channel (juce::jlimit (0, AudioEngine::numDecks - 1, deck)).meter;
    auto& held = vu[(size_t) (deck < 0 ? 2 : juce::jlimit (0, 1, deck))];
    const float peak = m.drainController();
    const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
    const float now = juce::jlimit (0.0f, 1.0f, (db + 36.0f) / 36.0f);     // −36 dB … 0 dB
    held = juce::jmax (now, held - 0.04f);                                 // smooth fall-off per frame
    return held;
}

void Actions::setLevel (const juce::String& id, std::function<Led (int)> fn)
{
    auto it = entries.find (id);
    if (it != entries.end())
        it->second.level = std::move (fn);
}

float Actions::currentValue (const juce::String& id, int deck) const
{
    auto it = entries.find (id);
    if (it == entries.end() || ! it->second.value)
        return -1.0f;
    if (it->second.info.perDeck && ! juce::isPositiveAndBelow (deck, AudioEngine::numDecks))
        return -1.0f;
    return it->second.value (deck);
}

void Actions::tick()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    for (int d = 0; d < AudioEngine::numDecks; ++d)
        if (lastJogMs[(size_t) d] > 0.0 && now - lastJogMs[(size_t) d] > 60.0)
        {
            engine.deck (d).jogBend (0.0);
            lastJogMs[(size_t) d] = 0.0;
        }
}

//==============================================================================
void Actions::build()
{
    using T = ControlType;
    auto deck = [this] (int d) -> Deck& { return engine.deck (d); };
    auto pressed = [] (float v) { return v > 0.5f; };

    // Transport --------------------------------------------------------------
    add ({ "play_pause", "Play / Pause", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).togglePlay(); },
         [=] (int d) { return deck (d).isPlaying(); });

    add ({ "cue_button", "Cue", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).cueDown(); else deck (d).cueUp(); },
         [=] (int d) { auto& k = deck (d); return k.hasTrack() && ! k.isPlaying() && std::abs (k.getPosition() - k.getCuePoint()) < 0.02; });

    add ({ "goto_cue_stop", "Volver al cue y parar", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) { deck (d).setPlaying (false); deck (d).seek (deck (d).getCuePoint()); } });

    add ({ "sync", "Sync", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setSync (! deck (d).isSyncOn()); },
         [=] (int d) { return deck (d).isSyncOn(); });

    add ({ "reverse", "Reverse", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setReverse (! deck (d).isReverse()); },
         [=] (int d) { return deck (d).isReverse(); });

    add ({ "censor", "Censor (mantener)", "Transporte", T::button },
         [=] (int d, float v) { deck (d).setCensor (pressed (v)); },
         [=] (int d) { return deck (d).isCensor(); });

    add ({ "brake", "Brake (freno de plato)", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).brakeStop(); },
         [=] (int d) { return deck (d).isBraking(); });

    add ({ "slip_mode", "Slip", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setSlip (! deck (d).isSlip()); },
         [=] (int d) { return deck (d).isSlip(); });

    add ({ "key_lock", "Key lock (master tempo)", "Tonalidad", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setKeyLock (! deck (d).isKeyLock()); },
         [=] (int d) { return deck (d).isKeyLock(); });
    add ({ "key_up", "Subir un semitono", "Tonalidad", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setKeyShift (deck (d).getKeyShift() + 1); },
         [=] (int d) { return deck (d).getKeyShift() > 0; });
    add ({ "key_down", "Bajar un semitono", "Tonalidad", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setKeyShift (deck (d).getKeyShift() - 1); },
         [=] (int d) { return deck (d).getKeyShift() < 0; });
    add ({ "key_reset", "Tonalidad original", "Tonalidad", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setKeyShift (0); });
    add ({ "key_match", "Igualar tonalidad con el otro deck", "Tonalidad", T::button },
         [deck] (int d, float v)
         {
             if (v < 0.5f) return;
             auto mine = deck (d).getTrack(), other = deck (1 - d).getTrack();
             if (mine == nullptr || other == nullptr) return;
             deck (d).setKeyShift (Analysis::semitonesToMatch (mine->meta.key, deck (1 - d).getEffectiveKey()));
         });

    add ({ "quantize", "Quantize", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setQuantize (! deck (d).isQuantizeOn()); },
         [=] (int d) { return deck (d).isQuantizeOn(); });

    add ({ "load", "Cargar pista seleccionada", "Biblioteca", T::button },
         [this] (int d, float v) { if (v > 0.5f && hooks.loadSelected) hooks.loadSelected (d); },
         [=] (int d) { return deck (d).hasTrack(); });

    // Jog / pitch ------------------------------------------------------------
    add ({ "jog", "Jog wheel", "Jog y pitch", T::relative },
         [this, deck] (int d, float revs)
         {
             auto& k = deck (d);
             const double seconds = (double) revs * 1.8;   // 33⅓ rpm platter
             if (k.isScratching())    k.scratchMove (seconds);
             else if (k.isPlaying())  { k.jogBend (juce::jlimit (-1.0, 1.0, (double) revs * 25.0)); lastJogMs[(size_t) d] = juce::Time::getMillisecondCounterHiRes(); }
             else                     k.jogScrub (seconds);
         });

    add ({ "jog_touch", "Jog touch (scratch)", "Jog y pitch", T::button },
         [deck] (int d, float v) { if (deck (d).isVinylMode() || v < 0.5f) deck (d).setScratching (v > 0.5f); },
         [=] (int d) { return deck (d).isScratching(); });

    add ({ "vinyl_mode", "Modo vinilo / CD", "Jog y pitch", T::button },
         [deck] (int d, float v) { if (v > 0.5f) deck (d).setVinylMode (! deck (d).isVinylMode()); },
         [deck] (int d) { return deck (d).isVinylMode(); });

    add ({ "pitch", "Pitch fader", "Jog y pitch", T::absolute },
         // Club player convention, same as the on-screen fader: down (low values) = faster (+), up = slower (−).
         [=] (int d, float v) { auto& k = deck (d); if (! k.isSyncOn()) k.setPitch ((1.0 - v * 2.0) * k.getPitchRange()); },
         {},
         [=] (int d) { auto& k = deck (d); return (float) juce::jlimit (0.0, 1.0, (1.0 - k.getPitch() / k.getPitchRange()) * 0.5); });

    add ({ "pitch_bend_minus", "Pitch bend −"_u, "Jog y pitch", T::button },
         [=] (int d, float v) { deck (d).jogBend (pressed (v) ? -0.35 : 0.0); });
    add ({ "pitch_bend_plus", "Pitch bend +", "Jog y pitch", T::button },
         [=] (int d, float v) { deck (d).jogBend (pressed (v) ? 0.35 : 0.0); });
    add ({ "pitch_reset", "Pitch a 0 %", "Jog y pitch", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setPitch (0.0); });
    add ({ "pitch_minus", "Pitch −0.1 %"_u, "Jog y pitch", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setPitch (deck (d).getPitch() - 0.001); });
    add ({ "pitch_plus", "Pitch +0.1 %", "Jog y pitch", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).setPitch (deck (d).getPitch() + 0.001); });
    add ({ "all_pitch_minus", "Pitch −0.1 % en ambos decks"_u, "Jog y pitch", T::button, false },
         [=] (int, float v) { if (pressed (v)) for (int k = 0; k < AudioEngine::numDecks; ++k) deck (k).setPitch (deck (k).getPitch() - 0.001); });
    add ({ "all_pitch_plus", "Pitch +0.1 % en ambos decks", "Jog y pitch", T::button, false },
         [=] (int, float v) { if (pressed (v)) for (int k = 0; k < AudioEngine::numDecks; ++k) deck (k).setPitch (deck (k).getPitch() + 0.001); });
    add ({ "pitch_range", "Cambiar rango de pitch", "Jog y pitch", T::button },
         [=] (int d, float v)
         {
             if (! pressed (v)) return;
             const double ranges[] = { 0.06, 0.08, 0.16, 0.5, 1.0 };
             auto& k = deck (d);
             double next = ranges[0];
             for (auto r : ranges) if (k.getPitchRange() < r - 1e-6) { next = r; break; }
             k.setPitchRange (next);
         });

    // Cues & loops -----------------------------------------------------------
    for (int i = 0; i < 8; ++i)
    {
        const auto n = juce::String (i + 1);
        add ({ "hot_cue_" + n, "Hot cue " + n, "Hot cues", T::button },
             [=] (int d, float v) { if (pressed (v)) deck (d).hotCue (i); else deck (d).hotCueRelease (i); },
             [=] (int d) { return deck (d).getHotCue (i) >= 0.0; });
        add ({ "delete_hot_cue_" + n, "Borrar hot cue " + n, "Hot cues", T::button },
             [=] (int d, float v) { if (pressed (v)) deck (d).clearHotCue (i); });
        add ({ "saved_loop_" + n, "Loop guardado " + n, "Loops", T::button },
             [=] (int d, float v) { if (pressed (v)) deck (d).triggerSavedLoop (i); },
             [=] (int d) { auto [a, b] = deck (d).getSavedLoop (i); return b > a; });
    }

    add ({ "loop", "Auto loop on/off", "Loops", T::button },
         [=] (int d, float v)
         {
             if (! pressed (v)) return;
             auto& k = deck (d);
             if (k.isLoopActive()) k.exitLoop(); else k.setAutoLoop (k.getAutoLoopSize());
         },
         [=] (int d) { return deck (d).isLoopActive(); });

    add ({ "loop_half", "Loop ½"_u, "Loops", T::button },
         [=] (int d, float v)
         {
             if (! pressed (v)) return;
             auto& k = deck (d);
             if (k.isLoopActive()) k.scaleLoop (0.5); else k.setAutoLoopSize (juce::jmax (1.0 / 32.0, k.getAutoLoopSize() * 0.5));
         });
    add ({ "loop_double", "Loop ×2"_u, "Loops", T::button },
         [=] (int d, float v)
         {
             if (! pressed (v)) return;
             auto& k = deck (d);
             if (k.isLoopActive()) k.scaleLoop (2.0); else k.setAutoLoopSize (juce::jmin (32.0, k.getAutoLoopSize() * 2.0));
         });
    add ({ "loop_in", "Loop in", "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).loopIn(); });
    add ({ "loop_out", "Loop out", "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).loopOut(); });
    add ({ "loop_exit", "Salir del loop", "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).exitLoop(); });
    add ({ "reloop", "Reloop", "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).reloop(); });
    add ({ "loop_move_back", "Mover loop atrás"_u, "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).loopMove (-deck (d).getLoopBeats()); });
    add ({ "loop_move_fwd", "Mover loop adelante", "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).loopMove (deck (d).getLoopBeats()); });
    add ({ "beatjump_back", "Beatjump atrás"_u, "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).beatJump (-deck (d).getAutoLoopSize()); });
    add ({ "beatjump_fwd", "Beatjump adelante", "Loops", T::button }, [=] (int d, float v) { if (pressed (v)) deck (d).beatJump (deck (d).getAutoLoopSize()); });

    for (int i = 0; i < 4; ++i)
    {
        const double size = rollSizes[i];
        const auto label = size < 1.0 ? "1/" + juce::String ((int) std::round (1.0 / size)) : juce::String ("1");
        add ({ "loop_roll_" + juce::String (i + 1), "Loop roll " + label, "Loops", T::button },
             [=] (int d, float v) { deck (d).loopRoll (size, pressed (v)); },
             [=] (int d) { return deck (d).isRolling() && std::abs (deck (d).getRollBeats() - size) < 1e-9; });
    }

    // Mixer ------------------------------------------------------------------
    auto absParam = [this] (const juce::String& id, const juce::String& label, auto getAtomic, float lo, float hi)
    {
        add ({ id, label, "Mixer", ControlType::absolute },
             [getAtomic, lo, hi] (int d, float v) { getAtomic (d) = juce::jmap (v, lo, hi); },
             {},
             [getAtomic, lo, hi] (int d) { return juce::jlimit (0.0f, 1.0f, (getAtomic (d).load() - lo) / (hi - lo)); });
    };
    absParam ("gain",     "Ganancia",  [this] (int d) -> std::atomic<float>& { return engine.channel (d).trimDb; }, -12.0f, 12.0f);
    absParam ("eq_high",  "EQ agudos", [this] (int d) -> std::atomic<float>& { return engine.channel (d).eqHigh; }, 0.0f, 1.0f);
    absParam ("eq_mid",   "EQ medios", [this] (int d) -> std::atomic<float>& { return engine.channel (d).eqMid; }, 0.0f, 1.0f);
    absParam ("eq_low",   "EQ graves", [this] (int d) -> std::atomic<float>& { return engine.channel (d).eqLow; }, 0.0f, 1.0f);
    absParam ("filter",   "Filtro",    [this] (int d) -> std::atomic<float>& { return engine.channel (d).filter; }, -1.0f, 1.0f);
    absParam ("volume",   "Volumen de canal", [this] (int d) -> std::atomic<float>& { return engine.channel (d).fader; }, 0.0f, 1.0f);

    // Pads in the deck's current page (PAD 1–8 / PAD PAGE)
    for (int i = 0; i < 8; ++i)
        add ({ "pad_" + juce::String (i + 1), "Pad " + juce::String (i + 1) + " (página actual)"_u, "Pads", T::button },
             [this, i] (int d, float v) { if (hooks.padPress) hooks.padPress (d, i, v > 0.5f); },
             [this, i] (int d) { return hooks.padLed && hooks.padLed (d, i) >= (int) Led::on; });
    add ({ "pad_page", "Página de pads siguiente"_u, "Pads", T::button },
         [this] (int d, float v) { if (v > 0.5f && hooks.padPage) hooks.padPage (d); });

    add ({ "switch_pfl", "Cambiar pre-escucha (PFL) de deck", "Mixer", T::button, false },
         [this] (int, float v)
         {
             if (v < 0.5f) return;
             const bool toB = engine.channel (0).pfl.load() && ! engine.channel (1).pfl.load();
             engine.channel (0).pfl = ! toB;
             engine.channel (1).pfl = toB;
         });
    add ({ "crossfader_left", "Crossfader a la izquierda", "Mixer", T::button, false },
         [this] (int, float v) { if (v > 0.5f) { engine.autoXfPerSecond = 0.0f; engine.crossfader = juce::jmax (-1.0f, engine.crossfader.load() - 0.1f); } });
    add ({ "crossfader_right", "Crossfader a la derecha", "Mixer", T::button, false },
         [this] (int, float v) { if (v > 0.5f) { engine.autoXfPerSecond = 0.0f; engine.crossfader = juce::jmin (1.0f, engine.crossfader.load() + 0.1f); } });
    add ({ "mix_now", "Mezclar ahora (entra el otro deck en sync y cruza en 8 beats)", "Mixer", T::button, false },
         [this] (int, float v)
         {
             if (v < 0.5f) return;
             const bool p0 = engine.deck (0).isPlaying(), p1 = engine.deck (1).isPlaying();
             if (p0 == p1) return;                                    // needs exactly one deck on air
             const int out = p0 ? 0 : 1, in = 1 - out;
             auto& next = engine.deck (in);
             if (next.getTrack() == nullptr) return;
             next.setSync (true);
             next.setPlaying (true);
             const double bpm = engine.deck (out).getEffectiveBpm();
             engine.startAutoCrossfade (in == 0 ? -1.0f : 1.0f, bpm > 1.0 ? 8.0 * 60.0 / bpm : 6.0);
         });
    add ({ "emergency_play", "Play de emergencia (si no suena nada)", "Transporte", T::button, false },
         [this] (int, float v)
         {
             if (v < 0.5f || engine.deck (0).isPlaying() || engine.deck (1).isPlaying()) return;
             int d = engine.crossfader.load() > 0.0f ? 1 : 0;
             if (engine.deck (d).getTrack() == nullptr) d = 1 - d;
             if (engine.deck (d).getTrack() != nullptr) engine.deck (d).setPlaying (true);
         });

    add ({ "pfl", "Pre-escucha (PFL)", "Mixer", T::button },
         [this] (int d, float v) { if (v > 0.5f) engine.channel (d).pfl = ! engine.channel (d).pfl.load(); },
         [this] (int d) { return engine.channel (d).pfl.load(); });

    for (int u = 0; u < AudioEngine::numFxSlots; ++u)
    {
        const auto n = juce::String (u + 1);
        add ({ "effect_active_" + n, "FX " + n + " on/off", "Efectos", T::button },
             [this, u] (int d, float v) { if (v > 0.5f) engine.fx (d, u).enabled = ! engine.fx (d, u).enabled.load(); },
             [this, u] (int d) { return engine.fx (d, u).enabled.load(); });
        add ({ "effect_wet_" + n, "FX " + n + " wet", "Efectos", T::absolute },
             [this, u] (int d, float v) { engine.fx (d, u).wet = v; }, {},
             [this, u] (int d) { return engine.fx (d, u).wet.load(); });
        add ({ "effect_param_" + n, "FX " + n + " parámetro"_u, "Efectos", T::absolute },
             [this, u] (int d, float v) { engine.fx (d, u).param = v; }, {},
             [this, u] (int d) { return engine.fx (d, u).param.load(); });
        add ({ "effect_select_" + n, "FX " + n + " siguiente efecto", "Efectos", T::button },
             [this, u] (int d, float v)
             {
                 if (v < 0.5f) return;
                 auto& f = engine.fx (d, u);
                 f.enabled = false;
                 const auto next = (BeatFx::Type) ((f.type.load() + 1) % (int) BeatFx::Type::numTypes);
                 f.type = (int) next;
                 f.param = BeatFx::defaultParam (next);
                 f.wet = BeatFx::defaultLevel (next);
             });
    }
    add ({ "instant_doubles", "Instant doubles (copiar el otro deck)", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).cloneFrom (deck (1 - d)); });
    add ({ "master", "Deck maestro", "Transporte", T::button },
         [this] (int d, float v) { if (v > 0.5f) engine.masterDeck = engine.masterDeck.load() == d ? -1 : d; },
         [this] (int d) { return engine.masterDeck.load() == d; });

    add ({ "backspin", "Backspin", "Transporte", T::button },
         [=] (int d, float v) { if (pressed (v)) deck (d).backspinStop(); },
         [=] (int d) { return deck (d).isBackspinning(); });

    auto globalAbs = [this] (const juce::String& id, const juce::String& label, std::atomic<float>& a, float lo, float hi)
    {
        add ({ id, label, "Mixer", ControlType::absolute, false },
             [&a, lo, hi] (int, float v) { a = juce::jmap (v, lo, hi); },
             {},
             [&a, lo, hi] (int) { return juce::jlimit (0.0f, 1.0f, (a.load() - lo) / (hi - lo)); });
    };
    globalAbs ("crossfader",        "Crossfader",            engine.crossfader, -1.0f, 1.0f);
    globalAbs ("master_volume",     "Volumen master",        engine.masterDb, -40.0f, 6.0f);
    globalAbs ("headphone_mix",     "Mezcla auriculares",    engine.cueMix, 0.0f, 1.0f);
    globalAbs ("headphone_volume",  "Volumen auriculares",   engine.phonesDb, -40.0f, 6.0f);

    // Browser & global -------------------------------------------------------
    add ({ "browser_scroll", "Navegar biblioteca", "Biblioteca", T::relative, false },
         [this] (int, float v) { if (hooks.browse && std::abs (v) > 0.0f) hooks.browse (v > 0 ? juce::jmax (1, (int) v) : juce::jmin (-1, (int) v)); });

    add ({ "record", "Grabar", "Global", T::button, false },
         [this] (int, float v) { if (v > 0.5f && hooks.toggleRecord) hooks.toggleRecord(); },
         [this] (int) { return engine.recorder().isRecording(); });

    add ({ "shift", "Shift", "Global", T::button, false }, [] (int, float) {});
    add ({ "automix", "AUTOMIX iniciar / detener", "Global", T::button, false },
         [this] (int, float v) { if (v > 0.5f && hooks.toggleAutoMix) hooks.toggleAutoMix(); });

    // Controller lights ------------------------------------------------------
    add ({ "vu_meter", "Medidor del canal (luces)", "Luces", T::absolute }, [] (int, float) {});
    add ({ "vu_master", "Medidor master (luces)", "Luces", T::absolute, false }, [] (int, float) {});
    add ({ "end_warning", "Aviso de fin de tema (luz)", "Luces", T::button }, [] (int, float) {});
    setLevel ("end_warning", [=] (int d)
    {
        auto& k = deck (d);
        return k.isPlaying() && k.hasTrack() && k.getLength() - k.getPosition() < 30.0 ? Led::blink : Led::off;
    });

    // Same states as the screen: PLAY blinks while a loaded deck is paused, CUE blinks when pressing
    // it would set a new cue point, PADs follow the deck's current pad page.
    setLevel ("play_pause", [=] (int d)
    {
        auto& k = deck (d);
        return k.isPlaying() ? Led::on : k.hasTrack() ? Led::blink : Led::off;
    });
    setLevel ("cue_button", [=] (int d)
    {
        auto& k = deck (d);
        if (! k.hasTrack()) return Led::off;
        if (k.isPlaying()) return Led::off;
        return std::abs (k.getPosition() - k.getCuePoint()) < 0.02 ? Led::on : Led::blink;
    });
    for (auto* id : { "loop_in", "loop_out" })            // the loop being played: both ends blink
        setLevel (id, [=] (int d) { return deck (d).isLoopActive() && ! deck (d).isRolling() ? Led::blink : Led::off; });
    for (auto* id : { "loop_exit", "reloop" })
        setLevel (id, [=] (int d) { return deck (d).isLoopActive() ? Led::on : Led::off; });
    for (int i = 0; i < 8; ++i)
        setLevel ("pad_" + juce::String (i + 1), [this, i] (int d)
        {
            return hooks.padLed ? (Led) juce::jlimit (0, 3, hooks.padLed (d, i)) : Led::off;
        });
}

} // namespace om
