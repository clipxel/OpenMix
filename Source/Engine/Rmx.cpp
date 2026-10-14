#include "Rmx.h"
#include <signalsmith-stretch.h>

namespace om
{

juce::String RmxUnit::padName (int pad)
{
    switch (pad)
    {
        case kick:      return "KICK";
        case snare:     return "SNARE";
        case clap:      return "CLAP";
        case hat:       return "HAT";
        case buildUp:   return "BUILD UP";
        case breakDown: return "BREAK DOWN";
        case echoOut:   return "ECHO OUT";
        case brake:     return "BRAKE";
        default:        break;
    }
    return {};
}

void RmxUnit::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    synthesiseDrums();
    {
        const juce::SpinLock::ScopedLockType sl (sampleLock);
        updateActive();
    }
    for (auto& v : voices) v = {};

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, maxBlock), 2 };
    for (auto* f : { &hp, &lp, &noiseHp })
    {
        f->prepare (spec);
        f->setResonance (0.9f);
    }
    hp.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    lp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    noiseHp.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    gateSm.reset (sampleRate, 0.003);
    gateSm.setCurrentAndTargetValue (1.0f);
    pendingBeat.fill (-1.0);
    sceneUp = sceneDown = 0.0f;

    sceneEcho.prepare (sampleRate, maxBlock);
    sceneEcho.type = (int) FxUnit::Type::echo;
    sceneEcho.param = 0.3f;          // ½ beat
    sceneEcho.feedback = 0.45f;
    sceneEcho.wet = 0.0f;
    releaseEcho.prepare (sampleRate, maxBlock);
    releaseEcho.type = (int) FxUnit::Type::releaseMute;
    releaseEcho.param = 0.3f;
    releaseEcho.wet = 1.0f;
    turntable.prepare (sampleRate, maxBlock);
    turntable.type = (int) FxUnit::Type::brake;
    turntable.param = 0.25f;         // ≈ 1 s
    turntable.wet = 1.0f;
}

void RmxUnit::synthesiseDrums()
{
    // Original synthesised kit (no third-party samples).
    juce::Random r (7);
    auto make = [this] (double seconds) { juce::AudioBuffer<float> b (2, (int) (seconds * sr)); b.clear(); return b; };

    // KICK: pitch sweep 140 → 45 Hz, exponential decay, short click.
    builtIn[kick] = make (0.45);
    {
        double ph = 0.0;
        for (int i = 0; i < builtIn[kick].getNumSamples(); ++i)
        {
            const double t = i / sr;
            const double f = 45.0 + 95.0 * std::exp (-t * 28.0);
            ph += f / sr;
            const float v = (float) (std::sin (juce::MathConstants<double>::twoPi * ph) * std::exp (-t * 7.0)
                                     + (t < 0.002 ? 0.3 * (1.0 - t / 0.002) : 0.0));
            builtIn[kick].setSample (0, i, v * 0.9f);
            builtIn[kick].setSample (1, i, v * 0.9f);
        }
    }
    // SNARE: 185 Hz body + band-limited noise.
    builtIn[snare] = make (0.3);
    {
        double ph = 0.0; float hpState = 0.0f, prev = 0.0f;
        for (int i = 0; i < builtIn[snare].getNumSamples(); ++i)
        {
            const double t = i / sr;
            ph += 185.0 / sr;
            const float n = r.nextFloat() * 2.0f - 1.0f;
            hpState = 0.92f * (hpState + n - prev); prev = n;           // simple high-pass on the noise
            const float v = (float) (0.45 * std::sin (juce::MathConstants<double>::twoPi * ph) * std::exp (-t * 30.0)
                                     + 0.55 * hpState * std::exp (-t * 14.0));
            builtIn[snare].setSample (0, i, v * 0.8f);
            builtIn[snare].setSample (1, i, v * 0.8f);
        }
    }
    // CLAP: three quick noise bursts then a tail, band-passed around 1.2 kHz.
    builtIn[clap] = make (0.35);
    {
        float bp1 = 0.0f, bp2 = 0.0f;
        const float f = 2.0f * std::sin (juce::MathConstants<float>::pi * 1200.0f / (float) sr);
        for (int i = 0; i < builtIn[clap].getNumSamples(); ++i)
        {
            const double t = i / sr;
            double env = std::exp (-t * 18.0);
            for (double b : { 0.0, 0.011, 0.022 })
                if (t >= b && t < b + 0.008) env = juce::jmax (env, 1.0 - (t - b) / 0.008);
            const float n = (r.nextFloat() * 2.0f - 1.0f) * (float) env;
            bp1 += f * (n - bp1 - 0.6f * bp2);                          // state-variable band-pass
            bp2 += f * bp1;
            builtIn[clap].setSample (0, i, bp1 * 0.9f);
            builtIn[clap].setSample (1, i, bp1 * 0.9f);
        }
    }
    // HAT: high-passed noise, very short.
    builtIn[hat] = make (0.08);
    {
        float prev = 0.0f, hpState = 0.0f;
        for (int i = 0; i < builtIn[hat].getNumSamples(); ++i)
        {
            const double t = i / sr;
            const float n = r.nextFloat() * 2.0f - 1.0f;
            hpState = 0.6f * (hpState + n - prev); prev = n;
            const float v = hpState * (float) std::exp (-t * 60.0) * 0.5f;
            builtIn[hat].setSample (0, i, v);
            builtIn[hat].setSample (1, i, v);
        }
    }
}

