#pragma once
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <array>
#include <vector>

// =====================================================================
//  Vega Productions EQ — filter math shared by the audio engine,
//  the linear‑phase FIR designer and the on‑screen curve.
// =====================================================================
namespace veq
{
constexpr int kNumBands = 6;
inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)  { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

enum BandType  { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass };
enum Channel   { Stereo = 0, Mid, Side };
enum Character { Clean = 0, Tube, Punch, Console };

// ---------------------------------------------------------------- biquad
struct Coeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    static Coeffs norm (double b0, double b1, double b2, double a0, double a1, double a2) { return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 }; }
    static double w0 (double f, double fs) { return juce::MathConstants<double>::twoPi * juce::jlimit (5.0, fs * 0.495, f) / fs; }

    static Coeffs peak (double fs, double f, double q, double g)
    {
        const double A = std::pow (10.0, g / 40.0), w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q);
        return norm (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    static Coeffs lowShelf (double fs, double f, double q, double g)
    {
        const double A = std::pow (10.0, g / 40.0), w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        return norm (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                     (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    static Coeffs highShelf (double fs, double f, double q, double g)
    {
        const double A = std::pow (10.0, g / 40.0), w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        return norm (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                     (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
    static Coeffs highPass (double fs, double f, double q)
    {
        const double w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q);
        return norm ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    static Coeffs lowPass (double fs, double f, double q)
    {
        const double w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q);
        return norm ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    static Coeffs notch (double fs, double f, double q)
    {
        const double w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q);
        return norm (1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }
    static Coeffs bandPass (double fs, double f, double q)   // 0 dB peak gain
    {
        const double w = w0 (f, fs), c = std::cos (w), al = std::sin (w) / (2 * q);
        return norm (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
    static Coeffs onePoleHP (double fs, double f)            // first‑order HP as biquad
    {
        const double k = std::tan (juce::MathConstants<double>::pi * juce::jlimit (5.0, fs * 0.495, f) / fs);
        return norm (1, -1, 0, 1 + k, k - 1, 0);
    }
    static Coeffs onePoleLP (double fs, double f)
    {
        const double k = std::tan (juce::MathConstants<double>::pi * juce::jlimit (5.0, fs * 0.495, f) / fs);
        return norm (k, k, 0, 1 + k, k - 1, 0);
    }

    double magnitude (double f, double fs) const
    {
        const double w = juce::MathConstants<double>::twoPi * f / fs;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        return std::abs ((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
    }
};

struct Biquad
{
    Coeffs c; double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline float process (float xf)
    {
        const double x = xf, y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};

// ---------------------------------------------------------------- band settings -> biquad cascade
struct BandSettings
{
    bool on = false; int type = Bell; float freq = 1000, gain = 0, q = 0.7f;
    int slope = 1;      // cut slopes: 0 = 12, 1 = 24, 2 = 48 dB/oct (also 6 dB with slope -1)
    int channel = Stereo;
    bool dynamic = false; float threshold = -24, range = -6;
    bool operator== (const BandSettings& o) const
    {
        return on == o.on && type == o.type && freq == o.freq && gain == o.gain && q == o.q && slope == o.slope
            && channel == o.channel && dynamic == o.dynamic && threshold == o.threshold && range == o.range;
    }
    bool operator!= (const BandSettings& o) const { return ! (*this == o); }
    bool canBeDynamic() const { return type == Bell || type == LowShelf || type == HighShelf; }
};

struct Cascade
{
    std::array<Coeffs, 4> c; int n = 0;
    double magnitude (double f, double fs) const { double m = 1; for (int i = 0; i < n; ++i) m *= c[(size_t) i].magnitude (f, fs); return m; }
};

/** Character‑dependent curve shaping. gainDb lets dynamic bands override the static gain. */
inline Cascade makeCascade (const BandSettings& b, double fs, int character, float gainDb)
{
    Cascade out;
    double q = b.q;
    if (b.type == Bell)
    {
        if (character == Punch)   q = q * (0.55 + 0.09 * std::abs (gainDb));   // proportional Q: narrower as you push
        if (character == Console) q = q * 0.8;                                   // broader, musical console bells
        if (character == Tube)    q = q * 0.65;                                  // wide passive‑style curves
        q = juce::jlimit (0.05, 40.0, q);
    }
    switch (b.type)
    {
        case Bell:      out.c[0] = Coeffs::peak (fs, b.freq, q, gainDb); out.n = 1; break;
        case LowShelf:  out.c[0] = Coeffs::lowShelf (fs, b.freq, juce::jlimit (0.3, 2.0, q), gainDb); out.n = 1; break;
        case HighShelf: out.c[0] = Coeffs::highShelf (fs, b.freq, juce::jlimit (0.3, 2.0, q), gainDb); out.n = 1; break;
        case Notch:     out.c[0] = Coeffs::notch (fs, b.freq, q); out.n = 1; break;
        case BandPass:  out.c[0] = Coeffs::bandPass (fs, b.freq, q); out.n = 1; break;
        case LowCut:
        case HighCut:
        {
            const bool hp = b.type == LowCut;
            // Butterworth sections; the Q knob adds resonance to the last section
            static const double q2[]  { 0.7071 };
            static const double q4[]  { 0.5412, 1.3066 };
            static const double q8[]  { 0.5098, 0.6013, 0.9000, 2.5629 };
            const double* qs = b.slope <= 0 ? q2 : (b.slope == 1 ? q4 : q8);
            const int n = b.slope <= 0 ? 1 : (b.slope == 1 ? 2 : 4);
            const double res = juce::jlimit (0.5, 4.0, (double) b.q / 0.7071);
            for (int i = 0; i < n; ++i)
            {
                const double qq = qs[i] * (i == n - 1 ? res : 1.0);
                out.c[(size_t) i] = hp ? Coeffs::highPass (fs, b.freq, qq) : Coeffs::lowPass (fs, b.freq, qq);
            }
            out.n = n;
            break;
        }
        default: break;
    }
    return out;
}

// ---------------------------------------------------------------- tube "program" EQ (passive EQP‑style)
struct ProgramSettings
{
    bool on = false;
    float lfFreq = 60, lfBoost = 0, lfAtten = 0;          // boost/atten 0..10
    float hfFreq = 10000, hfBoost = 0, hfBw = 5;           // bw 0 (sharp) .. 10 (broad)
    float hfAttFreq = 10000, hfAtten = 0;
    bool operator== (const ProgramSettings& o) const
    {
        return on == o.on && lfFreq == o.lfFreq && lfBoost == o.lfBoost && lfAtten == o.lfAtten && hfFreq == o.hfFreq
            && hfBoost == o.hfBoost && hfBw == o.hfBw && hfAttFreq == o.hfAttFreq && hfAtten == o.hfAtten;
    }
};

inline Cascade makeProgramCascade (const ProgramSettings& p, double fs)
{
    Cascade out;
    if (! p.on) return out;
    // Low boost: broad resonant shelf; low atten: shelf cut placed a little higher, so using both
    // gives the famous "boost and cut at the same time" bump‑and‑dip.
    if (p.lfBoost > 0.0f) out.c[(size_t) out.n++] = Coeffs::lowShelf (fs, p.lfFreq, 0.55, p.lfBoost * 1.35);
    if (p.lfAtten > 0.0f) out.c[(size_t) out.n++] = Coeffs::lowShelf (fs, p.lfFreq * 2.4, 0.5, -p.lfAtten * 1.75);
    if (p.hfBoost > 0.0f)
    {
        const double q = juce::jmap ((double) p.hfBw, 0.0, 10.0, 2.2, 0.35);
        out.c[(size_t) out.n++] = Coeffs::peak (fs, p.hfFreq, q, p.hfBoost * 1.6);
    }
    if (p.hfAtten > 0.0f) out.c[(size_t) out.n++] = Coeffs::highShelf (fs, p.hfAttFreq, 0.6, -p.hfAtten * 2.0);
    return out;
}

// ---------------------------------------------------------------- linear‑phase FIR from a magnitude response
inline void designLinearPhaseFir (const std::function<double (double)>& magnitudeAtHz, double fs, int fftOrder, float* out)
{
    const int N = 1 << fftOrder;
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) N * 2, 0.0f);
    for (int k = 0; k <= N / 2; ++k) data[(size_t) (2 * k)] = (float) magnitudeAtHz ((double) k * fs / N);
    // Calibrate the inverse FFT's scaling with a flat (all‑ones) spectrum, whose ideal
    // impulse peak is exactly 1. (Don't use H(0): it is 0 for low‑cut filters.)
    std::vector<float> ref ((size_t) N * 2, 0.0f);
    for (int k = 0; k <= N / 2; ++k) ref[(size_t) (2 * k)] = 1.0f;
    fft.performRealOnlyInverseTransform (ref.data());
    const float scale = std::abs (ref[0]) > 1.0e-12f ? 1.0f / ref[0] : 1.0f;
    fft.performRealOnlyInverseTransform (data.data());

    for (int n = 0; n < N; ++n)
    {
        const int src = (n + N / 2) % N;
        const double w = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * n / N)
                              + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * n / N);
        out[n] = data[(size_t) src] * scale * (float) w;
    }
}

// ---------------------------------------------------------------- analog character (saturation)
struct Saturator
{
    Biquad preL, preR, postL, postR;      // console mode: low‑frequency emphasis around the non‑linearity
    float dcX[2] {}, dcY[2] {}, dcCoef = 0.9995f;

    void prepare (double fs)
    {
        auto pre = Coeffs::lowShelf (fs, 120.0, 0.7, 3.0), post = Coeffs::lowShelf (fs, 120.0, 0.7, -3.0);
        preL.c = preR.c = pre; postL.c = postR.c = post;
        dcCoef = (float) std::exp (-juce::MathConstants<double>::twoPi * 8.0 / fs);
        reset();
    }
    void reset() { preL.reset(); preR.reset(); postL.reset(); postR.reset(); dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0; }

    static inline float tube (float u)       // asymmetric -> 2nd harmonic
    {
        constexpr float b = 0.25f;
        static const float tb = std::tanh (b), norm = 1.0f / (1.0f - tb * tb);
        return (std::tanh (u + b) - tb) * norm;
    }
    static inline float punch (float u)      // firmer, odd‑order transformer edge
    {
        return u / std::sqrt (1.0f + u * u * 0.6f) * 1.0f;
    }
    static inline float console (float u)    // soft knee, gentle
    {
        return std::tanh (u * 0.9f) / 0.9f;
    }

    void process (float* l, float* r, int n, int character, float amount /*0..1*/)
    {
        if (character == Clean || amount <= 0.0f) return;
        const float drive = 1.0f + amount * 5.0f, inv = 1.0f / drive;
        float* ch[2] = { l, r };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                float x = ch[c][i];
                if (character == Console) x = (c == 0 ? preL : preR).process (x);
                float y = character == Tube ? tube (x * drive) : character == Punch ? punch (x * drive) : console (x * drive);
                y *= inv;
                if (character == Console) y = (c == 0 ? postL : postR).process (y);
                // DC blocker (asymmetric curves)
                const float d = y - dcX[c] + dcCoef * dcY[c];
                dcX[c] = y; dcY[c] = d;
                ch[c][i] = d;
            }
    }
};
} // namespace veq
