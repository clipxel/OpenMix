#pragma once

#include "Meter.h"
#include "FxUnit.h"
#include <juce_dsp/juce_dsp.h>
#include <atomic>

namespace om
{

/** Mixer channel: trim → 3-band isolator EQ → filter → (FX inserts) → meter → fader.
    All parameters are atomics written by the UI and read once per block. */
class ChannelStrip
{
public:
    enum class XfAssign { a = 0, thru = 1, b = 2 };

    // Parameters (UI writes, audio reads)
    std::atomic<float> trimDb { 0.0f };          // −12 … +12
    std::atomic<float> eqLow { 0.5f }, eqMid { 0.5f }, eqHigh { 0.5f };  // 0 = kill, .5 = 0 dB, 1 = +6 dB
    std::atomic<float> filter { 0.0f };          // −1 LPF … 0 off … +1 HPF
    std::atomic<float> fader { 1.0f };           // 0 … 1
    std::atomic<int>   xfAssign { (int) XfAssign::thru };
    std::atomic<bool>  pfl { false };

    /** Sound Color FX on the FILTER knob (club mixer style): left/right variations of each colour. */
    enum class Color { filter = 0, space, dubEcho, sweep, noise, crush, numColors };
    std::atomic<int> colorType { (int) Color::filter };
    std::atomic<bool> colorOn { true };          // a SOUND COLOR FX button is lit (off: the COLOR knob does nothing)
    std::atomic<float> colorParam { 0.5f };      // PARAMETER knob of the SOUND COLOR FX section (0 … 1)
    static juce::String colorName (Color);

    MeterState meter;                            // post-EQ/FX, pre-fader

    static float eqKnobToGain (float v) noexcept;
    static float eqKnobToDb (float v) noexcept;  // −inf represented as −100
    static float faderToGain (float v) noexcept  { return v * v; }

    void prepare (double sampleRate, int maxBlock);

    /** Processes the deck signal in-place (pre-fader). */
    void processPreFader (float* l, float* r, int n, double deckBpm = 0.0);

    /** Gain to apply when summing to master, including crossfader. */
    float postFaderGain (float crossfader, int curve) const noexcept;   // channel fader × crossfader
    float faderGain() const noexcept;                                   // channel fader only
    float crossfaderGain (float crossfader, int curve) const noexcept;  // crossfader only

private:
    using LR = juce::dsp::LinkwitzRileyFilter<float>;
    LR lowSplit, highSplit, lowAllpass;
    juce::dsp::StateVariableTPTFilter<float> svf;
    juce::SmoothedValue<float> gLow, gMid, gHigh, gTrim, filterMix;
    float lastFilterKnob = 0.0f, lastParam = -1.0f;
    FxUnit colorFx;
    juce::dsp::StateVariableTPTFilter<float> bandSvf;
    juce::SmoothedValue<float> bandMix;
    bool sweepPass = false;                      // SWEEP: band-pass (right) or band-stop (left)
    int lastColor = 0;
    double sr = 48000.0;
};

} // namespace om
