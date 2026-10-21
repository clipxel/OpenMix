#include "SelfTest.h"
#include "../Engine/BeatFx.h"
#include "../Engine/ChannelStrip.h"
#include "../Engine/TrackAnalysis.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>

namespace om
{

/** Checks every BEAT FX of the club mixer against what its owner's manual says it does, by measuring
    it on test signals (timing of repeats, period of the sweep, gating, looping, pitch direction), and
    optionally renders a demo: a real track with each effect switched on in turn, to listen to. */
namespace
{
    constexpr double fs = 48000.0, bpm = 120.0, beatSec = 60.0 / bpm;
    constexpr int blk = 512;

    using Signal = std::vector<float>;

    // Runs an effect on a mono signal (fed to both sides); `on` decides per block whether it is switched on.
    std::pair<Signal, Signal> run (BeatFx::Type t, float param, float level, const Signal& in, std::function<bool (double)> on)
    {
        BeatFx fx; fx.prepare (fs, blk);
        fx.type = (int) t; fx.param = param; fx.wet = level;
        Signal L (in.size()), R (in.size()), l (blk), r (blk);
        for (size_t s = 0; s < in.size(); s += blk)
        {
            const int n = (int) juce::jmin ((size_t) blk, in.size() - s);
            for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = in[s + (size_t) i];
            fx.enabled = on ((double) s / fs);
            fx.process (l.data(), r.data(), n, bpm, (double) s / fs / beatSec, 1.0 / (fs * beatSec));
            for (int i = 0; i < n; ++i) { L[s + (size_t) i] = l[(size_t) i]; R[s + (size_t) i] = r[(size_t) i]; }
        }
        return { L, R };
    }

    float paramFor (BeatFx::Type t, const juce::String& label)
    {
        for (int i = 0; i < BeatFx::beatSteps (t); ++i)
            if (BeatFx::paramText (t, BeatFx::paramAt (t, i)) == label) return BeatFx::paramAt (t, i);
        return BeatFx::defaultParam (t);
    }

    Signal noise (double seconds, float amp = 0.3f)
    {
        juce::Random rnd (7);
        Signal s ((size_t) (seconds * fs));
        for (auto& v : s) v = amp * (rnd.nextFloat() * 2.0f - 1.0f);
        return s;
    }

    Signal tone (double seconds, double hz, float amp = 0.3f)
    {
        Signal s ((size_t) (seconds * fs));
        for (size_t i = 0; i < s.size(); ++i) s[i] = amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / fs);
        return s;
    }

