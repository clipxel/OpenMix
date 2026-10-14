#include "UsbExport.h"

namespace om::UsbExport
{

namespace
{
    // Little-endian readers with bounds checking (export.pdb is little-endian).
    struct Reader
    {
        const juce::uint8* p;
        size_t size;

        bool ok (size_t off, size_t n) const noexcept { return off + n <= size && off + n >= off; }
        juce::uint8  u8  (size_t o) const noexcept { return ok (o, 1) ? p[o] : 0; }
        juce::uint16 u16 (size_t o) const noexcept { return ok (o, 2) ? (juce::uint16) (p[o] | (p[o + 1] << 8)) : 0; }
        juce::uint32 u32 (size_t o) const noexcept
        {
            return ok (o, 4) ? (juce::uint32) p[o] | ((juce::uint32) p[o + 1] << 8) | ((juce::uint32) p[o + 2] << 16) | ((juce::uint32) p[o + 3] << 24) : 0;
        }

        /** DeviceSQL string at absolute offset `o`. */
        juce::String dstring (size_t o) const
        {
            if (! ok (o, 1)) return {};
            const auto kind = p[o];
            if ((kind & 1) == 1)                         // short ASCII: length in upper 7 bits
            {
                const size_t len = (size_t) (kind >> 1);
                if (len < 1 || ! ok (o + 1, len - 1)) return {};
                return juce::String::fromUTF8 ((const char*) p + o + 1, (int) len - 1);
            }
            const size_t total = u16 (o + 1);
            if (total < 4 || ! ok (o, total)) return {};
            const size_t n = total - 4;
            if (kind == 0x40)                            // long ASCII
                return juce::String::fromUTF8 ((const char*) p + o + 4, (int) n);
            if (kind == 0x90)                            // UTF-16LE
            {
                juce::String s;
                for (size_t i = 0; i + 1 < n; i += 2)
                {
                    const auto c = (juce::juce_wchar) (p[o + 4 + i] | (p[o + 5 + i] << 8));
                    if (c == 0) break;
                    s += juce::String::charToString (c);
                }
                return s;
            }
            return {};
        }
    };

    enum TableType { tracks = 0, genres = 1, artists = 2, albums = 3, labels = 4, keys = 5, colors = 6,
                     playlistTree = 7, playlistEntries = 8 };

