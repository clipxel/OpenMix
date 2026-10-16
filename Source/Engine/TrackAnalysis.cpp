#include <iostream>
#include "TrackAnalysis.h"
#include <juce_dsp/juce_dsp.h>

namespace om::Analysis
{

static bool aborted (const std::atomic<bool>* a) { return a != nullptr && a->load (std::memory_order_relaxed); }

static float monoSample (const juce::AudioBuffer<float>& b, int i)
{
    const int nc = b.getNumChannels();
    float s = 0.0f;
    for (int c = 0; c < nc; ++c)
        s += b.getSample (c, i);
    return nc > 0 ? s / (float) nc : 0.0f;
}

//==============================================================================
WaveformData computeWaveform (const juce::AudioBuffer<float>& audio, double sr, const std::atomic<bool>* abortFlag)
{
    WaveformData w;
    const int n = audio.getNumSamples();
    if (n <= 0 || sr <= 0.0)
        return w;

    const double hop = sr / WaveformData::framesPerSecond;
    const int frames = (int) std::ceil ((double) n / hop);

    juce::IIRFilter lp1, lp2, hp1, hp2, bpHi, bpLo;
    lp1.setCoefficients (juce::IIRCoefficients::makeLowPass  (sr, 200.0));
    lp2.setCoefficients (juce::IIRCoefficients::makeLowPass  (sr, 200.0));
    hp1.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 2500.0));
    hp2.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 2500.0));
    bpHi.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 200.0));
    bpLo.setCoefficients (juce::IIRCoefficients::makeLowPass  (sr, 2500.0));

    std::vector<float> lo ((size_t) frames, 0.0f), mi ((size_t) frames, 0.0f), hi ((size_t) frames, 0.0f);

    int frame = 0;
    double nextBoundary = hop;
    float pL = 0, pM = 0, pH = 0;

    for (int i = 0; i < n; ++i)
    {
        if ((i & 0xffff) == 0 && aborted (abortFlag))
            return {};

        const float x = monoSample (audio, i);
        const float l = lp2.processSingleSampleRaw (lp1.processSingleSampleRaw (x));
        const float h = hp2.processSingleSampleRaw (hp1.processSingleSampleRaw (x));
        const float m = bpLo.processSingleSampleRaw (bpHi.processSingleSampleRaw (x));

        pL = juce::jmax (pL, std::abs (l));
        pM = juce::jmax (pM, std::abs (m));
        pH = juce::jmax (pH, std::abs (h));

        if ((double) (i + 1) >= nextBoundary || i == n - 1)
        {
            if (frame < frames)
            {
                lo[(size_t) frame] = pL; mi[(size_t) frame] = pM; hi[(size_t) frame] = pH;
            }
            ++frame;
            nextBoundary += hop;
            pL = pM = pH = 0.0f;
        }
    }

    float maxV = 1.0e-6f;
    for (int f = 0; f < frames; ++f)
        maxV = juce::jmax (maxV, lo[(size_t) f], mi[(size_t) f], hi[(size_t) f]);

    const float scale = 255.0f / maxV;
    auto toByte = [scale] (float v) { return (juce::uint8) juce::jlimit (0, 255, (int) std::lround (v * scale)); };

    w.low.resize ((size_t) frames);
    w.mid.resize ((size_t) frames);
    w.high.resize ((size_t) frames);

    for (size_t f = 0; f < (size_t) frames; ++f)
    {
        w.low[f]  = toByte (lo[f]);
        w.mid[f]  = toByte (mi[f]);
        w.high[f] = toByte (hi[f]);
    }

    return w;
}

//==============================================================================
// Beat grid detection
//
//  1. Mono, ~11 kHz. STFT (1024 / hop 64 ≈ 5.8 ms) → log-compressed spectral flux
//     (onset strength), plus a low-band flux and per-frame chroma.
//  2. Tempo: autocorrelation of the onset strength with harmonic support and a
//     log-normal tempo prior (resolves ×2 / ÷2 ambiguity).
//  3. Beat tracking by dynamic programming (Ellis 2007) at that tempo.
//  4. Exact BPM and phase by robust linear regression over all tracked beats.
//  5. Phase refined at ~1 ms resolution on the transient attacks.
//  6. Downbeat ("1") chosen by harmonic change (chroma novelty) + low-band onsets.
namespace
{
    struct Features
    {
        double fps = 0.0;                 // onset frames per second
        double rate = 0.0;                // decimated sample rate
        std::vector<float> mono;          // decimated signal
        std::vector<float> odf, lowOdf;   // onset strength (full / low band)
        std::vector<std::array<float, 12>> chroma;
    };

