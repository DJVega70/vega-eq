#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "EqDSP.h"

namespace veq
{
inline juce::String bandId (int band, const char* name) { return "b" + juce::String (band + 1) + name; }

inline const juce::StringArray& typeNames()    { static juce::StringArray a { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" }; return a; }
inline const juce::StringArray& slopeNames()   { static juce::StringArray a { "12 dB/oct", "24 dB/oct", "48 dB/oct" }; return a; }
inline const juce::StringArray& channelNames() { static juce::StringArray a { "Stereo", "Mid", "Side" }; return a; }
inline const juce::StringArray& characterNames() { static juce::StringArray a { "Clean", "Tube", "Punch", "Console" }; return a; }
inline const juce::StringArray& lfFreqNames()  { static juce::StringArray a { "20 Hz", "30 Hz", "60 Hz", "100 Hz" }; return a; }
inline constexpr float lfFreqs[] { 20, 30, 60, 100 };
inline const juce::StringArray& hfFreqNames()  { static juce::StringArray a { "3k", "4k", "5k", "8k", "10k", "12k", "16k" }; return a; }
inline constexpr float hfFreqs[] { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
inline const juce::StringArray& hfAttNames()   { static juce::StringArray a { "5k", "10k", "20k" }; return a; }
inline constexpr float hfAttFreqs[] { 5000, 10000, 20000 };

struct BandDefault { int type; float freq; float gain; float q; };
inline constexpr BandDefault bandDefaults[kNumBands] {
    { LowCut, 30, 0, 0.71f }, { LowShelf, 100, 0, 0.71f }, { Bell, 400, 0, 1.0f },
    { Bell, 2000, 0, 1.0f },  { HighShelf, 8000, 0, 0.71f }, { HighCut, 18000, 0, 0.71f } };

inline juce::String fmtHz (float v, int)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
}
inline juce::String fmtDb (float v, int) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto flt = [&] (const String& id, const String& name, NormalisableRange<float> r, float def, std::function<String (float, int)> f)
    { l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, AudioParameterFloatAttributes().withStringFromValueFunction (f))); };
    auto boo = [&] (const String& id, const String& name, bool def) { l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def)); };
    auto cho = [&] (const String& id, const String& name, const StringArray& c, int def) { l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, c, def)); };
    auto logR = [] (float lo, float hi) { NormalisableRange<float> r (lo, hi); r.setSkewForCentre (std::sqrt (lo * hi)); return r; };

    for (int b = 0; b < kNumBands; ++b)
    {
        const String n = "Band " + String (b + 1) + " ";
        const auto& d = bandDefaults[b];
        boo (bandId (b, "On"), n + "On", false);
        cho (bandId (b, "Type"), n + "Type", typeNames(), d.type);
        flt (bandId (b, "Freq"), n + "Freq", logR (10.0f, 22000.0f), d.freq, fmtHz);
        flt (bandId (b, "Gain"), n + "Gain", { -24.0f, 24.0f, 0.01f }, d.gain, fmtDb);
        flt (bandId (b, "Q"), n + "Q", logR (0.1f, 18.0f), d.q, [] (float v, int) { return String (v, 2); });
        cho (bandId (b, "Slope"), n + "Slope", slopeNames(), 1);
        cho (bandId (b, "Chan"), n + "Channel", channelNames(), 0);
        boo (bandId (b, "Dyn"), n + "Dynamic", false);
        flt (bandId (b, "Thr"), n + "Threshold", { -60.0f, 0.0f, 0.1f }, -24.0f, fmtDb);
        flt (bandId (b, "Range"), n + "Dyn Range", { -24.0f, 24.0f, 0.1f }, -6.0f, fmtDb);
    }

    cho ("character", "Character", characterNames(), 0);
    flt ("drive", "Drive", { 0.0f, 100.0f, 0.1f }, 30.0f, [] (float v, int) { return String (juce::roundToInt (v)) + "%"; });
    cho ("phase", "Phase", StringArray { "Natural", "Linear" }, 0);
    flt ("output", "Output", { -24.0f, 24.0f, 0.01f }, 0.0f, fmtDb);
    boo ("bypass", "Bypass", false);

    // tube program EQ
    boo ("pOn", "Program EQ On", false);
    cho ("pLfFreq", "Program LF Freq", lfFreqNames(), 2);
    flt ("pLfBoost", "Program LF Boost", { 0.0f, 10.0f, 0.1f }, 0.0f, [] (float v, int) { return String (v, 1); });
    flt ("pLfAtten", "Program LF Atten", { 0.0f, 10.0f, 0.1f }, 0.0f, [] (float v, int) { return String (v, 1); });
    cho ("pHfFreq", "Program HF Freq", hfFreqNames(), 4);
    flt ("pHfBoost", "Program HF Boost", { 0.0f, 10.0f, 0.1f }, 0.0f, [] (float v, int) { return String (v, 1); });
    flt ("pHfBw", "Program HF Bandwidth", { 0.0f, 10.0f, 0.1f }, 5.0f, [] (float v, int) { return String (v, 1); });
    cho ("pHfAttFreq", "Program HF Atten Freq", hfAttNames(), 1);
    flt ("pHfAtten", "Program HF Atten", { 0.0f, 10.0f, 0.1f }, 0.0f, [] (float v, int) { return String (v, 1); });
    return l;
}
} // namespace veq
