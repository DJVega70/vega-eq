// Offline checks: transparency when flat, measured band gain matches the curve,
// linear‑phase null, dynamic band reacts, no NaNs in any mode.
#include "../Source/PluginProcessor.h"
#include <iostream>

static VegaEqProcessor* P; static double FS = 48000; static int BLK = 512; static int fails = 0;
static void set (const juce::String& id, float v) { auto* p = P->apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); }
static void check (bool ok, const juce::String& what) { std::cout << (ok ? "PASS " : "FAIL ") << what << "\n"; if (! ok) ++fails; }

// run a sine through, return output RMS gain in dB (after settling)
static float sineGainDb (double f, float amp = 0.1f, int seconds = 1)
{
    P->reset(); juce::AudioBuffer<float> b (2, BLK); juce::MidiBuffer m; double ph = 0; double si = 0, so = 0; bool bad = false;
    const int blocks = (int) (seconds * FS / BLK);
    for (int k = 0; k < blocks; ++k)
    {
        for (int i = 0; i < BLK; ++i) { const float x = amp * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * f / FS; b.setSample (0, i, x); b.setSample (1, i, x); if (k > blocks / 2) si += x * x; }
        P->processBlock (b, m);
        for (int i = 0; i < BLK; ++i) { const float y = b.getSample (0, i); if (! std::isfinite (y)) bad = true; if (k > blocks / 2) so += y * y; }
    }
    if (bad) return 999.0f;
    return 10.0f * (float) std::log10 (so / si);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    std::unique_ptr<juce::AudioProcessor> base (createPluginFilter());
    P = dynamic_cast<VegaEqProcessor*> (base.get());
    P->setPlayConfigDetails (2, 2, FS, BLK); P->prepareToPlay (FS, BLK);

    check (std::abs (sineGainDb (1000)) < 0.05f, "flat EQ is transparent (1 kHz)");
    check (std::abs (sineGainDb (60)) < 0.05f, "flat EQ is transparent (60 Hz)");

    set ("b3On", 1); set ("b3Type", 0); set ("b3Freq", 1000); set ("b3Gain", 6); set ("b3Q", 1);
    const float g = sineGainDb (1000);
    check (std::abs (g - 6.0f) < 0.2f, "bell +6 dB at 1 kHz measures " + juce::String (g, 2));
    set ("drive", 0); set ("character", 2);   // Punch: proportional Q -> same peak gain, different width
    const float gp = sineGainDb (1000);
    check (std::abs (gp - 6.0f) < 0.4f, "Punch bell peak still ~+6 dB (" + juce::String (gp, 2) + ")");
    set ("character", 0);

    set ("b1On", 1); set ("b1Type", 3); set ("b1Freq", 100); set ("b1Slope", 1);
    const float hp = sineGainDb (25);
    check (hp < -20.0f, "24 dB/oct low cut @100 Hz attenuates 25 Hz by " + juce::String (hp, 1) + " dB");

    {   // FIR designer sanity
        std::vector<float> h (4096); veq::designLinearPhaseFir ([] (double) { return 1.0; }, FS, 12, h.data());
        double sum = 0; float mx = 0; int at = 0; for (int i = 0; i < 4096; ++i) { sum += h[(size_t) i]; if (std::abs (h[(size_t) i]) > mx) { mx = std::abs (h[(size_t) i]); at = i; } }
        std::cout << "  FIR flat: sum " << sum << " peak " << mx << " at " << at << "\n";
    }
    {   // raw juce Convolution sanity
        juce::dsp::Convolution cv; cv.prepare ({ FS, (juce::uint32) BLK, 2 });
        { juce::AudioBuffer<float> ir0 (2, 4096); ir0.clear(); ir0.setSample (0, 0, 1.0f); ir0.setSample (1, 0, 1.0f);
          cv.loadImpulseResponse (std::move (ir0), FS, juce::dsp::Convolution::Stereo::yes, juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no); }
        cv.prepare ({ FS, (juce::uint32) BLK, 2 });
        juce::AudioBuffer<float> ir (2, 4096); ir.clear(); ir.setSample (0, 0, 1.0f); ir.setSample (1, 0, 1.0f);
        cv.loadImpulseResponse (std::move (ir), FS, juce::dsp::Convolution::Stereo::yes, juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        juce::Thread::sleep (200);
        double e = 0;
        for (int k = 0; k < 50; ++k)
        {
            juce::AudioBuffer<float> b (2, BLK); for (int i = 0; i < BLK; ++i) { b.setSample (0, i, 0.5f); b.setSample (1, i, 0.5f); }
            juce::dsp::AudioBlock<float> blk (b); cv.process (juce::dsp::ProcessContextReplacing<float> (blk));
            if (k > 40) e += b.getMagnitude (0, 0, BLK);
        }
        std::cout << "  raw convolution energy " << e << "\n";
    }
    // linear phase: same magnitude
    set ("phase", 1); P->prepareToPlay (FS, BLK); juce::Thread::sleep (300);
    const float gl = sineGainDb (1000, 0.1f, 2);
    check (std::abs (gl - 6.0f) < 0.3f, "linear-phase bell measures " + juce::String (gl, 2) + " dB");
    set ("phase", 0); set ("b1On", 0);

    // dynamic: loud signal triggers the -6 dB range, quiet one doesn't
    set ("b3Gain", 0); set ("b3Dyn", 1); set ("b3Thr", -30); set ("b3Range", -6);
    const float quiet = sineGainDb (1000, 0.003f);   // ~ -50 dBFS
    const float loud  = sineGainDb (1000, 0.5f);
    check (std::abs (quiet) < 0.5f && loud < -4.0f, "dynamic band: quiet " + juce::String (quiet, 2) + " dB, loud " + juce::String (loud, 2) + " dB");
    set ("b3Dyn", 0);

    // program EQ + every character, no NaN
    set ("pOn", 1); set ("pLfBoost", 6); set ("pLfAtten", 4); set ("pHfBoost", 5); set ("pHfAtten", 3);
    bool ok = true;
    for (int c = 0; c < 4; ++c) { set ("character", (float) c); set ("drive", 80); ok &= sineGainDb (80, 0.7f) < 100.0f; ok &= sineGainDb (5000, 0.7f) < 100.0f; }
    check (ok, "all characters + program EQ produce finite output");

    std::cout << (fails == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string (fails)) << std::endl;
    return fails;
}
