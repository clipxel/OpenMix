#pragma once

#include "LoadedTrack.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace om
{

/** One playback deck.

    Threading model:
      • Message thread calls the public control API (play, cue, loops…). Controls only
        write atomics or queue requests; they never touch audio buffers.
      • Audio thread calls beginBlock()/render(); it is the single writer of the playhead
        and publishes it back through atomics.
      • UI reads published state (getPosition(), isPlaying(), getPitch()…) so every
        control always reflects what the engine is actually doing. */
class Deck
{
public:
    explicit Deck (int index);
    ~Deck();

    int getIndex() const noexcept { return index; }

    //==== Message thread ======================================================
    void setTrack (LoadedTrackPtr);
    LoadedTrackPtr getTrack() const { return track; }
    bool hasTrack() const noexcept  { return track != nullptr; }

    void setPlaying (bool shouldPlay);
    void togglePlay()                             { setPlaying (! isPlaying()); }
    bool isPlaying() const noexcept               { return playRequested.load(); }

    void cueDown();
    void cueUp();
    double getCuePoint() const noexcept           { return cuePoint; }

    void seek (double seconds);
    /** Playhead estimate extrapolated from the last audio block — use for drawing. */
    double getPosition() const noexcept;
    double getLength() const noexcept             { return track ? track->lengthSeconds() : 0.0; }

    void   setPitch (double fraction);            // 0.05 == +5 %
    double getPitch() const noexcept              { return pitch.load(); }
    void   setPitchRange (double fraction);
    double getPitchRange() const noexcept         { return pitchRange.load(); }
    double getEffectiveBpm() const noexcept;
    double getTempoRatio() const noexcept         { return 1.0 + pitch.load(); }

    void setSync (bool on);
    bool isSyncOn() const noexcept                { return syncOn.load(); }

    void setQuantize (bool q) noexcept            { quantize = q; }
    bool isQuantizeOn() const noexcept            { return quantize.load(); }
    /** Quantize resolution in beats: 1, 1/2, 1/4 or 1/8. */
    void setQuantizeBeats (double b) noexcept     { quantizeBeats = juce::jlimit (0.125, 1.0, b); }
    double getQuantizeBeats() const noexcept      { return quantizeBeats.load(); }

    // Hot cues (8 slots). Unset slots are < 0.
    void   hotCue (int slot);                     // set if empty, else jump
    /** Gate cue: a hot cue pressed while paused plays only while held; release returns to it. */
    void   hotCueRelease (int slot);
    /** Instant doubles: take the other deck's track, position, tempo, key and loop, in time. */
    void   cloneFrom (const Deck& other);
    void   setAutoCue (bool on) noexcept          { autoCue = on; }
    bool   isAutoCue() const noexcept             { return autoCue; }
    void   clearHotCue (int slot);
    double getHotCue (int slot) const noexcept    { return hotCues[(size_t) slot]; }

    // Loops (require a valid beat grid for beat-based sizes).
    void   setAutoLoop (double beats);
    void   loopIn();
    void   loopOut();
    void   exitLoop();
    void   reloop();
    void   scaleLoop (double factor);             // 0.5 halve, 2 double
    bool   isLoopActive() const noexcept          { return loopActive.load(); }
    double getLoopStart() const noexcept          { return loopStart.load(); }
    double getLoopEnd() const noexcept            { return loopEnd.load(); }
    double getLoopBeats() const noexcept;
    double getAutoLoopSize() const noexcept       { return autoLoopBeats; }
    void   setAutoLoopSize (double beats)         { autoLoopBeats = beats; }

    void beatJump (double beats);

    // Beat-grid editing (message thread). Changes apply live and are persisted via onGridChanged.
    BeatGrid getGrid() const                      { return track ? track->grid() : BeatGrid {}; }
    void setGrid (BeatGrid);
    void shiftGrid (double seconds);
    void setBpmKeepingPlayhead (double bpm);      // the beat nearest the playhead stays put
    void setDownbeatHere();
    void tapTempo();
    void resetGrid();
    std::function<void()> onGridChanged;
    void loopMove (double beats);                 // shifts the active loop

    // Saved loops (8 slots, persisted with the track). Unset slots have end <= start.
    void saveLoop (int slot);
    void triggerSavedLoop (int slot);
    void clearSavedLoop (int slot);
    std::pair<double, double> getSavedLoop (int slot) const noexcept { return savedLoops[(size_t) slot]; }

    // Performance transport
    /** Vinyl: touching the jog scratches. CD: the jog only nudges / searches. */
    void setVinylMode (bool v) noexcept           { vinyl = v; }
    bool isVinylMode() const noexcept             { return vinyl.load(); }
    void setScratching (bool on);                 // vinyl mode: jog touched
    bool isScratching() const noexcept            { return scratching.load(); }
    void scratchMove (double seconds);            // platter displacement while touched
    void setReverse (bool on)                     { reverse = on; }
    bool isReverse() const noexcept               { return reverse.load(); }
    void setCensor (bool held)                    { censor = held; }   // momentary reverse with slip
    bool isCensor() const noexcept                { return censor.load(); }
    void setKeyLock (bool on)                     { keyLock = on; }
    bool isKeyLock() const noexcept               { return keyLock.load(); }
    void setKeyShift (int semitones)              { keyShift = juce::jlimit (-12, 12, semitones); }
    int  getKeyShift() const noexcept             { return keyShift.load(); }
    /** Track key (Camelot) after key shift, or empty when unknown. */
    juce::String getEffectiveKey() const;

    void setSlip (bool on)                        { slip = on; }
    bool isSlip() const noexcept                  { return slip.load(); }
    void brakeStop();                             // turntable power-off stop
    void audioBrake (double seconds) noexcept     { brakeSeconds = seconds; if (playRequested.load()) brake = true; }
    void audioBackspin (double seconds) noexcept  { backspinSeconds = seconds; if (playRequested.load()) backspin = true; }
    void backspinStop()                           { audioBackspin (1.2); }
    bool isBackspinning() const noexcept          { return backspin.load(); }
    bool isBraking() const noexcept               { return brake.load(); }
    void loopRoll (double beats, bool pressed);   // momentary loop, returns as if never touched
    bool isRolling() const noexcept               { return rollActive.load(); }
    double getRollBeats() const noexcept          { return rollBeats; }

    /** Where the track would be if not displaced by slip-able actions (−1 when not displaced). */
    double getSlipPosition() const noexcept       { return slipDisplaced.load() ? publishedSlipPos.load() : -1.0; }

    /** Jog: while playing bends tempo; while paused scrubs. */
    void jogBend (double amount);                 // ±1 → ±12 % momentary
    void jogScrub (double seconds);

    std::function<void()> onCuesChanged;          // persists cue edits to the library

    //==== Audio thread ========================================================
    void prepare (double outputSampleRate);
    void beginBlock();
    void render (float* left, float* right, int numSamples);

    // Engine-side helpers used by the sync stage (audio thread). Only valid
    // inside withAudioLock(), which guarantees the track can't be swapped.
    template <typename Fn>
    void withAudioLock (Fn&& fn)
    {
        const juce::SpinLock::ScopedTryLockType sl (trackLock);
        if (sl.isLocked() && audioTrack != nullptr)
            fn();
    }

    double audioPositionSeconds() const noexcept;
    double audioBpm() const noexcept;
    /** Audio thread, under withAudioLock: performs any pending seek / cue / hot-cue jump now, so sync
        can align the result before the first sample of it is heard. True if the playhead moved. */
    bool   audioResolveMoves() noexcept;
    void   audioSetPitch (double p) noexcept;
    void   audioShiftSeconds (double delta) noexcept;
    void   audioSetPhaseCorrection (double c) noexcept { phaseCorrection = c; }
    double getBend() const noexcept               { return bend.load(); }
    bool   consumeSyncEngaged() noexcept          { return syncEngaged.exchange (false); }
    bool   consumeJustLoaded() noexcept           { return justLoaded.exchange (false); }
    juce::int64 getSyncStamp() const noexcept     { return syncStamp.load(); }
    bool   isAudiblyPlaying() const noexcept      { return audible.load(); }
    BeatGrid audioGrid() const noexcept           { return audioTrack != nullptr ? audioTrack->grid() : BeatGrid {}; }
    /** Effective BPM as last rendered (safe from any thread). */
    double getRenderedBpm() const noexcept        { return renderedBpm.load(); }
    /** Musical clock of the last rendered block (audio thread): beat at its first sample
        (−1 when unknown / stopped) and beats advanced per output sample. */
    double audioBlockBeat() const noexcept        { return blockBeat; }
    double audioBeatsPerSample() const noexcept   { return blockBeatsPerSample; }

private:
    double snap (double seconds) const;
    void   requestSeek (double seconds)           { pendingSeek.store (seconds); }
    void   notifyCues()                           { if (onCuesChanged) onCuesChanged(); }

    const int index;

    // Message-thread state
    LoadedTrackPtr track;
    double cuePoint = 0.0;
    std::array<double, 8> hotCues {};
    std::atomic<bool> quantize { true };
    std::atomic<double> quantizeBeats { 1.0 };

    /** Jumps are resolved on the audio thread from the exact playhead (sample accurate),
        so quantized jumps keep the beat phase exactly — never from a UI-estimated position. */
    enum class Jump { none = 0, absolute, keepPhase, relativeBeats, autoLoop, roll };
    std::atomic<int> jumpMode { 0 };
    std::atomic<double> jumpValue { 0.0 };
    void requestJump (Jump mode, double value) noexcept { jumpValue.store (value); jumpMode.store ((int) mode); }
    void applyJump (const LoadedTrack&);
    bool previewing = false;
    double autoLoopBeats = 4.0;
    double rollBeats = 0.0;
    int gateSlot = -1;
    bool autoCue = true;
    std::vector<double> taps;
    std::array<std::pair<double, double>, 8> savedLoops {};

    // Shared atomics
    std::atomic<bool>   playRequested { false };
    std::atomic<double> pendingSeek { -1.0 };
    std::atomic<double> pitch { 0.0 }, pitchRange { 0.08 };
    std::atomic<double> bend { 0.0 };
    std::atomic<bool>   syncOn { false }, syncEngaged { false }, justLoaded { false };
    std::atomic<juce::int64> syncStamp { 0 };
    std::atomic<bool>   loopActive { false };
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 };
    std::atomic<bool>   audible { false };
    std::atomic<bool>   reachedEnd { false };
    std::atomic<double> renderedBpm { 0.0 };
    std::atomic<bool>   scratching { false }, reverse { false }, censor { false }, slip { false };
    std::atomic<bool>   rollActive { false }, brake { false }, slipDisplaced { false };
    std::atomic<double> scratchPending { 0.0 }, publishedSlipPos { 0.0 };
    std::atomic<bool>   keyLock { true };       // on by default (remembered per deck in settings)
    std::atomic<bool>   backspin { false };
    std::atomic<double> brakeSeconds { 1.0 }, backspinSeconds { 1.2 };
    std::atomic<double> phaseCorrection { 0.0 };   // sync phase-lock trim (fraction of speed)
    std::atomic<bool>   vinyl { true };
    std::atomic<int>    keyShift { 0 };

    // Published playhead
    std::atomic<double> publishedPos { 0.0 };
    std::atomic<double> publishedRate { 0.0 };
    std::atomic<double> publishedAtMs { 0.0 };

    // Audio-thread state
    juce::SpinLock trackLock;
    const LoadedTrack* audioTrack = nullptr;
    double outRate = 48000.0;
    double pos = 0.0;          // in source samples
    float gain = 0.0f;         // click-free start/stop ramp
    float rampStep = 0.005f;
    double scratchVel = 0.0, brakeFactor = 1.0, slipPos = 0.0, backspinT = -1.0;

    // Key lock / key shift: time-stretcher state (pimpl keeps the DSP header out of here).
    struct Stretch;
    std::unique_ptr<Stretch> stretch;
    bool stretchActive = false, needPrime = false;
    double stretchLead = 0.0;    // input samples (at output rate) the stretcher runs ahead
    bool stretchXfadePending = false;
    int stretchXfadeLeft = 0;
    static constexpr int stretchXfadeLen = 240;   // ≈ 5 ms
    double stretchOldPos = 0.0;
    /** Called before moving the playhead while the stretcher runs: converts pos to the
        heard position and arms a crossfade from the running stretcher. */
    void stretchJumping (double srcToOut) noexcept
    {
        if (! stretchActive) return;
        stretchOldPos = pos;
        stretchXfadePending = gain > 0.0f;
        pos -= stretchLead * srcToOut;
        stretchActive = false;
    }
    double inCarry = 0.0;
    double blockBeat = -1.0, blockBeatsPerSample = 0.0;
    bool wasDisplaced = false, slipWillReturn = false;

    // Click-free jumps: the old position keeps sounding and fades out while the
    // new one fades in (loop wraps, hot cues, seeks, slip returns).
    static constexpr int jumpFadeLen = 96;     // ≈ 2 ms
    double fadeFrom = 0.0;
    int fadeLeft = 0;
    float fadeGain = 1.0f;                     // level of the outgoing audio when the jump happened
    void beginJump (double from) noexcept      { fadeFrom = from; fadeLeft = jumpFadeLen; fadeGain = gain; }
};

} // namespace om
