#include "VolumeCueStore.h"
#include "../Common/Strings.h"

namespace om
{

static juce::File& extraRoot() { static juce::File f; return f; }
void VolumeCueStore::setExtraVolumeRoot (const juce::File& f) { extraRoot() = f; }

juce::File VolumeCueStore::externalVolumeFor (const juce::File& f)
{
    if (extraRoot() != juce::File() && f.isAChildOf (extraRoot()))
        return extraRoot();

    const auto path = f.getFullPathName();
    if (! path.startsWith ("/Volumes/"))
        return {};

    const auto name = path.fromFirstOccurrenceOf ("/Volumes/", false, false).upToFirstOccurrenceOf ("/", false, false);
    if (name.isEmpty())
        return {};

    const juce::File root ("/Volumes/" + name);
    if (root.isSymbolicLink())   // "/Volumes/Macintosh HD" → the boot disk
        return {};
    return root;
}

VolumeCueStore::Volume& VolumeCueStore::volume (const juce::File& root)
{
    auto& v = volumes[root.getFullPathName()];
    const auto file = storeFile (root);
    const auto mtime = file.getLastModificationTime();
    if (v.data.isVoid() || mtime != v.loadedMtime)
    {
        v.data = file.existsAsFile() ? juce::JSON::parse (file) : juce::var();
        if (! v.data.isObject())
        {
            auto o = std::make_unique<juce::DynamicObject>();
            o->setProperty ("version", 1);
            o->setProperty ("tracks", juce::var (new juce::DynamicObject()));
            v.data = juce::var (o.release());
        }
        v.loadedMtime = mtime;
    }
    return v;
}

std::optional<VolumeCueStore::Cues> VolumeCueStore::get (const juce::File& track)
{
    const auto root = externalVolumeFor (track);
    if (root == juce::File() || ! storeFile (root).existsAsFile())
        return std::nullopt;   // never creates anything on read

    const auto entry = volume (root).data["tracks"][juce::Identifier (track.getRelativePathFrom (root))];
    if (! entry.isObject())
        return std::nullopt;

    Cues c;
    c.cuePoint = (double) entry.getProperty ("cue", -1.0);
    if (auto* hc = entry["hotCues"].getArray())
        for (int i = 0; i < juce::jmin (8, hc->size()); ++i)
            c.hotCues[(size_t) i] = (double) (*hc)[i];
    if (auto* sl = entry["savedLoops"].getArray())
        for (int i = 0; i + 1 < juce::jmin (16, sl->size()); i += 2)
            c.savedLoops[(size_t) (i / 2)] = { (double) (*sl)[i], (double) (*sl)[i + 1] };
    c.bpm = (double) entry.getProperty ("bpm", 0.0);
    c.firstBeat = (double) entry.getProperty ("firstBeat", 0.0);
    return c;
}

juce::String VolumeCueStore::put (const juce::File& track, const Cues& c)
{
    const auto root = externalVolumeFor (track);
    if (root == juce::File())
        return "No es un disco externo";

    auto& v = volume (root);
    auto* tracks = v.data["tracks"].getDynamicObject();
    if (tracks == nullptr)
        return "Base de cues corrupta";

    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("cue", c.cuePoint);
    juce::Array<juce::var> hc, sl;
    for (auto h : c.hotCues) hc.add (h);
    for (auto [a, b] : c.savedLoops) { sl.add (a); sl.add (b); }
    o->setProperty ("hotCues", hc);
    o->setProperty ("savedLoops", sl);
    if (c.bpm > 0.0) { o->setProperty ("bpm", c.bpm); o->setProperty ("firstBeat", c.firstBeat); }
    o->setProperty ("updated", juce::Time::getCurrentTime().toISO8601 (true));
    tracks->setProperty (juce::Identifier (track.getRelativePathFrom (root)), juce::var (o.release()));

    // Writes are confined to <volume>/.OpenMix — atomic via temp file + rename inside it.
    const auto dir = root.getChildFile (".OpenMix");
    if (! dir.isDirectory() && ! dir.createDirectory())
        return "No se pudo crear .OpenMix en " + root.getFileName() + " (¿disco de solo lectura?)"_u;

    const auto file = storeFile (root);
    const auto tmp = dir.getChildFile ("cues.json.tmp");
    if (! tmp.replaceWithText (juce::JSON::toString (v.data, juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none))))
        return "No se pudo escribir en " + root.getFileName();
    if (! tmp.moveFileTo (file))
        return "No se pudo guardar cues.json en " + root.getFileName();

    v.loadedMtime = file.getLastModificationTime();
    return {};
}

} // namespace om
