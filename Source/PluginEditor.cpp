#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
    // Everything is laid out in the coordinates of the skin image (1206 x 790)
    constexpr float W = 1206.0f, H = 790.0f;
    const juce::Point<float> G { 608.0f, 298.0f };   // galaxy window centre
    constexpr float galaxyR = 166.0f;

    struct Meter { float cx, cy, r, bottom, top; };  // arcs fitted to the painted meter slots
    constexpr Meter meters[3] = {
        { 621.8f, 302.5f, 499.8f, -1.955f, -1.186f },   // INPUT
        { 584.1f, 305.3f, 371.2f, -1.986f, -1.139f },   // OUTPUT
        { 662.5f, 306.3f, 439.6f,  2.014f,  1.114f },   // REDUCTION
    };

    juce::Rectangle<float> centred (float cx, float cy, float size) { return { cx - size * 0.5f, cy - size * 0.5f, size, size }; }
}

NebulaAudioProcessorEditor::NebulaAudioProcessorEditor (NebulaAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    using namespace juce;
    setOpaque (true);
    setLookAndFeel (&lnf);
    skin = ImageCache::getFromMemory (BinaryData::skin_jpg, BinaryData::skin_jpgSize);

    struct Spec { const char* id; Rectangle<float> b; Colour accent; Rectangle<float> value; bool bar; };
    const Spec specs[] = {
        { "noise",  centred (339.5f, 597.0f, 152.0f), ec::orange, { 299.0f, 697.0f, 96.0f, 16.0f }, false },
        { "reverb", centred (874.5f, 592.5f, 152.0f), ec::cyan,   { 826.0f, 697.0f, 96.0f, 16.0f }, false },
        { "smooth", centred (167.0f, 612.0f,  80.0f), ec::orange, { 128.0f, 671.0f, 80.0f, 16.0f }, false },
        { "tail",   centred (1052.0f, 607.5f, 80.0f), ec::cyan,   { 1012.0f, 671.0f, 80.0f, 16.0f }, false },
        { "depth",  centred (37.0f,  751.0f,  48.0f), ec::cyan,   { 72.0f,  755.0f, 70.0f, 16.0f }, true },
        { "mix",    centred (202.0f, 751.0f,  48.0f), ec::cyan,   { 234.0f, 755.0f, 70.0f, 16.0f }, true },
        { "output", centred (359.0f, 751.0f,  48.0f), ec::cyan,   { 394.0f, 755.0f, 70.0f, 16.0f }, true },
    };

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        const auto& sp = specs[i];
        k.bounds = sp.b; k.valueArea = sp.value; k.inBar = sp.bar;
        k.slider.getProperties().set ("accent", (int64) sp.accent.getARGB());
        k.slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        k.slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
        k.attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, sp.id, k.slider);
        if (auto* param = proc.apvts.getParameter (sp.id))
            k.slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
        addAndMakeVisible (k.slider);
    }

    for (auto* b : { &dryButton, &ambientButton })
    {
        b->getProperties().set ("style", "pill");
        b->setClickingTogglesState (true);
        b->setRadioGroupId (2001);
        addAndMakeVisible (*b);
    }
    const bool d = proc.isDeltaOn();
    dryButton.setToggleState (! d, dontSendNotification);
    ambientButton.setToggleState (d, dontSendNotification);
    dryButton.onClick     = [this] { setDelta (false); };
    ambientButton.onClick = [this] { setDelta (true); };

    bypassButton.getProperties().set ("style", "switch");
    bypassButton.setClickingTogglesState (true);
    bypassAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "bypass", bypassButton);
    addAndMakeVisible (bypassButton);

    aiButton.getProperties().set ("style", "ai");
    aiButton.setClickingTogglesState (true);
    aiButton.setTooltip ("Neural voice isolation (DeepFilterNet 3) for a more natural de-noise");
    aiAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "ai", aiButton);
    if (! proc.isAIAvailable())
    {
        aiButton.setButtonText ("AI N/A");
        aiButton.setEnabled (false);
    }
    addAndMakeVisible (aiButton);

    setResizable (true, true);
    setResizeLimits (800, juce::roundToInt (800.0f * H / W), 1500, juce::roundToInt (1500.0f * H / W));
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (W / H);
    setSize (1000, juce::roundToInt (1000.0f * H / W));
    startTimerHz (40);
}

