#pragma once

#include "../Engine/AudioEngine.h"
#include "../Library/Library.h"

namespace om
{

/** Automatic mixing from a queue of tracks.

    While it runs, the next track waits loaded on the free deck. When the track on air reaches its
    last `mixBeats` beats, the next one starts on the beat (in sync when both tempos are close), the
    crossfader travels to it over those beats and the bass is swapped halfway (no doubled kick).
    Then the old deck stops, its tempo glides back to 0 % on the new track, and the following track
    is preloaded. Everything goes through the same deck / mixer controls the user sees, so taking
    over by hand at any moment just works: touching PLAY or the crossfader keeps the music going. */
class AutoMix : public juce::ChangeBroadcaster
{
public:
    struct Hooks
    {
        std::function<void (const TrackPtr&, int deck)> load;   // same path as loading by hand
    };

    AutoMix (AudioEngine&, Hooks);

    // Queue (message thread)
    const std::vector<TrackPtr>& queue() const noexcept     { return items; }
    void add (const std::vector<TrackPtr>&);
    void insertAt (int index, const std::vector<TrackPtr>&);
    void remove (int index);
    void move (int from, int to);
    void shuffle();
    void clear();

    // Running
    void setRunning (bool);
    bool isRunning() const noexcept                         { return running; }
    void mixNow();                                          // start the transition right away
    int  mixBeats() const noexcept                          { return beats; }
    void setMixBeats (int b)                                { beats = juce::jlimit (1, 64, b); sendChangeMessage(); }

    /** How each transition is made. */
    enum class Mode
    {
        end = 0,       // blend over the last N beats of the track
        half,          // each track plays about half, then the next comes in (on a phrase)
        electronic,    // long realistic blend: new track without bass, bass swap halfway, old one filtered out
        reverb,        // the old track washes out in reverb, is cut on the bar, the new one drops in
        echo,          // same with a tempo echo
        smart,         // picks per pair: electronic blend for close tempos, echo / reverb out otherwise
        numModes
    };
    static juce::String modeName (Mode);
    Mode mode() const noexcept                              { return userMode; }
    void setMode (Mode m)                                   { userMode = m; sendChangeMessage(); }

    /** Called every UI frame. */
    void tick();

    // Status for the panel
    int onAirDeck() const noexcept                          { return onAir; }
    bool isMixing() const noexcept                          { return phase == Phase::mixing; }
    double secondsToMix() const;                            // −1 if unknown
    juce::String status() const                             { return statusText; }

private:
    enum class Phase { idle, starting, playing, mixing };

    double mixSeconds (int deck) const;
    double beatSec (int deck) const;
    double triggerPointOf (int deck) const;      // track seconds where the next transition starts
    Mode   plannedMode (int& beatsOut) const;     // effective mode and length for the next transition
    void   applyMix (double p);                   // per frame during a transition (0…1)
    void   startIncoming();
    double startPointOf (int deck) const;
    double endPointOf (int deck) const;
    bool   isLoaded (int deck, const TrackPtr&) const;
    void   startTransition();
    void   finishTransition();
    void   setStatus (const juce::String&);

    AudioEngine& engine;
    Hooks hooks;
    std::vector<TrackPtr> items;
    bool running = false;
    int beats = 16;

    Phase phase = Phase::idle;
    int onAir = -1;                 // deck on air
    TrackPtr preloaded;             // what the free deck was asked to load
    int preloadDeck = -1;
    Mode userMode = Mode::end, active = Mode::end;
    int activeBeats = 16;
    double mixOutStart = 0.0, mixLengthSec = 0.0;
    bool incomingStarted = false, cutDone = false;
    struct Strip { float low, mid, high, filter, fader; int assign; };
    std::array<Strip, 2> saved {};
    struct Fx { bool enabled; int type; float wet, param; } savedFx {};
    juce::String statusText;
    mutable std::array<std::pair<const void*, double>, 2> endCache {};   // per deck: track → end of the music
};

} // namespace om