    bool computeFeatures (const juce::AudioBuffer<float>& audio, double sr, const std::atomic<bool>* abortFlag, Features& f)
    {
        const int n = audio.getNumSamples();
        const int factor = juce::jmax (1, (int) std::floor (sr / 11025.0));
        f.rate = sr / factor;
        f.mono.resize ((size_t) (n / factor));
        for (size_t i = 0; i < f.mono.size(); ++i)
        {
            float acc = 0.0f;
            for (int k = 0; k < factor; ++k) acc += monoSample (audio, (int) i * factor + k);
            f.mono[i] = acc / (float) factor;
        }

        constexpr int order = 10, size = 1 << order, hop = 64;
        const int frames = ((int) f.mono.size() - size) / hop;
        if (frames < 400) return false;
        f.fps = f.rate / hop;

        juce::dsp::FFT fft (order);
        std::vector<float> win ((size_t) size), buf ((size_t) size * 2), prev ((size_t) size / 2, 0.0f), cur ((size_t) size / 2);
        for (int i = 0; i < size; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

        const int maxBin = (int) (5000.0 / f.rate * size);
        const int lowBin = juce::jmax (2, (int) (150.0 / f.rate * size));
        std::vector<int> pcOf ((size_t) size / 2, -1);
        for (int k = 1; k < size / 2; ++k)
        {
            const double hz = k * f.rate / size;
            if (hz >= 60.0 && hz <= 2000.0)
                pcOf[(size_t) k] = ((int) std::lround (69.0 + 12.0 * std::log2 (hz / 440.0)) % 12 + 12) % 12;
        }

        f.odf.assign ((size_t) frames, 0.0f);
        f.lowOdf.assign ((size_t) frames, 0.0f);
        f.chroma.assign ((size_t) frames, {});

        for (int fr = 0; fr < frames; ++fr)
        {
            if ((fr & 1023) == 0 && aborted (abortFlag)) return false;
            std::fill (buf.begin(), buf.end(), 0.0f);
            const float* x = f.mono.data() + (size_t) fr * hop;
            for (int i = 0; i < size; ++i) buf[(size_t) i] = x[i] * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (buf.data(), true);

            float flux = 0.0f, lowFlux = 0.0f;
            auto& ch = f.chroma[(size_t) fr];
            for (int k = 1; k < size / 2; ++k)
            {
                const float m = buf[(size_t) k];
                cur[(size_t) k] = std::log1p (100.0f * m);
                if (k < maxBin)
                {
                    const float d = cur[(size_t) k] - prev[(size_t) k];
                    if (d > 0.0f) { flux += d; if (k <= lowBin) lowFlux += d; }
                }
                if (const int pc = pcOf[(size_t) k]; pc >= 0) ch[(size_t) pc] += m;
            }
            std::swap (prev, cur);
            f.odf[(size_t) fr] = flux;
            f.lowOdf[(size_t) fr] = lowFlux;
        }

        // Remove the slowly varying part (≈ 0.5 s moving mean), rectify, normalise.
        auto whiten = [&] (std::vector<float>& v)
        {
            const int half = (int) (f.fps * 0.25);
            std::vector<float> out (v.size());
            double run = 0.0; int lo = 0, hi = -1, count = 0;
            for (int i = 0; i < (int) v.size(); ++i)
            {
                while (hi < juce::jmin ((int) v.size() - 1, i + half)) { run += v[(size_t) ++hi]; ++count; }
                while (lo < i - half) { run -= v[(size_t) lo++]; --count; }
                out[(size_t) i] = juce::jmax (0.0f, v[(size_t) i] - (float) (run / juce::jmax (1, count)));
            }
            double sum = 0, sq = 0;
            for (auto x : out) { sum += x; sq += (double) x * x; }
            const double cnt = (double) out.size();
            const double sd = std::sqrt (juce::jmax (1e-12, sq / cnt - (sum / cnt) * (sum / cnt)));
            for (auto& x : out) x = (float) (x / sd);
            v = std::move (out);
        };
        whiten (f.odf);
        whiten (f.lowOdf);
        return true;
    }

    double interp (const std::vector<double>& v, double i)
    {
        const auto k = (size_t) i;
        if (k + 1 >= v.size()) return 0.0;
        const double t = i - (double) k;
        return v[k] + t * (v[k + 1] - v[k]);
    }

    /** Tempo (BPM) with the best harmonic periodicity × prior. */
    double estimateTempo (const Features& f)
    {
        const size_t N = f.odf.size();
        const int maxLag = (int) (f.fps * 60.0 / 40.0) * 2 + 4;
        std::vector<double> acf ((size_t) maxLag + 1, 0.0);
        for (int lag = 1; lag <= maxLag && (size_t) lag < N; ++lag)
        {
            double s = 0.0;
            for (size_t i = (size_t) lag; i < N; ++i) s += (double) f.odf[i] * f.odf[i - (size_t) lag];
            acf[(size_t) lag] = s / (double) (N - (size_t) lag);
        }

        double best = -1.0, bestBpm = 120.0;
        for (double bpm = 60.0; bpm <= 200.0; bpm += 0.25)
        {
            const double lag = f.fps * 60.0 / bpm;
            double s = 0.0;
            for (int k = 1; k <= 4; ++k)                       // beat, half bar, …: harmonic support
                s += interp (acf, lag * k) / std::sqrt ((double) k);
            s += 0.5 * interp (acf, lag * 0.5);                // eighth notes support the beat level too
            // Log-normal prior around 118 BPM (σ ≈ 0.75 octave): picks the "felt" octave.
            const double oct = std::log2 (bpm / 118.0);
            s *= std::exp (-0.5 * oct * oct / (0.75 * 0.75));
            if (s > best) { best = s; bestBpm = bpm; }
        }
        return bestBpm;
    }

    /** Dynamic-programming beat tracker; returns beat positions in frames. */
    std::vector<double> trackBeats (const std::vector<float>& odf, double period)
    {
        const int N = (int) odf.size();
        std::vector<double> score ((size_t) N), back ((size_t) N, -1);
        const double tightness = 300.0;
        const int lo = (int) std::round (period * 0.5), hi = (int) std::round (period * 2.0);

        for (int t = 0; t < N; ++t)
        {
            double bestPrev = 0.0; int arg = -1;
            for (int tau = t - hi; tau <= t - lo; ++tau)
            {
                if (tau < 0) continue;
                const double r = std::log ((t - tau) / period);
                const double v = score[(size_t) tau] - tightness * r * r;
                if (arg < 0 || v > bestPrev) { bestPrev = v; arg = tau; }
            }
            score[(size_t) t] = odf[(size_t) t] + (arg >= 0 ? juce::jmax (0.0, bestPrev) : 0.0);
            back[(size_t) t] = (arg >= 0 && bestPrev > 0.0) ? arg : -1;
        }

        int t = N - 1;
        for (int k = juce::jmax (0, N - (int) period * 2); k < N; ++k)
            if (score[(size_t) k] > score[(size_t) t]) t = k;

        std::vector<double> beats;
        while (t >= 0) { beats.push_back (t); t = (int) back[(size_t) t]; }
        std::reverse (beats.begin(), beats.end());
        return beats;
    }

    /** Robust least squares t_i = a + b·i over beat indices. */
    bool fitGrid (const std::vector<double>& beats, double period, double& a, double& b)
    {
        if (beats.size() < 8) return false;
        std::vector<double> idx (beats.size());
        for (size_t i = 0; i < beats.size(); ++i) idx[i] = std::round ((beats[i] - beats[0]) / period);

        std::vector<bool> use (beats.size(), true);
        for (int iter = 0; iter < 4; ++iter)
        {
            double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
            for (size_t i = 0; i < beats.size(); ++i)
                if (use[i]) { sx += idx[i]; sy += beats[i]; sxx += idx[i] * idx[i]; sxy += idx[i] * beats[i]; ++n; }
            const double den = n * sxx - sx * sx;
            if (n < 8 || std::abs (den) < 1e-9) return false;
            b = (n * sxy - sx * sy) / den;
            a = (sy - b * sx) / n;
            const double tol = period * (iter < 2 ? 0.25 : 0.12);
            for (size_t i = 0; i < beats.size(); ++i)
                use[i] = std::abs (beats[i] - (a + b * idx[i])) < tol;
        }
        return true;
    }
}

// Attack envelopes at ~1 ms: kick band (30–150 Hz) and full band, as the rise above the recent level.
struct Envelopes { std::vector<float> kick, full; double fps = 0.0; };

static Envelopes kickEnvelopes (const Features& f)
{
    Envelopes env;
    juce::IIRFilter lp1, lp2, hp;
    lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (f.rate, 150.0));
    lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (f.rate, 150.0));
    hp.setCoefficients (juce::IIRCoefficients::makeHighPass (f.rate, 30.0));
    const int hop = juce::jmax (1, (int) std::round (f.rate * 0.001));
    const int frames = (int) f.mono.size() / hop;
    env.kick.assign ((size_t) frames, 0.0f);
    env.full.assign ((size_t) frames, 0.0f);
    env.fps = f.rate / hop;
    float slow = 0.0f, slowFull = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        double e = 0.0, ef = 0.0;
        for (int k = 0; k < hop; ++k)
        {
            const float x = f.mono[(size_t) (i * hop + k)];
            const float y = hp.processSingleSampleRaw (lp2.processSingleSampleRaw (lp1.processSingleSampleRaw (x)));
            e += (double) y * y;
            ef += (double) x * x;
        }
        const float le = std::log1p (1.0e4f * (float) e), lf = std::log1p (1.0e4f * (float) ef);
        env.kick[(size_t) i] = juce::jmax (0.0f, le - slow);
        env.full[(size_t) i] = juce::jmax (0.0f, lf - slowFull);
        slow = 0.85f * slow + 0.15f * le;
        slowFull = 0.85f * slowFull + 0.15f * lf;
    }
    return env;
}

BeatGrid alignGridToKicks (const juce::AudioBuffer<float>& audio, double sr, BeatGrid grid)
{
    const double off = attackOffsetMs (audio, sr, grid);
    if (off == 0.0 || std::abs (off) > 50.0) return grid;
    return { grid.bpm, grid.firstBeatSeconds + off / 1000.0 };
}

double attackOffsetMs (const juce::AudioBuffer<float>& audio, double sr, BeatGrid grid)
{
    Features f;
    if (! grid.isValid() || ! computeFeatures (audio, sr, nullptr, f)) return 0.0;
    const auto env = kickEnvelopes (f);
    std::vector<double> offs;
    for (double t = grid.firstBeatSeconds; ; t += 60.0 / grid.bpm)
    {
        const int c = (int) std::round (t * env.fps);
        if (c + 40 >= (int) env.kick.size()) break;
        if (c < 40) continue;
        int bestK = 0; float bestV = 0.0f;
        for (int k = -40; k < 40; ++k) if (env.kick[(size_t) (c + k)] > bestV) { bestV = env.kick[(size_t) (c + k)]; bestK = k; }
        if (bestV > 0.8f) offs.push_back (bestK * 1000.0 / env.fps);
    }
    if (offs.size() < 16) return 0.0;
    std::nth_element (offs.begin(), offs.begin() + (long) offs.size() / 2, offs.end());
    return offs[offs.size() / 2];
}

