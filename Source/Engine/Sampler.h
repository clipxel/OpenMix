#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <array>

namespace om
{
class Sampler
{
public:
    static constexpr int numSlots = 8;

    bool load (int slot, const juce::File&, juce::AudioFormatManager&);
    void clear (int slot);
    juce::File file (int slot) const { return slots[(size_t) slot].source; }
    bool hasSample (int slot) const { return slots[(size_t) slot].source != juce::File(); }
    bool playing (int slot) const { return slots[(size_t) slot].playing.load(); }
    void trigger (int slot, int deck, float gain);
    void stop (int slot);
    void mix (int deck, float* left, float* right, int count, double outputRate);

private:
    struct Slot
    {
        juce::File source;
        juce::AudioBuffer<float> audio;
        double rate = 0.0, position = 0.0;
        int deck = 0;
        float gain = 0.0f;
        std::atomic<bool> playing { false };
    };
    juce::SpinLock lock;
    std::array<Slot, numSlots> slots;
};
}
