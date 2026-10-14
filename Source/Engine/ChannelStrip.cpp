#include "ChannelStrip.h"

namespace om
{

float ChannelStrip::eqKnobToDb (float v) noexcept
{
    if (v <= 0.005f) return -100.0f;                 // kill
    if (v < 0.5f)    return juce::jmap (v, 0.0f, 0.5f, -36.0f, 0.0f);
    return juce::jmap (v, 0.5f, 1.0f, 0.0f, 6.0f);
}

float ChannelStrip::eqKnobToGain (float v) noexcept
{
    const auto db = eqKnobToDb (v);
    return db <= -99.0f ? 0.0f : juce::Decibels::decibelsToGain (db);
}

void ChannelStrip::prepare (double sampleRate, int maxBlock)
{
    colorFx.prepare (sampleRate, juce::jmax (1, maxBlock));
    {
        juce::dsp::ProcessSpec bs { sampleRate, (juce::uint32) juce::jmax (1, maxBlock), 2 };
        bandSvf.prepare (bs);
        bandSvf.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        bandSvf.setResonance (2.0f);
        bandMix.reset (sampleRate, 0.02);
    }
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, maxBlock), 2 };

    lowSplit.prepare (spec);   lowSplit.setCutoffFrequency (300.0f);
    highSplit.prepare (spec);  highSplit.setCutoffFrequency (3000.0f);
    lowAllpass.prepare (spec); lowAllpass.setCutoffFrequency (3000.0f);
    lowAllpass.setType (juce::dsp::LinkwitzRileyFilterType::allpass);

    svf.prepare (spec);
    svf.setResonance (0.95f);

    for (auto* s : { &gLow, &gMid, &gHigh, &gTrim, &filterMix })
        s->reset (sampleRate, 0.02);

    gLow.setCurrentAndTargetValue (1.0f);
    gMid.setCurrentAndTargetValue (1.0f);
    gHigh.setCurrentAndTargetValue (1.0f);
    gTrim.setCurrentAndTargetValue (1.0f);
    filterMix.setCurrentAndTargetValue (0.0f);
}

juce::String ChannelStrip::colorName (Color c)
{
    switch (c)
    {
        case Color::filter:  return "FILTER";
        case Color::space:   return "SPACE";
        case Color::dubEcho: return "DUB ECHO";
        case Color::sweep:   return "SWEEP";
        case Color::noise:   return "NOISE";
        case Color::crush:   return "CRUSH";
        case Color::numColors: break;
    }
    return {};
}

