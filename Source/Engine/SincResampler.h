#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace om
{

/** Band-limited interpolation for variable-rate playback (pitch, sample-rate
    conversion, scratch). 32-tap Kaiser-windowed sinc, 1024 phases with linear
    phase interpolation. Several cutoff tables are kept so that playing faster than
    real time (pitch up, 48 k → 44.1 k) is low-passed instead of aliasing. */
class SincResampler
{
public:
    static constexpr int taps = 32;
    static constexpr int half = taps / 2;
    static constexpr int phases = 1024;

    static const SincResampler& get();

    /** Interpolates `data` (length n) at fractional position `pos` for a playback
        speed of `rate` source samples per output sample. */
    float interpolate (const float* data, int n, double pos, double rate) const noexcept
    {
        const int t = tableFor (std::abs (rate));
        const int i = (int) std::floor (pos);
        const float phase = (float) (pos - (double) i) * (float) phases;
        const int p = juce::jmin (phases - 1, (int) phase);
        const float f = phase - (float) p;
        const float* c0 = coeffs[(size_t) t].data() + (size_t) p * taps;
        const float* c1 = c0 + taps;

        float acc = 0.0f;
        const int first = i - half + 1;
        if (first >= 0 && first + taps <= n)
        {
            const float* x = data + first;
            for (int k = 0; k < taps; ++k)
                acc += x[k] * (c0[k] + f * (c1[k] - c0[k]));
        }
        else
        {
            for (int k = 0; k < taps; ++k)
            {
                const int idx = first + k;
                if (idx >= 0 && idx < n)
                    acc += data[idx] * (c0[k] + f * (c1[k] - c0[k]));
            }
        }
        return acc;
    }

private:
    SincResampler();

    static int tableFor (double speed) noexcept
    {
        if (speed <= 1.0)  return 0;    // full band (0.95 Nyquist)
        if (speed <= 1.25) return 1;
        if (speed <= 1.6)  return 2;
        if (speed <= 2.2)  return 3;
        return 4;                        // scratch / extreme speeds
    }

    std::array<std::vector<float>, 5> coeffs;   // [(phases + 1) * taps]
};

} // namespace om
