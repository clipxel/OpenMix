#include "VdjScript.h"
#include <set>

namespace om
{

//==============================================================================
// Parsing: "a & b", "cond ? then : else", "( … )", "deck N verb args…".

struct VdjScript::Node
{
    enum class Kind { seq, cond, cmd } kind = Kind::seq;
    std::vector<NodePtr> items;            // seq
    NodePtr test, then, otherwise;         // cond
    juce::String deck, verb;               // cmd
    juce::StringArray args;
};

namespace
{
    juce::StringArray tokenise (const juce::String& text)
    {
        juce::StringArray t;
        auto p = text.getCharPointer();
        juce::String cur;
        auto flush = [&] { if (cur.isNotEmpty()) t.add (cur); cur.clear(); };
        while (! p.isEmpty())
        {
            const auto c = *p;
            if (c == '\'' || c == '"')                         // quoted parameter, kept with its quotes
            {
                flush();
                juce::String q;
                q << c;
                ++p;
                while (! p.isEmpty() && *p != c) { q << *p; ++p; }
                if (! p.isEmpty()) ++p;
                t.add (q << c);
                continue;
            }
            if (c == '&' || c == '?' || c == ':' || c == '(' || c == ')')
            {
                flush();
                if (c == '&' && *(p + 1) == '&') ++p;          // "&&" is "&"
                t.add (juce::String::charToString (c));
                ++p;
                continue;
            }
            if (juce::CharacterFunctions::isWhitespace (c)) { flush(); ++p; continue; }
            cur << c;
            ++p;
        }
        flush();
        return t;
    }

    bool isSymbol (const juce::String& t) { return t == "&" || t == "?" || t == ":" || t == "(" || t == ")"; }

    struct Parser
    {
        juce::StringArray t;
        int i = 0;

        juce::String peek() const { return i < t.size() ? t[i] : juce::String(); }
        bool atEnd() const        { return i >= t.size(); }

        using Node = VdjScript::Node;
        using NodePtr = VdjScript::NodePtr;

        NodePtr seq (bool inThen, bool inParen)
        {
            auto n = std::make_shared<Node>();
            n->kind = Node::Kind::seq;
            while (! atEnd())
            {
                const auto p = peek();
                if ((inThen && p == ":") || (inParen && p == ")")) break;
                if (p == "&") { ++i; continue; }
                if (p == ":" || p == ")") { ++i; continue; }          // stray: skip
                if (auto item = term (inThen, inParen)) n->items.push_back (item);
                else break;
            }
            return n;
        }

        NodePtr term (bool inThen, bool inParen)
        {
            NodePtr first;
            if (peek() == "(")
            {
                ++i;
                first = seq (false, true);
                if (peek() == ")") ++i;
            }
            else
                first = command();
            if (first == nullptr) return nullptr;
            if (peek() != "?") return first;

            ++i;
            auto c = std::make_shared<Node>();
            c->kind = Node::Kind::cond;
            c->test = first;
            c->then = seq (true, inParen);
            if (peek() == ":") ++i;
            c->otherwise = seq (inThen, inParen);                 // else runs to the end, as in VirtualDJ
            return c;
        }

        NodePtr command()
        {
            auto n = std::make_shared<Node>();
            n->kind = Node::Kind::cmd;
            while (peek().equalsIgnoreCase ("deck") && i + 1 < t.size())
            {
                n->deck = t[i + 1].toLowerCase();
                i += 2;
            }
            if (atEnd() || isSymbol (peek())) return nullptr;
            n->verb = t[i++].toLowerCase();
            while (! atEnd() && ! isSymbol (peek()))
                n->args.add (t[i++]);
            return n;
        }
    };

    /** "50%" → 0.5, "+1" → 1, "-4bt" → −4, "1/2" → 0.5, "0.25" → 0.25. */
    double number (juce::String a, bool* ok = nullptr)
    {
        a = a.trim().unquoted().toLowerCase();
        if (ok) *ok = a.containsAnyOf ("0123456789");
        if (a.containsChar ('/'))
        {
            const double den = a.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
            return den != 0.0 ? a.upToFirstOccurrenceOf ("/", false, false).getDoubleValue() / den : 0.0;
        }
        const bool percent = a.endsWithChar ('%');
        const double v = a.retainCharacters ("+-0123456789.").getDoubleValue();
        return percent ? v / 100.0 : v;
    }

