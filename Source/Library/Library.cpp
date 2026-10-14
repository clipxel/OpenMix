#include "Library.h"
#include "../Engine/TrackAnalysis.h"

namespace om
{

static const juce::StringArray audioExtensions { "mp3", "wav", "aif", "aiff", "flac", "m4a", "aac", "ogg", "caf" };

Library::Library (juce::AudioFormatManager& fm) : formats (fm)
{
    load();

    for (auto& t : all)
        if (t->analysis == Track::Analysis::pending || t->analysis == Track::Analysis::analysing
            || (t->analysis == Track::Analysis::done && t->analysisVersion < currentAnalysisVersion))
        {
            t->analysis = Track::Analysis::pending;
            requestAnalysis (t, true);
        }
}

Library::~Library()
{
    alive->store (false);
    scanPool.removeAllJobs (true, 4000);
    analysisPool.removeAllJobs (true, 4000);
    if (dirty)
        save();
}

static juce::File& dataOverride() { static juce::File f; return f; }
void Library::setDataFolderOverride (const juce::File& f) { dataOverride() = f; }

juce::File Library::dataFolder()
{
    if (dataOverride() != juce::File())
    {
        dataOverride().createDirectory();
        return dataOverride();
    }

    // macOS: ~/Library/Application Support/OpenMix · Windows: %APPDATA%\OpenMix · Linux: ~/.config/OpenMix
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    auto f = base.getChildFile ("OpenMix");
    f.createDirectory();
    return f;
}

bool Library::isAudioFile (const juce::File& f)
{
    return audioExtensions.contains (f.getFileExtension().trimCharactersAtStart (".").toLowerCase());
}

TrackPtr Library::find (const juce::String& path) const
{
    auto it = byPath.find (path);
    return it != byPath.end() ? it->second : nullptr;
}

void Library::touched()
{
    ++rev;
    dirty = true;
    startTimer (800); // debounce saves
    sendChangeMessage();
}

void Library::timerCallback()
{
    stopTimer();
    if (dirty)
    {
        save();
        dirty = false;
    }
}

//==============================================================================
static TrackPtr makeTrack (const juce::File& f)
{
    auto t = std::make_shared<Track>();
    t->file = f;
    t->added = juce::Time::getCurrentTime();

    const auto tags = Metadata::read (f);
    t->title = tags.title;
    t->artist = tags.artist;
    t->album = tags.album;
    t->genre = tags.genre;
    t->comment = tags.comment;
    t->key = tags.key;
    t->duration = tags.durationSeconds;

    // Common "Artist - Title" file naming when tags are missing.
    if (t->title.isEmpty())
    {
        const auto name = f.getFileNameWithoutExtension();
        if (name.contains (" - ") && t->artist.isEmpty())
        {
            t->artist = name.upToFirstOccurrenceOf (" - ", false, false).trim();
            t->title  = name.fromFirstOccurrenceOf (" - ", false, false).trim();
        }
    }
    return t;
}

TrackPtr Library::addFile (const juce::File& f)
{
    if (auto existing = find (f.getFullPathName()))
        return existing;

    if (! f.existsAsFile() || ! isAudioFile (f))
        return nullptr;

    auto t = makeTrack (f);
    all.push_back (t);
    byPath[t->path()] = t;
    requestAnalysis (t);
    touched();
    return t;
}

void Library::addFolder (const juce::File& folder)
{
    if (! folder.isDirectory() || watched.contains (folder))
        return;

    watched.add (folder);
    touched();
    scanFolder (folder);
}

void Library::removeFolder (const juce::File& folder)
{
    watched.removeFirstMatchingValue (folder);
    touched();
}

void Library::rescan()
{
    for (auto& f : watched)
        scanFolder (f);
}

void Library::scanFolder (const juce::File& folder)
{
    juce::StringArray known;
    for (auto& t : all)
        known.add (t->path());

    ++scanJobs;
    sendChangeMessage();

    auto aliveFlag = alive;
    scanPool.addJob ([this, aliveFlag, folder, known]
    {
        std::vector<TrackPtr> batch;
        auto flush = [&]
        {
            if (batch.empty()) return;
            juce::MessageManager::callAsync ([this, aliveFlag, b = std::move (batch)]() mutable
            {
                if (aliveFlag->load()) addScanned (std::move (b));
            });
            batch = {};
        };

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            if (! aliveFlag->load())
                return;

            const auto& f = entry.getFile();
            if (! isAudioFile (f) || f.getFileName().startsWith ("._") || known.contains (f.getFullPathName()))
                continue;

            batch.push_back (makeTrack (f));
            if (batch.size() >= 40)
                flush();
        }
        flush();

        juce::MessageManager::callAsync ([this, aliveFlag]
        {
            if (! aliveFlag->load()) return;
            --scanJobs;
            sendChangeMessage();
        });
    });
}

