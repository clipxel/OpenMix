#pragma once

#include "Actions.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace om
{

/** Computer-keyboard control through the same Actions as MIDI.
    Momentary actions (cue, censor, rolls, bends) are released when the key goes up. */
class KeyboardControl
{
public:
    struct Binding
    {
        int keyCode;                // juce::KeyPress key code (lower-case letters, digits, keys)
        juce::ModifierKeys mods;    // shift / alt; command is reserved for app shortcuts
        juce::String action;
        int deck;                   // −1 for global
        float value = 1.0f;         // for relative actions: delta per press
    };

    explicit KeyboardControl (Actions&);

    const std::vector<Binding>& bindings() const noexcept { return keys; }
    static juce::String describe (const Binding&);
    /** Short key-cap caption for an action, as printed on the keyboard map. */
    static juce::String capLabel (const juce::String& action);

    /** Editing (Settings ▸ Controllers ▸ Keyboard). Changes are saved to keyboard.json. */
    const Binding* bindingFor (int keyCode, bool shift) const;
    void assign (int keyCode, bool shift, const juce::String& action, int deck);
    void clear (int keyCode, bool shift);
    void resetToFactory();
    void load (const juce::File&);
    void save() const;

    /** Returns true if the key was consumed. */
    bool keyPressed (const juce::KeyPress&);
    /** Per-frame: detect key releases for momentary actions. */
    void tick();

private:
    void addDefaults();
    static bool repeats (const juce::String& action);   // held key keeps acting (pitch, crossfader steps)

    Actions& actions;
    std::vector<Binding> keys;
    juce::File file;
    struct Held { int keyCode; juce::String action; int deck; };
    std::vector<Held> held;
};

} // namespace om
