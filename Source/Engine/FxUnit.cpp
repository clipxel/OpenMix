#include "FxUnit.h"
#include "../Common/Strings.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace om
{

//==============================================================================
juce::String FxUnit::typeName (Type t)
{
    switch (t)
    {
        case Type::echo:       return "ECHO";
        case Type::delay:      return "DELAY";
        case Type::reverb:     return "REVERB";
        case Type::flanger:    return "FLANGER";
        case Type::phaser:     return "PHASER";
        case Type::gate:       return "GATE";
        case Type::bitcrusher: return "BITCRUSH";
        case Type::noise:      return "NOISE";
        case Type::filter:     return "FILTER";
        case Type::roll:       return "ROLL";
        case Type::backspin:   return "BACKSPIN";
        case Type::brake:      return "VINYL BRAKE";
        case Type::shortDelay:   return "SHORT DELAY";
        case Type::tapeEcho:     return "TAPE ECHO";
        case Type::shimmer:      return "SHIMMER";
        case Type::echoVerb:     return "ECHO-VERB";
        case Type::spiral:       return "SPIRAL";
        case Type::trans:        return "TRANS";
        case Type::pitch:        return "PITCH";
        case Type::mobius:       return "MOBIUS";
        case Type::tripletFilter:return "TRIPLET FILTER";
        case Type::tripletRoll:  return "TRIPLET ROLL";
        case Type::releaseDry:   return "REL. ECHO DRY";
        case Type::releaseMute:  return "REL. ECHO MUTE";
        case Type::pingPong:     return "PING PONG";
        case Type::plugin:     return "PLUGIN";
        case Type::numTypes:   break;
    }
    return {};
}

juce::String FxUnit::paramName (Type t)
{
    switch (t)
    {
        case Type::echo: case Type::delay: case Type::pingPong: case Type::gate: case Type::roll: return "BEATS";
        case Type::flanger: case Type::phaser: return "RATE";
        case Type::reverb:     return "SIZE";
        case Type::bitcrusher: return "BITS";
        case Type::noise: case Type::filter: return "FREQ";
        case Type::backspin: case Type::brake: return "LENGTH";
        case Type::shortDelay: case Type::tapeEcho: case Type::echoVerb: case Type::spiral: case Type::trans:
        case Type::tripletRoll: case Type::releaseDry: case Type::releaseMute: return "BEATS";
        case Type::shimmer:      return "SIZE";
        case Type::pitch:        return "PITCH";
        case Type::mobius: case Type::tripletFilter: return "RATE";
        case Type::plugin: case Type::numTypes: break;
    }
    return "PARAM";
}

double FxUnit::beatsForParam (Type t, float p) noexcept
{
    static constexpr double echoDiv[] = { 0.125, 0.25, 0.5, 0.75, 1.0, 2.0, 4.0, 8.0 };   // club mixer beat values
    static constexpr double gateDiv[] = { 1.0 / 16, 1.0 / 8, 0.25, 0.5, 1.0 };
    static constexpr double rollDiv[] = { 1.0 / 16, 1.0 / 8, 0.25, 0.5, 1.0 };
    static constexpr double lfoDiv[]  = { 16.0, 8.0, 4.0, 2.0, 1.0 };
    static constexpr double shortDiv[] = { 1.0 / 32, 1.0 / 16, 1.0 / 8, 3.0 / 16, 0.25 };
    static constexpr double transDiv[] = { 1.0 / 16, 1.0 / 8, 0.25, 0.5, 1.0 };
    static constexpr double tripDiv[]  = { 1.0 / 6, 1.0 / 3, 2.0 / 3, 4.0 / 3, 8.0 / 3 };
    static constexpr double longDiv[]  = { 0.5, 0.75, 1.0, 2.0, 4.0 };
    const auto i = (size_t) juce::jlimit (0, 4, (int) std::floor (p * 4.999f));
    if (t == Type::shortDelay)                    return shortDiv[i];
    if (t == Type::trans)                         return transDiv[i];
    if (t == Type::tripletRoll || t == Type::tripletFilter) return tripDiv[i];
    if (t == Type::mobius)                        return longDiv[i] * 4.0;
    if (t == Type::gate)                          return gateDiv[i];
    if (t == Type::roll)                          return rollDiv[i];
    if (t == Type::flanger || t == Type::phaser)  return lfoDiv[i];
    return echoDiv[(size_t) juce::jlimit (0, 7, (int) std::floor (p * 7.999f))];
}

juce::String FxUnit::paramText (Type t, float p)
{
    auto beats = [] (double b) { return b < 1.0 ? "1/" + juce::String ((int) std::round (1.0 / b)) : juce::String (b, b == std::floor (b) ? 0 : 2); };
    switch (t)
    {
        case Type::echo: case Type::delay: case Type::pingPong: case Type::gate: case Type::roll:
            return beats (beatsForParam (t, p)) + " beat";
        case Type::flanger: case Type::phaser:
            return juce::String::fromUTF8 ("LFO ") + beats (beatsForParam (t, p)) + " beats";
        case Type::reverb:     return juce::String ((int) std::round (p * 100)) + "%";
        case Type::bitcrusher: return juce::String (juce::roundToInt (juce::jmap (p, 16.0f, 3.0f))) + " bits";
        case Type::noise: case Type::filter:
            return p < 0.47f ? "LPF " + juce::String ((int) std::round ((0.5f - p) * 200)) + "%"
                 : p > 0.53f ? "HPF " + juce::String ((int) std::round ((p - 0.5f) * 200)) + "%" : juce::String ("OFF");
        case Type::backspin: case Type::brake:
            return juce::String (juce::jmap (p, 0.3f, 3.0f), 1) + " s";
        case Type::shortDelay: case Type::trans: case Type::tripletRoll: case Type::tripletFilter:
            return beats (beatsForParam (t, p)) + " beat";
        case Type::tapeEcho: case Type::echoVerb: case Type::spiral: case Type::releaseDry: case Type::releaseMute:
            return beats (beatsForParam (Type::echo, p)) + " beat";
        case Type::mobius:       return juce::String::fromUTF8 ("ciclo ") + beats (beatsForParam (t, p)) + " beats";
        case Type::shimmer:      return juce::String ((int) std::round (p * 100)) + "%";
        case Type::pitch:
        {
            const int semi = juce::roundToInt ((p - 0.5f) * 24.0f);
            return (semi > 0 ? "+" : "") + juce::String (semi) + " st";
        }
        case Type::plugin: case Type::numTypes: break;
    }
    return {};
}

//==============================================================================
void FxUnit::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    maxBlockSize = juce::jmax (1, maxBlock);
    const auto len = (size_t) (sampleRate * 4.5);
    dl.assign (len, 0.0f);
    dr.assign (len, 0.0f);
    writePos = 0;
    rollL.assign ((size_t) (sampleRate * 2.2), 0.0f);
    rollR.assign ((size_t) (sampleRate * 2.2), 0.0f);
    wetSm.reset (sampleRate, 0.03);
    delaySm.reset (sampleRate, 0.08);
    reverb.setSampleRate (sampleRate);
    reverb.reset();
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 2 };
    svf.prepare (spec);
    svf.setResonance (1.2f);
    pluginBuf.setSize (2, maxBlockSize);
    for (auto& sh : shifters) sh.prepare (sampleRate);
    dryGain.reset (sampleRate, 0.006);
    dryGain.setCurrentAndTargetValue (1.0f);
}