    /** Calls fn (rowOffset) for every present row of every data page of a table. */
    template <typename Fn>
    void forEachRow (const Reader& r, juce::uint32 pageLen, juce::uint32 firstPage, juce::uint32 lastPage, Fn&& fn)
    {
        std::set<juce::uint32> visited;
        auto page = firstPage;
        while (visited.insert (page).second)
        {
            const size_t base = (size_t) page * pageLen;
            if (! r.ok (base, pageLen)) break;

            const auto flags = r.u8 (base + 0x1b);
            const bool isData = (flags & 0x40) == 0;
            if (isData)
            {
                const auto small = r.u8 (base + 0x18);
                const auto large = r.u16 (base + 0x22);
                const int numRows = (large > small && large != 0x1fff) ? large : small;
                const int groups = numRows > 0 ? (numRows - 1) / 16 + 1 : 0;

                for (int gi = 0; gi < groups; ++gi)
                {
                    const size_t gBase = base + pageLen - (size_t) gi * 0x24;
                    const auto present = r.u16 (gBase - 4);
                    for (int ri = 0; ri < 16; ++ri)
                    {
                        if (((present >> ri) & 1) == 0) continue;
                        const auto ofs = r.u16 (gBase - 6 - (size_t) ri * 2);
                        const size_t row = base + 0x28 + ofs;
                        if (r.ok (row, 4)) fn (row);
                    }
                }
            }

            if (page == lastPage) break;
            page = r.u32 (base + 0x0c);
        }
    }
}

//==============================================================================
const TrackRow* Export::track (int id) const
{
    auto it = trackIndex.find (id);
    return it != trackIndex.end() ? &tracks[it->second] : nullptr;
}

juce::File Export::fileFor (const TrackRow& t) const
{
    return root.getChildFile (t.filePath.trimCharactersAtStart ("/"));
}

bool isExportVolume (const juce::File& root)
{
    return root.getChildFile (databaseFile).existsAsFile();
}

std::unique_ptr<Export> parsePdb (const void* data, size_t size, juce::String& error)
{
    const Reader r { static_cast<const juce::uint8*> (data), size };
    if (size < 0x1c || r.u32 (0) != 0)
    {
        error = "export.pdb no tiene el formato esperado";
        return nullptr;
    }

    const auto pageLen = r.u32 (4);
    const auto numTables = r.u32 (8);
    if (pageLen < 512 || pageLen > 65536 || numTables == 0 || numTables > 64)
    {
        error = juce::String::fromUTF8 ("export.pdb: encabezado inválido");
        return nullptr;
    }

    struct Ptr { juce::uint32 first = 0, last = 0; bool present = false; };
    std::map<int, Ptr> tables;
    for (juce::uint32 i = 0; i < numTables; ++i)
    {
        const size_t o = 0x1c + (size_t) i * 16;
        tables[(int) r.u32 (o)] = { r.u32 (o + 8), r.u32 (o + 12), true };
    }

    auto each = [&] (int type, auto&& fn)
    {
        if (auto it = tables.find (type); it != tables.end())
            forEachRow (r, pageLen, it->second.first, it->second.last, fn);
    };

    // Lookup tables -----------------------------------------------------------
    std::unordered_map<juce::uint32, juce::String> artistNames, albumNames, genreNames, keyNames, labelNames;

    each (artists, [&] (size_t row)
    {
        const auto sub = r.u16 (row);
        const auto id = r.u32 (row + 4);
        const size_t nameOfs = sub == 0x64 ? r.u16 (row + 0x0a) : r.u8 (row + 9);
        artistNames[id] = r.dstring (row + nameOfs);
    });
    each (albums, [&] (size_t row)
    {
        const auto sub = r.u16 (row);
        const auto id = r.u32 (row + 0x0c);
        const size_t nameOfs = sub == 0x84 ? r.u16 (row + 0x16) : r.u8 (row + 0x15);
        albumNames[id] = r.dstring (row + nameOfs);
    });
    each (genres, [&] (size_t row) { genreNames[r.u32 (row)] = r.dstring (row + 4); });
    each (labels, [&] (size_t row) { labelNames[r.u32 (row)] = r.dstring (row + 4); });
    each (keys,   [&] (size_t row) { keyNames[r.u32 (row)] = r.dstring (row + 8); });

    auto name = [] (const auto& map, juce::uint32 id) -> juce::String
    {
        auto it = map.find (id);
        return it != map.end() ? it->second : juce::String();
    };

    auto out = std::make_unique<Export>();

    // Tracks -------------------------------------------------------------------
    each (tracks, [&] (size_t row)
    {
        TrackRow t;
        t.id = (int) r.u32 (row + 0x48);
        t.bpm = r.u32 (row + 0x38) / 100.0;
        t.artist = name (artistNames, r.u32 (row + 0x44));
        t.album = name (albumNames, r.u32 (row + 0x40));
        t.genre = name (genreNames, r.u32 (row + 0x3c));
        t.label = name (labelNames, r.u32 (row + 0x28));
        t.key = name (keyNames, r.u32 (row + 0x20));
        t.playCount = r.u16 (row + 0x4e);
        t.year = r.u16 (row + 0x50);
        t.durationSeconds = r.u16 (row + 0x54);
        t.rating = r.u8 (row + 0x59);

        auto str = [&] (int index) { return r.dstring (row + r.u16 (row + 0x5e + (size_t) index * 2)); };
        t.analyzePath = str (14);
        t.dateAdded = str (10);
        t.comment = str (16);
        t.title = str (17);
        t.filePath = str (20);
        if (t.title.isEmpty()) t.title = str (19);

        if (t.id > 0 && t.filePath.isNotEmpty() && out->trackIndex.count (t.id) == 0)
        {
            out->trackIndex[t.id] = out->tracks.size();
            out->tracks.push_back (std::move (t));
        }
    });

    // Playlists ----------------------------------------------------------------
    std::unordered_map<int, size_t> listIndex;
    each (playlistTree, [&] (size_t row)
    {
        PlaylistRow p;
        p.parentId = (int) r.u32 (row);
        p.sortOrder = (int) r.u32 (row + 8);
        p.id = (int) r.u32 (row + 0x0c);
        p.isFolder = r.u32 (row + 0x10) != 0;
        p.name = r.dstring (row + 0x14);
        if (listIndex.count (p.id) == 0)
        {
            listIndex[p.id] = out->playlists.size();
            out->playlists.push_back (std::move (p));
        }
    });

    std::unordered_map<int, std::vector<std::pair<int, int>>> entries; // playlist → (index, track)
    each (playlistEntries, [&] (size_t row)
    {
        entries[(int) r.u32 (row + 8)].push_back ({ (int) r.u32 (row), (int) r.u32 (row + 4) });
    });
    for (auto& [pid, list] : entries)
        if (auto it = listIndex.find (pid); it != listIndex.end())
        {
            std::sort (list.begin(), list.end());
            for (auto& e : list) out->playlists[it->second].trackIds.push_back (e.second);
        }

    std::sort (out->playlists.begin(), out->playlists.end(), [] (const PlaylistRow& a, const PlaylistRow& b)
    {
        return a.parentId != b.parentId ? a.parentId < b.parentId : a.sortOrder < b.sortOrder;
    });

    if (out->tracks.empty())
    {
        error = "El pendrive exportado no contiene pistas";
        return nullptr;
    }
    return out;
}

std::unique_ptr<Export> load (const juce::File& root, juce::String& error)
{
    const auto pdb = root.getChildFile (databaseFile);
    juce::MemoryBlock mb;
    {
        juce::FileInputStream in (pdb);   // read-only
        if (! in.openedOk())
        {
            error = "No se pudo leer " + pdb.getFullPathName();
            return nullptr;
        }
        in.readIntoMemoryBlock (mb);
    }

    auto ex = parsePdb (mb.getData(), mb.getSize(), error);
    if (ex != nullptr)
    {
        ex->root = root;
        ex->volumeName = root.getFileName();
    }
    return ex;
}

//==============================================================================
namespace
{
    // ANLZ files are big-endian.
    struct BE
    {
        const juce::uint8* p;
        size_t size;
        bool ok (size_t o, size_t n) const noexcept { return o + n <= size && o + n >= o; }
        juce::uint8  u8  (size_t o) const noexcept { return ok (o, 1) ? p[o] : 0; }
        juce::uint16 u16 (size_t o) const noexcept { return ok (o, 2) ? (juce::uint16) ((p[o] << 8) | p[o + 1]) : 0; }
        juce::uint32 u32 (size_t o) const noexcept
        {
            return ok (o, 4) ? ((juce::uint32) p[o] << 24) | ((juce::uint32) p[o + 1] << 16) | ((juce::uint32) p[o + 2] << 8) | p[o + 3] : 0;
        }
        bool tag (size_t o, const char* t) const noexcept { return ok (o, 4) && std::memcmp (p + o, t, 4) == 0; }
    };

