#pragma once

#include "MetadataReader.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <memory>

namespace om
{

namespace UsbExport { struct Export; }

struct Track
{
    enum class Analysis { pending, analysing, done, failed };

    juce::File file;
    juce::String title, artist, album, genre, comment, key;
    double bpm = 0.0;              // 0 → unknown
    double firstBeat = 0.0;
    double duration = 0.0;
    Analysis analysis = Analysis::pending;
    juce::Time added;
    int playCount = 0;
    juce::Time lastPlayed;
    double cuePoint = 0.0;
    std::array<double, 8> hotCues { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::array<std::pair<double, double>, 8> savedLoops {};
    juce::String detectedKey;      // Camelot, from analysis
    bool gridEdited = false;       // corrected by hand: analysis never overwrites it
    int analysisVersion = 0;       // bump Library::currentAnalysisVersion to re-analyse old entries

    // Tracks from an exported USB/SD: read-only, never stored in the local collection.
    std::shared_ptr<const UsbExport::Export> device;
    int deviceTrackId = 0;
    bool isExternal() const noexcept { return device != nullptr; }

    /** Key shown to the user: detected (Camelot) first, file tag as fallback. */
    juce::String displayKey() const { return detectedKey.isNotEmpty() ? detectedKey : key; }

    juce::String path() const      { return file.getFullPathName(); }
    juce::String displayTitle() const { return title.isNotEmpty() ? title : file.getFileNameWithoutExtension(); }
};

using TrackPtr = std::shared_ptr<Track>;

struct Playlist
{
    juce::String name;
    juce::StringArray paths;
};

struct HistoryEntry
{
    juce::String path;
    juce::Time time;
    int deck = 0;
};

/** Music collection: watched folders, tag reading, BPM analysis, playlists and
    play history. All public methods run on the message thread; scanning and
    analysis happen on background threads and post results back. */
class Library : public juce::ChangeBroadcaster,
                private juce::Timer
{
public:
    explicit Library (juce::AudioFormatManager&);
    ~Library() override;

    static juce::File dataFolder();
    /** Redirects all persistence (used by headless snapshot runs so user data is untouched). */
    static void setDataFolderOverride (const juce::File&);
    static bool isAudioFile (const juce::File&);
    static constexpr int currentAnalysisVersion = 7;   // … 5: whole-track attack fit, 6: grid fitted on the drawn hits + negative-phase fix + BPM 70–160

    const std::vector<TrackPtr>& tracks() const noexcept   { return all; }
    TrackPtr find (const juce::String& path) const;
    TrackPtr addFile (const juce::File&);                  // e.g. dropped from Finder

    // Folders
    const juce::Array<juce::File>& folders() const noexcept { return watched; }
    void addFolder (const juce::File&);
    void removeFolder (const juce::File&);
    void rescan();
    bool isScanning() const noexcept                        { return scanJobs.load() > 0; }

    // Playlists
    const std::vector<Playlist>& playlists() const noexcept { return lists; }
    int  createPlaylist (const juce::String& name);
    void renamePlaylist (int index, const juce::String& name);
    void deletePlaylist (int index);
    void addToPlaylist (int index, const juce::StringArray& paths);
    void removeFromPlaylist (int index, const juce::StringArray& paths);

    // History
    const std::vector<HistoryEntry>& history() const noexcept { return played; }
    void notePlayed (const juce::File&, int deck);
    void clearHistory();

    // Analysis
    void requestAnalysis (const TrackPtr&, bool force = false);
    std::function<void (const TrackPtr&)> onAnalysed;     // message thread, after an analysis finished
    int  pendingAnalysisCount() const noexcept              { return analysisQueued; }

    void trackEdited (const TrackPtr&);                     // cues / tags edited → save

    /** Monotonic revision, bumped on every content change (cheap change detection). */
    juce::uint64 revision() const noexcept                  { return rev; }

private:
    void touched();
    void timerCallback() override;
    void load();
    void save() const;
    void scanFolder (const juce::File&);
    void addScanned (std::vector<TrackPtr> batch);

    juce::AudioFormatManager& formats;
    std::vector<TrackPtr> all;
    std::unordered_map<juce::String, TrackPtr> byPath;
    juce::Array<juce::File> watched;
    std::vector<Playlist> lists;
    std::vector<HistoryEntry> played;

    juce::ThreadPool scanPool { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("OpenMix scan") };
    juce::ThreadPool analysisPool { juce::ThreadPoolOptions{}.withNumberOfThreads (2).withThreadName ("OpenMix analysis") };
    std::atomic<int> scanJobs { 0 };
    int analysisQueued = 0;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    juce::uint64 rev = 1;
    bool dirty = false;
};

} // namespace om
