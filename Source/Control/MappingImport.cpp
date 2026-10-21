#include "MappingImport.h"
#include <regex>
#include "ControllerLibrary.h"
#include <set>

namespace om::MappingImport
{

#if JUCE_MAC
static constexpr bool onMac = true;
#else
static constexpr bool onMac = false;
#endif

//==============================================================================
// Where installed DJ software keeps controller mappings. Found by folder shape, not by product:
//   · "MidiMappings" folders of CSV tables inside installed applications;
//   · a "Devices" + "Mappers" folder pair (XML definitions + mappers) in the user's documents or
//     application-data folders.

static void addCsvFolder (const juce::File& folder, std::vector<Source>& out)
{
    for (const auto& e : juce::RangedDirectoryIterator (folder, false, "*.csv", juce::File::findFiles))
    {
        const auto f = e.getFile();
        juce::String name;
        juce::FileInputStream in (f);
        if (in.openedOk())
        {
            const auto first = in.readNextLine();                       // "@file,1,<model>"
            if (first.startsWith ("@file"))
                name = juce::StringArray::fromTokens (first, ",", "\"")[2].trim();
        }
        if (name.isEmpty())
            name = f.getFileName().upToFirstOccurrenceOf (".", false, false);
        out.push_back ({ name, f, {} });
    }
}

static void addXmlPair (const juce::File& base, std::vector<Source>& out)
{
    const auto devices = base.getChildFile ("Devices"), mappers = base.getChildFile ("Mappers");
    if (! devices.isDirectory() || ! mappers.isDirectory())
        return;

    for (const auto& e : juce::RangedDirectoryIterator (devices, false, "*.xml", juce::File::findFiles))
    {
        auto xml = juce::XmlDocument::parse (e.getFile());
        if (xml == nullptr || ! xml->hasTagName ("device")) continue;
        const auto id = xml->getStringAttribute ("name");
        auto name = xml->getStringAttribute (onMac ? "macdrivername" : "drivername");
        if (name.isEmpty()) name = xml->getStringAttribute ("drivername");
        if (name.isEmpty()) name = xml->getStringAttribute ("description", id);

        juce::File mapper;
        for (const auto& m : juce::RangedDirectoryIterator (mappers, false, "*.xml", juce::File::findFiles))
            if (auto mx = juce::XmlDocument::parse (m.getFile()); mx != nullptr && mx->getStringAttribute ("device") == id)
                mapper = m.getFile();
        out.push_back ({ name, mapper != juce::File() ? mapper : e.getFile(), e.getFile() });
    }

    // Mappers of controllers the program supports natively (their definition is built in).
    for (const auto& m : juce::RangedDirectoryIterator (mappers, false, "*.xml", juce::File::findFiles))
    {
        auto mx = juce::XmlDocument::parse (m.getFile());
        if (mx == nullptr || ! mx->hasTagName ("mapper")) continue;
        const auto id = mx->getStringAttribute ("device");
        if (std::any_of (out.begin(), out.end(), [&m] (const Source& s) { return s.file == m.getFile(); })) continue;
        if (ControllerLibrary::findBundled (id))
            out.push_back ({ id, m.getFile(), {} });
    }
}

std::vector<Source> findInstalled()
{
    std::vector<Source> out;
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

    // CSV tables shipped inside installed applications.
    juce::Array<juce::File> appRoots;
   #if JUCE_MAC
    appRoots.add (juce::File ("/Applications"));
   #elif JUCE_WINDOWS
    appRoots.add (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory));
    appRoots.add (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectoryX86));
   #endif
    // Only where such folders live (a full recursive scan of the applications folder takes ages):
    // <app>/Contents/Resources/MidiMappings (macOS), <vendor>/<product>/MidiMappings (Windows).
    auto tryFolder = [&out] (const juce::File& f) { if (f.isDirectory()) addCsvFolder (f, out); };
    for (auto& root : appRoots)
        for (const auto& e : juce::RangedDirectoryIterator (root, false, "*", juce::File::findDirectories))
        {
            const auto d = e.getFile();
           #if JUCE_MAC
            if (d.hasFileExtension (".app"))
                tryFolder (d.getChildFile ("Contents/Resources/MidiMappings"));
            else
                for (const auto& inner : juce::RangedDirectoryIterator (d, false, "*.app", juce::File::findDirectories))
                    tryFolder (inner.getFile().getChildFile ("Contents/Resources/MidiMappings"));
           #else
            tryFolder (d.getChildFile ("MidiMappings"));
            for (const auto& inner : juce::RangedDirectoryIterator (d, false, "*", juce::File::findDirectories))
                tryFolder (inner.getFile().getChildFile ("MidiMappings"));
           #endif
        }

    // Definition + mapper folders of the user.
    juce::Array<juce::File> parents { docs, appData, appData.getChildFile ("Application Support"),
                                      home.getChildFile ("AppData").getChildFile ("Local") };
    for (auto& p : parents)
        for (const auto& e : juce::RangedDirectoryIterator (p, false, "*", juce::File::findDirectories))
            addXmlPair (e.getFile(), out);

    std::sort (out.begin(), out.end(), [] (const Source& a, const Source& b) { return a.controller.compareNatural (b.controller) < 0; });
    return out;
}

std::optional<Source> findFor (const juce::String& midiDeviceName)
{
    for (auto& s : findInstalled())
        if (ControllerLibrary::nameMatches (midiDeviceName, s.controller))
            return s;
    return std::nullopt;
}

//==============================================================================
// Function names → OpenMix actions.

struct Target { const char* action; int deck = -2; };     // deck −2: from the table's deck columns

static const std::map<juce::String, juce::String>& csvFunctions()
{
    static const std::map<juce::String, juce::String> m {
        { "playpause", "play_pause" }, { "cue", "cue_button" }, { "sync", "sync" }, { "mastertempo", "key_lock" },
        { "slip", "slip_mode" }, { "vinyl", "vinyl_mode" }, { "load", "load" }, { "headphonecue", "pfl" },
        { "channelfader", "volume" }, { "crossfader", "crossfader" }, { "gain", "gain" }, { "trim", "gain" },
        { "eqhigh", "eq_high" }, { "eqmid", "eq_mid" }, { "eqlow", "eq_low" }, { "browse", "browser_scroll" },
        { "shift", "shift" }, { "temposlider", "pitch" }, { "temporange", "pitch_range" }, { "jogtouch", "jog_touch" },
        { "jogscratch", "jog" }, { "jogpitchbend", "jog" }, { "wheelpitchbend", "jog" }, { "autoloop", "loop" },
        { "4beatloop", "loop" }, { "loopin", "loop_in" }, { "loopout", "loop_out" }, { "loophalf", "loop_half" },
        { "loopdouble", "loop_double" }, { "reloopexit", "reloop" }, { "masterlevel", "master_volume" },
        { "headphonelevel", "headphone_volume" }, { "headphonemix", "headphone_mix" }, { "reverse", "reverse" },
    };
    return m;
}

static juce::String csvAction (const juce::String& function, bool shift, int& fixedDeck)
{
    fixedDeck = -2;
    const auto key = function.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_");
    if (auto it = csvFunctions().find (key); it != csvFunctions().end())
        return it->second;

    // PADn_HotCue → hot cue n (with SHIFT: delete it, as on club gear).
    if (key.startsWith ("pad") && key.contains ("_hotcue"))
    {
        const int n = key.substring (3).getIntValue();
        if (n >= 1 && n <= 8) return (shift ? "delete_hot_cue_" : "hot_cue_") + juce::String (n);
    }
    // Colour FX knob of a channel: OpenMix's channel filter.
    if (key.startsWith ("cfxparameterch"))
    {
        const int ch = key.getTrailingIntValue();
        if (ch == 1 || ch == 2) { fixedDeck = ch - 1; return "filter"; }
    }
    return {};
}

