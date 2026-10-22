#include "BeatFx.h"

namespace om
{

namespace
{
    // BEAT values of each effect, as on the club mixer (owner's manual tables).
    constexpr double bDelay[]   = { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 3.0 / 4, 1, 2, 4, 8, 16 };   // DELAY ECHO PING PONG SPIRAL
    constexpr double bRoll[]    = { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 16 };                         // HELIX TRANS ROLL
    constexpr double bCycle[]   = { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 16, 32, 64 };                 // FLANGER PHASER FILTER
    constexpr double bTriplet[] = { 1.0 / 12, 1.0 / 6, 1.0 / 3, 2.0 / 3, 4.0 / 3, 8.0 / 3, 12.0 / 3 };              // TRIPLET FILTER / ROLL
    constexpr double bMobius[]  = { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 16, 32, 64,
                                    -64, -32, -16, -8, -4, -2, -1, -1.0 / 2, -1.0 / 4, -1.0 / 8, -1.0 / 16 };     // − = falling
    constexpr double bReverb[]  = { 1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };                                    // REVERB %

    struct BeatList { const double* v; int n; int def; };
    template <size_t N> constexpr BeatList list (const double (&a)[N], int def) { return { a, (int) N, def }; }

    BeatList beatList (BeatFx::Type t) noexcept
    {
        using T = BeatFx::Type;
        switch (t)
        {
            case T::delay: case T::echo: case T::pingPong: case T::spiral: return list (bDelay, 7);          // 1
            case T::flanger: case T::phaser: case T::filter:              return list (bCycle, 6);          // 4
            case T::tripletFilter:                                         return list (bTriplet, 4);        // 4/3
            case T::tripletRoll:                                           return list (bTriplet, 2);        // 1/3
            case T::mobius:                                                return list (bMobius, 6);         // 4
            case T::reverb:                                                return list (bReverb, 5);         // 50 %
            default:                                                       return list (bRoll, 4);           // 1
        }
    }

    int stepIndex (float p, int count) noexcept     { return juce::jlimit (0, count - 1, (int) std::floor (p * (float) count)); }
    float stepParam (int index, int count) noexcept { return ((float) index + 0.5f) / (float) count; }
    double listValue (BeatFx::Type t, float p) noexcept { const auto b = beatList (t); return b.v[stepIndex (p, b.n)]; }

    juce::String beatLabel (double b)
    {
        const juce::String sign = b < 0 ? "-" : "";
        b = std::abs (b);
        for (int den : { 1, 2, 3, 4, 6, 8, 12, 16 })
        {
            const int num = juce::roundToInt (b * den);
            if (num > 0 && std::abs ((double) num / den - b) < 1.0e-6)
                return sign + (den == 1 ? juce::String (num) : juce::String (num) + "/" + juce::String (den));
        }
        return sign + juce::String (b, 2);
    }

    float pitchPercent (float p) noexcept           { return (float) juce::roundToInt (-50.0f + 150.0f * juce::jlimit (0.0f, 1.0f, p)); }
    float reverbPercent (float p) noexcept          { return (float) listValue (BeatFx::Type::reverb, p); }

