#include "MappingImport.h"
#include "ControllerLibrary.h"
#include <set>

namespace om::MappingImport
{

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
        auto name = xml->getStringAttribute (JUCE_MAC ? "macdrivername" : "drivername");
        if (name.isEmpty()) name = xml->getStringAttribute ("drivername");
        if (name.isEmpty()) name = xml->getStringAttribute ("description", id);

        juce::File mapper;
        for (const auto& m : juce::RangedDirectoryIterator (mappers, false, "*.xml", juce::File::findFiles))
            if (auto mx = juce::XmlDocument::parse (m.getFile()); mx != nullptr && mx->getStringAttribute ("device") == id)
                mapper = m.getFile();
        out.push_back ({ name, mapper != juce::File() ? mapper : e.getFile(), e.getFile() });
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

Result fromCsv (const juce::String& text)
{
    Result r;
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
        while (f.size() < 8) f.add ({});
        const auto name = f[0].trim(), function = f[1].trim(), type = f[2].trim(), input = f[3].trim();
        if (function.isEmpty() || type.isEmpty())
            continue;

        auto parts = juce::StringArray::fromTokens (function, "+", "");
        const bool isShiftButton = parts.size() == 1 && parts[0] == "Shift";
        const bool shift = parts.contains ("Shift") && ! isShiftButton;     // "X+Shift": X with SHIFT held
        if (! isShiftButton) parts.removeString ("Shift");
        if (parts.size() > 1)                                  // "Browse+Press" and other gestures: not mapped
        {
            r.untranslated.addIfNotAlreadyThere (function);
            continue;
        }

        int fixedDeck = -2;
        auto action = csvAction (parts[0], shift, fixedDeck);
        if (action.isEmpty()) action = csvAction (name, shift, fixedDeck);   // the control's own label
        bool encOk = false;
        int lsbDummy = -1;
        csvEncoding (type, encOk, lsbDummy, 0);
        if (action.isEmpty() || ! encOk)
        {
            if (encOk) r.untranslated.addIfNotAlreadyThere (function);
            continue;
        }

        // The message: one code + per-deck channel offsets, or one full code per deck.
        std::vector<std::pair<int, int>> codes;                 // (deck, status<<8 | data1)
        if (input.isNotEmpty())
        {
            const int code = hex (input);
            bool anyDeck = false;
            for (int d = 0; d < 2; ++d)
                if (f[4 + d].trim().isNotEmpty())
                {
                    anyDeck = true;
                    codes.push_back ({ d, code + (f[4 + d].trim().getIntValue() << 8) });
                }
            if (! anyDeck) codes.push_back ({ -1, code });
            if (f[6].trim().isNotEmpty() || f[7].trim().isNotEmpty())
                r.untranslated.addIfNotAlreadyThere (function + juce::String::fromUTF8 (" (decks 3–4)"));
        }
        else
        {
            for (int d = 0; d < 2; ++d)
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
    r.controller = def->getStringAttribute (JUCE_MAC ? "macdrivername" : "drivername");
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

    // Control name → action. Without a mapper, the control names themselves are tried.
    std::vector<std::pair<juce::String, juce::String>> maps;
    if (auto mapper = juce::XmlDocument::parse (mapperXml); mapper != nullptr && mapper->hasTagName ("mapper"))
        for (auto* m : mapper->getChildWithTagNameIterator ("map"))
            maps.push_back ({ m->getStringAttribute ("value"), m->getStringAttribute ("action") });
    else
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
                if (b.deck > 1) continue;                       // decks 3–4: not in OpenMix
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
Result convert (const Source& s)
{
    if (s.file.hasFileExtension (".csv"))
    {
        auto r = fromCsv (s.file.loadFileAsString());
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
    }
    return {};
}

} // namespace om::MappingImport
