#include "PluginEditor.h"
using namespace veq;

namespace
{
    constexpr double fMin = 20.0, fMax = 20000.0;
    constexpr double dbRange = 18.0;
    juce::String hz (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz"; }
    juce::String db (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB"; }
}

//==============================================================================
EqGraph::EqGraph (VegaEqProcessor& p) : proc (p)
{
    preRing.assign (fftSize, 0.0f); postRing.assign (fftSize, 0.0f);
    preDisp.assign (fftSize / 2 + 1, -120.0f); postDisp.assign (fftSize / 2 + 1, -120.0f);
    work.assign (fftSize * 2, 0.0f);
    setRepaintsOnMouseActivity (false);
}

float EqGraph::xFor (double f) const { auto r = plot(); return r.getX() + (float) (std::log (f / fMin) / std::log (fMax / fMin)) * r.getWidth(); }
double EqGraph::fFor (float x) const { auto r = plot(); return fMin * std::pow (fMax / fMin, juce::jlimit (0.0, 1.0, (double) (x - r.getX()) / r.getWidth())); }
float EqGraph::yFor (double d) const { auto r = plot(); return juce::jmap ((float) juce::jlimit (-dbRange - 6, dbRange + 6, d), (float) -dbRange, (float) dbRange, r.getBottom(), r.getY()); }
double EqGraph::dbFor (float y) const { auto r = plot(); return juce::jmap ((double) y, (double) r.getBottom(), (double) r.getY(), -dbRange, dbRange); }

juce::Point<float> EqGraph::nodePos (int b) const
{
    const auto s = proc.readBand (b);
    const bool gainType = s.type == Bell || s.type == LowShelf || s.type == HighShelf;
    return { xFor (juce::jlimit (fMin, fMax, (double) s.freq)), yFor (gainType ? s.gain : 0.0) };
}

int EqGraph::hitNode (juce::Point<float> p) const
{
    int best = -1; float bestD = 14.0f;
    for (int b = 0; b < kNumBands; ++b)
    {
        const float d = nodePos (b).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}

void EqGraph::setParam (int band, const char* name, float value)
{
    if (auto* prm = proc.apvts.getParameter (bandId (band, name)))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

void EqGraph::analyse (MonoFifo& fifo, std::vector<float>& ring, int& pos, std::vector<float>& disp)
{
    float tmp[2048]; bool got = false;
    while (fifo.ready() > 0)
    {
        const int n = fifo.pull (tmp, 2048);
        for (int i = 0; i < n; ++i) { ring[(size_t) pos] = tmp[i]; pos = (pos + 1) % fftSize; }
        got = true;
    }
    if (! got) { for (auto& d : disp) d -= 1.2f; return; }
    for (int i = 0; i < fftSize; ++i) work[(size_t) i] = ring[(size_t) ((pos + i) % fftSize)];
    std::fill (work.begin() + fftSize, work.end(), 0.0f);
    window.multiplyWithWindowingTable (work.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (work.data());
    const double fs = proc.getCurrentSampleRate();
    for (int k = 1; k <= fftSize / 2; ++k)
    {
        const double f = (double) k * fs / fftSize;
        const float v = gainToDb (work[(size_t) k] / (fftSize * 0.25f)) + 4.5f * (float) std::log2 (std::max (f, 20.0) / 1000.0);
        auto& d = disp[(size_t) k];
        d = v > d ? d + (v - d) * 0.6f : d - 1.2f;
    }
}

void EqGraph::refresh()
{
    analyse (proc.preFifo, preRing, prePos, preDisp);
    analyse (proc.postFifo, postRing, postPos, postDisp);
    repaint();
}

void EqGraph::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    vegaui::drawScreen (g, b);
    const auto r = plot();
    const double fs = proc.getCurrentSampleRate();
    const int chr = proc.character();

    // grid
    g.setFont (VegaFonts::ui (9.5f));
    for (double f : { 30.0, 50.0, 100.0, 200.0, 300.0, 500.0, 1000.0, 2000.0, 3000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = xFor (f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
        g.setColour (VegaColours::textDim);
        g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x - 15, (int) r.getBottom() + 2, 30, 12, juce::Justification::centred);
    }
    for (int d = -18; d <= 18; d += 6)
    {
        const float y = yFor (d);
        g.setColour (juce::Colours::white.withAlpha (d == 0 ? 0.14f : 0.05f));
        g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        g.setColour (VegaColours::textDim);
        g.drawText ((d > 0 ? "+" : "") + juce::String (d), 2, (int) y - 6, 24, 12, juce::Justification::centredRight);
    }

    // spectra (pre = filled grey, post = white line)
    auto specPath = [&] (const std::vector<float>& disp, bool closed)
    {
        juce::Path p; const int Wd = (int) r.getWidth();
        for (int px = 0; px <= Wd; px += 2)
        {
            const double f0 = fMin * std::pow (fMax / fMin, (double) px / Wd), f1 = fMin * std::pow (fMax / fMin, (double) (px + 2) / Wd);
            const int k0 = juce::jlimit (1, fftSize / 2, (int) (f0 * fftSize / fs)), k1 = juce::jlimit (k0, fftSize / 2, (int) (f1 * fftSize / fs));
            float mx = -200; for (int k = k0; k <= k1; ++k) mx = std::max (mx, disp[(size_t) k]);
            const float y = juce::jmap (juce::jlimit (-90.0f, 6.0f, mx), -90.0f, 6.0f, r.getBottom(), r.getY());
            if (px == 0) { if (closed) { p.startNewSubPath (r.getX(), r.getBottom()); p.lineTo (r.getX(), y); } else p.startNewSubPath (r.getX(), y); }
            else p.lineTo (r.getX() + (float) px, y);
        }
        if (closed) { p.lineTo (r.getRight(), r.getBottom()); p.closeSubPath(); }
        return p;
    };
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillPath (specPath (preDisp, true));
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.strokePath (specPath (postDisp, false), juce::PathStrokeType (1.0f));

    // curves
    std::array<BandSettings, kNumBands> bs; std::array<Cascade, kNumBands> cas;
    bool anyMid = false, anySide = false;
    for (int i = 0; i < kNumBands; ++i)
    {
        bs[(size_t) i] = proc.readBand (i);
        if (bs[(size_t) i].on)
        {
            cas[(size_t) i] = makeCascade (bs[(size_t) i], fs, chr, proc.liveGainDb[(size_t) i].load());
            anyMid  |= bs[(size_t) i].channel == Mid;
            anySide |= bs[(size_t) i].channel == Side;
        }
    }
    const auto pc = makeProgramCascade (proc.readProgram(), fs);
    const int sel = proc.selectedBand.load();
    const int Wd = (int) r.getWidth();

    // selected band area
    if (bs[(size_t) sel].on)
    {
        juce::Path area; area.startNewSubPath (r.getX(), yFor (0));
        for (int px = 0; px <= Wd; px += 2)
        {
            const double f = fMin * std::pow (fMax / fMin, (double) px / Wd);
            area.lineTo (r.getX() + px, yFor (20.0 * std::log10 (cas[(size_t) sel].magnitude (f, fs))));
        }
        area.lineTo (r.getRight(), yFor (0)); area.closeSubPath();
        g.setColour (bandColours[sel].withAlpha (0.16f)); g.fillPath (area);
    }

    auto totalCurve = [&] (int which)   // 0 = mid/stereo, 1 = side
    {
        juce::Path p;
        for (int px = 0; px <= Wd; px += 2)
        {
            const double f = fMin * std::pow (fMax / fMin, (double) px / Wd);
            double m = pc.magnitude (f, fs);
            for (int i = 0; i < kNumBands; ++i)
                if (bs[(size_t) i].on && (bs[(size_t) i].channel == Stereo || bs[(size_t) i].channel == (which == 0 ? Mid : Side)))
                    m *= cas[(size_t) i].magnitude (f, fs);
            const float y = yFor (20.0 * std::log10 (m));
            if (px == 0) p.startNewSubPath (r.getX(), y); else p.lineTo (r.getX() + px, y);
        }
        return p;
    };
    if (anySide || anyMid)
    {
        g.setColour (VegaColours::gold.withAlpha (0.9f)); g.strokePath (totalCurve (1), juce::PathStrokeType (1.6f));
        g.setColour (VegaColours::accent); g.strokePath (totalCurve (0), juce::PathStrokeType (2.0f));
        g.setFont (VegaFonts::ui (10.0f, true));
        g.setColour (VegaColours::accent); g.drawText ("MID", (int) r.getRight() - 72, (int) r.getY() + 4, 30, 12, juce::Justification::right);
        g.setColour (VegaColours::gold);   g.drawText ("SIDE", (int) r.getRight() - 38, (int) r.getY() + 4, 34, 12, juce::Justification::right);
    }
    else
    {
        g.setColour (VegaColours::accent); g.strokePath (totalCurve (0), juce::PathStrokeType (2.0f));
    }

    // nodes
    for (int i = 0; i < kNumBands; ++i)
    {
        const auto s = bs[(size_t) i];
        auto pnt = nodePos (i);
        const auto col = bandColours[i];
        if (s.on && s.dynamic && s.canBeDynamic())
        {
            const float yl = yFor (proc.liveGainDb[(size_t) i].load());
            g.setColour (col.withAlpha (0.8f)); g.drawLine (pnt.x, pnt.y, pnt.x, yl, 2.0f);
            g.drawEllipse (pnt.x - 5, yl - 5, 10, 10, 1.5f);
        }
        const float rad = (i == sel || i == hoverBand) ? 10.0f : 8.5f;
        if (s.on) { g.setColour (col); g.fillEllipse (pnt.x - rad, pnt.y - rad, rad * 2, rad * 2); g.setColour (juce::Colour (0xff141414)); }
        else      { g.setColour (juce::Colour (0xff141414).withAlpha (0.8f)); g.fillEllipse (pnt.x - rad, pnt.y - rad, rad * 2, rad * 2); g.setColour (col.withAlpha (0.6f)); g.drawEllipse (pnt.x - rad, pnt.y - rad, rad * 2, rad * 2, 1.4f); }
        g.setFont (VegaFonts::ui (11.0f, true));
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (rad * 2, rad * 2).withCentre (pnt).toNearestInt(), juce::Justification::centred);
        if (i == sel) { g.setColour (juce::Colours::white.withAlpha (0.8f)); g.drawEllipse (pnt.x - rad - 3, pnt.y - rad - 3, rad * 2 + 6, rad * 2 + 6, 1.0f); }
    }

    // read‑out for hovered/selected band
    const int info = hoverBand >= 0 ? hoverBand : sel;
    {
        const auto s = bs[(size_t) info];
        juce::String t = "BAND " + juce::String (info + 1) + "  " + typeNames()[s.type].toUpperCase() + "  " + hz (s.freq);
        if (s.type == Bell || s.type == LowShelf || s.type == HighShelf) t << "  " << db (s.gain);
        t << "  Q " << juce::String (s.q, 2);
        if (s.channel != Stereo) t << "  " << channelNames()[s.channel].toUpperCase();
        if (s.dynamic && s.canBeDynamic()) t << "  DYN";
        if (! s.on) t << "  (OFF)";
        g.setFont (VegaFonts::mono (11.0f));
        g.setColour (bandColours[info]);
        g.drawText (t, (int) r.getX() + 8, (int) r.getY() + 4, 600, 14, juce::Justification::left);
    }
    g.setFont (VegaFonts::ui (9.5f));
    g.setColour (VegaColours::textDim.withAlpha (0.7f));
    g.drawText ("drag: freq/gain   wheel: Q   right-click: type   double-click: add / on-off",
                (int) r.getX() + 8, (int) r.getBottom() - 16, 520, 12, juce::Justification::left);
}

void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    const int h = hitNode (e.position);
    if (h != hoverBand) { hoverBand = h; setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor); }
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    dragBand = hitNode (e.position);
    if (dragBand < 0) return;
    proc.selectedBand.store (dragBand);
    if (onSelect) onSelect (dragBand);

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        const auto s = proc.readBand (dragBand);
        for (int t = 0; t < typeNames().size(); ++t) m.addItem (1 + t, typeNames()[t], true, s.type == t);
        m.addSeparator();
        for (int c = 0; c < 3; ++c) m.addItem (100 + c, channelNames()[c], true, s.channel == c);
        m.addSeparator();
        m.addItem (200, "Dynamic", s.canBeDynamic(), s.dynamic);
        m.addItem (201, s.on ? "Turn band off" : "Turn band on");
        const int band = dragBand;
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ e.getScreenX(), e.getScreenY(), 1, 1 }), [this, band, s] (int res)
        {
            auto set = [&] (const char* n, float v) { if (auto* prm = proc.apvts.getParameter (bandId (band, n))) { prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); prm->endChangeGesture(); } };
            if (res >= 1 && res < 100) { set ("Type", (float) (res - 1)); set ("On", 1.0f); }
            else if (res >= 100 && res < 200) set ("Chan", (float) (res - 100));
            else if (res == 200) set ("Dyn", s.dynamic ? 0.0f : 1.0f);
            else if (res == 201) set ("On", s.on ? 0.0f : 1.0f);
        });
        dragBand = -1;
        return;
    }
    for (auto* n : { "Freq", "Gain", "On" })
        if (auto* prm = proc.apvts.getParameter (bandId (dragBand, n))) prm->beginChangeGesture();
    setParam (dragBand, "On", 1.0f);
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0) return;
    const auto s = proc.readBand (dragBand);
    setParam (dragBand, "Freq", (float) fFor (e.position.x));
    if (s.type == Bell || s.type == LowShelf || s.type == HighShelf)
        setParam (dragBand, "Gain", (float) juce::jlimit (-24.0, 24.0, dbFor (e.position.y)));
    repaint();
}