void FxUnit::setPlugin (juce::AudioPluginInstance* p)
{
    const juce::SpinLock::ScopedLockType sl (pluginLock);
    plugin = p;
}

void FxUnit::process (float* l, float* r, int n, double bpm)
{
    const auto t = (Type) type.load();
    const bool on = enabled.load();
    if (on) lastWet = wet.load();

    const bool turntable = t == Type::brake || t == Type::backspin;
    const bool release = t == Type::releaseDry || t == Type::releaseMute;
    if ((turntable || (release && tailEnergy < 1.0e-5f && ! dryGain.isSmoothing()))
        && ! on && ! wetSm.isSmoothing() && wetSm.getCurrentValue() <= 0.0001f)
    {
        recordRing (l, r, n);      // keep the recent audio ready for an instant start
        ttActive = false;
        return;
    }

    // Echo / delay tails ring out after switching off; everything else fades out.
    const bool hasTail = t == Type::echo || t == Type::delay || t == Type::pingPong || t == Type::shortDelay || t == Type::tapeEcho
                      || t == Type::spiral || t == Type::echoVerb || t == Type::releaseDry || t == Type::releaseMute;
    wetSm.setTargetValue (on || (hasTail && tailEnergy > 1.0e-5f) ? lastWet : 0.0f);

    if (! on && ! wetSm.isSmoothing() && wetSm.getCurrentValue() <= 0.0001f && (! hasTail || tailEnergy < 1.0e-5f))
    {
        wasOn = false;
        rolling = false;
        return;
    }

    const double effBpm = bpm > 1.0 ? bpm : 120.0;
    switch (t)
    {
        case Type::echo:       { DelayOpts o; o.fbScale = 1.25f; runDelayCore (l, r, n, beatsForParam (t, param.load()) * 60.0 / effBpm, o); break; }
        case Type::delay:      { DelayOpts o; o.fbScale = 0.0f;  runDelayCore (l, r, n, beatsForParam (t, param.load()) * 60.0 / effBpm, o); break; }
        case Type::pingPong:   { DelayOpts o; o.pingPong = true; o.fbScale = 1.25f; runDelayCore (l, r, n, beatsForParam (t, param.load()) * 60.0 / effBpm, o); break; }
        case Type::reverb:     runReverb (l, r, n); break;
        case Type::flanger:    runModDelay (l, r, n, effBpm); break;
        case Type::phaser:     runPhaser (l, r, n, effBpm); break;
        case Type::gate:       runGate (l, r, n, effBpm); break;
        case Type::bitcrusher: runCrush (l, r, n); break;
        case Type::noise:      runNoise (l, r, n); break;
        case Type::filter:     runFilter (l, r, n); break;
        case Type::roll:       runRoll (l, r, n, effBpm); break;
        case Type::plugin:     runPlugin (l, r, n); break;
        case Type::shortDelay: { DelayOpts o; o.fbScale = 0.55f; runDelayCore (l, r, n, beatsForParam (t, param.load()) * 60.0 / effBpm, o); break; }
        case Type::tapeEcho:   { DelayOpts o; o.tape = true; runDelayCore (l, r, n, beatsForParam (Type::echo, param.load()) * 60.0 / effBpm, o); break; }
        case Type::spiral:     { DelayOpts o; o.glide = true; o.fbScale = 1.15f; runDelayCore (l, r, n, beatsForParam (Type::echo, param.load()) * 60.0 / effBpm, o); break; }
        case Type::shimmer:    runShimmer (l, r, n); break;
        case Type::echoVerb:   runEchoVerb (l, r, n, effBpm); break;
        case Type::trans:      runGate (l, r, n, effBpm); break;
        case Type::pitch:      runPitch (l, r, n); break;
        case Type::mobius:     runMobius (l, r, n, effBpm); break;
        case Type::tripletFilter: runTripletFilter (l, r, n, effBpm); break;
        case Type::tripletRoll:   runRoll (l, r, n, effBpm); break;
        case Type::releaseDry:  runRelease (l, r, n, effBpm, false); break;
        case Type::releaseMute: runRelease (l, r, n, effBpm, true); break;
        case Type::brake:      runTurntable (l, r, n, false); break;
        case Type::backspin:   runTurntable (l, r, n, true); break;
        case Type::numTypes:   break;
    }
    wasOn = on;
}