    // What each verb does in OpenMix.
    enum class Kind
    {
        press,       // runs once on press (toggles, jumps)
        momentary,   // press and release (cue, censor, touch…)
        slider,      // a position 0…1 (or the parameter: "volume 50%")
        turn,        // a movement (jog, encoders, "browser_scroll +1")
    };
    struct Verb { const char* id; Kind kind; };

    const std::map<juce::String, Verb>& verbs()
    {
        static const std::map<juce::String, Verb> v {
            // transport
            { "play_pause", { "play_pause", Kind::press } },      { "play_button", { "play_pause", Kind::press } },
            { "smart_play", { "play_pause", Kind::press } },      { "cue_button", { "cue_button", Kind::momentary } },
            { "cue", { "cue_button", Kind::momentary } },         { "cue_stop", { "goto_cue_stop", Kind::press } },
            { "play_stutter", { "stutter", Kind::press } },       { "stutter", { "stutter", Kind::press } },
            { "goto_start", { "start_play", Kind::press } },      { "sync", { "sync", Kind::press } },
            { "slip_mode", { "slip_mode", Kind::press } },        { "slip", { "slip_mode", Kind::press } },
            { "quantize", { "quantize", Kind::press } },          { "quantize_setcue", { "quantize", Kind::press } },
            { "censor", { "censor", Kind::momentary } },          { "reverse", { "reverse", Kind::press } },
            { "brake", { "brake", Kind::press } },                { "dump", { "brake", Kind::press } },
            { "backspin", { "backspin", Kind::press } },          { "instant_doubles", { "instant_doubles", Kind::press } },
            { "clone_deck", { "instant_doubles", Kind::press } }, { "master_deck", { "master", Kind::press } },
            { "load", { "load", Kind::press } },                  { "load_track", { "load", Kind::press } },
            { "automix", { "automix", Kind::press } },            { "record", { "record", Kind::press } },
            { "vinyl_mode", { "vinyl_mode", Kind::press } },
            // loops
            { "loop_in", { "loop_in", Kind::press } },            { "loop_out", { "loop_out", Kind::press } },
            { "loop_exit", { "loop_exit", Kind::press } },        { "reloop", { "reloop", Kind::press } },
            { "loop_half", { "loop_half", Kind::press } },        { "loop_halve", { "loop_half", Kind::press } },
            { "loop_double", { "loop_double", Kind::press } },    { "loop_select", { "loop_resize", Kind::turn } },
            // tempo / key
            { "pitch_slider", { "pitch", Kind::slider } },        { "pitch", { "pitch", Kind::slider } },
            { "pitch_reset", { "pitch_reset", Kind::press } },    { "pitch_zero", { "pitch_reset", Kind::press } },
            { "pitch_range", { "pitch_range", Kind::press } },    { "key_lock", { "key_lock", Kind::press } },
            { "keylock", { "key_lock", Kind::press } },           { "master_tempo", { "key_lock", Kind::press } },
            { "key_match", { "key_match", Kind::press } },        { "match_key", { "key_match", Kind::press } },
            { "key_sync", { "key_match", Kind::press } },         { "key_reset", { "key_reset", Kind::press } },
            // mixer
            { "volume", { "volume", Kind::slider } },             { "volume_slider", { "volume", Kind::slider } },
            { "level", { "volume", Kind::slider } },              { "level_slider", { "volume", Kind::slider } },
            { "gain", { "gain", Kind::slider } },                 { "gain_slider", { "gain", Kind::slider } },
            { "trim", { "gain", Kind::slider } },                 { "eq_high", { "eq_high", Kind::slider } },
            { "eq_hi", { "eq_high", Kind::slider } },             { "eq_high_slider", { "eq_high", Kind::slider } },
            { "eq_mid", { "eq_mid", Kind::slider } },             { "eq_med", { "eq_mid", Kind::slider } },
            { "eq_mid_slider", { "eq_mid", Kind::slider } },      { "eq_low", { "eq_low", Kind::slider } },
            { "eq_bass", { "eq_low", Kind::slider } },            { "eq_low_slider", { "eq_low", Kind::slider } },
            { "filter", { "filter", Kind::slider } },             { "filter_slider", { "filter", Kind::slider } },
            { "color_fx", { "filter", Kind::slider } },           { "crossfader", { "crossfader", Kind::slider } },
            { "master_volume", { "master_volume", Kind::slider } }, { "headphone_volume", { "headphone_volume", Kind::slider } },
            { "headphone_gain", { "headphone_volume", Kind::slider } }, { "headphone_mix", { "headphone_mix", Kind::slider } },
            { "pfl", { "pfl", Kind::press } },                    { "headphones", { "pfl", Kind::press } },
            // jog
            { "jogwheel", { "jog", Kind::turn } },                { "jog_wheel", { "jog", Kind::turn } },
            { "scratch_wheel", { "jog", Kind::turn } },           { "nudge_wheel", { "jog", Kind::turn } },
            { "touchwheel", { "jog_touch", Kind::momentary } },   { "jogwheel_touch", { "jog_touch", Kind::momentary } },
            { "scratch_wheel_touch", { "jog_touch", Kind::momentary } }, { "touch_wheel", { "jog_touch", Kind::momentary } },
            { "jog_search", { "jog_search", Kind::turn } },       { "seek", { "jog_search", Kind::turn } },
            // browser
            { "browser_scroll", { "browser_scroll", Kind::turn } }, { "browser_switch", { "browser_focus", Kind::press } },
            { "browser_window", { "browser_focus", Kind::press } },
            // pads
            { "pad_page", { "pad_page", Kind::press } },
        };
        return v;
    }