    // Triangle 0 → 1 → 0 over one cycle.
    float tri (double phase) noexcept               { return (float) (1.0 - std::abs (2.0 * phase - 1.0)); }
}

juce::String BeatFx::typeName (Type t)
{
    switch (t)
    {
        case Type::delay:      return "DELAY";
        case Type::echo:       return "ECHO";
        case Type::pingPong:   return "PING PONG";
        case Type::spiral:     return "SPIRAL";
        case Type::reverb:     return "REVERB";
        case Type::trans:      return "TRANS";
        case Type::filter:     return "FILTER";
        case Type::flanger:    return "FLANGER";
        case Type::phaser:     return "PHASER";
        case Type::pitch:      return "PITCH";
        case Type::slipRoll:   return "SLIP ROLL";
        case Type::roll:       return "ROLL";
        case Type::vinylBrake: return "VINYL BRAKE";
        case Type::helix:      return "HELIX";
        case Type::tripletFilter: return "TRIPLET FILTER";
        case Type::tripletRoll:   return "TRIPLET ROLL";
        case Type::mobius:     return "MOBIUS";
        case Type::numTypes:   break;
    }
    return {};
}

juce::String BeatFx::paramName (Type t)
{
    if (t == Type::reverb) return "AMOUNT";
    if (t == Type::pitch)  return "PITCH";
    return "BEAT";
}

double BeatFx::beatsFor (Type t, float p) noexcept
{
    if (t == Type::reverb || t == Type::pitch) return 1.0;
    return std::abs (listValue (t, p));
}

bool BeatFx::falling (Type t, float p) noexcept
{
    return t == Type::mobius && listValue (t, p) < 0.0;
}

int BeatFx::beatSteps (Type t) noexcept      { return t == Type::pitch ? 11 : beatList (t).n; }
float BeatFx::paramAt (Type t, int index) noexcept
{
    const int n = beatSteps (t);
    return stepParam (juce::jlimit (0, n - 1, index), n);
}

float BeatFx::stepBeat (Type t, float p, int dir) noexcept
{
    if (t == Type::pitch) return juce::jlimit (0.0f, 1.0f, p + (float) dir * 5.0f / 150.0f);   // 5 %
    const auto b = beatList (t);
    return stepParam (juce::jlimit (0, b.n - 1, stepIndex (p, b.n) + dir), b.n);
}

juce::String BeatFx::paramText (Type t, float p)
{
    if (t == Type::reverb) return juce::String ((int) reverbPercent (p)) + "%";
    if (t == Type::pitch)
    {
        const int pc = (int) pitchPercent (p);
        return (pc > 0 ? "+" : "") + juce::String (pc) + "%";
    }
    return beatLabel (listValue (t, p));
}

float BeatFx::defaultParam (Type t)
{
    if (t == Type::pitch) return 0.5f;                                                         // +25 %
    const auto b = beatList (t);
    return stepParam (b.def, b.n);
}

float BeatFx::defaultLevel (Type t)
{
    if (t == Type::trans || t == Type::pitch || t == Type::slipRoll || t == Type::roll || t == Type::vinylBrake
        || t == Type::tripletRoll) return 1.0f;
    if (t == Type::filter || t == Type::flanger || t == Type::phaser || t == Type::tripletFilter) return 0.75f;
    if (t == Type::helix) return 0.6f;
    return 0.5f;
}

//==============================================================================
void BeatFx::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate;
    ringL.assign ((size_t) (10.0 * sr), 0.0f);
    ringR.assign (ringL.size(), 0.0f);
    dl.assign ((size_t) (6.0 * sr), 0.0f);
    dr.assign (dl.size(), 0.0f);
    const double diffMs[4] = { 11.3, 7.1, 12.7, 8.3 };
    for (int k = 0; k < 4; ++k)
        diffuser[(size_t) k].assign ((size_t) (diffMs[k] * 0.001 * sr), 0.0f);
    psL.assign ((size_t) (0.15 * sr), 0.0f);
    psR.assign (psL.size(), 0.0f);
    hxL.assign ((size_t) (6.0 * sr), 0.0f);
    hxR.assign (hxL.size(), 0.0f);
    tmpL.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);
    tmpR.assign (tmpL.size(), 0.0f);

    reverb.setSampleRate (sr);
    svf.prepare ({ sr, (juce::uint32) juce::jmax (1, maxBlock), 2 });
    svf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

    levelSm.reset (sr, 0.03);
    inSm.reset (sr, 0.005);
    mixSm.reset (sr, 0.006);
    gateSm.reset (sr, 0.0015);
    levelSm.setCurrentAndTargetValue (wet.load());
    inSm.setCurrentAndTargetValue (0.0f);
    mixSm.setCurrentAndTargetValue (0.0f);
    gateSm.setCurrentAndTargetValue (1.0f);
    xfLen = (int) (0.03 * sr);
    written = 0;
    wasOn = false;
    lastType = -1;
    resetState();
}

void BeatFx::resetState()
{
    std::fill (dl.begin(), dl.end(), 0.0f);
    std::fill (dr.begin(), dr.end(), 0.0f);
    for (auto& d : diffuser) std::fill (d.begin(), d.end(), 0.0f);
    diffPos.fill (0);
    dw = 0;
    curDelay = prevDelay = 0.0;
    xfLeft = 0;
    glideDelay = -1.0;
    reverb.reset();
    revHpL = revHpR = revHpInL = revHpInR = 0.0f;
    svf.reset();
    svfCountdown = 0;
    for (auto& s : ap) s = { 0.0f, 0.0f };
    phFb[0] = phFb[1] = flFb[0] = flFb[1] = 0.0f;
    std::fill (psL.begin(), psL.end(), 0.0f);
    std::fill (psR.begin(), psR.end(), 0.0f);
    psW = 0;
    psPhase = 0.0;
    tail = 0.0f;
}

