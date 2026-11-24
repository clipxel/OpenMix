#include "Deck.h"
#include <signalsmith-stretch.h>
#include "SincResampler.h"

namespace om
{

struct Deck::Stretch
{
    // Two stretchers: on a jump the new one starts at the target while the old one
    // finishes its audio, and the two are crossfaded (no gap, no click).
    std::array<signalsmith::stretch::SignalsmithStretch<float>, 2> dsp;
    int cur = 0;
    std::vector<float> inL, inR, preL, preR, oldInL, oldInR, oldOutL, oldOutR;
    int maxIn = 0;
};

Deck::Deck (int i) : index (i), stretch (std::make_unique<Stretch>())
{
    hotCues.fill (-1.0);
}

Deck::~Deck() = default;

juce::String Deck::getEffectiveKey() const
{
    if (! track || track->meta.key.isEmpty())
        return {};
    return Analysis::transposeCamelot (track->meta.key, keyShift.load());
}

//==============================================================================
void Deck::setTrack (LoadedTrackPtr t)
{
    playRequested = false;
    previewing = false;
    loopActive = false;
    // SYNC stays on across loads (as on club players): the new track takes the tempo it has to follow
    // at once, or its own original tempo when there is nothing to follow.
    if (syncOn.load()) { syncEngaged = true; justLoaded = true; }

    {
        const juce::SpinLock::ScopedLockType sl (trackLock);
        audioTrack = t.get();
        audioStems = nullptr;
        stemBlend = 0.0f;
        stemGain = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        pos = 0.0;
        gain = 0.0f;
        pendingSeek = -1.0;
        jumpMode = 0;
        // Nothing from the previous track may leak into the new one.
        stretchActive = false;
        stretchXfadePending = false;
        stretchXfadeLeft = 0;
        needPrime = true;
        stretchLead = 0.0;
        inCarry = 0.0;
        fadeLeft = 0;
        scratchVel = 0.0;
        brakeFactor = 1.0;
        backspinT = -1.0;
        wasDisplaced = false;
        slipWillReturn = false;
        phaseCorrection = 0.0;
        bend = 0.0;
        brake = false;
        backspin = false;
    }

    // Old track (if any) is released here, on the message thread, never on the audio thread.
    track = std::move (t);
    stems.reset();
    resetStems();

    cuePoint = track ? juce::jlimit (0.0, track->lengthSeconds(), track->meta.cuePoint) : 0.0;
    hotCues = track ? track->meta.hotCues : std::array<double, 8> { -1, -1, -1, -1, -1, -1, -1, -1 };
    savedLoops = track ? track->meta.savedLoops : std::array<std::pair<double, double>, 8> {};
    reverse = censor = scratching = rollActive = brake = slicing = phraseLoop = false;
    phraseStart = phraseEnd = -1.0;

    // Auto cue (club player): with no stored cue, park on the first audible sound (≈ −36 dB of the peak).
    if (track && autoCue && cuePoint <= 0.0 && ! track->wave.isEmpty())
    {
        const auto& w = track->wave;
        for (int f = 0; f < w.numFrames(); ++f)
            if (juce::jmax (w.low[(size_t) f], w.mid[(size_t) f], w.high[(size_t) f]) > 4)
            {
                cuePoint = (double) f / WaveformData::framesPerSecond;
                break;
            }
        // On the first hit: the grid beat at (or just before) that first sound.
        const auto g = track->grid();
        if (g.isValid() && cuePoint > 0.0)
        {
            const double b = g.timeOfBeat (std::round (g.beatAt (cuePoint)));
            if (b >= 0.0 && std::abs (b - cuePoint) < g.beatLengthSeconds() * 0.5) cuePoint = b;
        }
    }

    publishedPos = cuePoint;
    publishedRate = 0.0;
    if (cuePoint > 0.0)
        requestSeek (cuePoint);
}

void Deck::setPlaying (bool shouldPlay)
{
    if (! track)
        return;

    if (shouldPlay && getPosition() >= getLength() - 0.01)
        return;

    previewing = false;
    gateSlot = -1;
    if (shouldPlay) restAt = -1.0;
    playRequested = shouldPlay;
}

void Deck::cueDown()
{
    if (! track)
        return;

    if (isPlaying() && ! previewing)
    {
        playRequested = false;
        requestSeek (cuePoint);
        return;
    }

    const auto here = getPosition();
    if (std::abs (here - cuePoint) > 0.02)
    {
        cuePoint = here;            // exactly where the DJ left it (never pulled onto the grid)
        requestSeek (cuePoint);
        notifyCues();
        return;
    }

    if (restAt.exchange (-1.0) >= 0.0)
        requestSeek (cuePoint);     // tapped again while the last tap still fades: start on the cue
    previewing = true;
    playRequested = true;
}

void Deck::cueUp()
{
    if (! previewing)
        return;

    previewing = false;
    playRequested = false;
    requestSeek (cuePoint);
    restAt = cuePoint;              // the short fade-out must not leave the deck past the cue
}

void Deck::seek (double seconds)
{
    if (! track)
        return;

    seconds = juce::jlimit (0.0, getLength(), seconds);
    requestSeek (seconds);
    publishedPos = seconds;
    publishedAtMs = juce::Time::getMillisecondCounterHiRes();
}

double Deck::getPosition() const noexcept
{
    auto p = publishedPos.load();
    if (playRequested.load() || scratching.load())
    {
        const auto elapsed = juce::jlimit (0.0, 0.08, (juce::Time::getMillisecondCounterHiRes() - publishedAtMs.load()) * 0.001);
        p += elapsed * publishedRate.load();
    }
    // In a loop shorter than the smoothing (1/32 beat ≈ 15 ms) or than the delay to the speakers, the
    // estimate falls outside it: fold it back in, so the waveform stays on the loop.
    if (loopActive.load())
    {
        const double s = loopStart.load(), e = loopEnd.load(), len = e - s;
        if (len > 1.0e-4 && ((p > e && p < e + 0.25) || (p < s && p > s - 0.25)))
            p = s + std::fmod (std::fmod (p - s, len) + len, len);
    }
    return juce::jlimit (0.0, getLength(), p);
}

void Deck::setPitch (double fraction)
{
    pitchGlide = 0.0;                             // a hand on the fader takes over
    pitch = juce::jlimit (-0.99, 1.0, fraction);
}

void Deck::pitchResetPressed()
{
    const auto now = juce::Time::getMillisecondCounter();
    const bool second = isPitchGliding() || now - lastPitchResetMs < 400;
    lastPitchResetMs = now;
    if (isSyncOn()) setSync (false);              // the tempo is the DJ's again
    if (second || std::abs (pitch.load()) < 1.0e-6)
    {
        setPitch (0.0);
        return;
    }
    // 1 % per second (the usual default for a smooth reset), but never longer than 6 s.
    pitchGlide = juce::jmax (0.01, std::abs (pitch.load()) / 6.0);
}

void Deck::setPitchRange (double fraction)
{
    pitchRange = fraction;
    if (std::abs (pitch.load()) > fraction)
        pitch = juce::jlimit (-fraction, fraction, pitch.load());
}

double Deck::getEffectiveBpm() const noexcept
{
    if (! track || ! track->grid().isValid())
        return 0.0;
    return track->grid().bpm * getTempoRatio();
}

void Deck::setSync (bool on)
{
    if (on && (! track || ! track->grid().isValid()))
        return;

    syncOn = on;
    if (on)
    {
        syncStamp = (juce::int64) juce::Time::getHighResolutionTicks();
        syncEngaged = true;
    }
}

void Deck::syncPress()
{
    syncPressedAt = juce::Time::getMillisecondCounterHiRes();
    syncWasOnAtPress = syncOn.load();
    if (syncWasOnAtPress)
    {
        syncStamp = (juce::int64) juce::Time::getHighResolutionTicks();
        syncEngaged = true;                     // the engine jumps the phase into place on its next block
    }
    else
        setSync (true);
}

void Deck::pitchFader (double p)
{
    const double before = lastFaderPitch;
    lastFaderPitch = p;
    if (syncOn.load())
    {
        // Caught: the fader reached the tempo sync set (within 5 % of the range: ½ % pitch at ±10 %),
        // or went across it between two readings (a fast move).
        const double cur = getPitch();
        const double near = juce::jmax (0.001, getPitchRange() * 0.05);
        const bool crossed = std::isfinite (before) && (before - cur) * (p - cur) <= 0.0 && std::abs (p - before) < getPitchRange() * 0.5
                           && std::abs (p - before) > getPitchRange() * 0.002;      // a real move, not fader noise
        if (std::abs (p - cur) > near && ! crossed) return;
        setSync (false);
    }
    setPitch (p);
}

void Deck::syncRelease()
{
    if (syncWasOnAtPress && juce::Time::getMillisecondCounterHiRes() - syncPressedAt >= 600.0)
        setSync (false);
    syncWasOnAtPress = false;
}

double Deck::snap (double seconds) const
{
    if (quantize.load() && track && track->grid().isValid())
    {
        // Nearest grid line at the quantize resolution (1, 1/2, 1/4, 1/8 beat).
        const auto g = track->grid();
        const double q = quantizeBeats.load();
        return juce::jlimit (0.0, getLength(), g.timeOfBeat (std::round (g.beatAt (seconds) / q) * q));
    }
    return seconds;
}

//==============================================================================
void Deck::hotCue (int slot)
{
    if (! track || ! juce::isPositiveAndBelow (slot, 8))
        return;

    auto& c = hotCues[(size_t) slot];
    if (c < 0.0)
    {
        c = snap (getPosition());
        notifyCues();
    }
    else if (! isPlaying())
    {
        // Paused: jump there and play. Gate cue (club player): only while the pad is held.
        requestJump (Jump::absolute, c);
        publishedPos = c;
        publishedAtMs = juce::Time::getMillisecondCounterHiRes();
        gateSlot = gateCue.load() ? slot : -1;
        previewing = false;
        playRequested = true;
    }
    else if (quantize.load() && track->grid().isValid())
    {
        requestJump (Jump::keepPhase, c);       // lands in time: same beat phase as the playhead
    }
    else
    {
        requestJump (Jump::absolute, c);
        publishedPos = c;
        publishedAtMs = juce::Time::getMillisecondCounterHiRes();
    }
}

void Deck::hotCueRelease (int slot)
{
    if (gateSlot != slot || ! track)
        return;
    gateSlot = -1;
    playRequested = false;
    requestJump (Jump::absolute, hotCues[(size_t) slot]);
}

void Deck::cloneFrom (const Deck& o)
{
    if (o.track == nullptr || isPlaying())
        return;
    setTrack (o.track);                         // shares the decoded audio — no reload
    setStems (o.stems);
    for (size_t k = 0; k < stemLevel.size(); ++k) { stemLevel[k] = o.stemLevel[k].load(); stemMute[k] = o.stemMute[k].load(); }
    hotCues = o.hotCues;
    savedLoops = o.savedLoops;
    cuePoint = o.cuePoint;
    pitchRange = o.pitchRange.load();
    pitch = o.pitch.load();
    keyLock = o.keyLock.load();
    keyShift = o.keyShift.load();
    loopStart = o.loopStart.load();
    loopEnd = o.loopEnd.load();
    loopActive = o.loopActive.load();
    autoLoopBeats = o.autoLoopBeats;
    // Same position, compensated for the time until our next block (≈ one block).
    requestJump (Jump::absolute, o.getPosition());
    playRequested = o.isPlaying();
    notifyCues();
}

void Deck::setStems (StemDataPtr s)
{
    {
        const juce::SpinLock::ScopedLockType sl (trackLock);
        audioStems = s.get();
        stemBlend = 0.0f;
    }
    stems = std::move (s);        // the previous ones are released here, never on the audio thread
}

void Deck::setStemLevel (int part, float level)
{
    if (juce::isPositiveAndBelow (part, (int) numStemParts))
        stemLevel[(size_t) part] = juce::jlimit (0.0f, 1.0f, level);
}

void Deck::setStemMute (int part, bool muted)
{
    if (juce::isPositiveAndBelow (part, (int) numStemParts))
        stemMute[(size_t) part] = muted;
}

void Deck::soloStem (int part)
{
    if (! juce::isPositiveAndBelow (part, (int) numStemParts))
        return;
    bool alreadySolo = ! isStemMuted (part);
    for (int k = 0; k < numStemParts; ++k)
        if (k != part && ! isStemMuted (k)) alreadySolo = false;
    for (int k = 0; k < numStemParts; ++k)
        setStemMute (k, alreadySolo ? false : k != part);
}

void Deck::resetStems()
{
    for (int k = 0; k < numStemParts; ++k)
    {
        stemLevel[(size_t) k] = 1.0f;
        stemMute[(size_t) k] = false;
    }
}

bool Deck::stemsTouched() const noexcept
{
    for (int k = 0; k < numStemParts; ++k)
        if (stemMute[(size_t) k].load() || stemLevel[(size_t) k].load() < 1.0f)
            return true;
    return false;
}

void Deck::clearHotCue (int slot)
{
    if (! juce::isPositiveAndBelow (slot, 8))
        return;
    hotCues[(size_t) slot] = -1.0;
    notifyCues();
}

//==============================================================================
void Deck::setAutoLoop (double beats)
{
    if (! track || ! track->grid().isValid())
        return;

    autoLoopBeats = beats;
    requestJump (Jump::autoLoop, beats);        // boundaries computed from the exact playhead
}

void Deck::loopIn()
{
    if (! track) return;
    loopStart = snap (getPosition());
    if (loopEnd.load() <= loopStart.load())
        loopActive = false;
}

void Deck::loopOut()
{
    if (! track) return;
    const auto end = snap (getPosition());
    if (end > loopStart.load() + 0.01)
    {
        loopEnd = end;
        loopActive = true;
    }
}

void Deck::exitLoop() { loopActive = false; phraseLoop = false; phraseStart = phraseEnd = -1.0; }

void Deck::reloop()
{
    if (! track || loopEnd.load() <= loopStart.load())
        return;
    loopActive = true;
    requestSeek (loopStart.load());
}

void Deck::scaleLoop (double factor)
{
    if (! track || loopEnd.load() <= loopStart.load())
        return;

    const auto len = (loopEnd.load() - loopStart.load()) * factor;
    if (len < 0.01 || loopStart.load() + len > getLength())
        return;

    loopEnd = loopStart.load() + len;
    if (track->grid().isValid())
        autoLoopBeats = len / track->grid().beatLengthSeconds();
}

double Deck::getLoopBeats() const noexcept
{
    if (! track || ! track->grid().isValid())
        return 0.0;
    return (loopEnd.load() - loopStart.load()) / track->grid().beatLengthSeconds();
}

void Deck::beatJump (double beats)
{
    if (! track || ! track->grid().isValid())
        return;
    requestJump (Jump::relativeBeats, beats);   // exact whole beats: phase is preserved
}

void Deck::setGrid (BeatGrid g)
{
    if (! track || g.bpm < 20.0 || g.bpm > 400.0)
        return;
    const double bar = 4.0 * 60.0 / g.bpm;
    g.firstBeatSeconds = std::fmod (std::fmod (g.firstBeatSeconds, bar) + bar, bar);
    track->setGrid (g);
    if (onGridChanged) onGridChanged();
}

void Deck::shiftGrid (double seconds)
{
    auto g = getGrid();
    if (! g.isValid()) return;
    g.firstBeatSeconds += seconds;
    setGrid (g);
}

void Deck::setBpmKeepingPlayhead (double bpm)
{
    auto g = getGrid();
    if (! track) return;
    if (! g.isValid()) { setGrid ({ bpm, getPosition() }); return; }
    const double beatIdx = std::round (g.beatAt (getPosition()));
    const double anchor = g.timeOfBeat (beatIdx);
    setGrid ({ bpm, anchor - beatIdx * 60.0 / bpm });
}

void Deck::setDownbeatHere()
{
    auto g = getGrid();
    if (! track || ! g.isValid()) return;
    g.firstBeatSeconds = getPosition();
    setGrid (g);
}

void Deck::tapTempo()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (! taps.empty() && now - taps.back() > 2.0) taps.clear();
    taps.push_back (now);
    if (taps.size() > 8) taps.erase (taps.begin());
    if (taps.size() == 1)
    {
        // One tap: the grid moves so a bar starts right here (same BPM). More taps in time: the BPM.
        auto g = getGrid();
        if (track && g.isValid())
        {
            const double bar = 4.0 * g.beatLengthSeconds();
            const double here = getPosition();
            g.firstBeatSeconds = here - std::floor (here / bar) * bar;
            setGrid (g);
        }
    }
    if (taps.size() >= 4)
        setBpmKeepingPlayhead (std::round (60.0 * (double) (taps.size() - 1) / (taps.back() - taps.front()) * 100.0) / 100.0);
}

