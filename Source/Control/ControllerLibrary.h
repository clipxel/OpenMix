#pragma once

#include "MidiEngine.h"
#include <juce_events/juce_events.h>

namespace om
{

/** Plug and play for controllers. When a MIDI device appears that has no bindings yet, it is
    looked up in the mapping library (official/index.json on the OpenMix site, cached locally) and
    its mapping is installed at once. Matching happens on this computer: the whole public index is
    downloaded, nothing about the device or the user is ever sent. A device the user mapped by hand
    is never touched. Offline, the last downloaded index keeps working. */
class ControllerLibrary : public juce::ChangeBroadcaster
{
public:
    struct Driver
    {
        juce::String url, sha256, args, signer;   // Windows installer (official vendor URL)
        bool isValid() const { return url.isNotEmpty() && sha256.length() == 64; }
    };

    struct Profile
    {
        juce::String id, name, file, author, description;
        juce::StringArray midiNames;              // names the device announces (port suffixes ignored; "Name *" = family)
        juce::StringArray usbIds;                 // "vvvv:pppp" (hex)
        Driver windowsDriver;
    };

    enum class Status { searching, mapped, mappedByUser, noMapping, offline };
    struct Device
    {
        juce::String name;                        // MIDI input name as the OS reports it
        Status status = Status::searching;
        juce::String profileName;                 // when mapped from the library
        juce::String detail;                      // last error / extra info
    };

    explicit ControllerLibrary (MidiEngine&);
    ~ControllerLibrary() override;

    /** Message thread, once per frame: notices new and removed devices. */
    void tick();

    const std::vector<Device>& devices() const noexcept { return list; }
    /** One-line status for the top bar ("Controlador listo", "Sin mapeo"…). */
    juce::String summary() const;

    /** Look mappings up on the internet (default on). Off: only the local cache is used. */
    void setOnlineLookup (bool on)            { online = on; }
    bool onlineLookup() const noexcept        { return online; }

    /** Resolves a device right now on the calling thread (tests; tick() does it in the background). */
    Device resolveNow (const juce::String& deviceName);

    static juce::String normalise (const juce::String& midiName);
    static bool nameMatches (const juce::String& deviceName, const juce::String& pattern);
    static std::vector<Profile> parseIndex (const juce::var&);

    /** Where the index lives (tests point it at a local file:// URL). */
    static void setIndexUrlOverride (const juce::URL& u) { indexOverride() = u; }

private:
    struct Result { Status status; juce::String profileName, detail; std::vector<MidiBinding> bindings; };
    Result lookup (const juce::String& deviceName);                 // any thread: cache + network
    void install (const juce::String& deviceName, Result);          // message thread
    juce::var loadIndex (bool allowNetwork, bool force, bool& downloaded, juce::String& error);
    static juce::URL indexUrl();
    static juce::File cacheFolder();
    static juce::URL& indexOverride() { static juce::URL u; return u; }

    MidiEngine& midi;
    std::vector<Device> list;
    std::atomic<bool> online { true };
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("OpenMix controllers") };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    juce::uint32 lastScan = 0;
    juce::CriticalSection indexLock;            // one index download at a time
};

} // namespace om
