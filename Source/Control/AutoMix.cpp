#include "AutoMix.h"
#include "../Common/Strings.h"

namespace om
{

AutoMix::AutoMix (AudioEngine& e, Hooks h) : engine (e), hooks (std::move (h)) {}

//==============================================================================
void AutoMix::add (const std::vector<TrackPtr>& tracks)
{
    insertAt ((int) items.size(), tracks);
}

void AutoMix::insertAt (int index, const std::vector<TrackPtr>& tracks)
{
    index = juce::jlimit (0, (int) items.size(), index);
    for (auto& t : tracks)
        if (t != nullptr)
            items.insert (items.begin() + index++, t);
    sendChangeMessage();
}

void AutoMix::remove (int index)
{
    if (juce::isPositiveAndBelow (index, (int) items.size()))
        items.erase (items.begin() + index);
    sendChangeMessage();
}

void AutoMix::move (int from, int to)
{
    if (! juce::isPositiveAndBelow (from, (int) items.size())) return;
    auto t = items[(size_t) from];
    items.erase (items.begin() + from);
    to = juce::jlimit (0, (int) items.size(), to > from ? to - 1 : to);
    items.insert (items.begin() + to, t);
    sendChangeMessage();
}

void AutoMix::shuffle()
{
    juce::Random rng;
    for (int i = (int) items.size() - 1; i > 0; --i)
        std::swap (items[(size_t) i], items[(size_t) rng.nextInt (i + 1)]);
    sendChangeMessage();
}

void AutoMix::clear()
{
    items.clear();
    sendChangeMessage();
}

//==============================================================================
void AutoMix::setRunning (bool on)
{
    if (on == running) return;
    running = on;
    preloaded = nullptr;
    preloadDeck = -1;
    if (on)
    {
        // The crossfader does the mixing: each deck on its side.
        engine.channel (0).xfAssign = (int) ChannelStrip::XfAssign::a;
        engine.channel (1).xfAssign = (int) ChannelStrip::XfAssign::b;
        const bool p0 = engine.deck (0).isPlaying(), p1 = engine.deck (1).isPlaying();
        onAir = p0 && ! p1 ? 0 : p1 && ! p0 ? 1 : p0 && p1 ? (engine.crossfader.load() > 0.0f ? 1 : 0) : -1;
        phase = onAir >= 0 ? Phase::playing : Phase::starting;
        if (onAir >= 0)
            engine.crossfader = onAir == 0 ? -1.0f : 1.0f;
        setStatus (onAir >= 0 ? juce::String() : "Cargando el primer tema…"_u);
    }
    else
    {
        if (phase == Phase::mixing) finishTransition();
        phase = Phase::idle;
        setStatus ({});
    }
    sendChangeMessage();
}

void AutoMix::mixNow()
{
    if (running && phase == Phase::playing)
        startTransition();
}

void AutoMix::setStatus (const juce::String& s)
{
    if (s != statusText) { statusText = s; sendChangeMessage(); }
}

//==============================================================================
juce::String AutoMix::modeName (Mode m)
{
    switch (m)
    {
        case Mode::end:        return "FINAL";
        case Mode::half:       return "MITAD";
        case Mode::electronic: return juce::String::fromUTF8 ("ELECTRÓNICA");
        case Mode::reverb:     return "REVERB";
        case Mode::echo:       return "ECO";
        case Mode::smart:      return "INTELIGENTE";
        default: break;
    }
    return {};
}

double AutoMix::beatSec (int deck) const
{
    const auto g = engine.deck (deck).getGrid();
    return g.isValid() ? g.beatLengthSeconds() : 0.5;          // track seconds
}

double AutoMix::mixSeconds (int deck) const
{
    return activeBeats * beatSec (deck);                        // track seconds of the outgoing deck
}

AutoMix::Mode AutoMix::plannedMode (int& b) const
{
    b = beats;
    if (userMode == Mode::reverb || userMode == Mode::echo) b = juce::jlimit (8, 16, beats);
    if (userMode != Mode::smart) return userMode;

    // Smart: close tempos get a long, realistic blend; distant ones are hidden behind an echo / reverb.
    double bo = onAir >= 0 ? engine.deck (onAir).getEffectiveBpm() : 0.0, bi = items.empty() ? 0.0 : items.front()->bpm;
    while (bi > 1.0 && bo > 1.0 && bi / bo > 1.4) bi *= 0.5;
    while (bi > 1.0 && bo > 1.0 && bi / bo < 0.7) bi *= 2.0;
    const double diff = bo > 1.0 && bi > 1.0 ? std::abs (bo / bi - 1.0) : 1.0;
    if (diff <= 0.06) { b = juce::jlimit (32, 64, beats); return Mode::electronic; }
    b = 8;
    return diff <= 0.15 ? Mode::echo : Mode::reverb;
}

double AutoMix::triggerPointOf (int deck) const
{
    auto& k = engine.deck (deck);
    const auto g = k.getGrid();
    const double end = endPointOf (deck), len = mixSeconds (deck);
    if (! g.isValid()) return active == Mode::half ? k.getLength() * 0.5 : end - len;

    // Transitions start on a phrase (16 beats from a downbeat; 4 for short mixes), like a DJ would.
    const double phrase = (activeBeats >= 16 ? 16 : 4) * g.beatLengthSeconds();
    auto phraseAtOrBefore = [&] (double t) { return g.firstBeatSeconds + std::floor ((t - g.firstBeatSeconds) / phrase) * phrase; };
    if (active == Mode::half)
    {
        const double mid = k.getLength() * 0.5;
        const double a = phraseAtOrBefore (mid), b = a + phrase;
        return juce::jmin (std::abs (mid - a) <= std::abs (b - mid) ? a : b, phraseAtOrBefore (end - len));
    }
    return phraseAtOrBefore (end - len);
}

double AutoMix::startPointOf (int deck) const
{
    // The cue point if the DJ set one, else the first beat (skips silence before the music).
    auto& k = engine.deck (deck);
    if (k.getCuePoint() > 0.05) return k.getCuePoint();
    const auto grid = k.getGrid();
    if (grid.isValid())
    {
        double first = grid.firstBeatSeconds;
        while (first < 0.0) first += grid.beatLengthSeconds();
        return first;
    }
    return 0.0;
}

double AutoMix::endPointOf (int deck) const
{
    // End of the music: the last frames that are not silence.
    auto t = engine.deck (deck).getTrack();
    if (t == nullptr) return 0.0;
    auto& cache = endCache[(size_t) deck];
    if (cache.first != t.get())
    {
        // Scan back from the end for the last block louder than −40 dBFS (computed once per track).
        const int n = t->numSamples(), block = 1024;
        int last = n;
        for (int i = n - block; i > 0; i -= block)
        {
            float peak = 0.0f;
            for (int c = 0; c < t->audio.getNumChannels(); ++c)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax (t->audio.getReadPointer (c, i), block);
                peak = juce::jmax (peak, r.getEnd(), -r.getStart());
            }
            if (peak > 0.01f) { last = juce::jmin (n, i + block); break; }
        }
        cache = { t.get(), (double) last / t->sampleRate };
    }
    return cache.second;
}

