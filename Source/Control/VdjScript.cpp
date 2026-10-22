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
            if (c == '\'' || c == '"' || c == '`')                 // quoted parameter, kept with its quotes
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

    /** "50%" → 0.5, "+1" → 1, "-4bt" → −4, "1/2" → 0.5, "0.25" → 0.25, "500ms" → 500. */
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

    bool isRelative (const juce::String& a) { const auto s = a.trim(); return s.startsWithChar ('+') || s.startsWithChar ('-'); }

    juce::Colour namedColour (const juce::StringArray& args)
    {
        if (args.isEmpty()) return {};
        const auto a = args[0].unquoted().trim().toLowerCase();
        if (a.startsWithChar ('#') || a.startsWith ("0x"))
            return juce::Colour ((juce::uint32) (0xff000000 | (juce::uint32) a.retainCharacters ("0123456789abcdef").fromFirstOccurrenceOf (a.startsWith ("0x") ? "0x" : "", false, false).getHexValue32()));
        if (args.size() >= 3)                                       // "color 1 0 0" or "color 255 0 0"
        {
            double r = number (args[0]), g = number (args[1]), b = number (args[2]);
            const double k = (r > 1.0 || g > 1.0 || b > 1.0) ? 1.0 : 255.0;
            return juce::Colour ((juce::uint8) juce::jlimit (0.0, 255.0, r * k), (juce::uint8) juce::jlimit (0.0, 255.0, g * k), (juce::uint8) juce::jlimit (0.0, 255.0, b * k));
        }
        static const std::map<juce::String, juce::uint32> names {
            { "red", 0xffff0000 }, { "green", 0xff00ff00 }, { "blue", 0xff0000ff }, { "yellow", 0xffffff00 },
            { "orange", 0xffff8000 }, { "purple", 0xff8000ff }, { "violet", 0xff8000ff }, { "magenta", 0xffff00ff },
            { "pink", 0xffff60c0 }, { "fuchsia", 0xffff00ff }, { "cyan", 0xff00ffff }, { "aqua", 0xff00ffff },
            { "white", 0xffffffff }, { "black", 0xff000000 }, { "gray", 0xff808080 }, { "grey", 0xff808080 },
            { "lime", 0xff80ff00 }, { "turquoise", 0xff00ffc0 }, { "brown", 0xff804000 }, { "gold", 0xffffc000 } };
        if (auto it = names.find (a); it != names.end()) return juce::Colour (it->second);
        return {};
    }

    // What each plain verb does in OpenMix.
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
            { "play_3button", { "play_pause", Kind::press } },    { "smart_play", { "play_pause", Kind::press } },
            { "auto_sync", { "play_pause", Kind::press } },       { "cue_button", { "cue_button", Kind::momentary } },
            { "cue_3button", { "cue_button", Kind::momentary } }, { "cue_cup", { "cue_button", Kind::momentary } },
            { "play_stutter", { "stutter", Kind::press } },       { "goto_start", { "start_play", Kind::press } },
            { "sync", { "sync", Kind::press } },                  { "play_sync", { "sync", Kind::press } },
            { "match_bpm", { "sync", Kind::press } },             { "play_sync_onbeat", { "sync", Kind::press } },
            { "sync_nocbg", { "sync", Kind::press } },            { "slip_mode", { "slip_mode", Kind::press } },
            { "quantize", { "quantize", Kind::press } },          { "quantize_setcue", { "quantize", Kind::press } },
            { "quantize_all", { "quantize", Kind::press } },      { "censor", { "censor", Kind::momentary } },
            { "reverse", { "reverse", Kind::press } },            { "dump", { "censor", Kind::momentary } },
            { "brake", { "brake", Kind::press } },                { "clone_deck", { "instant_doubles", Kind::press } },
            { "clone_from_deck", { "instant_doubles", Kind::press } }, { "instant_doubles", { "instant_doubles", Kind::press } },
            { "masterdeck", { "master", Kind::press } },          { "master_deck", { "master", Kind::press } },
            { "automix", { "automix", Kind::press } },            { "record", { "record", Kind::press } },
            { "vinyl_mode", { "vinyl_mode", Kind::press } },      { "emergency_play", { "emergency_play", Kind::press } },
            { "mix_now", { "mix_now", Kind::press } },            { "mix_now_nosync", { "mix_now", Kind::press } },
            // loops
            { "loop_in", { "loop_in", Kind::press } },            { "loop_out", { "loop_out", Kind::press } },
            { "pioneer_loop_in", { "loop_in", Kind::press } },    { "pioneer_loop_out", { "loop_out", Kind::press } },
            { "loop_exit", { "loop_exit", Kind::press } },        { "reloop", { "reloop", Kind::press } },
            { "loop_half", { "loop_half", Kind::press } },        { "loop_halve", { "loop_half", Kind::press } },
            { "loop_double", { "loop_double", Kind::press } },
            // tempo / key
            { "pitch_slider", { "pitch", Kind::slider } },        { "pitch", { "pitch", Kind::slider } },
            { "pitch2", { "pitch", Kind::slider } },              { "pitch2_slider", { "pitch", Kind::slider } },
            { "pitch_motorized", { "pitch", Kind::slider } },     { "pitch_reset", { "pitch_reset", Kind::press } },
            { "pitch_zero", { "pitch_reset", Kind::press } },     { "key_lock", { "key_lock", Kind::press } },
            { "keylock", { "key_lock", Kind::press } },           { "master_tempo", { "key_lock", Kind::press } },
            { "key_match_button", { "key_match", Kind::press } }, { "key_reset", { "key_reset", Kind::press } },
            { "beat_tap", { "beat_fx_tap", Kind::press } },
            // mixer
            { "volume", { "volume", Kind::slider } },             { "volume_slider", { "volume", Kind::slider } },
            { "level", { "volume", Kind::slider } },              { "level_slider", { "volume", Kind::slider } },
            { "gain", { "gain", Kind::slider } },                 { "gain_slider", { "gain", Kind::slider } },
            { "power_gain", { "gain", Kind::slider } },           { "trim", { "gain", Kind::slider } },
            { "eq_high", { "eq_high", Kind::slider } },           { "eq_high_slider", { "eq_high", Kind::slider } },
            { "eq_hi", { "eq_high", Kind::slider } },             { "eq_mid", { "eq_mid", Kind::slider } },
            { "eq_med", { "eq_mid", Kind::slider } },             { "eq_mid_slider", { "eq_mid", Kind::slider } },
            { "eq_low", { "eq_low", Kind::slider } },             { "eq_low_slider", { "eq_low", Kind::slider } },
            { "eq_bass", { "eq_low", Kind::slider } },            { "filter", { "filter", Kind::slider } },
            { "filter_slider", { "filter", Kind::slider } },      { "colorfx_slider", { "filter", Kind::slider } },
            { "effect_colorslider", { "filter", Kind::slider } }, { "filter_selectcolorfx", { "color_fx_next", Kind::press } },
            { "crossfader", { "crossfader", Kind::slider } },     { "crossfader_slider", { "crossfader", Kind::slider } },
            { "master_volume", { "master_volume", Kind::slider } }, { "headphone_volume", { "headphone_volume", Kind::slider } },
            { "headphone_gain", { "headphone_volume", Kind::slider } }, { "headphone_mix", { "headphone_mix", Kind::slider } },
            // jog: "jogwheel" / "touchwheel" are the turn of the wheel, "…_touch" the touch
            { "jogwheel", { "jog", Kind::turn } },                { "jog", { "jog", Kind::turn } },
            { "jog_wheel", { "jog", Kind::turn } },               { "touchwheel", { "jog", Kind::turn } },
            { "scratch_wheel", { "jog", Kind::turn } },           { "scratchwheel", { "jog", Kind::turn } },
            { "scratch", { "jog", Kind::turn } },                 { "nudge", { "jog", Kind::turn } },
            { "touchwheel_touch", { "jog_touch", Kind::momentary } }, { "scratch_wheel_touch", { "jog_touch", Kind::momentary } },
            { "scratchwheel_touch", { "jog_touch", Kind::momentary } }, { "speedwheel_touch", { "jog_touch", Kind::momentary } },
            { "jogwheel_touch", { "jog_touch", Kind::momentary } }, { "hold", { "jog_touch", Kind::momentary } },
            { "scratch_hold", { "jog_touch", Kind::momentary } },
            // browser
            { "browser_scroll", { "browser_scroll", Kind::turn } }, { "browser_switch", { "browser_focus", Kind::press } },
            { "load", { "load", Kind::press } },
            // pads
            { "pad_page", { "pad_page", Kind::press } },          { "pad_pages", { "pad_page", Kind::press } },
        };
        return v;
    }

    // Verbs handled one by one below (with numbers, conditions, values, variables…).
    const std::set<juce::String>& specialVerbs()
    {
        static const std::set<juce::String> v {
            "nothing", "true", "false", "on", "off", "yes", "no", "dim", "blink", "color", "constant", "get_constant",
            "down", "up", "holding", "doubleclick", "isrepeat", "shift", "pulse",
            "param_bigger", "param_greater", "param_smaller", "param_equal", "param_invert", "param_multiply", "param_add",
            "param_1_x", "param_pow", "param_mod", "param_delta", "param_make_discrete", "param_cast", "param_pingpong",
            "set", "toggle", "cycle", "var", "var_equal", "var_not_equal", "var_smaller", "var_greater", "get_var", "set_var",
            "leftdeck", "rightdeck", "action_deck", "device_side", "set_deck", "select", "masterdeck_auto",
            "loaded", "is_audible", "play", "pause", "stop", "pause_stop", "stop_button", "stop_3button",
            "cue", "cue_stop", "cue_play", "hot_cue", "hotcue", "set_cue", "goto_cue", "delete_cue", "has_cue", "cue_color", "cue_loop",
            "loop", "loop_button", "pioneer_loop", "auto_loop", "loop_roll", "loop_move", "loop_length", "loop_select", "reloop_exit",
            "loop_save", "loop_load", "loop_load_prepare", "loop_delete", "saved_loop", "saved_loop_prepare", "loop_pad",
            "beatjump", "beatjump_select", "beatjump_pad", "goto", "song_pos", "seek", "goto_first_beat", "songpos_remain",
            "songpos_warning", "beat_juggle", "key", "key_move", "key_smooth", "match_key", "key_sync", "key_match", "set_key",
            "pitch_bend", "pitch_relative", "pitch_range", "get_pitch_zero", "backspin", "is_sync", "sync_hint",
            "gain_relative", "eq_kill_high", "eq_kill_mid", "eq_kill_med", "eq_kill_low", "eq_reset", "pfl", "headphones", "listen",
            "effect_active", "effect_activate", "effect_slider", "effect_slider_slider", "effect_select", "effect_select_multi",
            "effect_button", "effect_beats", "pad", "pad_button_color", "pad_color", "pad_pushed",
            "load_next", "load_previous", "set_firstbeat", "edit_bpm", "reanalyze",
            "fake_mixer", "fake_eq", "fake_gain", "fake_hp", "fake_hpmix", "fake_pfl", "fake_master", "fake_filter", "mixermode",
            "pioneer_play", "pioneer_cue", "pad_pages", "padshift", "padshift_button_color", "filter_activate", "crossfader_curve",
            "crossfader_disable", "browser_window", "browser_folder", "browser_enter", "goto_last_folder", "sideview", "browser_geniusdj",
            "unload", "get_slip_active", "loop_adjust", "setting", "is_using", "pad_param", "pad_param2",
            "get_level", "get_vu_meter", "get_level_left", "get_level_right", "get_vu_meter_left", "get_vu_meter_right",
        };
        return v;
    }

    const std::set<juce::String>& continuousIds()
    {
        static const std::set<juce::String> v { "jog", "jog_search", "browser_scroll", "loop_resize", "pitch", "volume", "gain",
            "eq_high", "eq_mid", "eq_low", "filter", "crossfader", "master_volume", "headphone_volume", "headphone_mix" };
        return v;
    }

    int deckOf (const juce::String& spec, int current, const AudioEngine& engine)
    {
        if (spec.isEmpty() || spec == "active" || spec == "default" || spec == "current" || spec == "this" || spec == "all") return current;
        if (spec == "left" || spec == "leftdeck") return engine.visibleDeck (0);
        if (spec == "right" || spec == "rightdeck") return engine.visibleDeck (1);
        if (spec == "master") { const int m = engine.masterDeck.load(); return m >= 0 ? m : current; }
        if (spec == "other") return engine.otherDeck (current);
        const int n = spec.getIntValue();
        if (n >= 1 && n <= AudioEngine::numDecks) return n - 1;
        return -1;
    }

    double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
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
    auto c = ctx;
    if (auto n = parsed (script)) exec (*n, c, false);
}

