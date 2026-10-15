#include "MappingWizard.h"
#include "../Common/Strings.h"

namespace om
{

static constexpr double gestureEndSeconds = 0.6;   // a fader / jog step ends this long after it stops moving

MappingWizard::MappingWizard (const Actions& a, const juce::String& deviceName) : actions (a), device (deviceName)
{
    auto u = [] (const char* s) { return juce::String::fromUTF8 (s); };
    // Deck 1 (left). Deck 2 is mirrored from these, so this list is the only one the user goes through.
    deckSteps = {
        { "play_pause",  0, u ("Apretá PLAY del deck izquierdo"), false },
        { "cue_button",  0, u ("Apretá CUE del deck izquierdo") },
        { "sync",        0, u ("Apretá SYNC del deck izquierdo") },
        { "hot_cue_1",   0, u ("Apretá el pad 1 (hot cue 1) del deck izquierdo") },
        { "hot_cue_2",   0, u ("Apretá el pad 2 del deck izquierdo") },
        { "hot_cue_3",   0, u ("Apretá el pad 3 del deck izquierdo") },
        { "hot_cue_4",   0, u ("Apretá el pad 4 del deck izquierdo") },
        { "hot_cue_5",   0, u ("Apretá el pad 5 del deck izquierdo (si tiene)") },
        { "hot_cue_6",   0, u ("Apretá el pad 6 del deck izquierdo (si tiene)") },
        { "hot_cue_7",   0, u ("Apretá el pad 7 del deck izquierdo (si tiene)") },
        { "hot_cue_8",   0, u ("Apretá el pad 8 del deck izquierdo (si tiene)") },
        { "loop",        0, u ("Apretá el botón de LOOP (auto loop) del deck izquierdo") },
        { "loop_half",   0, u ("Apretá el botón para achicar el loop (½) del deck izquierdo") },
        { "loop_double", 0, u ("Apretá el botón para agrandar el loop (×2) del deck izquierdo") },
        { "jog",         0, u ("Girá el jog del deck izquierdo por el borde, para un lado y para el otro") },
        { "pitch",       0, u ("Mové el pitch (tempo) del deck izquierdo de arriba hasta abajo") },
        { "load",        0, u ("Apretá LOAD del deck izquierdo") },
        { "gain",        0, u ("Girá la ganancia (TRIM) del canal izquierdo") },
        { "eq_high",     0, u ("Girá el EQ de agudos (HI) del canal izquierdo") },
        { "eq_mid",      0, u ("Girá el EQ de medios (MID) del canal izquierdo") },
        { "eq_low",      0, u ("Girá el EQ de graves (LOW) del canal izquierdo") },
        { "filter",      0, u ("Girá el filtro (FILTER / CFX) del canal izquierdo") },
        { "volume",      0, u ("Mové el fader de volumen del canal izquierdo") },
        { "pfl",         0, u ("Apretá el botón de auriculares (CUE / PFL) del canal izquierdo") },
    };
    globalSteps = {
        { "crossfader",       -1, u ("Mové el crossfader") },
        { "browser_scroll",   -1, u ("Girá la perilla de navegación, para un lado y para el otro") },
        { "master_volume",    -1, u ("Girá el volumen master (si tiene)") },
        { "headphone_volume", -1, u ("Girá el volumen de auriculares (si lo maneja por MIDI)") },
        { "shift",            -1, u ("Apretá SHIFT (si tiene)") },
    };
}

const std::vector<MappingWizard::Step>& MappingWizard::activeSteps() const
{
    return currentPhase == Phase::globals ? globalSteps : deckSteps;
}

const MappingWizard::Step* MappingWizard::current() const
{
    if (currentPhase != Phase::deckOne && currentPhase != Phase::deckTwoManual && currentPhase != Phase::globals)
        return nullptr;
    const auto& steps = activeSteps();
    return juce::isPositiveAndBelow (index, (int) steps.size()) ? &steps[(size_t) index] : nullptr;
}

int MappingWizard::stepCount() const noexcept
{
    return (int) (deckSteps.size() + globalSteps.size()) + 2;
}

juce::String MappingWizard::prompt() const
{
    switch (currentPhase)
    {
        case Phase::deckTwoProbe:  return juce::String::fromUTF8 ("Ahora apretá PLAY del deck derecho");
        case Phase::deckTwoCheck:  return juce::String::fromUTF8 ("Para confirmar, apretá CUE del deck derecho");
        case Phase::finished:      return juce::String::fromUTF8 ("¡Listo! El controlador quedó mapeado");
        case Phase::deckTwoManual:
            if (auto* s = current()) return s->prompt.replace ("izquierdo", "derecho");
            return {};
        case Phase::deckOne:
        case Phase::globals:
            if (auto* s = current()) return s->prompt;
            return {};
    }
    return {};
}

bool MappingWizard::alreadyMapped (const juce::MidiMessage& m) const
{
    for (auto& b : result)
    {
        if (m.getChannel() != b.channel) continue;
        if (b.kind == MidiBinding::Kind::note && (m.isNoteOn() || m.isNoteOff()) && m.getNoteNumber() == b.number) return true;
        if (b.kind == MidiBinding::Kind::cc && m.isController()
            && (m.getControllerNumber() == b.number || m.getControllerNumber() == b.lsbNumber)) return true;
        if (b.kind == MidiBinding::Kind::pitchWheel && m.isPitchWheel()) return true;
    }
    return false;
}

bool MappingWizard::feed (const juce::MidiMessage& m, double t)
{
    poll (t);

    // Deck 2: one press tells how the decks differ; one more confirms it.
    if (currentPhase == Phase::deckTwoProbe || currentPhase == Phase::deckTwoCheck)
    {
        const bool press = (m.isNoteOn() && m.getVelocity() > 0) || (m.isController() && m.getControllerValue() > 0);
        if (! press) return false;
        const int num = m.isNoteOn() ? m.getNoteNumber() : m.getControllerNumber();
        const auto kind = m.isNoteOn() ? MidiBinding::Kind::note : MidiBinding::Kind::cc;

        if (currentPhase == Phase::deckTwoProbe)
        {
            if (kind != probe.kind || (m.getChannel() == probe.channel && num == probe.number))
                return false;                                   // the same deck-1 button again
            channelOffset = m.getChannel() - probe.channel;
            numberOffset = num - probe.number;
            mirrorDeckTwo();
            return true;
        }

        const bool ok = kind == checkExpected.kind && m.getChannel() == checkExpected.channel && num == checkExpected.number;
        if (ok)
        {
            currentPhase = Phase::globals;
            index = 0;
        }
        else
        {
            // The decks are not mirror images: drop the guess and ask deck 2 control by control.
            result.erase (std::remove_if (result.begin(), result.end(), [] (const MidiBinding& b) { return b.deck == 1; }), result.end());
            currentPhase = Phase::deckTwoManual;
            index = 0;
        }
        return true;
    }

    const auto* step = current();
    if (step == nullptr) return false;
    const auto* info = actions.find (step->action);
    if (info == nullptr) { advance(); return false; }

    const int deck = currentPhase == Phase::deckTwoManual ? 1 : step->deck;

    if (info->type == ControlType::button)
    {
        const bool press = (m.isNoteOn() && m.getVelocity() > 0) || (m.isController() && m.getControllerValue() > 0);
        if (! press || alreadyMapped (m)) return false;
        MidiBinding b;
        b.device = device;
        b.channel = m.getChannel();
        b.action = step->action;
        b.deck = info->perDeck ? deck : -1;
        b.kind = m.isNoteOn() ? MidiBinding::Kind::note : MidiBinding::Kind::cc;
        b.number = m.isNoteOn() ? m.getNoteNumber() : m.getControllerNumber();
        b.encoding = MidiBinding::Encoding::button;
        result.push_back (b);
        advance();
        return true;
    }

    // Fader, knob or jog: collect the whole gesture, then decide its encoding.
    if (! (m.isController() || m.isPitchWheel()) || alreadyMapped (m))
        return false;
    if (! pending.any)
    {
        pending = {};
        pending.any = true;
        auto& b = pending.binding;
        b.device = device;
        b.channel = m.getChannel();
        b.action = step->action;
        b.deck = info->perDeck ? deck : -1;
        b.kind = m.isPitchWheel() ? MidiBinding::Kind::pitchWheel : MidiBinding::Kind::cc;
        b.number = m.isPitchWheel() ? 0 : m.getControllerNumber();
    }
    if (m.getChannel() != pending.binding.channel)
        return false;
    if (m.isController())
    {
        pending.ccSeen.insert (m.getControllerNumber());
        if (m.getControllerNumber() == pending.binding.number && pending.values.size() < 64)
            pending.values.push_back (m.getControllerValue());
    }
    pending.lastTime = t;
    return false;
}

void MappingWizard::poll (double t)
{
    if (pending.any && t - pending.lastTime > gestureEndSeconds)
        finishPending();
}

void MappingWizard::finishPending()
{
    const auto p = pending;
    pending = {};
    auto b = p.binding;
    const auto* info = actions.find (b.action);
    if (info == nullptr) return;

    if (b.kind == MidiBinding::Kind::pitchWheel)
    {
        b.encoding = MidiBinding::Encoding::absolute7;          // 14-bit pitch-bend message, read as a fader
    }
    else if (info->type == ControlType::relative)
    {
        // Turned both ways: offset-64 sends 63/65, two's complement 1/127, sign-bit 1/65.
        auto any = [&p] (int lo, int hi) { return std::any_of (p.values.begin(), p.values.end(), [=] (int v) { return v >= lo && v <= hi; }); };
        const bool lowSmall = any (1, 15), highMid = any (65, 79), midLow = any (49, 63), veryHigh = any (113, 127);
        if (lowSmall && highMid && ! midLow)               b.encoding = MidiBinding::Encoding::relativeSign;
        else if ((midLow || highMid) && ! lowSmall && ! veryHigh) b.encoding = MidiBinding::Encoding::relativeOffset;
        else                                               b.encoding = MidiBinding::Encoding::relativeTwos;
        if (b.action == "jog") b.scale = 1.0f / 512.0f;
    }
    else
    {
        // 14-bit faders send the coarse value on CC n and the fine one on CC n+32.
        if (b.number < 32 && p.ccSeen.count (b.number + 32) > 0)
        {
            b.encoding = MidiBinding::Encoding::absolute14;
            b.lsbNumber = b.number + 32;
        }
        else if (b.number >= 32 && b.number < 64 && p.ccSeen.count (b.number - 32) > 0)
        {
            b.number -= 32;
            b.encoding = MidiBinding::Encoding::absolute14;
            b.lsbNumber = b.number + 32;
        }
        else
        {
            b.encoding = MidiBinding::Encoding::absolute7;
        }
        // Pitch moved top → bottom: bottom must read as the low end (faster). Controllers that send
        // their highest value at the bottom are inverted.
        if (b.action == "pitch" && p.values.size() >= 2 && p.values.back() > p.values.front())
            b.invert = true;
    }
    result.push_back (b);
    advance();
}

void MappingWizard::advance()
{
    ++index;
    const auto& steps = activeSteps();
    if (index < (int) steps.size())
        return;

    if (currentPhase == Phase::deckOne)
    {
        auto it = std::find_if (result.begin(), result.end(), [] (const MidiBinding& b) { return b.action == "play_pause"; });
        if (it != result.end())
        {
            probe = *it;
            currentPhase = Phase::deckTwoProbe;
        }
        else
        {
            currentPhase = Phase::globals;
        }
        index = 0;
    }
    else if (currentPhase == Phase::deckTwoManual)
    {
        currentPhase = Phase::globals;
        index = 0;
    }
    else
    {
        currentPhase = Phase::finished;
    }
}

void MappingWizard::skip()
{
    if (pending.any) { finishPending(); return; }
    if (currentPhase == Phase::deckTwoProbe || currentPhase == Phase::deckTwoCheck)
    {
        // A one-deck controller (or the user prefers not to): deck 2 stays unmapped.
        if (currentPhase == Phase::deckTwoCheck)
            result.erase (std::remove_if (result.begin(), result.end(), [] (const MidiBinding& b) { return b.deck == 1; }), result.end());
        currentPhase = Phase::globals;
        index = 0;
        return;
    }
    if (current() != nullptr)
        advance();
}

void MappingWizard::mirrorDeckTwo()
{
    std::vector<MidiBinding> mirrored;
    for (auto& b : result)
    {
        if (b.deck != 0) continue;
        auto c = b;
        c.deck = 1;
        c.channel = juce::jlimit (1, 16, b.channel + channelOffset);
        if (b.kind != MidiBinding::Kind::pitchWheel)
            c.number = juce::jlimit (0, 127, b.number + numberOffset);
        if (b.lsbNumber >= 0)
            c.lsbNumber = juce::jlimit (0, 127, b.lsbNumber + numberOffset);
        mirrored.push_back (c);
    }
    result.insert (result.end(), mirrored.begin(), mirrored.end());

    auto cue = std::find_if (mirrored.begin(), mirrored.end(), [] (const MidiBinding& b) { return b.action == "cue_button"; });
    if (cue != mirrored.end())
    {
        checkExpected = *cue;
        currentPhase = Phase::deckTwoCheck;
    }
    else
    {
        currentPhase = Phase::globals;
        index = 0;
    }
}

} // namespace om