//==============================================================================
void FxUnit::runEcho (float* l, float* r, int n, double bpm, bool pingPong)
{
    const bool on = enabled.load();
    const double delaySeconds = beatsForParam (pingPong ? Type::delay : Type::echo, param.load()) * 60.0 / bpm;
    delaySm.setTargetValue ((float) juce::jlimit (0.01, 4.0, delaySeconds) * (float) sr);
    const float fb = juce::jlimit (0.0f, 0.9f, feedback.load());
    const float inGain = on ? 1.0f : 0.0f;
    const int len = (int) dl.size();
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float d = delaySm.getNextValue();
        float rp = (float) writePos - d;
        while (rp < 0.0f) rp += (float) len;
        const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
        const float t = rp - std::floor (rp);
        const float el = dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0]);
        const float er = dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0]);

        if (pingPong)
        {
            // Cross-feedback: repeats bounce left ↔ right.
            dl[(size_t) writePos] = soft (0.5f * (l[i] + r[i]) * inGain + er * fb);
            dr[(size_t) writePos] = soft (el * fb);
        }
        else
        {
            dl[(size_t) writePos] = soft (l[i] * inGain + el * fb);
            dr[(size_t) writePos] = soft (r[i] * inGain + er * fb);
        }
        writePos = (writePos + 1) % len;

        const float w = wetSm.getNextValue();
        l[i] = l[i] * (1.0f - 0.25f * w) + el * 0.6f * w;
        r[i] = r[i] * (1.0f - 0.25f * w) + er * 0.6f * w;
        peak = juce::jmax (peak, std::abs (el), std::abs (er));
    }
    tailEnergy = peak;
    if (! on && tailEnergy < 1.0e-5f)
    {
        std::fill (dl.begin(), dl.end(), 0.0f);
        std::fill (dr.begin(), dr.end(), 0.0f);
    }
}

