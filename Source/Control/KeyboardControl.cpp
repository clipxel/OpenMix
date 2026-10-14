#include "KeyboardControl.h"

namespace om
{

KeyboardControl::KeyboardControl (Actions& a) : actions (a)
{
    addDefaults();
}

void KeyboardControl::addDefaults()
{
    // Factory default keyboard map: left half → deck 1, right half → deck 2.
    keys.clear();
    const juce::ModifierKeys none, shift (juce::ModifierKeys::shiftModifier);
    auto add = [this] (int key, juce::ModifierKeys m, const juce::String& action, int deck, float v = 1.0f)
    {
        keys.push_back ({ key, m, action, deck, v });
    };

    add ('`', none, "switch_pfl", -1);
    const char* topA = "123456";  const char* topB = "7890-=";
    const char* topActions[] = { "play_pause", "cue_button", "goto_cue_stop", "loop", "loop_half", "loop_double" };
    for (int i = 0; i < 6; ++i)
    {
        add (topA[i], none, topActions[i], 0);
        add (topB[i], none, topActions[i], 1);
    }
    add (juce::KeyPress::backspaceKey, none, "mix_now", -1);

    const char* midA = "qwerty";  const char* midB = "uiop[]";
    const char* midActions[] = { "sync", "pitch_bend_minus", "pitch_bend_plus", "pitch_minus", "pitch_plus", "pitch_reset" };
    for (int i = 0; i < 6; ++i)
    {
        add (midA[i], none, midActions[i], 0);
        add (midB[i], none, midActions[i], 1);
    }

    const char* padsA = "asdf";  const char* padsB = "jkl;";
    const char* pads2A = "zxcv"; const char* pads2B = "m,./";
    for (int i = 0; i < 4; ++i)
    {
        add (padsA[i],  none, "pad_" + juce::String (i + 1), 0);
        add (padsB[i],  none, "pad_" + juce::String (i + 1), 1);
        add (pads2A[i], none, "pad_" + juce::String (i + 5), 0);
        add (pads2B[i], none, "pad_" + juce::String (i + 5), 1);
    }
    add ('g', none, "pad_page", 0);
    add ('\'', none, "pad_page", 1);

    add (juce::KeyPress::spaceKey,  none, "emergency_play", -1);
    add (juce::KeyPress::leftKey,   none, "crossfader_left", -1);
    add (juce::KeyPress::rightKey,  none, "crossfader_right", -1);
    add (juce::KeyPress::upKey,     none, "all_pitch_minus", -1);
    add (juce::KeyPress::downKey,   none, "all_pitch_plus", -1);

    // SHIFT layer: browse and load.
    add (juce::KeyPress::upKey,    shift, "browser_scroll", -1, -1.0f);
    add (juce::KeyPress::downKey,  shift, "browser_scroll", -1, 1.0f);
    add (juce::KeyPress::leftKey,  shift, "load", 0);
    add (juce::KeyPress::rightKey, shift, "load", 1);
}

bool KeyboardControl::repeats (const juce::String& a)
{
    return a == "pitch_minus" || a == "pitch_plus" || a == "all_pitch_minus" || a == "all_pitch_plus"
        || a == "crossfader_left" || a == "crossfader_right" || a == "browser_scroll";
}

juce::String KeyboardControl::capLabel (const juce::String& a)
{
    static const std::map<juce::String, juce::String> caps {
        { "switch_pfl", "SWITCH PFL" }, { "play_pause", "PLAY" }, { "cue_button", "CUE" }, { "goto_cue_stop", "STOP" },
        { "loop", "LOOP" }, { "loop_half", "Loop /2" }, { "loop_double", "Loop x2" }, { "mix_now", "MIX NOW" },
        { "sync", "SYNC" }, { "pitch_bend_minus", "NUDGE LEFT" }, { "pitch_bend_plus", "NUDGE RIGHT" },
        { "pitch_minus", "PITCH -" }, { "pitch_plus", "PITCH +" }, { "pitch_reset", "PITCH RESET" },
        { "pad_page", "PAD PAGE" }, { "emergency_play", "EMERGENCY PLAY" }, { "crossfader_left", "CROSS <" },
        { "crossfader_right", "CROSS >" }, { "all_pitch_minus", "ALL PITCH -" }, { "all_pitch_plus", "ALL PITCH +" },
        { "browser_scroll", "BROWSE" }, { "load", "LOAD" }, { "brake", "BRAKE" }, { "censor", "CENSOR" },
        { "reverse", "REVERSE" }, { "slip_mode", "SLIP" }, { "key_lock", "KEY LOCK" }, { "quantize", "QUANTIZE" },
        { "pfl", "PFL" }, { "record", "REC" }, { "master", "MASTER" }, { "instant_doubles", "DOUBLES" },
        { "beatjump_back", "JUMP <" }, { "beatjump_fwd", "JUMP >" }, { "loop_in", "LOOP IN" }, { "loop_out", "LOOP OUT" },
        { "loop_exit", "LOOP EXIT" }, { "reloop", "RELOOP" }, { "backspin", "BACKSPIN" }, { "vinyl_mode", "VINYL" } };
    if (auto it = caps.find (a); it != caps.end()) return it->second;
    if (a.startsWith ("pad_"))           return "PAD " + a.fromLastOccurrenceOf ("_", false, false);
    if (a.startsWith ("hot_cue_"))       return "HOT CUE " + a.fromLastOccurrenceOf ("_", false, false);
    if (a.startsWith ("delete_hot_cue_"))return "DEL CUE " + a.fromLastOccurrenceOf ("_", false, false);
    if (a.startsWith ("loop_roll_"))     return "ROLL " + a.fromLastOccurrenceOf ("_", false, false);
    if (a.startsWith ("effect_active_")) return "FX " + a.fromLastOccurrenceOf ("_", false, false);
    return a.replaceCharacter ('_', ' ').toUpperCase();
}

const KeyboardControl::Binding* KeyboardControl::bindingFor (int keyCode, bool shift) const
{
    for (auto& b : keys)
        if (b.keyCode == keyCode && b.mods.isShiftDown() == shift)
            return &b;
    return nullptr;
}

void KeyboardControl::clear (int keyCode, bool shift)
{
    keys.erase (std::remove_if (keys.begin(), keys.end(), [&] (const Binding& b)
                { return b.keyCode == keyCode && b.mods.isShiftDown() == shift; }), keys.end());
    save();
}

void KeyboardControl::assign (int keyCode, bool shift, const juce::String& action, int deck)
{
    keys.erase (std::remove_if (keys.begin(), keys.end(), [&] (const Binding& b)
                { return b.keyCode == keyCode && b.mods.isShiftDown() == shift; }), keys.end());
    keys.push_back ({ keyCode, shift ? juce::ModifierKeys (juce::ModifierKeys::shiftModifier) : juce::ModifierKeys(), action, deck, 1.0f });
    save();
}

void KeyboardControl::resetToFactory()
{
    addDefaults();
    save();
}

void KeyboardControl::load (const juce::File& f)
{
    file = f;
    const auto json = juce::JSON::parse (f);
    if (auto* arr = json.getArray(); arr != nullptr && ! arr->isEmpty())
    {
        keys.clear();
        for (auto& v : *arr)
            keys.push_back ({ (int) v["key"], (bool) v["shift"] ? juce::ModifierKeys (juce::ModifierKeys::shiftModifier) : juce::ModifierKeys(),
                              v["action"].toString(), (int) v["deck"], (float) (double) v["value"] });
    }
}

void KeyboardControl::save() const
{
    if (file == juce::File()) return;
    juce::Array<juce::var> arr;
    for (auto& b : keys)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("key", b.keyCode);
        o->setProperty ("shift", b.mods.isShiftDown());
        o->setProperty ("action", b.action);
        o->setProperty ("deck", b.deck);
        o->setProperty ("value", b.value);
        arr.add (juce::var (o));
    }
    file.replaceWithText (juce::JSON::toString (juce::var (arr)));
}

