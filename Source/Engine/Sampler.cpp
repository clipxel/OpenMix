#include "Sampler.h"
#include "AudioEngine.h"

namespace om
{
bool Sampler::load (int index, const juce::File& file, juce::AudioFormatManager& formats)
{
    std::unique_ptr<juce::AudioFormatReader> reader (AudioEngine::openAudio (formats, file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0
        || (double) reader->lengthInSamples > 60.0 * reader->sampleRate || reader->lengthInSamples > 5760000)
        return false;

    juce::AudioBuffer<float> decoded (reader->numChannels > 1 ? 2 : 1, (int) reader->lengthInSamples);
    if (! reader->read (&decoded, 0, decoded.getNumSamples(), 0, true, true))
        return false;

    auto& slot = slots[(size_t) index];
    const juce::SpinLock::ScopedLockType guard (lock);
    slot.playing = false;
    slot.source = file;
    std::swap (slot.audio, decoded);
    slot.rate = reader->sampleRate;
    return true;
}

void Sampler::clear (int index)
{
    juce::AudioBuffer<float> removed;
    auto& slot = slots[(size_t) index];
    const juce::SpinLock::ScopedLockType guard (lock);
    slot.playing = false;
    slot.source = juce::File();
    std::swap (slot.audio, removed);
}

void Sampler::trigger (int index, int deck, float gain)
{
    auto& slot = slots[(size_t) index];
    const juce::SpinLock::ScopedLockType guard (lock);
    if (slot.audio.getNumSamples() == 0) return;
    slot.position = 0.0;
    slot.deck = deck;
    slot.gain = juce::jlimit (0.0f, 1.0f, gain);
    slot.playing = true;
}

void Sampler::stop (int index)
{
    const juce::SpinLock::ScopedLockType guard (lock);
    slots[(size_t) index].playing = false;
}

void Sampler::mix (int deck, float* left, float* right, int count, double outputRate)
{
    if (outputRate <= 0.0) return;
    const juce::SpinLock::ScopedTryLockType guard (lock);
    if (! guard.isLocked()) return;

    for (auto& slot : slots)
    {
        if (! slot.playing.load() || slot.deck != deck) continue;
        const auto* l = slot.audio.getReadPointer (0);
        const auto* r = slot.audio.getReadPointer (slot.audio.getNumChannels() - 1);
        const int length = slot.audio.getNumSamples();
        const double step = slot.rate / outputRate;
        for (int s = 0; s < count && slot.position < length; ++s)
        {
            const int a = (int) slot.position;
            const int b = juce::jmin (a + 1, length - 1);
            const auto fraction = (float) (slot.position - a);
            left[s] += slot.gain * (l[a] + fraction * (l[b] - l[a]));
            right[s] += slot.gain * (r[a] + fraction * (r[b] - r[a]));
            slot.position += step;
        }
        if (slot.position >= length) slot.playing = false;
    }
}
}