void EqGraph::mouseUp (const juce::MouseEvent&)
{
    if (dragBand >= 0)
        for (auto* n : { "Freq", "Gain", "On" })
            if (auto* prm = proc.apvts.getParameter (bandId (dragBand, n))) prm->endChangeGesture();
    dragBand = -1;
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int h = hitNode (e.position);
    auto gesture = [&] (int band, const char* n, float v)
    { if (auto* prm = proc.apvts.getParameter (bandId (band, n))) { prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); prm->endChangeGesture(); } };
    if (h >= 0)
    {
        gesture (h, "On", proc.readBand (h).on ? 0.0f : 1.0f);
        return;
    }
    // add: use the first band that is switched off, as a bell at the clicked point
    for (int b = 0; b < kNumBands; ++b)
        if (! proc.readBand (b).on)
        {
            gesture (b, "Type", (float) Bell);
            gesture (b, "Freq", (float) fFor (e.position.x));
            gesture (b, "Gain", (float) juce::jlimit (-24.0, 24.0, dbFor (e.position.y)));
            gesture (b, "On", 1.0f);
            proc.selectedBand.store (b);
            if (onSelect) onSelect (b);
            return;
        }
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int h = hitNode (e.position) >= 0 ? hitNode (e.position) : proc.selectedBand.load();
    if (auto* prm = proc.apvts.getParameter (bandId (h, "Q")))
    {
        const float q = proc.readBand (h).q * std::pow (2.0f, w.deltaY * (w.isReversed ? -1.0f : 1.0f) * 1.5f);
        prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (0.1f, 18.0f, q))); prm->endChangeGesture();
    }
}