bool VdjScript::query (const juce::String& script, const Context& ctx)
{
    auto c = ctx;
    auto n = parsed (script);
    return n != nullptr && exec (*n, c, true);
}

VdjScript::Light VdjScript::light (const juce::String& script, const Context& ctx)
{
    hint = {};
    auto c = ctx;
    auto n = parsed (script);
    const bool on = n != nullptr && exec (*n, c, true);
    Light l;
    l.colour = hint.colour;
    l.level = ! on ? Actions::Led::off : hint.level != Actions::Led::off ? hint.level : Actions::Led::on;
    if (l.colour.isTransparent())
        if (const auto id = mainAction (script, ctx.deck); id.isNotEmpty())
            l.colour = actions.ledColour (id, ctx.deck);
    return l;
}

bool VdjScript::exec (const Node& n, Context& ctx, bool asQuery)
{
    switch (n.kind)
    {
        case Node::Kind::seq:
        {
            bool last = false;
            for (auto& item : n.items)
                last = exec (*item, ctx, asQuery);          // value modifiers change ctx for what follows
            return last;
        }
        case Node::Kind::cond:
        {
            // Long press / double press: the branch is decided by timing, not by a state.
            if (n.test != nullptr && n.test->kind == Node::Kind::cmd && ! asQuery && ctx.input == Input::button
                && (n.test->verb == "holding" || n.test->verb == "doubleclick"))
            {
                const auto key = ctx.control + "|" + juce::String (ctx.deck) + "|" + n.test->verb;
                const bool pressed = ctx.value > 0.5f;
                const double t = n.test->args.isEmpty() ? (n.test->verb == "holding" ? 500.0 : 300.0) : number (n.test->args[0]);
                if (n.test->verb == "doubleclick")
                {
                    static std::map<juce::String, bool> chosen;
                    if (pressed)
                    {
                        const bool dbl = nowMs() - lastTap[key] < t;
                        lastTap[key] = dbl ? 0.0 : nowMs();
                        chosen[key] = dbl;
                    }
                    const auto& branch = chosen[key] ? n.then : n.otherwise;
                    return branch != nullptr && exec (*branch, ctx, false);
                }
                // holding: the "then" side runs once the press lasts `t` ms; a shorter press runs the
                // "else" side as a tap (press and release) when the button comes up.
                static std::map<juce::String, int> generation;
                static std::map<juce::String, bool> fired;
                if (pressed)
                {
                    pressedAt[key] = nowMs();
                    fired[key] = false;
                    const int gen = ++generation[key];
                    auto thenBranch = n.then;
                    auto c = ctx;
                    juce::Timer::callAfterDelay ((int) t, [this, flag = alive, key, gen, thenBranch, c]() mutable
                    {
                        if (! *flag || generation[key] != gen || pressedAt[key] <= 0.0) return;   // released already
                        fired[key] = true;
                        if (thenBranch) exec (*thenBranch, c, false);
                    });
                    return true;
                }
                pressedAt[key] = 0.0;
                ++generation[key];
                if (fired[key])
                {
                    if (n.then) exec (*n.then, ctx, false);                  // release of the held function
                }
                else if (n.otherwise)
                {
                    auto down = ctx;
                    down.value = 1.0f;
                    exec (*n.otherwise, down, false);
                    exec (*n.otherwise, ctx, false);
                }
                return true;
            }
            auto test = ctx;
            return exec (*n.test, test, true) ? (n.then ? exec (*n.then, ctx, asQuery) : false)
                                              : (n.otherwise ? exec (*n.otherwise, ctx, asQuery) : false);
        }
        case Node::Kind::cmd:
        {
            const int d = deckOf (n.deck, ctx.deck, actions.getEngine());
            if (d < 0) return false;
            if (n.deck.isEmpty()) return verb (n.verb, n.args, ctx, asQuery);
            auto c = ctx;
            c.deck = d;
            return verb (n.verb, n.args, c, asQuery);
        }
    }
    return false;
}

