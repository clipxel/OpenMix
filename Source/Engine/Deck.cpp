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

    cuePoint = track ? juce::jlimit (0.0, track->lengthSeconds(), track->meta.cuePoint) : 0.0;
    hotCues = track ? track->meta.hotCues : std::array<double, 8> { -1, -1, -1, -1, -1, -1, -1, -1 };
    savedLoops = track ? track->meta.savedLoops : std::array<std::pair<double, double>, 8> {};
    reverse = censor = scratching = rollActive = brake = false;

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
        // Gate cue (club player): plays while the pad is held, returns on release.
        requestJump (Jump::absolute, c);
        publishedPos = c;
        publishedAtMs = juce::Time::getMillisecondCounterHiRes();
        gateSlot = slot;
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

void Deck::exitLoop() { loopActive = false; }

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

    // Musical clock for effects / RMX scheduled on this deck (heard position, start of block).
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
            outL = sinc.interpolate (L, n, p, srcToOut);
            outR = sinc.interpolate (R, n, p, srcToOut);
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
            outL = sinc.interpolate (L, n, pos, rate) * gain;
            outR = sinc.interpolate (R, n, pos, rate) * gain;
        }

        if (fadeLeft > 0)
        {
            // Raised-cosine crossfade from the pre-jump position (at its own level).
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) fadeLeft / (float) jumpFadeLen);
            const double fr = advancing ? rate : t.sampleRate / outRate;
            outL = outL * (1.0f - w) + sinc.interpolate (L, n, fadeFrom, fr) * w * fadeGain;
            outR = outR * (1.0f - w) + sinc.interpolate (R, n, fadeFrom, fr) * w * fadeGain;
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
