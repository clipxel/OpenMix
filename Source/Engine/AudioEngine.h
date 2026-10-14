#pragma once

#include "Deck.h"
#include "ChannelStrip.h"
#include "FxUnit.h"
#include "BeatFx.h"
#include "Recorder.h"
#include "MasterLimiter.h"
#include "Rmx.h"
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
    static constexpr int numDecks = 2;
    static constexpr int numFxSlots = 3;   // per deck

    enum class DeviceStatus { initialising, running, error, disconnected, unavailable };

    AudioEngine();
    ~AudioEngine() override;

    void initialiseDevice (const juce::XmlElement* savedState);

    juce::AudioDeviceManager& getDeviceManager() noexcept    { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept    { return formatManager; }

    Deck&         deck (int i)       { return *decks[(size_t) i]; }
    ChannelStrip& channel (int i)    { return channels[(size_t) i]; }
    BeatFx&       fx (int deckIndex, int slot) { return fxSlots[(size_t) deckIndex][(size_t) slot]; }
    RmxUnit&      rmx (int deckIndex)  { return rmxUnits[(size_t) deckIndex]; }
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

    /** Loads a user sound onto an RMX X-pad (decoded and resampled on a worker thread).
        An empty file restores the built-in sound. */
    void loadRmxSample (int deckIndex, int pad, const juce::File&, LoadCallback);
    /** X-PAD TONE (semitones) and SPEED (ratio) for a deck; the sounds are re-rendered in the background. */
    void setRmxToneSpeed (int deckIndex, float semitones, float speed);
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
    enum OutputMode { speakersOnly = 0, speakersPhones = 1, separateDecks = 2 };
    std::atomic<int>  outputMode { speakersPhones };
    std::atomic<int>  masterChannel { 0 }, phonesChannel { 2 };
    std::array<std::atomic<int>, numDecks> deckChannel { 0, 2 };
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

    std::array<std::unique_ptr<Deck>, numDecks> decks;
    std::array<ChannelStrip, numDecks> channels;
    std::array<std::array<BeatFx, numFxSlots>, numDecks> fxSlots;
    std::array<RmxUnit, numDecks> rmxUnits;
    MeterState master;
    MasterLimiter masterLimiter;
    Recorder rec;

    juce::AudioBuffer<float> deckBuf, cueBuf, masterBuf, sepBuf;
    juce::ThreadPool loaderPool { juce::ThreadPoolOptions{}.withNumberOfThreads (2).withThreadName ("OpenMix loader") };
    std::array<std::atomic<bool>, numDecks> loading {};
    std::array<int, numDecks> loadGeneration {};
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    std::array<std::atomic<int>, numDecks> rmxRenderGen {};

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