    template <typename Fn>
    void forEachSection (const BE& b, Fn&& fn)
    {
        if (! b.tag (0, "PMAI")) return;
        size_t o = b.u32 (4);                       // header length
        while (b.ok (o, 12))
        {
            const auto lenTag = b.u32 (o + 8);
            if (lenTag < 12) break;
            fn (o, lenTag);
            o += lenTag;
        }
    }

    void readFile (const juce::File& f, juce::MemoryBlock& mb)
    {
        juce::FileInputStream in (f);              // read-only
        if (in.openedOk()) in.readIntoMemoryBlock (mb);
    }
}

Analysis readAnalysis (const Export& ex, const TrackRow& t)
{
    Analysis a;
    if (t.analyzePath.isEmpty())
        return a;

    const auto dat = ex.root.getChildFile (t.analyzePath.trimCharactersAtStart ("/"));
    const auto ext = dat.withFileExtension ("EXT");

    auto parse = [&a] (const juce::File& f, bool isExt)
    {
        juce::MemoryBlock mb;
        readFile (f, mb);
        const BE b { static_cast<const juce::uint8*> (mb.getData()), mb.getSize() };

        forEachSection (b, [&] (size_t o, juce::uint32 lenTag)
        {
            const auto lenHeader = b.u32 (o + 4);

            if (! isExt && b.tag (o, "PQTZ") && ! a.grid.isValid())
            {
                const auto n = b.u32 (o + 0x14);
                std::vector<double> tempos;
                double firstDownbeat = -1.0, firstBeat = -1.0;
                for (juce::uint32 i = 0; i < n; ++i)
                {
                    const size_t e = o + lenHeader + (size_t) i * 8;
                    if (! b.ok (e, 8) || e + 8 > o + lenTag) break;
                    const auto beatNo = b.u16 (e);
                    const double bpm = b.u16 (e + 2) / 100.0;
                    const double time = b.u32 (e + 4) / 1000.0;
                    if (bpm > 0) tempos.push_back (bpm);
                    if (firstBeat < 0) firstBeat = time;
                    if (beatNo == 1 && firstDownbeat < 0) firstDownbeat = time;
                }
                if (! tempos.empty())
                {
                    const auto [mn, mx] = std::minmax_element (tempos.begin(), tempos.end());
                    a.tempoVariation = *mx - *mn;
                    std::nth_element (tempos.begin(), tempos.begin() + (long) tempos.size() / 2, tempos.end());
                    const double bpm = tempos[tempos.size() / 2];
                    const double bar = 4.0 * 60.0 / bpm;
                    const double anchor = firstDownbeat >= 0 ? firstDownbeat : firstBeat;
                    a.grid.bpm = bpm;
                    a.grid.firstBeatSeconds = std::fmod (juce::jmax (0.0, anchor), bar); // keeps bars aligned
                }
            }

            const bool cueList = (! isExt && b.tag (o, "PCOB")) || (isExt && b.tag (o, "PCO2"));
            if (! cueList)
                return;

            const auto numCues = isExt ? b.u16 (o + 0x10) : b.u16 (o + 0x12);
            size_t e = o + lenHeader;
            for (int i = 0; i < numCues && b.ok (e, 12) && e < o + lenTag; ++i)
            {
                const auto lenEntry = b.u32 (e + 8);
                if (lenEntry < 0x20) break;
                const auto hot = b.u32 (e + 0x0c);
                double time, loopTime;
                int type;
                if (isExt)       // PCP2
                {
                    type = b.u8 (e + 0x10);
                    time = b.u32 (e + 0x14) / 1000.0;
                    loopTime = b.u32 (e + 0x18) / 1000.0;
                }
                else             // PCPT
                {
                    if (b.u32 (e + 0x10) == 0) { e += lenEntry; continue; }   // disabled
                    type = b.u8 (e + 0x1c);
                    time = b.u32 (e + 0x20) / 1000.0;
                    loopTime = b.u32 (e + 0x24) / 1000.0;
                }

                if (hot >= 1 && hot <= 8)
                {
                    a.hotCues[(size_t) hot - 1] = time;
                    if (type == 2 && loopTime > time) a.hotLoops[(size_t) hot - 1] = { time, loopTime };
                }
                else if (hot == 0 && (a.cuePoint < 0 || time < a.cuePoint))
                {
                    a.cuePoint = time;
                }
                e += lenEntry;
            }
        });
    };

    parse (dat, false);
    if (ext.existsAsFile())
        parse (ext, true);   // richer cue data (colours, more slots) overrides .DAT
    return a;
}

juce::String keyToCamelot (const juce::String& raw)
{
    auto k = raw.trim();
    if (k.isEmpty()) return {};
    if (k.getIntValue() >= 1 && (k.endsWithIgnoreCase ("A") || k.endsWithIgnoreCase ("B")))
        return juce::String (k.getIntValue()) + k.getLastCharacters (1).toUpperCase();

    const bool minor = k.endsWithIgnoreCase ("m") || k.containsIgnoreCase ("min");
    auto note = k.upToFirstOccurrenceOf ("m", false, true).upToFirstOccurrenceOf (" ", false, false).trim();
    static const std::pair<const char*, int> names[] = {
        { "C", 0 }, { "C#", 1 }, { "Db", 1 }, { "D", 2 }, { "D#", 3 }, { "Eb", 3 }, { "E", 4 }, { "F", 5 }, { "F#", 6 },
        { "Gb", 6 }, { "G", 7 }, { "G#", 8 }, { "Ab", 8 }, { "A", 9 }, { "A#", 10 }, { "Bb", 10 }, { "B", 11 } };
    for (auto& [n, pc] : names)
        if (note == n)
        {
            const int majorPc = minor ? (pc + 3) % 12 : pc;
            return juce::String ((majorPc * 7 + 7) % 12 + 1) + (minor ? "A" : "B");
        }
    return {};
}

} // namespace om::UsbExport