    // Short-time features, one value per 5 ms frame.
    constexpr int hop = 240;
    Signal envelope (const Signal& x)
    {
        Signal e;
        for (size_t s = 0; s + hop <= x.size(); s += hop)
        {
            double a = 0; for (int i = 0; i < hop; ++i) a += (double) x[s + (size_t) i] * x[s + (size_t) i];
            e.push_back ((float) std::sqrt (a / hop));
        }
        return e;
    }
    Signal centroid (const Signal& x)
    {
        juce::dsp::FFT fft (10);
        std::vector<float> buf (2048);
        Signal c;
        for (size_t s = 0; s + 1024 <= x.size(); s += hop)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < 1024; ++i)
                buf[(size_t) i] = x[s + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 1023.0f));
            fft.performFrequencyOnlyForwardTransform (buf.data());
            double num = 0, den = 1.0e-12;
            for (int k = 1; k < 512; ++k) { num += k * (double) buf[(size_t) k]; den += buf[(size_t) k]; }
            c.push_back ((float) (num / den * fs / 1024.0));
        }
        return c;
    }
    // Strongest repetition period (in seconds) of a frame series, searched between lo and hi seconds.
    double period (const Signal& f, double lo, double hi)
    {
        double mean = 0; for (auto v : f) mean += v; mean /= juce::jmax ((size_t) 1, f.size());
        const int a = juce::jmax (1, (int) (lo * fs / hop)), b = juce::jmin ((int) f.size() / 2, (int) (hi * fs / hop));
        std::vector<double> ac ((size_t) b + 2, 0.0);
        double best = -1e30;
        for (int lag = a; lag <= b; ++lag)
        {
            double s = 0; for (size_t i = 0; i + (size_t) lag < f.size(); ++i) s += (f[i] - mean) * (f[i + (size_t) lag] - mean);
            ac[(size_t) lag] = s / (double) (f.size() - (size_t) lag);
            best = juce::jmax (best, ac[(size_t) lag]);
        }
        // The fundamental: the first local peak that is nearly as strong as the best (multiples score alike).
        for (int lag = a + 1; lag < b; ++lag)
            if (ac[(size_t) lag] >= 0.8 * best && ac[(size_t) lag] >= ac[(size_t) lag - 1] && ac[(size_t) lag] >= ac[(size_t) lag + 1])
            {
                // parabolic refinement for frame-level precision
                const double y0 = ac[(size_t) lag - 1], y1 = ac[(size_t) lag], y2 = ac[(size_t) lag + 1];
                const double d = 0.5 * (y0 - y2) / juce::jmax (1.0e-30, y0 - 2 * y1 + y2);
                return ((double) lag + juce::jlimit (-0.5, 0.5, d)) * hop / fs;
            }
        return a * (double) hop / fs;
    }
    // Period of a sweeping effect from whole spectra: the lag at which each moment's spectrum matches best.
    double spectralPeriod (const Signal& x, double lo, double hi)
    {
        juce::dsp::FFT fft (10);
        std::vector<float> buf (2048);
        std::vector<std::vector<float>> spec;
        for (size_t s = 0; s + 1024 <= x.size(); s += hop)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < 1024; ++i)
                buf[(size_t) i] = x[s + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 1023.0f));
            fft.performFrequencyOnlyForwardTransform (buf.data());
            std::vector<float> v (256);
            for (int k = 0; k < 256; ++k) v[(size_t) k] = std::log (1.0e-6f + buf[(size_t) (k * 2 + 1)] + buf[(size_t) (k * 2 + 2)]);
            spec.push_back (std::move (v));
        }
        const int a = (int) (lo * fs / hop), b = juce::jmin ((int) spec.size() / 2, (int) (hi * fs / hop));
        double bestD = 1e30; int bestLag = a;
        for (int lag = a; lag <= b; ++lag)
        {
            double d = 0;
            for (size_t i = 0; i + (size_t) lag < spec.size(); ++i)
                for (size_t k = 0; k < 256; ++k) { const double e = spec[i][k] - spec[i + (size_t) lag][k]; d += e * e; }
            d /= (double) (spec.size() - (size_t) lag);
            if (d < bestD) { bestD = d; bestLag = lag; }
        }
        return bestLag * (double) hop / fs;
    }

    // Times (s) of the distinct repeats of an impulse (peaks of the envelope over a threshold).
    std::vector<double> repeats (const Signal& x, double from, float thr)
    {
        std::vector<double> t;
        const auto e = envelope (x);
        for (size_t i = (size_t) (from * fs / hop) + 1; i + 1 < e.size(); ++i)
            if (e[i] > thr && e[i] >= e[i - 1] && e[i] > e[i + 1] && (t.empty() || (double) i * hop / fs - t.back() > 0.05))
                t.push_back ((double) i * hop / fs);
        return t;
    }
    bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }
}

