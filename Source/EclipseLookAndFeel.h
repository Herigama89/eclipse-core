#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ec
{
    inline const juce::Colour bg        { 0xff04060a };
    inline const juce::Colour cyan      { 0xff38e1ff };
    inline const juce::Colour cyanSoft  { 0xffbdf3ff };
    inline const juce::Colour orange    { 0xffff9a2e };
    inline const juce::Colour orangeHot { 0xffffe2a8 };
    inline const juce::Colour ember     { 0xffc2410c };
    inline const juce::Colour purple    { 0xff8b5cf6 };
    inline const juce::Colour text      { 0xffd8e6ee };
    inline const juce::Colour textDim   { 0xff71879a };

    inline juce::Font font (float h, bool bold = false)
    {
       #if JUCE_MAC
        return juce::Font (juce::FontOptions ("Avenir Next", h, bold ? juce::Font::bold : juce::Font::plain));
       #else
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
       #endif
    }

    // Neon glow: wide transparent strokes under a crisp core stroke
    inline void glowPath (juce::Graphics& g, const juce::Path& p, juce::Colour c, float w, float spread = 1.0f)
    {
        using namespace juce;
        const float widths[] = { 10.0f, 6.0f, 3.0f };
        const float alphas[] = { 0.05f, 0.10f, 0.22f };
        for (int i = 0; i < 3; ++i)
        {
            g.setColour (c.withMultipliedAlpha (alphas[i]));
            g.strokePath (p, PathStrokeType (w + widths[i] * spread, PathStrokeType::curved, PathStrokeType::rounded));
        }
        g.setColour (c);
        g.strokePath (p, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded));
    }
}

class EclipseLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Dark knob with a segmented LED ring in the slider's "accent" colour
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float a0, float a1, juce::Slider& s) override
    {
        using namespace juce;
        const auto accent = Colour ((uint32) (int64) s.getProperties().getWithDefault ("accent", (int64) 0xff38e1ff));
        const auto area = Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float R = jmin (area.getWidth(), area.getHeight()) * 0.5f - 1.0f;
        const auto c = area.getCentre();
        const bool big = R > 34.0f;

        // LED segments
        const int segs = big ? 30 : 20;
        const float segIn = R * 0.83f, segOut = R * 0.99f;
        const float segW = jmax (1.4f, (a1 - a0) * R * 0.9f / (float) segs * 0.55f);
        for (int i = 0; i < segs; ++i)
        {
            const float t = (float) i / (float) (segs - 1);
            const float a = a0 + t * (a1 - a0);
            const Line<float> l (c.getPointOnCircumference (segIn, a), c.getPointOnCircumference (segOut, a));
            if (pos > 0.001f && t <= pos + 1.0e-4f)
            {
                g.setColour (accent.withAlpha (0.20f));
                g.drawLine (l, segW * 2.8f);
                g.setColour (accent.brighter (0.25f));
                g.drawLine (l, segW);
            }
            else
            {
                g.setColour (accent.withAlpha (0.13f));
                g.drawLine (l, segW);
            }
        }

        // body
        const float bodyR = R * 0.70f, capR = bodyR * 0.78f;
        g.setColour (Colours::black.withAlpha (0.65f));
        g.fillEllipse (Rectangle<float> (bodyR * 2.0f + 4.0f, bodyR * 2.0f + 4.0f).withCentre (c.translated (0.0f, 3.0f)));

        const auto body = Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (c);
        g.setGradientFill (ColourGradient (Colour (0xff2a313a), c.x, c.y - bodyR, Colour (0xff05070a), c.x, c.y + bodyR, false));
        g.fillEllipse (body);
        for (int i = 0; i < 48; ++i)
        {
            const float a = MathConstants<float>::twoPi * (float) i / 48.0f;
            const float light = 0.5f + 0.5f * std::cos (a);
            g.setColour (Colours::white.withAlpha (0.02f + 0.07f * light));
            g.drawLine ({ c.getPointOnCircumference (capR * 1.02f, a), c.getPointOnCircumference (bodyR * 0.97f, a) }, 0.8f);
        }
        g.setColour (accent.withAlpha (0.35f));
        g.drawEllipse (body, 1.0f);

        Path cap;
        cap.addEllipse (Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (c));
        {
            Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (cap);
            const int wedges = 96;
            for (int i = 0; i < wedges; ++i)
            {
                const float a = MathConstants<float>::twoPi * (float) i / (float) wedges;
                const float shade = 0.5f + 0.5f * std::cos (2.0f * (a - 0.5f));       // conical brushed highlight
                Path wedge;
                wedge.addPieSegment (c.x - capR, c.y - capR, capR * 2.0f, capR * 2.0f,
                                     a, a + MathConstants<float>::twoPi / (float) wedges + 0.02f, 0.0f);
                g.setColour (Colour::greyLevel (0.16f + 0.42f * shade));
                g.fillPath (wedge);
            }
            g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.10f), c.x, c.y - capR,
                                               Colours::black.withAlpha (0.35f), c.x, c.y + capR, false));
            g.fillPath (cap);
        }
        g.setColour (Colours::black.withAlpha (0.85f));
        g.strokePath (cap, PathStrokeType (1.2f));

        // pointer
        const float angle = a0 + pos * (a1 - a0);
        Path ptr;
        ptr.startNewSubPath (c.getPointOnCircumference (capR * 0.30f, angle));
        ptr.lineTo (c.getPointOnCircumference (bodyR * 0.90f, angle));
        g.setColour (accent.withAlpha (0.35f));
        g.strokePath (ptr, PathStrokeType (big ? 5.0f : 3.5f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (Colour (0xfff2f7fa));
        g.strokePath (ptr, PathStrokeType (big ? 2.2f : 1.5f, PathStrokeType::curved, PathStrokeType::rounded));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        using namespace juce;
        const auto style = button.getProperties()["style"].toString();
        const bool on = button.getToggleState();

        if (style == "pill")
        {
            auto b = button.getLocalBounds().toFloat().reduced (4.0f);
            const float r = b.getHeight() * 0.5f;
            if (on)
                for (int i = 1; i <= 4; ++i)
                {
                    g.setColour (ec::cyan.withAlpha (0.07f));
                    g.drawRoundedRectangle (b.expanded ((float) i * 1.3f), r + (float) i * 1.3f, 2.0f);
                }
            if (on)
                g.setGradientFill (ColourGradient (ec::cyan.withAlpha (0.38f), b.getX(), b.getY(),
                                                   ec::cyan.withAlpha (0.12f), b.getX(), b.getBottom(), false));
            else
                g.setColour (Colour (0xff0a141b).withAlpha (down ? 1.0f : 0.9f));
            g.fillRoundedRectangle (b, r);
            g.setColour (ec::cyan.withAlpha (on ? 1.0f : (highlighted ? 0.75f : 0.45f)));
            g.drawRoundedRectangle (b, r, 1.6f);
            return;
        }

        if (style == "ai")
        {
            auto b = button.getLocalBounds().toFloat().reduced (4.0f);
            const float r = b.getHeight() * 0.5f;
            const bool en = button.isEnabled();
            const juce::Colour v (0xffa78bfa);   // violet "AI" accent
            if (on && en)
                for (int i = 1; i <= 4; ++i)
                {
                    g.setColour (v.withAlpha (0.08f));
                    g.drawRoundedRectangle (b.expanded ((float) i * 1.3f), r + (float) i * 1.3f, 2.0f);
                }
            if (on && en)
                g.setGradientFill (ColourGradient (v.withAlpha (0.42f), b.getX(), b.getY(), ec::cyan.withAlpha (0.20f), b.getRight(), b.getBottom(), false));
            else
                g.setColour (Colour (0xff0d0a18).withAlpha (down ? 1.0f : 0.9f));
            g.fillRoundedRectangle (b, r);
            g.setColour (v.withAlpha (! en ? 0.25f : (on ? 1.0f : (highlighted ? 0.75f : 0.45f))));
            g.drawRoundedRectangle (b, r, 1.6f);

            // status dot
            const float dr = b.getHeight() * 0.13f;
            const juce::Point<float> dc (b.getX() + r, b.getCentreY());
            g.setColour ((on && en) ? Colours::white : v.withAlpha (0.35f));
            g.fillEllipse (dc.x - dr, dc.y - dr, dr * 2.0f, dr * 2.0f);
            return;
        }

        if (style == "switch")
        {
            const auto track = button.getLocalBounds().toFloat().reduced (1.5f);
            const float r = track.getHeight() * 0.5f, kd = track.getHeight() - 6.0f;
            if (on)
            {
                for (int i = 1; i <= 3; ++i)
                {
                    g.setColour (ec::orange.withAlpha (0.10f));
                    g.drawRoundedRectangle (track.expanded ((float) i * 1.2f), r + (float) i * 1.2f, 2.0f);
                }
                g.setColour (ec::orange.withAlpha (0.85f));
                g.fillRoundedRectangle (track, r);
                g.setColour (Colours::white);
                g.fillEllipse (track.getRight() - kd - 3.0f, track.getY() + 3.0f, kd, kd);
            }
            else
            {
                g.setColour (Colour (0xff120d08));
                g.fillRoundedRectangle (track, r);
                g.setColour (ec::orange.withAlpha (highlighted ? 0.95f : 0.7f));
                g.drawRoundedRectangle (track, r, 1.4f);
                g.setColour (ec::orange);
                g.fillEllipse (track.getX() + 3.0f, track.getY() + 3.0f, kd, kd);
            }
            return;
        }

        LookAndFeel_V4::drawButtonBackground (g, button, ec::cyan, highlighted, down);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down) override
    {
        using namespace juce;
        const auto style = button.getProperties()["style"].toString();
        auto b = button.getLocalBounds().toFloat();

        if (style == "pill")
        {
            g.setColour (button.getToggleState() ? Colours::white : ec::cyan.withAlpha (0.8f));
            g.setFont (ec::font (b.getHeight() * 0.40f, true).withExtraKerningFactor (0.08f));
            g.drawText (button.getButtonText(), b.toNearestInt(), Justification::centred);
            return;
        }
        if (style == "ai")
        {
            const bool on = button.getToggleState() && button.isEnabled();
            g.setColour (on ? Colours::white : Colour (0xffa78bfa).withAlpha (button.isEnabled() ? 0.8f : 0.35f));
            g.setFont (ec::font (b.getHeight() * 0.36f, true).withExtraKerningFactor (0.10f));
            g.drawText (button.getButtonText(), b.withTrimmedLeft (b.getHeight() * 0.55f).toNearestInt(), Justification::centred);
            return;
        }
        if (style == "switch" && button.getButtonText().isNotEmpty())
        {
            g.setColour (button.getToggleState() ? ec::orange : ec::text.withAlpha (0.85f));
            g.setFont (ec::font (b.getHeight() * 0.55f, true).withExtraKerningFactor (0.1f));
            g.drawText (button.getButtonText(), b.withTrimmedLeft (50.0f).toNearestInt(), Justification::centredLeft);
            return;
        }
        if (style == "switch") return;
        LookAndFeel_V4::drawButtonText (g, button, highlighted, down);
    }
};