//==============================================================================
class VegaEqEditor::Panel : public juce::Component
{
public:
    Panel (VegaEqProcessor& p) : proc (p), a (p.apvts), graph (p),
        character (a, "character", "Character", false, {}, "Clean|Tube|Punch|Console"),
        drive (a, "drive", "Drive"),
        phase (a, "phase", "Phase", "Natural|Linear"),
        output (a, "output", "Output", false),
        pOn (a, "pOn", "Program EQ"),
        pLfFreq (a, "pLfFreq", "LF Freq", false, {}, "20|30|60|100"),
        pLfBoost (a, "pLfBoost", "Boost"), pLfAtten (a, "pLfAtten", "Atten"),
        pHfFreq (a, "pHfFreq", "HF Freq", false, {}, "3k|4k|5k|8k|10k|12k|16k"),
        pHfBoost (a, "pHfBoost", "Boost"), pHfBw (a, "pHfBw", "Bandwidth"),
        pHfAttFreq (a, "pHfAttFreq", "Att Freq", false, {}, "5k|10k|20k"),
        pHfAtten (a, "pHfAtten", "Atten")
    {
        addAndMakeVisible (graph);
        graph.onSelect = [this] (int b) { selectBand (b); };
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& s = sets[(size_t) b];
            s.on    = std::make_unique<vegaui::Toggle> (a, bandId (b, "On"), "Band On");
            s.type  = std::make_unique<vegaui::Choice> (a, bandId (b, "Type"), "Type");
            s.slope = std::make_unique<vegaui::Choice> (a, bandId (b, "Slope"), "Slope");
            s.chan  = std::make_unique<vegaui::Choice> (a, bandId (b, "Chan"), "Channel");
            s.freq  = std::make_unique<vegaui::Knob> (a, bandId (b, "Freq"), "Frequency", true);
            s.gain  = std::make_unique<vegaui::Knob> (a, bandId (b, "Gain"), "Gain", true);
            s.q     = std::make_unique<vegaui::Knob> (a, bandId (b, "Q"), "Q", true);
            s.dyn   = std::make_unique<vegaui::Toggle> (a, bandId (b, "Dyn"), "Dynamic");
            s.thr   = std::make_unique<vegaui::Knob> (a, bandId (b, "Thr"), "Threshold");
            s.range = std::make_unique<vegaui::Knob> (a, bandId (b, "Range"), "Range");
            for (auto* c : s.all()) addChildComponent (c);

            auto& btn = bandBtn[(size_t) b];
            btn.setButtonText (juce::String (b + 1));
            btn.setClickingTogglesState (true); btn.setRadioGroupId (77);
            btn.setColour (juce::TextButton::buttonOnColourId, bandColours[b]);
            btn.onClick = [this, b] { proc.selectedBand.store (b); selectBand (b); };
            addAndMakeVisible (btn);
        }
        for (auto* c : std::initializer_list<juce::Component*> { &character, &drive, &phase, &output, &pOn, &pLfFreq, &pLfBoost, &pLfAtten,
                                                                &pHfFreq, &pHfBoost, &pHfBw, &pHfAttFreq, &pHfAtten, &meter })
            addAndMakeVisible (c);