static MidiBinding::Encoding csvEncoding (const juce::String& type, bool& ok, int& lsb, int number)
{
    ok = true;
    lsb = -1;
    const auto t = type.toLowerCase();
    if (t == "button" || t == "pad" || t == "jogtouch")        return MidiBinding::Encoding::button;
    if (t == "knobsliderhires")                                { lsb = number + 0x20; return MidiBinding::Encoding::absolute14; }
    if (t == "knobslider" || t == "slider" || t == "rotaryasknob") return MidiBinding::Encoding::absolute7;
    if (t == "jogrotate" || t == "difference")                 return MidiBinding::Encoding::relativeOffset;
    if (t == "rotary")                                         return MidiBinding::Encoding::relativeTwos;
    ok = false;                                                // outputs (indicators, values…)
    return MidiBinding::Encoding::button;
}

static int hex (const juce::String& s) { return s.trim().getHexValue32(); }

/** Effect controls of a vendor table. Two kinds of controllers:
      · one effect section (FX1 only: select, channel assign, beat, depth, on) → OpenMix's BEAT FX
      · one effect unit per side (FX1 = left deck, FX2 = right deck, 1–3 slots each) → the deck effects */
static juce::String csvFxAction (const juce::String& raw, bool perDeck, int& fixedDeck)
{
    static const std::regex re (R"(^fx([12])(?:-([123]))?(on|select|selectback|beatdown|beatup|assign\.(ch1|ch2|master))?$)",
                                std::regex::icase);
    std::smatch m;
    const auto str = raw.trim().toLowerCase().toStdString();
    if (! std::regex_match (str, m, re)) return {};
    const int unit = std::stoi (m[1].str());
    const int slot = m[2].matched ? std::stoi (m[2].str()) : 0;
    const auto what = m[3].str();
    if (perDeck)
    {
        fixedDeck = unit - 1;
        if (what == "beatdown") return "effect_beat_down";
        if (what == "beatup")   return "effect_beat_up";
        if (slot == 0) return {};
        const auto n = juce::String (slot);
        if (what.empty())       return "effect_wet_" + n;
        if (what == "on")       return "effect_active_" + n;
        if (what == "select")   return "effect_select_" + n;
        return {};
    }
    if (unit != 1) return {};
    fixedDeck = 0;
    if (what.empty() && slot == 1) return "beat_fx_level";
    if (what == "on" && slot == 1) return "beat_fx_on";
    if (what == "select")     return "beat_fx_select";
    if (what == "selectback") return "beat_fx_select_back";
    if (what == "beatdown")   return "beat_fx_beat_down";
    if (what == "beatup")     return "beat_fx_beat_up";
    if (m[4].matched)
    {
        const auto ch = m[4].str();
        if (ch == "master") return "beat_fx_master";
        fixedDeck = ch == "ch1" ? 0 : 1;
        return "beat_fx_channel";
    }
    return {};
}

Result fromCsv (const juce::String& text)
{
    Result r;
    bool anyOutput = false;      // a table without any output column: lights answer on the input message
    const bool fxPerDeck = text.contains ("FX2-");                    // an effect unit on each side
    juce::StringArray lines;
    lines.addLines (text);
    for (auto line : lines)
    {
        line = line.trim();
        if (line.startsWith ("@file"))
        {
            r.controller = juce::StringArray::fromTokens (line, ",", "\"")[2].trim();
            continue;
        }
        if (line.isEmpty() || line.startsWithChar ('#') || line.startsWithChar (','))
            continue;

        auto f = juce::StringArray::fromTokens (line, ",", "\"");
        while (f.size() < 13) f.add ({});
        const auto name = f[0].trim(), function = f[1].trim(), type = f[2].trim(), input = f[3].trim();
        if (type.isEmpty() || (function.isEmpty() && name.isEmpty()))
            continue;

        // The light of a control: column 8 is its output message (+ per-deck channel offsets in
        // 9–10), or 9–10 hold one full code per deck. −1: the control has no light.
        auto outCode = [&f] (int deck)
        {
            if (deck >= 2 && f[9 + deck].trim().isEmpty()) return -1;   // decks 3–4: only when the table names them
            const auto out = f[8].trim();
            if (out.isNotEmpty())
                return deck >= 0 && f[9 + deck].trim().isNotEmpty() ? hex (out) + (f[9 + deck].trim().getIntValue() << 8) : hex (out);
            return deck >= 0 && f[9 + deck].trim().isNotEmpty() ? hex (f[9 + deck]) : -1;
        };
        auto setLight = [] (MidiBinding& b, int code)
        {
            const int status = (code >> 8) & 0xFF;
            b.outChannel = (status & 0x0F) + 1;
            b.outKind = (status & 0xF0) == 0xB0 ? MidiBinding::Kind::cc : MidiBinding::Kind::note;
            b.outNumber = code & 0x7F;
        };

        anyOutput |= f[8].trim().isNotEmpty() || f[9].trim().isNotEmpty() || f[10].trim().isNotEmpty();

        if (type == "Indicator")       // lights without a control
        {
            const juce::String light = name == "ChannelLevel" ? "vu_meter" : name == "LoadedIndicator" ? "load"
                                     : name == "GetLoopActive" ? "loop" : name == "EndWarningIndicator" ? "end_warning" : "";
            if (light.isEmpty())
                continue;
            for (int d = 0; d < 4; ++d)
            {
                const int code = outCode (d);
                if (code < 0 || (((code >> 8) & 0xF0) != 0x90 && ((code >> 8) & 0xF0) != 0xB0))
                    continue;
                MidiBinding b;
                setLight (b, code);
                b.channel = b.outChannel; b.kind = b.outKind; b.number = b.outNumber;
                b.outChannel = 0; b.outNumber = -1;
                b.action = light;
                b.deck = d;
                b.outputOnly = true;
                b.encoding = light == "vu_meter" ? MidiBinding::Encoding::absolute7 : MidiBinding::Encoding::button;
                r.bindings.push_back (b);
            }
            continue;
        }
        const auto functionOrName = function.isNotEmpty() ? function : name;   // some tables leave "function" blank

        auto parts = juce::StringArray::fromTokens (functionOrName, "+", "");
        const bool isShiftButton = parts.size() == 1 && parts[0] == "Shift";
        const bool shift = parts.contains ("Shift") && ! isShiftButton;     // "X+Shift": X with SHIFT held
        if (! isShiftButton) parts.removeString ("Shift");
        if (parts.size() > 1)                                  // "Browse+Press" and other gestures: not mapped
        {
            r.untranslated.addIfNotAlreadyThere (functionOrName);
            continue;
        }

        int fixedDeck = -2;
        auto action = csvFxAction (name, fxPerDeck, fixedDeck);          // column 1 says what the control does
        if (action.isEmpty()) action = csvFxAction (parts[0], fxPerDeck, fixedDeck);
        if (action.isEmpty()) action = csvAction (parts[0], shift, fixedDeck);
        if (action.isEmpty()) action = csvAction (name, shift, fixedDeck);   // the control's own label
        bool encOk = false;
        int lsbDummy = -1;
        csvEncoding (type, encOk, lsbDummy, 0);
        if (action.isEmpty() || ! encOk)
        {
            if (encOk) r.untranslated.addIfNotAlreadyThere (functionOrName);
            continue;
        }

        // The message: one code + per-deck channel offsets, or one full code per deck.
        std::vector<std::pair<int, int>> codes;                 // (deck, status<<8 | data1)
        if (input.isNotEmpty())
        {
            const int code = hex (input);
            bool anyDeck = false;
            for (int d = 0; d < 4; ++d)
                if (f[4 + d].trim().isNotEmpty())
                {
                    anyDeck = true;
                    codes.push_back ({ d, code + (f[4 + d].trim().getIntValue() << 8) });
                }
            if (! anyDeck) codes.push_back ({ -1, code });
        }
        else
        {
            for (int d = 0; d < 4; ++d)
                if (f[4 + d].trim().isNotEmpty())
                    codes.push_back ({ d, hex (f[4 + d]) });
        }

        for (auto [deck, code] : codes)
        {
            const int status = (code >> 8) & 0xFF, number = code & 0x7F;
            MidiBinding b;
            b.channel = (status & 0x0F) + 1;
            const int kind = status & 0xF0;
            if (kind == 0x90 || kind == 0x80) b.kind = MidiBinding::Kind::note;
            else if (kind == 0xB0)            b.kind = MidiBinding::Kind::cc;
            else if (kind == 0xE0)            b.kind = MidiBinding::Kind::pitchWheel;
            else continue;
            b.number = number;
            int lsb = -1;
            b.encoding = csvEncoding (type, encOk, lsb, number);
            b.lsbNumber = lsb;
            if (b.kind == MidiBinding::Kind::note && b.encoding != MidiBinding::Encoding::button)
                continue;
            b.action = action;
            b.shift = shift && ! action.startsWith ("delete_hot_cue_");   // shifted pads send their own notes
            b.deck = fixedDeck >= 0 ? fixedDeck : deck;
            if (action == "jog") b.scale = 1.0f / 512.0f;
            if (b.encoding == MidiBinding::Encoding::button)
            {
                const int oc = outCode (deck);
                if (oc < 0)          b.ledOut = false;
                else if (oc != code) setLight (b, oc);
            }
            r.bindings.push_back (b);
        }
    }

    // "+Shift" rows: most controllers already send a different message while their SHIFT is held
    // (some have no SHIFT message at all). Only a message shared with the unshifted control needs
    // OpenMix's own SHIFT layer.
    std::set<juce::String> plain;
    auto key = [] (const MidiBinding& b) { return juce::String ((int) b.kind) + ":" + juce::String (b.channel) + ":" + juce::String (b.number); };
    for (auto& b : r.bindings) if (! b.shift) plain.insert (key (b));
    for (auto& b : r.bindings)
        if (b.shift && plain.count (key (b)) == 0)
            b.shift = false;
    if (! anyOutput)
        for (auto& b : r.bindings) b.ledOut = true;
    return r;
}