void FxUnit::runReverb (float* l, float* r, int n)
{
    juce::Reverb::Parameters p;
    p.roomSize = juce::jmap (param.load(), 0.3f, 0.95f);
    p.damping = 0.45f;
    p.width = 1.0f;
    p.wetLevel = 1.0f;
    p.dryLevel = 0.0f;
    reverb.setParameters (p);
    for (int i = 0; i < n; ++i)
    {
        float wl = l[i], wr = r[i];
        reverb.processStereo (&wl, &wr, 1);
        const float w = wetSm.getNextValue();
        l[i] += wl * 0.8f * w;
        r[i] += wr * 0.8f * w;
    }
}

void FxUnit::runModDelay (float* l, float* r, int n, double bpm)
{
    // Flanger: 0.5–7 ms modulated delay, LFO period in beats, feedback.
    const double period = beatsForParam (Type::flanger, param.load()) * 60.0 / bpm;
    const double inc = 1.0 / (period * sr);
    const float fb = juce::jlimit (0.0f, 0.6f, feedback.load());
    const int len = (int) dl.size();
    for (int i = 0; i < n; ++i)
    {
        lfoPhase += inc;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        const float mod = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) lfoPhase);
        const float d = (float) sr * (0.0005f + 0.0065f * mod);
        float rp = (float) writePos - d;
        while (rp < 0.0f) rp += (float) len;
        const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
        const float t = rp - std::floor (rp);
        const float el = dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0]);
        const float er = dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0]);
        dl[(size_t) writePos] = soft (l[i] + el * fb);
        dr[(size_t) writePos] = soft (r[i] + er * fb);
        writePos = (writePos + 1) % len;
        const float w = wetSm.getNextValue();
        l[i] = l[i] * (1.0f - 0.5f * w) + el * 0.5f * w;     // flanger: comb of dry + swept copy
        r[i] = r[i] * (1.0f - 0.5f * w) + er * 0.5f * w;
    }
}

void FxUnit::runPhaser (float* l, float* r, int n, double bpm)
{
    const double period = beatsForParam (Type::phaser, param.load()) * 60.0 / bpm;
    const double inc = 1.0 / (period * sr);
    const float fb = juce::jlimit (0.0f, 0.8f, feedback.load());
    float* ch[2] = { l, r };
    for (int i = 0; i < n; ++i)
    {
        lfoPhase += inc;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        const float mod = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) lfoPhase);
        const float fc = 300.0f * std::pow (12.0f, mod);                 // 300 Hz … 3.6 kHz sweep
        const float tn = std::tan (juce::MathConstants<float>::pi * fc / (float) sr);
        const float a = (tn - 1.0f) / (tn + 1.0f);
        const float w = wetSm.getNextValue();
        for (int c = 0; c < 2; ++c)
        {
            float x = ch[c][i] + phaserFb[c] * fb;
            for (auto& st : apState)
            {
                const float y = a * x + st[(size_t) c];
                st[(size_t) c] = x - a * y;
                x = y;
            }
            phaserFb[c] = soft (x);
            ch[c][i] = ch[c][i] * (1.0f - 0.5f * w) + x * 0.5f * w;
        }
    }
}

void FxUnit::runGate (float* l, float* r, int n, double bpm)
{
    // Rhythmic gate, beat-locked through the shared LFO phase; short ramps avoid clicks.
    const auto gt = (Type) type.load();
    const double period = beatsForParam (gt == Type::trans ? Type::trans : Type::gate, param.load()) * 60.0 / bpm;
    const double inc = 1.0 / (period * sr);
    const float ramp = (float) (1.0 / ((gt == Type::trans ? 0.0006 : 0.003) * sr));   // TRANS: hard cuts
    for (int i = 0; i < n; ++i)
    {
        lfoPhase += inc;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        const float target = lfoPhase < 0.5 ? 1.0f : 0.0f;
        gateEnv += juce::jlimit (-ramp, ramp, target - gateEnv);
        const float w = wetSm.getNextValue();
        const float g = 1.0f - w * (1.0f - gateEnv);
        l[i] *= g;
        r[i] *= g;
    }
}

void FxUnit::runCrush (float* l, float* r, int n)
{
    const float p = param.load();
    const float bits = juce::jmap (p, 16.0f, 3.0f);
    const float steps = std::pow (2.0f, bits - 1.0f);
    const double hold = 1.0 + p * p * 30.0;            // sample-rate reduction grows with the setting
    for (int i = 0; i < n; ++i)
    {
        holdCount += 1.0;
        if (holdCount >= hold)
        {
            holdCount -= hold;
            holdL = std::round (l[i] * steps) / steps;
            holdR = std::round (r[i] * steps) / steps;
        }
        const float w = wetSm.getNextValue();
        l[i] += (holdL - l[i]) * w;
        r[i] += (holdR - r[i]) * w;
    }
}

