#include "SelfTest.h"
#include "../Engine/AudioEngine.h"
#include "../Control/Actions.h"
#include "../Control/MidiEngine.h"
#include "../Control/ControllerLibrary.h"
#include "../Control/MappingWizard.h"
#include "../Control/MappingImport.h"
#include "../Control/AutoMix.h"
#include "../Common/Updater.h"
#include <juce_cryptography/juce_cryptography.h>
#include "../Library/Library.h"
#include "../Library/UsbExport.h"
#include "../Library/VolumeCueStore.h"
#include "../Engine/SincResampler.h"
#include "../Engine/MasterLimiter.h"
#include "../Engine/FxUnit.h"
#include "../Engine/BeatFx.h"
#include "../Engine/ChannelStrip.h"
#include "../Common/Strings.h"

namespace om
{

namespace
{
    struct Report
    {
        int passed = 0, failed = 0;
        void check (bool ok, const juce::String& what, const juce::String& detail = {})
        {
            (ok ? passed : failed)++;
            std::cout << (ok ? "  PASS  " : "  FAIL  ") << (what + (detail.isNotEmpty() ? "  (" + detail + ")" : juce::String())).toRawUTF8() << std::endl;
        }
    };

    constexpr double sr = 48000.0;
    constexpr int block = 512;

    /** Renders the deck for `seconds` of output time. */
    void run (Deck& d, double seconds)
    {
        std::vector<float> l (block), r (block);
        const int blocks = (int) std::ceil (seconds * sr / block);
        for (int i = 0; i < blocks; ++i)
            d.render (l.data(), r.data(), block);
    }

    LoadedTrackPtr decode (AudioEngine& engine, const juce::File& f)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (engine.getFormatManager().createReaderFor (f));
        if (reader == nullptr) return nullptr;
        auto t = std::make_shared<LoadedTrack>();
        t->file = f;
        t->sampleRate = reader->sampleRate;
        t->audio.setSize (2, (int) reader->lengthInSamples);
        reader->read (&t->audio, 0, (int) reader->lengthInSamples, 0, true, true);
        t->setGrid (Analysis::detectBeatGrid (t->audio, t->sampleRate));
        return t;
    }
}

namespace
{
    /** Residual (THD+N) of a captured sine relative to its best-fit pure sine, in dB. */
    double sineResidualDb (const std::vector<float>& y, double freq, double fs, int skip)
    {
        // Full least-squares fit of a·sin + b·cos + c (valid for any window length).
        double Sss = 0, Scc = 0, Ssc = 0, Ss = 0, Sc = 0, N = 0, Sys = 0, Syc = 0, Sy = 0;
        for (size_t i = (size_t) skip; i < y.size(); ++i)
        {
            const double ph = juce::MathConstants<double>::twoPi * freq * (double) i / fs;
            const double sn = std::sin (ph), cs = std::cos (ph);
            Sss += sn * sn; Scc += cs * cs; Ssc += sn * cs; Ss += sn; Sc += cs; N += 1;
            Sys += y[i] * sn; Syc += y[i] * cs; Sy += y[i];
        }
        // Solve the 3×3 normal equations (Cramer).
        const double m[3][3] = { { Sss, Ssc, Ss }, { Ssc, Scc, Sc }, { Ss, Sc, N } };
        const double v[3] = { Sys, Syc, Sy };
        auto det3 = [] (const double a3[3][3])
        {
            return a3[0][0] * (a3[1][1] * a3[2][2] - a3[1][2] * a3[2][1]) - a3[0][1] * (a3[1][0] * a3[2][2] - a3[1][2] * a3[2][0])
                 + a3[0][2] * (a3[1][0] * a3[2][1] - a3[1][1] * a3[2][0]);
        };
        const double d = det3 (m);
        double coef[3];
        for (int k = 0; k < 3; ++k)
        {
            double mk[3][3];
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) mk[r][c] = c == k ? v[r] : m[r][c];
            coef[k] = det3 (mk) / d;
        }
        double sig = 0, err = 0;
        for (size_t i = (size_t) skip; i < y.size(); ++i)
        {
            const double ph = juce::MathConstants<double>::twoPi * freq * (double) i / fs;
            const double model = coef[0] * std::sin (ph) + coef[1] * std::cos (ph);
            sig += model * model;
            const double e = y[i] - model - coef[2];
            err += e * e;
        }
        return 10.0 * std::log10 (juce::jmax (1e-30, err) / juce::jmax (1e-30, sig));
    }

    std::shared_ptr<LoadedTrack> sineTrack (double freq, double fs, double seconds, float amp)
    {
        auto t = std::make_shared<LoadedTrack>();
        t->sampleRate = fs;
        const int n = (int) (fs * seconds);
        t->audio.setSize (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / fs);
            t->audio.setSample (0, i, v);
            t->audio.setSample (1, i, v);
        }
        return t;
    }

    std::vector<float> capture (Deck& d, double seconds)
    {
        std::vector<float> out, l (block), r (block);
        const int blocks = (int) (seconds * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            d.render (l.data(), r.data(), block);
            out.insert (out.end(), l.begin(), l.end());
        }
        return out;
    }

    float hermiteRef (const float* d, int n, double p)
    {
        const int i = (int) p;
        const float t = (float) (p - (double) i);
        auto s = [d, n] (int k) { return d[juce::jlimit (0, n - 1, k)]; };
        const float y0 = s (i - 1), y1 = s (i), y2 = s (i + 1), y3 = s (i + 2);
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
}

static void audioQualityTests (Report& rep)
{
    std::cout << "== Calidad de audio" << std::endl;

    // Resampling 44.1 kHz → 48 kHz, 10 kHz tone: sinc vs. the previous Hermite interpolation.
    {
        auto t = sineTrack (10000.0, 44100.0, 4.0, 0.5f);
        Deck d (0);
        d.prepare (sr);
        d.setTrack (t);
        d.setKeyLock (false);
        d.setPlaying (true);
        const auto y = capture (d, 1.0);
        const double sincDb = sineResidualDb (y, 10000.0, sr, 4800);

        std::vector<float> h;
        const float* src = t->audio.getReadPointer (0);
        double pos = 0.0;
        for (size_t i = 0; i < y.size(); ++i, pos += 44100.0 / sr)
            h.push_back (hermiteRef (src, t->audio.getNumSamples(), pos) * 1.0f);
        const double hermDb = sineResidualDb (h, 10000.0, sr, 4800);

        rep.check (sincDb < -85.0, "Remuestreo 44.1→48 kHz, tono 10 kHz: THD+N "_u + juce::String (sincDb, 1) + " dB",
                   "antes (Hermite): " + juce::String (hermDb, 1) + " dB");
    }

    // Pitch +8 % on a 1 kHz tone.
    {
        auto t = sineTrack (1000.0, 48000.0, 4.0, 0.5f);
        Deck d (0);
        d.prepare (sr);
        d.setTrack (t);
        d.setKeyLock (false);                 // this measures the resampler (pitch follows speed)
        d.setPitch (0.08);
        d.setPlaying (true);
        const auto y = capture (d, 1.0);
        const double db = sineResidualDb (y, 1080.0, sr, 4800);
        rep.check (db < -90.0, "Pitch +8 %, tono 1 kHz: THD+N "_u + juce::String (db, 1) + " dB");

        Deck d2 (0);
        d2.prepare (sr);
        d2.setTrack (t);
        d2.setKeyLock (false);
        d2.setPitchRange (0.5);
        d2.setPitch (0.3);
        d2.setPlaying (true);
        const double db30 = sineResidualDb (capture (d2, 1.0), 1300.0, sr, 4800);
        rep.check (db30 < -90.0, "Pitch +30 %, tono 1 kHz: THD+N "_u + juce::String (db30, 1) + " dB");
    }

    // Clicks: loops whose length is not a whole number of periods, hot-cue jumps while playing.
    {
        auto clickScore = [] (const std::vector<float>& y)
        {
            // Largest second difference relative to what a clean 1 kHz tone produces.
            float worst = 0.0f;
            for (size_t i = 4800; i + 1 < y.size(); ++i)
                worst = juce::jmax (worst, std::abs (y[i + 1] - 2.0f * y[i] + y[i - 1]));
            const float clean = 0.5f * (float) std::pow (juce::MathConstants<double>::twoPi * 1000.0 / sr, 2.0);
            return worst / clean;   // 1.0 = as smooth as the tone itself
        };

        auto t = sineTrack (1000.0, 48000.0, 10.0, 0.5f);
        t->setGrid (BeatGrid { 123.0, 0.0 });      // loop lengths deliberately not period-aligned
        for (bool kl : { false, true })
        {
            const juce::String tag = kl ? " con key lock" : " sin key lock";
            Deck d (0);
            d.prepare (sr);
            d.setTrack (t);
            d.setKeyLock (kl);
            d.setPlaying (true);
            run (d, 0.3);
            const double base = clickScore (capture (d, 0.5));     // the path's own smoothness (stretcher adds a little)
            d.setAutoLoop (0.25);
            const auto loopCap = capture (d, 1.0);
            const double loopScore = clickScore (loopCap);
            d.exitLoop();
            rep.check (loopScore < juce::jmax (3.0, base * 1.5), "Loop sin clics al volver al inicio"_u + tag,
                       "salto x" + juce::String (loopScore, 1) + " (base x" + juce::String (base, 1) + ")");

            std::vector<float> y, l (block), r (block);
            for (int b = 0; b < 200; ++b)
            {
                if (b % 20 == 10) d.seek (1.0 + b * 0.0137);        // jump while playing
                d.render (l.data(), r.data(), block);
                y.insert (y.end(), l.begin(), l.end());
            }
            const double jumpScore = clickScore (y);
            rep.check (jumpScore < juce::jmax (3.0, base * 1.5), "Saltos de hot cue sin clics"_u + tag,
                       "salto x" + juce::String (jumpScore, 1) + " (base x" + juce::String (base, 1) + ")");
        }
    }

    // Every effect: stable output (no NaN / runaway) on a full-scale tone, on and off.
    {
        bool allOk = true;
        juce::String bad;
        for (int t = 0; t < (int) FxUnit::Type::numTypes; ++t)
        {
            const auto type = (FxUnit::Type) t;
            if (type == FxUnit::Type::plugin) continue;
            FxUnit fx;
            fx.prepare (sr, block);
            fx.type = t;
            fx.wet = 1.0f;
            fx.param = 0.7f;
            std::vector<float> l (block), r (block);
            float peak = 0.0f;
            bool finite = true;
            double ph = 0.0;
            for (int b = 0; b < 200; ++b)
            {
                fx.enabled = b < 150;
                for (int i = 0; i < block; ++i, ph += 220.0 / sr) l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * ph);
                fx.process (l.data(), r.data(), block, 124.0);
                for (int i = 0; i < block; ++i)
                {
                    finite = finite && std::isfinite (l[(size_t) i]) && std::isfinite (r[(size_t) i]);
                    peak = juce::jmax (peak, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]));
                }
            }
            if (! finite || peak > 4.0f) { allOk = false; bad << FxUnit::typeName (type) << " "; }
        }
        rep.check (allOk, "Motor interno (Color FX / RMX): procesa estable (sin NaN ni desbordes)", bad);
    }

    // Functional checks on a few effects.
    {
        auto goertzel = [] (const std::vector<float>& y, double f, size_t from)
        {
            const double w = juce::MathConstants<double>::twoPi * f / sr, c = 2.0 * std::cos (w);
            double s1 = 0, s2 = 0;
            for (size_t i = from; i < y.size(); ++i) { const double s0 = y[i] + c * s1 - s2; s2 = s1; s1 = s0; }
            return s1 * s1 + s2 * s2 - c * s1 * s2;
        };
        auto runFx = [] (FxUnit& fx, double seconds, float inAmp, std::function<void (int)> perBlock = {})
        {
            std::vector<float> out, l (block), r (block);
            double ph = 0.0;
            for (int b = 0; b < (int) (seconds * sr / block); ++b)
            {
                if (perBlock) perBlock (b);
                for (int i = 0; i < block; ++i, ph += 440.0 / sr)
                    l[(size_t) i] = r[(size_t) i] = inAmp * (float) std::sin (juce::MathConstants<double>::twoPi * ph);
                fx.process (l.data(), r.data(), block, 124.0);
                out.insert (out.end(), l.begin(), l.end());
            }
            return out;
        };
        {
            FxUnit fx; fx.prepare (sr, block);
            fx.type = (int) FxUnit::Type::pitch; fx.param = 1.0f; fx.wet = 1.0f; fx.enabled = true;   // +12 st
            const auto y = runFx (fx, 1.0, 0.5f);
            const double ratio = goertzel (y, 880.0, 9600) / juce::jmax (1e-12, goertzel (y, 440.0, 9600));
            rep.check (ratio > 20.0, "Motor interno: pitch +12 sube una octava (440 → 880 Hz)"_u, "relación de energía "_u + juce::String (ratio, 0) + "×"_u);
        }
        for (bool mute : { true, false })
        {
            FxUnit fx; fx.prepare (sr, block);
            fx.type = (int) (mute ? FxUnit::Type::releaseMute : FxUnit::Type::releaseDry);
            fx.wet = 1.0f; fx.enabled = true;
            // 440 Hz is captured into the echo; then the track switches to 880 Hz, so the
            // track and the echo tail can be told apart.
            const double beat = 60.0 / 124.0;
            std::vector<float> y, l (block), r (block);
            double ph = 0.0;
            for (int b = 0; b < (int) (5.0 * sr / block); ++b)
            {
                for (int i = 0; i < block; ++i)
                {
                    const double tSec = ((double) b * block + i) / sr;
                    ph += (tSec < 0.75 * beat ? 440.0 : 880.0) / sr;
                    l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * ph);
                }
                fx.process (l.data(), r.data(), block, 124.0);
                y.insert (y.end(), l.begin(), l.end());
            }
            std::vector<float> mid (y.begin() + (long) (1.5 * beat * sr), y.begin() + (long) (3.0 * beat * sr));
            std::vector<float> end (y.end() - (long) (0.5 * sr), y.end());
            const double trackDuring = goertzel (mid, 880.0, 0) / juce::jmax (1e-12, goertzel (end, 880.0, 0) * (double) mid.size() / (double) end.size());
            if (mute) rep.check (trackDuring < 0.01 && ! fx.enabled.load(), "Motor interno (RMX ECHO OUT): silencia el tema 4 beats y vuelve solo",
                                 "tema durante el mute: " + juce::String (trackDuring * 100.0, 2) + " %");
            else      rep.check (trackDuring > 0.5 && ! fx.enabled.load(), "Motor interno: release echo DRY deja sonar el tema y vuelve solo"_u,
                                 juce::String (trackDuring * 100.0, 0) + " % del tema");
        }
    }

    // Levels, club mixer style: additive effects (delay/echo/reverb family) never lower the dry track and
    // are clearly audible at full LEVEL/DEPTH, nothing pushes far above the track (≤ +6 dB), and every
    // tail dies away once the input stops.
    {
        juce::String loud, quiet, runaway;
        for (int t = 0; t < (int) FxUnit::Type::numTypes; ++t)
        {
            const auto type = (FxUnit::Type) t;
            if (type == FxUnit::Type::plugin) continue;
            FxUnit fx;
            fx.prepare (sr, block);
            fx.type = t; fx.wet = 1.0f; fx.param = 0.5f; fx.enabled = true;
            juce::Random rnd (42);
            std::vector<float> l (block), r (block);
            double inSq = 0, outSq = 0, diffSq = 0, ph1 = 0, ph2 = 0;
            std::vector<float> dry (block);
            float inPeak = 0, outPeak = 0, tailRms = 0;
            const int total = (int) (8.0 * sr / block), stopAt = (int) (5.0 * sr / block);
            for (int b = 0; b < total; ++b)
            {
                if (b == stopAt && type != FxUnit::Type::releaseDry && type != FxUnit::Type::releaseMute) fx.enabled = false;
                double tail = 0;
                for (int i = 0; i < block; ++i, ph1 += 110.0 / sr, ph2 += 1760.0 / sr)
                {
                    const float x = b < stopAt ? (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * ph1)
                                                          + 0.12 * std::sin (juce::MathConstants<double>::twoPi * ph2)
                                                          + 0.05 * (rnd.nextFloat() * 2.0f - 1.0f)) : 0.0f;
                    l[(size_t) i] = r[(size_t) i] = dry[(size_t) i] = x;
                    if (b < stopAt) { inSq += (double) x * x; inPeak = juce::jmax (inPeak, std::abs (x)); }
                }
                fx.process (l.data(), r.data(), block, 124.0);
                for (int i = 0; i < block; ++i)
                {
                    if (b < stopAt)
                    {
                        outSq += (double) l[(size_t) i] * l[(size_t) i]; outPeak = juce::jmax (outPeak, std::abs (l[(size_t) i]));
                        const double d = (double) l[(size_t) i] - dry[(size_t) i];
                        if (b > stopAt / 2) diffSq += d * d;
                    }
                    else tail += (double) l[(size_t) i] * l[(size_t) i];
                }
                if (b == total - 1) tailRms = (float) std::sqrt (tail / block);
            }
            const double gainDb = 10.0 * std::log10 (juce::jmax (1e-12, outSq) / juce::jmax (1e-12, inSq));
            using T = FxUnit::Type;
            const bool additive = type == T::delay || type == T::echo || type == T::pingPong || type == T::spiral
                               || type == T::reverb || type == T::tapeEcho
                               || type == T::shimmer || type == T::echoVerb;
            if (gainDb > 6.0 || outPeak > inPeak * 2.5f)
                loud << FxUnit::typeName (type) << " (" << juce::String (gainDb, 1) << " dB) ";
            if (additive)
            {
                const double effDb = 10.0 * std::log10 (juce::jmax (1e-12, diffSq) / juce::jmax (1e-12, inSq * 0.5));
                if (gainDb < -0.5 || effDb < -10.0)
                    quiet << FxUnit::typeName (type) << " (" << juce::String (gainDb, 1) << " / " << juce::String (effDb, 1) << " dB) ";
            }
            if (tailRms > 0.01f)
                runaway << FxUnit::typeName (type) << " ";
        }
        rep.check (loud.isEmpty(), "Motor interno: nada sube más de 6 dB"_u, loud);
        rep.check (quiet.isEmpty(), "Motor interno: delay/echo/reverb no bajan el track"_u, quiet);
        rep.check (runaway.isEmpty(), "Motor interno: las colas se apagan solas"_u, runaway);
    }

    // Vinyl brake as an audio effect: sound stops while on, comes back when off — the deck never stops.
    {
        FxUnit fx;
        fx.prepare (sr, block);
        fx.type = (int) FxUnit::Type::brake; fx.wet = 1.0f; fx.param = 0.0f;   // 0.3 s
        std::vector<float> l (block), r (block);
        double ph = 0;
        auto blockRms = [&] (bool on)
        {
            fx.enabled = on;
            double a = 0;
            for (int i = 0; i < block; ++i, ph += 440.0 / sr) l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * ph);
            fx.process (l.data(), r.data(), block, 124.0);
            for (int i = 0; i < block; ++i) a += (double) l[(size_t) i] * l[(size_t) i];
            return std::sqrt (a / block);
        };
        for (int b = 0; b < 40; ++b) blockRms (false);
        double stopped = 0, back = 0;
        for (int b = 0; b < 60; ++b) stopped = blockRms (true);      // 0.64 s on (> 0.3 s brake)
        for (int b = 0; b < 20; ++b) back = blockRms (false);
        rep.check (stopped < 0.01 && back > 0.3, "Motor interno (RMX BRAKE): frena el sonido y vuelve el tema",
                   "en freno " + juce::String (stopped, 3) + ", después "_u + juce::String (back, 3));
    }

    // Sound Color FX: neutral at centre (bit-identical to plain FILTER), active and stable when turned.
    {
        auto renderStrip = [] (int color, float knob)
        {
            ChannelStrip ch;
            ch.prepare (sr, block);
            ch.colorType = color;
            ch.filter = knob;
            std::vector<float> out, l (block), r (block);
            double ph = 0.0;
            for (int b = 0; b < 100; ++b)
            {
                for (int i = 0; i < block; ++i, ph += 330.0 / sr)
                    l[(size_t) i] = r[(size_t) i] = 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * ph);
                ch.processPreFader (l.data(), r.data(), block, 124.0);
                out.insert (out.end(), l.begin(), l.end());
            }
            return out;
        };
        const auto ref = renderStrip (0, 0.0f);
        bool neutral = true, active = true, stable = true;
        juce::String bad;
        for (int c = 1; c < (int) ChannelStrip::Color::numColors; ++c)
        {
            const auto centre = renderStrip (c, 0.0f);
            neutral = neutral && std::memcmp (centre.data(), ref.data(), ref.size() * sizeof (float)) == 0;
            for (float k : { -0.8f, 0.8f })
            {
                const auto y = renderStrip (c, k);
                double diff = 0.0;
                for (size_t i = 0; i < y.size(); ++i)
                {
                    stable = stable && std::isfinite (y[i]) && std::abs (y[i]) < 4.0f;
                    diff += std::abs (y[i] - ref[i]);
                }
                if (diff / (double) y.size() < 0.005) { active = false; bad << ChannelStrip::colorName ((ChannelStrip::Color) c) << (k < 0 ? "-" : "+") << " "; }
            }
        }
        rep.check (neutral, "Sound Color FX neutros con la perilla al centro");
        rep.check (active && stable, "Sound Color FX actúan a ambos lados y son estables"_u, bad);
    }

    // Limiter: transparent below the ceiling, never above it (true peak).
    {
        MasterLimiter lim;
        lim.prepare (sr, block);
        lim.ceilingDb = -0.5f;
        const int n = (int) sr;
        std::vector<float> l ((size_t) n), r ((size_t) n), inL ((size_t) n);
        for (int i = 0; i < n; ++i)
            inL[(size_t) i] = l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sr);
        for (int i = 0; i < n; i += block) lim.process (l.data() + i, r.data() + i, juce::jmin (block, n - i));
        float maxDiff = 0.0f;
        const int lat = lim.latencySamples();
        for (int i = lat; i < n; ++i) maxDiff = juce::jmax (maxDiff, std::abs (l[(size_t) i] - inL[(size_t) (i - lat)]));
        rep.check (maxDiff == 0.0f, "Limitador transparente bajo el techo (bit a bit)",
                   "latencia " + juce::String (lat * 1000.0 / sr, 2) + " ms");
    }
    {
        MasterLimiter lim;
        lim.prepare (sr, block);
        lim.ceilingDb = -0.5f;
        const int n = (int) sr * 2;
        std::vector<float> l ((size_t) n), r ((size_t) n);
        // fs/4 tone at 45° phase: sample peaks are 0.707 of the true (inter-sample) peak.
        for (int i = 0; i < n; ++i)
        {
            const double burst = (i / 4800) % 2 == 0 ? 2.0 : 0.3;   // +6 dB bursts
            l[(size_t) i] = (float) (burst * std::sin (juce::MathConstants<double>::halfPi * i + juce::MathConstants<double>::pi / 4)
                                     + 0.2 * std::sin (juce::MathConstants<double>::twoPi * 60.0 * i / sr));
            r[(size_t) i] = l[(size_t) i];
        }
        for (int i = 0; i < n; i += block) lim.process (l.data() + i, r.data() + i, juce::jmin (block, n - i));

        // Measure the true peak of the output with 8× band-limited interpolation.
        const auto& rs = SincResampler::get();
        float tp = 0.0f;
        for (double p = 100; p < n - 100; p += 0.125)
            tp = juce::jmax (tp, std::abs (rs.interpolate (l.data(), n, p, 1.0)));
        const double tpDb = juce::Decibels::gainToDecibels (tp);
        rep.check (tpDb <= -0.4, "Limitador: true peak de salida "_u + juce::String (tpDb, 2) + " dBTP con entrada +6 dB (techo -0.5)");
    }

    {
        // An isolated hard transient (a kick 12 dB over the ceiling) must not duck the music after it.
        MasterLimiter lim;
        lim.prepare (sr, block);
        lim.ceilingDb = -0.5f;
        const int n = (int) sr;
        std::vector<float> l ((size_t) n), r ((size_t) n);
        const int hit = n / 2, hitLen = (int) (0.003 * sr);
        for (int i = 0; i < n; ++i)
        {
            l[(size_t) i] = 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / sr);
            if (i >= hit && i < hit + hitLen) l[(size_t) i] = (i - hit) % 2 == 0 ? 3.5f : -3.5f;
            r[(size_t) i] = l[(size_t) i];
        }
        for (int i = 0; i < n; i += block) lim.process (l.data() + i, r.data() + i, juce::jmin (block, n - i));
        auto levelDbAt = [&] (double seconds)
        {
            const int from = hit + lim.latencySamples() + (int) (seconds * sr);
            float pk = 0.0f;
            for (int i = from; i < from + (int) (sr / 220.0) + 2 && i < n; ++i) pk = juce::jmax (pk, std::abs (l[(size_t) i]));
            return juce::Decibels::gainToDecibels (pk / 0.4f);
        };
        const float after = levelDbAt (0.25);
        rep.check (after > -0.5f, "Limitador: tras un golpe aislado la música vuelve enseguida (sin bajones)",
                   juce::String (levelDbAt (0.1), 1) + " dB a 100 ms · " + juce::String (after, 1) + " dB a 250 ms");
    }

    // Loudness: EBU Tech 3341 — stereo 1 kHz sine at −23 dBFS reads −23.0 LUFS.
    {
        const float amp = juce::Decibels::decibelsToGain (-23.0f);
        auto t = sineTrack (1000.0, 48000.0, 20.0, amp);
        const double lufs = Analysis::integratedLoudness (t->audio, 48000.0);
        rep.check (std::abs (lufs - (-23.0)) < 0.1, "Medición de sonoridad (EBU 3341): "_u + juce::String (lufs, 2) + " LUFS");
    }
}

