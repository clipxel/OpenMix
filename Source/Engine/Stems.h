#pragma once

#include "LoadedTrack.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace om
{

/** Rápido: Spleeter 4 stems (Deezer), the whole track in a few seconds.
    Calidad: HT-Demucs, cleaner, about 15–25 s per track. */
enum class StemMode { fast = 0, quality = 1 };

/** The stems of one track, at 44.1 kHz (the rate both models run at). Vocals, drums and bass are stored;
    the melody (everything else) is the track minus those three, so the four always add up to the original.
    The separator writes in the background, the part playing first; the audio thread reads a block only
    once it is marked ready, and a ready block is never written again. */
struct StemData
{
    static constexpr double rate = 44100.0;
    static constexpr int blockLen = 2048;
    enum Part { vocals = 0, drums, bass, numStored };

    StemData (int numSamples, StemMode);

    const int n;
    const StemMode mode;
    std::array<juce::AudioBuffer<float>, numStored> parts;          // 2 channels each
    std::array<std::array<float*, 2>, numStored> write {};          // raw pointers (taken once, no flag races)
    std::array<std::array<const float*, 2>, numStored> read {};

    bool readyAt (double sample) const noexcept
    {
        const auto b = (int) (sample / blockLen);
        return b >= 0 && b < numBlocks && ready[(size_t) b].load (std::memory_order_acquire);
    }
    float progress() const noexcept   { return numBlocks > 0 ? (float) readyCount.load() / (float) numBlocks : 0.0f; }
    bool  isComplete() const noexcept { return readyCount.load() >= numBlocks; }
    bool  hasFailed() const noexcept  { return failed.load(); }
    juce::String getError() const     { const std::lock_guard<std::mutex> l (errorLock); return error; }

    const int numBlocks;
    std::unique_ptr<std::atomic<bool>[]> ready;
    std::atomic<int> readyCount { 0 };
    std::atomic<bool> failed { false };
    mutable std::mutex errorLock;
    juce::String error;
    void fail (const juce::String& e) { { const std::lock_guard<std::mutex> l (errorLock); error = e; } failed = true; }
};

using StemDataPtr = std::shared_ptr<StemData>;

/** The models' arithmetic, kept apart from ONNX Runtime so it can be tested with stand-in networks. */
namespace StemDsp
{
    // HT-Demucs: 7.8 s segments, 25 % overlap with linear fades.
    constexpr int demucsLen = 343980, demucsOverlap = demucsLen / 4, demucsStride = demucsLen - demucsOverlap;
    // Spleeter: STFT 4096 / 1024 (periodic Hann), 512-frame splits, 1536 bins modelled (to 16 kHz).
    constexpr int fftSize = 4096, hop = 1024, pad = fftSize - hop, splitFrames = 512, bins = 2049, netBins = 1536;

    /** Stand-in for the networks. demucs: in 2×N planar → out 4×2×N (drums, bass, other, vocals).
        spleeter: stem 0…3 (vocals, drums, bass, other), in/out 2×512×1536 magnitudes. */
    struct Nets
    {
        std::function<void (const float* in, float* out)> demucs;
        std::function<void (int stem, const float* in, float* out)> spleeter;
    };

    /** One separation's plan: units of work (segments / splits), each adding to a range of samples. */
    struct Plan
    {
        Plan (StemMode, int numSamples);
        StemMode mode;
        int n = 0, numUnits = 0, numFrames = 0;
        std::vector<float> norm;          // demucs: 1 / Σ weights per sample · spleeter: 1 / Σ window² per padded sample
        std::pair<int, int> range (int unit) const noexcept;   // output samples the unit adds to
        int unitAt (double sample) const noexcept;
    };

    /** Runs one unit and adds its result into `out` (holding `addLock` while it adds). */
    void processUnit (const Plan&, int unit, const float* const* mix, StemData& out, const Nets&, std::mutex& addLock);

    /** Whole track, synchronously (tests). */
    void separateAll (const float* const* mix, int numSamples, StemData& out, const Nets&);

    /** After `unit` finished: marks the blocks every unit touching them has finished. */
    void markReady (const Plan&, const std::vector<uint8_t>& unitDone, int unit, StemData&);
}

/** Background separation with ONNX Runtime (loaded at run time; without it stems are just unavailable).
    Workers run below normal priority and never spin, so the audio thread is never starved. */
class StemSeparator
{
public:
    StemSeparator();
    ~StemSeparator();

    static juce::File folder();
    /** ONNX Runtime found and loaded (it ships with the app; Apple Silicon, Windows x64 and Linux x64). */
    static bool runtimeAvailable();
    static juce::String runtimeError();

    bool modelInstalled (StemMode) const;
    juce::int64 downloadSize (StemMode) const;
    /** Downloads the models of `mode` (verified with SHA-256). Callbacks on the message thread. */
    void install (StemMode, std::function<void (float)> progress, std::function<void (juce::String error)> done);
    bool isInstalling() const noexcept { return installing.load(); }

    /** Starts separating `track` (or returns what is already being made for it). The part at `playhead`
        (seconds, read from the workers) is separated first. */
    StemDataPtr separate (LoadedTrackPtr track, StemMode, std::function<double()> playhead);

private:
    struct Job;
    struct Worker;
    friend struct Worker;

    struct Pick { std::shared_ptr<Job> job; StemDataPtr out; int unit = -1; };
    Pick pick();
    void finish (const Pick&, bool ok, const juce::String& error);

    std::mutex lock;
    std::condition_variable wake;
    std::vector<std::shared_ptr<Job>> jobs;
    std::vector<std::unique_ptr<Worker>> workers;
    int threadBudget = 4;
    std::atomic<bool> installing { false }, quitting { false };
};

} // namespace om