// Diagnostics: where hits start in the drawn waveform (450 fps, bands layered as on screen), relative
// to the grid lines — per beat, valley before the hit → peak; start = 25 % of the climb. Histogram in ms.
static juce::String drawnHistogram (const juce::AudioBuffer<float>& audio, double sr, BeatGrid g)
{
    const auto w = computeWaveform (audio, sr);
    const int nf = w.numFrames();
    const double fps = WaveformData::framesPerSecond;
    auto height = [&w] (int f) { return juce::jmax (0.98f * w.low[(size_t) f], 0.80f * w.mid[(size_t) f], 0.55f * w.high[(size_t) f]); };
    std::array<int, 20> h {};
    std::vector<double> all;
    for (double t = g.firstBeatSeconds; ; t += 60.0 / g.bpm)
    {
        const int c = (int) std::round (t * fps), w0 = (int) (0.040 * fps), w1 = (int) (0.040 * fps);
        if (c + w1 >= nf) break;
        if (c - w0 < 1) continue;
        int pk = c - w0;
        for (int f = c - w0; f < c + w1; ++f) if (height (f) > height (pk)) pk = f;
        int lo = pk;
        for (int f = pk; f >= c - w0; --f) if (height (f) < height (lo)) lo = f;
        const float P = height (pk), V = height (lo);
        if (P < 8.0f || P < 1.25f * V) continue;
        const float level = V + 0.25f * (P - V);
        int st = lo;
        while (st < pk && height (st) < level) ++st;
        const double ms = (st - c) * 1000.0 / fps;
        all.push_back (ms);
        ++h[(size_t) juce::jlimit (0, 19, (int) std::floor ((ms + 40.0) / 4.0))];
    }
    juce::String out;
    for (auto v : h) out << juce::String (v).paddedLeft (' ', 4);
    if (! all.empty()) { std::nth_element (all.begin(), all.begin() + (long) all.size() / 2, all.end()); out << "   mediana " << juce::String (all[all.size() / 2], 1) << " ms (" << (int) all.size() << " golpes)"; }
    return out;
}

GridQuality evaluateGrid (const juce::AudioBuffer<float>& audio, double sr, BeatGrid g)
{
    GridQuality q;
    if (! g.isValid()) return q;
    const auto w = computeWaveform (audio, sr);
    const int nf = w.numFrames();
    const double fps = WaveformData::framesPerSecond;
    auto height = [&w] (int f) { return juce::jmax (0.98f * w.low[(size_t) f], 0.80f * w.mid[(size_t) f], 0.55f * w.high[(size_t) f]); };
    std::vector<std::pair<double, double>> hits;            // (time, offset ms)
    const int w0 = (int) (0.040 * fps), w1 = (int) (0.040 * fps);
    for (double t = g.firstBeatSeconds; ; t += 60.0 / g.bpm)
    {
        const int c = (int) std::round (t * fps);
        if (c + w1 >= nf) break;
        if (c - w0 < 1) continue;
        int pk = c - w0;
        for (int f = c - w0; f < c + w1; ++f) if (height (f) > height (pk)) pk = f;
        int lo = pk;
        for (int f = pk; f >= c - w0; --f) if (height (f) < height (lo)) lo = f;
        const float P = height (pk), V = height (lo);
        if (P < 8.0f || P < 1.25f * V) continue;
        const float level = V + 0.25f * (P - V);
        int st = lo;
        while (st < pk && height (st) < level) ++st;
        hits.push_back ({ t, (st - c) * 1000.0 / fps });
    }
    q.hits = (int) hits.size();
    if (hits.size() < 32) return q;
    auto median = [] (std::vector<double> v) { std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end()); return v[v.size() / 2]; };
    std::vector<double> all, first, last;
    const double len = (double) audio.getNumSamples() / sr;
    for (auto [t, ms] : hits)
    {
        all.push_back (ms);
        if (t < len / 3) first.push_back (ms);
        else if (t > 2 * len / 3) last.push_back (ms);
    }
    q.offsetMs = median (all);
    std::vector<double> dev;
    for (auto v : all) dev.push_back (std::abs (v - q.offsetMs));
    q.spreadMs = median (dev);
    if (first.size() >= 8 && last.size() >= 8) q.driftMs = median (last) - median (first);
    return q;
}