bool VdjScript::verb (const juce::String& name, const juce::StringArray& args, Context& ctx, bool asQuery)
{
    const bool pressed = ctx.value > 0.5f;
    const bool isButton = ctx.input == Input::button;
    const int d = juce::jlimit (0, AudioEngine::numDecks - 1, ctx.deck);
    auto& engine = actions.getEngine();
    auto& deck = engine.deck (d);
    auto arg = [&args] (int i, bool* ok = nullptr) -> double
    {
        if (i < args.size()) return number (args[i], ok);
        if (ok) *ok = false;
        return 0.0;
    };
    auto slot = [&] { const int s = (int) arg (0); return juce::isPositiveAndBelow (s - 1, 8) ? s - 1 : -1; };
    auto fire = [&] (const juce::String& id, float v) { actions.trigger (id, d, v); };
    auto onPress = [&] (auto&& fn) { if (! isButton || pressed) fn(); return true; };
    auto press = [&] (const juce::String& id) { return onPress ([&] { fire (id, 1.0f); }); };
    auto momentary = [&] (const juce::String& id) { fire (id, isButton ? (pressed ? 1.0f : 0.0f) : (ctx.value > 0.5f ? 1.0f : 0.0f)); return true; };
    auto beats = [&] (double fallback)
    {
        if (args.isEmpty()) return fallback;
        const auto a = args[0].trim().toLowerCase();
        double v = arg (0);
        if (a.endsWith ("ms") && deck.getEffectiveBpm() > 1.0) v = v / 1000.0 * deck.getEffectiveBpm() / 60.0;
        return v;
    };

    // Flow and values -----------------------------------------------------------
    if (name == "nothing" || name == "false" || name == "off" || name == "no") return false;
    if (name == "true" || name == "on" || name == "yes") return true;
    if (name == "dim")       { if (asQuery) hint.level = Actions::Led::dim; return true; }
    if (name == "blink")     { if (asQuery) hint.level = actions.blinkPhase (d) ? Actions::Led::on : Actions::Led::off; return actions.blinkPhase (d); }
    if (name == "color")     { hint.colour = namedColour (args); return true; }
    if (name == "constant" || name == "get_constant") return arg (0) != 0.0;
    if (name == "down")      return isButton ? pressed : true;
    if (name == "up")        return isButton && ! pressed;
    if (name == "holding" || name == "doubleclick") return isButton && pressed;
    if (name == "isrepeat" || name == "pulse") return false;
    if (name == "shift")     return ctx.shift;
    if (name == "param_bigger" || name == "param_greater") return ctx.value > arg (0);
    if (name == "param_smaller") return ctx.value < arg (0);
    if (name == "param_equal")   return std::abs (ctx.value - arg (0)) < 1e-4;
    if (name == "param_invert")  { ctx.value = ctx.input == Input::turn ? -ctx.value : 1.0f - ctx.value; return true; }
    if (name == "param_multiply") { ctx.value = (float) (ctx.value * arg (0)); return true; }
    if (name == "param_add")      { ctx.value = (float) (ctx.value + arg (0)); return true; }
    if (name == "param_1_x")      { ctx.value = std::abs (ctx.value) > 1e-6f ? 1.0f / ctx.value : 0.0f; return true; }
    if (name == "param_pow")      { ctx.value = (float) std::pow (std::abs ((double) ctx.value), arg (0)) * (ctx.value < 0 ? -1.0f : 1.0f); return true; }
    if (name == "param_mod")      { const double m = arg (0); if (m > 0.0) ctx.value = (float) std::fmod ((double) ctx.value, m); return true; }
    if (name == "param_pingpong") { ctx.value = 1.0f - std::abs (1.0f - 2.0f * ctx.value); return true; }
    if (name == "param_delta" || name == "param_make_discrete" || name == "param_cast") return true;

    // Variables ---------------------------------------------------------------------
    if (name == "set" || name == "toggle" || name == "cycle" || name == "var" || name == "var_equal" || name == "var_not_equal"
        || name == "var_smaller" || name == "var_greater" || name == "get_var" || name == "set_var")
    {
        if (args.isEmpty()) return false;
        const auto raw = args[0].unquoted();
        const auto var = raw.startsWith ("$") || raw.startsWith ("@$") ? raw : raw + "@" + juce::String (d);
        auto& v = vars[var];
        if (name == "var" || name == "get_var") return v != 0.0f;
        if (name == "var_equal")     return std::abs (v - arg (1)) < 1e-4;
        if (name == "var_not_equal") return std::abs (v - arg (1)) >= 1e-4;
        if (name == "var_smaller")   return v < arg (1);
        if (name == "var_greater")   return v > arg (1);
        if (asQuery) return v != 0.0f;
        if (isButton && ! pressed) return true;
        if (name == "toggle") v = v != 0.0f ? 0.0f : 1.0f;
        else if (name == "cycle") { const double lim = juce::jmax (1.0, arg (1)); v = (float) std::fmod ((double) v + 1.0, lim); }
        else v = (float) (args.size() > 1 ? arg (1) : (isButton ? 1.0 : ctx.value));
        return true;
    }

    // Decks -------------------------------------------------------------------------
    if (name == "leftdeck" || name == "rightdeck")
    {
        const int side = name == "leftdeck" ? 0 : 1;
        if (! args.isEmpty() && ! asQuery)                                   // "leftdeck 3": show deck 3 on the left
            return onPress ([&] { const int n = (int) arg (0) - 1; if (n % 2 == side) engine.showDeck (n); });
        return d % 2 == side;
    }
    if (name == "action_deck") return (int) arg (0) - 1 == d;
    if (name == "device_side") { const auto s = args.isEmpty() ? juce::String() : args[0].unquoted().toLowerCase(); return s == "left" ? d == 0 : s == "right" ? d == 1 : false; }
    if (name == "select")    { if (asQuery) return engine.isVisible (d); return onPress ([&] { engine.showDeck (d); }); }
    if (name == "set_deck")  return true;
    if (name == "masterdeck_auto") { if (! asQuery) onPress ([&] { engine.masterDeck = -1; }); return engine.masterDeck.load() < 0; }
    if (name == "loaded")    return deck.hasTrack();
    if (name == "is_audible") return deck.isAudiblyPlaying();
    if (name == "fake_mixer" || name == "fake_eq" || name == "fake_gain" || name == "fake_hp" || name == "fake_hpmix"
        || name == "fake_pfl" || name == "fake_master" || name == "fake_filter" || name == "mixermode")
        return engine.outputMode.load() == AudioEngine::separateDecks;            // the unit mixes itself
    if (name == "get_level" || name == "get_vu_meter" || name.startsWith ("get_level_") || name.startsWith ("get_vu_meter_"))
        return deck.isAudiblyPlaying();                                      // (the meters themselves are lights of their own)

    // Transport --------------------------------------------------------------------
    if (name == "play" || name == "pause" || name == "stop" || name == "pause_stop" || name == "stop_button" || name == "stop_3button")
    {
        if (asQuery) return name == "play" ? deck.isPlaying() : ! deck.isPlaying();
        return onPress ([&]
        {
            if (name == "play") deck.setPlaying (true);
            else if (name == "pause") deck.setPlaying (false);
            else if (deck.isPlaying() || name == "stop" || name == "stop_button" || name == "stop_3button")
            {
                const bool wasPlaying = deck.isPlaying();
                deck.setPlaying (false);
                if (! wasPlaying || name != "pause_stop") deck.seek (deck.getCuePoint());
            }
            else deck.seek (deck.getCuePoint());
        });
    }
    if (name == "cue" || name == "cue_stop" || name == "cue_play")
    {
        const int s = slot();
        if (s >= 0)                                                     // "cue 3": that hot cue
        {
            if (asQuery) return deck.getHotCue (s) >= 0.0;
            fire ("hot_cue_" + juce::String (s + 1), isButton ? (pressed ? 1.0f : 0.0f) : 1.0f);
            return true;
        }
        if (asQuery) return actions.isActive ("cue_button", d);
        if (name == "cue_stop") return press ("goto_cue_stop");
        return momentary ("cue_button");
    }
    if (name == "seek")
    {
        if (asQuery) return false;
        const double amount = args.isEmpty() ? (double) ctx.value : arg (0) * (ctx.input == Input::turn && ctx.value < 0 ? -1.0 : 1.0);
        if (ctx.input == Input::turn) { fire ("jog_search", ctx.value); return true; }
        return onPress ([&] { if (deck.hasTrack()) deck.seek (juce::jlimit (0.0, deck.getLength(), deck.getPosition() + amount)); });
    }
    if (name == "song_pos")
    {
        if (asQuery) return false;
        if (! isButton && deck.hasTrack()) deck.seek (juce::jlimit (0.0, 1.0, (double) ctx.value) * deck.getLength());
        return true;
    }
    if (name == "goto")
    {
        if (asQuery || args.isEmpty()) return false;
        const auto a = args[0].trim().toLowerCase();
        return onPress ([&]
        {
            if (! deck.hasTrack()) return;
            if (a.endsWithChar ('%')) { deck.seek (juce::jlimit (0.0, 1.0, arg (0)) * deck.getLength()); return; }
            if (a.endsWith ("ms")) { deck.seek (juce::jlimit (0.0, deck.getLength(), (isRelative (a) ? deck.getPosition() : 0.0) + arg (0) / 1000.0)); return; }
            deck.beatJump (arg (0));                                     // beats
        });
    }
    if (name == "goto_first_beat") { if (asQuery) return false; return onPress ([&] { if (deck.hasTrack()) deck.seek (deck.getGrid().firstBeatSeconds); }); }
    if (name == "songpos_remain")  return deck.hasTrack() && deck.getLength() - deck.getPosition() < (args.isEmpty() ? 30.0 : arg (0) / (args[0].containsIgnoreCase ("ms") ? 1000.0 : 1.0));
    if (name == "songpos_warning") return deck.hasTrack() && deck.isPlaying() && deck.getLength() - deck.getPosition() < 30.0;
    if (name == "beat_juggle")     { if (asQuery) return false; return onPress ([&] { deck.beatJump (args.isEmpty() ? 1.0 : arg (0)); }); }
    if (name == "backspin")        { if (asQuery) return deck.isBackspinning(); return press ("backspin"); }
    if (name == "load_next" || name == "load_previous")
    {
        if (asQuery) return false;
        return onPress ([&] { fire ("browser_scroll", name == "load_next" ? 1.0f : -1.0f); fire ("load", 1.0f); });
    }
    if (name == "set_firstbeat") { if (asQuery) return false; return onPress ([&] { deck.setDownbeatHere(); }); }
    if (name == "edit_bpm" || name == "reanalyze") return false;

    // Cues ---------------------------------------------------------------------------
    if (name == "hot_cue" || name == "hotcue" || name == "set_cue" || name == "goto_cue")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return deck.getHotCue (s) >= 0.0;
        if (name == "set_cue") return onPress ([&] { if (deck.getHotCue (s) < 0.0) deck.hotCue (s); });
        if (name == "goto_cue") return onPress ([&] { if (deck.getHotCue (s) >= 0.0) deck.hotCue (s); });
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
    if (name == "has_cue") { const int s = slot(); return s >= 0 && deck.getHotCue (s) >= 0.0; }
    if (name == "cue_color")
    {
        const int s = slot();
        if (s < 0) return false;
        hint.colour = actions.ledColour ("hot_cue_" + juce::String (s + 1), d);
        return deck.getHotCue (s) >= 0.0;
    }
    if (name == "cue_loop")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return deck.getHotCue (s) >= 0.0 && deck.isLoopActive();
        return onPress ([&] { if (deck.getHotCue (s) >= 0.0) { deck.hotCue (s); deck.setAutoLoop (deck.getAutoLoopSize()); } });
    }

    // Loops ---------------------------------------------------------------------------
    if (name == "loop" || name == "auto_loop" || name == "loop_button" || name == "pioneer_loop")
    {
        bool has = false;
        const double size = arg (0, &has);
        if (asQuery) return deck.isLoopActive() && ! deck.isRolling() && (! has || std::abs (deck.getLoopBeats() - size) < 0.01);
        if (! isButton && ctx.input == Input::turn)                     // a loop encoder
        {
            fire ("loop_resize", ctx.value);
            return true;
        }
        return onPress ([&]
        {
            if (! deck.hasTrack()) return;
            if (has && isRelative (args[0]))                            // "loop +1" / "loop -1": bigger / smaller
            {
                fire ("loop_resize", (float) size);
                return;
            }
            if (deck.isLoopActive() && (! has || std::abs (deck.getLoopBeats() - size) < 0.01)) deck.exitLoop();
            else deck.setAutoLoop (has && size > 0.0 ? size : deck.getAutoLoopSize());
        });
    }
    if (name == "loop_select" || name == "loop_length")
    {
        if (asQuery) return false;
        if (ctx.input == Input::turn) { fire ("loop_resize", ctx.value); return true; }
        bool has = false;
        const double size = arg (0, &has);
        return onPress ([&]
        {
            if (! has) return;
            if (isRelative (args[0])) { fire ("loop_resize", (float) size); return; }
            if (name == "loop_length" && deck.isLoopActive()) deck.scaleLoop (size / juce::jmax (1.0 / 32.0, deck.getLoopBeats()));
            else deck.setAutoLoopSize (juce::jlimit (1.0 / 32.0, 32.0, size));
        });
    }
    if (name == "reloop_exit")
    {
        if (asQuery) return deck.isLoopActive();
        return onPress ([&] { if (deck.isLoopActive()) deck.exitLoop(); else deck.reloop(); });
    }
    if (name == "loop_roll")
    {
        const double size = args.isEmpty() ? 0.25 : beats (0.25);
        if (asQuery) return deck.isRolling() && std::abs (deck.getRollBeats() - size) < 1e-6;
        deck.loopRoll (size, isButton ? pressed : true);
        return true;
    }
    if (name == "loop_move")
    {
        if (asQuery) return false;
        const double amount = args.isEmpty() ? (ctx.input == Input::turn ? (ctx.value < 0 ? -1.0 : 1.0) * deck.getLoopBeats() : deck.getLoopBeats())
                                             : beats (1.0) * (ctx.input == Input::turn && ctx.value < 0 ? -1.0 : 1.0);
        return onPress ([&] { deck.loopMove (amount); });
    }
    if (name == "loop_pad")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return actions.isActive ("pad_loop_" + juce::String (s + 1), d);
        return press ("pad_loop_" + juce::String (s + 1));
    }
    if (name == "saved_loop" || name == "saved_loop_prepare" || name == "loop_load" || name == "loop_load_prepare"
        || name == "loop_save" || name == "loop_delete")
    {
        const int s = slot();
        if (s < 0) return false;
        const auto [a, b] = deck.getSavedLoop (s);
        if (asQuery) return b > a;
        return onPress ([&]
        {
            if (name == "loop_save") deck.saveLoop (s);
            else if (name == "loop_delete") deck.clearSavedLoop (s);
            else if (b > a) deck.triggerSavedLoop (s);
            else if (name == "saved_loop" || name == "saved_loop_prepare") deck.saveLoop (s);
        });
    }

    // Beat jump -------------------------------------------------------------------------
    if (name == "beatjump")
    {
        if (asQuery) return false;
        double amount = beats (deck.getAutoLoopSize());
        if (ctx.input == Input::turn) amount = std::abs (amount) * (ctx.value < 0 ? -1.0 : 1.0);
        return onPress ([&] { deck.beatJump (amount); });
    }
    if (name == "beatjump_select")
    {
        if (asQuery) return false;
        if (ctx.input == Input::turn) { fire ("loop_resize", ctx.value); return true; }
        return onPress ([&] { if (! args.isEmpty()) deck.setAutoLoopSize (juce::jlimit (1.0 / 32.0, 32.0, isRelative (args[0]) ? deck.getAutoLoopSize() * (arg (0) > 0 ? 2.0 : 0.5) : arg (0))); });
    }
    if (name == "beatjump_pad")
    {
        const int s = slot();
        if (s < 0) return false;
        if (asQuery) return false;
        return press ("pad_jump_" + juce::String (s + 1));
    }

    // Key and pitch ------------------------------------------------------------------------
    if (name == "key" || name == "key_move" || name == "key_smooth" || name == "set_key")
    {
        if (asQuery) return deck.getKeyShift() != 0;
        if (ctx.input == Input::turn && args.isEmpty()) { deck.setKeyShift (deck.getKeyShift() + (ctx.value > 0 ? 1 : -1)); return true; }
        if (args.isEmpty()) return false;
        const int n = (int) std::lround (arg (0));
        return onPress ([&]
        {
            if (name == "key_move" || isRelative (args[0])) deck.setKeyShift (deck.getKeyShift() + n);
            else if (name != "set_key") deck.setKeyShift (n);
        });
    }
    if (name == "match_key" || name == "key_sync" || name == "key_match") { if (asQuery) return false; return press ("key_match"); }
    if (name == "pitch_bend")
    {
        if (asQuery) return false;
        const bool down = args.size() > 0 && args[0].trim().startsWithChar ('-');
        if (ctx.input == Input::turn) { fire ("jog", ctx.value); return true; }
        fire (down ? "pitch_bend_minus" : "pitch_bend_plus", isButton ? (pressed ? 1.0f : 0.0f) : 1.0f);
        return true;
    }
    if (name == "pitch_relative")
    {
        if (asQuery) return false;
        const float cur = actions.currentValue ("pitch", d);
        fire ("pitch", juce::jlimit (0.0f, 1.0f, cur + (ctx.input == Input::turn ? ctx.value : (float) arg (0))));
        return true;
    }
    if (name == "pitch_range") { if (asQuery) return false; return press ("pitch_range"); }
    if (name == "get_pitch_zero") return std::abs (deck.getPitch()) < (args.isEmpty() ? 0.0005 : arg (0) / 100.0);
    if (name == "is_sync" || name == "sync_hint") return deck.isSyncOn();
    if (name == "gain_relative")
    {
        if (asQuery) return false;
        fire ("gain", juce::jlimit (0.0f, 1.0f, actions.currentValue ("gain", d) + (ctx.input == Input::turn ? ctx.value : (float) arg (0))));
        return true;
    }

    // Mixer ---------------------------------------------------------------------------------
    if (name == "pfl" || name == "headphones" || name == "listen")
    {
        if (asQuery) return engine.channel (d).pfl.load();
        return press ("pfl");
    }
    if (name == "eq_kill_high" || name == "eq_kill_mid" || name == "eq_kill_med" || name == "eq_kill_low" || name == "eq_reset")
    {
        auto& ch = engine.channel (d);
        auto& band = name == "eq_kill_high" ? ch.eqHigh : name == "eq_kill_low" ? ch.eqLow : ch.eqMid;
        const int idx = name == "eq_kill_high" ? 0 : name == "eq_kill_low" ? 2 : 1;
        auto& saved = killed[d];
        if (asQuery) return name != "eq_reset" && band.load() <= 0.001f;
        return onPress ([&]
        {
            if (name == "eq_reset") { ch.eqHigh = 0.5f; ch.eqMid = 0.5f; ch.eqLow = 0.5f; return; }
            if (band.load() > 0.001f) { saved[(size_t) idx] = band.load(); band = 0.0f; }
            else band = saved[(size_t) idx] > 0.001f ? saved[(size_t) idx] : 0.5f;
        });
    }

    // Effects ---------------------------------------------------------------------------------
    if (name == "effect_active" || name == "effect_activate" || name == "effect_slider" || name == "effect_slider_slider"
        || name == "effect_select" || name == "effect_select_multi" || name == "effect_button" || name == "effect_beats")
    {
        bool has = false;
        int unit = (int) arg (0, &has);
        if (! has || unit < 1 || unit > AudioEngine::numDeckFx) unit = 1;
        const auto n = juce::String (unit);
        if (name == "effect_active" || name == "effect_activate")
        {
            if (asQuery) return actions.isActive ("effect_active_" + n, d);
            const auto want = args.size() > 1 ? args[args.size() - 1].unquoted().toLowerCase() : juce::String();
            return onPress ([&]
            {
                const bool on = actions.isActive ("effect_active_" + n, d);
                if ((want == "on" && on) || (want == "off" && ! on)) return;
                fire ("effect_active_" + n, 1.0f);
            });
        }
        if (name == "effect_slider" || name == "effect_slider_slider")
        {
            if (asQuery) return false;
            const int which = args.size() > 1 ? (int) arg (1) : 1;
            const float v = args.size() > 2 ? (float) arg (2) : ctx.value;
            fire ((which >= 2 ? "effect_param_" : "effect_wet_") + n, juce::jlimit (0.0f, 1.0f, v));
            return true;
        }
        if (name == "effect_beats")
        {
            if (asQuery) return false;
            const bool up = ctx.input == Input::turn ? ctx.value > 0 : (args.isEmpty() || ! args[args.size() - 1].trim().startsWithChar ('-'));
            return onPress ([&] { fire (up ? "effect_beat_up" : "effect_beat_down", 1.0f); });
        }
        if (asQuery) return false;
        return press ("effect_select_" + n);
    }

    // Pioneer-style PLAY / CUE (their lights blink like a club player's) --------------------
    if (name == "pioneer_play")
    {
        if (asQuery)
        {
            if (deck.isPlaying()) return true;
            if (! deck.hasTrack()) return false;
            hint.level = actions.blinkPhase (d) ? Actions::Led::on : Actions::Led::off;
            return true;
        }
        return press ("play_pause");
    }
    if (name == "pioneer_cue")
    {
        if (asQuery)
        {
            const auto l = actions.ledLevel ("cue_button", d);
            if (l == Actions::Led::blink) hint.level = actions.blinkPhase (d) ? Actions::Led::on : Actions::Led::off;
            return l != Actions::Led::off;
        }
        return momentary ("cue_button");
    }

    // Pad pages ("pad_pages 2") and the pads of the page chosen -----------------------------
    if (name == "pad_pages" || (name == "pad_page" && ! args.isEmpty()))
    {
        bool has = false;
        const int page = (int) arg (0, &has);
        if (! has) return false;                                         // a page by name ('INP FX'…): not in OpenMix
        if (asQuery) return vdjPage[(size_t) d] == page;
        return onPress ([&]
        {
            vdjPage[(size_t) d] = page;
            static const std::map<int, const char*> pages { { 1, "pad_mode_hot_cue" }, { 2, "pad_mode_loop" }, { 3, "pad_mode_saved_loop" },
                                                            { 4, "pad_mode_sampler" }, { 5, "pad_mode_key" }, { 6, "pad_mode_roll" },
                                                            { 7, "pad_mode_fx" }, { 8, "pad_mode_jump" }, { 10, "pad_mode_roll" } };
            if (auto it = pages.find (page); it != pages.end()) fire (it->second, 1.0f);
        });
    }
    if (name == "pad" || name == "padshift")
    {
        const int s = slot();
        if (s < 0) return false;
        const auto id = vdjPage[(size_t) d] > 0 ? padAction (d, s, name == "padshift") : juce::String ("pad_" + juce::String (s + 1));
        if (id.isEmpty()) return false;
        if (asQuery) return actions.isActive (id, d);
        const bool held = id.startsWith ("hot_cue_") || id.startsWith ("pad_roll_") || id.startsWith ("pad_") && ! id.startsWith ("pad_loop_") && ! id.startsWith ("pad_jump");
        if (held) fire (id, isButton ? (pressed ? 1.0f : 0.0f) : 1.0f);
        else onPress ([&] { fire (id, 1.0f); });
        return true;
    }
    if (name == "pad_button_color" || name == "pad_color" || name == "padshift_button_color")
    {
        const int s = slot();
        if (s < 0) return false;
        const auto id = vdjPage[(size_t) d] > 0 ? padAction (d, s, name == "padshift_button_color") : juce::String ("pad_" + juce::String (s + 1));
        if (id.isEmpty()) return false;
        const auto level = actions.ledLevel (id, d);
        hint.level = level == Actions::Led::blink ? (actions.blinkPhase (d) ? Actions::Led::on : Actions::Led::dim) : level;
        hint.colour = actions.ledColour (id, d);
        if (hint.colour.isTransparent() && deck.getHotCue (s) >= 0.0) hint.colour = actions.ledColour ("hot_cue_" + juce::String (s + 1), d);
        return level != Actions::Led::off;
    }

    // Mixer FX buttons that choose what the filter knob does -----------------------------------
    if (name == "filter_activate")
    {
        const auto fx = args.isEmpty() ? juce::String() : args[0].unquoted().toLowerCase();
        const juce::String id = fx.contains ("echo") ? "color_fx_echo" : fx.contains ("noise") ? "color_fx_noise"
                              : fx.contains ("pitch") || fx.contains ("sweep") ? "color_fx_sweep" : fx.contains ("space") || fx.contains ("reverb") ? "color_fx_space"
                              : fx.contains ("crush") ? "color_fx_crush" : "color_fx_filter";
        if (asQuery) return actions.isActive (id, d) && id != "color_fx_filter";
        return press (id);
    }
    if (name == "crossfader_curve")
    {
        if (asQuery) return false;
        const auto c = args.isEmpty() ? juce::String() : args[0].unquoted().toLowerCase();
        return onPress ([&] { engine.crossfaderCurve = c.contains ("scratch") || c.contains ("cut") ? 1 : c.contains ("power") ? 2 : 0; });
    }
    if (name == "crossfader_disable" || name == "loop_adjust" || name == "goto_last_folder" || name == "sideview"
        || name == "browser_geniusdj" || name == "setting" || name == "is_using")
        return false;                                                    // nothing like it in OpenMix (yet)

    // Browser zones ------------------------------------------------------------------------------
    if (name == "browser_window")
    {
        const auto zone = args.isEmpty() ? juce::String() : args[0].unquoted().toLowerCase();
        const bool folders = zone.contains ("folder");
        if (! folders && ! zone.contains ("song")) return false;        // sideview…: not in OpenMix
        if (asQuery) return actions.isActive (folders ? "browser_show_folders" : "browser_show_songs", d);
        return press (folders ? "browser_show_folders" : "browser_show_songs");
    }
    if (name == "browser_folder" || name == "browser_enter")
    {
        if (asQuery) return false;
        if (name == "browser_enter" && ! actions.isActive ("browser_show_folders", d)) return press ("load");
        return press (actions.isActive ("browser_show_folders", d) ? "browser_show_songs" : "browser_show_folders");
    }
    if (name == "unload")          { if (asQuery) return ! deck.hasTrack(); return press ("unload"); }
    if (name == "pad_param" || name == "pad_param2")                       // the pads' size: smaller / bigger
    {
        if (asQuery) return false;
        const bool up = ctx.input == Input::turn ? ctx.value > 0 : (args.isEmpty() || ! args[0].trim().startsWithChar ('-'));
        return press (up ? "loop_double" : "loop_half");
    }
    if (name == "get_slip_active") return deck.getSlipPosition() >= 0.0;

    // Pads -------------------------------------------------------------------------------------
    if (name == "pad_pushed") return isButton && pressed;

    if ((name == "touchwheel" || name == "jogwheel" || name == "jog") && ! args.isEmpty() && args[0].unquoted().containsIgnoreCase ("search"))
    {
        if (asQuery) return false;
        if (! isButton) fire ("jog_search", ctx.value);
        return true;
    }
    if (name == "touchwheel_touch" && ! args.isEmpty() && args[0].unquoted().containsIgnoreCase ("search"))
        return true;                                                     // searching needs no hold

    // Plain verbs: one OpenMix function each ----------------------------------------------
    auto it = verbs().find (name);
    if (it == verbs().end())
        return false;
    const juce::String id = it->second.id;
    if (asQuery) return actions.isActive (id, d);

    switch (it->second.kind)
    {
        case Kind::press:
            return press (id);
        case Kind::momentary:
            return momentary (id);
        case Kind::slider:
        {
            bool has = false;
            const double v = arg (0, &has);
            if (has)                                            // "volume 50%", "crossfader 0.5", "pitch +0.1"
            {
                if (isButton && ! pressed) return true;
                fire (id, (float) juce::jlimit (0.0, 1.0, isRelative (args[0]) ? actions.currentValue (id, d) + v : v));
            }
            else if (! isButton)
                fire (id, juce::jlimit (0.0f, 1.0f, ctx.input == Input::turn ? actions.currentValue (id, d) + ctx.value : ctx.value));
            return true;
        }
        case Kind::turn:
        {
            bool has = false;
            const double v = arg (0, &has);
            if (isButton) { if (pressed) fire (id, (float) (has ? v : 1.0)); }
            else fire (id, has ? (float) (std::abs (v) * (ctx.value < 0 ? -1.0 : 1.0)) : ctx.value);
            return true;
        }
    }
    return false;
}

