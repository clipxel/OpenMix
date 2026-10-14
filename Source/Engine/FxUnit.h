#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

namespace juce { class AudioPluginInstance; }

namespace om
{

/** One insert effect slot on a deck channel (3 per deck).
    Time-based parameters follow the deck's effective BPM (120 when the deck has none).
    Every effect fades in/out with WET so switching never clicks. */
class FxUnit
{
public:
    enum class Type
    {
        echo = 0, delay, reverb, flanger, phaser, gate, bitcrusher, noise, filter, roll, backspin, brake,
        shortDelay, tapeEcho, shimmer, echoVerb, spiral, trans, pitch, mobius, tripletFilter, tripletRoll,
        releaseDry, releaseMute, pingPong,
        plugin,
        numTypes
    };

    static juce::String typeName (Type);
    static juce::String paramName (Type);
    static juce::String paramText (Type, float param);
    /** Kept for API compatibility: no effect stops the deck any more (club mixer behaviour). */
    static bool isTransport (Type) noexcept       { return false; }

    std::atomic<int>   type { (int) Type::echo };
    std::atomic<bool>  enabled { false };
    std::atomic<float> wet { 0.5f };          // dry/wet or depth
    std::atomic<float> param { 0.5f };        // main parameter (beats, rate, amount…)
    std::atomic<float> feedback { 0.45f };    // echo / delay / flanger / phaser

    void prepare (double sampleRate, int maxBlock);
    void process (float* l, float* r, int n, double deckBpm);

    /** Plugin hosting: set from the message thread; the slot never owns the instance. */
    void setPlugin (juce::AudioPluginInstance*);
    juce::AudioPluginInstance* getPlugin() const noexcept { return plugin; }

private:
    static double beatsForParam (Type, float p) noexcept;

    void runEcho (float* l, float* r, int n, double bpm, bool pingPong);
    void runReverb (float* l, float* r, int n);
    void runModDelay (float* l, float* r, int n, double bpm);   // flanger
    void runPhaser (float* l, float* r, int n, double bpm);
    void runGate (float* l, float* r, int n, double bpm);
    void runCrush (float* l, float* r, int n);
    void runNoise (float* l, float* r, int n);
    void runFilter (float* l, float* r, int n);
    void runRoll (float* l, float* r, int n, double bpm);
    void runPlugin (float* l, float* r, int n);

    /** Shared delay core. tape: saturation + band-limit + wow in the loop; glide: slow
        delay-time changes (pitch swoops, "spiral"); captureOnly: release-echo feeding. */
    struct DelayOpts { bool pingPong = false, tape = false, glide = false, feedInput = true; float fbScale = 1.0f; };
    void runDelayCore (float* l, float* r, int n, double delaySeconds, DelayOpts);
    void runShimmer (float* l, float* r, int n);
    void runEchoVerb (float* l, float* r, int n, double bpm);
    void runPitch (float* l, float* r, int n);
    void runMobius (float* l, float* r, int n, double bpm);
    void runTripletFilter (float* l, float* r, int n, double bpm);
    void runRelease (float* l, float* r, int n, double bpm, bool mute);
    void runTurntable (float* l, float* r, int n, bool backspinMode);
    void recordRing (const float* l, const float* r, int n) noexcept;

    /** Soft limiter for feedback paths: transparent at normal levels, never runs away. */
    static float soft (float x) noexcept           { return x / (1.0f + 0.25f * std::abs (x)); }

    /** Dual read-head delay pitch shifter (60 ms window, crossfaded heads). */
    struct Shifter
    {
        std::vector<float> buf;
        int w = 0;
        double phase = 0.0;
        void prepare (double rate) { buf.assign ((size_t) (rate * 0.12), 0.0f); w = 0; phase = 0.0; }
        float process (float x, double ratio, double sr);
    };
    std::array<Shifter, 2> shifters;
    float shimmerFbL = 0.0f, shimmerFbR = 0.0f;

    // Tape loop filters and wow
    float tapeLpL = 0.0f, tapeLpR = 0.0f, tapeHpL = 0.0f, tapeHpR = 0.0f, tapeHpInL = 0.0f, tapeHpInR = 0.0f;
    double wowPhase = 0.0;

    // Mobius (barber-pole) and release echo state
    double mobiusPhase = 0.0;
    int releaseHold = 0;
    double ttRead = 0.0, ttTime = 0.0;
    bool ttActive = false;
    juce::SmoothedValue<float> dryGain;

    double sr = 48000.0;
    int maxBlockSize = 4096;
    juce::SmoothedValue<float> wetSm, delaySm;
    float lastWet = 0.5f, tailEnergy = 0.0f;
    bool wasOn = false;

    // Delay lines (echo / delay / flanger / roll capture)
    std::vector<float> dl, dr;
    int writePos = 0;

    juce::Reverb reverb;

    // LFO / modulation state
    double lfoPhase = 0.0;
    std::array<std::array<float, 2>, 6> apState {};     // phaser all-pass states per stage/channel
    float phaserFb[2] {};

    // Gate envelope
    float gateEnv = 1.0f;

    // Bitcrusher sample-and-hold
    float holdL = 0.0f, holdR = 0.0f;
    double holdCount = 0.0;

    // Noise + filter
    juce::dsp::StateVariableTPTFilter<float> svf;
    juce::Random random;

    // Roll
    std::vector<float> rollL, rollR;
    int rollLen = 0, rollPos = 0, rollFilled = 0;
    bool rolling = false;

    // Plugin
    juce::SpinLock pluginLock;
    juce::AudioPluginInstance* plugin = nullptr;
    juce::AudioBuffer<float> pluginBuf;
    juce::MidiBuffer emptyMidi;
};

} // namespace om
