#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "VegaUI.h"

namespace veq
{
inline const juce::Colour bandColours[kNumBands] { juce::Colour (0xff7ff7f2), juce::Colour (0xffd9b45a), juce::Colour (0xff8fd18a),
                                                   juce::Colour (0xffe58fb6), juce::Colour (0xffa99cf0), juce::Colour (0xfff0a35e) };
}

// Interactive EQ display: analyser + curves + draggable band nodes
class EqGraph : public juce::Component
{
public:
    explicit EqGraph (VegaEqProcessor&);
    void refresh();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;
    std::function<void (int)> onSelect;

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (30.0f, 10.0f).withTrimmedBottom (14.0f); }
    float xFor (double f) const;
    double fFor (float x) const;
    float yFor (double db) const;
    double dbFor (float y) const;
    juce::Point<float> nodePos (int b) const;
    int hitNode (juce::Point<float> p) const;
    void setParam (int band, const char* name, float value);
    void analyse (MonoFifo& fifo, std::vector<float>& ring, int& pos, std::vector<float>& disp);

    VegaEqProcessor& proc;
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> preRing, postRing, preDisp, postDisp, work;
    int prePos = 0, postPos = 0;
    int dragBand = -1, hoverBand = -1;
};

class VegaEqEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VegaEqEditor (VegaEqProcessor&);
    ~VegaEqEditor() override;
    void paint (juce::Graphics&) override { }
    void resized() override;
    static constexpr int W = 1440, H = 700;

private:
    class Panel;
    void timerCallback() override;
    void applyScale();
    VegaEqProcessor& proc;
    VegaLookAndFeel lnf;
    std::unique_ptr<Panel> panel;
};