int runAudioDeviceTest (const juce::File& usbRoot, const juce::File& scratch)
{
    auto print = [] (const juce::String& s) { std::cout << s.toRawUTF8() << std::endl; };
    AudioEngine engine;
    engine.masterDb = -24.0f;
    engine.autoGain = false;
    engine.initialiseDevice (nullptr);
    juce::Thread::sleep (800);

    auto* dev = engine.getDeviceManager().getCurrentAudioDevice();
    if (dev == nullptr) { print ("Sin dispositivo de audio: " + engine.getStatusDetail()); return 1; }
    print ("Dispositivo: " + dev->getName() + " · "_u + juce::String (dev->getCurrentSampleRate(), 0) + " Hz · buffer "_u
           + juce::String (dev->getCurrentBufferSizeSamples()) + " · latencia "_u + juce::String (engine.getOutputLatencyMs(), 1) + " ms");

    double peakCpu = 0.0;
    const int xrunStart = juce::jmax (0, engine.getXRunCount());
    auto watch = [&] (int ms)
    {
        for (int t = 0; t < ms; t += 50)
        {
            juce::Thread::sleep (50);
            peakCpu = juce::jmax (peakCpu, engine.getCpuUsage());
        }
    };

    // Phase A: test tone through the real device with loops and hot-cue jumps; recorded for click analysis.
    auto tone = sineTrack (1000.0, 44100.0, 30.0, 0.5f);
    tone->setGrid (BeatGrid { 123.0, 0.0 });
    auto& d1 = engine.deck (0);
    d1.setTrack (tone);
    engine.channel (0).fader = 1.0f;
    engine.crossfader = 0.0f;

    const auto recFile = scratch.getChildFile ("audio-test.wav");
    recFile.deleteFile();
    engine.recorder().start (recFile, dev->getCurrentSampleRate());
    d1.setPlaying (true);
    watch (1000);
    d1.setAutoLoop (0.25);   watch (1500);
    d1.exitLoop();           watch (300);
    for (int i = 0; i < 6; ++i) { d1.seek (2.0 + i * 0.731); watch (250); }
    d1.setPlaying (false);
    watch (300);
    engine.recorder().stop();
    d1.setTrack (nullptr);

    // Analyse the recording: clicks (2nd-difference spikes) and gaps (unexpected silence).
    {
        std::unique_ptr<juce::AudioFormatReader> rd (engine.getFormatManager().createReaderFor (recFile));
        if (rd != nullptr)
        {
            juce::AudioBuffer<float> buf (1, (int) rd->lengthInSamples);
            rd->read (&buf, 0, buf.getNumSamples(), 0, true, false);
            const float* y = buf.getReadPointer (0);
            const int n = buf.getNumSamples();
            const double w = juce::MathConstants<double>::twoPi * 1000.0 / rd->sampleRate;
            float amp = 0.0f;
            for (int i = 0; i < n; ++i) amp = juce::jmax (amp, std::abs (y[i]));
            const float clean = amp * (float) (w * w);
            float worst = 0.0f;
            int gaps = 0, run = 0;
            const int start = (int) (rd->sampleRate * 0.1), stop = n - (int) (rd->sampleRate * 0.5);
            for (int i = start; i < stop; ++i)
            {
                worst = juce::jmax (worst, std::abs (y[i + 1] - 2.0f * y[i] + y[i - 1]));
                run = std::abs (y[i]) < amp * 0.01f ? run + 1 : 0;
                if (run == (int) (rd->sampleRate * 0.002)) ++gaps;   // ≥ 2 ms of silence mid-tone
            }
            print ("Grabación de salida: "_u + juce::String (n / rd->sampleRate, 1) + " s · peor salto x"_u
                   + juce::String (clean > 0 ? worst / clean : 0.0f, 1) + " (un clic real da ~x100) · huecos de silencio: "_u + juce::String (gaps));
        }
    }

    // Phase B: two real tracks with key lock + pitch + key shift (worst-case DSP load).
    juce::String err;
    auto ex = usbRoot.isDirectory() ? UsbExport::load (usbRoot, err) : nullptr;
    int loaded = 0;
    for (size_t i = 0; ex != nullptr && i < ex->tracks.size() && loaded < 2; i += 997)
    {
        const auto f = ex->fileFor (ex->tracks[i]);
        if (! f.existsAsFile()) continue;
        std::unique_ptr<juce::AudioFormatReader> reader (engine.getFormatManager().createReaderFor (f));
        if (reader == nullptr) continue;
        auto lt = std::make_shared<LoadedTrack>();
        lt->sampleRate = reader->sampleRate;
        const int len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 60));
        lt->audio.setSize (2, len);
        reader->read (&lt->audio, 0, len, (juce::int64) (reader->sampleRate * 30) < reader->lengthInSamples - len ? (juce::int64) (reader->sampleRate * 30) : 0, true, reader->numChannels > 1);
        auto& d = engine.deck (loaded);
        d.setTrack (lt);
        d.setKeyLock (true);
        d.setPitchRange (0.16);
        d.setPitch (loaded == 0 ? 0.06 : -0.04);
        d.setKeyShift (loaded == 0 ? 0 : 1);
        engine.channel (loaded).fader = 0.8f;
        d.setPlaying (true);
        print ("Deck " + juce::String (loaded + 1) + ": " + f.getFileName() + "  (key lock, pitch "
               + juce::String (d.getPitch() * 100.0, 1) + " %, tono " + juce::String (d.getKeyShift()) + ")");
        ++loaded;
    }
    if (loaded > 0)
    {
        watch (7000);
        const double p1 = engine.deck (0).getPosition();
        watch (1000);
        const double advanced = engine.deck (0).getPosition() - p1;
        print ("Avance del deck 1 en 1 s de reloj: " + juce::String (advanced, 3) + " s de tema (esperado 1.060 con +6 %)");
    }

    const int xruns = juce::jmax (0, engine.getXRunCount()) - xrunStart;
    print ("Cortes (xruns): " + juce::String (xruns) + " · carga DSP pico: "_u + juce::String (peakCpu * 100.0, 1) + " %");
    for (int d = 0; d < 2; ++d) engine.deck (d).setPlaying (false);
    juce::Thread::sleep (200);
    return xruns == 0 ? 0 : 1;
}

int runAnalyzerBenchmark (const juce::File& usbRoot, int count, const juce::File& reportFile)
{
    auto print = [] (const juce::String& s) { std::cout << s.toRawUTF8() << std::endl; };
    juce::String err;
    auto ex = UsbExport::load (usbRoot, err);
    if (ex == nullptr) { print (err); return 1; }

    // Spread the sample over the whole collection (deterministic).
    std::vector<size_t> picks;
    const size_t stride = std::max<size_t> (1, ex->tracks.size() / (size_t) count);
    for (size_t i = 0; i < ex->tracks.size() && (int) picks.size() < count; i += stride)
        picks.push_back (i);

    struct Row { juce::String name, genre; double rbBpm, omBpm, rbFirst, omFirst, rbBar; bool variable; double secs; juce::String fmt; juce::String path; int lead = 0; };
    std::vector<Row> rows (picks.size());
    std::atomic<int> done { 0 };
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    {
        juce::ThreadPool pool (juce::ThreadPoolOptions{}.withNumberOfThreads (8));
        for (size_t k = 0; k < picks.size(); ++k)
            pool.addJob ([&, k]
            {
                const auto& t = ex->tracks[picks[k]];
                Row r { t.artist + " - " + t.title, t.genre, 0, 0, 0, 0, 0, false, 0, {}, ex->fileFor (t).getFullPathName() };
                {
                    // Format + gapless header (read-only peek at the first 8 KB).
                    const auto file = ex->fileFor (t);
                    r.fmt = file.getFileExtension().toLowerCase();
                    juce::FileInputStream in (file);
                    if (in.openedOk())
                    {
                        juce::MemoryBlock head;
                        in.readIntoMemoryBlock (head, 8192);
                        const auto txt = juce::String::fromUTF8 ((const char*) head.getData(), (int) head.getSize());
                        const char* hd = (const char*) head.getData();
                        auto has = [&] (const char* tag) { for (size_t i = 0; i + 4 < head.getSize(); ++i) if (std::memcmp (hd + i, tag, 4) == 0) return true; return false; };
                        r.fmt << (has ("LAME") ? "+lame" : "") << (has ("Xing") || has ("Info") ? "+xing" : "");
                        juce::ignoreUnused (txt);
                    }
                }
                const auto an = UsbExport::readAnalysis (*ex, t);
                r.rbBpm = an.grid.bpm;
                r.rbFirst = an.grid.firstBeatSeconds;
                r.rbBar = an.grid.firstBeatSeconds;
                r.variable = an.tempoVariation > 0.5;
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (ex->fileFor (t)));
                if (rd != nullptr && r.rbBpm > 0)
                {
                    const int lead = Analysis::mp3TrimmedLeadSamples (ex->fileFor (t));
                    r.lead = lead;
                    juce::AudioBuffer<float> buf ((int) std::min (2u, rd->numChannels), (int) rd->lengthInSamples + lead);
                    buf.clear();
                    rd->read (&buf, lead, (int) rd->lengthInSamples, 0, true, buf.getNumChannels() > 1);
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    const auto g = Analysis::detectBeatGrid (buf, rd->sampleRate);
                    r.secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
                    r.omBpm = g.bpm;
                    r.omFirst = g.firstBeatSeconds;
                }
                rows[k] = r;
                ++done;
            });
        while (done.load() < (int) picks.size()) juce::Thread::sleep (200);
    }

    int n = 0, exact = 0, octave = 0, wrong = 0, none = 0, phaseOk = 0, barOk = 0, varN = 0;
    double totalSecs = 0;
    juce::String report = "rb_bpm\tom_bpm\tphase_ms\tbar_ok\tvariable\tfmt\tgenre\ttrack\tbar_off\n";
    for (auto& r : rows)
    {
        if (r.rbBpm <= 0) continue;
        ++n; totalSecs += r.secs;
        if (r.variable) ++varN;
        juce::String cls;
        double phaseMs = -1, barOff = -1;
        bool bar = false;
        if (r.omBpm <= 0) { ++none; cls = "none"; }
        else if (std::abs (r.omBpm - r.rbBpm) <= 0.1)
        {
            ++exact;
            const double beat = 60.0 / r.rbBpm;
            double d = std::fmod (r.omFirst - r.rbFirst, beat);
            if (d > beat / 2) d -= beat;
            if (d < -beat / 2) d += beat;
            phaseMs = d * 1000.0;                       // signed: + means OpenMix is late
            if (std::abs (phaseMs) <= 20.0) ++phaseOk;
            double bd = std::fmod (std::abs (r.omFirst - r.rbBar), 4 * beat);
            bd = juce::jmin (bd, 4 * beat - bd);
            bar = bd <= 0.02;
            if (bar) ++barOk;
            barOff = std::fmod (std::round ((r.omFirst - r.rbBar) / beat) + 400.0, 4.0);
        }
        else if (std::abs (r.omBpm * 2 - r.rbBpm) <= 0.2 || std::abs (r.omBpm / 2 - r.rbBpm) <= 0.1
                 || std::abs (r.omBpm * 1.5 - r.rbBpm) <= 0.2 || std::abs (r.omBpm / 1.5 - r.rbBpm) <= 0.2) ++octave;
        else ++wrong;
        report << juce::String (r.rbBpm, 2) << "\t" << juce::String (r.omBpm, 2) << "\t" << juce::String (phaseMs, 1) << "\t"
               << (bar ? "1" : "0") << "\t" << (r.variable ? "1" : "0") << "\t" << r.fmt << "\t" << r.genre << "\t" << r.name << "\t" << juce::String (barOff, 0) << "\t" << r.path << "\t" << juce::String (r.rbFirst, 4) << "\t" << r.lead << "\n";
    }
    reportFile.replaceWithText (report);

    auto pct = [n] (int v) { return juce::String (100.0 * v / juce::jmax (1, n), 1) + " %"; };
    print ("Temas comparados: " + juce::String (n) + " (tempo variable según el pendrive: "_u + juce::String (varN) + ")");
    print ("BPM igual al del pendrive (±0.1): "_u + pct (exact));
    print ("Error de octava (×2, ÷2, ×1.5): "_u + pct (octave));
    print ("BPM distinto: " + pct (wrong) + " · sin BPM: "_u + pct (none));
    print ("Fase de la grilla a ≤20 ms (de los BPM iguales): "_u + juce::String (100.0 * phaseOk / juce::jmax (1, exact), 1) + " %");
    print ("Compás (beat 1) igual (de los BPM iguales): "_u + juce::String (100.0 * barOk / juce::jmax (1, exact), 1) + " %");
    print ("Tiempo medio de análisis: "_u + juce::String (totalSecs / juce::jmax (1, n), 2) + " s por tema");
    return 0;
}

int gridDebug (const juce::File& usbRoot, const juce::String& name)
{
    juce::String err;
    auto ex = UsbExport::load (usbRoot, err);
    if (ex == nullptr) { std::cout << err << std::endl; return 1; }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    for (auto& t : ex->tracks)
    {
        const auto file = ex->fileFor (t);
        if (! file.getFileName().containsIgnoreCase (name)) continue;
        const auto an = UsbExport::readAnalysis (*ex, t);
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
        if (rd == nullptr || ! an.grid.isValid()) continue;
        const int lead = Analysis::mp3TrimmedLeadSamples (file);
        juce::AudioBuffer<float> buf ((int) std::min (2u, rd->numChannels), (int) rd->lengthInSamples + lead);
        buf.clear();
        rd->read (&buf, lead, (int) rd->lengthInSamples, 0, true, buf.getNumChannels() > 1);
        std::cout << file.getFileName() << "  |  rekordbox " << an.grid.bpm << " BPM @ " << an.grid.firstBeatSeconds << " s" << std::endl
                  << Analysis::debugGrid (buf, rd->sampleRate, an.grid) << std::endl;
        return 0;
    }
    std::cout << "no encontrado" << std::endl;
    return 1;
}

