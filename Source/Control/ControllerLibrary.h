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
    /** A driver the controller needs on this operating system. OpenMix never downloads or installs it:
        it only notices it is missing and points the user to the vendor's official download page. */
    struct Driver
    {
        juce::String page;                        // official download page (https)
        juce::StringArray detect;                 // files that exist once the driver is installed
        bool isValid() const { return page.startsWithIgnoreCase ("https://"); }
    };

    struct Profile
    {
        juce::String id, name, file, author, description;
        juce::StringArray midiNames;              // names the device announces (port suffixes ignored; "Name *" = family)
        juce::StringArray usbIds;                 // "vvvv:pppp" (hex)
        Driver driver;                            // for the operating system OpenMix runs on
    };

    enum class Status { searching, mapped, mappedByUser, noMapping, offline, needsDriver };
    struct Device
    {
        juce::String name;                        // MIDI input name as the OS reports it
        Status status = Status::searching;
        juce::String profileName;                 // when mapped from the library
        juce::String detail;                      // last error / extra info
        juce::String driverPage;                  // set when its driver is missing: where to get it
    };

    explicit ControllerLibrary (MidiEngine&);
    ~ControllerLibrary() override;

    /** Message thread, once per frame: notices new and removed devices. */
    void tick();

    const std::vector<Device>& devices() const noexcept { return list; }
    /** One-line status for the top bar ("Controlador listo", "Sin mapeo"…). */
    juce::String summary() const;

    /** Also use the mappings of other DJ software installed on this computer (default on). */
    void setLocalImport (bool on)             { localImport = on; }

    /** Look mappings up on the internet (default on). Off: only the local cache is used. */
    void setOnlineLookup (bool on)            { online = on; }
    bool onlineLookup() const noexcept        { return online; }

    /** Official driver pages recommended for these MIDI devices right now (tests; tick() does it in the background). */
    juce::StringArray missingDriverPages (const juce::StringArray& midiDevices);

    /** Resolves a device right now on the calling thread (tests; tick() does it in the background). */
    Device resolveNow (const juce::String& deviceName);

    /** True when this bundled mapping's unit mixes the decks itself (each deck needs its own output pair). */
    static bool hasHardwareMixer (const juce::String& profileName);
    /** Puts back the mapping that ships with OpenMix for this connected controller (undoes edits / imports). */
    bool restoreLibraryMapping (const juce::String& deviceName);
    /** The mapping that ships inside OpenMix for this controller name, if any: (its name, bindings). */
    static std::optional<std::pair<juce::String, std::vector<MidiBinding>>> findBundled (const juce::String& deviceName);

    static juce::String normalise (const juce::String& midiName);
    static bool nameMatches (const juce::String& deviceName, const juce::String& pattern);
    static std::vector<Profile> parseIndex (const juce::var&);

    /** Where the index lives (tests point it at a local file:// URL). */
    static void setIndexUrlOverride (const juce::URL& u) { indexOverride() = u; }

private:
    struct Result { Status status; juce::String profileName, detail; std::vector<MidiBinding> bindings; };
    Result lookup (const juce::String& deviceName);                 // any thread: cache + network
    void install (const juce::String& deviceName, Result);          // message thread
    void refreshFromLibrary (const juce::String& deviceName);       // message thread
    juce::var loadIndex (bool allowNetwork, bool force, bool& downloaded, juce::String& error);
    static juce::URL indexUrl();
    static juce::File cacheFolder();
    static juce::URL& indexOverride() { static juce::URL u; return u; }

    struct MissingDriver { juce::String profileName, page; juce::StringArray midiDevices; };
    std::vector<MissingDriver> findMissingDrivers (const juce::StringArray& connected);   // background, read-only
    void applyMissingDrivers (const std::vector<MissingDriver>&);    // message thread

    MidiEngine& midi;
    std::vector<Device> list;
    std::atomic<bool> online { true }, driverJob { false }, localImport { true };
    juce::uint32 lastDriverScan = 0;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("OpenMix controllers") };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    juce::uint32 lastScan = 0;
    juce::CriticalSection indexLock;            // one index download at a time
};

} // namespace om