void Deck::resetGrid()
{
    if (track && track->analysedGrid.isValid())
        setGrid (track->analysedGrid);
}

void Deck::loopMove (double beats)
{
    if (! track || ! track->grid().isValid() || loopEnd.load() <= loopStart.load())
        return;
    const auto d = beats * track->grid().beatLengthSeconds();
    const auto s0 = loopStart.load() + d, e0 = loopEnd.load() + d;
    if (s0 < 0.0 || e0 > getLength())
        return;
    loopStart = s0;
    loopEnd = e0;
    if (loopActive.load())
        seek (getPosition() + d);
}

void Deck::saveLoop (int slot)
{
    if (! track || ! juce::isPositiveAndBelow (slot, 8) || loopEnd.load() <= loopStart.load())
        return;
    savedLoops[(size_t) slot] = { loopStart.load(), loopEnd.load() };
    notifyCues();
}

void Deck::triggerSavedLoop (int slot)
{
    if (! track || ! juce::isPositiveAndBelow (slot, 8))
        return;
    const auto [s0, e0] = savedLoops[(size_t) slot];
    if (e0 <= s0)
    {
        saveLoop (slot);
        return;
    }
    if (loopActive.load() && std::abs (loopStart.load() - s0) < 1e-6 && std::abs (loopEnd.load() - e0) < 1e-6)
    {
        loopActive = false;
        return;
    }
    loopStart = s0;
    loopEnd = e0;
    loopActive = true;
    requestJump (isPlaying() && quantize.load() && track->grid().isValid() ? Jump::keepPhase : Jump::absolute, s0);
}