juce::String debugGrid (const juce::AudioBuffer<float>& audio, double sr, BeatGrid reference)
{
    Features f;
    if (! reference.isValid() || ! computeFeatures (audio, sr, nullptr, f)) return "sin datos";
    const auto env = kickEnvelopes (f);
    const auto mine = detectBeatGrid (audio, sr);
    // Where do the attacks fall, folded onto one beat of the reference grid (16 bins)?
    auto fold = [&] (const std::vector<float>& e)
    {
        std::array<double, 16> h {};
        double total = 0;
        const double beatF = env.fps * 60.0 / reference.bpm, startF = reference.firstBeatSeconds * env.fps;
        for (size_t i = 0; i < e.size(); ++i)
        {
            // Local peaks only (one per attack), weighted by strength².
            if (e[i] <= 0.5f || i == 0 || i + 1 >= e.size() || e[i] < e[i - 1] || e[i] < e[i + 1]) continue;
            double ph = std::fmod ((double) i - startF, beatF) / beatF;
            if (ph < 0) ph += 1.0;
            const double w = (double) e[i] * e[i];
            h[(size_t) juce::jlimit (0, 15, (int) (ph * 16))] += w;
            total += w;
        }
        juce::String s;
        for (auto v : h) s << juce::String (juce::roundToInt (100.0 * v / juce::jmax (1e-9, total))).paddedLeft (' ', 3);
        return s;
    };
    const double beat = 60.0 / reference.bpm;
    double d = std::fmod (mine.firstBeatSeconds - reference.firstBeatSeconds, beat);
    if (d > beat / 2) d -= beat;
    if (d < -beat / 2) d += beat;
    // Fine view around each reference beat (−40…+40 ms, 4 ms slots): kick attack peaks, and the
    // first sample of each kick that reaches half of its peak in the 30–150 Hz band (what a
    // waveform shows as the start of the hit).
    juce::String fine;
    {
        std::array<int, 20> hk {}, hs {};
        juce::IIRFilter lp1, lp2;
        lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (f.rate, 150.0));
        lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (f.rate, 150.0));
        std::vector<float> low (f.mono.size());
        for (size_t i = 0; i < low.size(); ++i) low[i] = std::abs (lp2.processSingleSampleRaw (lp1.processSingleSampleRaw (f.mono[i])));
        for (double b = 0; ; b += 1.0)
        {
            const double tb = reference.firstBeatSeconds + (b - 64.0) * (60.0 / reference.bpm);
            if (tb < 0.05) continue;
            const int c = (int) std::round (tb * env.fps);
            if (c + 60 >= (int) env.kick.size()) break;
            int bestK = 0; float bestV = 0.0f;
            for (int k = -40; k < 40; ++k) if (env.kick[(size_t) (c + k)] > bestV) { bestV = env.kick[(size_t) (c + k)]; bestK = k; }
            if (bestV > 0.5f) ++hk[(size_t) juce::jlimit (0, 19, (bestK + 40) / 4)];
            // amplitude: peak in [−40, +80] ms, then first sample ≥ 50 % of it
            const int s0 = (int) ((tb - 0.040) * f.rate), s1 = (int) ((tb + 0.080) * f.rate);
            if (s0 < 0 || s1 >= (int) low.size()) continue;
            float pk = 0; for (int i = s0; i < s1; ++i) pk = juce::jmax (pk, low[(size_t) i]);
            if (pk < 0.05f) continue;
            for (int i = s0; i < s1; ++i) if (low[(size_t) i] >= 0.5f * pk) { const double ms = (i / f.rate - tb) * 1000.0; if (ms < 40.0) ++hs[(size_t) juce::jlimit (0, 19, (int) ((ms + 40.0) / 4.0))]; break; }
        }
        fine << "\nms desde la línea:   "; for (int k = 0; k < 20; ++k) fine << juce::String (-40 + k * 4).paddedLeft (' ', 4);
        fine << "\nataque (subida):     "; for (auto v : hk) fine << juce::String (v).paddedLeft (' ', 4);
        fine << "\ngolpe al 50 %:       "; for (auto v : hs) fine << juce::String (v).paddedLeft (' ', 4);
    }
    // Kick START (zero-phase low band, first sample above 10 % of each hit) relative to the
    // reference lines and to OpenMix's own grid — what the eye sees as "where the hit begins".
    auto startHist = [&] (BeatGrid g)
    {
        std::vector<float> y (f.mono);
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::IIRFilter a, b;
            a.setCoefficients (juce::IIRCoefficients::makeLowPass (f.rate, 140.0));
            b.setCoefficients (juce::IIRCoefficients::makeHighPass (f.rate, 35.0));
            for (auto& v : y) v = b.processSingleSampleRaw (a.processSingleSampleRaw (v));
            std::reverse (y.begin(), y.end());
        }
        // Envelope that follows the wave's peaks (no dips at each zero crossing), without moving the onset.
        {
            const float k = (float) std::exp (-1.0 / (0.015 * f.rate));
            float st = 0.0f;
            for (auto& v : y) { st = juce::jmax (std::abs (v), st * k); v = st; }
        }
        std::array<int, 20> h {};
        std::vector<double> all;
        for (double t = g.firstBeatSeconds; ; t += 60.0 / g.bpm)
        {
            const int c = (int) std::round (t * f.rate), w0 = (int) (0.045 * f.rate), w1 = (int) (0.035 * f.rate);
            if (c + w1 >= (int) y.size()) break;
            if (c - w0 < 1) continue;
            int pk = c - w0;
            for (int i = c - w0; i < c + w1; ++i) if (y[(size_t) i] > y[(size_t) pk]) pk = i;
            const float peak = y[(size_t) pk];
            float floorV = 1e9f; for (int i = c - w0; i < c - w0 + (int) (0.01 * f.rate); ++i) floorV = juce::jmin (floorV, y[(size_t) i]);
            if (peak < 1e-3f || floorV > 0.3f * peak) continue;
            int st = pk;
            while (st > c - w0 && y[(size_t) st] > 0.10f * peak) --st;
            const double ms = (st - c) * 1000.0 / f.rate;
            all.push_back (ms);
            ++h[(size_t) juce::jlimit (0, 19, (int) std::floor ((ms + 40.0) / 4.0))];
        }
        juce::String out;
        for (auto v : h) out << juce::String (v).paddedLeft (' ', 4);
        if (! all.empty()) { std::nth_element (all.begin(), all.begin() + (long) all.size() / 2, all.end()); out << "   mediana " << juce::String (all[all.size() / 2], 1) << " ms"; }
        return out;
    };
    fine << "\ninicio del golpe (10%):" << startHist (reference);
    fine << "\n  …respecto de la grilla de OpenMix:" << startHist (mine);
    fine << "\nDIBUJO: inicio del golpe respecto de la línea de OpenMix:" << drawnHistogram (audio, sr, mine);
    return fine + "\n" + "OpenMix " + juce::String (mine.bpm, 2) + " BPM, fase " + juce::String (d / beat, 2) + " beat respecto de la referencia\n"
         + "graves (16avos de beat desde la referencia):" + fold (env.kick) + "\n"
         + "banda completa:                           " + fold (env.full);
}

// Where the kicks START relative to the beat lines (seconds, median over the whole track): low band
// filtered forward and backward (no delay), an envelope that follows the wave's peaks (no dips at
// each zero crossing of the low tone), and for each beat the first moment the hit rises above 10 %
// of its peak. Club software draws the line there — at the start of the hit, not where it grows fastest.
static double kickStartOffset (const Features& f, double bpm, double phase)
{
    if (f.mono.size() < 1000 || bpm <= 0.0) return 0.0;
    std::vector<float> y (f.mono);
    for (int pass = 0; pass < 2; ++pass)
    {
        juce::IIRFilter a, b;
        a.setCoefficients (juce::IIRCoefficients::makeLowPass (f.rate, 140.0));
        b.setCoefficients (juce::IIRCoefficients::makeHighPass (f.rate, 35.0));
        for (auto& v : y) v = b.processSingleSampleRaw (a.processSingleSampleRaw (v));
        std::reverse (y.begin(), y.end());
    }
    {
        const float k = (float) std::exp (-1.0 / (0.015 * f.rate));
        float st = 0.0f;
        for (auto& v : y) { st = juce::jmax (std::abs (v), st * k); v = st; }
    }
    const double beat = 60.0 / bpm;
    const int w0 = (int) (0.045 * f.rate), w1 = (int) (0.035 * f.rate);
    std::vector<double> offs;
    for (double t = std::fmod (phase, beat); ; t += beat)
    {
        const int c = (int) std::round (t * f.rate);
        if (c + w1 >= (int) y.size()) break;
        if (c - w0 < 1) continue;
        int pk = c - w0;
        for (int i = c - w0; i < c + w1; ++i) if (y[(size_t) i] > y[(size_t) pk]) pk = i;
        const float peak = y[(size_t) pk];
        // The lowest point just before the hit: the bass may keep sounding underneath (no silence needed).
        int lo = pk;
        for (int i = pk; i > c - w0; --i) if (y[(size_t) i] < y[(size_t) lo]) lo = i;
        const float floorV = y[(size_t) lo];
        if (peak < 1.0e-3f || peak < 1.5f * floorV || lo <= c - w0 + 1) continue;   // no clear hit there
        // The hit starts where it has climbed 15 % of the way from that floor to its peak.
        const float level = floorV + 0.15f * (peak - floorV);
        int st = lo;
        while (st < pk && y[(size_t) st] < level) ++st;
        offs.push_back ((st - c) / f.rate);
    }
    if (offs.size() < 16) return 0.0;
    std::nth_element (offs.begin(), offs.begin() + (long) offs.size() / 2, offs.end());
    return offs[offs.size() / 2];
}

double kickStartOffsetMs (const juce::AudioBuffer<float>& audio, double sr, BeatGrid grid)
{
    Features f;
    if (! grid.isValid() || ! computeFeatures (audio, sr, nullptr, f)) return 0.0;
    return kickStartOffset (f, grid.bpm, grid.firstBeatSeconds) * 1000.0;
}

static constexpr double attackCalibrationSec = 0.0;     // the line sits on the attack: where the hit starts in the drawn waveform