float BeatFx::ringAt (const std::vector<float>& ring, double absPos) const noexcept
{
    const auto len = (juce::int64) ring.size();
    const auto i = (juce::int64) std::floor (absPos);
    const float t = (float) (absPos - (double) i);
    auto at = [&] (juce::int64 k) { return ring[(size_t) (((k % len) + len) % len)]; };
    const float xm = at (i - 1), x0 = at (i), x1 = at (juce::jmin (i + 1, written - 1)), x2 = at (juce::jmin (i + 2, written - 1));
    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

float BeatFx::delayRead (const std::vector<float>& line, double d) const noexcept
{
    const int len = (int) line.size();
    double rp = (double) dw - d;
    while (rp < 0.0) rp += len;
    const int i = (int) rp;
    const float t = (float) (rp - i);
    const float xm = line[(size_t) ((i - 1 + len) % len)], x0 = line[(size_t) (i % len)];
    const float x1 = line[(size_t) ((i + 1) % len)], x2 = line[(size_t) ((i + 2) % len)];
    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

//==============================================================================
void BeatFx::process (float* l, float* r, int n, double bpm, double beat, double beatsPerSample)
{
    if (n <= 0 || ringL.empty()) return;
    const auto t = (Type) juce::jlimit (0, (int) Type::numTypes - 1, type.load());
    if ((int) t != lastType)
    {
        resetState();
        lastType = (int) t;
    }

    const bool on = enabled.load();
    const bool rising = on && ! wasOn;
    wasOn = on;

    // Musical clock: the deck's grid when there is one, otherwise our own at the deck's BPM.
    const bool grid = beat >= 0.0 && beatsPerSample > 0.0;
    const double tempo = bpm > 1.0 ? bpm : 120.0;
    const double bps = grid ? beatsPerSample : tempo / 60.0 / sr;
    const double b0 = grid ? beat : freeBeat;
    freeBeat = b0 + n * bps;
    const double samplesPerBeat = 1.0 / bps;

    // Input history for the capture effects.
    const auto ringLen = (juce::int64) ringL.size();
    const juce::int64 base = written;
    for (int i = 0; i < n; ++i)
    {
        const auto k = (size_t) ((base + i) % ringLen);
        ringL[k] = l[i];
        ringR[k] = r[i];
    }
    written += n;

    levelSm.setTargetValue (juce::jlimit (0.0f, 1.0f, wet.load()));
    inSm.setTargetValue (on ? 1.0f : 0.0f);
    mixSm.setTargetValue (on ? 1.0f : 0.0f);

    const bool tails = hasTail (t);
    if (! on && ! mixSm.isSmoothing() && mixSm.getCurrentValue() <= 0.0f && ! inSm.isSmoothing()
        && (! tails || tail < 1.0e-5f))
    {
        levelSm.skip (n);
        if (tails && tail > 0.0f) { resetState(); }
        return;
    }

    const float p = param.load();
    // TIME (ms) replaces the BEAT fraction until a BEAT button is pressed again (as on the mixer).
    const double ms = timeMs.load();
    const double beats = ms > 0.0 && t != Type::reverb && t != Type::pitch ? juce::jmax (1.0e-3, ms * 0.001 * sr * bps)
                                                                          : beatsFor (t, p);
    float peak = 0.0f;

    switch (t)
    {
        //----------------------------------------------------------------------
        case Type::delay:
        case Type::echo:
        case Type::pingPong:
        {
            // DELAY: one repeat. ECHO: repeats fading out. PING PONG: repeats alternating L / R.
            // LEVEL/DEPTH = how loud the repeats are; the original stays untouched.
            const double D = juce::jlimit (16.0, (double) dl.size() - 8.0, beats * samplesPerBeat);
            if (curDelay <= 0.0) curDelay = prevDelay = D;
            else if (std::abs (D - curDelay) > 0.5) { prevDelay = curDelay; curDelay = D; xfLeft = xfLen; }
            const float fb = t == Type::delay ? 0.0f : t == Type::echo ? 0.55f : 0.6f;
            const int len = (int) dl.size();
            for (int i = 0; i < n; ++i)
            {
                const float g = inSm.getNextValue(), lv = levelSm.getNextValue();
                float rl = delayRead (dl, curDelay), rr = delayRead (dr, curDelay);
                if (xfLeft > 0)
                {
                    // the delay time changed: crossfade from the old tap, no click, no pitch swoop
                    const float a = 1.0f - (float) xfLeft / (float) xfLen;
                    rl = rl * a + delayRead (dl, prevDelay) * (1.0f - a);
                    rr = rr * a + delayRead (dr, prevDelay) * (1.0f - a);
                    --xfLeft;
                }
                const float inL = l[i] * g, inR = r[i] * g;
                if (t == Type::pingPong)
                {
                    dl[(size_t) dw] = soft (0.5f * (inL + inR) + fb * rr);
                    dr[(size_t) dw] = soft (fb * rl);
                }
                else
                {
                    dl[(size_t) dw] = soft (inL + fb * rl);
                    dr[(size_t) dw] = soft (inR + fb * rr);
                }
                dw = (dw + 1) % len;
                l[i] += rl * lv;
                r[i] += rr * lv;
                peak = juce::jmax (peak, std::abs (rl), std::abs (rr));
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::spiral:
        {
            // Reverberant echo whose delay time glides (changing BEAT bends the pitch);
            // LEVEL/DEPTH sets both the feedback and the amount.
            const double D = juce::jlimit (16.0, (double) dl.size() - 8.0, beats * samplesPerBeat);
            if (glideDelay < 0.0) glideDelay = D;
            const double coef = 1.0 / (0.25 * sr);
            const int len = (int) dl.size();
            auto allpass = [this] (int k, float x)
            {
                auto& buf = diffuser[(size_t) k];
                int& pos = diffPos[(size_t) k];
                const float v = buf[(size_t) pos];
                const float y = -0.6f * x + v;
                buf[(size_t) pos] = x + 0.6f * y;
                pos = (pos + 1) % (int) buf.size();
                return y;
            };
            for (int i = 0; i < n; ++i)
            {
                const float g = inSm.getNextValue(), lv = levelSm.getNextValue();
                glideDelay += (D - glideDelay) * coef;
                // Longer BEATs repeat less often: cap the feedback so the tail lasts the same time (≈ 9 s to −60 dB).
                const float fb = juce::jmin (0.35f + 0.55f * lv, (float) std::pow (0.001, glideDelay / (sr * 9.0)));
                const float rl = allpass (1, allpass (0, delayRead (dl, glideDelay)));
                const float rr = allpass (3, allpass (2, delayRead (dr, glideDelay)));
                dl[(size_t) dw] = soft (l[i] * g + fb * rl);
                dr[(size_t) dw] = soft (r[i] * g + fb * rr);
                dw = (dw + 1) % len;
                l[i] += rl * lv;
                r[i] += rr * lv;
                peak = juce::jmax (peak, std::abs (rl), std::abs (rr));
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::reverb:
        {
            // AMOUNT (1–100 %) = size / length of the room; LEVEL/DEPTH = how much of it is heard.
            const float amt = reverbPercent (p) / 100.0f;
            juce::Reverb::Parameters rp;
            rp.roomSize = 0.35f + 0.63f * amt;
            rp.damping = 0.45f - 0.25f * amt;
            rp.wetLevel = 1.0f;
            rp.dryLevel = 0.0f;
            rp.width = 1.0f;
            reverb.setParameters (rp);
            const float hp = (float) std::exp (-juce::MathConstants<double>::twoPi * 150.0 / sr);   // keep the low end out
            for (int off = 0; off < n; off += (int) tmpL.size())
            {
                const int m = juce::jmin ((int) tmpL.size(), n - off);
                for (int i = 0; i < m; ++i)
                {
                    const float g = inSm.getNextValue();
                    const float xl = l[off + i] * g, xr = r[off + i] * g;
                    revHpL = hp * (revHpL + xl - revHpInL); revHpInL = xl;
                    revHpR = hp * (revHpR + xr - revHpInR); revHpInR = xr;
                    tmpL[(size_t) i] = revHpL * 0.5f;
                    tmpR[(size_t) i] = revHpR * 0.5f;
                }
                reverb.processStereo (tmpL.data(), tmpR.data(), m);
                for (int i = 0; i < m; ++i)
                {
                    const float lv = levelSm.getNextValue();
                    l[off + i] += tmpL[(size_t) i] * lv * 1.4f;
                    r[off + i] += tmpR[(size_t) i] * lv * 1.4f;
                    peak = juce::jmax (peak, std::abs (tmpL[(size_t) i]), std::abs (tmpR[(size_t) i]));
                }
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::trans:
        {
            // The sound is cut on the grid every BEAT; LEVEL/DEPTH sets the duty and how deep the cut is.
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                const double bt = b0 + i * bps;
                const double ph = (bt - std::floor (bt / beats) * beats) / beats;
                const float cutFrac = 0.35f + 0.15f * lv;
                gateSm.setTargetValue (ph >= 1.0 - cutFrac ? 1.0f - lv : 1.0f);
                const float gain = 1.0f + (gateSm.getNextValue() - 1.0f) * m;
                l[i] *= gain;
                r[i] *= gain;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::filter:
        case Type::tripletFilter:
        {
            // Resonant filter whose cut-off rises and falls once per BEAT cycle, locked to the grid.
            // LEVEL/DEPTH: fully left = original sound; further right = deeper sweep, more resonance.
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                if (--svfCountdown <= 0)
                {
                    const double bt = b0 + i * bps;
                    const float c = tri ((bt - std::floor (bt / beats) * beats) / beats);
                    const float floorHz = 18000.0f * std::pow (120.0f / 18000.0f, lv);
                    svf.setCutoffFrequency (juce::jmin (18000.0f, floorHz * std::pow (18000.0f / floorHz, c)));
                    svf.setResonance (0.75f + 2.2f * lv);
                    svfCountdown = 16;
                }
                const float fl = svf.processSample (0, l[i]), fr = svf.processSample (1, r[i]);
                l[i] += (fl - l[i]) * m;
                r[i] += (fr - r[i]) * m;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::flanger:
        {
            // One flanger sweep per BEAT cycle, locked to the grid; LEVEL/DEPTH stresses the effect.
            const int len = (int) dl.size();
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                const double bt = b0 + i * bps;
                const float c = tri ((bt - std::floor (bt / beats) * beats) / beats);
                const double d = (0.25 + 6.0 * c) * 0.001 * sr;
                const float fb = 0.65f * lv;
                const float yl = delayRead (dl, d), yr = delayRead (dr, d);
                dl[(size_t) dw] = soft (l[i] + fb * yl);
                dr[(size_t) dw] = soft (r[i] + fb * yr);
                dw = (dw + 1) % len;
                const float norm = 1.0f / (1.0f + 0.45f * lv);
                const float ol = (l[i] + yl * lv) * norm, orr = (r[i] + yr * lv) * norm;
                l[i] += (ol - l[i]) * m;
                r[i] += (orr - r[i]) * m;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::phaser:
        {
            // Six all-pass stages swept once per BEAT cycle, locked to the grid.
            float a = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                if ((i & 15) == 0)
                {
                    const double bt = b0 + i * bps;
                    const float c = tri ((bt - std::floor (bt / beats) * beats) / beats);
                    const double fc = 220.0 * std::pow (18.0, (double) c);          // 220 Hz … 4 kHz
                    const double tn = std::tan (juce::MathConstants<double>::pi * fc / sr);
                    a = (float) ((tn - 1.0) / (tn + 1.0));
                }
                const float fb = 0.55f * lv;
                float* ch[2] = { l, r };
                for (int c = 0; c < 2; ++c)
                {
                    const float dry = ch[c][i];
                    float x = dry + fb * phFb[c];
                    for (auto& st : ap)
                    {
                        const float y = a * x + st[(size_t) c];
                        st[(size_t) c] = x - a * y;
                        x = y;
                    }
                    phFb[c] = soft (x);
                    const float out = (dry + x * lv) / (1.0f + 0.45f * lv);
                    ch[c][i] = dry + (out - dry) * m;
                }
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::pitch:
        {
            // −50 … +100 % pitch of the source; LEVEL/DEPTH blends from the original to the shifted sound.
            const double ratio = 1.0 + pitchPercent (p) / 100.0;
            const int len = (int) psL.size();
            const double W = 0.05 * sr;
            auto rd = [&] (const std::vector<float>& b, double d)
            {
                double rp = (double) psW - d;
                while (rp < 0.0) rp += len;
                const int k = (int) rp;
                const float fr = (float) (rp - k);
                return b[(size_t) (k % len)] * (1.0f - fr) + b[(size_t) ((k + 1) % len)] * fr;
            };
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                psL[(size_t) psW] = l[i];
                psR[(size_t) psW] = r[i];
                psPhase += (1.0 - ratio) / W;
                psPhase -= std::floor (psPhase);
                const double ph2 = psPhase + 0.5 - std::floor (psPhase + 0.5);
                const float g1 = (float) std::sin (juce::MathConstants<double>::pi * psPhase);
                const float g2 = (float) std::sin (juce::MathConstants<double>::pi * ph2);
                const float yl = rd (psL, psPhase * W + 2.0) * g1 + rd (psL, ph2 * W + 2.0) * g2;
                const float yr = rd (psR, psPhase * W + 2.0) * g1 + rd (psR, ph2 * W + 2.0) * g2;
                psW = (psW + 1) % len;
                const float k = lv * m;
                l[i] += (yl - l[i]) * k;
                r[i] += (yr - r[i]) * k;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::slipRoll:
        case Type::roll:
        case Type::tripletRoll:
        {
            // The sound from the last BEAT boundary on the grid repeats every BEAT — always in time,
            // while the track keeps running underneath (switching off returns to it, like slip).
            // SLIP ROLL re-records when BEAT changes; ROLL keeps its start and changes the length.
            const double lenS = juce::jlimit (64.0, (double) ringLen - 2.0 * n - 64.0, beats * samplesPerBeat);
            auto anchor = [&]
            {
                const double boundary = std::floor (b0 / beats + 1.0e-9) * beats;
                const double back = grid ? (b0 - boundary) * samplesPerBeat : 0.0;
                loopStart = base - (juce::int64) std::llround (juce::jmin (back, lenS - 1.0));
                loopLen = lenS;
                loopBeats = beats;
            };
            if (rising || loopLen <= 0.0) anchor();
            else if (on && std::abs (beats - loopBeats) > 1.0e-9)
            {
                if (t == Type::slipRoll) anchor();
                else { loopLen = lenS; loopBeats = beats; }
            }
            const double xf = juce::jmin (96.0, loopLen * 0.25);
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                const double since = (double) (base + i - loopStart);
                const double rel = since - std::floor (since / loopLen) * loopLen;
                const double src = (double) loopStart + rel;
                float yl = ringAt (ringL, src), yr = ringAt (ringR, src);
                if (rel < xf && since >= loopLen)
                {
                    // wrap: fade from the material that followed the loop end into the loop start
                    const float a = (float) (rel / xf);
                    yl = yl * a + ringAt (ringL, src + loopLen) * (1.0f - a);
                    yr = yr * a + ringAt (ringR, src + loopLen) * (1.0f - a);
                }
                const float dryG = juce::jmin (1.0f, 2.0f * (1.0f - lv)), wetG = juce::jmin (1.0f, 2.0f * lv);
                const float ol = l[i] * dryG + yl * wetG, orr = r[i] * dryG + yr * wetG;
                l[i] += (ol - l[i]) * m;
                r[i] += (orr - r[i]) * m;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::vinylBrake:
        {
            // The input slows down over BEAT until it stops; LEVEL/DEPTH = how far it slows (right = stop).
            const double lenS = juce::jmax (64.0, beats * samplesPerBeat);
            if (rising) { brakeRead = (double) base; brakeElapsed = 0.0; }
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                const double now = (double) (base + i);
                const double prog = juce::jmin (1.0, brakeElapsed / lenS);
                const double speed = 1.0 - lv * prog;
                float yl = 0.0f, yr = 0.0f;
                if (now - brakeRead < (double) ringLen - 2.0 * n)
                {
                    const float amp = (float) juce::jmin (1.0, speed * 4.0);
                    yl = ringAt (ringL, juce::jmin (brakeRead, now)) * amp;
                    yr = ringAt (ringR, juce::jmin (brakeRead, now)) * amp;
                }
                if (on) { brakeRead += speed; brakeElapsed += 1.0; }
                l[i] += (yl - l[i]) * m;
                r[i] += (yr - r[i]) * m;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::helix:
        {
            // Records BEAT of sound from the grid and repeats it; LEVEL/DEPTH = overlay of new input
            // (fully right freezes it). Changing BEAT afterwards changes the pitch of what was recorded.
            const double lenS = juce::jlimit (64.0, (double) hxL.size() - 4.0, beats * samplesPerBeat);
            if (rising)
            {
                hxLen = (int) lenS;
                hxBeats = beats;
                hxRate = 1.0;
                hxFilled = false;
                const double boundary = std::floor (b0 / beats + 1.0e-9) * beats;
                const int back = grid ? juce::jlimit (0, hxLen - 1, (int) std::llround ((b0 - boundary) * samplesPerBeat)) : 0;
                for (int k = 0; k < back; ++k)                       // what was played since the boundary
                {
                    hxL[(size_t) k] = ringAt (ringL, (double) (base - back + k));
                    hxR[(size_t) k] = ringAt (ringR, (double) (base - back + k));
                }
                hxRead = back;
            }
            else if (on && std::abs (beats - hxBeats) > 1.0e-9 && hxLen > 0)
            {
                hxRate = (double) hxLen / lenS;
                hxBeats = beats;
            }
            for (int i = 0; i < n && hxLen > 0; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                const int k = juce::jlimit (0, hxLen - 1, (int) hxRead);
                float yl = l[i], yr = r[i];
                if (hxFilled)
                {
                    yl = hxL[(size_t) k];
                    yr = hxR[(size_t) k];
                    if (on && lv < 0.999f)
                    {
                        hxL[(size_t) k] = yl * lv + l[i] * (1.0f - lv);
                        hxR[(size_t) k] = yr * lv + r[i] * (1.0f - lv);
                    }
                }
                else if (on)
                {
                    hxL[(size_t) k] = l[i];
                    hxR[(size_t) k] = r[i];
                }
                if (on)
                {
                    hxRead += hxRate;
                    if (hxRead >= hxLen) { hxRead -= hxLen; hxFilled = true; }
                }
                const float ol = l[i] * (1.0f - lv) + yl * lv, orr = r[i] * (1.0f - lv) + yr * lv;
                l[i] += (ol - l[i]) * m;
                r[i] += (orr - r[i]) * m;
            }
            break;
        }

        //----------------------------------------------------------------------
        case Type::mobius:
        {
            // An endless riser (or faller, for − beats) in time with the track: soft triangle tones an octave
            // apart glide up one octave per BEAT cycle under a bell-shaped envelope across 110 Hz – 3.5 kHz,
            // so each one fades in at the bottom as another fades out at the top. A gentle low-pass keeps it a
            // smooth "whoosh" over the music; LEVEL/DEPTH = its volume.
            const bool down = falling (t, p);
            constexpr int parts = 5;
            const float lpA = (float) std::exp (-juce::MathConstants<double>::twoPi * 4500.0 / sr);
            for (int i = 0; i < n; ++i)
            {
                const float lv = levelSm.getNextValue(), m = mixSm.getNextValue();
                const double bt = b0 + i * bps;
                double cyc = (bt - std::floor (bt / beats) * beats) / beats;
                if (down) cyc = 1.0 - cyc;
                float osc = 0.0f;
                for (int k = 0; k < parts; ++k)
                {
                    const double pos = ((double) k + cyc) / parts;                         // 0 … 1 across the range
                    const double hz = 110.0 * std::pow (2.0, pos * parts);
                    auto& ph = mbPhase[(size_t) k];
                    ph += hz / sr;
                    ph -= std::floor (ph);
                    const double env = std::pow (std::sin (juce::MathConstants<double>::pi * pos), 2.0);
                    const double w = juce::MathConstants<double>::twoPi * ph;
                    osc += (float) (env * (std::sin (w) - std::sin (3.0 * w) / 9.0));   // soft triangle
                }
                mbLp = osc + lpA * (mbLp - osc);
                const float o = 0.16f * mbLp * lv * m;
                l[i] += o;
                r[i] += o;
            }
            break;
        }

        case Type::numTypes:
            break;
    }

    tail = tails ? peak : 0.0f;
}

} // namespace om
