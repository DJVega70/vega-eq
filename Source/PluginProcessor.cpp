#include "PluginProcessor.h"
#include "PluginEditor.h"
using namespace veq;

VegaEqProcessor::VegaEqProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VegaEQ", createLayout())
{
    const char* names[] { "On", "Type", "Freq", "Gain", "Q", "Slope", "Chan", "Dyn", "Thr", "Range" };
    for (int b = 0; b < kNumBands; ++b)
    {
        for (int i = 0; i < 10; ++i) bp[(size_t) b][(size_t) i] = apvts.getRawParameterValue (bandId (b, names[i]));
        liveGainDb[(size_t) b].store (0.0f);
    }
    startTimerHz (30);
}
VegaEqProcessor::~VegaEqProcessor() { stopTimer(); }

bool VegaEqProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

BandSettings VegaEqProcessor::readBand (int b) const
{
    const auto& a = bp[(size_t) b];
    BandSettings s;
    s.on = a[0]->load() > 0.5f; s.type = juce::roundToInt (a[1]->load()); s.freq = a[2]->load(); s.gain = a[3]->load();
    s.q = a[4]->load(); s.slope = juce::roundToInt (a[5]->load()); s.channel = juce::roundToInt (a[6]->load());
    s.dynamic = a[7]->load() > 0.5f; s.threshold = a[8]->load(); s.range = a[9]->load();
    return s;
}

ProgramSettings VegaEqProcessor::readProgram() const
{
    ProgramSettings s;
    s.on = p ("pOn") > 0.5f;
    s.lfFreq = lfFreqs[juce::jlimit (0, 3, (int) p ("pLfFreq"))]; s.lfBoost = p ("pLfBoost"); s.lfAtten = p ("pLfAtten");
    s.hfFreq = hfFreqs[juce::jlimit (0, 6, (int) p ("pHfFreq"))]; s.hfBoost = p ("pHfBoost"); s.hfBw = p ("pHfBw");
    s.hfAttFreq = hfAttFreqs[juce::jlimit (0, 2, (int) p ("pHfAttFreq"))]; s.hfAtten = p ("pHfAtten");
    return s;
}

void VegaEqProcessor::prepareToPlay (double sr, int block)
{
    sampleRateHz = sr;
    maxChunk = juce::jmax (32, block);
    firOrder = sr <= 50000.0 ? 12 : (sr <= 100000.0 ? 13 : 14);
    for (auto& b : bands) { b.reset(); b.appliedCharacter = -1; }
    for (auto& c : prog) for (auto& q : c) q.reset();
    progValid = false;

    os = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    os->initProcessing ((size_t) maxChunk);
    sat.prepare (sr * 2.0);

    linearEq.prepare ({ sr, (juce::uint32) maxChunk, 2 });
    rebuildIr();

    for (auto& r : dryRing) r.assign (1 << 16, 0.0f);
    dryMask = (1 << 16) - 1; dryPos = 0;
    scratch.setSize (2, maxChunk);
    outS.reset (sr, 0.03); outS.setCurrentAndTargetValue (dbToGain (p ("output")));
    bypassS.reset (sr, 0.02); bypassS.setCurrentAndTargetValue (p ("bypass") > 0.5f ? 1.0f : 0.0f);
    attCoef = std::exp (-1.0f / (0.002f * (float) sr));
    relCoef = std::exp (-1.0f / (0.120f * (float) sr));
    setLatencySamples (computeLatency (p ("phase") > 0.5f));
}

int VegaEqProcessor::computeLatency (bool linear) const
{
    int lat = os ? (int) std::round (os->getLatencyInSamples()) : 0;
    if (linear) lat += (1 << firOrder) / 2;
    return lat;
}

