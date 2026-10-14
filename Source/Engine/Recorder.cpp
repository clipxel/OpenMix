#include "Recorder.h"
#include "../Common/Strings.h"

namespace om
{

Recorder::Recorder()  { thread.startThread(); }
Recorder::~Recorder() { stop(); thread.stopThread (2000); }

juce::File Recorder::defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("OpenMix Recordings");
}

juce::String Recorder::start (const juce::File& file, double sampleRate)
{
    stop();

    if (! file.getParentDirectory().createDirectory())
        return "No se pudo crear la carpeta " + file.getParentDirectory().getFullPathName();

    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    if (stream == nullptr)
        return "No se pudo escribir " + file.getFullPathName();

    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24);
    auto w = wav.createWriterFor (stream, options);
    if (w == nullptr)
        return "Formato de grabación no soportado a "_u + juce::String (sampleRate) + " Hz";

    writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), thread, 32768);
    currentFile = file;
    rate = sampleRate;
    samplesWritten = 0;
    overflow = false;

    {
        const juce::SpinLock::ScopedLockType sl (writerLock);
        active = writer.get();
    }
    recording = true;
    return {};
}

void Recorder::stop()
{
    {
        const juce::SpinLock::ScopedLockType sl (writerLock);
        active = nullptr;
    }
    recording = false;
    writer.reset(); // flushes to disk
}

void Recorder::write (const float* l, const float* r, int n)
{
    const juce::SpinLock::ScopedTryLockType sl (writerLock);
    if (! sl.isLocked() || active == nullptr)
        return;

    const float* chans[2] = { l, r };
    if (active->write (chans, n))
        samplesWritten += n;
    else
        overflow = true;
}

} // namespace om