    // Verbs that are about the script itself, not a function (conditions, values, variables).
    const std::set<juce::String>& languageVerbs()
    {
        static const std::set<juce::String> v { "nothing", "true", "false", "down", "up", "holding", "isrepeat", "shift",
            "param_bigger", "param_smaller", "param_equal", "param_invert", "param_multiply", "param_add", "set", "toggle",
            "var", "var_equal", "leftdeck", "rightdeck", "action_deck", "loaded", "loop", "auto_loop", "loop_roll",
            "hot_cue", "set_cue", "goto_cue", "delete_cue", "saved_loop", "loop_move", "beatjump", "goto", "key",
            "pitch_bend", "effect_active", "effect_slider", "effect_select", "pad", "play", "pause", "stop", "reloop_exit",
            "loop_button" };
        return v;
    }

    const std::set<juce::String>& continuousIds()
    {
        static const std::set<juce::String> v { "jog", "jog_search", "browser_scroll", "loop_resize", "pitch", "volume", "gain",
            "eq_high", "eq_mid", "eq_low", "filter", "crossfader", "master_volume", "headphone_volume", "headphone_mix" };
        return v;
    }

    int deckOf (const juce::String& spec, int current, int master)
    {
        if (spec.isEmpty() || spec == "active" || spec == "default" || spec == "current") return current;
        if (spec == "left" || spec == "1" || spec == "leftdeck") return 0;
        if (spec == "right" || spec == "2" || spec == "rightdeck") return 1;
        if (spec == "master") return master >= 0 ? master : current;
        if (spec == "other") return 1 - current;
        return -1;                                          // decks 3–4 (and anything else): not in OpenMix
    }
}

//==============================================================================
VdjScript::NodePtr VdjScript::parsed (const juce::String& script)
{
    if (auto it = cache.find (script); it != cache.end()) return it->second;
    Parser p { tokenise (script) };
    auto n = p.seq (false, false);
    if (cache.size() > 2000) cache.clear();
    cache[script] = n;
    return n;
}

void VdjScript::run (const juce::String& script, const Context& ctx)
{
    if (auto n = parsed (script)) exec (*n, ctx, false);
}

bool VdjScript::query (const juce::String& script, const Context& ctx)
{
    auto n = parsed (script);
    return n != nullptr && exec (*n, ctx, true);
}

bool VdjScript::exec (const Node& n, Context ctx, bool asQuery)
{
    switch (n.kind)
    {
        case Node::Kind::seq:
        {
            bool last = false;
            for (auto& item : n.items)
            {
                if (item->kind == Node::Kind::cmd && (item->verb == "param_invert" || item->verb == "param_multiply" || item->verb == "param_add"))
                {
                    // Value modifiers change the value for what follows in the chain.
                    const double x = item->args.isEmpty() ? 0.0 : number (item->args[0]);
                    if (item->verb == "param_invert") ctx.value = ctx.input == Input::turn ? -ctx.value : 1.0f - ctx.value;
                    else if (item->verb == "param_multiply") ctx.value = (float) (ctx.value * x);
                    else ctx.value = (float) (ctx.value + x);
                    continue;
                }
                last = exec (*item, ctx, asQuery);
            }
            return last;
        }
        case Node::Kind::cond:
            return exec (*n.test, ctx, true) ? (n.then ? exec (*n.then, ctx, asQuery) : false)
                                             : (n.otherwise ? exec (*n.otherwise, ctx, asQuery) : false);
        case Node::Kind::cmd:
        {
            const int d = deckOf (n.deck, ctx.deck, actions.getEngine().masterDeck.load());
            if (d < 0) return false;
            ctx.deck = d;
            return verb (n.verb, n.args, ctx, asQuery);
        }
    }
    return false;
}

bool VdjScript::verb (const juce::String& name, const juce::StringArray& args, Context& ctx, bool asQuery)
{
    const bool pressed = ctx.value > 0.5f;
    const bool isButton = ctx.input == Input::button;
    const int d = ctx.deck;
    auto& deck = actions.getEngine().deck (d);
    auto arg = [&args] (int i, bool* ok = nullptr) { return i < args.size() ? number (args[i], ok) : (ok ? (*ok = false, 0.0) : 0.0); };
    auto slot = [&] { const int s = (int) arg (0); return juce::isPositiveAndBelow (s - 1, 8) ? s - 1 : -1; };
    auto fire = [&] (const juce::String& id, float v) { actions.trigger (id, d, v); };
    auto onPress = [&] (auto&& fn) { if (! isButton || pressed) fn(); return true; };

    // Language: conditions, values, variables ---------------------------------
    if (name == "nothing" || name == "false") return false;
    if (name == "true")      return true;
    if (name == "down" || name == "holding") return isButton ? pressed : true;
    if (name == "up")        return isButton && ! pressed;
    if (name == "isrepeat")  return false;
    if (name == "shift")     return ctx.shift;
    if (name == "param_bigger")  return ctx.value > arg (0);
    if (name == "param_smaller") return ctx.value < arg (0);
    if (name == "param_equal")   return std::abs (ctx.value - arg (0)) < 1e-4;
    if (name == "leftdeck")  return d == 0;
    if (name == "rightdeck") return d == 1;
    if (name == "action_deck") return (int) arg (0) - 1 == d;
    if (name == "loaded")    return deck.hasTrack();
    if (name == "set" || name == "toggle" || name == "var" || name == "var_equal")
    {
        const auto var = args.isEmpty() ? juce::String() : args[0].unquoted() + (args[0].startsWith ("$") ? juce::String() : "@" + juce::String (d));
        auto& slotValue = vars[var];
        if (name == "var")       return slotValue != 0.0f;
        if (name == "var_equal") return std::abs (slotValue - arg (1)) < 1e-4;
        if (asQuery) return slotValue != 0.0f;
        if (! isButton || pressed)
            slotValue = name == "toggle" ? (slotValue != 0.0f ? 0.0f : 1.0f) : (float) (args.size() > 1 ? arg (1) : 1.0);
        return true;
    }

    // Verbs with a number ------------------------------------------------------
    if (name == "hot_cue" || name == "set_cue" || name == "goto_cue")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return deck.getHotCue (s) >= 0.0;
        fire ("hot_cue_" + juce::String (s + 1), isButton ? (pressed ? 1.0f : 0.0f) : 1.0f);
        return true;
    }
    if (name == "delete_cue")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return deck.getHotCue (s) >= 0.0;
        return onPress ([&] { deck.clearHotCue (s); });
    }
    if (name == "saved_loop")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return actions.isActive ("saved_loop_" + juce::String (s + 1), d);
        return onPress ([&] { deck.triggerSavedLoop (s); });
    }
    if (name == "loop" || name == "auto_loop" || name == "loop_button")
    {
        bool has = false;
        const double size = arg (0, &has);
        if (asQuery) return deck.isLoopActive() && ! deck.isRolling() && (! has || std::abs (deck.getLoopBeats() - size) < 0.01);
        return onPress ([&]
        {
            if (! deck.hasTrack()) return;
            if (deck.isLoopActive() && (! has || std::abs (deck.getLoopBeats() - size) < 0.01)) deck.exitLoop();
            else deck.setAutoLoop (has && size > 0.0 ? size : deck.getAutoLoopSize());
        });
    }
    if (name == "reloop_exit")
    {
        if (asQuery) return deck.isLoopActive();
        return onPress ([&] { if (deck.isLoopActive()) deck.exitLoop(); else deck.reloop(); });
    }
    if (name == "loop_roll")
    {
        const double size = args.isEmpty() ? 0.25 : arg (0);
        if (asQuery) return deck.isRolling() && std::abs (deck.getRollBeats() - size) < 1e-6;
        deck.loopRoll (size, isButton ? pressed : true);
        return true;
    }
    if (name == "loop_move")
    {
        if (asQuery) return false;
        const double beats = args.isEmpty() ? (ctx.input == Input::turn ? (double) ctx.value : 1.0) : arg (0);
        return onPress ([&] { deck.loopMove (beats * (args.isEmpty() ? deck.getLoopBeats() : 1.0)); });
    }
    if (name == "beatjump" || name == "goto")
    {
        if (asQuery) return false;
        double beats = args.isEmpty() ? deck.getAutoLoopSize() * (ctx.input == Input::turn ? (ctx.value < 0 ? -1.0 : 1.0) : 1.0) : arg (0);
        if (name == "goto" && args.size() > 0 && args[0].endsWithIgnoreCase ("ms")) { const double sec = beats / 1000.0; return onPress ([&] { deck.seek (deck.getPosition() + sec); }); }
        return onPress ([&] { deck.beatJump (beats); });
    }
    if (name == "key")
    {
        if (asQuery) return deck.getKeyShift() != 0;
        if (args.isEmpty()) return false;
        const auto a = args[0].trim();
        const int n = (int) std::lround (arg (0));
        return onPress ([&]
        {
            if (a.startsWithChar ('+') || a.startsWithChar ('-')) deck.setKeyShift (deck.getKeyShift() + n);
            else deck.setKeyShift (n);
        });
    }
    if (name == "pitch_bend")
    {
        if (asQuery) return false;
        const bool down = args.size() > 0 && args[0].trim().startsWithChar ('-');
        fire (down ? "pitch_bend_minus" : "pitch_bend_plus", isButton ? (pressed ? 1.0f : 0.0f) : 1.0f);
        return true;
    }
    if (name == "effect_active" || name == "effect_slider" || name == "effect_select")
    {
        bool has = false;
        int unit = (int) arg (0, &has);
        if (! has || unit < 1 || unit > AudioEngine::numDeckFx) unit = 1;
        const auto n = juce::String (unit);
        if (name == "effect_active")
        {
            if (asQuery) return actions.isActive ("effect_active_" + n, d);
            return onPress ([&] { fire ("effect_active_" + n, 1.0f); });
        }
        if (name == "effect_slider")
        {
            if (asQuery) return false;
            fire ("effect_wet_" + n, ctx.value);
            return true;
        }
        if (asQuery) return false;
        return onPress ([&] { fire ("effect_select_" + n, 1.0f); });
    }
    if (name == "pad")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return actions.isActive ("pad_" + juce::String (s + 1), d);
        fire ("pad_" + juce::String (s + 1), isButton ? (pressed ? 1.0f : 0.0f) : 1.0f);
        return true;
    }
    if (name == "play" || name == "pause" || name == "stop")
    {
        if (asQuery) return name == "play" ? deck.isPlaying() : ! deck.isPlaying();
        return onPress ([&]
        {
            if (name == "play") deck.setPlaying (true);
            else if (name == "pause") deck.setPlaying (false);
            else { deck.setPlaying (false); deck.seek (deck.getCuePoint()); }
        });
    }

    // Plain verbs: one OpenMix function each ---------------------------------
    auto it = verbs().find (name);
    if (it == verbs().end())
        return false;
    const juce::String id = it->second.id;
    if (asQuery) return actions.isActive (id, d);

    switch (it->second.kind)
    {
        case Kind::press:
            return onPress ([&] { fire (id, 1.0f); });
        case Kind::momentary:
            fire (id, isButton ? (pressed ? 1.0f : 0.0f) : (ctx.value > 0.5f ? 1.0f : 0.0f));
            return true;
        case Kind::slider:
        {
            bool has = false;
            const double v = arg (0, &has);
            if (has)                                            // "volume 50%", "crossfader 0.5", "pitch +0.1"
            {
                if (isButton && ! pressed) return true;
                fire (id, (float) juce::jlimit (0.0, 1.0, args[0].trim().startsWithChar ('+') || args[0].trim().startsWithChar ('-')
                                                            ? actions.currentValue (id, d) + v : v));
            }
            else if (! isButton)
                fire (id, juce::jlimit (0.0f, 1.0f, ctx.value));
            return true;
        }
        case Kind::turn:
        {
            bool has = false;
            const double v = arg (0, &has);
            if (isButton) { if (pressed) fire (id, (float) (has ? v : 1.0)); }
            else fire (id, has ? (float) (v * (ctx.value < 0 ? -1.0 : 1.0)) : ctx.value);
            return true;
        }
    }
    return false;
}

