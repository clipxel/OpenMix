#pragma once

#include "Library.h"
#include "UsbExport.h"
#include <juce_events/juce_events.h>

namespace om
{

/** Watches mounted volumes for DJ USB/SD exports and exposes their tracks and
    playlists to the browser. Strictly read-only: exports are parsed into memory and
    tracks are played straight from the medium — nothing is written to it, and its
    tracks are not added to the local collection. */
class DeviceMonitor : public juce::ChangeBroadcaster,
                      private juce::Timer
{
public:
    struct Device
    {
        std::shared_ptr<UsbExport::Export> ex;
        std::vector<TrackPtr> tracks;                         // OpenMix view of the export rows
        std::unordered_map<int, TrackPtr> byId;
        std::unordered_map<juce::String, TrackPtr> byPath;
    };

    DeviceMonitor();
    ~DeviceMonitor() override;

    const std::vector<std::shared_ptr<Device>>& devices() const noexcept { return list; }
    TrackPtr findByPath (const juce::String& path) const;
    juce::uint64 revision() const noexcept { return rev; }
    /** Every mounted external volume (for the file explorer), exported or not. */
    const juce::Array<juce::File>& volumes() const noexcept { return allVolumes; }
    juce::String lastError() const { return error; }

    /** Beat grid + cues for a device track (reads its ANLZ files). */
    static UsbExport::Analysis analysisFor (const Track&);

    void rescanNow() { timerCallback(); }

    /** Folder whose subfolders stand in for the mounted drives. Snapshots point it at an empty
        folder so the picture never shows the drives plugged into this computer. */
    static void setVolumesFolderOverride (const juce::File& f) { volumesOverride() = f; }

private:
    void timerCallback() override;
    void applyRoots (const juce::Array<juce::File>&);

    std::vector<std::shared_ptr<Device>> list;
    juce::StringArray loadingRoots;
    juce::Array<juce::File> allVolumes;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("OpenMix devices") };
    juce::ThreadPool scanPool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("OpenMix volume scan") };
    std::atomic<bool> scanning { false };
    static juce::File& volumesOverride() { static juce::File f; return f; }
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    juce::uint64 rev = 1;
    juce::String error;
};

} // namespace om