bool AutoMix::isLoaded (int deck, const TrackPtr& t) const
{
    auto lt = engine.deck (deck).getTrack();
    return t != nullptr && lt != nullptr && lt->file == t->file;
}

double AutoMix::secondsToMix() const
{
    if (phase != Phase::playing || onAir < 0) return -1.0;
    auto& k = engine.deck (onAir);
    return juce::jmax (0.0, (triggerPointOf (onAir) - k.getPosition()) / juce::jmax (0.01, k.getTempoRatio()));
}

//==============================================================================
void AutoMix::tick()
{
    if (! running) return;

    switch (phase)
    {
        case Phase::idle: break;

        case Phase::starting:
        {
            // Nothing on air: first track of the queue on deck 1.
            if (items.empty()) { setStatus ("Agregá temas a la lista"_u); return; }
            const int d = 0;
            if (preloaded != items.front() || preloadDeck != d)
            {
                preloaded = items.front();
                preloadDeck = d;
                hooks.load (preloaded, d);
                return;
            }
            if (! isLoaded (d, preloaded)) return;
            auto& k = engine.deck (d);
            k.seek (startPointOf (d));
            k.setPlaying (true);
            engine.crossfader = -1.0f;
            engine.autoXfPerSecond = 0.0f;
            items.erase (items.begin());
            onAir = d;
            preloaded = nullptr;
            preloadDeck = -1;
            phase = Phase::playing;
            setStatus ({});
            sendChangeMessage();
            break;
        }

        case Phase::playing:
        {
            auto& cur = engine.deck (onAir);
            const int next = 1 - onAir;
            if (! cur.isPlaying())
            {
                // Stopped by hand (or the track ran out): go on with the next one right away.
                phase = Phase::starting;
                onAir = -1;
                return;
            }

            // Preload the next track on the free deck.
            if (! items.empty() && ! engine.deck (next).isPlaying()
                && (preloaded != items.front() || preloadDeck != next))
            {
                preloaded = items.front();
                preloadDeck = next;
                hooks.load (preloaded, next);
            }

            // Tempo of the track on air glides back to its own (0 %) after a synced mix.
            if (! cur.isSyncOn() && std::abs (cur.getPitch()) > 1.0e-4)
            {
                const double step = 0.0002;   // ≈ 1.2 % per second at 60 fps: inaudible with key lock
                cur.setPitch (std::abs (cur.getPitch()) <= step ? 0.0 : cur.getPitch() - std::copysign (step, cur.getPitch()));
            }

            if (items.empty())
            {
                setStatus ("Último tema de la lista"_u);
                return;
            }
            setStatus ({});
            active = plannedMode (activeBeats);
            const double s = secondsToMix();
            if (s <= 0.0 && isLoaded (next, items.front()))
                startTransition();
            break;
        }

        case Phase::mixing:
        {
            // Progress measured on the outgoing track's own clock (exact, whatever the frame rate).
            auto& out = engine.deck (onAir);
            const double elapsed = (out.getPosition() - mixOutStart) / juce::jmax (0.01, out.getTempoRatio());
            const double p = juce::jlimit (0.0, 1.0, elapsed / juce::jmax (0.1, mixLengthSec / juce::jmax (0.01, out.getTempoRatio())));
            applyMix (p);
            if (p >= 1.0 || ! out.isPlaying())
                finishTransition();
            break;
        }
    }
}

