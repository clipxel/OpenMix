#include "DeviceMonitor.h"
#include "Volumes.h"

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
        for (auto& f : mounted)
            if (UsbExport::isExportVolume (f))
                roots.add (f);
        juce::MessageManager::callAsync ([this, aliveFlag, roots, mounted]
        {
            if (! aliveFlag->load()) return;
            scanning = false;
            if (mounted != allVolumes) { allVolumes = mounted; ++rev; sendChangeMessage(); }
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
                for (const auto& row : ex->tracks)
                {
                    auto t = std::make_shared<Track>();
                    t->file = ex->fileFor (row);
                    t->title = row.title;
                    t->artist = row.artist;
                    t->album = row.album;
                    t->genre = row.genre;
                    t->comment = row.comment;
                    t->key = row.key;
                    t->detectedKey = UsbExport::keyToCamelot (row.key);
                    t->bpm = row.bpm;
                    t->duration = row.durationSeconds;
                    t->playCount = row.playCount;
                    if (row.dateAdded.length() >= 10)
                        t->added = juce::Time (row.dateAdded.substring (0, 4).getIntValue(), row.dateAdded.substring (5, 7).getIntValue() - 1,
                                               row.dateAdded.substring (8, 10).getIntValue(), 0, 0);
                    t->analysis = Track::Analysis::done;   // analysed when the drive was exported
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
                if (dev->ex != nullptr) list.push_back (dev);
                else error = root.getFileName() + ": " + err;
                ++rev;
                sendChangeMessage();
            });
        });
    }
}

} // namespace om
