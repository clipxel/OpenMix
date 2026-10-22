#include "ControllerLibrary.h"
#include "DriverCheck.h"
#include "MappingImport.h"
#include <BinaryData.h>
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
    // "2- Model X-400 MIDI 1", "Model X-400 MIDI", "Model X-400" → "modelx400". Port numbers, the
    // Windows "n- " prefix and generic words differ between systems; the model name (numbers
    // included: "-400" vs "-200") does not.
    auto s = midiName.toLowerCase().trim();
    if (s.length() > 3 && juce::CharacterFunctions::isDigit (s[0]) && s.substring (1, 3) == "- ")
        s = s.substring (3);
    else if (s.length() > 4 && s.substring (0, 2).containsOnly ("0123456789") && s.substring (2, 4) == "- ")
        s = s.substring (4);

    static const juce::StringArray generic { "midi", "port", "usb", "audio", "in", "out", "input", "output", "device" };
    auto words = juce::StringArray::fromTokens (s, " \t", "");
    words.removeEmptyStrings();
    juce::StringArray kept;
    for (int i = 0; i < words.size(); ++i)
    {
        const auto w = words[i].retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
        if (generic.contains (w)) continue;
        // A bare number right after a generic word is a port number ("MIDI 1"), not part of the model.
        if (w.containsOnly ("0123456789") && i > 0 && generic.contains (words[i - 1].retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789")))
            continue;
        kept.add (w);
    }
    return kept.joinIntoString ("");
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
           #if JUCE_WINDOWS
            const auto& d = m["driver"]["windows"];
           #elif JUCE_MAC
            const auto& d = m["driver"]["mac"];
           #else
            const auto& d = m["driver"]["linux"];
           #endif
            p.driver.page = d["page"].toString();
            if (auto* det = d["detect"].getArray()) for (auto& x : *det) p.driver.detect.add (x.toString());
            if (! p.driver.isValid())
                p.driver = {};
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
        error = in == nullptr ? juce::String::fromUTF8 ("Sin conexión") : "Error " + juce::String (status);
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

// The mappings that ship inside OpenMix (Resources/mappings/library.json): tested controllers that
// work at once, offline.
static const juce::var& bundledLibrary()
{
    static const juce::var lib = juce::JSON::parse (juce::String::fromUTF8 (BinaryData::library_json, BinaryData::library_jsonSize));
    return lib;
}

bool ControllerLibrary::hasHardwareMixer (const juce::String& profileName)
{
    if (auto* arr = bundledLibrary()["mappings"].getArray())
        for (auto& m : *arr)
            if (m["name"].toString() == profileName)
                return (bool) m["hardwareMixer"];
    return false;
}

std::optional<std::pair<juce::String, juce::var>> ControllerLibrary::findVdjTable (const juce::String& vdjDevice)
{
    const auto want = normalise (vdjDevice);
    if (want.isEmpty()) return std::nullopt;
    if (auto* arr = bundledLibrary()["mappings"].getArray())
        for (auto& m : *arr)
            if (auto* ids = m["vdj"]["device"].getArray())
                for (auto& id : *ids)
                    if (normalise (id.toString()) == want || normalise (m["name"].toString()) == want)
                        return std::pair { m["name"].toString(), m["vdj"]["keys"] };
    return std::nullopt;
}

bool ControllerLibrary::hasOwnSoundcard (const juce::String& profileName)
{
    if (auto* arr = bundledLibrary()["mappings"].getArray())
        for (auto& m : *arr)
            if (m["name"].toString() == profileName)
                return (bool) m["soundcard"];
    return false;
}

std::optional<std::pair<juce::String, std::vector<MidiBinding>>> ControllerLibrary::findBundled (const juce::String& deviceName)
{
    if (auto* arr = bundledLibrary()["mappings"].getArray())
    {
        // The most specific name wins ("Mixtrack Pro 3" before "Mixtrack 3").
        const juce::var* best = nullptr;
        int bestLen = 0;
        for (auto& m : *arr)
            if (auto* names = m["match"]["midi"].getArray())
                for (auto& n : *names)
                {
                    // Exact, or the device adds a brand in front ("BRAND MODEL-X" for "MODEL-X").
                    const auto pat = normalise (n.toString());
                    const bool ok = nameMatches (deviceName, n.toString())
                                 || (pat.length() >= 5 && normalise (deviceName).endsWith (pat));
                    if (ok && pat.length() > bestLen)
                    {
                        best = &m;
                        bestLen = pat.length();
                    }
                }
        if (best != nullptr)
        {
            std::vector<MidiBinding> bindings;
            if (auto* b = (*best)["bindings"].getArray())
                for (auto& v : *b) bindings.push_back (MidiBinding::fromVar (v));
            if (! bindings.empty())
                return std::pair { (*best)["name"].toString(), std::move (bindings) };
        }
    }
    return std::nullopt;
}

ControllerLibrary::Result ControllerLibrary::lookup (const juce::String& deviceName)
{
    if (auto found = findBundled (deviceName))
        return { Status::mapped, found->first, {}, std::move (found->second) };

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
    {
        // Not in the OpenMix library: maybe DJ software installed on this computer knows it.
        if (localImport.load())
            if (auto src = MappingImport::findFor (deviceName))
            {
                auto imported = MappingImport::convert (*src);
                if (! imported.bindings.empty())
                    return { Status::mapped, src->controller + juce::String::fromUTF8 (" (importado de otro programa)"), {}, std::move (imported.bindings) };
            }
        return { index.isObject() ? Status::noMapping : Status::offline, {}, error, {} };
    }

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

// installed.json: device → { "profile": name, "hash": fingerprint of the bindings as installed }
// (older versions stored just the name).
static juce::var installedEntry (const juce::String& device)
{
    return juce::JSON::parse (installedFile())[device.toRawUTF8()];
}

static juce::String installedProfile (const juce::String& device)
{
    const auto e = installedEntry (device);
    return e.isObject() ? e["profile"].toString() : e.toString();
}

static juce::String fingerprint (std::vector<MidiBinding> bindings)
{
    juce::Array<juce::var> list;
    for (auto& b : bindings) { b.device = "*"; list.add (b.toVar()); }
    return juce::String::toHexString (juce::JSON::toString (juce::var (list), true).hashCode64());
}

static void rememberInstalled (const juce::String& device, const juce::String& profileName, const std::vector<MidiBinding>& bindings)
{
    auto v = juce::JSON::parse (installedFile());
    if (! v.isObject()) v = juce::var (new juce::DynamicObject());
    auto e = std::make_unique<juce::DynamicObject>();
    e->setProperty ("profile", profileName);
    e->setProperty ("hash", fingerprint (bindings));
    v.getDynamicObject()->setProperty (device, juce::var (e.release()));
    installedFile().getParentDirectory().createDirectory();
    installedFile().replaceWithText (juce::JSON::toString (v));
}

/** A mapping OpenMix installed from its library is brought up to date when a new version of OpenMix
    ships a better one, unless the user changed it since (MIDI learn, import): then it is left alone. */
void ControllerLibrary::refreshFromLibrary (const juce::String& deviceName)
{
    const auto entry = installedEntry (deviceName);
    const auto profile = installedProfile (deviceName);
    if (profile.isEmpty()) return;
    auto found = findBundled (deviceName);
    if (! found || found->first != profile) return;

    std::vector<MidiBinding> current;
    for (auto& b : midi.bindings()) if (b.device == deviceName) current.push_back (b);
    const auto now = fingerprint (current), shipped = fingerprint (found->second);
    if (now == shipped)
    {
        if (! entry.isObject()) rememberInstalled (deviceName, profile, current);
        return;
    }
    if (entry.isObject() && entry["hash"].toString() != now)
        return;                                                 // edited by the user: theirs now
    midi.installMapping (deviceName, found->second);
    rememberInstalled (deviceName, profile, found->second);
}

bool ControllerLibrary::restoreLibraryMapping (const juce::String& deviceName)
{
    auto found = findBundled (deviceName);
    if (! found) return false;
    midi.installMapping (deviceName, found->second);
    rememberInstalled (deviceName, found->first, found->second);
    for (auto& d : list)
        if (d.name == deviceName) { d.status = Status::mapped; d.profileName = found->first; }
    sendChangeMessage();
    return true;
}

void ControllerLibrary::install (const juce::String& deviceName, Result r)
{
    if (r.status == Status::mapped && ! r.bindings.empty())
    {
        if (! midi.hasBindingsFor (deviceName))                 // the user may have mapped it meanwhile
        {
            midi.installMapping (deviceName, r.bindings);
            rememberInstalled (deviceName, r.profileName, r.bindings);
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
        refreshFromLibrary (deviceName);
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
            refreshFromLibrary (name);
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

    // Drivers: every few seconds, notice plugged-in controllers whose driver is missing (read-only).
    if (now - lastDriverScan > 3000 && ! driverJob.exchange (true))
    {
        lastDriverScan = now;
        auto flag = alive;
        pool.addJob ([this, flag]
        {
            if (! flag->load()) { driverJob = false; return; }
            auto found = findMissingDrivers (midi.connectedInputs());
            juce::MessageManager::callAsync ([this, flag, missing = std::move (found)]
            {
                if (flag->load()) applyMissingDrivers (missing);
                driverJob = false;
            });
        });
    }
}

juce::StringArray ControllerLibrary::missingDriverPages (const juce::StringArray& midiDevices)
{
    juce::StringArray pages;
    for (auto& m : findMissingDrivers (midiDevices))
        pages.addIfNotAlreadyThere (m.page);
    return pages;
}

std::vector<ControllerLibrary::MissingDriver> ControllerLibrary::findMissingDrivers (const juce::StringArray& connected)
{
    std::vector<MissingDriver> out;
    const auto usb = DriverCheck::presentUsbIds();
    juce::String error;
    bool downloaded = false;
    for (auto& p : parseIndex (loadIndex (online.load(), false, downloaded, error)))
    {
        if (! p.driver.isValid())
            continue;
        MissingDriver m { p.name, p.driver.page, {} };
        for (auto& dev : connected)
            for (auto& n : p.midiNames)
                if (nameMatches (dev, n)) m.midiDevices.addIfNotAlreadyThere (dev);
        const bool viaUsb = std::any_of (p.usbIds.begin(), p.usbIds.end(), [&usb] (const juce::String& id) { return usb.contains (id); });
        if (m.midiDevices.isEmpty() && ! viaUsb)
            continue;                                                    // not plugged in
        const bool installed = std::any_of (p.driver.detect.begin(), p.driver.detect.end(),
                                            [] (const juce::String& f) { return DriverCheck::expandPath (f).exists(); });
        if (! installed)
            out.push_back (std::move (m));
    }
    return out;
}

void ControllerLibrary::applyMissingDrivers (const std::vector<MissingDriver>& missing)
{
    bool changed = false;
    // Placeholders for controllers that are plugged in but not visible as MIDI without their driver.
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Device& d)
    {
        const bool stale = d.status == Status::needsDriver
                        && std::none_of (missing.begin(), missing.end(), [&d] (const MissingDriver& m)
                                         { return m.profileName == d.name && m.midiDevices.isEmpty(); });
        changed = changed || stale;
        return stale;
    }), list.end());

    for (auto& d : list)
    {
        juce::String page;
        for (auto& m : missing)
            if (m.midiDevices.contains (d.name)) page = m.page;
        if (d.status != Status::needsDriver && page != d.driverPage) { d.driverPage = page; changed = true; }
    }
    for (auto& m : missing)
        if (m.midiDevices.isEmpty()
            && std::none_of (list.begin(), list.end(), [&m] (const Device& d) { return d.name == m.profileName; }))
        {
            Device d;
            d.name = m.profileName;
            d.profileName = m.profileName;
            d.status = Status::needsDriver;
            d.driverPage = m.page;
            list.push_back (d);
            changed = true;
        }
    if (changed) sendChangeMessage();
}

juce::String ControllerLibrary::summary() const
{
    if (list.empty()) return {};
    const auto& d = list.front();
    juce::String more = list.size() > 1 ? " +" + juce::String ((int) list.size() - 1) : juce::String();
    if (d.driverPage.isNotEmpty() && d.status != Status::needsDriver)
        more = juce::String::fromUTF8 (" · falta su driver") + more;
    switch (d.status)
    {
        case Status::searching:    return "Buscando mapeo…"_u + more;
        case Status::mapped:       return d.profileName + juce::String::fromUTF8 (" · listo") + more;
        case Status::mappedByUser: return d.name + juce::String::fromUTF8 (" · mapeo propio") + more;
        case Status::noMapping:    return d.name + juce::String::fromUTF8 (" · sin mapeo") + more;
        case Status::offline:      return d.name + juce::String::fromUTF8 (" · sin conexión") + more;
        case Status::needsDriver:  return d.name + juce::String::fromUTF8 (" · falta el driver") + more;
    }
    return {};
}

} // namespace om