//==============================================================================
void RmxUnit::padDown (int pad, bool shift)
{
    if (! juce::isPositiveAndBelow (pad, (int) numPads)) return;
    held[(size_t) pad] = true;
    if (pad < numDrums)
    {
        rollBeatsOf[(size_t) pad] = baseRoll.load();
        liveSemis[(size_t) pad] = 0.0f;
        fastRoll[(size_t) pad] = shift;
        triggerPending[(size_t) pad] = true;
    }
    else if (pad == echoOut)
        releaseEcho.enabled = true;
    else if (pad == brake)
        backspinMode = shift;
}

void RmxUnit::padUp (int pad)
{
    if (juce::isPositiveAndBelow (pad, (int) numPads))
        held[(size_t) pad] = false;
    if (juce::isPositiveAndBelow (pad, numDrums))
    {
        liveSemis[(size_t) pad] = 0.0f;                 // the gesture is momentary
        rollBeatsOf[(size_t) pad] = baseRoll.load();
    }
}

void RmxUnit::padGesture (int pad, float semitones, int rollSteps)
{
    if (! juce::isPositiveAndBelow (pad, numDrums)) return;
    liveSemis[(size_t) pad] = juce::jlimit (-12.0f, 12.0f, semitones);
    rollBeatsOf[(size_t) pad] = juce::jlimit (0.0625, 1.0, baseRoll.load() * std::pow (0.5, rollSteps));
}

bool RmxUnit::isLit (int pad) const
{
    if (! juce::isPositiveAndBelow (pad, (int) numPads)) return false;
    if (pad < numDrums)
        return held[(size_t) pad].load() || juce::Time::getMillisecondCounterHiRes() < flashUntil[(size_t) pad].load();
    if (pad == echoOut) return releaseEcho.enabled.load();
    return held[(size_t) pad].load();
}

void RmxUnit::setSample (int pad, std::shared_ptr<juce::AudioBuffer<float>> sample, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (pad, numDrums)) return;
    auto old = userSamples[(size_t) pad];
    auto oldProcessed = processed[(size_t) pad];   // both released below, on the message thread
    {
        const juce::SpinLock::ScopedLockType sl (sampleLock);
        userSamples[(size_t) pad] = std::move (sample);
        processed[(size_t) pad] = nullptr;      // re-rendered by the engine when TONE / SPEED are set
        for (auto& v : voices)                  // voices still playing the old buffers must not read freed memory
            if (v.active && ((old != nullptr && v.buf == old.get()) || (oldProcessed != nullptr && v.buf == oldProcessed.get())))
                v.active = false;
        updateActive();
    }
    names[(size_t) pad] = name;
    // `old` is released here, on the message thread.
}