//==============================================================================
// XML definition + mapper.

static int num (const juce::XmlElement& e, const char* attr, int fallback = -1)
{
    if (! e.hasAttribute (attr)) return fallback;
    const auto s = e.getStringAttribute (attr).trim();
    return s.startsWithIgnoreCase ("0x") ? s.substring (2).getHexValue32() : s.getIntValue();
}

static std::pair<juce::String, int> scriptAction (juce::String script, bool& shiftLayer)
{
    // "deck 2 play_pause", "shift ? reverse : play_pause", "play_pause & …" → (action, deck or −1).
    shiftLayer = false;
    script = script.trim();
    int deck = -1;
    auto words = juce::StringArray::fromTokens (script.upToFirstOccurrenceOf ("&", false, false), " ", "\"'");
    words.removeEmptyStrings();
    if (words.size() >= 2 && words[0] == "deck")
    {
        const auto d = words[1].toLowerCase();
        deck = d == "left" ? 0 : d == "right" ? 1 : d.getIntValue() - 1;
        words.removeRange (0, 2);
    }
    if (words.isEmpty()) return { {}, deck };

    static const std::map<juce::String, juce::String> verbs {
        { "play_pause", "play_pause" }, { "play", "play_pause" }, { "cue_button", "cue_button" }, { "cue_stop", "goto_cue_stop" },
        { "sync", "sync" }, { "loop", "loop" }, { "auto_loop", "loop" }, { "loop_half", "loop_half" }, { "loop_double", "loop_double" },
        { "loop_in", "loop_in" }, { "loop_out", "loop_out" }, { "loop_exit", "loop_exit" }, { "reloop_exit", "reloop" },
        { "reloop", "reloop" }, { "jogwheel", "jog" }, { "jog_wheel", "jog" }, { "touchwheel", "jog_touch" },
        { "pitch_slider", "pitch" }, { "pitch", "pitch" }, { "pitch_reset", "pitch_reset" }, { "key_lock", "key_lock" },
        { "keylock", "key_lock" }, { "master_tempo", "key_lock" }, { "match_key", "key_match" }, { "slip_mode", "slip_mode" },
        { "slip", "slip_mode" }, { "reverse", "reverse" }, { "brake", "brake" }, { "censor", "censor" }, { "vinyl_mode", "vinyl_mode" },
        { "load", "load" }, { "volume", "volume" }, { "level", "volume" }, { "gain", "gain" }, { "eq_high", "eq_high" },
        { "eq_mid", "eq_mid" }, { "eq_low", "eq_low" }, { "filter", "filter" }, { "pfl", "pfl" }, { "crossfader", "crossfader" },
        { "master_volume", "master_volume" }, { "headphone_volume", "headphone_volume" }, { "headphone_mix", "headphone_mix" },
        { "browser_scroll", "browser_scroll" }, { "shift", "shift" }, { "record", "record" }, { "quantize", "quantize" },
        { "pause", "play_pause" }, { "play_stutter", "stutter" }, { "stutter", "stutter" }, { "goto_start", "start_play" },
        { "cue", "cue_button" }, { "jogwheel_touch", "jog_touch" }, { "touch_wheel", "jog_touch" }, { "scratch_wheel_touch", "jog_touch" },
        { "scratch_wheel", "jog" }, { "nudge_wheel", "jog" }, { "jog_search", "jog_search" }, { "seek", "jog_search" },
        { "pitch_range", "pitch_range" }, { "pitch_zero", "pitch_reset" }, { "key_match", "key_match" }, { "key_sync", "key_match" },
        { "key_reset", "key_reset" }, { "eq_hi", "eq_high" }, { "eq_bass", "eq_low" }, { "trim", "gain" },
        { "filter_selectcolorfx", "filter" }, { "color_fx", "filter" }, { "headphones", "pfl" }, { "listen", "pfl" },
        { "headphone_gain", "headphone_volume" }, { "master_level", "master_volume" }, { "crossfader_curve", "" },
        { "automix", "automix" }, { "loop_move", "loop_move_fwd" }, { "loop_roll", "loop_roll_3" }, { "reloop", "reloop" },
        { "loop_button", "loop" }, { "loop_select", "loop_resize" }, { "loop_halve", "loop_half" }, { "instant_doubles", "instant_doubles" },
        { "clone_deck", "instant_doubles" }, { "master_deck", "master" }, { "backspin", "backspin" }, { "pad_page", "pad_page" },
        { "load_track", "load" }, { "unload", "" }, { "browser_enter", "" }, { "browser_switch", "browser_focus" }, { "dump", "brake" },
    };

    const auto verb = words[0].toLowerCase();
    if (verb == "hot_cue" && words.size() >= 2)
    {
        const int n = words[1].getIntValue();
        if (n >= 1 && n <= 8) return { "hot_cue_" + juce::String (n), deck };
    }
    if (verb == "delete_cue" && words.size() >= 2)
    {
        const int n = words[1].getIntValue();
        if (n >= 1 && n <= 8) return { "delete_hot_cue_" + juce::String (n), deck };
    }
    if (verb == "pitch_bend" && words.size() >= 2)
        return { words[1].startsWithChar ('-') ? "pitch_bend_minus" : "pitch_bend_plus", deck };
    if ((verb == "set_cue" || verb == "goto_cue") && words.size() >= 2)
    {
        const int n = words[1].getIntValue();
        if (n >= 1 && n <= 8) return { "hot_cue_" + juce::String (n), deck };
    }
    if (verb == "pad" && words.size() >= 2)                    // "pad 3": the pad of the current page
    {
        const int n = words[1].getIntValue();
        if (n >= 1 && n <= 8) return { "pad_" + juce::String (n), deck };
    }
    if ((verb == "effect_active" || verb == "effect_slider" || verb == "effect_select") && words.size() >= 2)
    {
        const int n = words[1].getIntValue();
        if (n >= 1 && n <= 3) return { (verb == "effect_active" ? "effect_active_" : verb == "effect_slider" ? "effect_wet_" : "effect_select_") + juce::String (n), deck };
        return { verb == "effect_active" ? "effect_active_1" : verb == "effect_slider" ? "effect_wet_1" : "effect_select_1", deck };
    }
    // "loop 4" / "loop 1/2": a loop of that size; "loop_roll 0.25": a roll of that size.
    auto beats = [] (const juce::String& w) -> double
    {
        if (w.containsChar ('/')) { const double den = w.fromFirstOccurrenceOf ("/", false, false).getDoubleValue(); return den > 0.0 ? w.upToFirstOccurrenceOf ("/", false, false).getDoubleValue() / den : 0.0; }
        return w.retainCharacters ("0123456789.").getDoubleValue();
    };
    if ((verb == "loop" || verb == "auto_loop" || verb == "loop_roll") && words.size() >= 2 && beats (words[1]) > 0.0)
    {
        const double size = beats (words[1]);
        const auto* sizes = verb == "loop_roll" ? Actions::padRollSizes : Actions::padLoopSizes;
        int best = 0;
        for (int i = 1; i < 8; ++i) if (std::abs (std::log2 (sizes[i] / size)) < std::abs (std::log2 (sizes[best] / size))) best = i;
        if (verb != "loop_roll" && std::abs (size - 4.0) < 1e-6) return { "loop_4", deck };
        return { (verb == "loop_roll" ? "pad_roll_" : "pad_loop_") + juce::String (best + 1), deck };
    }
    if ((verb == "beatjump" || verb == "goto") && words.size() >= 2)
        return { words[1].startsWithChar ('-') ? "beatjump_back" : "beatjump_fwd", deck };
    if (verb == "key" && words.size() >= 2)
    {
        const auto w = words[1];
        if (w.startsWithChar ('-')) return { w.contains ("2") ? "key_tone_down" : "key_down", deck };
        if (w.startsWithChar ('+')) return { w.contains ("2") ? "key_tone_up" : "key_up", deck };
        if (w == "0" || w == "0%") return { "key_reset", deck };
    }
    if (auto it = verbs.find (verb); it != verbs.end())
        return { it->second, deck };
    return { {}, deck };
}

