#include "MasterLimiter.h"

namespace om
{

void MasterLimiter::prepare (double sampleRate, int)
{
    sr = sampleRate;
    lookahead = juce::jmax (8, (int) std::round (sampleRate * 0.0015));   // 1.5 ms
    tpDelay = tpTaps / 2;
    delay = lookahead - 1 + tpDelay;

    dl.assign ((size_t) delay + 1, 0.0f);
    dr.assign ((size_t) delay + 1, 0.0f);
    writePos = 0;

    // Windowed-sinc interpolators at 1/4, 2/4, 3/4 sample (ITU-R BS.1770 style true-peak).
    for (int p = 1; p < tpPhases; ++p)
    {
        const double frac = (double) p / tpPhases;
        double sum = 0.0;
        for (int k = 0; k < tpTaps; ++k)
        {
            const double x = (double) (k - tpTaps / 2 + 1) - frac;   // taps around the interval centre
            const double sinc = std::abs (x) < 1e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            const double w = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * x / (tpTaps / 2.0));
            tpCoeffs[(size_t) p - 1][(size_t) k] = (float) (sinc * w);
            sum += sinc * w;
        }
        for (auto& c : tpCoeffs[(size_t) p - 1]) c = (float) (c / sum);
    }
    histL.assign (tpTaps, 0.0f);
    histR.assign (tpTaps, 0.0f);
    histPos = 0;

    minQ.clear();
    box.assign ((size_t) lookahead, 1.0f);
    boxSum = lookahead;
    boxPos = 0;
    sampleIndex = 0;
    gain = 1.0f;
}

void MasterLimiter::process (float* l, float* r, int n)
{
    const bool on = enabled.load();
    const float ceiling = juce::Decibels::decibelsToGain (ceilingDb.load());
    const float relCoef = (float) std::exp (-1.0 / (juce::jmax (5.0f, releaseMs.load()) * 0.001 * sr));
    float maxReduction = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        // 1. True-peak of the sample that is tpDelay samples old (needs future taps).
        histL[(size_t) histPos] = l[i];
        histR[(size_t) histPos] = r[i];
        histPos = (histPos + 1) % tpTaps;

        auto at = [this] (const std::vector<float>& h, int k) { return h[(size_t) ((histPos + k) % tpTaps)]; };
        float peak = juce::jmax (std::abs (at (histL, tpTaps / 2 - 1)), std::abs (at (histR, tpTaps / 2 - 1)));
        for (const auto& c : tpCoeffs)
        {
            float il = 0.0f, ir = 0.0f;
            for (int k = 0; k < tpTaps; ++k)
            {
                il += at (histL, k) * c[(size_t) k];
                ir += at (histR, k) * c[(size_t) k];
            }
            peak = juce::jmax (peak, std::abs (il), std::abs (ir));
        }

        const float required = peak > ceiling ? ceiling / peak : 1.0f;

        // 2. Sliding minimum over the look-ahead window.
        while (! minQ.empty() && minQ.back().second >= required) minQ.pop_back();
        minQ.emplace_back (sampleIndex, required);
        while (minQ.front().first <= sampleIndex - lookahead) minQ.pop_front();
        const float held = minQ.front().second;

        // 3. Box filter of the same length: reaches `held` exactly when the peak arrives.
        boxSum += held - box[(size_t) boxPos];
        box[(size_t) boxPos] = held;
        boxPos = (boxPos + 1) % lookahead;
        const float target = (float) (boxSum / lookahead);

        // 4. Instant attack (already smoothed), exponential release.
        gain = target < gain ? target : target + (gain - target) * relCoef;
        ++sampleIndex;

        // 5. Delay the audio so it meets its gain.
        const int readPos = (writePos + 1) % (delay + 1);
        dl[(size_t) writePos] = l[i];
        dr[(size_t) writePos] = r[i];
        const float outL = dl[(size_t) readPos], outR = dr[(size_t) readPos];
        writePos = readPos;

        const float g = on ? gain : 1.0f;
        l[i] = outL * g;
        r[i] = outR * g;

        if (on)
        {
            // Numerical safety net only; the look-ahead guarantees this is never hit in practice.
            l[i] = juce::jlimit (-ceiling, ceiling, l[i]);
            r[i] = juce::jlimit (-ceiling, ceiling, r[i]);
            maxReduction = juce::jmax (maxReduction, -juce::Decibels::gainToDecibels (gain, -60.0f));
        }
    }

    if (maxReduction > reductionDb.load())
        reductionDb.store (maxReduction);
}

} // namespace om
