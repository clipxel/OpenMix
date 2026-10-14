#pragma once

#include "FxUnit.h"
#include "TrackAnalysis.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

namespace om
{

/** SAMPLES/RMX: a per-deck performance unit inspired by remix stations.

    Row 1 · X-PAD: four one-shot sounds (built-in synthesised kick, snare, clap, hat, or
    user samples), triggered on the 1/16 grid of the deck and rolled while held.
    Row 2 · SCENE / RELEASE: BUILD UP and BREAK DOWN grow while held, ECHO OUT releases
    the track into an echo and brings it back by itself, BRAKE stops the sound like a
    turntable (⇧: backspin) while the deck keeps running.

    Message thread: padDown / padUp / setSample. Audio thread: process. Everything the
    audio thread needs is preallocated; samples are swapped under a spin lock. */
class RmxUnit
{
public:
    enum Pad { kick = 0, snare, clap, hat, buildUp, breakDown, echoOut, brake, numPads };
    static juce::String padName (int pad);
    static constexpr int numDrums = 4;

    std::atomic<float> drumLevel { 0.8f };

    /** X-PAD TONE (semitones, −12 … +12) and SPEED (×0.5 … ×2), independent of each other: the
        sounds are re-rendered off the audio thread with a time/pitch stretcher. Message thread. */
    float getTone() const noexcept                { return tone; }
    float getSpeed() const noexcept               { return speed; }
    void  setToneAndSpeed (float semitones, float speedRatio) { tone = semitones; speed = speedRatio; }
    bool  needsProcessing() const noexcept        { return std::abs (tone) > 0.01f || std::abs (speed - 1.0f) > 0.001f; }
    /** The unprocessed sound of a pad (user sample or built-in), for re-rendering. */
    std::shared_ptr<const juce::AudioBuffer<float>> sourceFor (int pad) const;
    /** Installs re-rendered sounds (nullptr = play the source as is). */
    void setProcessed (std::array<std::shared_ptr<juce::AudioBuffer<float>>, 4> bufs);
    static std::shared_ptr<juce::AudioBuffer<float>> render (const juce::AudioBuffer<float>& src, double sampleRate,
                                                             float semitones, float speedRatio);
    double sampleRate() const noexcept            { return sr; }

    void prepare (double sampleRate, int maxBlock);

    // Message thread
    void padDown (int pad, bool shift);
    void padUp (int pad);
    /** While an X-PAD is held: live tone in semitones (±12, on top of TONE) and roll steps relative to
        the VEL. setting (−2 … +2: each step halves / doubles the roll rate). */
    void padGesture (int pad, float semitones, int rollSteps);
    /** VEL. knob: the roll rate, in beats (1 = 1/4 note … 1/16 beat = 1/64 note). */
    void setBaseRoll (double beats)    { baseRoll = juce::jlimit (0.0625, 1.0, beats); }
    double getBaseRoll() const         { return baseRoll.load(); }
    float liveTone (int pad) const     { return juce::isPositiveAndBelow (pad, numDrums) ? liveSemis[(size_t) pad].load() : 0.0f; }
    double rollRate (int pad) const    { return juce::isPositiveAndBelow (pad, numDrums) ? rollBeatsOf[(size_t) pad].load() : defaultRoll; }
    static constexpr double defaultRoll = 0.25;          // 1/16 note
    bool isLit (int pad) const;
    /** Replaces an X-pad sound (already resampled to the output rate). Empty = built-in. */
    void setSample (int pad, std::shared_ptr<juce::AudioBuffer<float>> sample, const juce::String& name);
    juce::String sampleName (int pad) const;

    // Audio thread. `beat` is the deck's beat position at the first sample of the block
    // (−1 when unknown), `beatsPerSample` its rate, `bpm` the effective tempo, `stepBeats` the
    // deck's quantize resolution (1, ½, ¼, ⅛ beat).
    void process (float* l, float* r, int n, double beat, double beatsPerSample, double bpm, double stepBeats = 0.25);

private:
    struct Voice { const juce::AudioBuffer<float>* buf = nullptr; double pos = 0.0; int delay = 0; bool active = false; int pad = 0; };
    struct Scene { float amount = 0.0f; double startBeat = 0.0; };

    void synthesiseDrums();
    void startVoice (int pad, int delaySamples);

    double sr = 48000.0;
    std::array<juce::AudioBuffer<float>, numDrums> builtIn;
    std::array<std::shared_ptr<juce::AudioBuffer<float>>, numDrums> userSamples;   // message thread owns
    std::array<std::shared_ptr<juce::AudioBuffer<float>>, numDrums> processed;     // message thread owns
    float tone = 0.0f, speed = 1.0f;
    void updateActive();                                                           // under sampleLock
    std::array<const juce::AudioBuffer<float>*, numDrums> active {};               // audio thread reads
    std::array<juce::String, numDrums> names;
    juce::SpinLock sampleLock;

    std::array<Voice, 16> voices;

    // Pad state written by the UI, consumed by the audio thread.
    std::array<std::atomic<bool>, numPads> held {};
    std::array<std::atomic<bool>, numDrums> triggerPending {};
    std::array<std::atomic<bool>, numDrums> fastRoll {};
    std::array<std::atomic<float>, numDrums> liveSemis {};
    std::array<std::atomic<double>, numDrums> rollBeatsOf { defaultRoll, defaultRoll, defaultRoll, defaultRoll };
    std::atomic<double> baseRoll { defaultRoll };
    std::array<double, numDrums> nextRollBeat {};
    std::array<double, numDrums> pendingBeat { -1.0, -1.0, -1.0, -1.0 };    // audio thread: grid target
    std::array<std::atomic<double>, numDrums> flashUntil {};
    std::atomic<bool> backspinMode { false };

    // Scene FX
    float sceneUp = 0.0f, sceneDown = 0.0f;
    double sceneBeats = 0.0;                     // beats since BUILD UP / BREAK DOWN was pressed
    bool upWasHeld = false, downWasHeld = false;
    juce::SmoothedValue<float> gateSm;
    juce::dsp::StateVariableTPTFilter<float> hp, lp, noiseHp;
    FxUnit sceneEcho, releaseEcho, turntable;
    juce::Random rnd;
};

} // namespace om