juce::String VdjScript::mainAction (const juce::String& script, int)
{
    // The first command that names a function.
    std::function<juce::String (const Node&)> first = [&first] (const Node& n) -> juce::String
    {
        if (n.kind == Node::Kind::seq) { for (auto& i : n.items) if (auto s = first (*i); s.isNotEmpty()) return s; return {}; }
        if (n.kind == Node::Kind::cond)
        {
            if (n.otherwise) if (auto s = first (*n.otherwise); s.isNotEmpty()) return s;
            return n.then ? first (*n.then) : juce::String();
        }
        const int s = n.args.isEmpty() ? 0 : (int) number (n.args[0]);
        if ((n.verb == "hot_cue" || n.verb == "set_cue" || n.verb == "goto_cue" || n.verb == "delete_cue") && s >= 1 && s <= 8) return "hot_cue_" + juce::String (s);
        if (n.verb == "saved_loop" && s >= 1 && s <= 8) return "saved_loop_" + juce::String (s);
        if (n.verb == "loop" || n.verb == "loop_roll" || n.verb == "reloop_exit") return "loop";
        if (n.verb == "effect_active") return "effect_active_" + juce::String (juce::jlimit (1, AudioEngine::numDeckFx, s));
        if (auto it = verbs().find (n.verb); it != verbs().end()) return it->second.id;
        return {};
    };
    auto n = parsed (script);
    return n != nullptr ? first (*n) : juce::String();
}

