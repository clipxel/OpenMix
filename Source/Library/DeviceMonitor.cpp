#include "DeviceMonitor.h"
#include "Volumes.h"
#include "VolumeCueStore.h"

namespace om
{

DeviceMonitor::DeviceMonitor()
{
    startTimer (3000);
    timerCallback();
}

DeviceMonitor::~DeviceMonitor()
{
    alive->store (false);
    pool.removeAllJobs (true, 4000);
    scanPool.removeAllJobs (true, 1000);
}

juce::String DeviceMonitor::lastError() const
{
    if (deniedVolumes.isNotEmpty()) return deniedVolumes;
    if (! errors.empty()) return errors.begin()->second;
    return {};
}

TrackPtr DeviceMonitor::findByPath (const juce::String& path) const
{
    for (auto& d : list)
        if (auto it = d->byPath.find (path); it != d->byPath.end())
            return it->second;
    return nullptr;
}

UsbExport::Analysis DeviceMonitor::analysisFor (const Track& t)
{
    if (t.device == nullptr)
        return {};
    if (const auto* row = t.device->track (t.deviceTrackId))
        return UsbExport::readAnalysis (*t.device, *row);
    return {};
}

void DeviceMonitor::timerCallback()
{
    // Which mounted drives carry an export right now? Listing drives can block for a long time on
    // a stuck or sleeping disk, so it never runs on the message thread.
    if (scanning.exchange (true)) return;
    auto aliveFlag = alive;
    const auto folderOverride = volumesOverride();
    scanPool.addJob ([this, aliveFlag, folderOverride]
    {
        juce::Array<juce::File> roots, mounted;
        if (folderOverride != juce::File())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (folderOverride, false, "*", juce::File::findDirectories))
                mounted.add (entry.getFile());
        }
        else
        {
            mounted = Volumes::mounted();
        }
        juce::StringArray denied;
        for (auto& f : mounted)
        {
            if (Volumes::accessDenied (f)) denied.add (f.getFileName());
            else if (UsbExport::isExportVolume (f)) roots.add (f);
        }
        const auto message = denied.isEmpty() ? juce::String()
            : juce::String::fromUTF8 ("Sin permiso para leer: ") + denied.joinIntoString (", ");
        juce::MessageManager::callAsync ([this, aliveFlag, roots, mounted, message]
        {
            if (! aliveFlag->load()) return;
            scanning = false;
            if (mounted != allVolumes) { allVolumes = mounted; ++rev; sendChangeMessage(); }
            if (message != deniedVolumes) { deniedVolumes = message; sendChangeMessage(); }
            applyRoots (roots);
        });
    });
}

void DeviceMonitor::applyRoots (const juce::Array<juce::File>& roots)
{
    // Drop devices that were ejected.
    const auto before = list.size();
    list.erase (std::remove_if (list.begin(), list.end(), [&roots] (const std::shared_ptr<Device>& d)
    {
        return ! roots.contains (d->ex->root);
    }), list.end());
    if (list.size() != before) { ++rev; sendChangeMessage(); }

    if (std::erase_if (errors, [&roots] (const auto& entry) { return ! roots.contains (juce::File (entry.first)); }) > 0)
        sendChangeMessage();

    // Parse new ones in the background.
    for (auto& root : roots)
    {
        const bool known = std::any_of (list.begin(), list.end(), [&root] (const std::shared_ptr<Device>& d) { return d->ex->root == root; });
        if (known || loadingRoots.contains (root.getFullPathName()))
            continue;

        loadingRoots.add (root.getFullPathName());
        auto aliveFlag = alive;
        pool.addJob ([this, aliveFlag, root]
        {
            juce::String err;
            std::shared_ptr<UsbExport::Export> ex (UsbExport::load (root, err).release());

            auto dev = std::make_shared<Device>();
            if (ex != nullptr)
            {
                dev->ex = ex;
                const auto driveDb = VolumeCueStore::readDatabase (root);   // OpenMix's analysis on this drive, read once
                for (const auto& row : ex->tracks)
                {
                    auto t = std::make_shared<Track>();
                    t->file = ex->fileFor (row);
                    t->title = row.title;
                    t->artist = row.artist;
                    t->album = row.album;
                    t->genre = row.genre;
                    t->comment = row.comment;
                    // Only OpenMix analyses: the export's BPM, key and grid are not used. Tracks never
                    // played in OpenMix show as not analysed and are analysed when loaded on a deck.
                    t->bpm = 0.0;
                    t->duration = row.durationSeconds;
                    t->playCount = row.playCount;
                    if (row.dateAdded.length() >= 10)
                        t->added = juce::Time (row.dateAdded.substring (0, 4).getIntValue(), row.dateAdded.substring (5, 7).getIntValue() - 1,
                                               row.dateAdded.substring (8, 10).getIntValue(), 0, 0);
                    t->analysis = Track::Analysis::pending;
                    if (auto c = VolumeCueStore::entryFor (driveDb, t->file.getRelativePathFrom (root)); c && (c->analysedBpm > 0.0 || c->bpm > 0.0))
                    {
                        t->bpm = c->bpm > 0.0 ? c->bpm : c->analysedBpm;
                        t->firstBeat = c->bpm > 0.0 ? c->firstBeat : c->analysedFirst;
                        t->detectedKey = c->key;
                        t->analysis = Track::Analysis::done;
                    }
                    t->device = ex;
                    t->deviceTrackId = row.id;
                    dev->byId[row.id] = t;
                    dev->byPath[t->path()] = t;
                    dev->tracks.push_back (t);
                }
            }

            juce::MessageManager::callAsync ([this, aliveFlag, root, dev, err]
            {
                if (! aliveFlag->load()) return;
                loadingRoots.removeString (root.getFullPathName());
                if (! allVolumes.contains (root)) return;
                const auto path = root.getFullPathName();
                if (dev->ex != nullptr)
                {
                    errors.erase (path);
                    list.push_back (dev);
                    ++rev;
                    sendChangeMessage();
                }
                else if (const auto message = root.getFileName() + ": " + err; errors[path] != message)
                {
                    errors[path] = message;
                    sendChangeMessage();
                }
            });
        });
    }
}

} // namespace om
