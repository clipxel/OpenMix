#include "ControllerLibrary.h"
#include "../Library/Library.h"
#include "../Common/Strings.h"

namespace om
{

static constexpr const char* defaultIndex = "https://clipxel.github.io/OpenMix/official/index.json";
static constexpr int refreshHours = 12;          // re-download the index at most this often

ControllerLibrary::ControllerLibrary (MidiEngine& m) : midi (m) {}

ControllerLibrary::~ControllerLibrary()
{
    alive->store (false);
    pool.removeAllJobs (true, 10000);
}

juce::URL ControllerLibrary::indexUrl()
{
    return ! indexOverride().isEmpty() ? indexOverride() : juce::URL (defaultIndex);
}

juce::File ControllerLibrary::cacheFolder()
{
    auto f = Library::dataFolder().getChildFile ("controllers");
    f.createDirectory();
    return f;
}

//==============================================================================
juce::String ControllerLibrary::normalise (const juce::String& midiName)
{
    // "2- DDJ X MIDI 1", "DDJ X MIDI", "DDJ X" → "ddjx": port numbers, the Windows "n- " prefix and
    // generic words differ between systems; the model name does not.
    auto s = midiName.toLowerCase().trim();
    if (s.length() > 3 && juce::CharacterFunctions::isDigit (s[0]) && s.substring (1, 3) == "- ")
        s = s.substring (3);
    else if (s.length() > 4 && s.substring (0, 2).containsOnly ("0123456789") && s.substring (2, 4) == "- ")
        s = s.substring (4);
    auto words = juce::StringArray::fromTokens (s, " _-:()[]/", "");
    words.removeEmptyStrings();
    for (auto* generic : { "midi", "port", "usb", "audio", "in", "out", "input", "output", "device" })
        words.removeString (generic);
    while (! words.isEmpty() && words[words.size() - 1].containsOnly ("0123456789"))
        words.remove (words.size() - 1);
    return words.joinIntoString ("").retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
}

bool ControllerLibrary::nameMatches (const juce::String& deviceName, const juce::String& pattern)
{
    // Exact model name; a trailing '*' allows a family ("Model *" also matches "Model MK2").
    const bool prefix = pattern.trimEnd().endsWithChar ('*');
    const auto d = normalise (deviceName), p = normalise (pattern.trimEnd().trimCharactersAtEnd ("*"));
    return p.isNotEmpty() && (prefix ? d.startsWith (p) : d == p);
}

std::vector<ControllerLibrary::Profile> ControllerLibrary::parseIndex (const juce::var& index)
{
    std::vector<Profile> out;
    if (auto* arr = index["mappings"].getArray())
        for (auto& m : *arr)
        {
            Profile p;
            p.id = m["id"].toString();
            p.name = m["name"].toString();
            p.file = m["file"].toString();
            p.author = m["author"].toString();
            p.description = m["description"].toString();
            if (auto* names = m["match"]["midi"].getArray()) for (auto& n : *names) p.midiNames.add (n.toString());
            if (auto* usb = m["match"]["usb"].getArray())    for (auto& u : *usb)   p.usbIds.add (u.toString().toLowerCase());
            const auto& d = m["driver"]["windows"];
            p.windowsDriver = { d["url"].toString(), d["sha256"].toString().toLowerCase(), d["args"].toString(), d["signer"].toString() };
            if (p.file.isNotEmpty() && ! p.file.contains ("..") && ! p.file.containsChar ('/') && ! p.midiNames.isEmpty())
                out.push_back (std::move (p));
        }
    return out;
}

//==============================================================================
static juce::String download (const juce::URL& url, juce::String& error)
{
    if (url.isLocalFile())
        return url.getLocalFile().loadFileAsString();

    int status = 0;
    auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                         .withConnectionTimeoutMs (8000)
                                         .withNumRedirectsToFollow (3)
                                         .withStatusCode (&status));
    if (in == nullptr || (status != 0 && status != 200))
    {
        error = in == nullptr ? juce::String ("Sin conexión") : "Error " + juce::String (status);
        return {};
    }
    return in->readEntireStreamAsString();
}

juce::var ControllerLibrary::loadIndex (bool allowNetwork, bool force, bool& downloaded, juce::String& error)
{
    const juce::ScopedLock sl (indexLock);
    downloaded = false;
    const auto cached = cacheFolder().getChildFile ("index.json");
    const bool fresh = cached.existsAsFile()
                    && juce::Time::getCurrentTime() - cached.getLastModificationTime() < juce::RelativeTime::hours (refreshHours);

    if (allowNetwork && (force || ! fresh))
    {
        const auto text = download (indexUrl(), error);
        const auto parsed = juce::JSON::parse (text);
        if (parsed.isObject() && parsed["mappings"].isArray())
        {
            cached.replaceWithText (text);
            downloaded = true;
            return parsed;
        }
        if (error.isEmpty()) error = "Índice de mapeos inválido"_u;
    }
    return juce::JSON::parse (cached);          // offline (or fresh): the last good copy
}