void Deck::clearSavedLoop (int slot)
{
    if (! juce::isPositiveAndBelow (slot, 8)) return;
    savedLoops[(size_t) slot] = {};
    notifyCues();
}

void Deck::setScratching (bool on)
{
    if (! track) return;
    scratching = on;
    if (! on) scratchPending = 0.0;
}

void Deck::scratchMove (double seconds)
{
    if (! track) return;
    scratchPending = scratchPending.load() + seconds;
}

void Deck::brakeStop()
{
    if (isPlaying())
        brake = true;
}

void Deck::loopRoll (double beats, bool pressed)
{
    if (! track || ! track->grid().isValid())
        return;

    if (pressed)
    {
        rollBeats = beats;
        rollActive = true;
        requestJump (Jump::roll, beats);
    }
    else if (rollActive.load() && std::abs (rollBeats - beats) < 1e-9)
    {
        loopActive = false;
        rollActive = false;
        rollBeats = 0.0;
    }
}

std::pair<double, double> Deck::slicerPhrase() const
{
    if (! track || ! track->grid().isValid()) return { -1.0, -1.0 };
    if (phraseLoop.load() && phraseStart.load() >= 0.0) return { phraseStart.load(), phraseEnd.load() };
    const auto g = track->grid();
    const double where = slipDisplaced.load() ? publishedSlipPos.load() : getPosition();
    const double first = std::floor (g.beatAt (where) / 8.0 + 1.0e-7) * 8.0;
    return { g.timeOfBeat (first), g.timeOfBeat (first + 8.0) };
}