juce::String KeyboardControl::describe (const Binding& b)
{
    juce::String k;
    if (b.mods.isShiftDown()) k << juce::String::fromUTF8 ("⇧");
    if (b.keyCode == juce::KeyPress::spaceKey)          k << "SPACE";
    else if (b.keyCode == juce::KeyPress::backspaceKey) k << juce::String::fromUTF8 ("⌫");
    else if (b.keyCode == juce::KeyPress::upKey)    k << juce::String::fromUTF8 ("↑");
    else if (b.keyCode == juce::KeyPress::downKey)  k << juce::String::fromUTF8 ("↓");
    else if (b.keyCode == juce::KeyPress::leftKey)  k << juce::String::fromUTF8 ("←");
    else if (b.keyCode == juce::KeyPress::rightKey) k << juce::String::fromUTF8 ("→");
    else k << juce::String::charToString ((juce::juce_wchar) b.keyCode).toUpperCase();
    return k;
}

bool KeyboardControl::keyPressed (const juce::KeyPress& kp)
{
    if (kp.getModifiers().isCommandDown() || kp.getModifiers().isCtrlDown())
        return false;

    int code = kp.getKeyCode();
    if (code >= 'A' && code <= 'Z') code += 'a' - 'A';
    const bool shiftDown = kp.getModifiers().isShiftDown();

    // Shifted digits/punctuation arrive as their shifted character; match on the text char too.
    for (const auto& b : keys)
    {
        if (b.mods.isShiftDown() != shiftDown) continue;
        if (b.keyCode != code && (int) kp.getTextCharacter() != b.keyCode) continue;

        const auto* info = actions.find (b.action);
        if (info == nullptr) return false;

        if (repeats (b.action))                 // steps: every auto-repeat acts again
        {
            actions.trigger (b.action, b.deck, b.value);
            return true;
        }

        // Ignore auto-repeat for momentary actions already held.
        for (auto& h : held)
            if (h.keyCode == b.keyCode && h.action == b.action && h.deck == b.deck)
                return true;

        actions.trigger (b.action, b.deck, b.value);
        if (info->type == ControlType::button)
            held.push_back ({ b.keyCode, b.action, b.deck });
        return true;
    }
    return false;
}

void KeyboardControl::tick()
{
    for (auto it = held.begin(); it != held.end();)
    {
        if (! juce::KeyPress::isKeyCurrentlyDown (it->keyCode))
        {
            actions.trigger (it->action, it->deck, 0.0f);   // release: momentary actions stop
            it = held.erase (it);
        }
        else ++it;
    }
}

} // namespace om