int inspectUsbExport (const juce::File& root)
{
    auto print = [] (const juce::String& s) { std::cout << s.toRawUTF8() << std::endl; };

    auto fingerprint = [&root]
    {
        juce::StringArray fp;
        for (const auto& e : juce::RangedDirectoryIterator (root.getChildFile (UsbExport::exportFolder), true, "*", juce::File::findFilesAndDirectories))
            fp.add (e.getFile().getRelativePathFrom (root) + "|" + juce::String (e.getFileSize()) + "|"
                    + juce::String (e.getModificationTime().toMilliseconds()));
        fp.sort (false);
        return fp;
    };

    const auto before = fingerprint();
    print ("Archivos en la carpeta del export: " + juce::String (before.size()));

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    juce::String err;
    auto ex = UsbExport::load (root, err);
    if (ex == nullptr) { print ("ERROR: " + err); return 1; }
    print ("export.pdb: " + juce::String (ex->tracks.size()) + " pistas, " + juce::String (ex->playlists.size())
           + juce::String::fromUTF8 (" playlists/carpetas, leído en ")
           + juce::String ((int) (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms");

    int missing = 0, withKey = 0, withBpm = 0, withAnlz = 0;
    for (auto& t : ex->tracks)
    {
        if (! ex->fileFor (t).existsAsFile()) ++missing;
        if (UsbExport::keyToCamelot (t.key).isNotEmpty()) ++withKey;
        if (t.bpm > 0) ++withBpm;
        if (t.analyzePath.isNotEmpty()) ++withAnlz;
    }
    print ("Archivos faltantes: " + juce::String (missing) + " · con BPM: "_u + juce::String (withBpm) + " · con key: "_u
           + juce::String (withKey) + juce::String::fromUTF8 (" · con análisis: ") + juce::String (withAnlz));

    print ("\n-- Playlists (primeras 25)");
    std::function<void (int, int)> tree = [&] (int parent, int depth)
    {
        int shown = 0;
        for (auto& p : ex->playlists)
        {
            if (p.parentId != parent || shown++ > 25) continue;
            print (juce::String::repeatedString ("   ", depth) + (p.isFolder ? "[+] " : " -  ") + p.name
                   + (p.isFolder ? juce::String() : "  (" + juce::String ((int) p.trackIds.size()) + ")"));
            if (p.isFolder && depth < 3) tree (p.id, depth + 1);
        }
    };
    tree (0, 0);

    print ("\n-- Muestra de pistas");
    for (size_t i = 0; i < std::min<size_t> (8, ex->tracks.size()); ++i)
    {
        const auto& t = ex->tracks[i * (ex->tracks.size() / std::min<size_t> (8, ex->tracks.size()))];
        const auto an = UsbExport::readAnalysis (*ex, t);
        int hot = 0;
        for (auto c : an.hotCues) if (c >= 0) ++hot;
        print (t.artist + " - " + t.title + " | " + juce::String (t.bpm, 2) + " BPM | key " + t.key + " (" + UsbExport::keyToCamelot (t.key)
               + ") | grid " + (an.grid.isValid() ? juce::String (an.grid.bpm, 2) + " @ " + juce::String (an.grid.firstBeatSeconds, 3) + "s" : "—"_u)
               + " | hot cues " + juce::String (hot) + " | cue " + (an.cuePoint >= 0 ? juce::String (an.cuePoint, 2) + "s" : "—"_u)
               + (ex->fileFor (t).existsAsFile() ? "" : "  [FALTA ARCHIVO]"));
    }

    // Cue statistics over the whole export (reads ANLZ files only).
    {
        int withHot = 0, withMem = 0, withLoop = 0, scanned = 0;
        juce::String example;
        for (auto& t : ex->tracks)
        {
            const auto an = UsbExport::readAnalysis (*ex, t);
            ++scanned;
            bool h = false, l = false;
            for (auto c : an.hotCues) h = h || c >= 0;
            for (auto [a, b] : an.hotLoops) l = l || b > a;
            withHot += h; withLoop += l; withMem += an.cuePoint >= 0;
            if (h && example.isEmpty())
            {
                example = t.artist + " - " + t.title + ":";
                for (int k = 0; k < 8; ++k)
                    if (an.hotCues[(size_t) k] >= 0) example << " " << juce::String::charToString ((juce::juce_wchar) ('A' + k)) << "=" << juce::String (an.hotCues[(size_t) k], 2) << "s";
            }
        }
        print ("\nCues en " + juce::String (scanned) + " pistas: con hot cues " + juce::String (withHot) + ", con memory cue "
               + juce::String (withMem) + ", con hot loops " + juce::String (withLoop));
        if (example.isNotEmpty()) print ("Ejemplo: " + example);
    }

    // Decode + play one real track from the stick.
    AudioEngine engine;
    for (auto& t : ex->tracks)
    {
        const auto f = ex->fileFor (t);
        if (! f.existsAsFile()) continue;
        std::unique_ptr<juce::AudioFormatReader> reader (engine.getFormatManager().createReaderFor (f));
        if (reader == nullptr) { print ("No se pudo decodificar: " + f.getFileName()); continue; }
        auto lt = std::make_shared<LoadedTrack>();
        lt->sampleRate = reader->sampleRate;
        const int len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 20));
        lt->audio.setSize (2, len);
        reader->read (&lt->audio, 0, len, 0, true, reader->numChannels > 1);
        auto& d = engine.deck (0);
        d.prepare (sr);
        d.setTrack (lt);
        d.setPlaying (true);
        run (d, 1.0);
        print (juce::String::fromUTF8 ("\nReproducción de prueba: ") + f.getFileName() + " → "_u + juce::String (d.getPosition(), 2) + " s avanzados");
        d.setTrack (nullptr);
        break;
    }

    const auto after = fingerprint();
    const bool same = before == after;
    print (same ? juce::String::fromUTF8 ("\nOK: ningún archivo del export cambió (") + juce::String (after.size()) + " archivos comparados)"
                : juce::String ("\nATENCION: hubo cambios en el export"));
    return same ? 0 : 1;
}

int runSelfTest (const juce::File& trackA, const juce::File& trackB, const juce::File& usbRoot)
{
    Report rep;
    AudioEngine engine;

    audioQualityTests (rep);

    std::cout << "== Analysis" << std::endl;
    auto ta = decode (engine, trackA);
    auto tb = decode (engine, trackB);
    if (ta == nullptr || tb == nullptr)
    {
        std::cout << "  cannot decode test tracks" << std::endl;
        return 2;
    }
    rep.check (std::abs (ta->grid().bpm - 124.0) < 0.05, "BPM pista A = 124", juce::String (ta->grid().bpm, 3));
    rep.check (std::abs (tb->grid().bpm - 128.0) < 0.05, "BPM pista B = 128", juce::String (tb->grid().bpm, 3));
    const auto keyA = Analysis::detectKey (ta->audio, ta->sampleRate);
    rep.check (keyA.isNotEmpty(), "Tonalidad detectada pista A", keyA + " = " + Analysis::camelotToMusical (keyA));
    rep.check (Analysis::camelotToMusical ("8A") == "Am" && Analysis::camelotToMusical ("8B") == "C"
               && Analysis::camelotToMusical ("11B") == "A", "Camelot ↔ notación musical"_u);
    rep.check (Analysis::camelotCompatible ("8A", "9A") && Analysis::camelotCompatible ("8A", "8B")
               && ! Analysis::camelotCompatible ("8A", "3B"), "Compatibilidad armónica Camelot"_u);

    auto& d = engine.deck (0);
    d.prepare (sr);
    d.setTrack (ta);
    const double beat = ta->grid().beatLengthSeconds();

    std::cout << "== Transporte" << std::endl;
    d.setPlaying (true);
    run (d, 1.0);
    rep.check (std::abs (d.getPosition() - 1.0) < 0.02, "Play avanza 1 s", juce::String (d.getPosition(), 3));

    d.setReverse (true);
    run (d, 0.5);
    d.setReverse (false);
    rep.check (std::abs (d.getPosition() - 0.5) < 0.03, "Reverse retrocede", juce::String (d.getPosition(), 3));

    std::cout << "== Slip / roll / censor" << std::endl;
    d.seek (ta->grid().timeOfBeat (16));
    run (d, 0.01);
    const double rollStart = d.getPosition();
    d.loopRoll (0.25, true);
    run (d, 1.0);
    rep.check (d.getPosition() < rollStart + 0.25 * beat + 0.02, "Loop roll repite 1/4 de beat", juce::String (d.getPosition() - rollStart, 3));
    d.loopRoll (0.25, false);
    run (d, 0.02);
    rep.check (std::abs (d.getPosition() - (rollStart + 1.02)) < 0.06, "Al soltar el roll sigue como si nada",
               juce::String (d.getPosition() - rollStart, 3) + " s");

    const double cStart = d.getPosition();
    d.setCensor (true);
    run (d, 0.5);
    rep.check (d.getPosition() < cStart, "Censor reproduce al revés"_u, juce::String (d.getPosition() - cStart, 3));
    d.setCensor (false);
    run (d, 0.02);
    rep.check (std::abs (d.getPosition() - (cStart + 0.52)) < 0.06, "Censor retoma donde estaría"_u, juce::String (d.getPosition() - cStart, 3));

    d.setSlip (true);
    const double sStart = d.getPosition();
    d.setReverse (true);
    run (d, 0.5);
    d.setReverse (false);
    run (d, 0.02);
    rep.check (std::abs (d.getPosition() - (sStart + 0.52)) < 0.06, "Slip + reverse retoma la posición"_u, juce::String (d.getPosition() - sStart, 3));
    d.setSlip (false);

    std::cout << "== Scratch / brake" << std::endl;
    const double scStart = d.getPosition();
    d.setScratching (true);
    for (int i = 0; i < 20; ++i) { d.scratchMove (-0.05); run (d, 0.02); }
    d.setScratching (false);
    rep.check (d.getPosition() < scStart + 0.1, "Scratch hacia atrás mueve el playhead"_u, juce::String (d.getPosition() - scStart, 3));

    d.brakeStop();
    run (d, 1.3);
    rep.check (! d.isPlaying(), "Brake detiene el deck");

    std::cout << "== Loops guardados" << std::endl;
    d.setPlaying (true);
    d.setAutoLoop (4);
    run (d, 0.02);           // loops are placed by the audio thread from the exact playhead
    d.saveLoop (0);
    d.exitLoop();
    d.seek (5.0);
    d.triggerSavedLoop (0);
    rep.check (d.isLoopActive() && std::abs (d.getLoopBeats() - 4.0) < 0.01, "Loop guardado se reactiva");

    std::cout << "== Quantize" << std::endl;
    {
        Deck q (0);
        q.prepare (sr);
        q.setTrack (ta);
        const auto g = ta->grid();
        const double beatSec = g.beatLengthSeconds();
        const double sampleBeats = (ta->sampleRate > 0 ? 1.0 / sr : 0.0) / beatSec;   // one output sample, in beats
        auto exactBeat = [&] { return g.beatAt (q.audioPositionSeconds()); };
        auto frac = [] (double b) { return b - std::floor (b); };

        // Hot cue set while paused, slightly after beat 40 → snaps exactly to beat 40.
        q.seek (g.timeOfBeat (40) + 0.012);
        run (q, 0.01);
        q.hotCue (0);
        rep.check (std::abs (g.beatAt (q.getHotCue (0)) - 40.0) < 1e-9, "Cue fijado se ajusta exacto al beat",
                   juce::String ((g.beatAt (q.getHotCue (0)) - 40.0) * beatSec * 1000.0, 4) + " ms");

        // Playing mid-beat at 16.37, press hot cue 1 → lands at 40.37 (+ block advance): beat phase kept.
        q.seek (g.timeOfBeat (16.37));
        q.setPlaying (true);
        run (q, 0.2);
        const double before = exactBeat();
        q.hotCue (0);
        std::vector<float> l (block), r (block);
        q.render (l.data(), r.data(), block);
        const double advance = (double) block / sr / beatSec;
        const double after = exactBeat();
        double err = frac (after) - frac (before + advance);
        if (err > 0.5) err -= 1.0;
        if (err < -0.5) err += 1.0;
        rep.check (std::abs (err) < sampleBeats && std::floor (after) == 40.0, "Hot cue con quantize mantiene la fase (error < 1 muestra)",
                   juce::String (err * beatSec * 1.0e6, 2) + " µs, beat "_u + juce::String (after, 4));

        // Beat jump +4 is exactly 4 beats.
        const double bj0 = exactBeat();
        q.beatJump (4.0);
        q.render (l.data(), r.data(), block);
        const double bjErr = exactBeat() - (bj0 + 4.0 + advance);
        rep.check (std::abs (bjErr) < sampleBeats, "Beat jump exacto (+4 beats)", juce::String (bjErr * beatSec * 1.0e6, 2) + " µs"_u);

        // Auto loop pressed 3 ms after beat 60 starts exactly on beat 60 (never the previous beat).
        q.seek (g.timeOfBeat (60) + 0.003 - (double) block / sr);
        q.render (l.data(), r.data(), block);
        q.setAutoLoop (4.0);
        q.render (l.data(), r.data(), block);
        rep.check (std::abs (g.beatAt (q.getLoopStart()) - 60.0) < 1e-9 && std::abs (q.getLoopBeats() - 4.0) < 1e-9,
                   "Auto loop arranca exacto en el beat (aunque se apriete 3 ms tarde)",
                   "inicio beat " + juce::String (g.beatAt (q.getLoopStart()), 6));
        q.exitLoop();

        // Resolution 1/4: a cue set at beat 30.30 snaps to 30.25.
        q.setPlaying (false);
        q.setQuantizeBeats (0.25);
        q.seek (g.timeOfBeat (30.30));
        run (q, 0.01);
        q.clearHotCue (1);
        q.hotCue (1);
        rep.check (std::abs (g.beatAt (q.getHotCue (1)) - 30.25) < 1e-9, "Resolución 1/4 de beat"_u, juce::String (g.beatAt (q.getHotCue (1)), 4));

        // With key lock the jump also keeps the phase (stretcher latency compensated).
        q.setQuantizeBeats (1.0);
        q.setKeyLock (true);
        q.setPitch (0.05);
        q.seek (g.timeOfBeat (20.61));
        q.setPlaying (true);
        run (q, 0.5);
        const double kb = exactBeat();
        q.hotCue (0);
        q.render (l.data(), r.data(), block);
        double kErr = frac (exactBeat()) - frac (kb + advance * 1.05);
        if (kErr > 0.5) kErr -= 1.0;
        if (kErr < -0.5) kErr += 1.0;
        rep.check (std::abs (kErr * beatSec) < 0.001, "Hot cue con key lock mantiene la fase (< 1 ms)", juce::String (kErr * beatSec * 1000.0, 3) + " ms");
    }

    std::cout << "== Carga y play inmediato" << std::endl;
    {
        // Deck playing another track with key lock, then a new track is loaded and played at once.
        Deck dk (0);
        dk.prepare (sr);
        dk.setTrack (tb);
        dk.setKeyLock (true);
        dk.setPitch (0.05);
        dk.setPlaying (true);
        run (dk, 1.0);
        dk.setPlaying (false);
        run (dk, 0.02);
        auto tone = sineTrack (1000.0, 48000.0, 5.0, 0.5f);
        dk.setTrack (tone);
        dk.setKeyLock (false);
        dk.setPitch (0.0);
        dk.setPlaying (true);
        const auto y = capture (dk, 0.5);
        const double db = sineResidualDb (y, 1000.0, sr, 600);   // from 12.5 ms on (after the 4 ms fade-in)
        rep.check (db < -80.0, "Cargar y dar play al instante suena limpio (sin restos del tema anterior)", juce::String (db, 1) + " dB");
    }

    std::cout << "== SAMPLES/RMX" << std::endl;
    {
        AudioEngine re;
        re.prepareForTest (sr, block);
        re.autoGain = false;
        re.limiter().enabled = false;
        auto silent = std::make_shared<LoadedTrack>();
        silent->sampleRate = sr;
        silent->audio.setSize (2, (int) (sr * 30));
        silent->audio.clear();
        silent->setGrid ({ 120.0, 0.0 });                  // 1 beat = 0.5 s = 24000 samples
        auto& dk = re.deck (0);
        dk.setTrack (silent);
        dk.seek (10.0 + 0.137);                            // mid-beat
        dk.setPlaying (true);
        std::vector<float> ol (block), orr (block), cap;
        float* outs2[2] = { ol.data(), orr.data() };
        auto runBlocks = [&] (int count) { for (int k = 0; k < count; ++k) { re.audioDeviceIOCallbackWithContext (nullptr, 0, outs2, 2, block, {}); cap.insert (cap.end(), ol.begin(), ol.end()); } };
        runBlocks (4);
        const size_t before = cap.size();
        const double beatAtCap = dk.audioBlockBeat() + block * (120.0 / 60.0 / sr);   // next block's first sample
        re.rmx (0).padDown (RmxUnit::kick, false);
        runBlocks (40);
        size_t onset = 0;
        for (size_t i = before; i < cap.size(); ++i) if (std::abs (cap[i]) > 1.0e-4f) { onset = i; break; }
        juce::ignoreUnused (beatAtCap);
        // The master limiter delays all output equally (look-ahead): the hit is the very next sample out.
        const int wait = (int) (onset - before) - re.limiter().latencySamples();
        rep.check (onset > 0 && wait <= 1, "X-PAD suena apenas se aprieta (no espera el próximo golpe)"_u, juce::String (wait) + " muestras");

        // Held → roll in 1/8: count onsets during 2 beats.
        int onsets = 0;
        cap.clear();
        runBlocks ((int) (1.0 * sr / block));             // 1 s = 2 beats at 120 BPM
        re.rmx (0).padUp (RmxUnit::kick);
        // Energy in 25 ms windows: each new kick makes the energy jump (body + click).
        double prevEnv = 1.0e9, lastOnset = -1.0;        // the first window holds the tail of the initial hit
        const size_t win = (size_t) (0.025 * sr);
        for (size_t w0 = 0; w0 + win < cap.size(); w0 += win)
        {
            double e = 0; for (size_t i = w0; i < w0 + win; ++i) e += (double) cap[i] * cap[i];
            if (e > 1.0 && e > prevEnv * 2.0 && (double) w0 / sr - lastOnset > 0.06) { ++onsets; lastOnset = (double) w0 / sr; }
            prevEnv = e;
        }
        rep.check (onsets == 8, "Mantener el X-PAD hace roll (VEL. por defecto 1/16)", juce::String (onsets) + " golpes en 2 beats");

        // VEL. knob and the held-pad gesture: left / right halves or doubles the roll speed around VEL.
        re.rmx (0).setBaseRoll (0.5);
        re.rmx (0).padDown (RmxUnit::snare, false);
        const double atBase = re.rmx (0).rollRate (RmxUnit::snare);
        re.rmx (0).padGesture (RmxUnit::snare, 5.0f, 1);
        const double faster = re.rmx (0).rollRate (RmxUnit::snare);
        const float liveSt = re.rmx (0).liveTone (RmxUnit::snare);
        re.rmx (0).padUp (RmxUnit::snare);
        const double released = re.rmx (0).rollRate (RmxUnit::snare);
        rep.check (std::abs (atBase - 0.5) < 1e-9 && std::abs (faster - 0.25) < 1e-9 && std::abs (liveSt - 5.0f) < 1e-6
                       && std::abs (released - 0.5) < 1e-9 && std::abs (re.rmx (0).liveTone (RmxUnit::snare)) < 1e-6,
                   "Gesto del X-PAD: arriba/abajo = tono, izquierda/derecha = velocidad del roll; al soltar vuelve a las perillas"_u);
        re.rmx (0).setBaseRoll (RmxUnit::defaultRoll);
        runBlocks ((int) (1.5 * sr / block));             // let the quantized snare hit and its tail pass

        // Speed up and back down while rolling: every hit must still sit on the 1/16 grid.
        {
            auto& rx = re.rmx (0);
            rx.setSample (RmxUnit::hat, [] { auto b = std::make_shared<juce::AudioBuffer<float>> (2, 64);
                                               b->clear(); for (int i = 0; i < 64; ++i) { b->setSample (0, i, 0.5f); b->setSample (1, i, 0.5f); } return b; }(), "click");
            const double bpsStep = 120.0 / 60.0 / sr;
            cap.clear();
            const double capStartBeat = dk.audioBlockBeat() + block * bpsStep;
            rx.padDown (RmxUnit::hat, false);
            runBlocks (60);                                   // first hit (waits for the next beat) and some 1/16
            rx.padGesture (RmxUnit::hat, 0.0f, 1);            // 1/32
            runBlocks (13);
            rx.padGesture (RmxUnit::hat, 0.0f, 2);            // 1/64
            runBlocks (7);
            rx.padGesture (RmxUnit::hat, 0.0f, 0);            // back to 1/16
            const size_t backAt = cap.size();
            runBlocks (60);
            rx.padUp (RmxUnit::hat);
            runBlocks (10);
            double worstOff = 0.0; int hits = 0;
            const size_t lat = (size_t) re.limiter().latencySamples();
            for (size_t i = 1; i < cap.size(); ++i)
                if (std::abs (cap[i]) > 0.3f && std::abs (cap[i - 1]) < 0.3f && i > lat)
                {
                    const double b = capStartBeat + (double) (i - lat) * bpsStep;
                    // after the return to 1/16 (one step of slack), hits must be on 1/16; before, at least on 1/64
                    const double g = i > backAt + (size_t) (0.25 * sr) ? 4.0 : 16.0;
                    const double off = (b * g - std::round (b * g)) / g;
                    worstOff = juce::jmax (worstOff, std::abs (off) / bpsStep);
                    ++hits;
                }
            rep.check (hits >= 10 && worstOff <= 1.5, "Roll del RMX: subir y bajar la velocidad nunca lo saca de tiempo",
                       juce::String (hits) + " golpes · desvío máx " + juce::String (worstOff, 2) + " muestras");
            rx.setSample (RmxUnit::hat, nullptr, {});
        }

        // Wherever in the beat the pad is pressed, the hit sounds at once.
        auto pressAt = [&] (double beatFrac)
        {
            dk.seek (12.0 + beatFrac * 0.5);
            runBlocks (30);                                         // let earlier hits die out
            for (int k = 0; k < 400; ++k)                           // advance until the next block starts right after a beat
            {
                const double nb = dk.audioBlockBeat() + block * (120.0 / 60.0 / sr);
                const double ph = nb - std::floor (nb);
                if (std::abs (ph - beatFrac) < block * (120.0 / 60.0 / sr) * 0.5) break;
                runBlocks (1);
            }
            const double nextBlockBeat = dk.audioBlockBeat() + block * (120.0 / 60.0 / sr);
            cap.clear();
            re.rmx (0).padDown (RmxUnit::kick, false);
            runBlocks (1);
            re.rmx (0).padUp (RmxUnit::kick);
            runBlocks ((int) (0.6 * sr / block));
            size_t on = 0;
            for (size_t i = 0; i < cap.size(); ++i) if (std::abs (cap[i]) > 1.0e-4f) { on = i; break; }
            return std::pair<double, size_t> { nextBlockBeat, on };
        };
        const auto lat = (size_t) re.limiter().latencySamples();
        const auto early = pressAt (0.05).second, mid = pressAt (0.40).second, lateHit = pressAt (0.93).second;
        rep.check (early <= lat + 1 && mid <= lat + 1 && lateHit <= lat + 1, "X-PAD al instante en cualquier momento del beat"_u,
                   juce::String ((int) early - (int) lat) + " · " + juce::String ((int) mid - (int) lat) + " · " + juce::String ((int) lateHit - (int) lat) + " muestras");
    }
    {
        // X-PAD roll against the music itself: a track with a click on every beat (left channel only), played
        // with and without key lock at several tempos. The first hit sounds at once; every following roll
        // hit (a short click on both channels) must sit on a 1/16 of the clicks we hear.
        auto clicks = std::make_shared<LoadedTrack>();
        clicks->sampleRate = sr;
        clicks->audio.setSize (2, (int) (sr * 40));
        clicks->audio.clear();
        for (int b = 0; b < 80; ++b)
            for (int i = 0; i < 48; ++i)
                clicks->audio.setSample (0, b * 24000 + i, 0.8f * std::sin ((float) i * 0.9f) * (1.0f - (float) i / 48.0f));
        clicks->setGrid ({ 120.0, 0.0 });
        auto padClick = std::make_shared<juce::AudioBuffer<float>> (2, 64);
        padClick->clear();
        for (int i = 0; i < 64; ++i) { padClick->setSample (0, i, 0.5f); padClick->setSample (1, i, 0.5f); }
        juce::String worst;
        double maxErrMs = 0.0;
        int rollHits = 0;
        struct Cfg { bool keyLock; double pitch; };
        for (auto cfg : { Cfg { false, 0.0 }, Cfg { false, 0.06 }, Cfg { true, 0.06 }, Cfg { true, -0.04 }, Cfg { true, 0.0 } })
        {
            AudioEngine re;
            re.prepareForTest (sr, block);
            re.autoGain = false;
            re.limiter().enabled = false;
            re.rmx (0).setSample (RmxUnit::hat, padClick, "click");
            auto& dk = re.deck (0);
            dk.setKeyLock (cfg.keyLock);
            dk.setTrack (clicks);
            dk.setPitch (cfg.pitch);
            dk.seek (4.0);
            dk.setPlaying (true);
            std::vector<float> ol (block), orr (block), capL, capR;
            float* outs2[2] = { ol.data(), orr.data() };
            auto run = [&] (int count) { for (int k = 0; k < count; ++k) { re.audioDeviceIOCallbackWithContext (nullptr, 0, outs2, 2, block, {});
                                                                           capL.insert (capL.end(), ol.begin(), ol.end()); capR.insert (capR.end(), orr.begin(), orr.end()); } };
            run (200);
            const double sixteenth = 0.25 * 60.0 / (120.0 * (1.0 + cfg.pitch)) * sr;   // output samples
            for (int trial = 0; trial < 3; ++trial)
            {
                run (7 + trial * 5);                                  // press at varied phases
                const size_t from = capR.size();
                re.rmx (0).padDown (RmxUnit::hat, false);
                run ((int) (1.0 * sr / block));
                re.rmx (0).padUp (RmxUnit::hat);
                run (20);
                std::vector<size_t> hits;
                for (size_t i = from + 1; i < capR.size(); ++i)
                    if (std::abs (capR[i]) > 0.3f && std::abs (capR[i - 1]) < 0.3f && (hits.empty() || i - hits.back() > 1000))
                        hits.push_back (i);
                for (size_t h = 1; h < hits.size(); ++h)              // the first hit is the immediate one
                {
                    const size_t hit = hits[h];
                    size_t click = 0;                                 // last music click onset at or before the hit
                    for (size_t i = hit; i > 2 && i + 30000 > hit; --i)
                        if (std::abs (capL[i] - capR[i]) > 0.05f && std::abs (capL[i - 1] - capR[i - 1]) <= 0.05f
                            && std::abs (capL[i - 2] - capR[i - 2]) <= 0.05f) { bool quiet = true;
                                for (size_t j = i > 600 ? i - 600 : 0; j < i - 2; ++j) if (std::abs (capL[j] - capR[j]) > 0.05f) { quiet = false; break; }
                                if (quiet) { click = i; break; } }
                    if (click == 0) continue;
                    const double gap = (double) (hit - click);
                    const double err = (gap - std::round (gap / sixteenth) * sixteenth) / sr * 1000.0;
                    ++rollHits;
                    if (std::abs (err) > std::abs (maxErrMs)) { maxErrMs = err;
                        worst = juce::String (cfg.keyLock ? "keylock " : "") + juce::String (cfg.pitch * 100.0, 0) + "%"; }
                }
            }
        }
        rep.check (rollHits > 50 && std::abs (maxErrMs) < 1.0, "Roll del X-PAD en tiempo con la música (key lock y tempo cambiado)"_u,
                   juce::String (rollHits) + " golpes · " + juce::String (maxErrMs, 2) + " ms · " + worst);
    }
    {
        // TONO renders the sound in another key without changing its length or delaying its attack.
        juce::AudioBuffer<float> src (2, (int) (0.5 * sr));
        for (int i = 0; i < src.getNumSamples(); ++i)
        {
            const float v = (i < (int) (0.45 * sr)) ? 0.5f * std::sin (juce::MathConstants<float>::twoPi * 440.0f * (float) i / (float) sr) : 0.0f;
            src.setSample (0, i, v); src.setSample (1, i, v);
        }
        auto up = RmxUnit::render (src, sr, 12.0f, 1.0f);
        int crossings = 0, first = -1;
        const int a0 = (int) (0.1 * sr), a1 = (int) (0.3 * sr);
        for (int i = 1; up != nullptr && i < up->getNumSamples(); ++i)
        {
            if (first < 0 && std::abs (up->getSample (0, i)) > 0.05f) first = i;
            if (i > a0 && i < a1 && (up->getSample (0, i - 1) < 0.0f) != (up->getSample (0, i) < 0.0f)) ++crossings;
        }
        const double freq = crossings / 2.0 / ((a1 - a0) / sr);
        rep.check (up != nullptr && up->getNumSamples() == src.getNumSamples() && std::abs (freq - 880.0) < 15.0 && first >= 0 && first < (int) (0.003 * sr),
                   "TONO +12 sube una octava sin cambiar la duración ni atrasar el golpe"_u,
                   juce::String (freq, 0) + " Hz · ataque " + juce::String (first / sr * 1000.0, 1) + " ms");
    }
    {
        // Scene and release FX on a tone track.
        AudioEngine re;
        re.prepareForTest (sr, block);
        re.autoGain = false;
        re.limiter().enabled = false;
        auto tone = sineTrack (100.0, 48000.0, 30.0, 0.5f);
        tone->setGrid ({ 120.0, 0.0 });
        auto& dk = re.deck (0);
        dk.setTrack (tone);
        dk.setPlaying (true);
        std::vector<float> ol (block), orr (block);
        float* outs2[2] = { ol.data(), orr.data() };
        auto blockRms = [&] (int count)
        {
            double a = 0;
            for (int k = 0; k < count; ++k)
            {
                re.audioDeviceIOCallbackWithContext (nullptr, 0, outs2, 2, block, {});
                for (auto v : ol) a += (double) v * v;
            }
            return std::sqrt (a / (count * block));
        };
        const double dry = blockRms (50);
        re.rmx (0).padDown (RmxUnit::buildUp, false);
        blockRms ((int) (4.0 * sr / block));                 // 8 beats at 120 BPM
        const double built = blockRms (20);
        re.rmx (0).padUp (RmxUnit::buildUp);
        blockRms (10);
        const double back = blockRms (20);
        rep.check (built < dry * 0.5 && back > dry * 0.9, "BUILD UP quita los graves al crecer y vuelve al soltar",
                   juce::String (built / dry * 100.0, 0) + " % → "_u + juce::String (back / dry * 100.0, 0) + " %");

        re.rmx (0).padDown (RmxUnit::brake, false);
        blockRms (150);                                       // > 1 s brake
        const double braked = blockRms (10);
        re.rmx (0).padUp (RmxUnit::brake);
        blockRms (5);
        const double unbraked = blockRms (10);
        rep.check (braked < dry * 0.05 && unbraked > dry * 0.9 && dk.isPlaying(), "BRAKE frena el sonido y el tema sigue corriendo",
                   juce::String (braked / dry * 100.0, 1) + " % → "_u + juce::String (unbraked / dry * 100.0, 0) + " %");

        re.rmx (0).padDown (RmxUnit::echoOut, false);
        re.rmx (0).padUp (RmxUnit::echoOut);
        const double echoNow = blockRms ((int) (0.25 * sr / block));          // first ½ beat after the press
        rep.check (echoNow > dry * 0.5, "ECHO OUT repite al instante lo último que sonó (sin hueco)"_u,
                   juce::String (echoNow / dry * 100.0, 0) + " %");
        blockRms ((int) (2.25 * sr / block));                 // rest of the 4 beats + 1
        rep.check (! re.rmx (0).isLit (RmxUnit::echoOut) && blockRms (20) > dry * 0.8, "ECHO OUT vuelve solo al tema");
    }

    std::cout << "== BEAT FX" << std::endl;
    {
        // Run one Beat FX on a stereo signal with a 120 BPM grid (1 beat = 0.5 s at 48 kHz).
        using BT = BeatFx::Type;
        const double bpsGrid = 2.0 / sr;
        // Music-like test signal: two tones plus deterministic noise (a pure tone would make echoes add up in phase).
        auto signal = [] (juce::int64 k)
        {
            auto h = (juce::uint32) (k * 2654435761u); h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            const float nz = (float) (h & 0xffff) / 32768.0f - 1.0f;
            return (float) (0.2 * std::sin (0.00523 * (double) k) + 0.1 * std::sin (0.0731 * (double) k)) + 0.15f * nz;
        };
        struct Run { std::vector<float> in, out; };
        auto runFx = [&] (BT t, float level, float paramValue, double seconds, double onAt, double offAt, double startBeat)
        {
            BeatFx fx;
            fx.prepare (sr, block);
            fx.type = (int) t; fx.wet = level; fx.param = paramValue;
            Run res;
            const int total = (int) (seconds * sr / block);
            std::vector<float> l (block), r (block);
            for (int b = 0; b < total; ++b)
            {
                const double tSec = (double) b * block / sr;
                fx.enabled = tSec >= onAt && tSec < offAt;
                for (int i = 0; i < block; ++i)
                {
                    const auto k = (juce::int64) b * block + i;
                    l[(size_t) i] = r[(size_t) i] = signal (k);
                    res.in.push_back (l[(size_t) i]);
                }
                fx.process (l.data(), r.data(), block, 120.0, startBeat + (double) b * block * bpsGrid, bpsGrid);
                res.out.insert (res.out.end(), l.begin(), l.end());
            }
            return res;
        };

        // 1. All 14 are stable, on and off, at every LEVEL/DEPTH extreme.
        juce::String unstable;
        for (int t = 0; t < (int) BT::numTypes; ++t)
            for (float lv : { 0.0f, 1.0f })
            {
                const auto res = runFx ((BT) t, lv, BeatFx::defaultParam ((BT) t), 3.0, 0.3, 2.0, 0.0);
                float pk = 0.0f; bool finite = true;
                for (auto v : res.out) { finite = finite && std::isfinite (v); pk = juce::jmax (pk, std::abs (v)); }
                if (! finite || pk > 1.6f) unstable << BeatFx::typeName ((BT) t) << " ";
            }
        rep.check (unstable.isEmpty(), "Los 14 BEAT FX procesan estable (sin NaN ni desbordes)", unstable);

        // 2. DELAY / ECHO / PING PONG / SPIRAL / REVERB: the track keeps its level, the effect is
        //    clearly heard, and it keeps ringing after FX is switched off (input silent), then dies out.
        juce::String weak, endless;
        for (auto t : { BT::delay, BT::echo, BT::pingPong, BT::spiral, BT::reverb })
        {
            BeatFx fx;
            fx.prepare (sr, block);
            fx.type = (int) t; fx.wet = 0.8f; fx.param = BeatFx::defaultParam (t); fx.enabled = true;
            std::vector<float> l (block), r (block);
            double inE = 0, outE = 0, diffE = 0, ringE = 0, endE = 0;
            const int total = (int) (14.0 * sr / block);
            for (int b = 0; b < total; ++b)
            {
                const double tSec = (double) b * block / sr;
                if (tSec >= 3.0) fx.enabled = false;
                for (int i = 0; i < block; ++i)
                    l[(size_t) i] = r[(size_t) i] = tSec < 3.0 ? signal ((juce::int64) b * block + i) : 0.0f;
                std::vector<float> dry (l);
                fx.process (l.data(), r.data(), block, 120.0, (double) b * block * bpsGrid, bpsGrid);
                for (int i = 0; i < block; ++i)
                {
                    if (tSec > 1.0 && tSec < 3.0) { inE += dry[(size_t) i] * dry[(size_t) i]; outE += l[(size_t) i] * l[(size_t) i];
                                                    diffE += (l[(size_t) i] - dry[(size_t) i]) * (l[(size_t) i] - dry[(size_t) i]); }
                    if (tSec >= 3.0 && tSec < 3.3) ringE += l[(size_t) i] * l[(size_t) i];
                    if (tSec > 13.5) endE += l[(size_t) i] * l[(size_t) i];
                }
            }
            const double effDb = 10.0 * std::log10 (diffE / juce::jmax (1e-12, inE));
            const double gainDb = 10.0 * std::log10 (outE / juce::jmax (1e-12, inE));
            if (effDb < -12.0 || gainDb < -1.0 || gainDb > 6.0 || ringE < 1.0)
                weak << BeatFx::typeName (t) << " (efecto " << juce::String (effDb, 1) << " dB, nivel " << juce::String (gainDb, 1) << " dB) ";
            if (endE > 1.0e-3) endless << BeatFx::typeName (t) << " ";
        }
        rep.check (weak.isEmpty(), "DELAY/ECHO/PING PONG/SPIRAL/REVERB: se escuchan, no bajan el tema y siguen sonando al apagar"_u, weak);
        rep.check (endless.isEmpty(), "BEAT FX: las colas se apagan solas", endless);

        // 3. DELAY = one repeat, ECHO = several: an impulse in, count the repeats.
        auto repeats = [&] (BT t)
        {
            BeatFx fx; fx.prepare (sr, block);
            fx.type = (int) t; fx.wet = 1.0f; fx.param = BeatFx::defaultParam (t); fx.enabled = true;   // 1/2 beat = 12000
            std::vector<float> l (block), r (block), out;
            for (int b = 0; b < (int) (4.0 * sr / block); ++b)
            {
                std::fill (l.begin(), l.end(), 0.0f); std::fill (r.begin(), r.end(), 0.0f);
                if (b == 10) l[0] = r[0] = 1.0f;
                fx.process (l.data(), r.data(), block, 120.0, (double) b * block * bpsGrid, bpsGrid);
                out.insert (out.end(), l.begin(), l.end());
            }
            int count = 0;
            for (int k = 1; k < 7; ++k)
            {
                const size_t at = (size_t) (10 * block + k * 12000);
                float pk = 0.0f;
                for (size_t i = at - 8; i < at + 8 && i < out.size(); ++i) pk = juce::jmax (pk, std::abs (out[i]));
                if (pk > 0.02f) ++count;
            }
            return count;
        };
        const int dRep = repeats (BT::delay), eRep = repeats (BT::echo);
        rep.check (dRep == 1 && eRep >= 4, "DELAY repite una vez y ECHO repite varias veces apagándose, a 1/2 beat exacto",
                   "DELAY " + juce::String (dRep) + " · ECHO " + juce::String (eRep));

        // 4. TRANS cuts on the grid: 1/4-beat cells, sound in the first part, cut in the last part.
        {
            const auto res = runFx (BT::trans, 1.0f, BeatFx::defaultParam (BT::trans), 2.0, 0.2, 9.0, 0.0);
            const int cell = 6000;                                           // 1/4 beat
            double openE = 0, cutE = 0;
            for (int c = 4; c < 15; ++c)                                    // 0.5 s … 1.9 s
                for (int i = 0; i < cell; ++i)
                {
                    const size_t k = (size_t) (c * cell + i);
                    const double v = (double) res.out[k] * res.out[k];
                    if (i > 300 && i < cell / 2 - 300) openE += v;
                    if (i > cell * 2 / 3 + 300 && i < cell - 300) cutE += v;
                }
            rep.check (cutE < openE * 1.0e-3, "TRANS corta en la grilla (1/4 beat), siempre en tiempo",
                       juce::String (10.0 * std::log10 (juce::jmax (1e-12, cutE) / juce::jmax (1e-12, openE)), 0) + " dB en el corte");
        }

        // 5. ROLL: pressed mid-beat, it repeats the 1/4 beat that started on the grid — sample exact.
        {
            const double onAt = 1.0 + 0.0021;                                     // not on a grid line
            const auto res = runFx (BT::roll, 1.0f, BeatFx::defaultParam (BT::roll), 2.0, onAt, 9.0, 0.0);
            const int onSample = (int) std::ceil (onAt * sr / block) * block;
            const int L = 6000;
            const int startCell = (onSample / L) * L;                             // last 1/4 beat boundary
            double err = 0.0;
            for (int k = onSample + 1000; k < onSample + 3 * L; ++k)
            {
                const int rel = (k - startCell) % L;
                if (rel < 200) continue;                                          // wrap crossfade
                err = juce::jmax (err, (double) std::abs (res.out[(size_t) k] - signal (startCell + rel)));
            }
            rep.check (err < 1.0e-3, "ROLL repite el 1/4 beat desde la línea de la grilla (exacto a la muestra)", juce::String (err, 5));
        }

        // 6. VINYL BRAKE slows the sound to a stop over the BEAT time (1 beat), and comes back when off.
        {
            const auto res = runFx (BT::vinylBrake, 1.0f, BeatFx::defaultParam (BT::vinylBrake), 3.0, 0.5, 2.0, 0.0);
            auto rmsAt = [&] (double a, double b2) { double e = 0; int c = 0; for (size_t k = (size_t) (a * sr); k < (size_t) (b2 * sr); ++k) { e += res.out[k] * res.out[k]; ++c; } return std::sqrt (e / c); };
            const double before = rmsAt (0.2, 0.45), stopped = rmsAt (1.2, 1.9), after = rmsAt (2.1, 2.5);
            rep.check (stopped < before * 0.02 && after > before * 0.9, "VINYL BRAKE frena hasta parar en 1 beat y vuelve al apagar",
                       juce::String (stopped / before * 100.0, 1) + " % → "_u + juce::String (after / before * 100.0, 0) + " %");
        }
    }

    std::cout << "== Salidas de audio" << std::endl;
    {
        // A 4-output card: SOLO ALTAVOCES, ALTAVOCES + AUDIFONOS, DIVISOR EN "Y" and DECKS SEPARADOS.
        auto toneA = sineTrack (200.0, 48000.0, 10.0, 0.4f); toneA->setGrid ({ 120.0, 0.0 });
        auto toneB = sineTrack (300.0, 48000.0, 10.0, 0.4f); toneB->setGrid ({ 120.0, 0.0 });
        auto measure = [&] (int mode, bool split)
        {
            AudioEngine ae;
            ae.prepareForTest (sr, block, 4);
            ae.autoGain = false;
            ae.limiter().enabled = false;
            ae.deck (0).setTrack (toneA); ae.deck (1).setTrack (toneB);
            ae.deck (0).setPlaying (true);
            ae.deck (1).setPlaying (true);
            ae.channel (0).pfl = true;                       // headphones listen to deck 1 only
            ae.cueMix = 0.0f;
            ae.crossfader = -1.0f;                           // master = deck 1 only
            ae.outputMode = mode; ae.splitMono = split;
            ae.masterChannel = 0; ae.phonesChannel = 2; ae.deckChannel[0] = 0; ae.deckChannel[1] = 2;
            std::array<std::vector<float>, 4> o; for (auto& v : o) v.assign (block, 0.0f);
            float* outs4[4] = { o[0].data(), o[1].data(), o[2].data(), o[3].data() };
            std::array<double, 4> e {};
            for (int b = 0; b < 60; ++b)
            {
                ae.audioDeviceIOCallbackWithContext (nullptr, 0, outs4, 4, block, {});
                if (b > 20) for (int c = 0; c < 4; ++c) for (auto v : o[(size_t) c]) e[(size_t) c] += (double) v * v;
            }
            for (auto& x : e) x = std::sqrt (x / (39.0 * block));
            return e;
        };
        const auto spk = measure (AudioEngine::speakersOnly, false);
        const auto both = measure (AudioEngine::speakersPhones, false);
        const auto ysplit = measure (AudioEngine::speakersPhones, true);
        const auto sep = measure (AudioEngine::separateDecks, false);
        const bool okSpk  = spk[0] > 0.1 && spk[1] > 0.1 && spk[2] < 1e-6 && spk[3] < 1e-6;
        const bool okBoth = both[0] > 0.1 && both[2] > 0.1 && both[3] > 0.1;
        const bool okY    = ysplit[0] > 0.1 && ysplit[1] > 0.1 && ysplit[2] < 1e-6 && ysplit[3] < 1e-6;
        const bool okSep  = sep[0] > 0.1 && sep[1] > 0.1 && sep[2] > 0.1 && sep[3] > 0.1;
        rep.check (okSpk && okBoth && okY && okSep, "Salidas: solo altavoces, altavoces + auriculares, divisor en Y y decks separados"_u,
                   juce::String (okSpk ? "ok" : "ALTAVOCES") + " · " + (okBoth ? "ok" : "AURIC.") + " · " + (okY ? "ok" : "Y") + " · " + (okSep ? "ok" : "DECKS"));
    }

    std::cout << "== FX post-fader" << std::endl;
    {
        // As on a club mixer: with ECHO / REVERB on, pulling the channel fader down stops feeding the effect,
        // but its tail keeps ringing at full level; without effects the fader still silences the deck.
        auto tone = sineTrack (220.0, 48000.0, 30.0, 0.5f);
        tone->setGrid ({ 120.0, 0.0 });
        juce::String bad;
        for (int type : { (int) BeatFx::Type::echo, (int) BeatFx::Type::reverb, -1 })
        {
            AudioEngine pe;
            pe.prepareForTest (sr, block);
            pe.autoGain = false;
            pe.limiter().enabled = false;
            auto& dk = pe.deck (0);
            dk.setTrack (tone);
            dk.setPlaying (true);
            auto& fx = pe.fx (0, 0);
            if (type >= 0) { fx.type = type; fx.wet = 0.7f; fx.param = 0.5f; fx.enabled = true; }
            std::vector<float> ol (block), orr (block);
            float* o2[2] = { ol.data(), orr.data() };
            auto rms = [&] (int count)
            {
                double a = 0;
                for (int k = 0; k < count; ++k)
                {
                    pe.audioDeviceIOCallbackWithContext (nullptr, 0, o2, 2, block, {});
                    for (auto v : ol) a += (double) v * v;
                }
                return std::sqrt (a / (count * block));
            };
            const double before = rms (150);
            pe.channel (0).fader = 0.0f;
            rms (2);
            const double tail = rms ((int) (0.25 * sr / block));     // the ½ s right after the fader closes
            const double ratio = tail / juce::jmax (1.0e-9, before);
            const bool ok = type < 0 ? ratio < 1.0e-3 : ratio > 0.15;
            if (! ok) bad << (type < 0 ? juce::String ("sin FX") : BeatFx::typeName ((BeatFx::Type) type)) << " (" << juce::String (ratio * 100.0, 1) << " %) ";
        }
        rep.check (bad.isEmpty(), "Bajar el fader deja sonar la cola del ECHO / REVERB (FX post-fader)"_u, bad);
    }

    std::cout << "== Funciones de deck" << std::endl;
    {
        Deck c (0);
        c.prepare (sr);
        auto quiet = std::make_shared<LoadedTrack>();     // 1.5 s silence, then the groove
        quiet->sampleRate = ta->sampleRate;
        const int lead = (int) (1.5 * ta->sampleRate);
        quiet->audio.setSize (2, ta->numSamples() + lead);
        quiet->audio.clear();
        for (int ch = 0; ch < 2; ++ch) quiet->audio.copyFrom (ch, lead, ta->audio, ch, 0, ta->numSamples());
        quiet->wave = Analysis::computeWaveform (quiet->audio, quiet->sampleRate);
        quiet->setGrid ({ ta->grid().bpm, ta->grid().firstBeatSeconds + 1.5 });
        c.setTrack (quiet);
        rep.check (std::abs (c.getCuePoint() - 1.5) < 0.02, "Auto cue en el primer sonido", juce::String (c.getCuePoint(), 3) + " s");

        // Gate cue: paused, hold hot cue → plays; release → back to the cue, paused.
        c.seek (10.0);
        run (c, 0.01);
        c.hotCue (2);                       // set at 10 s
        c.seek (3.0);
        run (c, 0.01);
        c.hotCue (2);                       // press while paused
        run (c, 0.5);
        const bool playedWhileHeld = c.isPlaying() && c.audioPositionSeconds() > c.getHotCue (2) + 0.4;
        c.hotCueRelease (2);
        run (c, 0.05);
        rep.check (playedWhileHeld && ! c.isPlaying() && std::abs (c.audioPositionSeconds() - c.getHotCue (2)) < 0.001,
                   "Gate cue: suena mientras se mantiene y vuelve al soltar");

        // Instant doubles: the other deck lands in the same place, tempo and play state.
        Deck other (1);
        other.prepare (sr);
        other.setTrack (ta);
        other.setPitch (0.04);
        other.seek (30.0);
        other.setPlaying (true);
        run (other, 0.3);
        Deck copy (0);
        copy.prepare (sr);
        copy.cloneFrom (other);
        run (copy, 0.01);
        run (other, 0.01);
        rep.check (copy.getTrack() == other.getTrack() && copy.isPlaying() && std::abs (copy.getPitch() - 0.04) < 1e-9
                   && std::abs (copy.audioPositionSeconds() - other.audioPositionSeconds()) < 0.02,
                   "Instant doubles copia tema, posición, tempo y estado"_u,
                   juce::String ((copy.audioPositionSeconds() - other.audioPositionSeconds()) * 1000.0, 1) + " ms");
    }

    {
        // Pitch to 0 % button: one press glides, a second press jumps.
        Deck pd (0);
        pd.prepare (sr);
        pd.setTrack (ta);
        pd.setPitchRange (0.5);
        pd.setPitch (0.2);
        pd.setPlaying (true);
        pd.pitchResetPressed();
        run (pd, 1.0);
        const double after1 = pd.getPitch();
        run (pd, 5.5);
        const double after45 = pd.getPitch();
        pd.setPitch (0.2);
        pd.pitchResetPressed();
        pd.pitchResetPressed();
        const double doubled = pd.getPitch();
        pd.setPitch (-0.1);
        pd.pitchResetPressed();
        pd.setPitch (-0.07);                         // the DJ grabs the fader: glide stops
        run (pd, 1.0);
        rep.check (after1 > 0.15 && after1 < 0.18 && std::abs (after45) < 1e-9 && doubled == 0.0 && std::abs (pd.getPitch() + 0.07) < 1e-9,
                   "Botón 0 del pitch: un toque vuelve de a poco, dos toques de una, la mano manda"_u,
                   "+20 % → " + juce::String (after1 * 100.0, 1) + " % al segundo, " + juce::String (after45 * 100.0, 1) + " % a los 6,5 s");
    }

    {
        // Grid corrector: from a BPM 0.3 off and a phase 30 ms off, back onto the kicks.
        const auto truth = Analysis::detectBeatGrid (tb->audio, tb->sampleRate);
        const BeatGrid rough { truth.bpm + 0.3, truth.firstBeatSeconds + 0.030 };
        const auto fixed = Analysis::refineGrid (tb->audio, tb->sampleRate, rough);
        const double beat = 60.0 / fixed.bpm;
        double d = std::fmod (fixed.firstBeatSeconds - truth.firstBeatSeconds, beat);
        if (d > beat / 2) d -= beat;
        if (d < -beat / 2) d += beat;
        rep.check (std::abs (fixed.bpm - truth.bpm) < 0.02 && std::abs (d) < 0.003,
                   "Ajuste automático de grilla: BPM y fase vuelven a los bombos"_u,
                   juce::String (fixed.bpm, 2) + " BPM, fase a " + juce::String (d * 1000.0, 1) + " ms");
    }

    std::cout << "== AUTOMIX" << std::endl;
    {
        AudioEngine eng;
        eng.prepareForTest (sr, block);
        std::vector<float> l (block), r (block), l3 (block), r3 (block);
        float* outs[4] = { l.data(), r.data(), l3.data(), r3.data() };
        auto mkTrack = [] (const LoadedTrackPtr& lt) { auto t = std::make_shared<Track>(); t->file = lt->file; t->bpm = lt->grid().bpm; return t; };
        const auto tA = mkTrack (ta), tB = mkTrack (tb);
        AutoMix am (eng, { [&] (const TrackPtr& t, int d) { eng.deck (d).setTrack (t->file == ta->file ? ta : tb); } });
        am.setMixBeats (8);
        am.add ({ tA, tB });
        am.setRunning (true);

        const double step = 0.05;
        double t = 0.0, mixAt = -1.0, mixPosA = -1.0, longestGap = 0.0, gap = 0.0;
        bool both = false, started = false;
        while (t < 75.0)
        {
            for (int k = 0; k < (int) (step * sr / block); ++k)
            {
                eng.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
                float pk = 0.0f;
                for (int i = 0; i < block; ++i) pk = juce::jmax (pk, std::abs (l[(size_t) i]));
                if (started) { gap = pk < 0.01f ? gap + (double) block / sr : 0.0; longestGap = juce::jmax (longestGap, gap); }
                started |= pk > 0.05f;
            }
            am.tick();
            t += step;
            if (eng.deck (0).isPlaying() && eng.deck (1).isPlaying() && ! both)
            {
                both = true; mixAt = t; mixPosA = eng.deck (0).getPosition();
            }
            if (both && ! am.isMixing() && am.onAirDeck() == 1 && t > mixAt + 1.0) break;
        }
        const double endA = ta->lengthSeconds(), beat = 60.0 / ta->grid().bpm;
        const double before = (endA - mixPosA) / beat;
        rep.check (both && am.onAirDeck() == 1 && ! eng.deck (0).isPlaying() && eng.deck (1).isPlaying()
                   && eng.crossfader.load() > 0.99f,
                   "AUTOMIX: pasa del tema 1 al 2 sola (cruce con crossfader, el viejo se detiene)",
                   "mezcla a " + juce::String (before, 1) + " beats del final");
        rep.check (before > 6.0 && before < 10.5, "AUTOMIX: la mezcla empieza a 8 beats del final (largo elegido)");
        rep.check (longestGap < 0.1, "AUTOMIX: nunca se corta el sonido durante el pase",
                   "silencio máximo " + juce::String (longestGap * 1000.0, 0) + " ms");
        rep.check (eng.channel (0).eqLow.load() == 0.5f && eng.channel (1).eqLow.load() == 0.5f,
                   "AUTOMIX: los graves vuelven a su lugar después del cambio de bajos");
        am.setRunning (false);
    }

    std::cout << "== Sync" << std::endl;
    {
        AudioEngine eng;
        eng.prepareForTest (sr, block);
        auto& a = eng.deck (0);
        auto& b = eng.deck (1);
        a.setTrack (ta);
        b.setTrack (tb);
        b.setPitchRange (0.16);
        b.setSync (true);                 // engaged while the leader is still stopped
        std::vector<float> l (block), r (block), l3 (block), r3 (block);
        float* outs[4] = { l.data(), r.data(), l3.data(), r3.data() };
        auto runEngine = [&] (double seconds)
        {
            const int blocks = (int) (seconds * sr / block);
            for (int k = 0; k < blocks; ++k)
                eng.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
        };
        a.seek (ta->grid().timeOfBeat (8) + 0.013);
        b.seek (tb->grid().timeOfBeat (20) + 0.171);    // deliberately out of phase
        a.setPlaying (true);
        runEngine (1.0);
        b.setPlaying (true);
        auto phaseErrMs = [&]
        {
            double worst = 0.0;
            for (int k = 0; k < 20; ++k)
            {
                runEngine (0.05);
                const double fa = ta->grid().beatAt (a.getPosition()), fb = tb->grid().beatAt (b.getPosition());
                double dlt = (fa - std::floor (fa)) - (fb - std::floor (fb));
                if (dlt > 0.5) dlt -= 1.0;
                if (dlt < -0.5) dlt += 1.0;
                worst = juce::jmax (worst, std::abs (dlt) * 60000.0 / 124.0);
            }
            return worst;
        };
        runEngine (0.5);
        const double e1 = phaseErrMs();
        rep.check (std::abs (b.getEffectiveBpm() - a.getEffectiveBpm()) < 0.01, "Sync iguala el tempo (124 ← 128)"_u,
                   juce::String (b.getEffectiveBpm(), 3));
        rep.check (e1 < 5.0, "Sync alinea la fase al arrancar (aunque se activó con el otro deck parado)"_u, juce::String (e1, 1) + " ms");
        runEngine (20.0);
        const double e2 = phaseErrMs();
        rep.check (e2 < 5.0, "Fase sigue enganchada tras 20 s", juce::String (e2, 1) + " ms");
        b.setKeyLock (true);
        runEngine (10.0);
        const double e3 = phaseErrMs();
        rep.check (e3 < 8.0, "Fase enganchada con key lock", juce::String (e3, 1) + " ms");

        // SYNC engaged with KEY LOCK already on (reported: the track went mute).
        {
            AudioEngine eng2;
            eng2.prepareForTest (sr, block);
            auto& x = eng2.deck (0);
            auto& y = eng2.deck (1);
            auto toneB = sineTrack (500.0, 48000.0, 60.0, 0.5f);      // continuous: any silent block is a bug
            toneB->setGrid ({ 128.0, 0.0 });
            x.setTrack (ta); y.setTrack (toneB);
            y.setPitchRange (0.16);
            y.setKeyLock (true);
            x.setPlaying (true);
            y.seek (toneB->grid().timeOfBeat (12) + 0.2);
            y.setPlaying (true);
            std::vector<float> ol (block), orr (block);
            float* o2[2] = { ol.data(), orr.data() };
            eng2.channel (0).fader = 0.0f;        // listen to deck 2 only
            for (int k = 0; k < 100; ++k) eng2.audioDeviceIOCallbackWithContext (nullptr, 0, o2, 2, block, {});
            y.setSync (true);
            double energy = 0; int silentBlocks = 0;
            for (int k = 0; k < 300; ++k)
            {
                eng2.audioDeviceIOCallbackWithContext (nullptr, 0, o2, 2, block, {});
                double bl = 0; for (int i = 0; i < block; ++i) bl += (double) ol[(size_t) i] * ol[(size_t) i];
                energy += bl;
                if (k > 20 && bl / block < 1e-4) ++silentBlocks;
            }
            rep.check (silentBlocks == 0 && energy > 0.0, "SYNC con key lock activado no mutea el tema",
                       juce::String (silentBlocks) + " bloques en silencio");
        }

        // SYNC pressed on the deck that is ON AIR: it keeps its tempo, the incoming deck adapts.
        {
            AudioEngine e4;
            e4.prepareForTest (sr, block);
            auto& onAir = e4.deck (0);
            auto& incoming = e4.deck (1);
            onAir.setTrack (ta);                         // 124 BPM, playing
            incoming.setTrack (tb);                      // 128 BPM, just loaded
            incoming.setPitchRange (0.16);
            onAir.setPlaying (true);
            std::vector<float> ql (block), qr (block);
            float* q2[2] = { ql.data(), qr.data() };
            for (int k = 0; k < 50; ++k) e4.audioDeviceIOCallbackWithContext (nullptr, 0, q2, 2, block, {});
            onAir.setSync (true);                        // pressed on the playing deck
            incoming.setSync (true);
            for (int k = 0; k < 50; ++k) e4.audioDeviceIOCallbackWithContext (nullptr, 0, q2, 2, block, {});
            incoming.setPlaying (true);
            for (int k = 0; k < 200; ++k) e4.audioDeviceIOCallbackWithContext (nullptr, 0, q2, 2, block, {});
            rep.check (std::abs (onAir.getPitch()) < 1e-12 && std::abs (incoming.getEffectiveBpm() - 124.0) < 0.01,
                       "SYNC nunca cambia el tempo del tema que está sonando; se acomoda el que entra"_u,
                       "al aire " + juce::String (onAir.getEffectiveBpm(), 2) + " · entrante "_u + juce::String (incoming.getEffectiveBpm(), 2));
        }

        // PLAY on a synced deck starts already in phase (no audible settling), and a manual
        // nudge while synced is kept, not undone.
        {
            AudioEngine e5;
            e5.prepareForTest (sr, block);
            auto& lead = e5.deck (0);
            auto& fol = e5.deck (1);
            lead.setTrack (ta); fol.setTrack (tb);
            fol.setPitchRange (0.16);
            lead.setPlaying (true);
            std::vector<float> zl (block), zr (block);
            float* z2[2] = { zl.data(), zr.data() };
            auto runE = [&] (int k) { for (int j = 0; j < k; ++j) e5.audioDeviceIOCallbackWithContext (nullptr, 0, z2, 2, block, {}); };
            auto phaseMs = [&]
            {
                const double fa = ta->grid().beatAt (lead.audioPositionSeconds()), fb = tb->grid().beatAt (fol.audioPositionSeconds());
                double dd = (fa - std::floor (fa)) - (fb - std::floor (fb));
                if (dd > 0.5) dd -= 1.0;
                if (dd < -0.5) dd += 1.0;
                return dd * 60000.0 / 124.0;
            };
            runE (30);
            fol.seek (tb->grid().timeOfBeat (30) + 0.21);
            fol.setSync (true);
            runE (5);
            fol.setPlaying (true);
            runE (1);                                         // the very first audible block
            const double first = phaseMs();
            rep.check (std::abs (first) < 1.0, "PLAY con SYNC arranca ya en fase (sin ir acomodándose)"_u, juce::String (first, 2) + " ms");

            fol.jogBend (0.5);                                // DJ nudges forward for ~0.2 s
            runE (20);
            fol.jogBend (0.0);
            runE (5);
            const double afterNudge = phaseMs();
            runE (500);                                       // ≈ 5 s later
            const double later = phaseMs();
            rep.check (std::abs (afterNudge) > 5.0 && std::abs (later - afterNudge) < 1.0,
                       "Con SYNC, una corrección manual se respeta (no se vuelve a sincronizar)"_u,
                       juce::String (afterNudge, 1) + " ms → "_u + juce::String (later, 1) + " ms");
        }

        // Heard result, not positions: A has a click on every beat (left only), B on every beat (right
        // only). Whatever way B is launched with SYNC on, its very first beat must already coincide with
        // A's and stay there — no audible settling.
        {
            auto clickTrack = [] (double bpm, double first, int channel)
            {
                auto t = std::make_shared<LoadedTrack>();
                t->sampleRate = sr;
                t->audio.setSize (2, (int) (sr * 90));
                t->audio.clear();
                for (int bt = 0;; ++bt)
                {
                    const int at = (int) std::round ((first + bt * 60.0 / bpm) * sr);
                    if (at + 64 >= t->audio.getNumSamples()) break;
                    for (int i = 0; i < 48; ++i)
                        t->audio.setSample (channel, at + i, 0.8f * std::sin ((float) i * 0.9f) * (1.0f - (float) i / 48.0f));
                }
                t->setGrid ({ bpm, first });
                return t;
            };
            auto ca = clickTrack (124.0, 0.05, 0), cb = clickTrack (128.0, 0.137, 1);
            auto onsets = [] (const std::vector<float>& x)
            {
                std::vector<size_t> o;
                size_t lastLoud = 0; bool any = false;
                for (size_t i = 0; i < x.size(); ++i)
                    if (std::abs (x[i]) > 0.02f)
                    {
                        if (! any || i - lastLoud > 3000) o.push_back (i);
                        lastLoud = i; any = true;
                    }
                return o;
            };
            struct Launch { const char* name; int how; bool keyLock; double leadPitch; };
            juce::String bad;
            double worstFirst = 0.0, worstAny = 0.0;
            for (auto L : { Launch { "PLAY", 0, true, 0.0 }, Launch { "PLAY sin key lock", 0, false, 0.0 },
                            Launch { "PLAY con el otro a +3 %", 0, true, 0.03 }, Launch { "HOT CUE", 1, true, 0.0 },
                            Launch { "SYNC con el tema andando", 2, true, 0.0 } })   // CUE en pausa no sincroniza (golpes sueltos desde el cue): test aparte
            {
                AudioEngine e6;
                e6.prepareForTest (sr, block);
                e6.autoGain = false;
                e6.limiter().enabled = false;
                auto& da = e6.deck (0);
                auto& db = e6.deck (1);
                da.setKeyLock (L.keyLock); db.setKeyLock (L.keyLock);
                da.setTrack (ca); db.setTrack (cb);
                db.setPitchRange (0.16);
                da.setPitch (L.leadPitch);
                da.seek (3.0);
                da.setPlaying (true);
                std::vector<float> zl (block), zr (block), capL, capR;
                float* z2[2] = { zl.data(), zr.data() };
                auto runE = [&] (int k) { for (int j = 0; j < k; ++j) { e6.audioDeviceIOCallbackWithContext (nullptr, 0, z2, 2, block, {});
                                                                        capL.insert (capL.end(), zl.begin(), zl.end()); capR.insert (capR.end(), zr.begin(), zr.end()); } };
                runE (100);
                const double startAt = cb->grid().timeOfBeat (40) + 0.213;     // off-beat on purpose
                if (L.how == 2)
                {
                    db.seek (startAt);
                    db.setPlaying (true);
                    runE (37);
                    capL.clear(); capR.clear();
                    db.setSync (true);
                }
                else
                {
                    db.setSync (true);
                    db.seek (startAt);
                    if (L.how == 1) { db.hotCue (0); db.seek (startAt - 1.7); runE (3); }
                    if (L.how == 3) { runE (3); db.cueDown(); runE (2); db.cueUp(); }   // sets the cue point here
                    runE (13);
                    capL.clear(); capR.clear();
                    if (L.how == 0) db.setPlaying (true);
                    else if (L.how == 1) db.hotCue (0);
                    else { db.cueDown(); }
                }
                runE ((int) (6.0 * sr / block));
                if (L.how == 3) db.cueUp();
                const auto oa = onsets (capL), ob = onsets (capR);
                double first = 1.0e9, worst = 0.0;
                for (size_t k = 0; k < ob.size(); ++k)
                {
                    double best = 1.0e9;
                    for (auto oaK : oa) if (std::abs ((double) oaK - (double) ob[k]) < std::abs (best)) best = (double) ob[k] - (double) oaK;
                    const double ms = best / sr * 1000.0;
                    if (k == 0) first = ms;
                    worst = juce::jmax (worst, std::abs (ms));
                }
                if (ob.empty() || std::abs (first) > 1.5 || worst > 1.5)
                    bad << L.name << " (1.er beat " << juce::String (first, 1) << " ms, peor " << juce::String (worst, 1) << " ms) ";
                worstFirst = juce::jmax (worstFirst, std::abs (first));
                worstAny = juce::jmax (worstAny, worst);
            }
            rep.check (bad.isEmpty(), "SYNC: el tema sale ya en fase desde el primer beat que se oye (PLAY, HOT CUE, SYNC)"_u,
                       bad.isEmpty() ? juce::String (worstFirst, 2) + " ms el 1.er beat · peor "_u + juce::String (worstAny, 2) + " ms" : bad);
        }

        // Loading a new track with SYNC on: SYNC stays on and the new track takes the right tempo at
        // once (following the deck on air); with nothing to follow it plays at its own tempo.
        {
            AudioEngine e7;
            e7.prepareForTest (sr, block);
            auto& da = e7.deck (0);
            auto& db = e7.deck (1);
            std::vector<float> zl (block), zr (block);
            float* z2[2] = { zl.data(), zr.data() };
            auto runE = [&] (int k) { for (int j = 0; j < k; ++j) e7.audioDeviceIOCallbackWithContext (nullptr, 0, z2, 2, block, {}); };
            auto t100 = sineTrack (200.0, 48000.0, 20.0, 0.3f); t100->setGrid ({ 100.0, 0.0 });
            auto t110 = sineTrack (300.0, 48000.0, 20.0, 0.3f); t110->setGrid ({ 110.0, 0.0 });
            da.setTrack (ta); db.setTrack (tb);
            db.setPitchRange (0.5);
            da.setPlaying (true);
            db.setSync (true);
            runE (5);
            db.setTrack (t100);                               // new track while SYNC is on
            runE (2);
            const double followPitch = db.getPitch();
            const bool stillSync = db.isSyncOn();
            da.setPlaying (false);
            da.setTrack (nullptr);                            // the other deck empty: nothing to follow
            runE (5);
            db.setTrack (t110);
            runE (2);
            const double ownPitch = db.getPitch();
            rep.check (stillSync && std::abs (followPitch - (124.0 / 100.0 - 1.0)) < 1.0e-6 && std::abs (ownPitch) < 1.0e-9,
                       "Cargar con SYNC: sigue en SYNC y toma el tempo correcto al instante (o el suyo si no hay a quién seguir)"_u,
                       "sigue " + juce::String (followPitch * 100.0, 2) + " % · solo " + juce::String (ownPitch * 100.0, 2) + " %");
        }

        // Loops with SYNC (reported: with a ½ or ¼ beat loop, sync kept retiming and the other track
        // was cut). A loop on either deck must never make sync move a playhead that is being heard.
        {
            juce::String bad;
            int worstJumps = 0;
            for (double loopBeats : { 0.5, 0.25, 4.0 })
                for (bool loopOnLeader : { true, false })
                {
                    AudioEngine e8;
                    e8.prepareForTest (sr, block);
                    auto& da = e8.deck (0);
                    auto& db = e8.deck (1);
                    da.setTrack (ta); db.setTrack (tb);
                    db.setPitchRange (0.16);
                    std::vector<float> zl (block), zr (block);
                    float* z2[2] = { zl.data(), zr.data() };
                    auto runE = [&] (int k) { for (int j = 0; j < k; ++j) e8.audioDeviceIOCallbackWithContext (nullptr, 0, z2, 2, block, {}); };
                    da.seek (ta->grid().timeOfBeat (16) + 0.01);
                    da.setPlaying (true);
                    db.setSync (true);
                    db.seek (tb->grid().timeOfBeat (32) + 0.2);
                    runE (20);
                    db.setPlaying (true);
                    runE (100);                                        // in phase, both on air

                    auto& looper = loopOnLeader ? da : db;
                    looper.setAutoLoop (loopBeats);
                    runE (2);
                    const double loopLen = looper.getLoopEnd() - looper.getLoopStart();
                    auto& watched = db;                                // the follower: the deck sync retimes
                    const double step = (double) block / sr * (1.0 + db.getPitch());   // seconds of track per block

                    // Every block the follower advances by `step`, or (if it is the one looping) wraps by
                    // one loop length. Anything else is sync moving a playhead that is being heard.
                    int jumps = 0;
                    double prev = watched.audioPositionSeconds();
                    auto watch = [&] (int blocks, bool looping)
                    {
                        for (int k = 0; k < blocks; ++k)
                        {
                            runE (1);
                            const double now = watched.audioPositionSeconds();
                            const double moved = now - prev;
                            const bool advance = std::abs (moved - step) < 0.002;
                            const bool wrap = looping && std::abs (moved - step + loopLen) < 0.003;
                            if (! advance && ! wrap) ++jumps;
                            prev = now;
                        }
                    };
                    watch ((int) (4.0 * sr / block), ! loopOnLeader);
                    looper.exitLoop();
                    watch ((int) (3.0 * sr / block), false);            // after the loop: no catch-up jump either

                    worstJumps = juce::jmax (worstJumps, jumps);
                    if (jumps > 0)
                        bad << "loop " << juce::String (loopBeats) << " en " << (loopOnLeader ? "el que suena" : "el que entra")
                            << ": " << jumps << " saltos · ";
                }
            rep.check (bad.isEmpty(), "Con SYNC, un loop (4, ½ o ¼) en cualquier deck no corta ni corre el tema que suena"_u,
                       bad.isEmpty() ? juce::String ("0 saltos") : bad);
        }

        // Master deck: with B as master, A follows B even though A was synced first.
        a.setSync (true);
        eng.masterDeck = 1;
        b.setSync (false);
        b.setPitch (0.02);
        runEngine (0.5);
        rep.check (std::abs (a.getEffectiveBpm() - b.getEffectiveBpm()) < 0.01 && std::abs (b.getPitch() - 0.02) < 1e-9,
                   "Deck maestro: el otro deck lo sigue", juce::String (a.getEffectiveBpm(), 3) + " / " + juce::String (b.getEffectiveBpm(), 3));

        // CUE on a paused synced deck: the stab starts exactly on the cue point (no alignment);
        // PLAY afterwards aligns as usual.
        {
            eng.masterDeck = -1;
            a.setSync (false);
            a.setPlaying (true);
            b.setSync (true);
            b.setPlaying (false);
            runEngine (0.3);
            const double cueAt = tb->grid().timeOfBeat (16) + 0.137;     // deliberately off the leader's phase
            b.seek (cueAt);
            runEngine (0.1);
            b.cueDown();                                                  // sets the cue point here
            runEngine (0.05);
            b.cueDown();                                                  // held: sounds from the cue
            runEngine (0.2);
            const double heard = b.audioPositionSeconds();
            const double expected = b.getCuePoint() + 0.2 * (1.0 + b.getPitch());   // the cue as set (quantized)
            b.cueUp();
            runEngine (0.1);
            const bool stab = std::abs (heard - expected) < 0.03;

            b.setPlaying (true);
            runEngine (0.3);
            const auto ag = ta->grid(), bg = tb->grid();
            const double pa = ag.beatAt (a.audioPositionSeconds()), pb = bg.beatAt (b.audioPositionSeconds());
            double ph = (pa - std::floor (pa)) - (pb - std::floor (pb));
            if (ph > 0.5) ph -= 1.0;
            if (ph < -0.5) ph += 1.0;
            rep.check (stab && std::abs (ph) < 0.02,
                       "CUE en pausa suena desde el cue sin sincronizar; PLAY sí alinea"_u,
                       "desvío del golpe " + juce::String ((heard - expected) * 1000.0, 1) + " ms · fase al dar PLAY "
                       + juce::String (ph * 100.0, 1) + " % de beat");
            b.setPlaying (false);
            a.setPlaying (false);
        }
    }

    std::cout << "== MIDI" << std::endl;
    Actions actions (engine, {});
    MidiEngine midi (actions);
    std::vector<MidiBinding> b;
    MidiBinding play; play.device = "Test"; play.channel = 1; play.kind = MidiBinding::Kind::note; play.number = 11; play.action = "play_pause"; play.deck = 1;
    MidiBinding vol;  vol.device = "Test"; vol.channel = 1; vol.kind = MidiBinding::Kind::cc; vol.number = 19; vol.encoding = MidiBinding::Encoding::absolute7; vol.action = "volume"; vol.deck = 1;
    MidiBinding jog;  jog.device = "Test"; jog.channel = 2; jog.kind = MidiBinding::Kind::cc; jog.number = 33; jog.encoding = MidiBinding::Encoding::relativeOffset; jog.action = "jog"; jog.deck = 1; jog.scale = 1.0f / 512.0f;
    MidiBinding shift; shift.device = "Test"; shift.channel = 1; shift.kind = MidiBinding::Kind::note; shift.number = 63; shift.action = "shift"; shift.deck = -1;
    MidiBinding shPlay = play; shPlay.shift = true; shPlay.action = "reverse";
    b = { play, vol, jog, shift, shPlay };
    midi.setBindings (b);

    auto& d2 = engine.deck (1);
    d2.prepare (sr);
    d2.setTrack (tb);

    midi.inject ("Test", juce::MidiMessage::noteOn (1, 11, (juce::uint8) 127));
    midi.inject ("Test", juce::MidiMessage::noteOff (1, 11));
    rep.check (d2.isPlaying(), "Nota → play_pause en deck 2"_u);

    engine.channel (1).fader = 1.0f;
    midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 19, 30));
    rep.check (std::abs (engine.channel (1).fader.load() - 1.0f) < 1e-6f, "Soft takeover: fader lejos no salta");
    midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 19, 127));
    midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 19, 64));
    rep.check (std::abs (engine.channel (1).fader.load() - 64.0f / 127.0f) < 0.01f, "Fader tomado sigue el control", juce::String (engine.channel (1).fader.load(), 3));

    d2.setPlaying (false);
    run (d2, 0.05);
    const double jp = d2.getPosition();
    for (int i = 0; i < 64; ++i) midi.inject ("Test", juce::MidiMessage::controllerEvent (2, 33, 65)); // +64 ticks = 1/8 rev
    rep.check (std::abs (d2.getPosition() - (jp + 1.8 / 8.0)) < 0.02, "Jog relativo (offset 64) mueve 1/8 de vuelta", juce::String (d2.getPosition() - jp, 3));

    midi.inject ("Test", juce::MidiMessage::noteOn (1, 63, (juce::uint8) 127));
    midi.inject ("Test", juce::MidiMessage::noteOn (1, 11, (juce::uint8) 127));
    rep.check (d2.isReverse() && ! d2.isPlaying(), "SHIFT + botón dispara la capa shift"_u);
    midi.inject ("Test", juce::MidiMessage::noteOff (1, 63));

    {
        // Pitch fader: down (low values) = faster, like the screen and club players.
        MidiBinding pf; pf.device = "Test"; pf.channel = 1; pf.kind = MidiBinding::Kind::cc; pf.number = 0;
        pf.encoding = MidiBinding::Encoding::absolute7; pf.action = "pitch"; pf.deck = 1;
        midi.setBindings ({ pf });
        d2.setSync (false);
        d2.setPitch (0.0);
        for (int v : { 64, 48, 32, 16, 0 }) midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 0, v));
        const double low = d2.getPitch();
        for (int v = 0; v <= 127; v += 16) midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 0, v));
        midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 0, 127));
        rep.check (low > 0.0 && d2.getPitch() < 0.0, "Pitch del controlador: abajo más rápido, arriba más lento"_u,
                   juce::String (low * 100.0, 1) + " % / " + juce::String (d2.getPitch() * 100.0, 1) + " %");
        d2.setPitch (0.0);
        midi.setBindings (b);
    }

    std::cout << "== Luces del controlador" << std::endl;
    {
        struct Sent { int status, number, value; };
        std::vector<Sent> sent;
        midi.onOutput = [&sent] (const juce::String&, const juce::MidiMessage& m)
        {
            sent.push_back ({ m.getRawData()[0], m.getRawData()[1], m.getRawData()[2] });
        };
        auto valuesOf = [&sent] (int status, int number)
        {
            std::set<int> v;
            for (auto& s : sent) if (s.status == status && s.number == number) v.insert (s.value);
            return v;
        };

        MidiBinding cue; cue.device = "Test"; cue.channel = 1; cue.kind = MidiBinding::Kind::note; cue.number = 12; cue.action = "cue_button"; cue.deck = 1;
        cue.outChannel = 3; cue.outNumber = 50; cue.outKind = MidiBinding::Kind::note; cue.onValue = 5; cue.offValue = 1;
        MidiBinding vu; vu.device = "Test"; vu.channel = 1; vu.kind = MidiBinding::Kind::cc; vu.number = 2; vu.action = "vu_meter"; vu.deck = 1;
        vu.encoding = MidiBinding::Encoding::absolute7; vu.outputOnly = true;
        MidiBinding hc; hc.device = "Test"; hc.channel = 1; hc.kind = MidiBinding::Kind::note; hc.number = 20; hc.action = "hot_cue_1"; hc.deck = 1;
        MidiBinding hc2 = hc; hc2.action = "hot_cue_2";                        // same pad bound twice: no flicker
        b = { play, cue, vu, hc, hc2 };
        midi.setBindings (b);

        d2.setReverse (false);
        d2.setPlaying (true);
        midi.tick();
        rep.check (valuesOf (0x90, 11) == std::set<int> { 127 }, "PLAY encendido mientras suena");

        d2.setPlaying (false);
        d2.seek (10.0);
        run (d2, 0.05);
        sent.clear();
        for (int i = 0; i < 14; ++i) { midi.tick(); juce::Thread::sleep (50); }
        rep.check (valuesOf (0x90, 11) == std::set<int> { 0, 127 }, "PLAY parpadea con el tema en pausa"_u);
        rep.check (valuesOf (0x92, 50) == std::set<int> { 1, 5 }, "CUE parpadea fuera del punto de cue, en su propia nota y valores"_u);

        d2.clearHotCue (0);
        d2.clearHotCue (1);
        sent.clear();
        midi.tick();
        d2.hotCue (1); d2.hotCueRelease (1);                                  // only hot cue 2 set
        for (int i = 0; i < 4; ++i) midi.tick();
        const auto pad = valuesOf (0x90, 20);
        rep.check (pad.size() <= 1 && ! pad.count (127), "Un pad con dos funciones no titila entre ellas", juce::String ((int) pad.size()) + " valores");

        std::vector<float> loud (512, 0.9f);
        engine.channel (1).meter.push (loud.data(), loud.data(), 512);
        sent.clear();
        midi.tick();
        const auto vuNow = valuesOf (0xB0, 2);
        rep.check (! vuNow.empty() && *vuNow.rbegin() > 110, "VU del canal en el controlador", vuNow.empty() ? juce::String ("nada") : juce::String (*vuNow.rbegin()));
        bool vuTriggered = false;
        midi.onDispatch = [&vuTriggered] (const MidiBinding& x, int, float) { if (x.action == "vu_meter") vuTriggered = true; };
        midi.inject ("Test", juce::MidiMessage::controllerEvent (1, 2, 100));
        midi.onDispatch = nullptr;
        rep.check (! vuTriggered, "Una luz sin control no reacciona a mensajes entrantes");

        sent.clear();
        midi.allLightsOff();
        bool allOff = ! sent.empty();
        for (auto& x : sent) allOff &= (x.status == 0x92 ? x.value == 1 : x.value == 0);
        rep.check (allOff, "Al cerrar se apagan todas las luces", juce::String ((int) sent.size()) + " mensajes");

        const auto json = juce::JSON::toString (cue.toVar());
        const auto back = MidiBinding::fromVar (juce::JSON::parse (json));
        rep.check (back.lightNumber() == 50 && back.lightChannel() == 3 && back.onValue == 5 && back.offValue == 1
                   && MidiBinding::fromVar (vu.toVar()).outputOnly, "La salida de luz se guarda en el mapeo");

        // Importers: a table whose light answers on another message, and a "track loaded" light.
        const auto table = MappingImport::fromCsv ("PlayPause,PlayPause,Button,900B,0,1,,,9040,0,1,\n"
                                                   "LoadedIndicator,LoadedIndicator,Indicator,,,,,,,9B00,9B01,\n"
                                                   "ChannelLevel,ChannelLevel,Indicator,,,,,,B002,0,1,\n");
        int lights = 0, meters = 0, outs = 0;
        for (auto& x : table.bindings)
        {
            if (x.action == "play_pause" && x.lightNumber() == 0x40) ++outs;
            if (x.action == "load" && x.outputOnly && x.channel == 12) ++lights;
            if (x.action == "vu_meter" && x.outputOnly && x.kind == MidiBinding::Kind::cc && x.number == 2) ++meters;
        }
        rep.check (outs == 2 && lights == 2 && meters == 2, "Tabla CSV: luces en otra nota, indicador de pista cargada y VU",
                   juce::String (outs) + " / " + juce::String (lights) + " / " + juce::String (meters));

        const auto preset = MappingImport::fromPreset (R"(<MixxxControllerPreset><controller><controls>
            <control><group>[Channel1]</group><key>play</key><status>0x90</status><midino>0x0B</midino></control>
            </controls><outputs>
            <output><group>[Channel1]</group><key>play_indicator</key><status>0x91</status><midino>0x0B</midino><on>0x7E</on><off>0x01</off></output>
            <output><group>[Channel1]</group><key>end_of_track</key><status>0x90</status><midino>0x30</midino></output>
            </outputs></controller></MixxxControllerPreset>)");
        bool playLight = false, endLight = false;
        for (auto& x : preset.bindings)
        {
            playLight |= x.action == "play_pause" && x.lightChannel() == 2 && x.onValue == 0x7E && x.offValue == 1;
            endLight |= x.action == "end_warning" && x.outputOnly && x.number == 0x30;
        }
        rep.check (playLight && endLight, "Preset XML: luces de <outputs> (otro canal, valores, aviso de fin)");
        midi.onOutput = nullptr;
    }

    std::cout << "== Controladores plug and play" << std::endl;
    {
        // A library like the one on the site: index + one mapping file, served from a local folder.
        const auto site = juce::File::createTempFile ("om-ctl-site");
        site.createDirectory();
        auto entry = std::make_unique<juce::DynamicObject>();
        entry->setProperty ("id", "test-x");
        entry->setProperty ("name", "Controlador de prueba X");
        entry->setProperty ("file", "test-x.midi.json");
        auto match = std::make_unique<juce::DynamicObject>();
        match->setProperty ("midi", juce::Array<juce::var> { "Test Controller X" });
        entry->setProperty ("match", juce::var (match.release()));
        const auto driverMarker = juce::File::createTempFile ("om-driver-installed");
        auto drv = std::make_unique<juce::DynamicObject>();
        drv->setProperty ("page", "https://example.com/drivers/x");
        drv->setProperty ("detect", juce::Array<juce::var> { driverMarker.getFullPathName() });
        auto drivers = std::make_unique<juce::DynamicObject>();
        const juce::var drvVar (drv.release());
        for (auto* os : { "windows", "mac", "linux" }) drivers->setProperty (os, drvVar);
        entry->setProperty ("driver", juce::var (drivers.release()));
        auto evil = std::make_unique<juce::DynamicObject>();                       // must be ignored
        evil->setProperty ("file", "../../escape.json");
        evil->setProperty ("match", juce::JSON::parse (R"({"midi":["Unknown Box"]})"));
        auto root = std::make_unique<juce::DynamicObject>();
        root->setProperty ("version", 2);
        root->setProperty ("mappings", juce::Array<juce::var> { juce::var (entry.release()), juce::var (evil.release()) });
        site.getChildFile ("index.json").replaceWithText (juce::JSON::toString (juce::var (root.release())));
        MidiBinding p; p.device = "*"; p.channel = 1; p.kind = MidiBinding::Kind::note; p.number = 11; p.action = "play_pause"; p.deck = 0;
        auto mapping = std::make_unique<juce::DynamicObject>();
        mapping->setProperty ("version", 1);
        mapping->setProperty ("bindings", juce::Array<juce::var> { p.toVar() });
        site.getChildFile ("test-x.midi.json").replaceWithText (juce::JSON::toString (juce::var (mapping.release())));
        ControllerLibrary::setIndexUrlOverride (juce::URL (site.getChildFile ("index.json")));
        Library::dataFolder().getChildFile ("controllers").deleteRecursively();

        rep.check (ControllerLibrary::nameMatches ("2- Test Controller X MIDI 1", "Test Controller X")
                   && ControllerLibrary::nameMatches ("TEST CONTROLLER X", "Test Controller X")
                   && ! ControllerLibrary::nameMatches ("Test Controller X2", "Test Controller X")
                   && ControllerLibrary::nameMatches ("Test Controller X MK2", "Test Controller X *")
                   && ! ControllerLibrary::nameMatches ("Model X-200", "Model X-400") && ! ControllerLibrary::nameMatches ("Board 200", "Board 400")
                   && ControllerLibrary::nameMatches ("Model X-400 MIDI 1", "Model X-400"),
                   "Reconoce el modelo aunque cambie el puerto o el prefijo del sistema (y no confunde modelos parecidos)");

        ControllerLibrary lib (midi);
        const auto before = midi.bindings().size();
        const auto dev = lib.resolveNow ("Test Controller X MIDI 1");
        const bool installed = dev.status == ControllerLibrary::Status::mapped && midi.hasBindingsFor ("Test Controller X MIDI 1")
                            && midi.bindings().size() == before + 1;
        rep.check (installed, "Al enchufar un controlador conocido, su mapeo se instala solo", dev.profileName);

        auto& d1 = engine.deck (0);
        d1.prepare (sr);
        d1.setTrack (ta);
        d1.setPlaying (false);
        midi.inject ("Test Controller X MIDI 1", juce::MidiMessage::noteOn (1, 11, (juce::uint8) 127));
        midi.inject ("Test Controller X MIDI 1", juce::MidiMessage::noteOff (1, 11));
        rep.check (d1.isPlaying(), "El controlador funciona apenas se conecta (PLAY del mapeo instalado)");
        d1.setPlaying (false);

        const auto unknown = lib.resolveNow ("Unknown Box");
        rep.check (unknown.status == ControllerLibrary::Status::noMapping && ! midi.hasBindingsFor ("Unknown Box")
                   && ! site.getParentDirectory().getParentDirectory().getChildFile ("escape.json").existsAsFile(),
                   "Controlador desconocido: queda sin mapeo (y un índice malicioso no escribe fuera de la caché)"_u);

        auto own = midi.bindings();
        MidiBinding mine = p; mine.device = "My Own Box"; mine.number = 40;
        own.push_back (mine);
        midi.setBindings (own);
        const auto ownDev = lib.resolveNow ("My Own Box");
        rep.check (ownDev.status == ControllerLibrary::Status::mappedByUser, "Un mapeo hecho por el usuario nunca se reemplaza");

        // Driver: OpenMix only recommends the official page while it is missing, never downloads it.
        const auto pagesBefore = lib.missingDriverPages ({ "Test Controller X MIDI 1" });
        driverMarker.create();
        const auto pagesAfter = lib.missingDriverPages ({ "Test Controller X MIDI 1" });
        driverMarker.deleteFile();
        rep.check (pagesBefore == juce::StringArray { "https://example.com/drivers/x" } && pagesAfter.isEmpty()
                   && ! Library::dataFolder().getChildFile ("controllers").getChildFile ("drivers").exists(),
                   "Si falta el driver, recomienda la página oficial (y deja de hacerlo al instalarlo); no descarga nada"_u);

        // Built-in library: real controllers recognised by the names they announce, the right model each.
        {
            ControllerLibrary builtIn (midi);
            builtIn.setOnlineLookup (false);
            builtIn.setLocalImport (false);
            const auto inpulse = builtIn.resolveNow ("DJControl Inpulse 200");
            const auto ddj4 = builtIn.resolveNow ("DDJ-400 MIDI 1");
            const auto ddj2 = builtIn.resolveNow ("2- DDJ-200");
            const auto mt3 = builtIn.resolveNow ("Mixtrack Pro 3");
            const auto mtfx = builtIn.resolveNow ("Mixtrack Pro FX");
            rep.check (inpulse.profileName.contains ("Inpulse 200") && ddj4.profileName.contains ("DDJ-400") && ddj2.profileName.contains ("DDJ-200")
                       && mt3.profileName.contains ("(Pro) 3") && mtfx.profileName.contains ("Pro FX"),
                       "Biblioteca incluida: cada controlador se reconoce como su modelo exacto (400 ≠ 200, Pro 3 ≠ Pro FX)"_u,
                       inpulse.profileName + " · " + ddj4.profileName + " · " + ddj2.profileName);

            // Plug in the Inpulse 200 and press its PLAY: the deck starts, no setup.
            const MidiBinding* playB = nullptr;
            for (auto& mb : midi.bindings()) if (mb.device == "DJControl Inpulse 200" && mb.action == "play_pause" && mb.deck == 0 && ! mb.shift) { playB = &mb; break; }
            auto& dk = engine.deck (0);
            dk.setPlaying (false);
            if (playB != nullptr)
            {
                const auto msg = playB->kind == MidiBinding::Kind::note ? juce::MidiMessage::noteOn (playB->channel, playB->number, (juce::uint8) 127)
                                                                        : juce::MidiMessage::controllerEvent (playB->channel, playB->number, 127);
                midi.inject ("DJControl Inpulse 200", msg);
                midi.inject ("DJControl Inpulse 200", juce::MidiMessage::noteOff (playB->channel, playB->number));
            }
            rep.check (playB != nullptr && dk.isPlaying(), "Controlador de la biblioteca (Inpulse 200): se enchufa y PLAY funciona sin configurar nada");
            dk.setPlaying (false);
        }

        // Offline: the site is gone; a reconnect still works from the cache.
        site.deleteRecursively();
        lib.setOnlineLookup (false);
        ControllerLibrary again (midi);
        again.setOnlineLookup (false);
        const auto re = again.resolveNow ("Test Controller X MIDI 1");
        rep.check (re.status == ControllerLibrary::Status::mapped && re.profileName == "Controlador de prueba X",
                   "Sin internet, un controlador ya instalado se reconoce igual", re.profileName);
        ControllerLibrary::setIndexUrlOverride ({});
    }

    std::cout << "== Asistente de mapeo" << std::endl;
    {
        // A simulated controller: deck 1 on MIDI channel 1, deck 2 the same on channel 2.
        auto runWizard = [&actions] (MappingWizard& w, int deck2Channel, int deck2CueNote)
        {
            double t = 0.0;
            using M = juce::MidiMessage;
            auto press = [&] (int ch, int note) { w.feed (M::noteOn (ch, note, (juce::uint8) 127), t += 0.3); w.feed (M::noteOff (ch, note), t += 0.05); };
            auto turn = [&] (int ch, int cc, std::initializer_list<int> values) { for (int v : values) w.feed (M::controllerEvent (ch, cc, v), t += 0.02); w.poll (t += 1.0); };
            juce::ignoreUnused (actions);
            int guard = 0;
            while (w.phase() != MappingWizard::Phase::finished && ++guard < 200)
            {
                using P = MappingWizard::Phase;
                if (w.phase() == P::deckTwoProbe) { press (deck2Channel, 11); continue; }
                if (w.phase() == P::deckTwoCheck) { press (deck2Channel, deck2CueNote); continue; }
                const auto* st = w.current();
                if (st == nullptr) break;
                const int ch = w.phase() == P::deckTwoManual ? deck2Channel : 1;
                const auto a = st->action;
                if (a == "play_pause") press (ch, 11);
                else if (a == "cue_button") press (ch, 12);
                else if (a == "sync") press (ch, 13);
                else if (a.startsWith ("hot_cue_")) press (ch, 19 + a.getTrailingIntValue());
                else if (a == "loop") press (ch, 30);
                else if (a == "loop_half") press (ch, 31);
                else if (a == "loop_double") press (ch, 32);
                else if (a == "load") press (ch, 40);
                else if (a == "pfl") press (ch, 50);
                else if (a == "jog") turn (ch, 33, { 65, 65, 66, 63, 63, 62 });
                else if (a == "pitch") { for (int v = 0; v < 120; v += 8) { w.feed (M::controllerEvent (ch, 0, v), t += 0.01); w.feed (M::controllerEvent (ch, 32, 64), t += 0.01); } w.poll (t += 1.0); }
                else if (a == "gain") turn (ch, 4, { 60, 70, 80 });
                else if (a == "eq_high") turn (ch, 7, { 60, 70, 80 });
                else if (a == "eq_mid") turn (ch, 11, { 60, 70, 80 });
                else if (a == "eq_low") turn (ch, 15, { 60, 70, 80 });
                else if (a == "filter") turn (ch, 23, { 60, 70, 80 });
                else if (a == "volume") turn (ch, 19, { 20, 60, 100 });
                else if (a == "crossfader") turn (7, 31, { 10, 64, 120 });
                else if (a == "browser_scroll") turn (7, 64, { 1, 1, 127, 127 });
                else if (a == "shift") press (1, 63);
                else w.skip();                                   // master / headphones: this controller has none
            }
            return guard < 200;
        };

        MappingWizard w (actions, "Unmapped Box");
        const bool done = runWizard (w, 2, 12);
        const auto& out = w.bindings();
        auto find = [&out] (const char* action, int deck) -> const MidiBinding*
        {
            for (auto& mb : out) if (mb.action == action && mb.deck == deck) return &mb;
            return nullptr;
        };
        const int deck1 = (int) std::count_if (out.begin(), out.end(), [] (const MidiBinding& mb) { return mb.deck == 0; });
        const int deck2 = (int) std::count_if (out.begin(), out.end(), [] (const MidiBinding& mb) { return mb.deck == 1; });
        rep.check (done && w.phase() == MappingWizard::Phase::finished && deck1 == 24 && deck2 == deck1
                   && w.deckChannelOffset() == 1 && w.deckNumberOffset() == 0,
                   "Asistente: se mapea solo el deck izquierdo y el derecho se deduce (canal +1)",
                   juce::String (deck1) + " + " + juce::String (deck2) + " controles");

        const auto* jogB = find ("jog", 0);
        const auto* pitchB = find ("pitch", 1);
        const auto* browseB = find ("browser_scroll", -1);
        rep.check (jogB != nullptr && jogB->encoding == MidiBinding::Encoding::relativeOffset
                   && pitchB != nullptr && pitchB->encoding == MidiBinding::Encoding::absolute14 && pitchB->lsbNumber == 32 && pitchB->channel == 2
                   && browseB != nullptr && browseB->encoding == MidiBinding::Encoding::relativeTwos,
                   "Asistente: reconoce jog relativo, fader de 14 bits y encoder en complemento a dos",
                   (jogB ? juce::String ((int) jogB->encoding) : "-") + " / " + (pitchB ? juce::String ((int) pitchB->encoding) + " lsb " + juce::String (pitchB->lsbNumber) + " ch " + juce::String (pitchB->channel) : "-")
                   + " / " + (browseB ? juce::String ((int) browseB->encoding) : "-"));

        midi.installMapping ("Unmapped Box", out);
        auto& dB = engine.deck (1);
        dB.setPlaying (false);
        midi.inject ("Unmapped Box", juce::MidiMessage::noteOn (2, 11, (juce::uint8) 127));
        midi.inject ("Unmapped Box", juce::MidiMessage::noteOff (2, 11));
        rep.check (dB.isPlaying(), "El mapeo del asistente funciona en el deck derecho sin haberlo mapeado a mano");
        dB.setPlaying (false);

        // Decks that are not mirror images: the check fails and deck 2 is asked control by control.
        MappingWizard w2 (actions, "Odd Box");
        const bool done2 = runWizard (w2, 2, 99);
        const int odd2 = (int) std::count_if (w2.bindings().begin(), w2.bindings().end(), [] (const MidiBinding& mb) { return mb.deck == 1; });
        rep.check (done2 && odd2 == 24, "Si los decks no son espejo, el asistente pide el deck derecho control por control",
                   juce::String (odd2) + " controles del deck 2");
    }

    std::cout << "== Actualización automática" << std::endl;
    {
        rep.check (Updater::compareVersions ("1.10.0", "1.9.2") > 0 && Updater::compareVersions ("2.0", "2.0.0") == 0
                   && Updater::compareVersions ("v1.0.1", "1.0.0") > 0 && Updater::compareVersions ("1.0.0", "1.0.1") < 0,
                   "Compara versiones por número (1.10 > 1.9)"_u);

        // A published package (here: any local file) and its manifest.
        const auto dir = juce::File::createTempFile ("om-upd");
        dir.createDirectory();
        const auto pkg = dir.getChildFile ("OpenMix-9.9.9-test.bin");
        pkg.replaceWithText ("contenido de prueba");
        juce::FileInputStream hin (pkg);
        const auto sha = juce::SHA256 (hin).toHexString();
        auto manifest = [&] (const juce::String& hash)
        {
            const juce::String entry = R"({"file":")" + pkg.getFileName() + R"(","size":19,"sha256":")" + hash + R"(","url":")" + juce::URL (pkg).toString (false) + R"("})";
            return juce::JSON::parse (R"({"version":"9.9.9","notes":"x","assets":{"mac":)" + entry + R"(,"windows":)" + entry + R"(,"linux":)" + entry + "}}");
        };
        const auto good = Updater::parseManifest (manifest (sha));
        rep.check (good.has_value() && good->version == "9.9.9" && good->sha256 == sha && ! Updater::parseManifest (juce::JSON::parse ("{}")).has_value(),
                   "Lee el manifiesto de la versión publicada para este sistema"_u);

        Updater bad;
        bad.setAvailable (*Updater::parseManifest (manifest (juce::String::repeatedString ("0", 64))));
        rep.check (! bad.downloadNow() && bad.error().contains ("no coincide"),
                   "Una descarga alterada (huella distinta) se rechaza y no se instala"_u, bad.error());
        dir.deleteRecursively();
    }

    std::cout << "== Importar mapeos de otros programas" << std::endl;
    {
        // A CSV table in the same shape as the ones DJ software ships (fictional controller).
        const juce::String csv = juce::String::fromUTF8 (
            "@file,1,Test Board 2\n"
            "#name,function,type,input,deck1,deck2,deck3,deck4,output,deck1,deck2,deck3,deck4,option,comment\n"
            "Browse,Browse,Rotary,B640,,,,,,,,,,,\n"
            "Load,Load,Button,,9646,9647,,,,,,,,,\n"
            "Shift,Shift,Button,903F,0,1,,,,,,,,RO,\n"
            "PlayPause,PlayPause,Button,900B,0,1,,,900B,0,1,,,,\n"
            "PlayPause,PlayPause+Shift,Button,9047,0,1,,,,,,,,,\n"
            "Cue,Cue,Button,900C,0,1,,,900C,0,1,,,,\n"
            "Sync,Sync,Button,9058,0,1,,,,,,,,,\n"
            "JogScratch,JogScratch,JogRotate,B022,0,1,,,,,,,,RO,\n"
            "JogTouch,JogTouch,JogTouch,9036,0,1,,,,,,,,RO,\n"
            "TempoSlider,TempoSlider,KnobSliderHiRes,B000,0,1,,,,,,,,,\n"
            "ChannelFader,ChannelFader,KnobSliderHiRes,B013,0,1,,,,,,,,,\n"
            "EQHigh,EQHigh,KnobSliderHiRes,B007,0,1,,,,,,,,,\n"
            "CrossFader,CrossFader,KnobSliderHiRes,B61F,,,,,,,,,,,\n"
            "CFXParameterCH1,CFXParameterCH1,KnobSliderHiRes,B617,,,,,,,,,,,\n"
            "PAD1_HotCue,PAD1_PadMode1,Pad,9000,7,9,,,,,,,,,\n"
            "PAD1_HotCue,PAD1_PadMode1+Shift,Pad,9000,8,10,,,,,,,,,\n"
            "FX1-1On,FX1-1On,Button,9447,,,,,,,,,,,\n");
        const auto r = MappingImport::fromCsv (csv);
        auto has = [&r] (const char* act, int dk, int chn, int nm, bool sh)
        {
            return std::any_of (r.bindings.begin(), r.bindings.end(), [=] (const MidiBinding& mb)
                { return mb.action == act && mb.deck == dk && mb.channel == chn && mb.number == nm && mb.shift == sh; });
        };
        rep.check (r.controller == "Test Board 2" && has ("play_pause", 0, 1, 11, false) && has ("play_pause", 1, 2, 11, false)
                   && has ("hot_cue_1", 0, 8, 0, false) && has ("delete_hot_cue_1", 1, 11, 0, false)
                   && has ("load", 1, 7, 71, false) && has ("filter", 0, 7, 23, false) && has ("play_pause", 0, 1, 71, false)
                   && r.untranslated.contains ("FX1-1On"),
                   "Tabla CSV de otro programa: decks, pads por canal, SHIFT del hardware, filtro y funciones sin equivalente");
        const auto pitchRow = std::find_if (r.bindings.begin(), r.bindings.end(), [] (const MidiBinding& mb) { return mb.action == "pitch" && mb.deck == 1; });
        rep.check (pitchRow != r.bindings.end() && pitchRow->encoding == MidiBinding::Encoding::absolute14 && pitchRow->lsbNumber == 32,
                   "Los faders de alta resolución se importan como 14 bits (MSB + LSB)"_u);

        // Definition + mapper pair (XML).
        const juce::String def = R"(<device name="TB3" type="MIDI" decks="2" drivername="Test Board 3">
              <button note="0x0B" name="PLAY" deck="1" channel="0"/><button note="0x0B" name="PLAY" deck="2" channel="1"/>
              <slider cc="0x33" ccmsb="0x13" name="VOLUME" deck="1" channel="0"/>
              <jog cc="0x22" name="JOG" deck="1" channel="0" zero="0x40"/>
              <button note="0x3F" name="SHIFT" channel="0"/></device>)";
        const juce::String mapper = R"(<mapper device="TB3" version="800">
              <map value="PLAY" action="shift ? reverse : play_pause"/><map value="VOLUME" action="volume"/>
              <map value="JOG" action="jogwheel"/><map value="SHIFT" action="shift"/></mapper>)";
        const auto x = MappingImport::fromXml (def, mapper);
        auto hasX = [&x] (const char* act, int dk, int chn, bool sh)
        {
            return std::any_of (x.bindings.begin(), x.bindings.end(), [=] (const MidiBinding& mb)
                { return mb.action == act && mb.deck == dk && mb.channel == chn && mb.shift == sh; });
        };
        const auto volRow = std::find_if (x.bindings.begin(), x.bindings.end(), [] (const MidiBinding& mb) { return mb.action == "volume"; });
        const auto jogRow = std::find_if (x.bindings.begin(), x.bindings.end(), [] (const MidiBinding& mb) { return mb.action == "jog"; });
        rep.check (x.controller == "Test Board 3" && hasX ("play_pause", 0, 1, false) && hasX ("play_pause", 1, 2, false)
                   && hasX ("reverse", 0, 1, true) && hasX ("shift", 0, 1, false)
                   && volRow != x.bindings.end() && volRow->encoding == MidiBinding::Encoding::absolute14 && volRow->number == 0x13 && volRow->lsbNumber == 0x33
                   && jogRow != x.bindings.end() && jogRow->encoding == MidiBinding::Encoding::relativeOffset,
                   "Definición + mapeo XML: decks, capa SHIFT, fader de 14 bits y jog"_u);

        // One-file XML preset, with script-driven controls named by their path.
        const juce::String preset = R"(<SomeControllerPreset schemaVersion="1"><info><name>Test Board 4</name></info>
              <controller id="TB4"><controls>
              <control><group>[Channel1]</group><key>play</key><status>0x90</status><midino>0x0B</midino><options><normal/></options></control>
              <control><group>[Channel2]</group><key>TB4.rightDeck.playButton.input</key><status>0x91</status><midino>0x0B</midino><options><script-binding/></options></control>
              <control><group>[Channel1]</group><key>TB4.leftDeck.wheelTurn</key><status>0xB0</status><midino>0x06</midino><options><script-binding/></options></control>
              <control><group>[Channel1]</group><key>TB4.leftDeck.wheelTouch</key><status>0x90</status><midino>0x06</midino><options><script-binding/></options></control>
              <control><group>[EqualizerRack1_[Channel1]_Effect1]</group><key>TB4.deckA.eqKnob[3].inputMSB</key><status>0xB0</status><midino>0x10</midino><options><script-binding/></options></control>
              <control><group>[EqualizerRack1_[Channel1]_Effect1]</group><key>TB4.deckA.eqKnob[3].inputLSB</key><status>0xB0</status><midino>0x30</midino><options><script-binding/></options></control>
              <control><group>[Channel1]</group><key>TB4.leftDeck.padSection.padPress</key><status>0x94</status><midino>0x15</midino><options><script-binding/></options></control>
              <control><group>[Channel1]</group><key>TB4.leftDeck.padSection.padPress</key><status>0x94</status><midino>0x14</midino><options><script-binding/></options></control>
              </controls></controller></SomeControllerPreset>)";
        const auto p = MappingImport::fromPreset (preset);
        auto hasP = [&p] (const char* act, int dk, int nm)
        {
            return std::any_of (p.bindings.begin(), p.bindings.end(), [=] (const MidiBinding& mb) { return mb.action == act && mb.deck == dk && mb.number == nm; });
        };
        const auto eqRow = std::find_if (p.bindings.begin(), p.bindings.end(), [] (const MidiBinding& mb) { return mb.action == "eq_high"; });
        rep.check (p.controller == "Test Board 4" && hasP ("play_pause", 0, 11) && hasP ("play_pause", 1, 11) && hasP ("jog", 0, 6)
                   && hasP ("jog_touch", 0, 6) && hasP ("pad_1", 0, 0x14) && hasP ("pad_2", 0, 0x15)
                   && eqRow != p.bindings.end() && eqRow->encoding == MidiBinding::Encoding::absolute14 && eqRow->lsbNumber == 0x30,
                   "Preset XML de un archivo: controles directos y por script (jog, pads en orden, EQ de 14 bits)");

        // The simulator plays every control of both through OpenMix.
        const auto dir = juce::File::createTempFile ("om-import");
        dir.createDirectory();
        dir.getChildFile ("Test Board 2.midi.csv").replaceWithText (csv);
        std::vector<MappingImport::Source> srcs { { "Test Board 2", dir.getChildFile ("Test Board 2.midi.csv"), {} } };
        const int simOk = simulateControllers (srcs, {});
        dir.deleteRecursively();
        rep.check (simOk == 0, "Simulación: cada control importado responde con la función, el deck y el valor correctos"_u);
    }

    if (usbRoot.isDirectory())
    {
        std::cout << "== Pendrive exportado (solo lectura)" << std::endl;

        // Snapshot every file (size + mtime) to prove nothing on the medium changes.
        auto fingerprint = [&usbRoot]
        {
            juce::StringArray fp;
            for (const auto& e : juce::RangedDirectoryIterator (usbRoot, true, "*", juce::File::findFilesAndDirectories))
                fp.add (e.getFile().getRelativePathFrom (usbRoot) + "|" + juce::String (e.getFileSize()) + "|"
                        + juce::String (e.getModificationTime().toMilliseconds()));
            fp.sort (false);
            return fp.joinIntoString ("\n");
        };
        const auto before = fingerprint();

        juce::String err;
        auto ex = UsbExport::load (usbRoot, err);
        rep.check (ex != nullptr, "export.pdb leído"_u, err);
        if (ex != nullptr)
        {
            rep.check (ex->tracks.size() == 2, "2 pistas", juce::String ((int) ex->tracks.size()));
            const auto* t1 = ex->track (1);
            rep.check (t1 != nullptr && t1->title == "Test Groove 124 (USB ñ)"_u, "Título UTF-16"_u, t1 ? t1->title : juce::String());
            rep.check (t1 != nullptr && t1->artist == "Test Artist" && t1->genre == "House", "Artista y género"_u, t1 ? t1->artist + " / " + t1->genre : juce::String());
            rep.check (t1 != nullptr && std::abs (t1->bpm - 124.0) < 0.01, "BPM del pendrive");
            rep.check (t1 != nullptr && UsbExport::keyToCamelot (t1->key) == "11B", "Tonalidad → Camelot"_u, t1 ? t1->key + " → "_u + UsbExport::keyToCamelot (t1->key) : juce::String());
            rep.check (t1 != nullptr && ex->fileFor (*t1).existsAsFile(), "Ruta del archivo en el pendrive");

            const UsbExport::PlaylistRow* warm = nullptr;
            bool folder = false;
            for (auto& p : ex->playlists) { if (p.name == "Warm up") warm = &p; if (p.name == "Sets" && p.isFolder) folder = true; }
            rep.check (folder && warm != nullptr && warm->parentId == 10, "Carpeta y playlist anidada");
            rep.check (warm != nullptr && warm->trackIds == std::vector<int> { 2, 1 }, "Orden de la playlist");

            if (t1 != nullptr)
            {
                const auto an = UsbExport::readAnalysis (*ex, *t1);
                rep.check (std::abs (an.grid.bpm - 124.0) < 0.01 && std::abs (an.grid.firstBeatSeconds - 0.25) < 0.002, "Beatgrid (PQTZ)",
                           juce::String (an.grid.bpm, 2) + " @ " + juce::String (an.grid.firstBeatSeconds, 3));
                rep.check (std::abs (an.hotCues[0] - 8.0) < 0.001 && std::abs (an.hotCues[2] - 32.0) < 0.001 && an.hotCues[1] < 0.0, "Hot cues (PCOB)");
                rep.check (std::abs (an.cuePoint - 4.0) < 0.001, "Memory cue", juce::String (an.cuePoint, 3));

                auto tr = decode (engine, ex->fileFor (*t1));
                auto& d3 = engine.deck (0);
                d3.setTrack (tr);
                d3.setPlaying (true);
                run (d3, 0.5);
                rep.check (d3.getPosition() > 0.4, "Reproduce desde el pendrive");
                d3.setTrack (nullptr);
            }
        }
        rep.check (before == fingerprint(), "Ningún archivo del pendrive cambió"_u);

        // Cues created in OpenMix travel with the drive in <drive>/.OpenMix only.
        VolumeCueStore::setExtraVolumeRoot (usbRoot);
        usbRoot.getChildFile (".OpenMix").deleteRecursively();
        const auto exportBefore = [&usbRoot]
        {
            juce::StringArray fp;
            for (const auto& e : juce::RangedDirectoryIterator (usbRoot, true, "*", juce::File::findFilesAndDirectories))
                if (! e.getFile().getRelativePathFrom (usbRoot).startsWith (".OpenMix"))
                    fp.add (e.getFile().getRelativePathFrom (usbRoot) + "|" + juce::String (e.getFileSize()) + "|"
                            + juce::String (e.getModificationTime().toMilliseconds()));
            fp.sort (false);
            return fp.joinIntoString ("\n");
        };
        const auto fpBefore = exportBefore();
        VolumeCueStore store;
        const auto trackFile = usbRoot.getChildFile ("Contents/Test Artist - Test Groove 124.wav");
        rep.check (! store.get (trackFile).has_value() && ! usbRoot.getChildFile (".OpenMix").exists(),
                   "Leer no crea nada en el pendrive");
        VolumeCueStore::Cues c;
        c.hotCues[0] = 12.5; c.hotCues[4] = 60.0; c.cuePoint = 2.0; c.savedLoops[1] = { 10.0, 12.0 };
        const auto putErr = store.put (trackFile, c);
        rep.check (putErr.isEmpty() && VolumeCueStore::storeFile (usbRoot).existsAsFile(), "Cues guardadas en .OpenMix/cues.json", putErr);
        VolumeCueStore fresh;
        const auto back = fresh.get (trackFile);
        rep.check (back.has_value() && std::abs (back->hotCues[0] - 12.5) < 1e-9 && std::abs (back->hotCues[4] - 60.0) < 1e-9
                   && std::abs (back->cuePoint - 2.0) < 1e-9 && std::abs (back->savedLoops[1].second - 12.0) < 1e-9,
                   "Cues se recuperan desde el pendrive");
        rep.check (fpBefore == exportBefore(), "Fuera de .OpenMix no cambió nada"_u);
        usbRoot.getChildFile (".OpenMix").deleteRecursively();
        VolumeCueStore::setExtraVolumeRoot ({});
    }

    std::cout << std::endl << rep.passed << " OK, " << rep.failed << " fallas" << std::endl;
    return rep.failed == 0 ? 0 : 1;
}