void RmxUnit::updateActive()
{
    for (int i = 0; i < numDrums; ++i)
        active[(size_t) i] = processed[(size_t) i] != nullptr ? processed[(size_t) i].get()
                           : userSamples[(size_t) i] != nullptr ? userSamples[(size_t) i].get() : &builtIn[(size_t) i];
}

std::shared_ptr<const juce::AudioBuffer<float>> RmxUnit::sourceFor (int pad) const
{
    if (! juce::isPositiveAndBelow (pad, numDrums)) return {};
    if (userSamples[(size_t) pad] != nullptr) return userSamples[(size_t) pad];
    return std::make_shared<juce::AudioBuffer<float>> (builtIn[(size_t) pad]);
}

void RmxUnit::setProcessed (std::array<std::shared_ptr<juce::AudioBuffer<float>>, 4> bufs)
{
    auto old = processed;                       // freed here, on the message thread, after the swap
    {
        const juce::SpinLock::ScopedLockType sl (sampleLock);
        processed = std::move (bufs);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            bool stale = false;
            for (auto& o : old) stale = stale || (o != nullptr && v.buf == o.get());
            if (stale) v.active = false;
        }
        updateActive();
    }
}

std::shared_ptr<juce::AudioBuffer<float>> RmxUnit::render (const juce::AudioBuffer<float>& src, double sampleRate,
                                                           float semitones, float speedRatio)
{
    // Offline time/pitch stretch: TONE changes pitch without changing length, SPEED changes length
    // without changing pitch. The output is aligned to the input start (no latency gap before the hit).
    const int inLen = src.getNumSamples();
    if (inLen <= 0) return {};
    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault (2, (float) sampleRate);
    st.setTransposeSemitones (semitones);
    const int outLen = juce::jmax (1, (int) std::round (inLen / (double) speedRatio));
    const int seekLen = st.outputSeekLength (speedRatio);
    const int tailPad = seekLen + (int) (0.1 * sampleRate);
    juce::AudioBuffer<float> in (2, inLen + tailPad);
    in.clear();
    for (int c = 0; c < 2; ++c)
        in.copyFrom (c, 0, src, juce::jmin (c, src.getNumChannels() - 1), 0, inLen);
    auto out = std::make_shared<juce::AudioBuffer<float>> (2, outLen);
    out->clear();
    const float* seekIn[2] = { in.getReadPointer (0), in.getReadPointer (1) };
    st.outputSeek (seekIn, juce::jmin (seekLen, in.getNumSamples()));
    const int consumed = juce::jmin (seekLen, in.getNumSamples());
    const int needIn = juce::jlimit (0, in.getNumSamples() - consumed, (int) std::round (outLen * (double) speedRatio));
    const float* procIn[2] = { in.getReadPointer (0) + consumed, in.getReadPointer (1) + consumed };
    float* procOut[2] = { out->getWritePointer (0), out->getWritePointer (1) };
    st.process (procIn, needIn, procOut, outLen);
    // Short fade at the end so a cut tail never clicks.
    const int fade = juce::jmin (outLen, (int) (0.004 * sampleRate));
    for (int c = 0; c < 2; ++c)
        out->applyGainRamp (c, outLen - fade, fade, 1.0f, 0.0f);
    return out;
}

juce::String RmxUnit::sampleName (int pad) const
{
    return juce::isPositiveAndBelow (pad, numDrums) ? names[(size_t) pad] : juce::String();
}

void RmxUnit::startVoice (int pad, int delaySamples)
{
    for (auto& v : voices)
        if (! v.active)
        {
            v = { active[(size_t) pad], 0.0, delaySamples, true, pad };
            return;
        }
}