void AutoMix::startTransition()
{
    if (onAir < 0 || items.empty()) return;
    const int next = 1 - onAir;
    if (! isLoaded (next, items.front())) return;

    auto& out = engine.deck (onAir);
    active = plannedMode (activeBeats);
    mixLengthSec = mixSeconds (onAir);
    mixOutStart = out.getPosition();

    for (int d = 0; d < 2; ++d)
    {
        auto& c = engine.channel (d);
        saved[(size_t) d] = { c.eqLow.load(), c.eqMid.load(), c.eqHigh.load(), c.filter.load(), c.fader.load(), c.xfAssign.load() };
    }
    auto& fx = engine.fx (onAir, 0);
    savedFx = { fx.enabled.load(), fx.type.load(), fx.wet.load(), fx.param.load() };
    engine.autoXfPerSecond = 0.0f;

    if (active == Mode::reverb || active == Mode::echo)
    {
        // Channel faders do this one (FX ring out after the fader; the crossfader would cut the tail).
        for (int d = 0; d < 2; ++d) engine.channel (d).xfAssign = (int) ChannelStrip::XfAssign::thru;
        fx.enabled = false;
        fx.type = (int) (active == Mode::reverb ? BeatFx::Type::reverb : BeatFx::Type::echo);
        fx.param = BeatFx::defaultParam ((BeatFx::Type) fx.type.load());
        fx.wet = 0.0f;
        fx.enabled = true;
        incomingStarted = false;          // the new track drops in on the bar, halfway
    }
    else
        startIncoming();

    cutDone = false;
    items.erase (items.begin());
    preloaded = nullptr;
    preloadDeck = -1;
    phase = Phase::mixing;
    sendChangeMessage();
}