//==============================================================================
/** Plays every control of a controller mapping through OpenMix as the real device would (the exact
    MIDI messages), and checks that each one reaches the right function, deck and value, that no
    message is claimed by two functions, and that the decks really react. */
int simulateControllers (const std::vector<MappingImport::Source>& sources, const juce::String& only)
{
    auto print = [] (const juce::String& s) { std::cout << s.toRawUTF8() << std::endl; };
    const auto previousData = Library::dataFolder();
    const auto scratch = juce::File::createTempFile ("om-sim");
    scratch.createDirectory();
    Library::setDataFolderOverride (scratch);                 // never touch the user's own mappings

    int controllers = 0, clean = 0;
    for (auto& src : sources)
    {
        if (only.isNotEmpty() && src.controller != only) continue;
        const auto conv = MappingImport::convert (src);
        ++controllers;
        if (conv.bindings.empty())
        {
            print (juce::String::fromUTF8 ("  FALLA ") + src.controller + juce::String::fromUTF8 ("  ·  no se pudo traducir ningún control"));
            continue;
        }

        AudioEngine eng;
        eng.prepareForTest (sr, block);
        auto ta = sineTrack (220.0, sr, 60.0, 0.3f); ta->setGrid ({ 124.0, 0.0 });
        auto tb = sineTrack (330.0, sr, 60.0, 0.3f); tb->setGrid ({ 128.0, 0.0 });
        eng.deck (0).setTrack (ta);
        eng.deck (1).setTrack (tb);
        std::vector<float> l (block), r (block);
        float* outs[2] = { l.data(), r.data() };
        auto runE = [&] (int n) { for (int i = 0; i < n; ++i) eng.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {}); };

        Actions actions (eng, {});
        MidiEngine midi (actions);
        std::vector<MidiBinding> map;
        for (auto b : conv.bindings) { b.device = "SIM"; map.push_back (b); }
        midi.setBindings (map);

        struct Hit { juce::String action; int deck; float value; };
        std::vector<Hit> hits;
        midi.onDispatch = [&hits] (const MidiBinding& b, int deck, float v) { hits.push_back ({ b.action, deck, v }); };

        const MidiBinding* shiftKey = nullptr;
        for (auto& b : map) if (b.action == "shift") { shiftKey = &b; break; }
        auto send = [&midi] (const juce::MidiMessage& m) { midi.inject ("SIM", m); };
        auto press = [&] (const MidiBinding& b, bool down)
        {
            const int v = down ? 127 : 0;
            if (b.kind == MidiBinding::Kind::note) send (down ? juce::MidiMessage::noteOn (b.channel, b.number, (juce::uint8) v) : juce::MidiMessage::noteOff (b.channel, b.number));
            else send (juce::MidiMessage::controllerEvent (b.channel, b.number, v));
        };

        juce::StringArray problems;
        int ok = 0, checked = 0;
        std::map<juce::String, juce::String> seenMessage;        // message (+layer) → action, for conflicts
        for (auto& b : map)
        {
            if (b.action == "shift" || b.outputOnly) continue;           // lights without a control: nothing to press
            ++checked;
            const auto* info = actions.find (b.action);
            const int expectDeck = info != nullptr && info->perDeck ? b.deck : -1;
            const auto key = juce::String ((int) b.kind) + ":" + juce::String (b.channel) + ":" + juce::String (b.number) + (b.shift ? ":S" : "");
            if (auto it = seenMessage.find (key); it != seenMessage.end() && it->second != b.action)
                problems.add (juce::String::fromUTF8 ("conflicto: ") + b.describeControl() + " → " + it->second + " y " + b.action);
            seenMessage[key] = b.action;

            if (b.shift)
            {
                if (shiftKey == nullptr) { problems.add (b.action + " necesita SHIFT y no hay botón SHIFT"); continue; }
                press (*shiftKey, true);
            }

            hits.clear();
            float target = -1.0f;
            switch (b.encoding)
            {
                case MidiBinding::Encoding::button:
                    press (b, true);
                    break;
                case MidiBinding::Encoding::absolute7:
                case MidiBinding::Encoding::absolute14:
                {
                    // Catch the control first (soft takeover), then move it to a known place.
                    const float cur = actions.currentValue (b.action, expectDeck);
                    auto move = [&] (float v)
                    {
                        if (b.kind == MidiBinding::Kind::pitchWheel) { send (juce::MidiMessage::pitchWheel (b.channel, juce::jlimit (0, 16383, (int) std::round (v * 16383.0f)))); return; }
                        const int v14 = juce::jlimit (0, 16383, (int) std::round (v * 16383.0f));
                        send (juce::MidiMessage::controllerEvent (b.channel, b.number, b.encoding == MidiBinding::Encoding::absolute14 ? v14 >> 7 : juce::jlimit (0, 127, (int) std::round (v * 127.0f))));
                        if (b.encoding == MidiBinding::Encoding::absolute14) send (juce::MidiMessage::controllerEvent (b.channel, b.lsbNumber, v14 & 127));
                    };
                    if (cur >= 0.0f) move (cur);
                    target = cur >= 0.5f ? 0.2f : 0.8f;
                    hits.clear();
                    move (target);
                    break;
                }
                case MidiBinding::Encoding::relativeOffset: send (juce::MidiMessage::controllerEvent (b.channel, b.number, 65)); break;
                case MidiBinding::Encoding::relativeTwos:   send (juce::MidiMessage::controllerEvent (b.channel, b.number, 1)); break;
                case MidiBinding::Encoding::relativeSign:   send (juce::MidiMessage::controllerEvent (b.channel, b.number, 1)); break;
            }

            bool reached = false;
            for (auto& h : hits)
                if (h.action == b.action && h.deck == expectDeck
                    && (target < 0.0f || std::abs (h.value - (b.invert ? 1.0f - target : target)) < 0.02f))
                    reached = true;
            if (reached) ++ok;
            else problems.add (b.action + " (deck " + juce::String (expectDeck + 1) + ") no respondió a " + b.describeControl());

            if (b.encoding == MidiBinding::Encoding::button) press (b, false);
            if (b.shift && shiftKey != nullptr) press (*shiftKey, false);
        }

        // The decks really react: PLAY, volume, EQ, pitch, hot cue, jog — on both decks.
        juce::StringArray effects;
        for (int d = 0; d < 2; ++d)
        {
            auto find = [&map, d] (const char* action) -> const MidiBinding*
            {
                for (auto& b : map) if (b.action == action && b.deck == d && ! b.shift && ! b.outputOnly) return &b;
                return nullptr;
            };
            auto& deck = eng.deck (d);
            if (auto* p = find ("play_pause"))
            {
                deck.setPlaying (false);
                press (*p, true); press (*p, false); runE (4);
                if (! deck.isPlaying()) effects.add ("PLAY no arranca el deck " + juce::String (d + 1));
                press (*p, true); press (*p, false); runE (2);
            }
            if (auto* hc = find ("hot_cue_1"))
            {
                deck.setPlaying (false);
                deck.seek (10.0); runE (2);
                press (*hc, true); press (*hc, false); runE (2);
                if (deck.getHotCue (0) < 0.0) effects.add ("el pad 1 no guarda el hot cue en el deck " + juce::String (d + 1));
            }
            if (find ("volume") != nullptr)
                if (std::abs (eng.channel (d).fader.load() - actions.currentValue ("volume", d)) > 0.01f || actions.currentValue ("volume", d) < 0.0f)
                    effects.add ("el fader de volumen no movió el canal " + juce::String (d + 1));
            if (auto* j = find ("jog"))
            {
                deck.setPlaying (false);
                runE (2);
                const double before = deck.getPosition();
                for (int i = 0; i < 64; ++i)
                    send (juce::MidiMessage::controllerEvent (j->channel, j->number, j->encoding == MidiBinding::Encoding::relativeOffset ? 65 : 1));
                runE (2);
                if (std::abs (deck.getPosition() - before) < 0.01) effects.add ("el jog no mueve el deck " + juce::String (d + 1));
            }
        }
        problems.addArray (effects);

        // Core functions a two-deck controller is expected to drive.
        juce::StringArray missing;
        for (auto* core : { "play_pause", "cue_button", "sync", "jog", "pitch", "volume", "eq_high", "eq_mid", "eq_low", "hot_cue_1", "load", "crossfader" })
            for (int d = 0; d < 2; ++d)
            {
                const bool perDeck = actions.find (core) != nullptr && actions.find (core)->perDeck;
                if (! perDeck && d == 1) continue;
                const juce::String alt = juce::String (core) == "hot_cue_1" ? "pad_1" : juce::String();   // pads start on the hot cue page
                if (std::none_of (map.begin(), map.end(), [&] (const MidiBinding& mb) { return (mb.action == core || (alt.isNotEmpty() && mb.action == alt)) && (! perDeck || mb.deck == d); }))
                    missing.add (juce::String (core) + (perDeck ? " (deck " + juce::String (d + 1) + ")" : juce::String()));
            }

        const bool allGood = problems.isEmpty();
        if (allGood) ++clean;
        print ((allGood ? "  OK    " : "  FALLA ") + src.controller + "  ·  " + juce::String (ok) + "/" + juce::String (checked)
               + juce::String::fromUTF8 (" controles responden · ") + juce::String (conv.untranslated.size())
               + juce::String::fromUTF8 (" funciones sin equivalente")
               + (missing.isEmpty() ? juce::String::fromUTF8 (" · funciones básicas completas") : juce::String::fromUTF8 (" · faltan: ") + missing.joinIntoString (", ")));
        for (int i = 0; i < juce::jmin (problems.size(), 12); ++i)
            print ("          - " + problems[i]);
        if (problems.size() > 12) print ("          … y " + juce::String (problems.size() - 12) + " más");
    }
    print ("\n" + juce::String (clean) + " de " + juce::String (controllers) + juce::String::fromUTF8 (" controladores sin fallas"));
    scratch.deleteRecursively();
    Library::setDataFolderOverride (previousData);
    return clean == controllers && controllers > 0 ? 0 : 1;
}