Result fromXml (const juce::String& definitionXml, const juce::String& mapperXml)
{
    Result r;
    auto def = juce::XmlDocument::parse (definitionXml);
    if (def == nullptr || ! def->hasTagName ("device"))
        return r;
    r.controller = def->getStringAttribute (onMac ? "macdrivername" : "drivername");
    if (r.controller.isEmpty()) r.controller = def->getStringAttribute ("drivername", def->getStringAttribute ("description"));

    // Control name → the MIDI messages that carry it.
    std::multimap<juce::String, MidiBinding> controls;
    for (auto* e : def->getChildIterator())
    {
        const auto tag = e->getTagName();
        const auto name = e->getStringAttribute ("name");
        if (name.isEmpty()) continue;
        MidiBinding b;
        b.channel = juce::jlimit (1, 16, num (*e, "channel", 0) + 1);
        b.deck = num (*e, "deck", 0) - 1;                       // −1: not tied to a deck
        b.invert = e->getBoolAttribute ("inverted");
        const int note = num (*e, "note"), cc = num (*e, "cc"), msb = num (*e, "ccmsb");
        if (tag == "button" || tag == "toggle")
        {
            b.encoding = MidiBinding::Encoding::button;
            if (note >= 0) { b.kind = MidiBinding::Kind::note; b.number = note; }
            else if (cc >= 0) { b.kind = MidiBinding::Kind::cc; b.number = cc; }
            else continue;
            const int nb = juce::jlimit (1, 4, num (*e, "nbdecks", 1));
            for (int i = 0; i < nb; ++i)
            {
                auto c = b;
                c.channel = juce::jlimit (1, 16, b.channel + i);
                if (nb > 1) c.deck = i;
                controls.insert ({ name, c });
            }
            continue;
        }
        if (tag == "slider")
        {
            if (e->getBoolAttribute ("pitch")) { b.kind = MidiBinding::Kind::pitchWheel; b.encoding = MidiBinding::Encoding::absolute7; }
            else if (cc >= 0 && msb >= 0)      { b.kind = MidiBinding::Kind::cc; b.number = msb; b.lsbNumber = cc; b.encoding = MidiBinding::Encoding::absolute14; }
            else if (cc >= 0)                  { b.kind = MidiBinding::Kind::cc; b.number = cc; b.encoding = MidiBinding::Encoding::absolute7; }
            else continue;
        }
        else if (tag == "encoder" || tag == "jog")
        {
            if (cc < 0) continue;
            b.kind = MidiBinding::Kind::cc;
            b.number = cc;
            const int zero = num (*e, "zero", 0);
            b.encoding = zero == 0x40 ? MidiBinding::Encoding::relativeOffset : MidiBinding::Encoding::relativeTwos;
            if (tag == "jog") b.scale = 1.0f / (float) juce::jmax (1, num (*e, "full", 128));
        }
        else continue;                                          // LEDs, displays, SysEx
        controls.insert ({ name, b });
    }

    // With a mapper: each control runs its VirtualDJ script word for word (VdjScript), so it does
    // exactly what it did there. The SHIFT button stays OpenMix's SHIFT (scripts ask "shift ? …").
    if (auto mapper = juce::XmlDocument::parse (mapperXml); mapper != nullptr && mapper->hasTagName ("mapper"))
    {
        for (auto* m : mapper->getChildWithTagNameIterator ("map"))
        {
            const auto key = m->getStringAttribute ("value");
            const auto script = m->getStringAttribute ("action").trim();
            if (script.isEmpty() || controls.count (key) == 0) continue;
            for (auto& v : VdjScript::unknownVerbs (script)) r.untranslated.addIfNotAlreadyThere (v);
            for (auto [it, end] = controls.equal_range (key); it != end; ++it)
            {
                auto b = it->second;
                if (b.deck > 3) continue;
                if (b.deck < 0) b.deck = 0;
                if (script.equalsIgnoreCase ("shift")) b.action = "shift";
                else { b.action = "vdj"; b.script = script; }
                r.bindings.push_back (b);
            }
        }
        return r;
    }

    // Without a mapper, the control names themselves are tried.
    std::vector<std::pair<juce::String, juce::String>> maps;
    for (auto& [name, b] : controls)
        maps.push_back ({ name, name.toLowerCase() });

    for (auto& [value, script] : maps)
    {
        auto key = value;
        bool shiftHeld = false;
        if (key.startsWithIgnoreCase ("SHIFT+")) { shiftHeld = true; key = key.substring (6); }

        // "shift ? A : B": B normally, A with SHIFT held.
        juce::StringArray variants { script }, layers { shiftHeld ? "1" : "0" };
        if (script.contains ("?") && script.trimStart().startsWithIgnoreCase ("shift"))
        {
            const auto rest = script.fromFirstOccurrenceOf ("?", false, false);
            variants = { rest.upToFirstOccurrenceOf (":", false, false), rest.fromFirstOccurrenceOf (":", false, false) };
            layers = { "1", "0" };
        }

        bool any = false;
        for (int v = 0; v < variants.size(); ++v)
        {
            bool unused = false;
            auto [action, deck] = scriptAction (variants[v], unused);
            if (action.isEmpty()) continue;
            for (auto [it, end] = controls.equal_range (key); it != end; ++it)
            {
                auto b = it->second;
                b.action = action;
                b.shift = layers[v] == "1";
                if (deck >= 0) b.deck = deck;
                if (b.deck > 3) continue;
                if (b.deck < 0) b.deck = 0;
                if (action == "jog" && b.encoding == MidiBinding::Encoding::button) b.action = "jog_touch";
                r.bindings.push_back (b);
                any = true;
            }
        }
        if (! any && controls.count (key) > 0)
            r.untranslated.addIfNotAlreadyThere (value);
    }
    return r;
}