// Sharp phase from the hits' attacks: full-band transient strength (pre-emphasised energy rise at
// 0.25 ms) folded onto the beat lines of the WHOLE track; the grid moves to where the attacks pile up.
// One fit for all beats together is far steadier than measuring hits one by one.
static double attackPhaseShift (const juce::AudioBuffer<float>& audio, double sr, double bpm, double phase)
{
    const int n = audio.getNumSamples();
    if (n < 1000 || bpm <= 0.0 || sr <= 0.0) return 0.0;
    const int hop = juce::jmax (1, (int) std::round (sr * 0.00025));
    const int frames = n / hop;
    std::vector<float> le ((size_t) frames, 0.0f);
    float prev = 0.0f;
    for (int fr = 0; fr < frames; ++fr)
    {
        double e = 0.0;
        for (int k = 0; k < hop; ++k)
        {
            const float x = monoSample (audio, fr * hop + k);
            const float d = x - 0.97f * prev;                 // pre-emphasis: the click of each hit stands out
            prev = x;
            e += (double) d * d;
        }
        le[(size_t) fr] = std::log1p (1.0e5f * (float) e);
    }
    const double fps = sr / hop;
    const int lag = juce::jmax (1, (int) std::round (0.001 * fps));   // rise over 1 ms
    const int span = (int) std::round (0.030 * fps);
    std::vector<double> h ((size_t) (2 * span + 1), 0.0);
    const double beat = 60.0 / bpm;
    for (double t = std::fmod (phase, beat); ; t += beat)
    {
        const int c = (int) std::round (t * fps);
        if (c + span + 1 >= frames) break;
        if (c - span - lag < 0) continue;
        for (int k = -span; k <= span; ++k)
        {
            const float r = le[(size_t) (c + k)] - le[(size_t) (c + k - lag)];
            if (r > 0.0f) h[(size_t) (k + span)] += (double) r * r;
        }
    }
    // Smooth over ±1 ms and take the peak.
    const int sm = juce::jmax (1, (int) std::round (0.001 * fps));
    int best = span;
    double bestV = -1.0;
    for (int k = sm; k < (int) h.size() - sm; ++k)
    {
        double v = 0.0;
        for (int j = -sm; j <= sm; ++j) v += h[(size_t) (k + j)] * (1.0 - std::abs (j) / (double) (sm + 1));
        if (v > bestV) { bestV = v; best = k; }
    }
    return (best - span) / fps;
}

// Last check, with the DJ's own criterion: where each hit starts in the drawn waveform (bands layered as
// on screen, 450 fps). Per beat: valley before the hit → its peak, start at 25 % of the climb; median
// over the whole track. Returns 0 when there are too few clear hits to tell.
static double drawnHitStart (const juce::AudioBuffer<float>& audio, double sr, double bpm, double phase)
{
    const auto w = computeWaveform (audio, sr);
    const int nf = w.numFrames();
    if (nf < 100 || bpm <= 0.0) return 0.0;
    const double fps = WaveformData::framesPerSecond;
    auto height = [&w] (int f) { return juce::jmax (0.98f * w.low[(size_t) f], 0.80f * w.mid[(size_t) f], 0.55f * w.high[(size_t) f]); };
    std::vector<double> all;
    const int w0 = (int) (0.040 * fps), w1 = (int) (0.040 * fps);
    for (double t = std::fmod (phase, 60.0 / bpm); ; t += 60.0 / bpm)
    {
        const int c = (int) std::round (t * fps);
        if (c + w1 >= nf) break;
        if (c - w0 < 1) continue;
        int pk = c - w0;
        for (int f = c - w0; f < c + w1; ++f) if (height (f) > height (pk)) pk = f;
        int lo = pk;
        for (int f = pk; f >= c - w0; --f) if (height (f) < height (lo)) lo = f;
        const float P = height (pk), V = height (lo);
        if (P < 8.0f || P < 1.25f * V) continue;
        const float level = V + 0.25f * (P - V);
        int st = lo;
        while (st < pk && height (st) < level) ++st;
        all.push_back ((st - c) / fps);
    }
    if (all.size() < 32) return 0.0;
    std::nth_element (all.begin(), all.begin() + (long) all.size() / 2, all.end());
    return all[all.size() / 2];
}

// The grid fitted straight onto the hits: the start of every hit in the drawn waveform (as the DJ sees
// it), then a robust straight line through them (time = first + n · beat). Gives the exact phase AND
// the exact tempo — a grid that slowly slides off the hits along the track had a slightly wrong BPM.
static bool fitGridToDrawnHits (const juce::AudioBuffer<float>& audio, double sr, double& bpm, double& phase)
{
    const auto w = computeWaveform (audio, sr);
    const int nf = w.numFrames();
    if (nf < 100 || bpm <= 0.0) return false;
    const double fps = WaveformData::framesPerSecond;
    auto height = [&w] (int f) { return juce::jmax (0.98f * w.low[(size_t) f], 0.80f * w.mid[(size_t) f], 0.55f * w.high[(size_t) f]); };
    const double beat = 60.0 / bpm;
    const int w0 = (int) (0.045 * fps), w1 = (int) (0.045 * fps);
    std::vector<std::pair<double, double>> pts;             // (beat index, hit start in seconds)
    const double first = std::fmod (phase, beat);
    for (int n = 0; ; ++n)
    {
        const double t = first + n * beat;
        const int c = (int) std::round (t * fps);
        if (c + w1 >= nf) break;
        if (c - w0 < 1) continue;
        int pk = c - w0;
        for (int f = c - w0; f < c + w1; ++f) if (height (f) > height (pk)) pk = f;
        int lo = pk;
        for (int f = pk; f >= c - w0; --f) if (height (f) < height (lo)) lo = f;
        const float P = height (pk), V = height (lo);
        if (P < 8.0f || P < 1.25f * V) continue;
        const float level = V + 0.25f * (P - V);
        int st = lo;
        while (st < pk && height (st) < level) ++st;
        pts.push_back ({ (double) n, st / fps });
    }
    static const bool dbg = std::getenv ("OPENMIX_GRID_DEBUG") != nullptr;
    if (dbg) std::cerr << "[fit] golpes " << pts.size() << std::endl;
    if (pts.size() < 48) return false;

    // Robust line fit. Start from the most repeated hit position (some tracks have two hits per beat,
    // e.g. a strong kick 13 ms after a softer click 28 ms before): only that one is fitted.
    double a = first, b = beat;
    std::vector<bool> keep (pts.size(), true);
    {
        std::array<double, 46> hist {};                        // 2 ms slots, −45 … +45 ms
        for (auto [x, y] : pts)
        {
            const double d = (y - (first + beat * x)) * 1000.0;
            hist[(size_t) juce::jlimit (0, 45, (int) std::floor ((d + 45.0) / 2.0))] += 1.0;
        }
        int bi = 0; double bv = -1.0;
        for (int i = 1; i + 1 < 46; ++i)
            if (const double v = hist[(size_t) i - 1] * 0.5 + hist[(size_t) i] + hist[(size_t) i + 1] * 0.5; v > bv) { bv = v; bi = i; }
        a = first + (bi * 2.0 - 45.0 + 1.0) / 1000.0;
        if (dbg)
        {
            std::cerr << "[fit] histograma:";
            for (int i = 0; i < 46; ++i) std::cerr << " " << (int) hist[(size_t) i];
            std::cerr << "  moda " << (bi * 2.0 - 45.0 + 1.0) << " ms" << std::endl;
        }
    }
    for (int it = 0; it < 5; ++it)
    {
        if (it == 0)
        {
            for (size_t i = 0; i < pts.size(); ++i) keep[i] = std::abs (pts[i].second - (a + b * pts[i].first)) < 0.010;
            continue;
        }
        double sn = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < pts.size(); ++i)
            if (keep[i]) { const auto [x, y] = pts[i]; sn += 1; sx += x; sy += y; sxx += x * x; sxy += x * y; }
        const double den = sn * sxx - sx * sx;
        if (sn < 32 || std::abs (den) < 1e-9) return false;
        b = (sn * sxy - sx * sy) / den;
        a = (sy - b * sx) / sn;
        const double tol = 0.012;
        for (size_t i = 0; i < pts.size(); ++i) keep[i] = std::abs (pts[i].second - (a + b * pts[i].first)) < tol;
    }
    if (dbg) std::cerr << "[fit] bpm " << 60.0 / beat << " -> " << 60.0 / b << " a " << a << " fase vieja " << first << std::endl;
    if (b <= 0.0 || std::abs (b / beat - 1.0) > 0.003) return false;         // only a fine correction
    double newBpm = 60.0 / b;
    // Produced music sits on round tempos: keep the round value when the fit is that close to it.
    if (std::abs (newBpm - std::round (newBpm)) < 0.015) newBpm = std::round (newBpm);
    else newBpm = std::round (newBpm * 100.0) / 100.0;
    // Re-anchor the line on the hits with the final tempo (median residual).
    const double nb = 60.0 / newBpm;
    std::vector<double> res;
    for (size_t i = 0; i < pts.size(); ++i) if (keep[i]) res.push_back (pts[i].second - (a + nb * pts[i].first));
    if (res.size() < 32) return false;
    std::nth_element (res.begin(), res.begin() + (long) res.size() / 2, res.end());
    bpm = newBpm;
    phase = a + res[res.size() / 2];
    return true;
}

