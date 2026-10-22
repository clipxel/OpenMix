#pragma once

#include "Deck.h"
#include "ChannelStrip.h"
#include "FxUnit.h"
#include "BeatFx.h"
#include "Recorder.h"
#include "MasterLimiter.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace om
{

/** Owns the audio device, both decks, the mixer and the recorder.
    The UI only ever observes this object — it never keeps its own copy of
    engine state, so displayed values always match what is being heard. */
class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::ChangeBroadcaster,
                    private juce::ChangeListener,
                    private juce::Timer
{
public:
    static constexpr int numDecks = 4;

    /** Two decks are on screen at a time, as on a 2-deck controller: the left side shows deck 1 or 3,
        the right side deck 2 or 4 (a button swaps them). Controllers without their own deck buttons
        drive the deck their side shows. */
    int  visibleDeck (int side) const noexcept    { return visible[(size_t) juce::jlimit (0, 1, side)].load(); }
    bool isVisible (int deck) const noexcept      { return visibleDeck (deck % 2) == deck; }
    void showDeck (int deck) noexcept             { if (juce::isPositiveAndBelow (deck, numDecks)) visible[(size_t) (deck % 2)] = deck; }
    void swapSide (int side) noexcept             { const int s = juce::jlimit (0, 1, side); showDeck (visibleDeck (s) == s ? s + 2 : s); }
    /** The deck this one mixes against: the one on screen on the other side. */
    int  otherDeck (int deck) const noexcept      { return visibleDeck (1 - deck % 2); }
    static constexpr int numFxSlots = 1;   // per channel: the processor the BEAT FX section drives
    static constexpr int numDeckFx = 3;    // per deck: the effect units of controllers that have FX on each side

    /** BEAT FX section of a club mixer (one effect for the whole mixer): pick the effect, the channel it
        goes on (CH1, CH2, crossfader side A or B, or the master), the beat, LEVEL/DEPTH, and ON/OFF.
        FX FREQUENCY chooses which bands (LOW / MID / HI) go through the effect; the rest stays dry.
        UI / MIDI write these; the audio thread copies them onto the processors every block. */
    struct BeatFxSection
    {
        enum Target { ch1 = 0, ch2, xfA, xfB, master, numTargets };
        std::atomic<int>   type { (int) BeatFx::Type::echo };
        std::atomic<float> param { BeatFx::defaultParam (BeatFx::Type::echo) };
        std::atomic<float> level { BeatFx::defaultLevel (BeatFx::Type::echo) };
        std::atomic<bool>  on { false };
        std::atomic<int>   target { master };
        std::array<std::atomic<bool>, 3> bands { true, true, true };   // LOW · MID · HI
        std::atomic<float> timeMs { 0.0f };       // TIME knob (0 = follow BEAT)
        std::atomic<float> tapBpm { 0.0f };       // TAP (0 = AUTO: the tempo of the track)
        /** BEAT ◀ ▶: back to beat sync, one step. */
        void stepBeat (int dir) { timeMs = 0.0f; param = BeatFx::stepBeat ((BeatFx::Type) type.load(), param.load(), dir); }
        /** TAP: three or more taps in time set the BPM the effect runs on. */
        void tap();
        double lastTap = 0.0;
        std::array<double, 8> tapGaps {};
        int tapCount = 0;
        /** Pick another effect: switches off (as on the mixer) and loads its usual beat and level. */
        void select (BeatFx::Type t) { on = false; type = (int) t; param = BeatFx::defaultParam (t); level = BeatFx::defaultLevel (t); timeMs = 0.0f; }
        static juce::String targetName (int t);
    };

    enum class DeviceStatus { initialising, running, error, disconnected, unavailable };

    AudioEngine();
    ~AudioEngine() override;

    void initialiseDevice (const juce::XmlElement* savedState);

    juce::AudioDeviceManager& getDeviceManager() noexcept    { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept    { return formatManager; }

    Deck&         deck (int i)       { return *decks[(size_t) i]; }
    ChannelStrip& channel (int i)    { return channels[(size_t) i]; }
    BeatFxSection& beatFx()          { return beatFxSection; }
    /** A deck's own effects (controllers with FX1 / FX2 units, one per side): post-fader, after BEAT FX. */
    BeatFx&       deckFx (int deckIndex, int slot) { return deckFxSlots[(size_t) deckIndex][(size_t) slot]; }
    /** AUTOMIX drives the section itself while it mixes (the DJ's settings come back afterwards). */
    BeatFx&       fx (int deckIndex, int) { return fxSlots[(size_t) deckIndex][0]; }
    Recorder&     recorder()         { return rec; }
    MeterState&   masterMeter()      { return master; }

    // Mixer-wide parameters
    std::atomic<float> masterDb { 0.0f };         // −40 (mute) … +6
    std::atomic<float> crossfader { 0.0f };       // −1 … +1
    /** Moves the crossfader to `target` over `seconds` (MIX NOW); a manual move is simply overridden. */
    void startAutoCrossfade (float target, double seconds)
    {
        autoXfTarget = juce::jlimit (-1.0f, 1.0f, target);
        autoXfPerSecond = (float) (2.0 / juce::jmax (0.1, seconds));
    }
    std::atomic<float> autoXfTarget { 0.0f }, autoXfPerSecond { 0.0f };
    std::atomic<bool>  autoMixFx { false };        // AUTOMIX is driving the channel FX itself
    std::atomic<int>   crossfaderCurve { 0 };     // 0 dipless, 1 sharp, 2 constant power
    std::atomic<float> cueMix { 0.0f };           // 0 cue … 1 master (headphones)
    std::atomic<float> phonesDb { 0.0f };
    std::atomic<bool>  autoGain { true };
    std::atomic<int>   masterDeck { -1 };         // −1: automatic (first deck synced leads)         // set channel trim from track loudness on load
    std::atomic<float> autoGainTargetLufs { -14.0f };   // leaves headroom for effects before the limiter

    MasterLimiter& limiter() noexcept { return masterLimiter; }
    /** Trim (dB) auto-gain would apply to a track, or 0 when loudness is unknown. */
    float autoGainTrimFor (const LoadedTrack&) const;

    static float masterDbToGain (float db) noexcept { return db <= -39.9f ? 0.0f : juce::Decibels::decibelsToGain (db); }

    //==========================================================================
    using LoadCallback = std::function<void (bool ok, const juce::String& error)>;

    /** Decodes and analyses on a background thread, then installs on the deck
        (message thread). Refuses if the deck is playing. */
    void loadTrack (int deckIndex, const juce::File&, TrackMeta, LoadCallback);
    bool isLoading (int deckIndex) const noexcept { return loading[(size_t) deckIndex].load(); }
    /** The track being loaded has no analysis yet: its beat grid is being detected now. */
    bool isAnalysingOnLoad (int deckIndex) const noexcept { return analysingOnLoad[(size_t) deckIndex].load(); }

    /** Opens every output channel of the current card (routing picks the pairs). */
    void enableAllOutputs();
    std::function<void (int deck, const juce::File&)> onTrackLoaded;

    //==========================================================================
    DeviceStatus getStatus() const noexcept       { return status; }
    juce::String getStatusDetail() const          { return statusDetail; }
    double getCpuUsage() const                    { return deviceManager.getCpuUsage(); }
    int    getXRunCount() const                   { return deviceManager.getXRunCount(); }
    bool   hasHeadphoneOutput() const noexcept
    {
        const int mode = outputMode.load();
        return (mode == speakersPhones && (splitMono.load() || phonesChannel.load() + 1 < numOutputs.load()))
            || (mode == separateDecks && phonesChannel.load() >= 0 && phonesChannel.load() + 1 < numOutputs.load());
    }

    /** Output routing (Settings ▸ Audio): what goes to which channel pair of the card.
        `…Channel` = first channel of a stereo pair (0 = chan 1 & 2). With splitMono ("Y" splitter) a
        stereo output carries the master in mono on the left and the headphones in mono on the right. */
    int outputChannelCount() const noexcept { return numOutputs.load(); }
    enum OutputMode { speakersOnly = 0, speakersPhones = 1, separateDecks = 2 };
    std::atomic<int>  outputMode { speakersPhones };
    std::atomic<int>  masterChannel { 0 }, phonesChannel { 2 };
    std::array<std::atomic<int>, numDecks> deckChannel { 0, 2, 4, 6 };   // decks 3–4: their side's pair if the card has fewer
    std::atomic<bool> splitMono { false };
    double getSampleRate() const noexcept         { return currentRate.load(); }
    int    getBufferSize() const noexcept         { return currentBlock.load(); }
    double getOutputLatencyMs() const;

    juce::String startRecording();
    void stopRecording();

    //==========================================================================
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    /** Tests: prepare and run the full mix graph without an audio device. */
    void prepareForTest (double sampleRate, int blockSize, int outputs = 2);
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateStatus();
    void applySync();

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;

    std::array<std::atomic<int>, 2> visible { 0, 1 };
    std::array<std::unique_ptr<Deck>, numDecks> decks;
    std::array<ChannelStrip, numDecks> channels;
    std::array<std::array<BeatFx, numFxSlots>, numDecks> fxSlots;
    BeatFxSection beatFxSection;
    std::array<std::array<BeatFx, numDeckFx>, numDecks> deckFxSlots;
    BeatFx masterFx;
    /** FX FREQUENCY: splits a signal into LOW / MID / HI so only the chosen bands feed the effect. */
    struct BandSplit
    {
        juce::dsp::LinkwitzRileyFilter<float> lowSplit, highSplit, lowAllpass;
        void prepare (double sr, int block);
        /** Moves the bands not in `keep` from (l, r) into (restL, restR). */
        void split (float* l, float* r, float* restL, float* restR, int n, const std::array<bool, 3>& keep);
    };
    std::array<BandSplit, numDecks + 1> fxBands;               // channels + master
    juce::AudioBuffer<float> fxRest;
    void runBeatFx (BeatFx&, BandSplit&, float* l, float* r, int n, const Deck& clock);
    MeterState master;
    MasterLimiter masterLimiter;
    Recorder rec;

    juce::AudioBuffer<float> deckBuf, cueBuf, masterBuf, sepBuf;
    juce::ThreadPool loaderPool { juce::ThreadPoolOptions{}.withNumberOfThreads (2).withThreadName ("OpenMix loader") };
    std::array<std::atomic<bool>, numDecks> loading {}, analysingOnLoad {};
    std::array<int, numDecks> loadGeneration {};
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    std::atomic<juce::uint64> callbackCount { 0 };
    juce::uint64 lastSeenCount = 0;
    std::atomic<int> numOutputs { 0 }, currentBlock { 0 };
    std::atomic<double> currentRate { 0.0 };
    DeviceStatus status = DeviceStatus::initialising;
    juce::String statusDetail, lastError;
    bool everHadDevice = false;
    juce::SmoothedValue<float> masterGain;
    std::array<float, numDecks> lastFaderGain {}, lastXfGain {};
    std::array<bool, numDecks> wasAudible {};
    std::array<juce::int64, numDecks> playingSince {};
    std::array<double, numDecks> phaseOffset {};      // DJ's manual phase correction to hold (beats)
    std::array<bool, numDecks> wasNudging {};         // nudge, loop or roll in progress: phase re-held after it
    juce::int64 syncClock = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace om