        bypass.setClickingTogglesState (true);
        bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (a, "bypass", bypass);
        addAndMakeVisible (bypass);
        reset.onClick = [this] { vegaui::resetAllParameters (proc); };
        addAndMakeVisible (reset);

        zoom.addItemList ({ "75%", "85%", "100%", "115%", "125%" }, 1);
        const float sc[] { 0.75f, 0.85f, 1.0f, 1.15f, 1.25f };
        int zi = 2; for (int i = 0; i < 5; ++i) if (std::abs (sc[i] - proc.uiScale.load()) < 0.01f) zi = i;
        zoom.setSelectedItemIndex (zi, juce::dontSendNotification);
        zoom.onChange = [this]
        {
            const float s[] { 0.75f, 0.85f, 1.0f, 1.15f, 1.25f };
            proc.uiScale.store (s[juce::jlimit (0, 4, zoom.getSelectedItemIndex())]);
            juce::Component::SafePointer<Panel> sp (this);
            juce::MessageManager::callAsync ([sp] { if (sp != nullptr && sp->onScale) sp->onScale(); });
        };
        addAndMakeVisible (zoom);

        selectBand (proc.selectedBand.load());
    }

    std::function<void()> onScale;

    void selectBand (int b)
    {
        for (int i = 0; i < kNumBands; ++i)
        {
            for (auto* c : sets[(size_t) i].all()) c->setVisible (i == b);
            bandBtn[(size_t) i].setToggleState (i == b, juce::dontSendNotification);
        }
        repaint();
    }

    void refresh()
    {
        graph.refresh();
        meter.setLevels (proc.outPeakL.exchange (0.0f), proc.outPeakR.exchange (0.0f));
        // dim controls that don't apply to the selected band's type
        const int b = proc.selectedBand.load();
        const auto s = proc.readBand (b);
        auto& set = sets[(size_t) b];
        const bool gainType = s.type == Bell || s.type == LowShelf || s.type == HighShelf;
        set.gain->setAlpha (gainType ? 1.0f : 0.35f);
        set.slope->setAlpha (s.type == LowCut || s.type == HighCut ? 1.0f : 0.35f);
        const bool dynOk = s.canBeDynamic();
        set.dyn->setAlpha (dynOk ? 1.0f : 0.35f);
        set.thr->setAlpha (dynOk && s.dynamic ? 1.0f : 0.35f);
        set.range->setAlpha (dynOk && s.dynamic ? 1.0f : 0.35f);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VegaColours::background);
        vegaui::drawTopBar (g, W, "EQUALIZER  VP-6");
        vegaui::drawBody (g, { 0, 400, W, 270 });
        const char* titles[] { "BAND", "DYNAMIC", "CHARACTER", "TUBE PROGRAM EQ", "OUTPUT" };
        for (int i = 0; i < 5; ++i)
            vegaui::drawSection (g, { sx[(size_t) i], 404, sx[(size_t) i + 1] - sx[(size_t) i], 262 }, titles[i], i > 0);
        vegaui::drawBottomBar (g, { 0, H - 30, W, 30 }, "PARAMETRIC  /  DYNAMIC  /  MID-SIDE  /  ANALOG CHARACTER  /  LINEAR PHASE");
        g.setFont (VegaFonts::ui (10.0f, true)); g.setColour (VegaColours::textDim);
        g.drawText ("ZOOM", W - 150, H - 25, 40, 20, juce::Justification::centredRight);
        // LF / HF labels in program section
        g.drawText ("LOW", sx[3] + 4, 424, 40, 12, juce::Justification::left);
        g.drawText ("HIGH", sx[3] + 4, 549, 40, 12, juce::Justification::left);
    }

    void resized() override
    {
        sx = { 0, 540, 720, 880, 1310, W };
        bypass.setBounds (W - 76, 7, 66, 20);
        reset.setBounds (W - 142, 7, 60, 20);
        graph.setBounds (10, 42, W - 100, 352);
        meter.setBounds (W - 82, 42, 70, 352);

        // BAND
        for (int b = 0; b < kNumBands; ++b) bandBtn[(size_t) b].setBounds (16 + b * 34, 432, 30, 22);
        for (auto& s : sets)
        {
            s.on->setBounds (222, 420, 64, 74);
            s.type->setBounds (292, 424, 118, 46);
            s.slope->setBounds (414, 424, 118, 46);
            s.chan->setBounds (16, 462, 200, 46);
            s.freq->setBounds (14, 506, 170, 160);
            s.gain->setBounds (186, 506, 170, 160);
            s.q   ->setBounds (358, 506, 170, 160);
            s.dyn->setBounds (sx[1] + 55, 420, 70, 74);
            s.thr->setBounds (sx[1] + 4, 520, 88, 130);
            s.range->setBounds (sx[1] + 92, 520, 88, 130);
        }
        // CHARACTER
        character.setBounds (sx[2] + 5, 420, 150, 120);
        drive.setBounds (sx[2] + 30, 545, 100, 120);
        // PROGRAM EQ (2 rows; stepped selectors get wider cells)
        int x = sx[3] + 28;
        pLfFreq.setBounds (x, 420, 112, 122); x += 112;
        pLfBoost.setBounds (x, 420, 78, 122); x += 78;
        pLfAtten.setBounds (x, 420, 78, 122); x += 78;
        pOn.setBounds (x + 10, 424, 90, 80);
        x = sx[3] + 28;
        pHfFreq.setBounds (x, 545, 112, 122); x += 112;
        pHfBoost.setBounds (x, 545, 64, 122); x += 64;
        pHfBw.setBounds (x, 545, 68, 122); x += 68;
        pHfAttFreq.setBounds (x, 545, 90, 122); x += 90;
        pHfAtten.setBounds (x, 545, 66, 122);
        // OUTPUT
        phase.setBounds (sx[4] + 15, 420, 110, 60);
        output.setBounds (sx[4] + 10, 500, 110, 150);
        zoom.setBounds (W - 104, H - 25, 64, 20);
    }