void AutoMix::startIncoming()
{
    const int next = 1 - onAir;
    auto& out = engine.deck (onAir);
    auto& in = engine.deck (next);
    // Sync only between close tempos (±8 %): a 90 → 128 BPM jump is better without it.
    const double bo = out.getEffectiveBpm(), bi = in.getGrid().bpm;
    const bool close = bo > 1.0 && bi > 1.0 && std::abs (bo / bi - 1.0) <= 0.08;
    in.seek (startPointOf (next));
    in.setSync (close);
    in.setPlaying (true);
    engine.channel (next).fader = saved[(size_t) onAir].fader;
    incomingStarted = true;
}

void AutoMix::applyMix (double p)
{
    const int o = onAir, n = 1 - onAir;
    auto& co = engine.channel (o);
    auto& cn = engine.channel (n);
    const float so = o == 0 ? -1.0f : 1.0f;                     // crossfader side of the outgoing deck
    auto ramp = [p] (double a, double b) { return (float) juce::jlimit (0.0, 1.0, (p - a) / (b - a)); };

    switch (active)
    {
        case Mode::end:
        case Mode::half:
            engine.crossfader = so * (1.0f - 2.0f * (float) p);
            cn.eqLow = p < 0.5 ? 0.0f : saved[(size_t) n].low;   // bass swap halfway
            co.eqLow = p < 0.5 ? saved[(size_t) o].low : 0.0f;
            break;

        case Mode::electronic:
        {
            // In over the first quarter, both together, out over the last quarter.
            engine.crossfader = p < 0.25 ? so * (1.0f - ramp (0.0, 0.25)) : p < 0.75 ? 0.0f : -so * ramp (0.75, 1.0);
            // The new track comes in thin (no bass, soft highs); bass swap halfway; the old one is filtered out.
            cn.eqLow  = p < 0.5 ? 0.0f : saved[(size_t) n].low;
            cn.eqHigh = juce::jmap (ramp (0.0, 0.5), 0.25f, saved[(size_t) n].high);
            co.eqLow  = p < 0.5 ? saved[(size_t) o].low : 0.0f;
            co.eqHigh = juce::jmap (ramp (0.5, 1.0), saved[(size_t) o].high, 0.2f);
            co.filter = saved[(size_t) o].filter + 0.55f * ramp (0.7, 1.0);
            break;
        }

        case Mode::reverb:
        case Mode::echo:
        {
            auto& fx = engine.fx (o, 0);
            fx.wet = 0.9f * ramp (0.0, 0.5);
            co.filter = saved[(size_t) o].filter + 0.35f * ramp (0.0, 0.5);
            if (p >= 0.5 && ! cutDone)
            {
                co.fader = 0.0f;                                      // cut on the bar: the FX tail keeps ringing
                cutDone = true;
                if (! incomingStarted) startIncoming();
            }
            break;
        }

        case Mode::smart:
        case Mode::numModes:
        default: break;
    }
}

void AutoMix::finishTransition()
{
    const int old = onAir, next = 1 - onAir;
    if (old < 0) return;
    if (! incomingStarted) startIncoming();
    engine.deck (old).setPlaying (false);
    for (int d = 0; d < 2; ++d)
    {
        auto& c = engine.channel (d);
        const auto& sv = saved[(size_t) d];
        c.eqLow = sv.low; c.eqMid = sv.mid; c.eqHigh = sv.high; c.filter = sv.filter;
        c.fader = d == next ? juce::jmax (sv.fader, saved[(size_t) old].fader) : sv.fader;
    }
    auto& fx = engine.fx (old, 0);
    fx.enabled = savedFx.enabled; fx.type = savedFx.type; fx.wet = savedFx.wet; fx.param = savedFx.param;
    engine.channel (0).xfAssign = (int) ChannelStrip::XfAssign::a;
    engine.channel (1).xfAssign = (int) ChannelStrip::XfAssign::b;
    engine.deck (next).setSync (false);                      // its tempo glides home while it plays
    engine.crossfader = next == 0 ? -1.0f : 1.0f;
    engine.autoXfPerSecond = 0.0f;
    onAir = next;
    phase = Phase::playing;
    sendChangeMessage();
}

} // namespace om