// Fine tempo (±0.6 %) and phase (≈1 ms) from the kick attacks of the whole track.
static void refineTempoAndPhase (const Features& f, double& bpm, double& phase)
{
    double beatSec = 60.0 / bpm;

    // 5. Refine the phase on the kick attacks: low band (30–150 Hz) energy rise at ~1 ms
    //    resolution, median offset over all beats, two passes. Kicks sit on the beat;
    //    hi-hats and claps (often off-beat) are excluded by the band limit.
    {
        const auto env = kickEnvelopes (f);
        const auto& rise = env.kick;
        const auto& full = env.full;
        const int frames = (int) rise.size();
        const double envFps = env.fps;

        // Fine BPM: fold the kick-attack envelope onto one beat for candidate tempi
        // (±0.6 % in 0.01 BPM steps); the true tempo gives the sharpest peak. Robust to
        // skipped or mis-tracked beats, unlike the regression alone.
        {
            constexpr int bins = 240;
            double bestSharp = -1.0, bestBpm = bpm, bestPhase = phase;
            std::vector<double> hist (bins);
            auto evaluate = [&] (double cand, double& sharpOut, double& phaseOut)
            {
                std::fill (hist.begin(), hist.end(), 0.0);
                const double beatFrames = envFps * 60.0 / cand;
                double total = 0.0;
                for (int i = 0; i < frames; ++i)
                {
                    const float v = rise[(size_t) i];
                    if (v <= 0.2f) continue;
                    const double ph = std::fmod ((double) i, beatFrames) / beatFrames;
                    hist[(size_t) juce::jlimit (0, bins - 1, (int) (ph * bins))] += v;
                    total += v;
                }
                int bi = 0;
                double bv = -1.0;
                for (int b = 0; b < bins; ++b)
                {
                    const double v3 = hist[(size_t) b] + 0.5 * (hist[(size_t) ((b + 1) % bins)] + hist[(size_t) ((b + bins - 1) % bins)]);
                    if (v3 > bv) { bv = v3; bi = b; }
                }
                sharpOut = total > 0 ? bv / total : 0.0;
                phaseOut = (bi + 0.5) / bins * (60.0 / cand);
            };

            for (double cand = bpm * 0.994; cand <= bpm * 1.006; cand += 0.01)
            {
                double sh, ph;
                evaluate (cand, sh, ph);
                if (sh > bestSharp) { bestSharp = sh; bestBpm = cand; bestPhase = ph; }
            }

            // Prefer the round tempo when it is (almost) as good — produced tracks sit on the grid.
            const double rounded = std::round (bestBpm);
            if (std::abs (rounded - bestBpm) < 0.06)
            {
                double sh, ph;
                evaluate (rounded, sh, ph);
                if (sh >= bestSharp * 0.97) { bestBpm = rounded; bestPhase = ph; }
            }

            if (bestSharp > 0.0)
            {
                bpm = std::round (bestBpm * 100.0) / 100.0;
                beatSec = 60.0 / bpm;
                // The folded peak sits on a beat; keep it in the tracked beat family (≤ ½ beat move).
                double d = std::fmod (bestPhase - phase, beatSec);
                if (d > beatSec / 2) d -= beatSec;
                if (d < -beatSec / 2) d += beatSec;
                if (std::abs (d) < beatSec * 0.2) phase += d;
            }
        }

        // Anchor on the kicks: fold the strong kick attacks of the whole track onto one beat
        // (64 slots, peaks only, weighted by strength²). If most of them sit away from the tracked
        // phase — half a beat when hi-hats or claps led the tracker, a quarter on syncopated
        // patterns — move the grid there.
        {
            constexpr int slots = 64;
            std::array<double, slots> h {};
            const double beatF = envFps * beatSec, startF = phase * envFps;
            for (int i = 1; i + 1 < frames; ++i)
            {
                const float v = rise[(size_t) i];
                if (v <= 0.5f || v < rise[(size_t) i - 1] || v < rise[(size_t) i + 1]) continue;
                double ph = std::fmod ((double) i - startF, beatF) / beatF;
                if (ph < 0.0) ph += 1.0;
                h[(size_t) ((int) (ph * slots) % slots)] += (double) v * v;
            }
            auto around = [&h] (int c) { double sum = 0; for (int k = -2; k <= 2; ++k) sum += h[(size_t) ((c + k + slots) % slots)]; return sum; };
            int best = 0;
            for (int c = 1; c < slots; ++c) if (around (c) > around (best)) best = c;
            const int dist = juce::jmin (best, slots - best);
            if (dist > 3 && around (best) > around (0) * 1.15)
                phase += (best <= slots / 2 ? best : best - slots) * beatSec / slots;
        }

        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<double> offs;
            const int search = (int) ((pass == 0 ? 0.06 : 0.025) * envFps);
            for (double t = phase; t < (double) f.mono.size() / f.rate; t += beatSec)
            {
                const int c = (int) std::round (t * envFps);
                if (c - search < 1 || c + search >= frames) continue;
                // Prefer the kick band; fall back to full band when the track has no kick.
                const auto& env = rise;
                int bestK = 0; float bestV = 0.0f;
                for (int k = -search; k <= search; ++k)
                    if (env[(size_t) (c + k)] > bestV) { bestV = env[(size_t) (c + k)]; bestK = k; }
                if (bestV < 0.3f)
                {
                    bestV = 0.0f;
                    for (int k = -search; k <= search; ++k)
                        if (full[(size_t) (c + k)] > bestV) { bestV = full[(size_t) (c + k)]; bestK = k; }
                }
                if (bestV > 0.3f) offs.push_back (bestK / envFps);
            }
            if (offs.size() > 16)
            {
                std::nth_element (offs.begin(), offs.begin() + (long) offs.size() / 2, offs.end());
                phase += offs[offs.size() / 2];
            }
        }
    }


}

BeatGrid refineGrid (const juce::AudioBuffer<float>& audio, double sr, BeatGrid approx)
{
    Features f;
    if (! approx.isValid() || sr <= 0.0 || ! computeFeatures (audio, sr, nullptr, f))
        return approx;
    double bpm = approx.bpm, phase = approx.firstBeatSeconds;
    refineTempoAndPhase (f, bpm, phase);
    for (int pass = 0; pass < 2; ++pass)                       // same attack fit as the analysis
    {
        const double d = attackPhaseShift (audio, sr, bpm, phase);
        if (std::abs (d) < 0.029) phase += d;
    }
    phase += attackCalibrationSec;
    if (! fitGridToDrawnHits (audio, sr, bpm, phase))          // exact tempo and phase from every hit
        if (const double d = drawnHitStart (audio, sr, bpm, phase); std::abs (d) > 0.003 && std::abs (d) < 0.040)
            phase += d;                                        // too few clear hits: just the median start
    // Keep the DJ's bar: the refined beat nearest to the old beat 1, as the new beat 1.
    const double beat = 60.0 / bpm;
    const double k = std::round ((approx.firstBeatSeconds - phase) / beat);
    double first = phase + k * beat;
    while (first < 0.0) first += 4.0 * beat;
    return { bpm, std::fmod (first, 4.0 * beat) };
}

