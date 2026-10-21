#pragma once

#include "Actions.h"

namespace om
{

/** Runs controller scripts written in the language of VirtualDJ mappers ("VDJScript") as they are:
    "play_pause", "deck 2 hot_cue 3", "shift ? loop_half : loop_double", "loop 4 & sync",
    "holding ? sync : play_pause", "loop ? on : dim", "pitch_slider", "jogwheel"… A mapper imported
    from VirtualDJ keeps its scripts word for word; nothing is translated by hand.

    Written from VirtualDJ's public list of verbs (what each verb does), not from its code.

    Grammar: commands joined by "&" (all run); "condition ? then : else" (else runs to the end, as in
    VirtualDJ); parentheses group; "deck N|left|right|master|active" in front picks the deck. A
    command is a verb and its parameters (numbers, +N / -N, 50%, 1bt, 500ms, 'quoted text').

    The verbs OpenMix has a function for run that function (Actions), so a script does exactly what
    the same function does on screen. Verbs for things OpenMix does not have (video, karaoke,
    sampler, stems…) do nothing and are reported when a mapping is imported. */
class VdjScript
{
public:
    explicit VdjScript (Actions& a) : actions (a) {}
    ~VdjScript()                                  { *alive = false; }

    /** How the control delivers its value. Buttons call the script on press (value 1) and on release
        (value 0); most verbs act on the press only, momentary ones (cue, censor, loop rolls, touch)
        also on the release. Faders: 0…1. Jogs / encoders: the movement (already scaled). */
    enum class Input { button, position, turn };

    struct Context
    {
        int deck = 0;                   // the control's deck (0, 1)
        float value = 1.0f;
        Input input = Input::button;
        bool shift = false;             // the controller's SHIFT is held
        juce::String control;           // which control runs it (for "holding" / "doubleclick" timing)
    };

    void run (const juce::String& script, const Context&);
    /** The script read as a condition (play_pause → playing, hot_cue 2 → set…). */
    bool query (const juce::String& script, const Context&);

    /** What the control's light shows when the script is its light: on / off, "dim", "blink", and a
        colour when the script names one ("color 'red'") or stands for a function that has one. */
    struct Light { Actions::Led level = Actions::Led::off; juce::Colour colour; };
    Light light (const juce::String& script, const Context&);

    /** The OpenMix function the script stands for, for its light's colour (hot cue colours…). */
    juce::String mainAction (const juce::String& script, int deck);

    /** Verbs in the script that OpenMix does not know (empty: it all runs). */
    static juce::StringArray unknownVerbs (const juce::String& script);
    /** True if the script moves something continuously (faders, jogs, encoders): such a script only fits
        a fader / knob / jog, never a plain button. */
    static bool isContinuous (const juce::String& script);

    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

private:
    NodePtr parsed (const juce::String& script);
    bool exec (const Node&, Context&, bool asQuery);
    bool verb (const juce::String& name, const juce::StringArray& args, Context&, bool asQuery);

    Actions& actions;
    std::map<juce::String, NodePtr> cache;
    std::map<juce::String, float> vars;          // "set 'name' 1", "toggle 'name'", "var 'name'"
    std::map<juce::String, double> pressedAt;    // control → time of its last press (holding / doubleclick)
    std::map<juce::String, double> lastTap;      // control → time of the press before (doubleclick)
    std::map<int, std::array<float, 3>> killed;  // deck → EQ before "eq_kill_*" (high, mid, low)
    Light hint;                                  // set by "dim", "blink", "color" while reading a light
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);   // long-press timers outliving the engine
};

} // namespace om