int Deck::slicePlaying() const
{
    const auto [a, b] = slicerPhrase();
    if (a < 0.0 || b <= a) return -1;
    const double p = getPosition();
    if (p < a || p >= b) return -1;
    return juce::jlimit (0, 7, (int) ((p - a) / (b - a) * 8.0));
}

void Deck::slice (int n, bool pressed)
{
    if (! track || ! track->grid().isValid() || ! juce::isPositiveAndBelow (n, 8)) return;
    if (pressed)
    {
        const auto g = track->grid();
        rollBeats = -1.0 - n;                                    // a slice, not a roll size
        rollActive = true;
        slicing = true;
        requestJump (Jump::slice, g.beatAt (slicerPhrase().first) + n);
    }
    else if (slicing.load() && std::abs (rollBeats - (-1.0 - n)) < 1e-9)
    {
        slicing = false;
        rollActive = false;
        rollBeats = 0.0;
        if (phraseLoop.load() && phraseStart.load() >= 0.0)
        {
            loopStart = phraseStart.load();                      // back to the phrase loop
            loopEnd = phraseEnd.load();
        }
        else
            loopActive = false;
    }
}

void Deck::setPhraseLoop (bool on)
{
    if (! track || ! track->grid().isValid()) return;
    if (on) requestJump (Jump::phraseLoop, 8.0);
    else if (phraseLoop.load())
    {
        phraseLoop = false;
        phraseStart = phraseEnd = -1.0;
        loopActive = false;
    }
}

void Deck::cueLoop (int slot)
{
    if (! track || ! track->grid().isValid() || ! juce::isPositiveAndBelow (slot, 8)) return;
    const double c = hotCues[(size_t) slot];
    if (c < 0.0)
    {
        hotCue (slot);                                           // set here, loop from here
        setAutoLoop (autoLoopBeats);
        return;
    }
    if (loopActive.load() && std::abs (loopStart.load() - c) < 0.01) { exitLoop(); return; }
    requestJump (Jump::cueLoop, c);
}

void Deck::jogBend (double amount)      { bend = juce::jlimit (-1.0, 1.0, amount) * 0.12; }
void Deck::jogScrub (double seconds)    { if (! isPlaying()) seek (getPosition() + seconds); }