//==============================================================================
// A mapper of a controller the other program supports natively: its definition (which MIDI message
// each control name stands for) is built into that program and never written to disk. The messages
// come from OpenMix's own mapping for that model; each control name of the mapper is matched to the
// control of that mapping that does the same job ("PLAY" → the binding of play_pause…), and gets the
// user's action. Controls the mapper does not mention keep OpenMix's mapping.

/** Which OpenMix action a control name usually stands for ("PLAY", "PAD3", "HOTCUE_5", "LOOP_IN"…). */
static juce::String roleOf (const juce::String& controlName, bool& shifted)
{
    // Only exact, well-known control names. A name is never guessed from a part of it: "PLAYLIST" is
    // not PLAY, "MASTER_VOLUME" is not a channel fader. Unknown names keep OpenMix's own mapping.
    // "SHIFT_CUE", "PLAY_SHIFT": that control with SHIFT held; only "SHIFT" itself is the SHIFT button.
    auto k = controlName.toUpperCase().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    shifted = false;
    if (k != "SHIFT" && (k.startsWith ("SHIFT") || k.endsWith ("SHIFT")))
    {
        shifted = true;
        k = k.startsWith ("SHIFT") ? k.substring (5) : k.dropLastCharacters (5);
    }
    static const std::map<juce::String, juce::String> exact {
        { "SHIFT", "shift" },
        { "PLAY", "play_pause" }, { "PLAYPAUSE", "play_pause" }, { "CUE", "cue_button" }, { "SYNC", "sync" },
        { "LOAD", "load" }, { "PFL", "pfl" }, { "HEADPHONES", "pfl" }, { "HEADPHONE", "pfl" }, { "LISTEN", "pfl" },
        { "VOLUME", "volume" }, { "LEVEL", "volume" }, { "FADER", "volume" }, { "CHANNELFADER", "volume" }, { "VOLUMESLIDER", "volume" },
        { "EQHIGH", "eq_high" }, { "EQHI", "eq_high" }, { "HIGH", "eq_high" }, { "TREBLE", "eq_high" },
        { "EQMID", "eq_mid" }, { "MID", "eq_mid" }, { "EQLOW", "eq_low" }, { "LOW", "eq_low" }, { "BASS", "eq_low" },
        { "GAIN", "gain" }, { "TRIM", "gain" }, { "FILTER", "filter" },
        { "PITCH", "pitch" }, { "PITCHSLIDER", "pitch" }, { "PITCHFADER", "pitch" }, { "TEMPO", "pitch" },
        { "JOG", "jog" }, { "JOGWHEEL", "jog" }, { "WHEEL", "jog" }, { "PLATTER", "jog" },
        { "JOGTOUCH", "jog_touch" }, { "TOUCH", "jog_touch" }, { "WHEELTOUCH", "jog_touch" }, { "TOUCHWHEEL", "jog_touch" },
        { "CROSSFADER", "crossfader" }, { "XFADER", "crossfader" },
        { "LOOPIN", "loop_in" }, { "LOOPOUT", "loop_out" }, { "LOOP", "loop" }, { "LOOPPUSH", "loop" }, { "LOOPBUTTON", "loop" },
        { "LOOPKNOBPUSH", "loop" }, { "LOOPENCODERPUSH", "loop" },
        { "VINYL", "vinyl_mode" }, { "SLIP", "slip_mode" }, { "Q", "quantize" }, { "QUANTIZE", "quantize" },
        { "KEYLOCK", "key_lock" }, { "MASTERTEMPO", "key_lock" },
        { "BROWSE", "browser_scroll" }, { "BROWSER", "browser_scroll" }, { "BROWSERKNOB", "browser_scroll" },
        { "BROWSEPUSH", "browser_focus" }, { "BROWSEBUTTON", "browser_focus" }, { "BROWSERPUSH", "browser_focus" }, { "BROWSERBUTTON", "browser_focus" },
    };
    if (auto it = exact.find (k); it != exact.end()) return it->second;

    // Pads: "PAD3", "HOTCUE3", "LOOP3", "ROLL3"…
    const int n = k.getTrailingIntValue();
    if (n >= 1 && n <= 8)
    {
        const auto word = k.dropLastCharacters (juce::String (n).length());
        const auto pad = juce::String (n);
        if (word == "PAD" || word == "PADS" || word == "HOTCUE" || word == "CUE" || word == "HOT") return "hot_cue_" + pad;
        if (word == "LOOP" || word == "LOOPPAD")                                                return "pad_loop_" + pad;
        if (word == "ROLL" || word == "LOOPROLL")                                               return "pad_roll_" + pad;
        if (word == "SLICER" || word == "SAVEDLOOP")                                            return "saved_loop_" + pad;
        if (word == "JUMP" || word == "BEATJUMP")                                               return "pad_jump_" + pad;
    }
    return {};
}

