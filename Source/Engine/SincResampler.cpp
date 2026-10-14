#include "SincResampler.h"

namespace om
{

static double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

const SincResampler& SincResampler::get()
{
    static const SincResampler instance;
    return instance;
}

SincResampler::SincResampler()
{
    // Cutoff relative to the source Nyquist for each speed band (slightly below
    // 1 / speed so the transition band stays out of the audible alias region).
    const double cutoffs[5] = { 0.95, 0.95 / 1.25, 0.95 / 1.6, 0.95 / 2.2, 0.95 / 4.0 };
    const double beta = 9.0;                           // ≈ −90 dB stop band
    const double i0b = besselI0 (beta);

    for (size_t t = 0; t < coeffs.size(); ++t)
    {
        auto& table = coeffs[t];
        table.assign ((size_t) (phases + 1) * taps, 0.0f);
        const double fc = cutoffs[t];

        for (int p = 0; p <= phases; ++p)
        {
            const double frac = (double) p / phases;
            double sum = 0.0;
            std::array<double, taps> row {};
            for (int k = 0; k < taps; ++k)
            {
                // Tap k sits at source index (i - half + 1 + k); distance from pos:
                const double x = (double) (k - half + 1) - frac;
                const double arg = juce::MathConstants<double>::pi * fc * x;
                const double sinc = std::abs (x) < 1e-9 ? 1.0 : std::sin (arg) / arg;
                const double w = x / (double) half;
                const double win = std::abs (w) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - w * w)) / i0b;
                row[(size_t) k] = fc * sinc * win;
                sum += row[(size_t) k];
            }
            for (int k = 0; k < taps; ++k)
                table[(size_t) p * taps + (size_t) k] = (float) (row[(size_t) k] / sum);   // unity DC gain
        }
    }
}

} // namespace om