void Library::addScanned (std::vector<TrackPtr> batch)
{
    for (auto& t : batch)
    {
        if (byPath.count (t->path()) > 0)
            continue;
        all.push_back (t);
        byPath[t->path()] = t;
        requestAnalysis (t);
    }
    touched();
}

//==============================================================================
void Library::requestAnalysis (const TrackPtr& t, bool force)
{
    if (t == nullptr || (! force && t->analysis == Track::Analysis::done))
        return;

    t->analysis = Track::Analysis::pending;
    ++analysisQueued;

    auto aliveFlag = alive;
    std::weak_ptr<Track> weak = t;
    const auto file = t->file;

    analysisPool.addJob ([this, aliveFlag, weak, file]
    {
        if (! aliveFlag->load())
            return;

        juce::MessageManager::callAsync ([aliveFlag, weak]
        {
            if (auto tr = weak.lock(); tr && aliveFlag->load())
                tr->analysis = Track::Analysis::analysing;
        });

        BeatGrid grid;
        juce::String key;
        double duration = 0.0;
        bool ok = false;

        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) })
        {
            const auto len = reader->lengthInSamples;
            if (len > 0 && reader->sampleRate > 0 && (double) len / reader->sampleRate < 60.0 * 45.0)
            {
                // Fast path: stream the file in chunks and keep a decimated mono copy
                // (~11 kHz). Beat tracking only needs < 200 Hz onset detail, so this is
                // ~4× less work and memory than analysing the full-rate stereo audio.
                const int factor = juce::jmax (1, (int) std::floor (reader->sampleRate / 11025.0));
                const double outRate = reader->sampleRate / factor;
                const int lead = Analysis::mp3TrimmedLeadSamples (file) / factor;   // club player timeline
                juce::AudioBuffer<float> mono (1, (int) (len / factor) + 1 + lead);
                mono.clear();
                juce::AudioBuffer<float> chunk ((int) juce::jmax (1u, reader->numChannels), 65536);
                int outIdx = lead;
                float acc = 0.0f;
                int accN = 0;

                for (juce::int64 startPos = 0; startPos < len && aliveFlag->load(); startPos += chunk.getNumSamples())
                {
                    const int n = (int) juce::jmin ((juce::int64) chunk.getNumSamples(), len - startPos);
                    reader->read (&chunk, 0, n, startPos, true, chunk.getNumChannels() > 1);
                    for (int i = 0; i < n; ++i)
                    {
                        float v = 0.0f;
                        for (int c = 0; c < chunk.getNumChannels(); ++c) v += chunk.getSample (c, i);
                        acc += v / (float) chunk.getNumChannels();
                        if (++accN == factor)
                        {
                            if (outIdx < mono.getNumSamples()) mono.setSample (0, outIdx++, acc / (float) factor);
                            acc = 0.0f;
                            accN = 0;
                        }
                    }
                }

                mono.setSize (1, outIdx, true);
                duration = (double) len / reader->sampleRate;
                grid = Analysis::detectBeatGrid (mono, outRate, nullptr);
                key = Analysis::detectKey (mono, outRate, nullptr);
                ok = aliveFlag->load();
            }
        }

        juce::MessageManager::callAsync ([this, aliveFlag, weak, grid, key, duration, ok]
        {
            if (! aliveFlag->load()) return;
            --analysisQueued;
            if (auto tr = weak.lock())
            {
                tr->analysis = ok ? Track::Analysis::done : Track::Analysis::failed;
                if (ok)
                {
                    tr->duration = duration;
                    if (! tr->gridEdited)
                    {
                        tr->bpm = grid.bpm;
                        tr->firstBeat = grid.firstBeatSeconds;
                    }
                    tr->detectedKey = key;
                    tr->analysisVersion = currentAnalysisVersion;
                }
            }
            touched();
        });
    });
}