//==============================================================================
void RmxUnit::process (float* l, float* r, int n, double beat, double beatsPerSample, double bpm, double stepBeats)
{
    const juce::SpinLock::ScopedTryLockType sl (sampleLock);
    const bool canPlay = sl.isLocked();
    const bool hasGrid = beat >= 0.0 && beatsPerSample > 0.0;
    const double bps = hasGrid ? beatsPerSample : (120.0 / 60.0) / sr;   // free tempo when no grid
    const double startBeat = hasGrid ? beat : 0.0;
    const double endBeat = startBeat + n * bps;
    juce::ignoreUnused (stepBeats);

    // ── X-PAD: the hit sounds the instant the pad is pressed; while held, the roll hits sit on the
    // deck's grid (multiples of the roll step), so the roll is always in time (1/16 by VEL., ⇧ 1/32) ──
    for (int p = 0; p < numDrums && canPlay; ++p)
    {
        const double rollStep = fastRoll[(size_t) p].load() ? 0.125 : rollBeatsOf[(size_t) p].load();
        auto& next = nextRollBeat[(size_t) p];
        if (triggerPending[(size_t) p].load())
        {
            triggerPending[(size_t) p] = false;
            startVoice (p, 0);
            // First roll hit: the next grid line at least half a step away (never two hits glued together).
            next = hasGrid ? std::ceil ((startBeat + 0.5 * rollStep) / rollStep - 1.0e-6) * rollStep
                           : startBeat + rollStep;
            flashUntil[(size_t) p] = juce::Time::getMillisecondCounterHiRes() + 120.0;
        }
        else if (held[(size_t) p].load())
        {
            if (! hasGrid) next -= n * bps;                            // keep relative in free tempo
            const double base = hasGrid ? startBeat : 0.0;
            const double end  = hasGrid ? endBeat : n * bps;
            if (hasGrid)
            {
                // Every roll hit sits on the absolute grid of the current speed: after a VEL. / gesture
                // change the next hit snaps to the next multiple of the new step, never drifting.
                next = std::ceil (juce::jmax (next, base) / rollStep - 1.0e-6) * rollStep;
            }
            else
                while (next < base) next += rollStep;                 // never fire a roll hit late
            while (next < end)
            {
                startVoice (p, juce::jlimit (0, n - 1, (int) std::round ((next - base) / bps)));
                next += rollStep;
            }
        }
    }

    // ── SCENE FX ──
    // BUILD UP (held): over 8 beats the low end is filtered away, a noise riser climbs and the track is
    // chopped on the grid at 1/4 → 1/8 → 1/16 beats, with a ½-beat echo. Release = instant drop.
    // BREAK DOWN (held): the filter closes down to the bass and a 1-beat echo trails the track.
    const bool upHeld = held[buildUp].load(), downHeld = held[breakDown].load();
    if ((upHeld && ! upWasHeld) || (downHeld && ! downWasHeld)) sceneBeats = 0.0;
    if (! upWasHeld && upHeld && sceneUp <= 0.0f)   { hp.reset(); noiseHp.reset(); }
    if (! downWasHeld && downHeld && sceneDown <= 0.0f) lp.reset();
    upWasHeld = upHeld; downWasHeld = downHeld;
    sceneBeats += n * bps;
    const float ramp = (float) juce::jlimit (0.0, 1.0, sceneBeats / 8.0);
    const float fall = (float) n / (float) (0.02 * sr);                 // ≈ 20 ms back to dry
    sceneUp   = upHeld   ? ramp : juce::jmax (0.0f, sceneUp - fall);
    sceneDown = downHeld ? ramp : juce::jmax (0.0f, sceneDown - fall);

    if (sceneUp > 0.0f)
    {
        hp.setCutoffFrequency (30.0f * std::pow (40.0f, sceneUp));           // 30 Hz → 1.2 kHz
        noiseHp.setCutoffFrequency (400.0f * std::pow (20.0f, sceneUp));     // rising noise sweep
        const float noiseLevel = 0.12f * sceneUp * sceneUp;
        // Grid-locked chop: off for the second half of each division, deeper as it grows.
        const double div = ! upHeld || ! hasGrid ? 0.0 : sceneBeats < 4.0 ? 0.0 : sceneBeats < 6.0 ? 0.5 : sceneBeats < 7.0 ? 0.25 : 0.125;
        const float depth = juce::jlimit (0.0f, 0.85f, (sceneUp - 0.45f) * 1.6f);
        for (int i = 0; i < n; ++i)
        {
            float gate = 1.0f;
            if (div > 0.0)
            {
                const double b = startBeat + i * bps;
                gate = std::fmod (b, div) < div * 0.5 ? 1.0f : 1.0f - depth;
            }
            gateSm.setTargetValue (gate);
            const float g = gateSm.getNextValue();
            const float nz = noiseHp.processSample (0, rnd.nextFloat() * 2.0f - 1.0f) * noiseLevel;
            l[i] = hp.processSample (0, l[i]) * g + nz;
            r[i] = hp.processSample (1, r[i]) * g + nz;
        }
    }
    else gateSm.setCurrentAndTargetValue (1.0f);
    if (sceneDown > 0.0f)
    {
        lp.setCutoffFrequency (18000.0f * std::pow (0.015f, sceneDown));    // down to ≈ 270 Hz
        for (int i = 0; i < n; ++i)
        {
            l[i] = lp.processSample (0, l[i]);
            r[i] = lp.processSample (1, r[i]);
        }
    }
    // The echo follows the scene; once released its tail rings out at the level it had.
    sceneEcho.param = downHeld ? 0.6f : 0.3f;                            // 1 beat / ½ beat
    sceneEcho.enabled = upHeld || downHeld;
    if (upHeld || downHeld) sceneEcho.wet = 0.45f * ramp;
    sceneEcho.process (l, r, n, bpm);

    // ── RELEASE FX ──
    releaseEcho.process (l, r, n, bpm);
    turntable.type = (int) (backspinMode.load() ? FxUnit::Type::backspin : FxUnit::Type::brake);
    turntable.enabled = held[brake].load();
    turntable.process (l, r, n, bpm);

    // ── Mix the X-pad voices on top (they follow the channel fader) ──
    // A held pad moved up / down plays its voices faster / slower (live tone, cubic interpolation).
    const float lvl = drumLevel.load();
    auto hermite = [] (const float* s, int len, double p)
    {
        const int i = (int) p;
        const float t = (float) (p - i);
        const float xm = s[juce::jmax (0, i - 1)], x0 = s[i], x1 = s[juce::jmin (len - 1, i + 1)], x2 = s[juce::jmin (len - 1, i + 2)];
        const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    };
    for (auto& v : voices)
    {
        if (! v.active || v.buf == nullptr) continue;
        const int len = v.buf->getNumSamples();
        const float* sL = v.buf->getReadPointer (0);
        const float* sR = v.buf->getReadPointer (v.buf->getNumChannels() > 1 ? 1 : 0);
        const float semis = liveSemis[(size_t) v.pad].load();
        if (std::abs (semis) < 0.01f)
        {
            int ip = (int) v.pos;
            for (int i = v.delay; i < n && ip < len; ++i, ++ip)
            {
                l[i] += sL[ip] * lvl;
                r[i] += sR[ip] * lvl;
            }
            v.pos = ip;
        }
        else
        {
            const double rate = std::pow (2.0, semis / 12.0);
            for (int i = v.delay; i < n && v.pos < len - 1; ++i, v.pos += rate)
            {
                l[i] += hermite (sL, len, v.pos) * lvl;
                r[i] += hermite (sR, len, v.pos) * lvl;
            }
            if (v.pos >= len - 1) v.pos = len;
        }
        v.delay = 0;
        if (v.pos >= len) v.active = false;
    }
}

} // namespace om
