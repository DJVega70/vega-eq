#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Params.h"

// lock‑free SPSC mono sample FIFO (analyser feed)
class MonoFifo
{
public:
    explicit MonoFifo (int size) : fifo (size), buf ((size_t) size) {}
    void push (const float* x, int n)
    {
        int s1, n1, s2, n2; fifo.prepareToWrite (n, s1, n1, s2, n2);
        std::copy (x, x + n1, buf.begin() + s1); std::copy (x + n1, x + n1 + n2, buf.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }
    int pull (float* x, int maxN)
    {
        int s1, n1, s2, n2; fifo.prepareToRead (std::min (maxN, fifo.getNumReady()), s1, n1, s2, n2);
        std::copy (buf.begin() + s1, buf.begin() + s1 + n1, x); std::copy (buf.begin() + s2, buf.begin() + s2 + n2, x + n1);
        fifo.finishedRead (n1 + n2); return n1 + n2;
    }
    int ready() const { return fifo.getNumReady(); }
private:
    juce::AbstractFifo fifo; std::vector<float> buf;
};

class VegaEqProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    VegaEqProcessor();
    ~VegaEqProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Vega Productions EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }

    juce::AudioProcessorValueTreeState apvts;

    // ---- UI feeds
    veq::BandSettings readBand (int b) const;
    veq::ProgramSettings readProgram() const;
    int character() const { return juce::roundToInt (apvts.getRawParameterValue ("character")->load()); }
    double getCurrentSampleRate() const { return sampleRateHz; }
    std::array<std::atomic<float>, veq::kNumBands> liveGainDb;   // current gain incl. dynamics
    std::atomic<float> outPeakL { 0 }, outPeakR { 0 };
    MonoFifo preFifo { 32768 }, postFifo { 32768 };
    std::atomic<int> selectedBand { 0 };      // UI state
    std::atomic<float> uiScale { 1.0f };

private:
    void timerCallback() override;
    void processChunk (float* L, float* R, int n);
    int computeLatency (bool linear) const;
    void rebuildIr();
    float p (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    struct BandState
    {
        veq::BandSettings applied; int appliedCharacter = -1; float appliedGain = 0;
        std::array<veq::Biquad, 4> f[2];    // [mid/side][sections]
        veq::Biquad det[2];
        int nSections = 0;
        float env = 0, amount = 0;
        void setCascade (const veq::Cascade& c) { nSections = c.n; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < c.n; ++i) f[ch][(size_t) i].c = c.c[(size_t) i]; }
        void reset() { for (auto& a : f) for (auto& q : a) q.reset(); det[0].reset(); det[1].reset(); env = amount = 0; }
    };

    std::array<std::array<std::atomic<float>*, 10>, veq::kNumBands> bp {};
    double sampleRateHz = 44100.0;
    int maxChunk = 512, firOrder = 12;
    std::array<BandState, veq::kNumBands> bands;
    std::array<veq::Biquad, 4> prog[2]; int progN = 0; veq::ProgramSettings progApplied; bool progValid = false;
    juce::dsp::Convolution linearEq;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    veq::Saturator sat;
    std::vector<float> dryRing[2]; int dryPos = 0, dryMask = 0;
    juce::AudioBuffer<float> scratch;
    juce::SmoothedValue<float> outS, bypassS;
    float attCoef = 0.9f, relCoef = 0.999f;

    // linear‑phase IR bookkeeping (message thread)
    std::array<veq::BandSettings, veq::kNumBands> irBands; veq::ProgramSettings irProg; int irChar = -1;
    bool irValid = false; int ticksSinceIr = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VegaEqProcessor)
};