void Library::trackEdited (const TrackPtr&) { touched(); }

//==============================================================================
int Library::createPlaylist (const juce::String& name)
{
    lists.push_back ({ name, {} });
    touched();
    return (int) lists.size() - 1;
}

void Library::renamePlaylist (int i, const juce::String& name)
{
    if (juce::isPositiveAndBelow (i, (int) lists.size())) { lists[(size_t) i].name = name; touched(); }
}

void Library::deletePlaylist (int i)
{
    if (juce::isPositiveAndBelow (i, (int) lists.size())) { lists.erase (lists.begin() + i); touched(); }
}

void Library::addToPlaylist (int i, const juce::StringArray& paths)
{
    if (! juce::isPositiveAndBelow (i, (int) lists.size())) return;
    for (auto& p : paths)
        lists[(size_t) i].paths.addIfNotAlreadyThere (p);
    touched();
}

void Library::removeFromPlaylist (int i, const juce::StringArray& paths)
{
    if (! juce::isPositiveAndBelow (i, (int) lists.size())) return;
    for (auto& p : paths)
        lists[(size_t) i].paths.removeString (p);
    touched();
}

void Library::notePlayed (const juce::File& f, int deck)
{
    // Unknown files (e.g. from an exported USB) are logged but not imported.
    if (auto t = find (f.getFullPathName()))
    {
        ++t->playCount;
        t->lastPlayed = juce::Time::getCurrentTime();
    }
    played.push_back ({ f.getFullPathName(), juce::Time::getCurrentTime(), deck });
    touched();
}

void Library::clearHistory()
{
    played.clear();
    touched();
}

//==============================================================================
static juce::String analysisToString (Track::Analysis a)
{
    switch (a)
    {
        case Track::Analysis::done:   return "done";
        case Track::Analysis::failed: return "failed";
        case Track::Analysis::pending:
        case Track::Analysis::analysing:
        default:                      return "pending";
    }
}

static Track::Analysis analysisFromString (const juce::String& s)
{
    if (s == "done")   return Track::Analysis::done;
    if (s == "failed") return Track::Analysis::failed;
    return Track::Analysis::pending;
}