Result fromMapperOnly (const juce::String& mapperXml, const juce::String& fallbackName)
{
    Result r;
    auto mapper = juce::XmlDocument::parse (mapperXml);
    if (mapper == nullptr || ! mapper->hasTagName ("mapper"))
        return r;
    r.controller = mapper->getStringAttribute ("device", fallbackName);
    auto base = ControllerLibrary::findBundled (r.controller);
    if (! base && fallbackName.isNotEmpty()) base = ControllerLibrary::findBundled (fallbackName);
    if (! base)
    {
        r.missingDefinition = true;
        return r;
    }
    r.basedOn = base->first;

    const auto& from = base->second;
    std::vector<MidiBinding> out = from;
    auto same = [] (const MidiBinding& a, const MidiBinding& b) { return a.kind == b.kind && a.number == b.number && a.channel == b.channel && a.deck == b.deck; };
    for (auto* m : mapper->getChildWithTagNameIterator ("map"))
    {
        const auto key = m->getStringAttribute ("value");
        const auto script = m->getStringAttribute ("action").trim();
        bool shiftKey = false;
        auto role = roleOf (key, shiftKey);
        if (role.isEmpty() || script.isEmpty() || (role == "shift" && shiftKey)) { r.untranslated.addIfNotAlreadyThere (key); continue; }
        if (role == "shift") continue;                          // SHIFT stays OpenMix's SHIFT
        // SHIFT + pad: pads send their own note with SHIFT, which is OpenMix's "delete hot cue" binding.
        if (shiftKey && role.startsWith ("hot_cue_")) { role = "delete_hot_cue_" + role.fromLastOccurrenceOf ("_", false, false); shiftKey = false; }

        // The controls of OpenMix's mapping that do this job. A "SHIFT_…" key is the message that
        // control sends with SHIFT held: on controllers that send it on their own channel (Hercules:
        // channel + 3) that is the binding with the same number there.
        std::vector<size_t> targets;
        for (size_t i = 0; i < from.size(); ++i)
        {
            const auto& b = from[i];
            if (b.action != role || b.outputOnly || b.shift) continue;
            if (! shiftKey) { targets.push_back (i); continue; }
            for (size_t j = 0; j < from.size(); ++j)
                if (! from[j].outputOnly && from[j].kind == b.kind && from[j].number == b.number && from[j].deck == b.deck
                    && from[j].channel == b.channel + 3)
                    targets.push_back (j);
        }

        bool any = false;
        for (auto i : targets)
        {
            // A script must fit the control: a turning / sliding function on a jog or fader, a button
            // function on a button (else that control would just stop working).
            const bool continuous = from[i].encoding != MidiBinding::Encoding::button;
            if (continuous != VdjScript::isContinuous (script)) continue;
            for (auto& o : out)
                if (same (o, from[i]) && ! o.outputOnly && o.action != "shift")
                {
                    o.action = "vdj";
                    o.script = script;
                }
            ++r.changed;
            any = true;
        }
        if (! any) r.untranslated.addIfNotAlreadyThere (key);
        else for (auto& v : VdjScript::unknownVerbs (script)) r.untranslated.addIfNotAlreadyThere (v);
    }
    r.bindings = std::move (out);
    return r;
}

//==============================================================================
// XML "controller preset": one <control> per MIDI message with a group ([Channel1], [Master]…), the
// function it drives, and options. Many controls go through the program's own scripts; those are
// recognised by the function name in the script path (deck[0].playButton, wheelTurn…).

static juce::String presetAction (const juce::String& group, const juce::String& key, bool script)
{
    const auto g = group.toLowerCase(), k = key.toLowerCase();
    if (! script)
    {
        static const std::map<juce::String, juce::String> direct {
            { "play", "play_pause" }, { "cue_default", "cue_button" }, { "cue_gotoandstop", "goto_cue_stop" },
            { "sync_enabled", "sync" }, { "beatsync", "sync" }, { "keylock", "key_lock" }, { "slip_enabled", "slip_mode" },
            { "reverse", "reverse" }, { "pfl", "pfl" }, { "volume", "volume" }, { "pregain", "gain" }, { "rate", "pitch" },
            { "loadselectedtrack", "load" }, { "beatloop_activate", "loop" }, { "beatloop_4_toggle", "loop" },
            { "loop_halve", "loop_half" }, { "loop_double", "loop_double" }, { "loop_in", "loop_in" }, { "loop_out", "loop_out" },
            { "reloop_toggle", "reloop" }, { "reloop_exit", "reloop" }, { "beatjump_forward", "beatjump_fwd" },
            { "beatjump_backward", "beatjump_back" }, { "crossfader", "crossfader" }, { "headgain", "headphone_volume" },
            { "headmix", "headphone_mix" }, { "quantize", "quantize" }, { "pitch_adjust_up", "key_up" }, { "pitch_adjust_down", "key_down" },
            { "jog", "jog" }, { "movevertical", "browser_scroll" }, { "selecttrackknob", "browser_scroll" },
            { "filterhigh", "eq_high" }, { "filtermid", "eq_mid" }, { "filterlow", "eq_low" },   // older EQ names
        };
        if (k.startsWith ("hotcue_") && k.endsWith ("_activate")) return "hot_cue_" + juce::String (k.substring (7).getIntValue());
        if (k.startsWith ("hotcue_") && k.endsWith ("_clear"))    return "delete_hot_cue_" + juce::String (k.substring (7).getIntValue());
        if (g.contains ("equalizerrack"))
            return k == "parameter3" ? "eq_high" : k == "parameter2" ? "eq_mid" : k == "parameter1" ? "eq_low" : juce::String();
        if (g.contains ("quickeffectrack") && k == "super1") return "filter";
        if ((g == "[master]" || g == "[mixer]") && k == "gain") return "master_volume";
        if (auto it = direct.find (k); it != direct.end()) return it->second;
        return {};
    }

    // Script: the path after the script's own name ("Model.leftDeck.playButton.input") names the
    // control; the script name itself (often the model) must not be read as a control.
    const auto path = k.containsChar ('.') ? k.fromFirstOccurrenceOf (".", false, false) : k;
    auto has = [&path] (const char* w) { return path.contains (w); };
    if (has ("indicator") || has ("toneplay") || has ("sampler") || has ("effect") || has ("slicer") || has ("roll")) return {};
    if (has ("shift"))                                           return "shift";
    if (has ("wheeltouch") || has ("jogtouch") || has ("touch")) return "jog_touch";
    if (has ("wheelturn") || has ("jogturn") || has ("jogwheel") || has ("jogspin") || (has ("wheel") && ! has ("pitch")) || has (".jog") || path.startsWith ("jog")) return "jog";
    if (has ("hotcue"))                                          return "hot_cue_*";
    if (has ("pads["))                                          // "pads[0]": numbered by the script itself
    {
        const int n = path.fromFirstOccurrenceOf ("pads[", false, false).getIntValue() + 1;
        if (n >= 1 && n <= 8) return "pad_" + juce::String (n);
    }
    if (has ("padpress") || has ("padbutton"))                   return "pad_*";
    if (has ("pitchbendup"))                                     return "pitch_bend_plus";
    if (has ("pitchbenddown"))                                   return "pitch_bend_minus";
    if (has ("loophalf"))                                        return "loop_half";
    if (has ("loopdouble"))                                      return "loop_double";
    if (has ("loopin"))                                          return "loop_in";
    if (has ("loopout"))                                         return "loop_out";
    if (has ("reloop"))                                          return "reloop";
    if (has ("loop"))                                            return "loop";
    if (has ("sync"))                                            return "sync";
    if (has ("play"))                                            return "play_pause";
    if (has ("cue") && ! has ("pfl"))                            return "cue_button";
    if (has ("pfl") || has ("headphone"))                        return "pfl";
    if (has ("load"))                                            return "load";
    if (has ("keylock"))                                         return "key_lock";
    if (has ("slip"))                                            return "slip_mode";
    if (has ("scratchtoggle") || has ("vinyl"))                  return "vinyl_mode";
    if (has ("maingain") || has ("mastergain") || has ("mastervol")) return "master_volume";
    if (has ("gain") || has ("trim"))                            return "gain";
    {
        // EQ knobs numbered low → high, as the equalizer's parameters 1–3 ("eqKnob[2]", "parameter2").
        const int n = has ("eqknob[") ? path.fromFirstOccurrenceOf ("eqknob[", false, false).getIntValue()
                    : (g.contains ("equalizerrack") && has ("parameter")) ? path.fromFirstOccurrenceOf ("parameter", false, false).getIntValue() : 0;
        if (n == 1) return "eq_low";
        if (n == 2) return "eq_mid";
        if (n == 3) return "eq_high";
    }
    if (has ("eqhi") || has ("high") || has ("treble"))          return "eq_high";
    if (has ("eqmid") || has (".mid"))                           return "eq_mid";
    if (has ("eqlo") || has ("low") || has ("bass"))             return "eq_low";
    if (has ("filter") || has ("quickeffect") || has ("super"))  return "filter";
    if (has ("pitch") || has ("rate") || has ("tempo"))          return "pitch";
    if (has ("volume") || has ("fader"))                         return g == "[master]" ? "crossfader" : "volume";
    if (has ("browse") || has ("scroll") || has ("library"))     return "browser_scroll";
    return {};
}