juce::StringArray VdjScript::unknownVerbs (const juce::String& script)
{
    juce::StringArray out;
    Parser p { tokenise (script) };
    std::function<void (const Node&)> walk = [&] (const Node& n)
    {
        if (n.kind == Node::Kind::seq) for (auto& i : n.items) walk (*i);
        else if (n.kind == Node::Kind::cond) { if (n.test) walk (*n.test); if (n.then) walk (*n.then); if (n.otherwise) walk (*n.otherwise); }
        else if (verbs().count (n.verb) == 0 && languageVerbs().count (n.verb) == 0) out.addIfNotAlreadyThere (n.verb);
    };
    if (auto n = p.seq (false, false)) walk (*n);
    return out;
}

bool VdjScript::isContinuous (const juce::String& script)
{
    bool any = false;
    Parser p { tokenise (script) };
    std::function<void (const Node&)> walk = [&] (const Node& n)
    {
        if (n.kind == Node::Kind::seq) for (auto& i : n.items) walk (*i);
        else if (n.kind == Node::Kind::cond) { if (n.then) walk (*n.then); if (n.otherwise) walk (*n.otherwise); }
        else if (auto it = verbs().find (n.verb); it != verbs().end() && n.args.isEmpty() && continuousIds().count (it->second.id) > 0) any = true;
        else if (n.verb == "effect_slider") any = true;
    };
    if (auto n = p.seq (false, false)) walk (*n);
    return any;
}

} // namespace om