void FxUnit::runNoise (float* l, float* r, int n)
{
    const float p = param.load();
    svf.setType (p >= 0.5f ? juce::dsp::StateVariableTPTFilterType::highpass : juce::dsp::StateVariableTPTFilterType::lowpass);
    const float a = std::abs (p - 0.5f) * 2.0f;
    svf.setCutoffFrequency (p >= 0.5f ? 200.0f * std::pow (60.0f, a) : 16000.0f * std::pow (0.02f, a));
    for (int i = 0; i < n; ++i)
    {
        const float w = wetSm.getNextValue();
        const float nL = svf.processSample (0, random.nextFloat() * 2.0f - 1.0f) * 0.25f;
        const float nR = svf.processSample (1, random.nextFloat() * 2.0f - 1.0f) * 0.25f;
        l[i] += nL * w;
        r[i] += nR * w;
    }
}

void FxUnit::runFilter (float* l, float* r, int n)
{
    const float p = param.load();
    if (std::abs (p - 0.5f) < 0.03f)
    {
        for (int i = 0; i < n; ++i) wetSm.getNextValue();
        return;
    }
    svf.setType (p > 0.5f ? juce::dsp::StateVariableTPTFilterType::highpass : juce::dsp::StateVariableTPTFilterType::lowpass);
    const float a = std::abs (p - 0.5f) * 2.0f;
    svf.setCutoffFrequency (juce::jlimit (20.0f, (float) sr * 0.45f, p > 0.5f ? 20.0f * std::pow (400.0f, a) : 20000.0f * std::pow (0.004f, a)));
    for (int i = 0; i < n; ++i)
    {
        const float w = wetSm.getNextValue();
        l[i] += (svf.processSample (0, l[i]) - l[i]) * w;
        r[i] += (svf.processSample (1, r[i]) - r[i]) * w;
    }
}

void FxUnit::runRoll (float* l, float* r, int n, double bpm)
{
    // Beat repeat: record one division when switched on, then repeat it (2 ms seam fades).
    const bool on = enabled.load();
    if (on && ! rolling)
    {
        rolling = true;
        const auto rt = (Type) type.load() == Type::tripletRoll ? Type::tripletRoll : Type::roll;
        rollLen = juce::jlimit (16, (int) rollL.size(), (int) std::round (beatsForParam (rt, param.load()) * 60.0 / bpm * sr));
        rollPos = 0;
        rollFilled = 0;
    }
    if (! on) rolling = false;

    const float fadeLen = (float) (0.002 * sr);
    for (int i = 0; i < n; ++i)
    {
        const float w = wetSm.getNextValue();
        float outL = l[i], outR = r[i];
        if (rolling)
        {
            if (rollFilled < rollLen)
            {
                rollL[(size_t) rollFilled] = l[i];
                rollR[(size_t) rollFilled] = r[i];
                ++rollFilled;
            }
            else
            {
                const int edge = juce::jmin (rollPos, rollLen - 1 - rollPos);
                const float fade = juce::jmin (1.0f, (float) edge / fadeLen);
                outL = rollL[(size_t) rollPos] * fade + l[i] * (1.0f - fade) * 0.0f;
                outR = rollR[(size_t) rollPos] * fade;
                rollPos = (rollPos + 1) % rollLen;
            }
        }
        l[i] += (outL - l[i]) * w;
        r[i] += (outR - r[i]) * w;
    }
}

void FxUnit::runPlugin (float* l, float* r, int n)
{
    const juce::SpinLock::ScopedTryLockType sl (pluginLock);
    if (! sl.isLocked() || plugin == nullptr || n > pluginBuf.getNumSamples())
    {
        for (int i = 0; i < n; ++i) wetSm.getNextValue();
        return;
    }
    pluginBuf.copyFrom (0, 0, l, n);
    pluginBuf.copyFrom (1, 0, r, n);
    juce::AudioBuffer<float> view (pluginBuf.getArrayOfWritePointers(), 2, n);
    emptyMidi.clear();
    plugin->processBlock (view, emptyMidi);
    for (int i = 0; i < n; ++i)
    {
        const float w = wetSm.getNextValue();
        l[i] += (view.getSample (0, i) - l[i]) * w;
        r[i] += (view.getSample (1, i) - r[i]) * w;
    }
}