static int presetDeck (const juce::String& group, const juce::String& key, int status)
{
    const auto g = group.toLowerCase(), k = key.toLowerCase();
    for (int n = 1; n <= 4; ++n)
        if (g.contains ("[channel" + juce::String (n) + "]")) return n - 1;
    if (k.contains ("deck[0]") || k.contains ("decks[0]") || k.contains ("decka") || k.contains ("leftdeck") || k.contains ("deck1")) return 0;
    if (k.contains ("deck[1]") || k.contains ("decks[1]") || k.contains ("deckb") || k.contains ("rightdeck") || k.contains ("deck2")) return 1;
    const int ch = status & 0x0F;
    return ch <= 1 ? ch : -1;
}

Result fromPreset (const juce::String& xmlText)
{
    Result r;
    auto xml = juce::XmlDocument::parse (xmlText);
    if (xml == nullptr || ! xml->getTagName().endsWithIgnoreCase ("Preset"))
        return r;
    if (auto* info = xml->getChildByName ("info"))
        r.controller = info->getChildElementAllSubText ("name", {}).trim();
    auto* controller = xml->getChildByName ("controller");
    auto* controls = controller != nullptr ? controller->getChildByName ("controls") : nullptr;
    if (controls == nullptr) return r;

    struct Raw { juce::String group, key; int status, number; bool script, invert, msb, lsb, spread64, diff; };
    std::vector<Raw> raws;
    for (auto* c : controls->getChildWithTagNameIterator ("control"))
    {
        Raw raw;
        raw.group = c->getChildElementAllSubText ("group", {}).trim();
        raw.key = c->getChildElementAllSubText ("key", {}).trim();
        auto parseNum = [] (juce::String v) { v = v.trim(); return v.startsWithIgnoreCase ("0x") ? v.substring (2).getHexValue32() : v.getIntValue(); };
        raw.status = parseNum (c->getChildElementAllSubText ("status", {}));
        raw.number = parseNum (c->getChildElementAllSubText ("midino", {})) & 0x7F;
        raw.script = raw.invert = raw.msb = raw.lsb = raw.spread64 = raw.diff = false;
        if (auto* o = c->getChildByName ("options"))
            for (auto* opt : o->getChildIterator())
            {
                const auto t = opt->getTagName().toLowerCase();
                raw.script   |= t == "script-binding";
                raw.invert   |= t == "invert";
                raw.msb      |= t == "fourteen-bit-msb";
                raw.lsb      |= t == "fourteen-bit-lsb";
                raw.spread64 |= t == "spread64";
                raw.diff     |= t == "diff" || t == "selectknob";
            }
        // Script halves of a 14-bit control: "….inputMSB" / "….inputLSB".
        const auto lk = raw.key.toLowerCase();
        if (raw.script && lk.endsWith ("lsb")) raw.lsb = true;
        if (raw.script && lk.endsWith ("msb")) raw.msb = true;
        raws.push_back (raw);
    }
    auto baseKey = [] (const juce::String& k) { auto l = k.toLowerCase(); return l.endsWith ("msb") || l.endsWith ("lsb") ? l.dropLastCharacters (3) : l; };

    // Pads / hot cues behind a script: numbered by their note within each deck.
    std::map<juce::String, std::vector<int>> ranks;            // "deck|kind|status" → sorted numbers
    for (auto& raw : raws)
    {
        const auto a = presetAction (raw.group, raw.key, raw.script);
        if (a.endsWith ("_*"))
        {
            auto& v = ranks[juce::String (presetDeck (raw.group, raw.key, raw.status)) + "|" + a + "|" + juce::String (raw.status)];
            if (std::find (v.begin(), v.end(), raw.number) == v.end()) v.push_back (raw.number);
        }
    }
    for (auto& [k, v] : ranks) std::sort (v.begin(), v.end());

    std::set<juce::String> taken;                              // one function per message
    for (auto& raw : raws)
    {
        if (raw.lsb) continue;                                 // paired with its MSB below
        auto action = presetAction (raw.group, raw.key, raw.script);
        const int deck = presetDeck (raw.group, raw.key, raw.status);
        if (action.isEmpty() || deck < -1 || deck > 3)
        {
            if (raw.key.isNotEmpty()) r.untranslated.addIfNotAlreadyThere (raw.key);
            continue;
        }
        if (action.endsWith ("_*"))
        {
            const auto& v = ranks[juce::String (deck) + "|" + action + "|" + juce::String (raw.status)];
            const int n = (int) (std::find (v.begin(), v.end(), raw.number) - v.begin()) + 1;
            if (n < 1 || n > 8) continue;
            action = action.replace ("_*", "_" + juce::String (n));
        }

        MidiBinding b;
        const int kind = raw.status & 0xF0;
        b.channel = (raw.status & 0x0F) + 1;
        b.number = raw.number;
        b.invert = raw.invert;
        b.action = action;
        b.deck = deck < 0 ? 0 : deck;
        const bool relative = action == "jog" || action == "browser_scroll";
        const bool continuous = relative || action == "pitch" || action == "volume" || action == "gain" || action == "crossfader"
                             || action.startsWith ("eq_") || action == "filter" || action.endsWith ("_volume") || action == "headphone_mix";
        if (kind == 0x90 || kind == 0x80)
        {
            if (continuous) continue;                          // a note cannot drive a fader
            b.kind = MidiBinding::Kind::note;
            b.encoding = MidiBinding::Encoding::button;
        }
        else if (kind == 0xE0)
        {
            b.kind = MidiBinding::Kind::pitchWheel;
            b.number = 0;
            b.encoding = MidiBinding::Encoding::absolute7;
        }
        else if (kind == 0xB0)
        {
            b.kind = MidiBinding::Kind::cc;
            if (! continuous)
                b.encoding = MidiBinding::Encoding::button;
            else if (relative)
            {
                b.encoding = raw.spread64 ? MidiBinding::Encoding::relativeOffset : MidiBinding::Encoding::relativeTwos;
                if (action == "jog") b.scale = 1.0f / 512.0f;
            }
            else if (raw.msb)
            {
                b.encoding = MidiBinding::Encoding::absolute14;
                for (auto& o : raws)                           // its LSB: same function, other CC
                    if (o.lsb && baseKey (o.key) == baseKey (raw.key) && o.group == raw.group && (o.status & 0x0F) == (raw.status & 0x0F))
                        b.lsbNumber = o.number;
                if (b.lsbNumber < 0) b.encoding = MidiBinding::Encoding::absolute7;
            }
            else
                b.encoding = MidiBinding::Encoding::absolute7;
        }
        else continue;

        const auto key = juce::String ((int) b.kind) + ":" + juce::String (b.channel) + ":" + juce::String (b.number);
        if (! taken.insert (key).second) continue;
        r.bindings.push_back (b);
    }

    // Lights the preset lists explicitly (<outputs>): the message and on/off values of each one.
    if (auto* outputs = controller->getChildByName ("outputs"))
        for (auto* o : outputs->getChildWithTagNameIterator ("output"))
        {
            auto parseNum = [] (juce::String v) { v = v.trim(); return v.startsWithIgnoreCase ("0x") ? v.substring (2).getHexValue32() : v.getIntValue(); };
            const auto group = o->getChildElementAllSubText ("group", {}).trim();
            const auto k = o->getChildElementAllSubText ("key", {}).trim().toLowerCase();
            const int status = parseNum (o->getChildElementAllSubText ("status", {}));
            const int number = parseNum (o->getChildElementAllSubText ("midino", {})) & 0x7F;
            const auto onText = o->getChildElementAllSubText ("on", {}), offText = o->getChildElementAllSubText ("off", {});
            const int on = onText.isNotEmpty() ? juce::jlimit (0, 127, parseNum (onText)) : 127;
            const int off = offText.isNotEmpty() ? juce::jlimit (0, 127, parseNum (offText)) : 0;
            if (o->getChildByName ("minimum") != nullptr && parseNum (o->getChildElementAllSubText ("minimum", {})) > 0
                && (k.contains ("vu") || k.contains ("peak")))
                continue;                                     // one LED of a segmented meter: not supported

            juce::String action;
            if (k == "play_indicator" || k == "play")                      action = "play_pause";
            else if (k == "cue_indicator" || k == "cue_default")           action = "cue_button";
            else if (k == "sync_enabled" || k == "beatsync")               action = "sync";
            else if (k == "pfl")                                           action = "pfl";
            else if (k == "keylock")                                       action = "key_lock";
            else if (k == "slip_enabled")                                  action = "slip_mode";
            else if (k == "quantize")                                      action = "quantize";
            else if (k == "reverse")                                       action = "reverse";
            else if (k == "loop_enabled")                                  action = "loop";
            else if (k == "end_of_track")                                  action = "end_warning";
            else if (k == "track_loaded" || k == "track_samples")          action = "load";
            else if (k.startsWith ("hotcue_") && (k.endsWith ("_enabled") || k.endsWith ("_status")))
            {
                const int n = k.substring (7).getIntValue();
                if (n >= 1 && n <= 8) action = "hot_cue_" + juce::String (n);
            }
            const int deck = presetDeck (group, k, status);
            const int kind = status & 0xF0;
            if (action.isEmpty() || deck < -1 || deck > 3 || (kind != 0x90 && kind != 0xB0))
                continue;

            const auto lightKind = kind == 0xB0 ? MidiBinding::Kind::cc : MidiBinding::Kind::note;
            const int channel = (status & 0x0F) + 1;
            auto sameMessage = [&] (const MidiBinding& b) { return b.kind == lightKind && b.channel == channel && b.number == number; };

            // 1. A control on that very message keeps its own logic (a PAD follows its page).
            // 2. Else the control with that function gets its light on this message.
            // 3. Else a light of its own.
            bool done = false;
            for (auto& b : r.bindings)
                if (! b.outputOnly && b.outNumber < 0 && sameMessage (b) && ! b.shift)
                {
                    b.onValue = on; b.offValue = off; done = true;
                }
            for (auto& b : r.bindings)
                if (! done && ! b.outputOnly && b.action == action && b.deck == juce::jmax (0, deck) && ! b.shift && b.outNumber < 0)
                {
                    b.outChannel = channel; b.outKind = lightKind; b.outNumber = number;
                    b.onValue = on; b.offValue = off; done = true;
                }
            if (! done)
            {
                MidiBinding b;
                b.kind = lightKind; b.channel = channel; b.number = number;
                b.action = action; b.deck = juce::jmax (0, deck);
                b.onValue = on; b.offValue = off;
                b.outputOnly = true;
                r.bindings.push_back (b);
            }
        }
    return r;
}