void VegaEqProcessor::rebuildIr()
{
    const double fs = sampleRateHz;
    const int ch = character();
    std::array<Cascade, kNumBands> cas;
    for (int b = 0; b < kNumBands; ++b)
    {
        irBands[(size_t) b] = readBand (b);
        const auto& s = irBands[(size_t) b];
        if (s.on && ! (s.dynamic && s.canBeDynamic())) cas[(size_t) b] = makeCascade (s, fs, ch, s.gain);
    }
    irProg = readProgram(); irChar = ch;
    const auto pc = makeProgramCascade (irProg, fs);
    const int N = 1 << firOrder;
    juce::AudioBuffer<float> ir (2, N);
    for (int c = 0; c < 2; ++c)
        designLinearPhaseFir ([&] (double f)
        {
            double m = pc.magnitude (f, fs);
            for (int b = 0; b < kNumBands; ++b)
            {
                const auto& s = irBands[(size_t) b];
                if (cas[(size_t) b].n == 0) continue;
                if (s.channel == Stereo || (s.channel == Mid && c == 0) || (s.channel == Side && c == 1))
                    m *= cas[(size_t) b].magnitude (f, fs);
            }
            return m;
        }, fs, firOrder, ir.getWritePointer (c));
    linearEq.loadImpulseResponse (std::move (ir), fs, juce::dsp::Convolution::Stereo::yes,
                                  juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    irValid = true; ticksSinceIr = 0;
}

void VegaEqProcessor::timerCallback()
{
    ++ticksSinceIr;
    const bool linear = p ("phase") > 0.5f;
    const int wanted = computeLatency (linear);
    if (wanted != getLatencySamples()) setLatencySamples (wanted);
    if (irValid && linear && ticksSinceIr >= 3)
    {
        bool changed = character() != irChar || ! (readProgram() == irProg);
        for (int b = 0; b < kNumBands && ! changed; ++b) changed = readBand (b) != irBands[(size_t) b];
        if (changed) rebuildIr();
    }
}

void VegaEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    if (buffer.getNumChannels() < 2) return;
    float* L = buffer.getWritePointer (0); float* R = buffer.getWritePointer (1);
    for (int s = 0, t = buffer.getNumSamples(); s < t; )
    {
        const int n = juce::jmin (maxChunk, t - s);
        processChunk (L + s, R + s, n);
        s += n;
    }
}