//==============================================================================
void Deck::applyJump (const LoadedTrack& t)
{
    const auto mode = (Jump) jumpMode.exchange (0);
    if (mode == Jump::none)
        return;

    const double v = jumpValue.load();
    const auto g = t.grid();
    const int n = t.numSamples();
    const double s2o = t.sampleRate / outRate;
    const double heardPos = stretchActive ? pos - stretchLead * s2o : pos;    // what is being heard now
    const double cur = heardPos / t.sampleRate;
    const double q = quantizeBeats.load();
    const bool quant = quantize.load() && g.isValid();

    auto phaseOf = [&g] (double sec, double res)
    {
        const double b = g.beatAt (sec);
        double p = b - res * std::floor (b / res);
        if (p > res - 1.0e-9) p -= res;
        return p;
    };

    double target = -1.0;
    switch (mode)
    {
        case Jump::absolute:
            target = v;
            break;

        case Jump::keepPhase:
            // Land at the cue plus the playhead's current position within the quantize cell.
            target = quant ? g.timeOfBeat (g.beatAt (v) - phaseOf (v, q) + phaseOf (cur, q)) : v;
            break;

        case Jump::relativeBeats:
            if (g.isValid()) target = cur + v * g.beatLengthSeconds();
            break;

        case Jump::autoLoop:
        case Jump::roll:
        {
            if (! g.isValid()) break;
            // Loop starts on the last grid line at or before the playhead (resolution ≤ loop length),
            // so playback simply continues and wraps exactly N beats later.
            const double res = juce::jmin (q, v);
            const double startBeat = quant ? std::floor (g.beatAt (cur) / res + 1.0e-7) * res : g.beatAt (cur);
            const double ls = juce::jmax (0.0, g.timeOfBeat (startBeat));
            loopStart = ls;
            loopEnd = juce::jmin ((double) n / t.sampleRate, ls + v * g.beatLengthSeconds());
            loopActive = true;
            break;
        }

        case Jump::slice:
        {
            if (! g.isValid()) break;
            // The ghost keeps the track's place from what is being heard; the slice plays in a one-beat loop.
            if (! wasDisplaced) { slipPos = heardPos; wasDisplaced = true; slipWillReturn = true; }
            const double ls = juce::jmax (0.0, g.timeOfBeat (v));
            loopStart = ls;
            loopEnd = juce::jmin ((double) n / t.sampleRate, ls + g.beatLengthSeconds());
            loopActive = true;
            target = ls;
            break;
        }

        case Jump::phraseLoop:
        {
            if (! g.isValid()) break;
            const double first = std::floor (g.beatAt (cur) / v + 1.0e-7) * v;
            phraseStart = juce::jmax (0.0, g.timeOfBeat (first));
            phraseEnd = juce::jmin ((double) n / t.sampleRate, g.timeOfBeat (first + v));
            loopStart = phraseStart.load();
            loopEnd = phraseEnd.load();
            loopActive = true;
            phraseLoop = true;
            break;
        }

        case Jump::cueLoop:
        {
            if (! g.isValid()) break;
            loopStart = v;
            loopEnd = juce::jmin ((double) n / t.sampleRate, v + autoLoopBeats * g.beatLengthSeconds());
            loopActive = true;
            target = v;
            break;
        }

        case Jump::none:
            break;
    }

    if (target < 0.0)
        return;

    const double newPos = juce::jlimit (0.0, (double) n - 1.0, target * t.sampleRate);
    if (stretchActive)
    {
        stretchJumping (s2o);
        if (! playRequested.load()) { gain = 0.0f; stretchXfadePending = false; }
    }
    else if (gain > 0.0f)
    {
        beginJump (pos);                                  // crossfaded, click-free
        if (! playRequested.load())
            gain = 0.0f;                                  // stopping: old audio fades out, new spot stays silent
    }
    pos = newPos;                                         // a heard position: the stretcher re-primes from it
    needPrime = true;
}

void Deck::prepare (double sr)
{
    outRate = sr;
    rampStep = (float) (1.0 / (0.004 * sr));
    stemStep = (float) (1.0 / (0.012 * sr));     // a stem fades in or out over ≈ 12 ms
    {
        const auto c = juce::IIRCoefficients::makeHighPass (sr, juce::jmin (5000.0, sr * 0.45), 0.7071);
        hatCoef = { c.coefficients[0], c.coefficients[1], c.coefficients[2], c.coefficients[3], c.coefficients[4] };
    }

    SincResampler::get();   // build filter tables here, never on the audio thread

    // Allocate everything up front: the audio thread never allocates.
    auto& st = *stretch;
    st.maxIn = 16384 * 2 + 8;
    for (auto* v : { &st.inL, &st.inR, &st.oldInL, &st.oldInR, &st.oldOutL, &st.oldOutR })
        v->assign ((size_t) st.maxIn, 0.0f);
    for (auto& dsp : st.dsp)
    {
        dsp.presetDefault (2, (float) sr);
        const auto preLen = (size_t) juce::jmax (dsp.seekLength(), dsp.outputSeekLength (2.0f)) + 8;
        st.preL.assign (preLen, 0.0f);
        st.preR.assign (preLen, 0.0f);
        float* pre[2] = { st.preL.data(), st.preR.data() };
        dsp.outputSeek (pre, dsp.outputSeekLength (1.0f));   // warms every internal buffer (no allocation later)
        dsp.reset();
    }
    stretchActive = false;
}

double Deck::audioPositionSeconds() const noexcept
{
    if (audioTrack == nullptr) return 0.0;
    const double heard = stretchActive ? pos - stretchLead * (audioTrack->sampleRate / outRate) : pos;
    return heard / audioTrack->sampleRate;
}

double Deck::audioBpm() const noexcept
{
    return audioTrack != nullptr && audioTrack->grid().isValid() ? audioTrack->grid().bpm : 0.0;
}

void Deck::audioSetPitch (double p) noexcept
{
    p = juce::jlimit (-0.99, 1.0, p);
    pitch = p;

    // Keep the fader honest: widen the range if sync needs more travel.
    for (double r : { 0.06, 0.08, 0.16, 0.5, 1.0 })
        if (std::abs (p) <= r) { if (r > pitchRange.load()) pitchRange = r; break; }
}

void Deck::audioShiftSeconds (double delta) noexcept
{
    if (audioTrack != nullptr)
    {
        if (stretchActive) stretchJumping (audioTrack->sampleRate / outRate);   // re-prime + crossfade
        else if (gain > 0.0f) beginJump (pos);                                  // crossfade, no click
        pos = juce::jlimit (0.0, (double) audioTrack->numSamples() - 1.0, pos + delta * audioTrack->sampleRate);
        needPrime = true;
    }
}

void Deck::beginBlock()
{
    // Called with trackLock not held; render() takes it.
}

