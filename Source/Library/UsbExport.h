#pragma once

#include "../Engine/TrackAnalysis.h"
#include <juce_core/juce_core.h>

namespace om
{

/** Read-only reader for USB/SD media exported for DJ players (device library).

    Parses the export database (DeviceSQL, the one club players read) and the per-track
    ANLZ files (beat grid, cues). Files are only ever opened through juce::FileInputStream;
    nothing on the medium is created, modified or deleted. */
namespace UsbExport
{
    /** Folder names fixed by the export format (the players look for exactly these). */
    inline constexpr const char* exportFolder = "PIONEER";
    inline constexpr const char* databaseFile = "PIONEER/rekordbox/export.pdb";

    struct TrackRow
    {
        int id = 0;
        juce::String title, artist, album, genre, key, comment, label;
        double bpm = 0.0;
        int durationSeconds = 0, year = 0, rating = 0, playCount = 0;
        juce::String filePath;      // relative to the medium root, e.g. /Contents/…/x.mp3
        juce::String analyzePath;   // <exportFolder>/USBANLZ/…/ANLZ0000.DAT
        juce::String dateAdded;     // YYYY-MM-DD
    };

    struct PlaylistRow
    {
        int id = 0, parentId = 0, sortOrder = 0;
        bool isFolder = false;
        juce::String name;
        std::vector<int> trackIds;   // in playlist order
    };

    struct Export
    {
        juce::File root;             // volume root
        juce::String volumeName;
        std::vector<TrackRow> tracks;
        std::vector<PlaylistRow> playlists;
        std::unordered_map<int, size_t> trackIndex;

        const TrackRow* track (int id) const;
        juce::File fileFor (const TrackRow&) const;
    };

    /** True if the volume contains a DJ device export. */
    bool isExportVolume (const juce::File& volumeRoot);

    /** Parses the export. Returns nullptr and fills `error` on failure. */
    std::unique_ptr<Export> load (const juce::File& volumeRoot, juce::String& error);

    /** Parses an in-memory export.pdb (exposed for tests). */
    std::unique_ptr<Export> parsePdb (const void* data, size_t size, juce::String& error);

    struct Analysis
    {
        BeatGrid grid;                                   // from PQTZ (downbeat-aligned)
        double tempoVariation = 0.0;                     // max − min BPM across the grid (0 = constant)
        double cuePoint = -1.0;                          // first memory cue
        std::array<double, 8> hotCues { -1, -1, -1, -1, -1, -1, -1, -1 };
        std::array<std::pair<double, double>, 8> hotLoops {};
    };

    /** Reads beat grid and cues from the track's ANLZ .DAT (and .EXT when present). */
    Analysis readAnalysis (const Export&, const TrackRow&);

    /** "Am" / "F#" / "8A" → Camelot ("8A"); empty if unknown. */
    juce::String keyToCamelot (const juce::String&);
}

} // namespace om