void ChannelStrip::processPreFader (float* l, float* r, int n, double deckBpm)
{
    gTrim.setTargetValue (juce::Decibels::decibelsToGain (trimDb.load()));
    gLow.setTargetValue  (eqKnobToGain (eqLow.load()));
    gMid.setTargetValue  (eqKnobToGain (eqMid.load()));
    gHigh.setTargetValue (eqKnobToGain (eqHigh.load()));

    // Sound Color: each colour also uses part of the filter (as on club mixers).
    const auto color = (Color) colorType.load();
    const float knob = filter.load();
    float fk = knob;
    switch (color)
    {
        case Color::space: case Color::crush: fk = knob * 0.5f; break;
        case Color::dubEcho: fk = knob * 0.4f; break;
        case Color::sweep: case Color::noise: fk = 0.0f; break;
        case Color::filter: case Color::numColors: break;
    }

    // Filter: dead zone around centre means "off"; crossfade avoids clicks on mode change.
    const bool filterOn = std::abs (fk) > 0.03f;
    filterMix.setTargetValue (filterOn ? 1.0f : 0.0f);

    if (filterOn)
    {
        const bool hp = fk > 0.0f;
        if ((lastFilterKnob > 0.0f) != hp || std::abs (lastFilterKnob) <= 0.03f)
            svf.setType (hp ? juce::dsp::StateVariableTPTFilterType::highpass
                            : juce::dsp::StateVariableTPTFilterType::lowpass);

        const float a = std::abs (fk);
        const float cutoff = hp ? 20.0f * std::pow (8000.0f / 20.0f, a)
                                : 20000.0f * std::pow (80.0f / 20000.0f, a);
        svf.setCutoffFrequency (juce::jlimit (20.0f, (float) (sr * 0.45), cutoff));
        lastFilterKnob = fk;
    }

    float* ch[2] = { l, r };
    for (int i = 0; i < n; ++i)
    {
        const float trim = gTrim.getNextValue();
        const float glo = gLow.getNextValue(), gmi = gMid.getNextValue(), ghi = gHigh.getNextValue();
        const float fm = filterMix.getNextValue();

        for (int c = 0; c < 2; ++c)
        {
            const float x = ch[c][i] * trim;
            float lo, rest, mid, hi;
            lowSplit.processSample (c, x, lo, rest);
            highSplit.processSample (c, rest, mid, hi);
            lo = lowAllpass.processSample (c, lo);
            float y = lo * glo + mid * gmi + hi * ghi;

            if (fm > 0.0f)
                y = y + fm * (svf.processSample (c, y) - y);

            ch[c][i] = y;
        }
    }

    // Colour effect proper (Filter colour is the SVF above).
    if (color != (Color) lastColor)
    {
        colorFx.enabled = false;
        lastColor = (int) color;
    }
    const float amt = std::abs (knob);
    const bool on = color != Color::filter && amt > 0.03f;
    switch (color)
    {
        case Color::space:   colorFx.type = (int) FxUnit::Type::reverb;     colorFx.param = 0.8f; colorFx.wet = amt * 0.85f; break;
        case Color::dubEcho: colorFx.type = (int) FxUnit::Type::tapeEcho;   colorFx.param = 0.5f; colorFx.feedback = 0.62f; colorFx.wet = amt; break;
        case Color::noise:   colorFx.type = (int) FxUnit::Type::noise;      colorFx.param = (knob + 1.0f) * 0.5f; colorFx.wet = amt * 0.6f; break;
        case Color::crush:   colorFx.type = (int) FxUnit::Type::bitcrusher; colorFx.param = amt; colorFx.wet = juce::jmin (1.0f, amt * 1.5f); break;
        case Color::sweep:   colorFx.type = (int) FxUnit::Type::trans;      colorFx.param = 0.3f; colorFx.wet = amt; break;
        case Color::filter: case Color::numColors: break;
    }
    // SWEEP: left = gate, right = sweeping band-pass.
    colorFx.enabled = on && ! (color == Color::sweep && knob > 0.0f);
    colorFx.process (l, r, n, deckBpm);

    const bool band = color == Color::sweep && knob > 0.03f;
    bandMix.setTargetValue (band ? amt : 0.0f);
    if (band || bandMix.isSmoothing() || bandMix.getCurrentValue() > 0.0f)
    {
        bandSvf.setCutoffFrequency (juce::jlimit (60.0f, (float) sr * 0.4f, 150.0f * std::pow (60.0f, amt)));
        for (int i = 0; i < n; ++i)
        {
            const float m = bandMix.getNextValue();
            l[i] += (bandSvf.processSample (0, l[i]) * 1.4f - l[i]) * m;
            r[i] += (bandSvf.processSample (1, r[i]) * 1.4f - r[i]) * m;
        }
    }
}

float ChannelStrip::postFaderGain (float x, int curve) const noexcept
{
    return faderGain() * crossfaderGain (x, curve);
}

float ChannelStrip::faderGain() const noexcept
{
    return faderToGain (fader.load());
}

float ChannelStrip::crossfaderGain (float x, int curve) const noexcept
{
    const auto assign = (XfAssign) xfAssign.load();
    if (assign == XfAssign::thru)
        return 1.0f;

    // x: −1 (full A) … +1 (full B). t = how far toward the opposite side.
    const float t = assign == XfAssign::a ? (x + 1.0f) * 0.5f : (1.0f - x) * 0.5f;
    float g;
    switch (curve)
    {
        case 1:  g = t < 0.97f ? 1.0f : 0.0f;  break;                                  // sharp (scratch)
        case 2:  g = std::cos (t * juce::MathConstants<float>::halfPi); break;         // constant power
        default: g = juce::jlimit (0.0f, 1.0f, 2.0f * (1.0f - t)); break;              // dipless
    }
    return g;
}

} // namespace om