//==============================================================================
float FxUnit::Shifter::process (float x, double ratio, double rate)
{
    const int len = (int) buf.size();
    buf[(size_t) w] = x;
    const double window = rate * 0.06;
    phase += (1.0 - ratio) / window;                 // head drift per sample
    phase -= std::floor (phase);
    float out = 0.0f;
    for (int h = 0; h < 2; ++h)
    {
        const double ph = std::fmod (phase + 0.5 * h, 1.0);
        double rp = (double) w - ph * window;
        while (rp < 0.0) rp += len;
        const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
        const float t = (float) (rp - std::floor (rp));
        const float v = buf[(size_t) i0] + t * (buf[(size_t) i1] - buf[(size_t) i0]);
        out += v * (float) std::sin (juce::MathConstants<double>::pi * ph);   // equal-sum crossfade of the two heads
    }
    w = (w + 1) % len;
    return out * 0.70710678f;
}

void FxUnit::runDelayCore (float* l, float* r, int n, double delaySeconds, DelayOpts o)
{
    const bool on = enabled.load();
    if (o.glide) delaySm.reset (sr, 0.6);           // slow time changes → audible pitch swoops
    delaySm.setTargetValue ((float) juce::jlimit (0.005, 4.0, delaySeconds) * (float) sr);
    const float fb = juce::jlimit (0.0f, 0.92f, feedback.load() * o.fbScale);
    const float inGain = on && o.feedInput ? 1.0f : 0.0f;
    const int len = (int) dl.size();
    const float lpA = (float) std::exp (-juce::MathConstants<double>::twoPi * 3500.0 / sr);
    const float hpA = (float) std::exp (-juce::MathConstants<double>::twoPi * 120.0 / sr);
    float peak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float d = delaySm.getNextValue();
        if (o.tape)
        {
            wowPhase += 0.8 / sr;
            if (wowPhase >= 1.0) wowPhase -= 1.0;
            d *= 1.0f + 0.0025f * (float) std::sin (juce::MathConstants<double>::twoPi * wowPhase);   // tape wow
        }
        float rp = (float) writePos - d;
        while (rp < 0.0f) rp += (float) len;
        const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
        const float t = rp - std::floor (rp);
        float el = dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0]);
        float er = dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0]);

        float fl = el * fb, fr = er * fb;
        if (o.tape)
        {
            // Band-limit and saturate inside the loop: each repeat gets darker and warmer.
            tapeLpL = fl + lpA * (tapeLpL - fl);
            tapeLpR = fr + lpA * (tapeLpR - fr);
            tapeHpL = hpA * (tapeHpL + tapeLpL - tapeHpInL); tapeHpInL = tapeLpL;
            tapeHpR = hpA * (tapeHpR + tapeLpR - tapeHpInR); tapeHpInR = tapeLpR;
            fl = std::tanh (1.6f * tapeHpL) / 1.6f;
            fr = std::tanh (1.6f * tapeHpR) / 1.6f;
        }

        if (o.pingPong)
        {
            dl[(size_t) writePos] = soft (0.5f * (l[i] + r[i]) * inGain + fr);
            dr[(size_t) writePos] = soft (fl);
        }
        else
        {
            dl[(size_t) writePos] = soft (l[i] * inGain + fl);
            dr[(size_t) writePos] = soft (r[i] * inGain + fr);
        }
        writePos = (writePos + 1) % len;

        const float w = wetSm.getNextValue();
        l[i] += el * w;                // dry untouched; repeats added at full level × LEVEL/DEPTH
        r[i] += er * w;
        peak = juce::jmax (peak, std::abs (el), std::abs (er));
    }
    if (o.glide) delaySm.reset (sr, 0.08);
    tailEnergy = peak;
    if (! on && tailEnergy < 1.0e-5f)
    {
        std::fill (dl.begin(), dl.end(), 0.0f);
        std::fill (dr.begin(), dr.end(), 0.0f);
    }
}

void FxUnit::runShimmer (float* l, float* r, int n)
{
    // Reverb whose tail is fed back an octave up: the classic rising "shimmer".
    juce::Reverb::Parameters p;
    p.roomSize = juce::jmap (param.load(), 0.6f, 0.97f);
    p.damping = 0.3f;
    p.width = 1.0f;
    p.wetLevel = 1.0f;
    p.dryLevel = 0.0f;
    reverb.setParameters (p);
    for (int i = 0; i < n; ++i)
    {
        float wl = l[i] * 0.6f + shimmerFbL, wr = r[i] * 0.6f + shimmerFbR;
        reverb.processStereo (&wl, &wr, 1);
        shimmerFbL = soft (shifters[0].process (wl, 2.0, sr) * 0.2f);
        shimmerFbR = soft (shifters[1].process (wr, 2.0, sr) * 0.2f);
        const float w = wetSm.getNextValue();
        l[i] += soft (wl) * 0.4f * w;
        r[i] += soft (wr) * 0.4f * w;
    }
}

