#pragma once

#include <juce_core/juce_core.h>

namespace om::Volumes
{

/** External drives mounted on this computer (USB sticks, SD cards, external disks), never the
    system disk. macOS: /Volumes/<name>. Windows: drive letters other than the system drive.
    Linux: /media/<user>/<name>, /run/media/<user>/<name> and /mnt/<name>.
    Listing can block on a sleeping disk: call it off the message thread. */
juce::Array<juce::File> mounted();

/** The external drive that holds `f` (its mount root), or an empty File for the internal disk.
    Works from the path alone, without touching the disk. */
juce::File rootOf (const juce::File& f);

} // namespace om::Volumes