int fxAudit (const juce::File& demoIn, const juce::File& demoOut)
{
    using T = BeatFx::Type;
    int bad = 0;
    auto report = [&bad] (const juce::String& name, bool ok, const juce::String& what)
    {
        if (! ok) ++bad;
        std::cout << (ok ? "  OK     " : "  REVISAR ") << name.paddedRight (' ', 15).toRawUTF8() << what.toRawUTF8() << std::endl;
    };
    std::cout << "BEAT FX contra el manual (120 BPM: 1 beat = 0.5 s)" << std::endl;

    // Impulse 0.25 s in; the effect on throughout.
    Signal imp ((size_t) (6.0 * fs)); imp[(size_t) (0.25 * fs)] = 0.9f;
    auto always = [] (double) { return true; };
    {
        auto [L, R] = run (T::delay, paramFor (T::delay, "1"), 0.5f, imp, always);
        const auto t = repeats (L, 0.3, 0.004f);
        report ("DELAY", t.size() == 1 && near (t[0] - 0.25, 0.5, 0.02), "un solo eco a 1 beat: " + juce::String ((int) t.size()) + " eco(s), el 1.º a "
                + (t.empty() ? juce::String ("-") : juce::String ((t[0] - 0.25) * 1000.0, 0) + " ms"));
    }
    {
        auto [L, R] = run (T::echo, paramFor (T::echo, "1"), 0.5f, imp, always);
        const auto t = repeats (L, 0.3, 0.002f);
        const auto e = envelope (L);
        bool decays = t.size() >= 3;
        for (size_t k = 1; k < t.size() && decays; ++k) decays = e[(size_t) (t[k] * fs / hop)] <= e[(size_t) (t[k - 1] * fs / hop)] * 1.05f;
        report ("ECHO", decays && ! t.empty() && near (t[0] - 0.25, 0.5, 0.02),
                "varios ecos que se apagan, cada 1 beat: " + juce::String ((int) t.size()) + " ecos");
    }
    {
        auto [L, R] = run (T::pingPong, paramFor (T::pingPong, "1"), 0.5f, imp, always);
        const auto tl = repeats (L, 0.3, 0.002f), tr = repeats (R, 0.3, 0.002f);
        const bool alt = ! tl.empty() && ! tr.empty() && std::abs (tl[0] - tr[0]) > 0.05;
        report ("PING PONG", alt, "ecos alternando izquierda / derecha: 1.º izq " + (tl.empty() ? juce::String ("-") : juce::String ((tl[0] - 0.25) * 1000, 0))
                + " ms, 1.º der " + (tr.empty() ? juce::String ("-") : juce::String ((tr[0] - 0.25) * 1000, 0)) + " ms");
    }
    {
        auto [L, R] = run (T::spiral, paramFor (T::spiral, "1"), 0.6f, imp, always);
        const auto e = envelope (L);
        double early = 0, late = 0;
        for (size_t i = (size_t) (0.7 * fs / hop); i < (size_t) (1.5 * fs / hop); ++i) early += e[i];
        for (size_t i = (size_t) (3.5 * fs / hop); i < (size_t) (4.3 * fs / hop); ++i) late += e[i];
        report ("SPIRAL", early > 0.01 && late > 0.0005 && late < early, "eco con reverberación que gira y se apaga: "
                + juce::String (early, 3) + " → " + juce::String (late, 4));
    }
    {
        // HELIX: records and repeats the recorded sound every BEAT (here on a sweep, so repeats are visible).
        Signal sw ((size_t) (6.0 * fs));
        double ph = 0; for (size_t i = 0; i < sw.size(); ++i) { ph += (200.0 + 300.0 * std::fmod ((double) i / fs, 3.0)) / fs; sw[i] = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * ph); }
        auto [L, R] = run (T::helix, paramFor (T::helix, "1"), 1.0f, sw, [] (double s) { return s > 1.0; });
        Signal tail (L.begin() + (long) (2.0 * fs), L.begin() + (long) (5.0 * fs));
        const double p = period (centroid (tail), 0.2, 1.4);
        report ("HELIX", near (p, 0.5, 0.03), "repite lo grabado cada 1 beat: período " + juce::String (p * 1000, 0) + " ms");
    }
    {
        auto [L, R] = run (T::reverb, paramFor (T::reverb, "50%"), 0.6f, imp, always);
        const auto e = envelope (L);
        int t60 = 0; float pk = 0; for (auto v : e) pk = juce::jmax (pk, v);
        for (size_t i = 0; i < e.size(); ++i) if (e[i] > pk * 0.001f) t60 = (int) i;
        const double rt = t60 * (double) hop / fs - 0.25;
        report ("REVERB", rt > 0.4 && rt < 5.0, "cola de reverberación: " + juce::String (rt, 2) + " s");
    }
    // Cycle effects on noise: the colour of the sound must sweep once per cycle.
    for (auto [t, label, cyc] : { std::tuple { T::flanger, "4", 2.0 }, std::tuple { T::phaser, "4", 2.0 }, std::tuple { T::filter, "4", 2.0 },
                                  std::tuple { T::tripletFilter, "4/3", 2.0 / 3.0 } })
    {
        auto [L, R] = run (t, paramFor (t, label), 0.8f, noise (12.0), always);
        Signal part (L.begin() + (long) (2.0 * fs), L.end());
        const double p = spectralPeriod (part, cyc * 0.6, cyc * 1.4);
        report (BeatFx::typeName (t), near (p, cyc, cyc * 0.06), juce::String ("barre una vez cada ") + label + " beats (" + juce::String (cyc, 2)
                + " s): medido " + juce::String (p, 2) + " s");
    }
    {
        auto [L, R] = run (T::trans, paramFor (T::trans, "1/4"), 1.0f, tone (6.0, 330.0), always);
        Signal part (L.begin() + (long) (1.0 * fs), L.end());
        const auto e = envelope (part);
        float lo = 1, hi = 0; for (auto v : e) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
        const double p = period (e, 0.06, 0.3);
        report ("TRANS", near (p, 0.125, 0.01) && lo < hi * 0.1, "corta el sonido cada 1/4 beat: período " + juce::String (p * 1000, 0) + " ms");
    }
    for (auto [t, label, len] : { std::tuple { T::roll, "1/4", 0.125 }, std::tuple { T::tripletRoll, "1/3", 1.0 / 6.0 } })
    {
        Signal sw ((size_t) (6.0 * fs));
        double ph = 0; for (size_t i = 0; i < sw.size(); ++i) { ph += (200.0 + 400.0 * std::fmod ((double) i / fs, 2.0)) / fs; sw[i] = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * ph); }
        auto [L, R] = run (t, paramFor (t, label), 1.0f, sw, [] (double s) { return s > 1.0; });
        Signal part (L.begin() + (long) (1.5 * fs), L.begin() + (long) (4.5 * fs));
        const double p = period (centroid (part), len * 0.5, len * 2.5);
        report (BeatFx::typeName (t), near (p, len, 0.012), juce::String ("repite el tramo de ") + label + " beat (" + juce::String (len * 1000, 0)
                + " ms): medido " + juce::String (p * 1000, 0) + " ms");
    }
    {
        // MOBIUS: an added tone whose pitch keeps rising through each cycle (falling for − beats).
        auto measure = [] (float param)
        {
            auto [L, R] = run (T::mobius, param, 0.8f, Signal ((size_t) (10.0 * fs), 0.0f), [] (double) { return true; });
            Signal part (L.begin() + (long) (2.0 * fs), L.end());
            // Frequency of the strongest component per frame (8192-point FFT, 50 ms hop): its glide between
            // jumps is the direction the tone moves.
            juce::dsp::FFT fft (13);
            std::vector<float> buf (16384);
            std::vector<double> peak;
            for (size_t s = 0; s + 8192 <= part.size(); s += 2400)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < 8192; ++i) buf[(size_t) i] = part[s + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 8191.0f));
                fft.performFrequencyOnlyForwardTransform (buf.data());
                int kb = 1; for (int k = 2; k < 4096; ++k) if (buf[(size_t) k] > buf[(size_t) kb]) kb = k;
                peak.push_back (kb * fs / 8192.0);
            }
            int up = 0, down = 0;
            for (size_t i = 1; i < peak.size(); ++i)
            {
                const double r = peak[i] / peak[i - 1];
                if (r > 1.0005 && r < 1.3) ++up; else if (r < 0.9995 && r > 0.77) ++down;
            }
            return std::make_pair ((double) (up - down) / juce::jmax (1, up + down), period (centroid (part), 1.0, 3.0));
        };
        const auto [rise, p] = measure (paramFor (T::mobius, "4"));
        const auto [fall, p2] = measure (paramFor (T::mobius, "-4"));
        juce::ignoreUnused (p, p2);
        report ("MOBIUS", rise > 0.6 && fall < -0.6, "tono que sube sin fin (y baja con − beats): sube "
                + juce::String (rise * 100, 0) + " % / con − beats " + juce::String (fall * 100, 0) + " %");
    }

    // SOUND COLOR FX: each one through a channel, COLOR knob turned, compared with the knob at the centre.
    std::cout << "\nSOUND COLOR FX contra el manual" << std::endl;
    {
        auto strip = [] (ChannelStrip::Color c, float knob, const Signal& in, float param = 0.5f)
        {
            ChannelStrip ch; ch.prepare (fs, blk);
            ch.colorType = (int) c; ch.colorOn = true; ch.colorParam = param; ch.filter = knob;
            Signal out (in.size()), l (blk), r (blk);
            for (size_t s = 0; s < in.size(); s += blk)
            {
                const int n = (int) juce::jmin ((size_t) blk, in.size() - s);
                for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = in[s + (size_t) i];
                ch.processPreFader (l.data(), r.data(), n, bpm);
                for (int i = 0; i < n; ++i) out[s + (size_t) i] = l[(size_t) i];
            }
            return out;
        };
        auto rms = [] (const Signal& x, double a, double b)
        {
            double e = 0; size_t n = 0;
            for (size_t i = (size_t) (a * fs); i < (size_t) (b * fs) && i < x.size(); ++i, ++n) e += (double) x[i] * x[i];
            return std::sqrt (e / juce::jmax ((size_t) 1, n));
        };
        using C = ChannelStrip::Color;
        // SPACE / DUB ECHO: a burst, then silence: the effect must keep sounding after it.
        Signal burst = tone (4.0, 440.0); for (size_t i = (size_t) (1.0 * fs); i < burst.size(); ++i) burst[i] = 0.0f;
        for (auto c : { C::space, C::dubEcho })
        {
            const auto o = strip (c, 0.8f, burst);
            const double after = rms (o, 1.3, 2.3);
            report (ChannelStrip::colorName (c), after > 0.005, "sigue sonando después del sonido (reverberación / eco): " + juce::String (after, 4));
        }
        {
            const auto quiet = strip (C::noise, 0.0f, Signal ((size_t) fs, 0.0f));
            const auto loud = strip (C::noise, 0.6f, Signal ((size_t) fs, 0.0f));
            const auto louder = strip (C::noise, 0.6f, Signal ((size_t) fs, 0.0f), 1.0f);
            report ("NOISE", rms (quiet, 0.2, 1.0) < 1e-4 && rms (loud, 0.2, 1.0) > 0.005 && rms (louder, 0.2, 1.0) > rms (loud, 0.2, 1.0),
                    "agrega ruido, y PARAMETER sube su volumen: " + juce::String (rms (loud, 0.2, 1.0), 4) + " → " + juce::String (rms (louder, 0.2, 1.0), 4));
        }
        {
            const auto t = tone (1.0, 220.0);
            const auto o = strip (C::crush, -0.9f, t);
            double d = 0, e = 0; for (size_t i = 4800; i < t.size(); ++i) { d += std::pow (o[i] - t[i], 2.0); e += std::pow (t[i], 2.0); }
            report ("CRUSH", std::sqrt (d / e) > 0.1, "ensucia el sonido (bits / muestreo): diferencia " + juce::String (std::sqrt (d / e), 2));
        }
        {
            const auto lowT = tone (1.0, 150.0), highT = tone (1.0, 6000.0);
            const double lpHigh = rms (strip (C::filter, -0.8f, highT), 0.3, 1.0) / rms (highT, 0.3, 1.0);
            const double hpLow = rms (strip (C::filter, 0.8f, lowT), 0.3, 1.0) / rms (lowT, 0.3, 1.0);
            report ("FILTER", lpHigh < 0.2 && hpLow < 0.2, "izquierda pasa-bajos, derecha pasa-altos: agudos " + juce::String (lpHigh, 3) + ", graves " + juce::String (hpLow, 3));
        }
        {
            const auto c = 150.0 * std::pow (2.0, 0.5 * 6.5);
            const auto t = tone (1.0, c);
            const double pass = rms (strip (C::sweep, 0.9f, t), 0.3, 1.0) / rms (t, 0.3, 1.0);
            const double stop = rms (strip (C::sweep, -0.9f, t), 0.3, 1.0) / rms (t, 0.3, 1.0);
            report ("SWEEP", pass > 0.7 && stop < 0.2, "derecha pasa-banda, izquierda corta-banda: centro " + juce::String (pass, 2) + " / " + juce::String (stop, 3));
        }
    }

    std::cout << (bad == 0 ? "\nTodos los efectos hacen lo que dice el manual." : "\n" + juce::String (bad).toStdString() + " efecto(s) para revisar.") << std::endl;

    // Demo: a real track, each effect switched on for 4 bars in turn (between dry bars), to listen to.
    if (demoIn.existsAsFile())
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (demoIn));
        if (rd == nullptr) { std::cout << "No se pudo abrir " << demoIn.getFullPathName() << std::endl; return 1; }
        const double sr = rd->sampleRate;
        juce::AudioBuffer<float> track (2, (int) rd->lengthInSamples);
        rd->read (&track, 0, track.getNumSamples(), 0, true, true);
        const auto grid = Analysis::detectBeatGrid (track, sr, nullptr);
        const double beat = 60.0 / grid.bpm;
        const auto& order = BeatFx::clubOrder();
        const double startSec = grid.firstBeatSeconds + std::ceil ((30.0 - grid.firstBeatSeconds) / (4 * beat)) * 4 * beat;
        const int segBeats = 24;                                     // 8 dry + 16 with the effect
        const int segLen = (int) std::round (segBeats * beat * sr);
        juce::AudioBuffer<float> out (2, segLen * (int) order.size());
        juce::String cues;
        for (size_t k = 0; k < order.size(); ++k)
        {
            BeatFx fx; fx.prepare (sr, blk);
            const auto t = order[k];
            fx.type = (int) t; fx.param = BeatFx::defaultParam (t); fx.wet = juce::jmax (0.5f, BeatFx::defaultLevel (t));
            const int src0 = (int) std::round ((startSec + (double) k * segBeats * beat) * sr) % juce::jmax (1, track.getNumSamples() - segLen);
            for (int s = 0; s < segLen; s += blk)
            {
                const int n = juce::jmin (blk, segLen - s);
                float* l = out.getWritePointer (0, (int) k * segLen + s);
                float* r = out.getWritePointer (1, (int) k * segLen + s);
                juce::FloatVectorOperations::copy (l, track.getReadPointer (0, src0 + s), n);
                juce::FloatVectorOperations::copy (r, track.getReadPointer (1, src0 + s), n);
                const double pos = ((double) (src0 + s) / sr - grid.firstBeatSeconds) / beat;
                fx.enabled = s >= (int) (8 * beat * sr);
                fx.process (l, r, n, grid.bpm, pos, 1.0 / (beat * sr));
            }
            const double at = (double) k * segBeats * beat;
            cues << juce::String ((int) at / 60) << ":" << juce::String ((int) at % 60).paddedLeft ('0', 2) << "  "
                 << BeatFx::typeName (t) << " (se prende a los " << juce::String (at + 8 * beat, 0) << " s)\n";
        }
        demoOut.deleteFile();
        juce::WavAudioFormat wav;
        if (auto os = std::unique_ptr<juce::OutputStream> (demoOut.createOutputStream()))
            if (auto w = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (os.get(), sr, 2, 16, {}, 0)))
            {
                os.release();
                w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
            }
        demoOut.withFileExtension ("txt").replaceWithText (cues);
        std::cout << "\nDemo: " << demoOut.getFullPathName() << "\n" << cues;
    }
    return bad == 0 ? 0 : 1;
}

} // namespace om