NebulaAudioProcessorEditor::~NebulaAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void NebulaAudioProcessorEditor::setDelta (bool on)
{
    if (auto* param = proc.apvts.getParameter ("delta"))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (on ? 1.0f : 0.0f);
        param->endChangeGesture();
    }
}

void NebulaAudioProcessorEditor::resized()
{
    const float s = (float) getWidth() / W;
    for (auto& k : knobs)
        k.slider.setBounds ((k.bounds * s).toNearestInt());
    dryButton.setBounds     ((juce::Rectangle<float> (460.0f, 734.0f, 142.0f, 40.0f) * s).toNearestInt());
    ambientButton.setBounds ((juce::Rectangle<float> (617.0f, 734.0f, 140.0f, 40.0f) * s).toNearestInt());
    bypassButton.setBounds  ((juce::Rectangle<float> (1056.0f, 740.0f, 51.0f, 26.0f) * s).toNearestInt());
    aiButton.setBounds      ((juce::Rectangle<float> (812.0f, 734.0f, 170.0f, 40.0f) * s).toNearestInt());
}

//==============================================================================
void NebulaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0a0b0e));
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    if (skin.isValid())
        g.drawImage (skin, getLocalBounds().toFloat());

    g.addTransform (juce::AffineTransform::scale ((float) getWidth() / W));
    drawMeters (g);
    drawScope (g);
    drawReadouts (g);
}

void NebulaAudioProcessorEditor::drawMeters (juce::Graphics& g)
{
    using namespace juce;
    const float levels[3] = { inLevel, outLevel, proc.isBypassed() ? 0.0f : reductionPct * 0.01f };
    for (int i = 0; i < 3; ++i)
    {
        const auto& m = meters[i];
        if (levels[i] < 0.005f) continue;
        const float range = m.top - m.bottom;
        const float a0 = m.bottom + range * 0.03f;
        const float a1 = a0 + levels[i] * range * 0.94f;
        Path p;
        p.addCentredArc (m.cx, m.cy, m.r, m.r, 0.0f, a0, a1, true);
        ec::glowPath (g, p, ec::cyan, 3.6f, 0.7f);
    }
}