void VegaEqProcessor::processChunk (float* L, float* R, int n)
{
    const bool linear = p ("phase") > 0.5f;
    const int latency = computeLatency (linear);
    const int chr = character();
    const double fs = sampleRateHz;

    // latency‑aligned dry (bypass) + pre‑EQ analyser feed
    float* dL = scratch.getWritePointer (0); float* dR = scratch.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        dryRing[0][(size_t) dryPos] = L[i]; dryRing[1][(size_t) dryPos] = R[i];
        const int rd = (dryPos - latency) & dryMask;
        dL[i] = dryRing[0][(size_t) rd]; dR[i] = dryRing[1][(size_t) rd];
        dryPos = (dryPos + 1) & dryMask;
    }
    {
        float mono[2048];
        for (int s = 0; s < n; s += 2048)
        {
            const int m = juce::jmin (2048, n - s);
            for (int i = 0; i < m; ++i) mono[i] = 0.5f * (L[s + i] + R[s + i]);
            preFifo.push (mono, m);
        }
    }

    // L/R -> M/S
    for (int i = 0; i < n; ++i) { const float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]); L[i] = m; R[i] = s; }
    float* ms[2] = { L, R };

    if (linear)
    {
        juce::dsp::AudioBlock<float> blk (ms, 2, (size_t) n);
        linearEq.process (juce::dsp::ProcessContextReplacing<float> (blk));
    }

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& st = bands[(size_t) b];
        const auto s = readBand (b);
        const bool dyn = s.dynamic && s.canBeDynamic();
        if (! s.on || (linear && ! dyn)) { liveGainDb[(size_t) b].store (s.gain); st.applied.on = false; continue; }

        const bool useM = s.channel != Side, useS = s.channel != Mid;
        if (! dyn)
        {
            if (s != st.applied || st.appliedCharacter != chr || st.appliedGain != s.gain)
            {
                if (! st.applied.on || st.applied.type != s.type || st.applied.slope != s.slope) st.reset();
                st.setCascade (makeCascade (s, fs, chr, s.gain));
                st.applied = s; st.appliedCharacter = chr; st.appliedGain = s.gain;
            }
            for (int c = 0; c < 2; ++c)
            {
                if ((c == 0 && ! useM) || (c == 1 && ! useS)) continue;
                float* x = ms[c];
                for (int k = 0; k < st.nSections; ++k)
                {
                    auto& f = st.f[c][(size_t) k];
                    for (int i = 0; i < n; ++i) x[i] = f.process (x[i]);
                }
            }
            liveGainDb[(size_t) b].store (s.gain);
            continue;
        }

        // ---- dynamic band
        if (s != st.applied || st.appliedCharacter != chr)
        {
            if (! st.applied.on || st.applied.type != s.type) st.reset();
            Coeffs dc = s.type == LowShelf ? Coeffs::lowPass (fs, s.freq, 0.7071)
                      : s.type == HighShelf ? Coeffs::highPass (fs, s.freq, 0.7071)
                      : Coeffs::bandPass (fs, s.freq, juce::jmax (0.5f, s.q));
            st.det[0].c = st.det[1].c = dc;
            st.applied = s; st.appliedCharacter = chr; st.appliedGain = 1.0e9f;   // force coefficient update
        }
        for (int i0 = 0; i0 < n; i0 += 16)
        {
            const int m = juce::jmin (16, n - i0);
            for (int i = i0; i < i0 + m; ++i)
            {
                const float a = std::abs (useM ? st.det[0].process (ms[0][i]) : 0.0f) + std::abs (useS ? st.det[1].process (ms[1][i]) : 0.0f);
                st.env = a > st.env ? a + (st.env - a) * attCoef : st.env * relCoef;
            }
            const float over = gainToDb (st.env) - s.threshold;
            const float target = juce::jlimit (0.0f, 1.0f, over / 12.0f);
            st.amount += (target - st.amount) * (target > st.amount ? 0.5f : 0.08f);
            const float g = s.gain + s.range * st.amount;
            if (std::abs (g - st.appliedGain) > 0.03f)
            {
                st.setCascade (makeCascade (s, fs, chr, g));
                st.appliedGain = g;
            }
            for (int c = 0; c < 2; ++c)
            {
                if ((c == 0 && ! useM) || (c == 1 && ! useS)) continue;
                for (int k = 0; k < st.nSections; ++k)
                {
                    auto& f = st.f[c][(size_t) k];
                    for (int i = i0; i < i0 + m; ++i) ms[c][i] = f.process (ms[c][i]);
                }
            }
        }
        liveGainDb[(size_t) b].store (st.appliedGain);
    }

    // tube program EQ (natural phase mode; in linear mode it lives in the FIR)
    if (! linear)
    {
        const auto ps = readProgram();
        if (! progValid || ! (ps == progApplied))
        {
            const auto c = makeProgramCascade (ps, fs);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < c.n; ++i) prog[ch][(size_t) i].c = c.c[(size_t) i];
            if (progN != c.n) for (auto& a : prog) for (auto& q : a) q.reset();
            progN = c.n; progApplied = ps; progValid = true;
        }
        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < progN; ++k)
                for (int i = 0; i < n; ++i) ms[ch][i] = prog[ch][(size_t) k].process (ms[ch][i]);
    }

    // M/S -> L/R
    for (int i = 0; i < n; ++i) { const float m = L[i], s = R[i]; L[i] = m + s; R[i] = m - s; }

    // analog character (2x oversampled; always run so latency is constant)
    {
        float* lr[2] = { L, R };
        juce::dsp::AudioBlock<float> blk (lr, 2, (size_t) n);
        auto up = os->processSamplesUp (blk);
        sat.process (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples(), chr, p ("drive") * 0.01f);
        os->processSamplesDown (blk);
    }

    // output + bypass
    outS.setTargetValue (dbToGain (p ("output")));
    bypassS.setTargetValue (p ("bypass") > 0.5f ? 1.0f : 0.0f);
    float pl = 0, pr = 0;
    for (int i = 0; i < n; ++i)
    {
        const float g = outS.getNextValue(), b = bypassS.getNextValue();
        L[i] *= g; R[i] *= g;
        if (b > 0.0f) { L[i] += b * (dL[i] - L[i]); R[i] += b * (dR[i] - R[i]); }
        pl = std::max (pl, std::abs (L[i])); pr = std::max (pr, std::abs (R[i]));
    }
    auto amax = [] (std::atomic<float>& a, float v) { float prev = a.load(); while (prev < v && ! a.compare_exchange_weak (prev, v)) {} };
    amax (outPeakL, pl); amax (outPeakR, pr);
    {
        float mono[2048];
        for (int s = 0; s < n; s += 2048)
        {
            const int m = juce::jmin (2048, n - s);
            for (int i = 0; i < m; ++i) mono[i] = 0.5f * (L[s + i] + R[s + i]);
            postFifo.push (mono, m);
        }
    }
}

void VegaEqProcessor::getStateInformation (juce::MemoryBlock& d)
{
    auto st = apvts.copyState();
    st.setProperty ("uiScale", (double) uiScale.load(), nullptr);
    if (auto x = st.createXml()) copyXmlToBinary (*x, d);
}
void VegaEqProcessor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
        if (x->hasTagName (apvts.state.getType()))
        {
            auto t = juce::ValueTree::fromXml (*x);
            uiScale.store ((float) (double) t.getProperty ("uiScale", 1.0));
            apvts.replaceState (t);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VegaEqProcessor(); }