BeatGrid detectBeatGrid (const juce::AudioBuffer<float>& audio, double sr, const std::atomic<bool>* abortFlag)
{
    BeatGrid grid;
    Features f;
    if (sr <= 0.0 || ! computeFeatures (audio, sr, abortFlag, f))
        return grid;

    // 2–4. Tempo → tracked beats → exact period/phase.
    const double bpm0 = estimateTempo (f);
    const auto beats = trackBeats (f.odf, f.fps * 60.0 / bpm0);
    double a = 0.0, period = f.fps * 60.0 / bpm0;
    if (! fitGrid (beats, period, a, period))
    {
        a = beats.empty() ? 0.0 : beats.front();
        period = f.fps * 60.0 / bpm0;
    }
    if (aborted (abortFlag)) return grid;

    double bpm = 60.0 * f.fps / period;
    if (std::abs (bpm - std::round (bpm)) < 0.025) bpm = std::round (bpm);
    else bpm = std::round (bpm * 100.0) / 100.0;
    double beatSec = 60.0 / bpm;

    // STFT frame f is centred at (f·hop + size/2) samples.
    const double frameOffsetSec = 512.0 / f.rate;
    double phase = a / f.fps + frameOffsetSec;

    refineTempoAndPhase (f, bpm, phase);
    beatSec = 60.0 / bpm;

    // Lines on the hits' attacks, fitted on the whole track at 0.25 ms (twice, to settle).
    for (int pass = 0; pass < 2; ++pass)
    {
        const double d = attackPhaseShift (audio, sr, bpm, phase);
        if (std::abs (d) < 0.029) phase += d;
    }
    phase += attackCalibrationSec;
    if (! fitGridToDrawnHits (audio, sr, bpm, phase))          // exact tempo and phase from every hit
        if (const double d = drawnHitStart (audio, sr, bpm, phase); std::abs (d) > 0.003 && std::abs (d) < 0.040)
            phase += d;                                        // too few clear hits: just the median start

    // BPM range 70–160 (the DJ's choice): a double- or half-time reading is folded into it.
    while (bpm > 160.0)
    {
        // Half the tempo keeps every other line: the set that carries the kicks.
        auto kickAt = [&] (double ph)
        {
            double sum = 0;
            for (double t = ph; t * f.fps < (double) f.lowOdf.size() - 1; t += 2.0 * beatSec)
                sum += f.lowOdf[(size_t) juce::jlimit (0, (int) f.lowOdf.size() - 1, (int) std::round (t * f.fps))];
            return sum;
        };
        if (kickAt (phase + beatSec) > kickAt (phase)) phase += beatSec;
        bpm *= 0.5;
        beatSec = 60.0 / bpm;
    }
    while (bpm > 1.0 && bpm < 70.0) { bpm *= 2.0; beatSec = 60.0 / bpm; }
    beatSec = 60.0 / bpm;

    // 6. Downbeat: harmonic change and low-band onsets favour the "1" of the bar.
    {
        // Phase may be negative (the first line before the file starts): wrap it, never clamp it.
        const double firstBeat = std::fmod (std::fmod (phase, beatSec) + beatSec, beatSec);
        const int nBeats = (int) (((double) f.odf.size() / f.fps - firstBeat) / beatSec) - 1;
        std::vector<std::array<float, 12>> beatChroma ((size_t) juce::jmax (0, nBeats));
        std::vector<float> beatLow ((size_t) juce::jmax (0, nBeats), 0.0f);
        for (int b = 0; b < nBeats; ++b)
        {
            const int f0 = (int) ((firstBeat + b * beatSec) * f.fps), f1 = (int) ((firstBeat + (b + 1) * beatSec) * f.fps);
            auto& c = beatChroma[(size_t) b];
            for (int fr = juce::jmax (0, f0); fr < juce::jmin ((int) f.chroma.size(), f1); ++fr)
                for (int p = 0; p < 12; ++p) c[(size_t) p] += f.chroma[(size_t) fr][(size_t) p];
            for (int fr = juce::jmax (0, f0 - 2); fr <= juce::jmin ((int) f.lowOdf.size() - 1, f0 + 2); ++fr)
                beatLow[(size_t) b] = juce::jmax (beatLow[(size_t) b], f.lowOdf[(size_t) fr]);
        }
        std::array<double, 4> score {};
        for (int b = 1; b < nBeats; ++b)
        {
            const auto& c0 = beatChroma[(size_t) b - 1];
            const auto& c1 = beatChroma[(size_t) b];
            double dot = 0, n0 = 0, n1 = 0;
            for (int p = 0; p < 12; ++p) { dot += c0[(size_t) p] * c1[(size_t) p]; n0 += c0[(size_t) p] * c0[(size_t) p]; n1 += c1[(size_t) p] * c1[(size_t) p]; }
            const double novelty = n0 > 0 && n1 > 0 ? 1.0 - dot / std::sqrt (n0 * n1) : 0.0;
            score[(size_t) (b % 4)] += novelty + 0.02 * beatLow[(size_t) b];
        }
        int down = 0;
        for (int k = 1; k < 4; ++k) if (score[(size_t) k] > score[(size_t) down]) down = k;
        phase = firstBeat + down * beatSec;

        // DJ-friendly tracks (extended mixes, edits, intros) start right on a "1": when the music
        // starts with a strong hit on the grid, that hit is the downbeat.
        if (! f.odf.empty())
        {
            std::vector<float> sorted (f.odf);
            const auto p95 = sorted.begin() + (long) (sorted.size() * 95 / 100);
            std::nth_element (sorted.begin(), p95, sorted.end());
            const float strong = *p95 * 0.35f;
            int startFrame = -1;
            for (int fr = 0; fr < (int) f.odf.size() && fr < (int) (30.0 * f.fps); ++fr)
                if (f.odf[(size_t) fr] >= strong) { startFrame = fr; break; }
            if (startFrame >= 0)
            {
                const double t0 = startFrame / f.fps;
                const double k = std::round ((t0 - firstBeat) / beatSec);
                if (k >= 0 && std::abs (t0 - (firstBeat + k * beatSec)) < beatSec * 0.2)
                    phase = firstBeat + k * beatSec;
            }
        }
    }

    grid.bpm = bpm;
    grid.firstBeatSeconds = std::fmod (std::fmod (phase, 4.0 * beatSec) + 4.0 * beatSec, 4.0 * beatSec);   // a downbeat, within the first bar
    return grid;
}
//==============================================================================
static juce::String camelotFor (int tonicPc, bool minor)
{
    const int pc = minor ? (tonicPc + 3) % 12 : tonicPc;   // minor shares the number of its relative major
    const int number = (pc * 7 + 7) % 12 + 1;
    return juce::String (number) + (minor ? "A" : "B");
}