//==============================================================================
/** Builds the mapping library that ships inside OpenMix: every source that passes the full
    simulation is converted, given the MIDI names its controller may announce, and written to one
    JSON file (Resources/mappings/library.json). */
int buildMappingLibrary (const juce::File& sourceFolder, const juce::File& outFile)
{
    juce::Array<juce::var> entries;
    juce::StringArray notices;
    std::vector<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (sourceFolder, false, "*.xml;*.csv", juce::File::findFiles))
        files.push_back (e.getFile());
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });

    for (auto& f : files)
    {
        MappingImport::Source src { f.getFileNameWithoutExtension(), f, f };
        auto r = MappingImport::convert (src);
        const bool twoDecks = std::count_if (r.bindings.begin(), r.bindings.end(), [] (const MidiBinding& b) { return b.action == "play_pause" && ! b.outputOnly; }) >= 2;
        if (r.bindings.size() < 20 || ! twoDecks || simulateControllers ({ src }, {}) != 0)
        {
            std::cout << "  descartado: " << f.getFileName().toRawUTF8() << std::endl;
            continue;
        }

        // Names to recognise it by: the full model name and its tail ("Brand Model X" → "Model X"),
        // for every variant the title covers ("Mix 2/4" → "Mix 2", "Mix 4"; "(Pro) 3" with and without
        // "Pro"; version tags dropped).
        auto name = r.controller.isNotEmpty() ? r.controller : src.controller;
        auto words = juce::StringArray::fromTokens (name, " ", "");
        words.removeEmptyStrings();
        for (int i = words.size(); --i >= 0;)
            if (words[i].startsWithIgnoreCase ("v") && words[i].substring (1).containsOnly ("0123456789."))
                words.remove (i);                                   // "v0.995"
        name = words.joinIntoString (" ");
        std::vector<juce::StringArray> variants { {} };
        for (auto& w : words)
        {
            std::vector<juce::StringArray> next;
            juce::StringArray options;
            if (w.startsWithChar ('(') && w.endsWithChar (')')) options = juce::StringArray (w.substring (1, w.length() - 1), juce::String());
            else if (w.containsChar ('/'))                      options = juce::StringArray::fromTokens (w, "/", "");
            else                                                options = { w };
            for (auto& v : variants)
                for (auto& o : options) { auto c = v; if (o.isNotEmpty()) c.add (o); next.push_back (c); }
            variants = next;
        }
        juce::Array<juce::var> match;
        for (auto& v : variants)
            for (int i = 0; i < v.size(); ++i)
            {
                juce::StringArray tail;
                for (int j = i; j < v.size(); ++j) tail.add (v[j]);
                const auto pattern = tail.joinIntoString (" ");
                const bool specific = i == 0 || tail.size() >= 2 || pattern.containsAnyOf ("0123456789-");   // the full name always; not just "Platinum"
                if (specific && ControllerLibrary::normalise (pattern).length() >= 5 && ! match.contains (pattern))
                    match.add (pattern);
            }

        juce::String author;
        if (auto xml = juce::XmlDocument::parse (f))
            if (auto* info = xml->getChildByName ("info"))
                author = info->getChildElementAllSubText ("author", {}).trim();

        juce::Array<juce::var> bindings;
        for (auto b : r.bindings) { b.device = "*"; bindings.add (b.toVar()); }
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("id", ControllerLibrary::normalise (name));
        o->setProperty ("name", name);
        o->setProperty ("author", author);
        auto m = std::make_unique<juce::DynamicObject>();
        m->setProperty ("midi", match);
        o->setProperty ("match", juce::var (m.release()));
        o->setProperty ("bindings", bindings);
        entries.add (juce::var (o.release()));
        notices.add ("- " + name + (author.isNotEmpty() ? " — " + author : juce::String()));
        std::cout << "  incluido: " << name.toRawUTF8() << " (" << bindings.size() << " asignaciones)" << std::endl;
    }

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("version", 1);
    root->setProperty ("mappings", entries);
    outFile.getParentDirectory().createDirectory();
    outFile.replaceWithText (juce::JSON::toString (juce::var (root.release()), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none)));
    outFile.getSiblingFile ("CREDITS.txt").replaceWithText (notices.joinIntoString ("\n") + "\n");
    std::cout << entries.size() << " mapeos en la biblioteca incluida" << std::endl;
    return entries.isEmpty() ? 1 : 0;
}

} // namespace om