void FxUnit::runEchoVerb (float* l, float* r, int n, double bpm)
{
    // Echo first, then the echoes are sent into a reverb (dry kept intact).
    std::array<float, 4096> el {}, er {};
    for (int off = 0; off < n; off += 4096)
    {
        const int m = juce::jmin (4096, n - off);
        for (int i = 0; i < m; ++i) { el[(size_t) i] = 0.0f; er[(size_t) i] = 0.0f; }
        // Run the delay core on a silent buffer fed from the input: echo-only signal.
        const bool on = enabled.load();
        const float fb = juce::jlimit (0.0f, 0.85f, feedback.load());
        delaySm.setTargetValue ((float) (beatsForParam (Type::echo, param.load()) * 60.0 / bpm * sr));
        const int len = (int) dl.size();
        float peak = 0.0f;
        for (int i = 0; i < m; ++i)
        {
            const float d = delaySm.getNextValue();
            float rp = (float) writePos - d;
            while (rp < 0.0f) rp += (float) len;
            const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
            const float t = rp - std::floor (rp);
            const float al = dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0]);
            const float ar = dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0]);
            dl[(size_t) writePos] = soft ((on ? l[off + i] : 0.0f) + al * fb);
            dr[(size_t) writePos] = soft ((on ? r[off + i] : 0.0f) + ar * fb);
            writePos = (writePos + 1) % len;
            float vl = al, vr = ar;
            reverb.processStereo (&vl, &vr, 1);
            el[(size_t) i] = al * 0.65f + soft (vl) * 0.4f;
            er[(size_t) i] = ar * 0.65f + soft (vr) * 0.4f;
            peak = juce::jmax (peak, std::abs (al), std::abs (ar));
        }
        tailEnergy = peak;
        for (int i = 0; i < m; ++i)
        {
            const float w = wetSm.getNextValue();
            l[off + i] += el[(size_t) i] * w;
            r[off + i] += er[(size_t) i] * w;
        }
    }
}

void FxUnit::runPitch (float* l, float* r, int n)
{
    const double semis = std::round ((param.load() - 0.5) * 24.0);
    const double ratio = std::pow (2.0, semis / 12.0);
    for (int i = 0; i < n; ++i)
    {
        const float w = wetSm.getNextValue();
        const float pl = shifters[0].process (l[i], ratio, sr);
        const float pr = shifters[1].process (r[i], ratio, sr);
        l[i] += (pl - l[i]) * w;
        r[i] += (pr - r[i]) * w;
    }
}

void FxUnit::runMobius (float* l, float* r, int n, double bpm)
{
    // Barber-pole flanger: two delay sweeps half a cycle apart, crossfaded so the
    // comb seems to rise forever (Shepard illusion). Cycle length in beats.
    const double period = beatsForParam (Type::mobius, param.load()) * 60.0 / bpm;
    const double inc = 1.0 / (period * sr);
    const int len = (int) dl.size();
    for (int i = 0; i < n; ++i)
    {
        mobiusPhase += inc;
        if (mobiusPhase >= 1.0) mobiusPhase -= 1.0;
        dl[(size_t) writePos] = l[i];
        dr[(size_t) writePos] = r[i];
        float ol = 0.0f, orr = 0.0f;
        for (int h = 0; h < 2; ++h)
        {
            const double ph = std::fmod (mobiusPhase + 0.5 * h, 1.0);
            const float d = (float) sr * (0.0005f + 0.009f * (float) (1.0 - ph));   // delay shrinks → pitch/comb rises
            const float g = (float) std::sin (juce::MathConstants<double>::pi * ph);
            float rp = (float) writePos - d;
            while (rp < 0.0f) rp += (float) len;
            const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
            const float t = rp - std::floor (rp);
            ol += (dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0])) * g;
            orr += (dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0])) * g;
        }
        writePos = (writePos + 1) % len;
        const float w = wetSm.getNextValue();
        l[i] = l[i] * (1.0f - 0.5f * w) + ol * 0.5f * w;
        r[i] = r[i] * (1.0f - 0.5f * w) + orr * 0.5f * w;
    }
}

void FxUnit::runTripletFilter (float* l, float* r, int n, double bpm)
{
    // Low-pass whose cutoff pulses on triplet subdivisions.
    const double period = beatsForParam (Type::tripletFilter, param.load()) * 60.0 / bpm;
    const double inc = 1.0 / (period * sr);
    svf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    for (int i = 0; i < n; ++i)
    {
        lfoPhase += inc;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        if ((i & 15) == 0)
        {
            const float env = (float) std::pow (1.0 - lfoPhase, 2.0);   // snap open on each triplet, then close
            svf.setCutoffFrequency (juce::jlimit (40.0f, (float) sr * 0.45f, 250.0f * std::pow (40.0f, env)));
        }
        const float w = wetSm.getNextValue();
        l[i] += (svf.processSample (0, l[i]) - l[i]) * w;
        r[i] += (svf.processSample (1, r[i]) - r[i]) * w;
    }
}

