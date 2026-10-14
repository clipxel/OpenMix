#pragma once

#include <juce_events/juce_events.h>

namespace om
{

/** Self-update. On launch OpenMix reads the latest release's update.json (version + SHA-256 of each
    installer, written by the release workflow). If it is newer, the user is offered "Actualizar
    ahora / Más tarde". "Now" downloads the installer for this system, checks its SHA-256, prepares
    the new app next to the old one and, once OpenMix quits, a small script swaps them and opens the
    new version. No reinstall, nothing to do by hand. */
class Updater : public juce::ChangeBroadcaster
{
public:
    struct Info
    {
        juce::String version, notes, url, sha256, file;
        juce::int64 size = 0;
    };

    enum class State { idle, checking, upToDate, available, downloading, ready, failed };

    Updater();
    ~Updater() override;

    /** Background check (message thread to call; the answer arrives as a change message). */
    void checkForUpdates();
    /** Background download + verification + staging. When ready(), call installAndRelaunch(). */
    void download();
    /** Starts the swap script and quits OpenMix. Message thread. */
    void installAndRelaunch();

    State state() const noexcept           { return current.load(); }
    const Info& available() const noexcept { return info; }
    float progress() const noexcept        { return prog.load(); }
    juce::String error() const             { return lastError; }

    static juce::String currentVersion();
    /** −1 / 0 / +1, numeric parts ("1.10.0" > "1.9.2"). */
    static int compareVersions (const juce::String& a, const juce::String& b);
    /** This system's entry of an update.json ("mac", "windows", "linux"). */
    static std::optional<Info> parseManifest (const juce::var& manifest);
    static void setManifestUrlOverride (const juce::URL& u) { manifestOverride() = u; }

    /** Blocking pieces (tests and the diagnostics flag). */
    void setAvailable (const Info& i)      { info = i; current = State::available; }
    void markReady()                       { current = State::ready; }
    bool downloadNow();
    juce::File stagedApp() const { return staged; }

private:
    static juce::URL& manifestOverride() { static juce::URL u; return u; }
    bool stage (const juce::File& package);

    std::atomic<State> current { State::idle };
    std::atomic<float> prog { 0.0f };
    Info info;
    juce::String lastError;
    juce::File workDir, staged;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("OpenMix update") };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace om