void NebulaAudioProcessorEditor::drawScope (juce::Graphics& g)
{
    using namespace juce;
    const int n = (int) scopeNoise.size();
    if (n < 2) return;

    Graphics::ScopedSaveState ss (g);
    Path clip;
    clip.addEllipse (G.x - galaxyR, G.y - galaxyR, galaxyR * 2.0f, galaxyR * 2.0f);
    g.reduceClipRegion (clip);

    const float halfW = 150.0f, maxA = 80.0f;
    const float scale = maxA / scopePeak;
    const Colour blue (0xff3d8bff);

    auto trace = [&] (const std::vector<float>& v, Colour col)
    {
        Path p;
        for (int i = 0; i < n; ++i)
        {
            const float t = (float) i / (float) (n - 1);
            const float x = G.x - halfW + t * 2.0f * halfW;
            const float taper = std::pow (std::sin (MathConstants<float>::pi * t), 0.7f);
            const float y = G.y + jlimit (-maxA, maxA, -v[(size_t) i] * scale) * taper;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (Colours::black.withAlpha (0.45f));   // thin shadow so it reads over the bright core
        g.strokePath (p, PathStrokeType (4.0f, PathStrokeType::curved, PathStrokeType::rounded));
        ec::glowPath (g, p, col, 1.6f, 0.7f);
    };
    trace (scopeReverb, blue);              // room / reverb being removed
    trace (scopeNoise, Colours::white);     // noise being removed
}

void NebulaAudioProcessorEditor::drawReadouts (juce::Graphics& g)
{
    using namespace juce;
    const float cx = G.x;

    const String big = proc.isBypassed() ? String ("BYPASSED") : String (roundToInt (reductionPct)) + "% REDUCTION";
    g.setFont (ec::font (31.0f, true).withExtraKerningFactor (0.03f));
    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawText (big, Rectangle<float> (cx - 220.0f, 539.0f, 440.0f, 44.0f), Justification::centred, false);
    g.setColour (Colour (0xfff4f6f8));
    g.drawText (big, Rectangle<float> (cx - 220.0f, 537.0f, 440.0f, 44.0f), Justification::centred, false);

    if (! proc.isBypassed())
    {
        const int np = roundToInt (noiseShare * 100.0f);
        const auto f = ec::font (12.5f, true).withExtraKerningFactor (0.12f);
        const String a = "NOISE " + String (np) + "%", sep = "   |   ", b = "REVERB " + String (100 - np) + "%";
        const float wa = GlyphArrangement::getStringWidth (f, a), ws = GlyphArrangement::getStringWidth (f, sep),
                    wb = GlyphArrangement::getStringWidth (f, b);
        float x = cx - (wa + ws + wb) * 0.5f;
        g.setFont (f);
        g.setColour (Colour (0xfff2ece4));  g.drawText (a,   Rectangle<float> (x, 589.0f, wa + 4.0f, 18.0f), Justification::centredLeft, false); x += wa;
        g.setColour (ec::textDim);          g.drawText (sep, Rectangle<float> (x, 589.0f, ws + 4.0f, 18.0f), Justification::centredLeft, false); x += ws;
        g.setColour (Colour (0xff3d8bff));  g.drawText (b,   Rectangle<float> (x, 589.0f, wb + 4.0f, 18.0f), Justification::centredLeft, false);
    }

    for (auto& k : knobs)
    {
        const String v = k.slider.getTextFromValue (k.slider.getValue());
        if (k.inBar)
        {
            g.setFont (ec::font (11.5f, true));
            g.setColour (Colour (0xffe8edf0));
            g.drawText (v, k.valueArea, Justification::centredLeft, false);
        }
        else
        {
            g.setFont (ec::font (11.5f));
            g.setColour (Colour (0xffb8c2c9));
            g.drawText (v, k.valueArea, Justification::centred, false);
        }
    }
}

//==============================================================================
void NebulaAudioProcessorEditor::timerCallback()
{
    using namespace juce;
    reductionPct += 0.15f * (proc.getReductionPercent() - reductionPct);

    proc.getScope (scopeNoise, scopeReverb, scopePoints);
    {
        float pk = 0.0f; double en = 0.0, er = 0.0;
        for (int i = 0; i < scopePoints; ++i)
        {
            const float a = scopeNoise[(size_t) i], b = scopeReverb[(size_t) i];
            pk = jmax (pk, std::abs (a), std::abs (b));
            en += (double) a * a;  er += (double) b * b;
        }
        scopePeak = jmax (0.003f, pk, scopePeak * 0.97f);
        if (en + er > 1.0e-9)
            noiseShare += 0.08f * ((float) (en / (en + er)) - noiseShare);
    }

    proc.getWave (waveIn, waveOut, wavePoints);
    float pi = 0.0f, po = 0.0f;
    for (int i = 0; i < wavePoints; ++i) { pi = jmax (pi, waveIn[(size_t) i]); po = jmax (po, waveOut[(size_t) i]); }
    auto norm = [] (float p) { return jlimit (0.0f, 1.0f, (Decibels::gainToDecibels (p, -60.0f) + 60.0f) / 60.0f); };
    const float ti = norm (pi), to = norm (po);
    inLevel  = ti > inLevel  ? ti : jmax (ti, inLevel  - 0.02f);
    outLevel = to > outLevel ? to : jmax (to, outLevel - 0.02f);

    const bool aiOk = proc.isAIAvailable();
    if (aiButton.isEnabled() != aiOk)
    {
        aiButton.setEnabled (aiOk);
        aiButton.setButtonText (aiOk ? "AI ENGINE" : "AI N/A");
    }

    const bool d = proc.isDeltaOn();
    if (ambientButton.getToggleState() != d)
    {
        ambientButton.setToggleState (d, dontSendNotification);
        dryButton.setToggleState (! d, dontSendNotification);
    }
    repaint();
}