void Library::save() const
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("version", 1);

    juce::Array<juce::var> folderArr;
    for (auto& f : watched) folderArr.add (f.getFullPathName());
    root->setProperty ("folders", folderArr);

    juce::Array<juce::var> trackArr;
    for (auto& t : all)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("path", t->path());
        o->setProperty ("title", t->title);
        o->setProperty ("artist", t->artist);
        o->setProperty ("album", t->album);
        o->setProperty ("genre", t->genre);
        o->setProperty ("comment", t->comment);
        o->setProperty ("key", t->key);
        o->setProperty ("bpm", t->bpm);
        o->setProperty ("firstBeat", t->firstBeat);
        o->setProperty ("duration", t->duration);
        o->setProperty ("analysis", analysisToString (t->analysis));
        o->setProperty ("added", t->added.toMilliseconds());
        o->setProperty ("playCount", t->playCount);
        o->setProperty ("lastPlayed", t->lastPlayed.toMilliseconds());
        o->setProperty ("cue", t->cuePoint);
        juce::Array<juce::var> hc;
        for (auto c : t->hotCues) hc.add (c);
        o->setProperty ("hotCues", hc);
        juce::Array<juce::var> sl;
        for (auto [a, b] : t->savedLoops) { sl.add (a); sl.add (b); }
        o->setProperty ("savedLoops", sl);
        o->setProperty ("detectedKey", t->detectedKey);
        o->setProperty ("analysisVersion", t->analysisVersion);
        if (t->gridEdited) o->setProperty ("gridEdited", true);
        trackArr.add (juce::var (o.release()));
    }
    root->setProperty ("tracks", trackArr);

    juce::Array<juce::var> listArr;
    for (auto& p : lists)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("name", p.name);
        juce::Array<juce::var> paths;
        for (auto& s : p.paths) paths.add (s);
        o->setProperty ("paths", paths);
        listArr.add (juce::var (o.release()));
    }
    root->setProperty ("playlists", listArr);

    juce::Array<juce::var> histArr;
    for (auto& h : played)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("path", h.path);
        o->setProperty ("time", h.time.toMilliseconds());
        o->setProperty ("deck", h.deck);
        histArr.add (juce::var (o.release()));
    }
    root->setProperty ("history", histArr);

    auto file = dataFolder().getChildFile ("library.json");
    auto tmp = file.getSiblingFile ("library.json.tmp");
    if (tmp.replaceWithText (juce::JSON::toString (juce::var (root.release()), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none))))
        tmp.moveFileTo (file);
}

void Library::load()
{
    const auto json = juce::JSON::parse (dataFolder().getChildFile ("library.json"));
    if (! json.isObject())
        return;

    if (auto* arr = json["folders"].getArray())
        for (auto& f : *arr)
            watched.add (juce::File (f.toString()));

    if (auto* arr = json["tracks"].getArray())
        for (auto& o : *arr)
        {
            auto t = std::make_shared<Track>();
            t->file = juce::File (o["path"].toString());
            t->title = o["title"].toString();
            t->artist = o["artist"].toString();
            t->album = o["album"].toString();
            t->genre = o["genre"].toString();
            t->comment = o["comment"].toString();
            t->key = o["key"].toString();
            t->bpm = (double) o["bpm"];
            t->firstBeat = (double) o["firstBeat"];
            t->duration = (double) o["duration"];
            t->analysis = analysisFromString (o["analysis"].toString());
            t->added = juce::Time ((juce::int64) o["added"]);
            t->playCount = (int) o["playCount"];
            t->lastPlayed = juce::Time ((juce::int64) o["lastPlayed"]);
            t->cuePoint = (double) o["cue"];
            if (auto* hc = o["hotCues"].getArray())
                for (int i = 0; i < juce::jmin (8, hc->size()); ++i)
                    t->hotCues[(size_t) i] = (double) (*hc)[i];
            if (auto* sl = o["savedLoops"].getArray())
                for (int i = 0; i + 1 < juce::jmin (16, sl->size()); i += 2)
                    t->savedLoops[(size_t) (i / 2)] = { (double) (*sl)[i], (double) (*sl)[i + 1] };
            t->detectedKey = o["detectedKey"].toString();
            t->analysisVersion = (int) o["analysisVersion"];
            t->gridEdited = (bool) o.getProperty ("gridEdited", false);

            if (t->path().isNotEmpty() && byPath.count (t->path()) == 0)
            {
                all.push_back (t);
                byPath[t->path()] = t;
            }
        }

    if (auto* arr = json["playlists"].getArray())
        for (auto& o : *arr)
        {
            Playlist p { o["name"].toString(), {} };
            if (auto* paths = o["paths"].getArray())
                for (auto& s : *paths) p.paths.add (s.toString());
            lists.push_back (p);
        }

    if (auto* arr = json["history"].getArray())
        for (auto& o : *arr)
            played.push_back ({ o["path"].toString(), juce::Time ((juce::int64) o["time"]), (int) o["deck"] });
}

} // namespace om
