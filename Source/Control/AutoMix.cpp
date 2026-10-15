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
double AutoMix::mixSeconds (int deck) const
{
    const double bpm = engine.deck (deck).getEffectiveBpm();
    return bpm > 1.0 ? beats * 60.0 / bpm : juce::jlimit (4.0, 20.0, beats * 0.5);
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
    const double left = (endPointOf (onAir) - k.getPosition()) / juce::jmax (0.01, k.getTempoRatio());
    return juce::jmax (0.0, left - mixSeconds (onAir));
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
            const double s = secondsToMix();
            if (s <= 0.0 && isLoaded (next, items.front()))
                startTransition();
            break;
        }

        case Phase::mixing:
        {
            // Progress measured on the incoming track's own clock (exact, whatever the frame rate).
            const int next = 1 - onAir;
            auto& in = engine.deck (next);
            const double elapsed = (in.getPosition() - mixInStart) / juce::jmax (0.01, in.getTempoRatio());
            if (! bassSwapped && elapsed >= mixLengthSec * 0.5)
            {
                // Halfway: the new track's bass comes in as the old one's goes out.
                engine.channel (next).eqLow = savedLow[(size_t) next];
                engine.channel (onAir).eqLow = 0.0f;
                bassSwapped = true;
            }
            if (engine.autoXfPerSecond.load() <= 0.0f && elapsed >= mixLengthSec)
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
    auto& in = engine.deck (next);
    mixLengthSec = mixSeconds (onAir);

    // Sync only between close tempos (±8 %): a 90 → 128 BPM jump is better as a plain blend.
    const double bo = out.getEffectiveBpm(), bi = in.getGrid().bpm;
    const bool close = bo > 1.0 && bi > 1.0 && std::abs (bo / bi - 1.0) <= 0.08;
    in.seek (startPointOf (next));
    in.setSync (close);
    in.setPlaying (true);

    engine.channel (next).fader = engine.channel (onAir).fader.load();
    savedLow = { engine.channel (0).eqLow.load(), engine.channel (1).eqLow.load() };
    engine.channel (next).eqLow = 0.0f;                      // bass of the new track waits for the swap
    bassSwapped = false;
    engine.startAutoCrossfade (next == 0 ? -1.0f : 1.0f, mixLengthSec);
    mixInStart = startPointOf (next);         // the seek lands on the audio thread; its target is exact

    items.erase (items.begin());
    preloaded = nullptr;
    preloadDeck = -1;
    phase = Phase::mixing;
    sendChangeMessage();
}

void AutoMix::finishTransition()
{
    const int old = onAir, next = 1 - onAir;
    if (old < 0) return;
    engine.deck (old).setPlaying (false);
    engine.channel (old).eqLow = savedLow[(size_t) old];
    engine.channel (next).eqLow = savedLow[(size_t) next];
    engine.deck (next).setSync (false);                      // its tempo glides home while it plays
    engine.crossfader = next == 0 ? -1.0f : 1.0f;
    engine.autoXfPerSecond = 0.0f;
    onAir = next;
    phase = Phase::playing;
    sendChangeMessage();
}

} // namespace om
