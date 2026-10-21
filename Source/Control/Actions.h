#pragma once

#include "../Engine/AudioEngine.h"

namespace om
{

/** How a physical/virtual control delivers values to an action. */
enum class ControlType
{
    button,     // 1 = pressed, 0 = released
    absolute,   // 0 … 1 (fader / knob position)
    relative    // signed delta (encoders, jog wheels); jog: revolutions
};

struct ActionInfo
{
    juce::String id;        // stable id, snake_case (play_pause, hot_cue_1…)
    juce::String label;     // shown in the mapping editor
    juce::String category;
    ControlType type = ControlType::button;
    bool perDeck = true;
};

/** Single entry point for every controllable function. Keyboard, MIDI and imported
    mappings all call trigger(); LED feedback reads ledState(). No other path
    changes engine state from a controller, so behaviour is identical everywhere. */
class Actions
{
public:
    struct Hooks
    {
        std::function<void (int deck)> loadSelected;
        std::function<void (int delta)> browse;
        std::function<void()> toggleRecord;
        std::function<void (int deck, int pad, bool down)> padPress;   // PAD 1–8 in the deck's current pad page
        std::function<void (int deck)> padPage;                        // next pad page (HOT CUE, ROLL, …)
        std::function<int (int deck, int pad)> padLed;                 // Led of PAD 1–8 in the current page
        std::function<void()> toggleAutoMix;
        std::function<void()> browseFocus;                             // browse knob pushed: tracks ↔ playlists
        /** A controller's pad-mode button: show that pad page on screen (0 HOT CUE, 1 ROLL, 2 LOOP,
            3 S.LOOP, 4 JUMP), or −1 with a note for a mode OpenMix has no page for. */
        std::function<void (int deck, int page, const juce::String& note)> padMode;
    };

    Actions (AudioEngine&, Hooks);

    const std::vector<ActionInfo>& list() const noexcept   { return infos; }
    const ActionInfo* find (const juce::String& id) const;

    void trigger (const juce::String& id, int deck, float value);
    bool ledState (const juce::String& id, int deck) const   { return ledLevel (id, deck) != Led::off; }
    /** Whether the function is on right now (playing, loop active, hot cue set…), for scripts' conditions. */
    bool isActive (const juce::String& id, int deck) const;
    AudioEngine& getEngine() noexcept                         { return engine; }

    /** What a controller light shows for an action: off, dim (available but not active, for
        controllers that have a dim level), on, or blinking (play while paused, cue away from the
        cue point…). Blinking follows the beat while the deck plays, otherwise twice a second. */
    enum class Led { off = 0, dim, on, blink };
    Led ledLevel (const juce::String& id, int deck) const;
    /** Colour of an RGB light (pads), the same the screen shows; transparent when the action has none. */
    juce::Colour ledColour (const juce::String& id, int deck) const;
    bool blinkPhase (int deck) const;

    /** Controller VU meters: 0…1 with peak hold and fall-off. deck −1 = master. Message thread. */
    float meterLevel (int deck);
    static bool isMeter (const juce::String& id)      { return id == "vu_meter" || id == "vu_master"; }
    /** Current engine value for absolute actions (0…1), or −1 if not applicable. */
    float currentValue (const juce::String& id, int deck) const;

    /** Called once per UI frame: releases jog pitch-bend when the wheel stops. */
    void tick();


    static constexpr double rollSizes[4] = { 0.125, 0.25, 0.5, 1.0 };
    // The screen's pad pages (DeckView): LOOP, ROLL and JUMP (pads 1–4 back, 5–8 forward).
    static constexpr double padLoopSizes[8] = { 0.25, 0.5, 1, 2, 4, 8, 16, 32 };
    static constexpr double padRollSizes[8] = { 1.0 / 32, 1.0 / 16, 0.125, 0.25, 0.5, 1, 2, 4 };
    static constexpr double padJumpSizes[4] = { 1, 4, 8, 16 };

private:
    struct Entry
    {
        ActionInfo info;
        std::function<void (int deck, float value)> run;
        std::function<bool (int deck)> led;
        std::function<float (int deck)> value;
        std::function<Led (int deck)> level;        // overrides `led` when set
        std::function<juce::Colour (int deck)> colour;
    };

    void add (ActionInfo, std::function<void (int, float)>, std::function<bool (int)> led = {},
              std::function<float (int)> value = {});
    void build();
    void setLevel (const juce::String& id, std::function<Led (int)>);
    void setColour (const juce::String& id, std::function<juce::Colour (int)>);
    /** Beatmatch guide (tempo / phase lights): this deck against the other one. */
    enum class Guide { none, faster, slower, ok };
    Guide tempoGuide (int deck) const;
    Guide phaseGuide (int deck) const;      // faster = behind (nudge forward), slower = ahead

    AudioEngine& engine;
    Hooks hooks;
    std::vector<ActionInfo> infos;
    std::map<juce::String, Entry> entries;
    std::array<double, AudioEngine::numDecks> lastJogMs {};
    std::array<float, AudioEngine::numDecks + 1> vu {};       // decks, then master
};

} // namespace om