juce::String detectKey (const juce::AudioBuffer<float>& audio, double sr, const std::atomic<bool>* abortFlag)
{
    const int n = audio.getNumSamples();
    constexpr int order = 13, size = 1 << order;
    if (sr <= 0.0 || n < size * 4)
        return {};

    // Work at ~11 kHz: plenty for pitch classes up to ~2 kHz, and cheap.
    const int factor = juce::jmax (1, (int) std::floor (sr / 11025.0));
    const double rate = sr / factor;
    std::vector<float> mono ((size_t) (n / factor));
    for (size_t i = 0; i < mono.size(); ++i)
    {
        float acc = 0.0f;
        for (int k = 0; k < factor; ++k)
            acc += monoSample (audio, (int) i * factor + k);
        mono[i] = acc / (float) factor;
    }

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), buf ((size_t) size * 2);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    // Precompute bin → pitch class (55 Hz … 2 kHz).
    std::vector<int> binPc ((size_t) size / 2, -1);
    for (int k = 1; k < size / 2; ++k)
    {
        const double f = (double) k * rate / size;
        if (f < 55.0 || f > 2000.0) continue;
        const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
        binPc[(size_t) k] = ((int) std::lround (midi) % 12 + 12) % 12;
    }

    std::array<double, 12> chroma {};
    double totalEnergy = 0.0;
    const int hop = size / 2;

    for (size_t start = 0; start + (size_t) size <= mono.size(); start += (size_t) hop)
    {
        if (abortFlag != nullptr && abortFlag->load())
            return {};

        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = mono[start + (size_t) i] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);

        std::array<double, 12> frame {};
        double e = 0.0;
        for (int k = 1; k < size / 2; ++k)
            if (const int pc = binPc[(size_t) k]; pc >= 0)
            {
                const double m = buf[(size_t) k];
                frame[(size_t) pc] += m;
                e += m;
            }

        if (e <= 1.0e-6) continue;
        totalEnergy += e;
        for (int pc = 0; pc < 12; ++pc)
            chroma[(size_t) pc] += std::sqrt (frame[(size_t) pc] / e); // per-frame normalised, compressed
    }

    if (totalEnergy < 1.0e-3)
        return {};

    static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    auto correlate = [&chroma] (const double* profile, int tonic)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) ((i + tonic) % 12)]; my += profile[i]; }
        mx /= 12; my /= 12;
        double num = 0, dx = 0, dy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = chroma[(size_t) ((i + tonic) % 12)] - mx, y = profile[i] - my;
            num += x * y; dx += x * x; dy += y * y;
        }
        return dx > 0 && dy > 0 ? num / std::sqrt (dx * dy) : 0.0;
    };

    double best = -2.0;
    int bestPc = 0;
    bool bestMinor = false;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        if (const double c = correlate (major, tonic); c > best) { best = c; bestPc = tonic; bestMinor = false; }
        if (const double c = correlate (minor, tonic); c > best) { best = c; bestPc = tonic; bestMinor = true; }
    }

    return best > 0.3 ? camelotFor (bestPc, bestMinor) : juce::String();
}

int mp3TrimmedLeadSamples (const juce::File& file)
{
    if (! file.hasFileExtension ("mp3"))
        return 0;

    juce::FileInputStream in (file);
    if (! in.openedOk())
        return 0;

    // Skip an ID3v2 tag, then look at the first MPEG frame for a Xing/Info + LAME header.
    juce::uint8 id3[10] {};
    juce::int64 start = 0;
    if (in.read (id3, 10) == 10 && id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3')
        start = 10 + ((id3[6] & 0x7f) << 21 | (id3[7] & 0x7f) << 14 | (id3[8] & 0x7f) << 7 | (id3[9] & 0x7f));
    in.setPosition (start);
    juce::MemoryBlock frame;
    in.readIntoMemoryBlock (frame, 4096);
    const auto* p = static_cast<const juce::uint8*> (frame.getData());
    const size_t n = frame.getSize();

    size_t xing = 0;
    for (size_t i = 0; i + 4 < n && i < 2048; ++i)
        if (std::memcmp (p + i, "Xing", 4) == 0 || std::memcmp (p + i, "Info", 4) == 0) { xing = i; break; }
    if (xing == 0)
        return 0;

    // Club players count the decoder delay (529) at the start, plus the encoder delay LAME declares;
    // this decoder trims both (gapless). FFmpeg-made files ("Lavc"/"Lavf") and Xing/Info frames without
    // encoder data only differ by the decoder delay. Measured on 200 tracks against exported USB grids.
    for (size_t i = xing; i + 24 < n && i < xing + 400; ++i)
    {
        if (std::memcmp (p + i, "LAME", 4) == 0)
        {
            const int delay = (p[i + 21] << 4) | (p[i + 22] >> 4);
            return delay > 0 && delay < 4096 ? delay + 529 : 529;
        }
        if (std::memcmp (p + i, "Lavc", 4) == 0 || std::memcmp (p + i, "Lavf", 4) == 0)
            return 529;
    }
    return 529;
}

double integratedLoudness (const juce::AudioBuffer<float>& audio, double fs)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || fs <= 0.0) return -100.0;

    // K-weighting for any sample rate (same derivation as libebur128).
    struct Biquad { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
        double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };

    auto makeShelf = [fs]
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        return Biquad { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                        2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
    };
    auto makeHighPass = [fs]
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        return Biquad { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
    };

    const int chans = juce::jmin (2, audio.getNumChannels());
    const int step = (int) std::round (fs * 0.1);                // 100 ms hops, 400 ms blocks (75 % overlap)
    const int hops = n / step;
    std::vector<double> hopEnergy ((size_t) hops, 0.0);

    for (int c = 0; c < chans; ++c)
    {
        auto shelf = makeShelf();
        auto hp = makeHighPass();
        const float* x = audio.getReadPointer (c);
        for (int h = 0; h < hops; ++h)
        {
            double acc = 0.0;
            for (int i = h * step; i < (h + 1) * step; ++i)
            {
                const double y = hp.run (shelf.run (x[i]));
                acc += y * y;
            }
            hopEnergy[(size_t) h] += acc;
        }
    }

    std::vector<double> blocks;
    for (int h = 0; h + 4 <= hops; ++h)
    {
        const double ms = (hopEnergy[(size_t) h] + hopEnergy[(size_t) h + 1] + hopEnergy[(size_t) h + 2] + hopEnergy[(size_t) h + 3]) / (4.0 * step);
        blocks.push_back (ms);
    }

    auto lufs = [] (double ms) { return -0.691 + 10.0 * std::log10 (juce::jmax (1e-12, ms)); };
    auto gatedMean = [&] (double gateLufs)
    {
        double sum = 0.0; int count = 0;
        for (auto b : blocks) if (lufs (b) > gateLufs) { sum += b; ++count; }
        return count > 0 ? sum / count : 0.0;
    };

    const double abs = gatedMean (-70.0);
    if (abs <= 0.0) return -100.0;
    const double rel = gatedMean (lufs (abs) - 10.0);
    return rel > 0.0 ? lufs (rel) : -100.0;
}

juce::String camelotToMusical (const juce::String& camelot)
{
    const int number = camelot.getIntValue();
    const bool minor = camelot.endsWithIgnoreCase ("A");
    if (number < 1 || number > 12) return camelot;
    static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    // Invert number = (pc*7+7)%12+1  →  pc = (number-8)*7 mod 12
    const int majorPc = (((number - 8) * 7) % 12 + 12) % 12;
    const int pc = minor ? (majorPc + 9) % 12 : majorPc;
    return juce::String (names[pc]) + (minor ? "m" : "");
}

juce::String transposeCamelot (const juce::String& camelot, int semitones)
{
    const int number = camelot.getIntValue();
    if (number < 1 || number > 12 || ! (camelot.endsWithIgnoreCase ("A") || camelot.endsWithIgnoreCase ("B")))
        return camelot;
    const int shifted = ((number - 1 + semitones * 7) % 12 + 12) % 12 + 1;
    return juce::String (shifted) + camelot.getLastCharacters (1).toUpperCase();
}

int semitonesToMatch (const juce::String& key, const juce::String& target)
{
    const int a = key.getIntValue(), b = target.getIntValue();
    if (a < 1 || b < 1) return 0;
    // Same Camelot number (incl. relative major/minor) is harmonically identical.
    for (int s = 0; s <= 6; ++s)
    {
        if (transposeCamelot (key, s).getIntValue() == b)  return s;
        if (transposeCamelot (key, -s).getIntValue() == b) return -s;
    }
    return 0;
}

bool camelotCompatible (const juce::String& a, const juce::String& b)
{
    const int na = a.getIntValue(), nb = b.getIntValue();
    if (na < 1 || nb < 1) return false;
    const bool sameLetter = a.getLastCharacter() == b.getLastCharacter();
    const int diff = std::abs (na - nb);
    return (sameLetter && (diff == 0 || diff == 1 || diff == 11)) || (! sameLetter && diff == 0);
}

} // namespace om::Analysis
