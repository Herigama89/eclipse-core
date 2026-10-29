#pragma once

#include "PluginProcessor.h"
#include "EclipseLookAndFeel.h"

class NebulaAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NebulaAudioProcessorEditor (NebulaAudioProcessor&);
    ~NebulaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setDelta (bool on);
    void drawMeters (juce::Graphics&);
    void drawScope (juce::Graphics&);
    void drawReadouts (juce::Graphics&);

    NebulaAudioProcessor& proc;
    EclipseLookAndFeel lnf;
    juce::Image skin;

    struct Knob
    {
        juce::Slider slider;
        juce::Rectangle<float> bounds, valueArea;   // skin (1206 x 790) coordinates
        bool inBar = false;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    std::array<Knob, 7> knobs;

    juce::TextButton dryButton { "DRY VOCAL" }, ambientButton { "AMBIENT" }, bypassButton { "" }, aiButton { "AI ENGINE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment, aiAttachment;

    static constexpr int scopePoints = 200, wavePoints = 8;
    std::vector<float> inPow, outPow, waveIn, waveOut, scopeNoise, scopeReverb;
    float reductionPct = 0.0f, inLevel = 0.0f, outLevel = 0.0f, scopePeak = 0.003f, noiseShare = 0.5f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NebulaAudioProcessorEditor)
};