void FxUnit::runRelease (float* l, float* r, int n, double bpm, bool mute)
{
    // Release echo, as on a club mixer: while idle the recent audio is kept in the delay line, so pressing
    // the button echoes what was just heard straight away — the repeats are whole beat divisions of
    // the music, therefore always in time. Nothing new enters the echo after the press.
    //  · DRY  — the track keeps playing under the fading repeats;
    //  · MUTE — the track is muted for 4 beats while the tail rings, then fades back in by itself.
    const bool on = enabled.load();
    const double delaySeconds = juce::jlimit (0.01, 4.0, beatsForParam (Type::echo, param.load()) * 60.0 / bpm);
    if (on && ! wasOn)
    {
        delaySm.setCurrentAndTargetValue ((float) (delaySeconds * sr));
        releaseHold = (int) ((mute ? 4.0 * 60.0 / bpm : delaySeconds) * sr);
    }
    else
        delaySm.setTargetValue ((float) (delaySeconds * sr));
    const float fb = juce::jlimit (0.0f, 0.75f, feedback.load() * 1.3f);
    const int len = (int) dl.size();
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (releaseHold > 0 && --releaseHold == 0)
            enabled = false;                                  // auto-return
        dryGain.setTargetValue (mute && releaseHold > 0 ? 0.0f : 1.0f);

        const float d = delaySm.getNextValue();
        float rp = (float) writePos - d;
        while (rp < 0.0f) rp += (float) len;
        const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
        const float t = rp - std::floor (rp);
        const float el = dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0]);
        const float er = dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0]);
        dl[(size_t) writePos] = soft (el * fb);
        dr[(size_t) writePos] = soft (er * fb);
        writePos = (writePos + 1) % len;
        const float g = dryGain.getNextValue();
        const float w = juce::jmax (wetSm.getNextValue(), lastWet);   // the tail keeps its level after auto-off
        l[i] = l[i] * g + el * w;
        r[i] = r[i] * g + er * w;
        peak = juce::jmax (peak, std::abs (el), std::abs (er));
    }
    tailEnergy = peak;
}

void FxUnit::recordRing (const float* l, const float* r, int n) noexcept
{
    const int len = (int) dl.size();
    for (int i = 0; i < n; ++i)
    {
        dl[(size_t) writePos] = l[i];
        dr[(size_t) writePos] = r[i];
        writePos = (writePos + 1) % len;
    }
}

void FxUnit::runTurntable (float* l, float* r, int n, bool backspinMode)
{
    // Club-mixer VINYL BRAKE / BACKSPIN: the deck keeps running underneath; the heard sound
    // is a "virtual platter" read from the recent audio, slowing to a stop (or spun backwards).
    // Turning the effect off brings the track back at its real position.
    const bool on = enabled.load();
    const int len = (int) dl.size();
    if (on && ! ttActive)
    {
        ttActive = true;
        ttRead = (double) writePos;
        ttTime = 0.0;
    }
    if (! on) ttActive = false;

    const double length = juce::jmap ((double) param.load(), 0.3, 3.0);
    for (int i = 0; i < n; ++i)
    {
        dl[(size_t) writePos] = l[i];
        dr[(size_t) writePos] = r[i];
        writePos = (writePos + 1) % len;

        float vl = 0.0f, vr = 0.0f;
        if (ttActive)
        {
            ttTime += 1.0 / sr;
            const double k = juce::jmax (0.0, 1.0 - ttTime / length);
            const double speed = backspinMode ? -4.0 * k * k : k * std::sqrt (k);
            if (k > 0.0)
            {
                double rp = std::fmod (ttRead, (double) len);
                if (rp < 0.0) rp += len;
                const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
                const float t = (float) (rp - std::floor (rp));
                vl = dl[(size_t) i0] + t * (dl[(size_t) i1] - dl[(size_t) i0]);
                vr = dr[(size_t) i0] + t * (dr[(size_t) i1] - dr[(size_t) i0]);
                // Fade the last few milliseconds of the stop to avoid a DC click.
                const float tailFade = (float) juce::jmin (1.0, (length - ttTime) / 0.02);
                vl *= tailFade;
                vr *= tailFade;
            }
            ttRead += speed;
        }
        const float w = wetSm.getNextValue();
        l[i] += (vl - l[i]) * w;
        r[i] += (vr - r[i]) * w;
    }
}

} // namespace om
