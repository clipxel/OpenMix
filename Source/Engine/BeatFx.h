#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>
#include <array>
#include <algorithm>

namespace om
{

/** BEAT FX — one effect slot of a channel (3 per deck), modelled on the beat effects of
    club mixers and their usual behaviour:

      DELAY · ECHO · PING PONG · SPIRAL · REVERB · TRANS · FILTER · FLANGER · PHASER · PITCH ·
      SLIP ROLL · ROLL · VINYL BRAKE · HELIX

    BEAT (param) selects the beat fraction (1/16 … 16, or … 64 for FILTER / FLANGER / PHASER;
    REVERB 1–100 %, PITCH −50 … +100 %). LEVEL/DEPTH (wet) does what a club mixer does for each
    effect. Everything rhythmic is locked to the deck's beat grid (the position passed to process),
    so cuts, sweeps and rolls always land on the beat. The slot sits after the channel fader:
    DELAY / ECHO / PING PONG / SPIRAL / REVERB keep ringing when the fader closes or FX is switched off.

    Audio thread: process. Message / MIDI threads: the atomics. No allocation after prepare. */
class BeatFx
{
public:
    enum class Type
    {
        delay = 0, echo, pingPong, spiral, reverb, trans, filter, flanger, phaser, pitch,
        slipRoll, roll, vinylBrake, helix,
        tripletFilter, tripletRoll, mobius,
        numTypes
    };

    static juce::String typeName (Type);
    /** The 14 BEAT FX of the club mixer, in its selector's order. */
    static const std::array<Type, 14>& clubOrder()
    {
        static const std::array<Type, 14> o { Type::delay, Type::echo, Type::pingPong, Type::spiral, Type::helix, Type::reverb,
                                              Type::flanger, Type::phaser, Type::filter, Type::tripletFilter, Type::trans,
                                              Type::roll, Type::tripletRoll, Type::mobius };
        return o;
    }
    /** Next / previous effect on the club mixer's selector (wraps around). */
    static Type clubStep (Type current, int dir)
    {
        const auto& o = clubOrder();
        const auto it = std::find (o.begin(), o.end(), current);
        const int i = it == o.end() ? 0 : (int) (it - o.begin());
        return o[(size_t) ((i + dir + (int) o.size()) % (int) o.size())];
    }
    static juce::String paramName (Type);
    static juce::String paramText (Type, float param);
    static float defaultParam (Type);       // the BEAT value the effect starts on
    static float defaultLevel (Type);       // LEVEL/DEPTH it starts on
    static bool  isTransport (Type) noexcept      { return false; }

    std::atomic<int>   type { (int) Type::echo };
    std::atomic<bool>  enabled { false };
    std::atomic<float> wet { 0.5f };        // LEVEL/DEPTH
    std::atomic<float> param { 0.5f };      // BEAT (normalised 0 … 1)
    std::atomic<float> timeMs { 0.0f };     // TIME knob: > 0 = this many ms instead of the BEAT fraction

    void prepare (double sampleRate, int maxBlock);
    /** `beat` = deck beat position at the block's first sample (−1 = no grid / stopped),
        `beatsPerSample` its rate; without a grid the effect runs on its own clock at `bpm`. */
    void process (float* l, float* r, int n, double bpm, double beat = -1.0, double beatsPerSample = 0.0);

    /** Beats for the current BEAT setting (delay time, cut, cycle, roll length …). */
    static double beatsFor (Type, float param) noexcept;
    /** MOBIUS with a negative BEAT: the tone falls instead of rising. */
    static bool falling (Type, float param) noexcept;
    /** The BEAT values of an effect, as control values (for the X-PAD's positions). */
    static int beatSteps (Type) noexcept;
    static float paramAt (Type, int index) noexcept;
    /** BEAT ◀ ▶ on the mixer: the next beat value (or the next REVERB / PITCH step) in that direction. */
    static float stepBeat (Type, float param, int direction) noexcept;

private:
    static bool hasTail (Type t) noexcept
    {
        return t == Type::delay || t == Type::echo || t == Type::pingPong || t == Type::spiral || t == Type::reverb;
    }
    static float soft (float x) noexcept          { return x / (1.0f + 0.3f * std::abs (x)); }

    void resetState();
    float ringAt (const std::vector<float>& ring, double absPos) const noexcept;   // cubic, absolute index
    float delayRead (const std::vector<float>& line, double delaySamples) const noexcept;

    double sr = 48000.0;
    int lastType = -1;
    bool wasOn = false;
    juce::SmoothedValue<float> levelSm, inSm, mixSm;
    float tail = 0.0f;
    double freeBeat = 0.0;

    // Input history (ROLL / SLIP ROLL / VINYL BRAKE / HELIX) — absolute sample counter.
    std::vector<float> ringL, ringR;
    juce::int64 written = 0;

    // Delay lines (DELAY / ECHO / PING PONG / SPIRAL / FLANGER)
    std::vector<float> dl, dr;
    int dw = 0;
    double curDelay = 0.0, prevDelay = 0.0;
    int xfLeft = 0, xfLen = 1;
    double glideDelay = 0.0;                     // SPIRAL: delay time glides (pitch swoops)
    std::array<std::vector<float>, 4> diffuser;  // SPIRAL: all-pass diffusion inside the loop
    std::array<int, 4> diffPos {};

    // REVERB
    juce::Reverb reverb;
    std::vector<float> tmpL, tmpR;
    float revHpL = 0.0f, revHpR = 0.0f, revHpInL = 0.0f, revHpInR = 0.0f;

    // FILTER
    juce::dsp::StateVariableTPTFilter<float> svf;
    int svfCountdown = 0;

    // PHASER
    std::array<std::array<float, 2>, 6> ap {};
    float phFb[2] {};

    // FLANGER
    float flFb[2] {};

    // TRANS
    juce::SmoothedValue<float> gateSm;

    // PITCH: two crossfaded read heads over a short buffer
    std::vector<float> psL, psR;
    int psW = 0;
    double psPhase = 0.0;

    // ROLL / SLIP ROLL
    juce::int64 loopStart = 0;
    double loopLen = 0.0, loopBeats = 0.0;

    // VINYL BRAKE
    double brakeRead = 0.0, brakeElapsed = 0.0;

    // MOBIUS: octave-spaced sine partials gliding endlessly up (or down) once per BEAT cycle
    std::array<double, 7> mbPhase {};
    float mbLp = 0.0f;

    // HELIX
    std::vector<float> hxL, hxR;
    int hxLen = 0;
    double hxRead = 0.0, hxRate = 1.0, hxBeats = 0.0;
    bool hxFilled = false;
};

} // namespace om