ControllerLibrary::Result ControllerLibrary::lookup (const juce::String& deviceName)
{
    juce::String error;
    bool downloaded = false;
    auto index = loadIndex (online.load(), false, downloaded, error);
    auto profiles = parseIndex (index);

    auto find = [&deviceName] (const std::vector<Profile>& ps) -> std::optional<Profile>
    {
        for (auto& p : ps)
            for (auto& n : p.midiNames)
                if (nameMatches (deviceName, n))
                    return p;
        return std::nullopt;
    };

    auto profile = find (profiles);
    if (! profile && online.load() && ! downloaded)
    {
        // The cached index may predate this controller: one fresh download before giving up.
        index = loadIndex (true, true, downloaded, error);
        profiles = parseIndex (index);
        profile = find (profiles);
    }
    if (! profile)
        return { index.isObject() ? Status::noMapping : Status::offline, {}, error, {} };

    // The mapping file sits next to the index; it is downloaded once and cached.
    const auto local = cacheFolder().getChildFile (profile->file);
    if (! local.existsAsFile())
    {
        juce::String err;
        const auto source = indexUrl();
        const auto fileUrl = source.isLocalFile() ? juce::URL (source.getLocalFile().getSiblingFile (profile->file))
                                                 : juce::URL (source.toString (false).upToLastOccurrenceOf ("/", true, false) + profile->file);
        const auto text = online.load() ? download (fileUrl, err) : juce::String();
        if (! juce::JSON::parse (text)["bindings"].isArray())
            return { Status::offline, profile->name, err.isNotEmpty() ? err : juce::String ("Mapeo no disponible"_u), {} };
        local.replaceWithText (text);
    }

    std::vector<MidiBinding> bindings;
    const auto parsed = juce::JSON::parse (local);          // keep it alive while iterating
    if (auto* arr = parsed["bindings"].getArray())
        for (auto& v : *arr)
            bindings.push_back (MidiBinding::fromVar (v));
    if (bindings.empty())
        return { Status::offline, profile->name, "Mapeo vacío"_u, {} };
    return { Status::mapped, profile->name, {}, std::move (bindings) };
}

// Which library mapping was installed for each device (so a reconnect says "listo", not "propio").
static juce::File installedFile() { return Library::dataFolder().getChildFile ("controllers").getChildFile ("installed.json"); }

static juce::String installedProfile (const juce::String& device)
{
    return juce::JSON::parse (installedFile())[device.toRawUTF8()].toString();
}

static void rememberInstalled (const juce::String& device, const juce::String& profileName)
{
    auto v = juce::JSON::parse (installedFile());
    if (! v.isObject()) v = juce::var (new juce::DynamicObject());
    v.getDynamicObject()->setProperty (device, profileName);
    installedFile().getParentDirectory().createDirectory();
    installedFile().replaceWithText (juce::JSON::toString (v));
}

void ControllerLibrary::install (const juce::String& deviceName, Result r)
{
    if (r.status == Status::mapped && ! r.bindings.empty())
    {
        if (! midi.hasBindingsFor (deviceName))                 // the user may have mapped it meanwhile
        {
            midi.installMapping (deviceName, r.bindings);
            rememberInstalled (deviceName, r.profileName);
        }
        else
        {
            r.status = Status::mappedByUser;
        }
    }

    for (auto& d : list)
        if (d.name == deviceName)
        {
            d.status = r.status;
            d.profileName = r.profileName;
            d.detail = r.detail;
        }
    sendChangeMessage();
}

ControllerLibrary::Device ControllerLibrary::resolveNow (const juce::String& deviceName)
{
    if (std::none_of (list.begin(), list.end(), [&] (const Device& d) { return d.name == deviceName; }))
    {
        Device d;
        d.name = deviceName;
        list.push_back (d);
    }
    if (midi.hasBindingsFor (deviceName))
    {
        const auto p = installedProfile (deviceName);
        install (deviceName, { p.isNotEmpty() ? Status::mapped : Status::mappedByUser, p, {}, {} });
    }
    else
        install (deviceName, lookup (deviceName));
    for (auto& d : list)
        if (d.name == deviceName) return d;
    return {};
}

//==============================================================================
void ControllerLibrary::tick()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastScan < 1000) return;
    lastScan = now;

    const auto connected = midi.connectedInputs();
    bool changed = false;

    // Unplugged devices leave the list (their bindings stay, ready for next time).
    const auto before = list.size();
    list.erase (std::remove_if (list.begin(), list.end(), [&connected] (const Device& d) { return ! connected.contains (d.name); }), list.end());
    changed = list.size() != before;

    for (auto& name : connected)
    {
        if (std::any_of (list.begin(), list.end(), [&name] (const Device& d) { return d.name == name; }))
            continue;
        Device fresh;
        fresh.name = name;
        list.push_back (fresh);
        changed = true;

        if (midi.hasBindingsFor (name))
        {
            list.back().profileName = installedProfile (name);
            list.back().status = list.back().profileName.isNotEmpty() ? Status::mapped : Status::mappedByUser;
            continue;
        }

        auto flag = alive;
        pool.addJob ([this, flag, name]
        {
            auto found = lookup (name);
            juce::MessageManager::callAsync ([this, flag, name, r = std::move (found)]() mutable
            {
                if (flag->load()) install (name, std::move (r));
            });
        });
    }

    if (changed) sendChangeMessage();
}

juce::String ControllerLibrary::summary() const
{
    if (list.empty()) return {};
    const auto& d = list.front();
    const juce::String more = list.size() > 1 ? " +" + juce::String ((int) list.size() - 1) : juce::String();
    switch (d.status)
    {
        case Status::searching:    return "Buscando mapeo…"_u + more;
        case Status::mapped:       return d.profileName + juce::String::fromUTF8 (" · listo") + more;
        case Status::mappedByUser: return d.name + juce::String::fromUTF8 (" · mapeo propio") + more;
        case Status::noMapping:    return d.name + juce::String::fromUTF8 (" · sin mapeo") + more;
        case Status::offline:      return d.name + juce::String::fromUTF8 (" · sin conexión") + more;
    }
    return {};
}

} // namespace om