private:
    struct BandSet
    {
        std::unique_ptr<vegaui::Toggle> on, dyn;
        std::unique_ptr<vegaui::Choice> type, slope, chan;
        std::unique_ptr<vegaui::Knob> freq, gain, q, thr, range;
        std::vector<juce::Component*> all() { return { on.get(), dyn.get(), type.get(), slope.get(), chan.get(), freq.get(), gain.get(), q.get(), thr.get(), range.get() }; }
    };
    VegaEqProcessor& proc;
    juce::AudioProcessorValueTreeState& a;
    EqGraph graph;
    std::array<BandSet, kNumBands> sets;
    std::array<juce::TextButton, kNumBands> bandBtn;
    vegaui::Knob character, drive; vegaui::Toggle phase; vegaui::Knob output;
    vegaui::Toggle pOn; vegaui::Knob pLfFreq, pLfBoost, pLfAtten, pHfFreq, pHfBoost, pHfBw, pHfAttFreq, pHfAtten;
    vegaui::StereoMeter meter;
    juce::TextButton bypass { "BYPASS" }, reset { "RESET" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;
    juce::ComboBox zoom;
    std::array<int, 6> sx {};
};

//==============================================================================
VegaEqEditor::VegaEqEditor (VegaEqProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    panel = std::make_unique<Panel> (p);
    panel->onScale = [this] { applyScale(); };
    addAndMakeVisible (*panel);
    applyScale();
    startTimerHz (30);
}
VegaEqEditor::~VegaEqEditor() { stopTimer(); panel.reset(); setLookAndFeel (nullptr); }
void VegaEqEditor::timerCallback() { panel->refresh(); }
void VegaEqEditor::applyScale()
{
    const float s = juce::jlimit (0.5f, 2.0f, proc.uiScale.load());
    panel->setTransform (juce::AffineTransform::scale (s));
    setSize (juce::roundToInt (W * s), juce::roundToInt (H * s));
}
void VegaEqEditor::resized() { panel->setBounds (0, 0, W, H); }

juce::AudioProcessorEditor* VegaEqProcessor::createEditor() { return new VegaEqEditor (*this); }
