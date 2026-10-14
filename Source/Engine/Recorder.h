#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

namespace om
{

/** Records the master output to a 24-bit WAV on a background writer thread. */
class Recorder
{
public:
    Recorder();
    ~Recorder();

    /** Message thread. Returns an error message, or empty on success. */
    juce::String start (const juce::File& file, double sampleRate);
    void stop();

    bool isRecording() const noexcept            { return recording.load(); }
    double getSecondsRecorded() const noexcept   { return (double) samplesWritten.load() / juce::jmax (1.0, rate.load()); }
    juce::File getFile() const                   { return currentFile; }
    bool hadOverflow() const noexcept            { return overflow.load(); }

    static juce::File defaultFolder();

    /** Audio thread. */
    void write (const float* l, const float* r, int n);

private:
    juce::TimeSliceThread thread { "OpenMix recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::SpinLock writerLock;
    juce::AudioFormatWriter::ThreadedWriter* active = nullptr;
    juce::File currentFile;
    std::atomic<bool> recording { false }, overflow { false };
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<double> rate { 48000.0 };
};

} // namespace om