/** The OpenMix function of pad `pad` (0–7) in the pad page chosen with "pad_pages N" — VirtualDJ's
    usual pages: 1 HOT CUE, 2 LOOP, 3 SLICER, 4 SAMPLER, 5 KEY, 6 / 10 ROLL, 7 FX, 8 BEAT JUMP. */
juce::String VdjScript::padAction (int deck, int pad, bool shifted) const
{
    const auto n = juce::String (pad + 1);
    switch (vdjPage[(size_t) juce::jlimit (0, AudioEngine::numDecks - 1, deck)])
    {
        case 1:  return (shifted ? "delete_hot_cue_" : "hot_cue_") + n;
        case 2:  return shifted ? "saved_loop_" + n : "pad_loop_" + n;
        case 3:  return "saved_loop_" + n;
        case 5:
        {
            static const char* keys[] = { "key_tone_down", "key_down", "key_up", "key_tone_up", "key_reset", "pitch_range", "pitch_range", "key_match" };
            return keys[pad & 7];
        }
        case 6: case 10: return "pad_roll_" + n;
        case 7:
        {
            static const char* fx[]      = { "effect_active_1", "effect_active_2", "effect_active_3", "reverse", "censor", "brake", "backspin", "slip_mode" };
            static const char* fxShift[] = { "effect_select_1", "effect_select_2", "effect_select_3", "reverse", "censor", "brake", "backspin", "slip_mode" };
            return (shifted ? fxShift : fx)[pad & 7];
        }
        case 8:  return (shifted ? "pad_jump_long_" : "pad_jump_") + n;
        default: return {};                                              // sampler, or a page OpenMix does not have
    }
}