bool Deck::audioResolveMoves() noexcept
{
    if (audioTrack == nullptr) return false;
    const auto& t = *audioTrack;
    const int n = t.numSamples();
    if (n <= 0) return false;
    const double before = pos;
    if (auto s = pendingSeek.exchange (-1.0); s >= 0.0)
    {
        const double target = juce::jlimit (0.0, (double) n - 1.0, s * t.sampleRate);
        if (stretchActive)
            stretchJumping (t.sampleRate / outRate);   // crossfaded between the two stretchers
        else if (playRequested.load() && gain > 0.0f)
            beginJump (pos);           // crossfade old → new position
        else
            gain = 0.0f;               // not sounding: fade in from the new point
        pos = target;
        needPrime = true;
    }

    applyJump (t);
    return std::abs (pos - before) > 0.5;
}

void Deck::render (float* left, float* right, int numSamples)
{
    juce::FloatVectorOperations::clear (left, numSamples);
    juce::FloatVectorOperations::clear (right, numSamples);

    const juce::SpinLock::ScopedTryLockType sl (trackLock);
    if (! sl.isLocked() || audioTrack == nullptr)
    {
        audible = false;
        renderedBpm = 0.0;
        blockBeat = -1.0;
        blockBeatsPerSample = 0.0;
        return;
    }

    const auto& t = *audioTrack;
    const int n = t.numSamples();
    if (n <= 0)
        return;

    audioResolveMoves();

    // CUE released and the fade-out is over: the deck rests exactly on the cue point.
    if (const double r = restAt.load(); r >= 0.0 && ! playRequested.load() && gain <= 0.0f)
    {
        pos = juce::jlimit (0.0, (double) n - 1.0, r * t.sampleRate);
        stretchActive = false;
        stretchLead = 0.0;
        restAt = -1.0;
    }

    // Musical clock for effects scheduled on this deck (heard position, start of block).
    {
        const auto g = t.grid();
        const double heardSec = (stretchActive ? pos - stretchLead * (t.sampleRate / outRate) : pos) / t.sampleRate;
        const bool moving = playRequested.load();
        blockBeat = g.isValid() && moving ? g.beatAt (heardSec) : -1.0;
        blockBeatsPerSample = g.isValid() && moving
            ? (g.bpm / 60.0) * (1.0 + pitch.load() + bend.load()) * (1.0 + phaseCorrection.load()) / outRate : 0.0;
    }

    const double blockSeconds = (double) numSamples / outRate;
    const double srcToOut = t.sampleRate / outRate;

    // Pitch reset glide: a constant, gentle slope back to 0 %.
    if (const double glide = pitchGlide.load(); glide > 0.0)
    {
        const double p = pitch.load(), stepP = glide * blockSeconds;
        if (syncOn.load() || std::abs (p) <= stepP) { pitch = syncOn.load() ? p : 0.0; pitchGlide = 0.0; }
        else pitch = p - std::copysign (stepP, p);
    }

    // Brake: tempo ramps to zero over ~1 s, then the deck stops.
    if (brake.load())
    {
        brakeFactor -= blockSeconds / juce::jmax (0.1, brakeSeconds.load());
        if (brakeFactor <= 0.0)
        {
            brakeFactor = 1.0;
            brake = false;
            playRequested = false;
        }
    }
    else
    {
        brakeFactor = 1.0;
    }

    const bool wantPlay = playRequested.load();
    const bool scr = scratching.load();
    const bool rev = reverse.load() || censor.load();
    const double baseRate = srcToOut * (1.0 + pitch.load() + bend.load()) * (1.0 + phaseCorrection.load());

    // Scratch: follow the hand — platter displacement per block becomes playback speed.
    if (scr)
    {
        const double desired = scratchPending.exchange (0.0) / blockSeconds;
        scratchVel = juce::jlimit (-8.0, 8.0, scratchVel * 0.5 + desired * 0.5);
    }
    else
    {
        scratchVel = 0.0;
    }

    // Backspin: the platter is thrown backwards (≈ −4× speed) and slows to a stop.
    double spinRate = 0.0;
    if (backspin.load())
    {
        if (backspinT < 0.0) backspinT = 0.0;
        backspinT += blockSeconds;
        const double len = juce::jmax (0.2, backspinSeconds.load());
        const double k = 1.0 - backspinT / len;
        if (k <= 0.0)
        {
            backspin = false;
            backspinT = -1.0;
            playRequested = false;
        }
        else spinRate = -4.0 * srcToOut * k * k;
    }
    else backspinT = -1.0;

    const bool active = wantPlay || scr;
    const double rate = scr ? scratchVel * srcToOut
                      : spinRate != 0.0 ? spinRate
                      : (rev ? -baseRate : baseRate) * brakeFactor;

    const bool looping = loopActive.load();
    const double lStart = loopStart.load() * t.sampleRate;
    const double lEnd   = loopEnd.load() * t.sampleRate;

    // Slip: while displaced, a ghost playhead keeps running at normal speed;
    // when the displacement ends the deck continues from the ghost position.
    const bool displaced = wantPlay && (scr || rev || (looping && (slip.load() || rollActive.load())));
    if (displaced && ! wasDisplaced)
    {
        // The ghost starts from what is being heard (the key-lock stretcher runs ahead).
        slipPos = stretchActive ? pos - stretchLead * srcToOut : pos;
        slipWillReturn = slip.load() || rollActive.load() || censor.load();
    }
    if (! displaced && wasDisplaced && slipWillReturn && wantPlay)
    {
        if (stretchActive) stretchJumping (srcToOut);
        else               beginJump (pos);
        pos = juce::jlimit (0.0, (double) n - 1.0, slipPos);
        needPrime = true;
    }
    if (displaced)
        slipPos += baseRate * numSamples;
    wasDisplaced = displaced;
    slipDisplaced = displaced && slipWillReturn;
    publishedSlipPos = slipPos / t.sampleRate;

    const float* L = t.audio.getReadPointer (0);
    const float* R = t.audio.getReadPointer (1);
    const auto& sinc = SincResampler::get();

    // Stems: out = melody·track + Σ (part − melody)·stem (the melody is the track minus the other three),
    // so with every part at full it is the track itself, bit for bit — and then nothing extra is computed.
    const StemData* sd = audioStems;
    std::array<float, 5> stemTarget {};
    for (size_t k = 0; k < 4; ++k)
        stemTarget[k] = stemMute[k].load() ? 0.0f : stemLevel[k].load();
    stemTarget[4] = stemTarget[1];
    const bool eqWasActive = stemEqActive;
    stemEqActive = stemEqWanted && sd != nullptr;
    if (stemEqActive)
    {
        const float low = stemEqGain (eqKnob[0]), high = stemEqGain (eqKnob[2]), mid = eqKnob[1];
        const float voice = mid < 0.5f ? (2.0f * mid) * (2.0f * mid) : 1.0f;
        const float melody = mid > 0.5f ? (2.0f * (1.0f - mid)) * (2.0f * (1.0f - mid)) : 1.0f;
        // MID only trades voice and melody: the beat (LOW) and the hi-hats (HIGH) stay as they are.
        stemTarget[0] *= voice;
        stemTarget[1] *= low;
        stemTarget[2] *= low;
        stemTarget[3] *= melody;
        stemTarget[4] *= high;
    }
    if (! eqWasActive && stemEqActive)
        for (auto& st : hatState) st.fill (0.0);
    bool stemsMoving = false;
    for (size_t k = 0; k < stemTarget.size(); ++k)
        if (stemTarget[k] != 1.0f || stemGain[k] != 1.0f) stemsMoving = true;
    const bool useStems = sd != nullptr && stemsMoving;
    const double k44 = sd != nullptr ? StemData::rate / t.sampleRate : 1.0;
    auto fetch = [&] (double p, double r, float& oL, float& oR)
    {
        oL = sinc.interpolate (L, n, p, r);
        oR = sinc.interpolate (R, n, p, r);
        if (! useStems)
            return;
        for (size_t k = 0; k < stemGain.size(); ++k)
            stemGain[k] += juce::jlimit (-stemStep, stemStep, stemTarget[k] - stemGain[k]);
        const double q = p * k44;
        stemBlend += juce::jlimit (-stemStep * 4.0f, stemStep * 4.0f, (sd->readyAt (q) ? 1.0f : 0.0f) - stemBlend);
        if (stemBlend <= 0.0f)
            return;
        const float gm = stemGain[3];
        float sL = gm * oL, sR = gm * oR;
        const double rq = r * k44;
        for (int s = 0; s < StemData::numStored; ++s)
        {
            const float d = stemGain[(size_t) s] - gm;
            const bool hats = s == StemData::drums && stemEqActive;
            if (d == 0.0f && ! hats) continue;
            const float xL = sinc.interpolate (sd->read[(size_t) s][0], sd->n, q, rq);
            const float xR = sinc.interpolate (sd->read[(size_t) s][1], sd->n, q, rq);
            sL += d * xL;
            sR += d * xR;
            if (hats)
            {
                // Hi-hats: the drums' top (a running high-pass, so kick + hats always add up to the drums).
                const float dh = stemGain[4] - stemGain[(size_t) s];
                const float x[2] { xL, xR };
                float h[2];
                for (size_t c = 0; c < 2; ++c)
                {
                    auto& z = hatState[c];
                    const double y = hatCoef[0] * x[c] + hatCoef[1] * z[0] + hatCoef[2] * z[1] - hatCoef[3] * z[2] - hatCoef[4] * z[3];
                    z[1] = z[0]; z[0] = x[c]; z[3] = z[2]; z[2] = y;
                    h[c] = (float) y;
                }
                sL += dh * h[0];
                sR += dh * h[1];
            }
        }
        oL += (sL - oL) * stemBlend;
        oR += (sR - oR) * stemBlend;
    };

    bool hitEnd = false;

    // Key lock / key shift path: read the track at its natural pitch, let the
    // stretcher change tempo (and transpose by key shift). Not used for scratch,
    // reverse or brake, which are meant to sound like a turntable.
    const int shift = keyShift.load();
    const bool wantStretch = (keyLock.load() || shift != 0) && active && ! scr && ! rev && brakeFactor >= 1.0
                             && numSamples * 2 + 4 < stretch->maxIn;
    if (! wantStretch && stretchActive)
    {
        // Realign to what was heard — unless a jump just put the playhead exactly where it must be
        // (CUE released: the deck stops right on the cue point).
        if (! needPrime)
            pos = juce::jlimit (0.0, (double) n - 1.0, pos - stretchLead * srcToOut);
        stretchActive = false;
        stretchLead = 0.0;
    }

    if (wantStretch)
    {
        auto& st = *stretch;
        const double tempo = juce::jmax (0.05, (1.0 + pitch.load() + bend.load()) * (1.0 + phaseCorrection.load()));
        auto readSample = [&] (double p, float& outL, float& outR)
        {
            p = juce::jlimit (0.0, (double) n - 1.0, p);
            fetch (p, srcToOut, outL, outR);
        };

        // Reads the stretcher's next input sample at p and advances p, following the loop:
        // the last 2 ms before the loop end are crossfaded into the loop start (seamless wrap).
        const double loopLen = lEnd - lStart;
        const double xfIn = 96.0 * srcToOut;
        auto feed = [&] (double& p, float& outL, float& outR)
        {
            readSample (p, outL, outR);
            if (looping && loopLen > xfIn * 2.0 && p >= lEnd - xfIn && p < lEnd)
            {
                float wl, wr;
                readSample (p - loopLen, wl, wr);
                const float w = (float) ((p - (lEnd - xfIn)) / xfIn);
                outL = outL * (1.0f - w) + wl * w;
                outR = outR * (1.0f - w) + wr * w;
            }
            p += srcToOut;
            while (looping && loopLen > 0.0 && p >= lEnd)
                p -= loopLen;
        };

        // A loop engaged while the stretcher reads ahead: if its read head is already past the
        // loop end (short loops), restart it at the heard position, which is inside the loop.
        if (looping && stretchActive && lEnd > lStart)
        {
            const double heardNow = pos - stretchLead * srcToOut;
            if ((pos >= lEnd || pos < lStart) && heardNow >= lStart - 1.0 && heardNow < lEnd)
            {
                stretchJumping (srcToOut);
                needPrime = true;
            }
        }

        if (! stretchActive || needPrime)
        {
            // Start (or restart after a jump) producing output immediately at the heard position.
            if (stretchXfadePending)
            {
                st.cur ^= 1;                                   // the old stretcher keeps playing for the crossfade
                stretchXfadeLeft = stretchXfadeLen;
            }
            auto& dsp = st.dsp[(size_t) st.cur];
            dsp.setTransposeSemitones ((float) shift);
            const int seekLen = juce::jmin ((int) st.preL.size() - 8, dsp.outputSeekLength ((float) tempo));
            double p = pos;
            for (int k = 0; k < seekLen; ++k)
                feed (p, st.preL[(size_t) k], st.preR[(size_t) k]);      // the pre-roll follows the loop too
            float* preP[2] = { st.preL.data(), st.preR.data() };
            dsp.outputSeek (preP, seekLen);
            pos = p;
            stretchLead = seekLen;
            stretchActive = true;
            stretchXfadePending = false;
            inCarry = 0.0;
        }
        needPrime = false;

        auto& dsp = st.dsp[(size_t) st.cur];
        dsp.setTransposeSemitones ((float) shift);

        inCarry += numSamples * tempo;
        const int nIn = juce::jlimit (0, st.maxIn, (int) inCarry);
        inCarry -= nIn;

        for (int k = 0; k < nIn; ++k)
            feed (pos, st.inL[(size_t) k], st.inR[(size_t) k]);

        float* inP[2]  = { st.inL.data(), st.inR.data() };
        float* outP[2] = { left, right };
        dsp.process (inP, nIn, outP, numSamples);

        if (stretchXfadeLeft > 0)
        {
            // The previous stretcher continues from where it was and fades out.
            auto& od = st.dsp[(size_t) (st.cur ^ 1)];
            for (int k = 0; k < nIn; ++k)
            {
                readSample (stretchOldPos, st.oldInL[(size_t) k], st.oldInR[(size_t) k]);
                stretchOldPos += srcToOut;
            }
            float* oin[2]  = { st.oldInL.data(), st.oldInR.data() };
            float* oout[2] = { st.oldOutL.data(), st.oldOutR.data() };
            od.process (oin, nIn, oout, numSamples);
            for (int i = 0; i < numSamples && stretchXfadeLeft > 0; ++i, --stretchXfadeLeft)
            {
                const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) stretchXfadeLeft / (float) stretchXfadeLen);
                left[i]  = left[i]  * (1.0f - w) + st.oldOutL[(size_t) i] * w;
                right[i] = right[i] * (1.0f - w) + st.oldOutR[(size_t) i] * w;
            }
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float target = active ? 1.0f : 0.0f;
            if (gain < target)      gain = juce::jmin (target, gain + rampStep);
            else if (gain > target) gain = juce::jmax (target, gain - rampStep);
            left[i] *= gain;
            right[i] *= gain;
        }

        const double heard = pos - stretchLead * srcToOut;
        if (heard >= (double) n - 1.0)
            hitEnd = true;

        if (hitEnd)
        {
            playRequested = false;
            gain = 0.0f;
        }

        audible = ! hitEnd;
        renderedBpm = t.grid().isValid() ? t.grid().bpm * (1.0 + pitch.load()) : 0.0;
        publishedPos = juce::jlimit (0.0, (double) n - 1.0, heard) / t.sampleRate;
        publishedRate = hitEnd ? 0.0 : tempo;
        publishedAtMs = juce::Time::getMillisecondCounterHiRes();
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float target = active ? 1.0f : 0.0f;
        if (gain < target)      gain = juce::jmin (target, gain + rampStep);
        else if (gain > target) gain = juce::jmax (target, gain - rampStep);

        // Silent and stopped: nothing to do (except finishing a fade-out of a jump).
        const bool advancing = active || gain > 0.0f;
        if (! advancing && fadeLeft <= 0)
            break;

        float outL = 0.0f, outR = 0.0f;
        if (advancing)
        {
            fetch (pos, rate, outL, outR);
            outL *= gain;
            outR *= gain;
        }

        if (fadeLeft > 0)
        {
            // Raised-cosine crossfade from the pre-jump position (at its own level).
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) fadeLeft / (float) jumpFadeLen);
            const double fr = advancing ? rate : t.sampleRate / outRate;
            float fl, frr;
            fetch (fadeFrom, fr, fl, frr);
            outL = outL * (1.0f - w) + fl * w * fadeGain;
            outR = outR * (1.0f - w) + frr * w * fadeGain;
            fadeFrom += fr;
            --fadeLeft;
        }

        left[i]  = outL;
        right[i] = outR;

        if (! advancing)
            continue;                  // parked (e.g. gate cue released): keep the exact position

        pos += rate;

        if (looping && lEnd > lStart && rate > 0.0 && pos >= lEnd)
        {
            beginJump (pos);           // the audio just past the loop end fades out
            pos -= (lEnd - lStart);
        }

        if (pos < 0.0)
            pos = 0.0;

        if (pos >= (double) n - 1.0)
        {
            pos = (double) n - 1.0;
            if (! scr)
            {
                hitEnd = true;
                break;
            }
        }
    }

    if (hitEnd)
    {
        playRequested = false;
        gain = 0.0f;
    }

    audible = active && ! hitEnd;
    renderedBpm = t.grid().isValid() ? t.grid().bpm * (1.0 + pitch.load()) : 0.0;
    publishedPos = pos / t.sampleRate;
    publishedRate = active && ! hitEnd ? rate * outRate / t.sampleRate : 0.0;
    publishedAtMs = juce::Time::getMillisecondCounterHiRes();
}

} // namespace om
