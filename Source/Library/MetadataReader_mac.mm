#include "MetadataReader.h"

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

namespace om::Metadata
{

static juce::String fromNS (id value)
{
    if (value == nil)
        return {};
    if ([value isKindOfClass: [NSString class]])
        return juce::String::fromUTF8 ([(NSString*) value UTF8String]).trim();
    if ([value isKindOfClass: [NSNumber class]])
        return juce::String::fromUTF8 ([[(NSNumber*) value stringValue] UTF8String]);
    return {};
}

FileTags read (const juce::File& file)
{
    FileTags tags;

    @autoreleasepool
    {
        NSString* path = [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()];
        if (path == nil)
            return tags;
        NSURL* url = [NSURL fileURLWithPath: (NSString* _Nonnull) path];
        AudioFileID audioFile = nullptr;

        if (AudioFileOpenURL ((__bridge CFURLRef) url, kAudioFileReadPermission, 0, &audioFile) != noErr)
            return tags;

        CFDictionaryRef info = nullptr;
        UInt32 size = sizeof (info);
        if (AudioFileGetProperty (audioFile, kAudioFilePropertyInfoDictionary, &size, &info) == noErr && info != nullptr)
        {
            NSDictionary* d = (__bridge NSDictionary*) info;
            tags.title    = fromNS (d[@kAFInfoDictionary_Title]);
            tags.artist   = fromNS (d[@kAFInfoDictionary_Artist]);
            tags.album    = fromNS (d[@kAFInfoDictionary_Album]);
            tags.genre    = fromNS (d[@kAFInfoDictionary_Genre]);
            tags.comment  = fromNS (d[@kAFInfoDictionary_Comments]);
            tags.key      = fromNS (d[@kAFInfoDictionary_KeySignature]);
            tags.bpm      = fromNS (d[@kAFInfoDictionary_Tempo]).getDoubleValue();
            tags.durationSeconds = fromNS (d[@kAFInfoDictionary_ApproximateDurationInSeconds]).getDoubleValue();
            CFRelease (info);
        }

        Float64 seconds = 0;
        size = sizeof (seconds);
        if (AudioFileGetProperty (audioFile, kAudioFilePropertyEstimatedDuration, &size, &seconds) == noErr && seconds > 0)
            tags.durationSeconds = seconds;

        AudioFileClose (audioFile);
    }

    return tags;
}

} // namespace om::Metadata