//==============================================================================
Result convert (const Source& s)
{
    if (s.file.hasFileExtension (".csv"))
    {
        auto r = fromCsv (s.file.loadFileAsString());
        if (r.controller.isEmpty()) r.controller = s.controller;
        return r;
    }
    if (s.definition == juce::File() || s.definition == s.file)
        if (auto xml = juce::XmlDocument::parse (s.file); xml != nullptr && xml->getTagName().endsWithIgnoreCase ("Preset"))
        {
            auto r = fromPreset (s.file.loadFileAsString());
            if (r.controller.isEmpty()) r.controller = s.controller;
            return r;
        }
    if (s.definition == juce::File())
    {
        auto r = fromMapperOnly (s.file.loadFileAsString(), s.controller);
        if (r.controller.isEmpty()) r.controller = s.controller;
        return r;
    }
    const auto mapper = s.file != s.definition ? s.file.loadFileAsString() : juce::String();
    auto r = fromXml (s.definition.loadFileAsString(), mapper);
    if (r.controller.isEmpty()) r.controller = s.controller;
    return r;
}

Result fromFile (const juce::File& f)
{
    if (f.hasFileExtension (".csv"))
        return fromCsv (f.loadFileAsString());

    auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr) return {};
    if (xml->getTagName().endsWithIgnoreCase ("Preset"))
        return fromPreset (f.loadFileAsString());
    if (xml->hasTagName ("device"))
    {
        // A definition: its mapper, if any, sits in the sibling "Mappers" folder.
        const auto id = xml->getStringAttribute ("name");
        for (const auto& m : juce::RangedDirectoryIterator (f.getParentDirectory().getSiblingFile ("Mappers"), false, "*.xml", juce::File::findFiles))
            if (auto mx = juce::XmlDocument::parse (m.getFile()); mx != nullptr && mx->getStringAttribute ("device") == id)
                return fromXml (f.loadFileAsString(), m.getFile().loadFileAsString());
        return fromXml (f.loadFileAsString(), {});
    }
    if (xml->hasTagName ("mapper"))
    {
        const auto id = xml->getStringAttribute ("device");
        for (const auto& d : juce::RangedDirectoryIterator (f.getParentDirectory().getSiblingFile ("Devices"), false, "*.xml", juce::File::findFiles))
            if (auto dx = juce::XmlDocument::parse (d.getFile()); dx != nullptr && dx->getStringAttribute ("name") == id)
                return fromXml (d.getFile().loadFileAsString(), f.loadFileAsString());
        // Built-in controller of the other program: no definition on disk.
        return fromMapperOnly (f.loadFileAsString(), f.getFileNameWithoutExtension().upToFirstOccurrenceOf (" - ", false, false));
    }
    return {};
}

Result fromFiles (const juce::Array<juce::File>& files)
{
    // A definition and its mapper chosen together; otherwise the first file that reads.
    juce::File def, map;
    for (auto& f : files)
        if (auto x = juce::XmlDocument::parse (f))
        {
            if (x->hasTagName ("device")) def = f;
            else if (x->hasTagName ("mapper")) map = f;
        }
    if (def.existsAsFile() && map.existsAsFile())
        return fromXml (def.loadFileAsString(), map.loadFileAsString());
    Result last;
    for (auto& f : files)
    {
        auto r = fromFile (f);
        if (! r.bindings.empty()) return r;
        if (r.missingDefinition || last.controller.isEmpty()) last = r;
    }
    return last;
}

} // namespace om::MappingImport
