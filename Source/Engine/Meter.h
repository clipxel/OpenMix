#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace om
{

/** Lock-free stereo peak meter. The audio thread accumulates peaks; the UI
    drains them at its own frame rate and applies ballistics. Clip latches until read. */
class MeterState
{
public:
    void push (const float* l, const float* r, int n) noexcept
    {
        auto peak = [n] (const float* d)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (d, n);
            return juce::jmax (range.getEnd(), -range.getStart());
        };
        const auto pl = peak (l);
        const auto pr = peak (r);

        if (pl > peakL.load (std::memory_order_relaxed)) peakL.store (pl, std::memory_order_relaxed);
        if (pr > peakR.load (std::memory_order_relaxed)) peakR.store (pr, std::memory_order_relaxed);
        const auto pm = juce::jmax (pl, pr);
        if (pm > peakCtl.load (std::memory_order_relaxed)) peakCtl.store (pm, std::memory_order_relaxed);
        if (pl >= clipThreshold || pr >= clipThreshold) clipped.store (true, std::memory_order_relaxed);
    }

    /** UI side: returns peaks since last call and resets them. */
    std::pair<float, float> drain() noexcept   { return { peakL.exchange (0.0f), peakR.exchange (0.0f) }; }
    bool drainClip() noexcept                  { return clipped.exchange (false); }
    /** Controller VU LEDs: a second, independent reader (louder channel, since its last call). */
    float drainController() noexcept           { return peakCtl.exchange (0.0f); }

    static constexpr float clipThreshold = 0.999f; // ≈ 0 dBFS

private:
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f }, peakCtl { 0.0f };
    std::atomic<bool> clipped { false };
};

} // namespace om