juce::String VdjScript::mainAction (const juce::String& script, int)
{
    // The first command that names a function.
    std::function<juce::String (const Node&)> first = [&first] (const Node& n) -> juce::String
    {
        if (n.kind == Node::Kind::seq) { for (auto& i : n.items) if (auto s = first (*i); s.isNotEmpty()) return s; return {}; }
        if (n.kind == Node::Kind::cond)
        {
            if (n.test) if (auto s = first (*n.test); s.isNotEmpty()) return s;
            if (n.otherwise) if (auto s = first (*n.otherwise); s.isNotEmpty()) return s;
            return n.then ? first (*n.then) : juce::String();
        }
        const int s = n.args.isEmpty() ? 0 : (int) number (n.args[0]);
        if ((n.verb == "hot_cue" || n.verb == "hotcue" || n.verb == "set_cue" || n.verb == "goto_cue" || n.verb == "delete_cue"
             || n.verb == "has_cue" || n.verb == "cue_color" || n.verb == "cue") && s >= 1 && s <= 8) return "hot_cue_" + juce::String (s);
        if ((n.verb == "saved_loop" || n.verb == "loop_load") && s >= 1 && s <= 8) return "saved_loop_" + juce::String (s);
        if (n.verb == "loop_pad" && s >= 1 && s <= 8) return "pad_loop_" + juce::String (s);
        if (n.verb == "loop" || n.verb == "loop_roll" || n.verb == "reloop_exit" || n.verb == "loop_button") return "loop";
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
        else if (verbs().count (n.verb) == 0 && specialVerbs().count (n.verb) == 0) out.addIfNotAlreadyThere (n.verb);
    };
    if (auto n = p.seq (false, false)) walk (*n);
    return out;
}

bool VdjScript::isContinuous (const juce::String& script)
{
    bool any = false;
    Parser p { tokenise (script) };
    static const std::set<juce::String> continuousSpecial { "effect_slider", "effect_slider_slider", "song_pos", "pitch_relative",
                                                            "gain_relative", "loop_select", "beatjump_select" };
    std::function<void (const Node&)> walk = [&] (const Node& n)
    {
        if (n.kind == Node::Kind::seq) for (auto& i : n.items) walk (*i);
        else if (n.kind == Node::Kind::cond) { if (n.then) walk (*n.then); if (n.otherwise) walk (*n.otherwise); }
        else if (auto it = verbs().find (n.verb); it != verbs().end() && n.args.isEmpty() && continuousIds().count (it->second.id) > 0) any = true;
        else if (continuousSpecial.count (n.verb) > 0 && (n.args.isEmpty() || n.verb.startsWith ("effect_slider"))) any = true;
    };
    if (auto n = p.seq (false, false)) walk (*n);
    return any;
}

} // namespace om
