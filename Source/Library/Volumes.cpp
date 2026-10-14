#include "Volumes.h"

namespace om::Volumes
{

#if JUCE_WINDOWS
static juce::String systemDrive()
{
    return juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getFullPathName().substring (0, 2).toUpperCase();
}
#endif

#if JUCE_LINUX || JUCE_BSD
static juce::StringArray mountParents()
{
    const auto user = juce::SystemStats::getLogonName();
    return { "/media/" + user, "/run/media/" + user, "/media", "/mnt" };
}
#endif

juce::Array<juce::File> mounted()
{
    juce::Array<juce::File> out;
   #if JUCE_MAC
    for (const auto& e : juce::RangedDirectoryIterator (juce::File ("/Volumes"), false, "*", juce::File::findDirectories))
        if (! e.getFile().isSymbolicLink())                   // "Macintosh HD" → / (the system disk)
            out.add (e.getFile());
   #elif JUCE_WINDOWS
    juce::Array<juce::File> roots;
    juce::File::findFileSystemRoots (roots);
    for (auto& r : roots)
        if (r.getFullPathName().substring (0, 2).toUpperCase() != systemDrive() && r.isDirectory())
            out.add (r);
   #elif JUCE_LINUX || JUCE_BSD
    const auto user = juce::SystemStats::getLogonName();
    for (auto& parent : mountParents())
        for (const auto& e : juce::RangedDirectoryIterator (juce::File (parent), false, "*", juce::File::findDirectories))
        {
            const auto f = e.getFile();
            if (f.getFileName() == user && (parent == "/media"))   // /media/<user> is a parent, not a drive
                continue;
            if (! out.contains (f))
                out.add (f);
        }
   #endif
    return out;
}

juce::File rootOf (const juce::File& f)
{
    const auto path = f.getFullPathName();
   #if JUCE_MAC
    if (! path.startsWith ("/Volumes/"))
        return {};
    const auto name = path.fromFirstOccurrenceOf ("/Volumes/", false, false).upToFirstOccurrenceOf ("/", false, false);
    if (name.isEmpty())
        return {};
    const juce::File root ("/Volumes/" + name);
    return root.isSymbolicLink() ? juce::File() : root;     // "/Volumes/Macintosh HD" → the boot disk
   #elif JUCE_WINDOWS
    if (path.length() < 3 || path[1] != ':' || path.substring (0, 2).toUpperCase() == systemDrive())
        return {};
    return juce::File (path.substring (0, 3));
   #elif JUCE_LINUX || JUCE_BSD
    for (auto& parent : mountParents())
    {
        if (! path.startsWith (parent + "/"))
            continue;
        const auto name = path.fromFirstOccurrenceOf (parent + "/", false, false).upToFirstOccurrenceOf ("/", false, false);
        if (name.isEmpty() || (parent == "/media" && name == juce::SystemStats::getLogonName()))
            continue;
        return juce::File (parent + "/" + name);
    }
    return {};
   #else
    juce::ignoreUnused (path);
    return {};
   #endif
}

} // namespace om::Volumes
