#pragma once

#include "LoadedTrack.h"
#include "Stems.h"
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
    /** Pitch reset button: one press glides the tempo back to 0 % (1 % per second, at most 6 s;
        inaudible with key lock); a second press while it glides (or a double press) jumps straight to 0 %. Moving the
        pitch by hand cancels the glide. Message thread. */
    void pitchResetPressed();
    /** CUE held on a paused deck: the track sounds from the cue point for as long as it is held.
        Sync matches the tempo but never moves it (stabs land exactly on the cue). */
    bool isCuePreviewing() const noexcept         { return previewing.load(); }
    bool isPitchGliding() const noexcept          { return pitchGlide.load() > 0.0; }
    void   setPitchRange (double fraction);
    double getPitchRange() const noexcept         { return pitchRange.load(); }
    double getEffectiveBpm() const noexcept;
    double getTempoRatio() const noexcept         { return 1.0 + pitch.load(); }

    void setSync (bool on);
    bool isSyncOn() const noexcept                { return syncOn.load(); }
    /** SYNC button: off → on (aligned); already on → aligned again at once (emergency); held ≥ 0.6 s → off. */
    void syncPress();
    /** The pitch fader moved to `p`: with SYNC on, once it is at the tempo sync set (it caught it),
        moving it further leaves SYNC and the fader rules, as on club players. */
    void pitchFader (double p);
    void syncRelease();

    void setQuantize (bool q) noexcept            { quantize = q; }
    bool isQuantizeOn() const noexcept            { return quantize.load(); }
    /** Quantize resolution in beats: 1, 1/2, 1/4 or 1/8. */
    void setQuantizeBeats (double b) noexcept     { quantizeBeats = juce::jlimit (0.125, 1.0, b); }
    double getQuantizeBeats() const noexcept      { return quantizeBeats.load(); }

    // Hot cues (8 slots). Unset slots are < 0.
    void   hotCue (int slot);                     // set if empty, else jump
    /** A hot cue pressed while paused jumps there and starts playing (and keeps playing). With gate cue
        on (club player style) it plays only while held and release returns to it. */
    void   hotCueRelease (int slot);
    void   setGateCue (bool on) noexcept          { gateCue = on; }
    bool   isGateCue() const noexcept             { return gateCue.load(); }
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
    void setBrakeTime (double seconds) noexcept   { brakeSeconds = seconds; }   // STOP TIME knob
    double getBrakeTime() const noexcept          { return brakeSeconds.load(); }
    void audioBrake (double seconds) noexcept     { brakeSeconds = seconds; if (playRequested.load()) brake = true; }
    void audioBackspin (double seconds) noexcept  { backspinSeconds = seconds; if (playRequested.load()) backspin = true; }
    void backspinStop()                           { audioBackspin (1.2); }
    bool isBackspinning() const noexcept          { return backspin.load(); }
    bool isBraking() const noexcept               { return brake.load(); }
    void loopRoll (double beats, bool pressed);   // momentary loop, returns as if never touched
    /** SLICER: the 8 beats of the phrase playing are 8 slices; holding pad n repeats slice n (one beat),
        letting go returns to where the track would be. With `phraseLoop` the phrase itself stays in a loop
        (SLICER LOOP). */
    void slice (int slice, bool pressed);
    void setPhraseLoop (bool on);                 // SLICER LOOP: the 8-beat phrase playing loops
    bool isPhraseLoop() const noexcept            { return phraseLoop.load(); }
    /** The phrase the slicer cuts (seconds) and the slice playing now (0…7, −1 none). */
    std::pair<double, double> slicerPhrase() const;
    int  slicePlaying() const;
    /** CUE LOOP: to hot cue `slot` (set it here when empty) and loop from it for the auto-loop size;
        on the loop already playing from that cue, the loop ends. */
    void cueLoop (int slot);
    bool isRolling() const noexcept               { return rollActive.load(); }
    double getRollBeats() const noexcept          { return rollBeats; }

    /** Where the track would be if not displaced by slip-able actions (−1 when not displaced). */
    double getSlipPosition() const noexcept       { return slipDisplaced.load() ? publishedSlipPos.load() : -1.0; }

    /** Jog: while playing bends tempo; while paused scrubs. */
    void jogBend (double amount);                 // ±1 → ±12 % momentary
    void jogScrub (double seconds);

    std::function<void()> onCuesChanged;          // persists cue edits to the library

    // Stems: vocals, drums, bass and melody (everything else), each with its level and mute.
    // Until a part of the track is separated it plays as it is; the change crossfades in.
    enum StemPart { stemVocals = 0, stemDrums, stemBass, stemMelody, numStemParts };
    void setStems (StemDataPtr);
    StemDataPtr getStems() const                  { return stems; }
    void  setStemLevel (int part, float level);   // 0…1
    float getStemLevel (int part) const noexcept  { return juce::isPositiveAndBelow (part, (int) numStemParts) ? stemLevel[(size_t) part].load() : 1.0f; }
    void  setStemMute (int part, bool muted);
    void  toggleStemMute (int part)               { setStemMute (part, ! isStemMuted (part)); }
    bool  isStemMuted (int part) const noexcept   { return juce::isPositiveAndBelow (part, (int) numStemParts) && stemMute[(size_t) part].load(); }
    /** Only this part sounds (pressing it again brings all back). */
    void  soloStem (int part);
    void  resetStems();
    bool  stemsTouched() const noexcept;
    /** Stem EQ (audio thread, before render): the channel's EQ knobs act on the stems — LOW kick and bass,
        MID voice ↔ melody (left: no voice, right: no melody; beat and hi-hats stay), HIGH hi-hats. Applies only while the
        track has stems; isStemEqActive() says whether it did in the last block. */
    void  setStemEq (bool on, float low, float mid, float high) noexcept { stemEqWanted = on; eqKnob = { low, mid, high }; }
    bool  isStemEqActive() const noexcept         { return stemEqActive; }
    /** Gain of a stem EQ knob: 0 kill, centre 1, right up to 2 (+6 dB). */
    static float stemEqGain (float v) noexcept    { return v >= 0.5f ? 1.0f + (v - 0.5f) * 2.0f : (2.0f * v) * (2.0f * v); }

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
    enum class Jump { none = 0, absolute, keepPhase, relativeBeats, autoLoop, roll, slice, phraseLoop, cueLoop };
    std::atomic<bool> phraseLoop { false }, slicing { false };
    std::atomic<double> phraseStart { -1.0 }, phraseEnd { -1.0 };
    std::atomic<int> jumpMode { 0 };
    std::atomic<double> jumpValue { 0.0 };
    void requestJump (Jump mode, double value) noexcept { jumpValue.store (value); jumpMode.store ((int) mode); }
    void applyJump (const LoadedTrack&);
    std::atomic<bool> previewing { false };
    std::atomic<double> restAt { -1.0 };          // CUE released: once silent, park exactly here (seconds)      // CUE held while paused (audio thread reads it)
    double autoLoopBeats = 4.0;
    double rollBeats = 0.0;
    int gateSlot = -1;
    std::atomic<bool> gateCue { false };
    bool autoCue = true;
    std::vector<double> taps;
    double syncPressedAt = 0.0;
    double lastFaderPitch = std::numeric_limits<double>::quiet_NaN();
    bool syncWasOnAtPress = false;
    std::array<std::pair<double, double>, 8> savedLoops {};

    // Shared atomics
    std::atomic<bool>   playRequested { false };
    std::atomic<double> pendingSeek { -1.0 };
    std::atomic<double> pitch { 0.0 }, pitchRange { 0.08 };
    std::atomic<double> pitchGlide { 0.0 };       // glide to 0 %: speed in pitch fraction per second
    juce::uint32 lastPitchResetMs = 0;
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

    StemDataPtr stems;
    std::array<std::atomic<float>, 4> stemLevel { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<std::atomic<bool>, 4> stemMute {};

    // Audio-thread state
    juce::SpinLock trackLock;
    const StemData* audioStems = nullptr;
    std::array<float, 5> stemGain { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   // vocals, drums (low), bass, melody, hi-hats
    float stemBlend = 0.0f, stemStep = 0.002f;
    bool stemEqWanted = false, stemEqActive = false;
    std::array<float, 3> eqKnob { 0.5f, 0.5f, 0.5f };
    std::array<double, 5> hatCoef {};              // hi-hats: drums above ≈ 5 kHz (biquad high-pass)
    std::array<std::array<double, 4>, 2> hatState {};
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
