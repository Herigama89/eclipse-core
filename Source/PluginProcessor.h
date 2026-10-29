#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "SpectralCleaner.h"
#include "eclipse_ai.h"

class NebulaAudioProcessor : public juce::AudioProcessor
{
public:
    NebulaAudioProcessor();
    ~NebulaAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    // GUI helpers
    void getAnalyzerData (std::vector<float>& inPow, std::vector<float>& outPow) const;
    bool isDarkMode() const          { return (bool) apvts.state.getProperty ("darkMode", true); }
    void setDarkMode (bool shouldBeDark) { apvts.state.setProperty ("darkMode", shouldBeDark, nullptr); }
    bool isDeltaOn() const           { return deltaParam->load() > 0.5f; }
    bool isBypassed() const          { return bypassParam != nullptr && bypassParam->get(); }
    bool isAIAvailable() const       { return aiAvailable.load(); }
    bool isAIOn() const              { return aiParam->load() > 0.5f && aiAvailable.load(); }
    float getReductionPercent() const { return reductionPct.load (std::memory_order_relaxed); }

    // Waveform peaks for the GUI (newest first)
    static constexpr int waveSize = 512, waveChunk = 256;
    void getWave (std::vector<float>& in, std::vector<float>& out, int count) const
    {
        in.resize ((size_t) count);
        out.resize ((size_t) count);
        const int w = waveWrite.load (std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
        {
            const int idx = ((w - 1 - i) % waveSize + waveSize) % waveSize;
            in[(size_t) i]  = waveIn[(size_t) idx].load (std::memory_order_relaxed);
            out[(size_t) i] = waveOut[(size_t) idx].load (std::memory_order_relaxed);
        }
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Removed-signal oscilloscope (noise part / reverb part), oldest first
    static constexpr int scopeSize = 512, scopeChunk = 32;
    void getScope (std::vector<float>& noise, std::vector<float>& reverb, int count) const
    {
        noise.resize ((size_t) count);
        reverb.resize ((size_t) count);
        const int w = scopeWrite.load (std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
        {
            const int idx = ((w - count + i) % scopeSize + scopeSize) % scopeSize;
            noise[(size_t) i]  = scopeN[(size_t) idx].load (std::memory_order_relaxed);
            reverb[(size_t) i] = scopeR[(size_t) idx].load (std::memory_order_relaxed);
        }
    }

private:
    // simple fixed delay line (length 0 = pass-through)
    struct Delay
    {
        std::vector<float> buf; int pos = 0;
        void setLength (int len) { buf.assign ((size_t) juce::jmax (0, len), 0.0f); pos = 0; }
        float process (float x) noexcept
        {
            if (buf.empty()) return x;
            const float y = buf[(size_t) pos]; buf[(size_t) pos] = x;
            if (++pos >= (int) buf.size()) pos = 0;
            return y;
        }
    };
    struct AIChannel
    {
        EclipseAI* engine = nullptr;
        Delay dryToAI, dryToOut, removedToOut;
    };
    void releaseAI();

    std::array<AIChannel, 2> aiCh;
    std::vector<float> aiIn, aiOut, inPeak;
    std::atomic<bool> aiAvailable { false };
    int aiLatency = 0;
    float aiWeight = 0.0f, mixSm = 1.0f, deltaSm = 0.0f, smoothCoef = 0.001f;
    double eIn = 0.0, eOut = 0.0;
    std::atomic<float> reductionPct { 0.0f };

    std::array<std::atomic<float>, scopeSize> scopeN {}, scopeR {};
    std::atomic<int> scopeWrite { 0 };
    float scN = 0.0f, scR = 0.0f;
    int scCount = 0;

    std::array<std::unique_ptr<SpectralCleaner>, 2> cleaners;
    std::atomic<int> activeChannels { 2 };
    float lastOutGain = 1.0f;

    std::array<std::atomic<float>, waveSize> waveIn {}, waveOut {};
    std::atomic<int> waveWrite { 0 };
    float accIn = 0.0f, accOut = 0.0f;
    int accCount = 0;

    std::atomic<float>* noiseParam  = nullptr;
    std::atomic<float>* smoothParam = nullptr;
    std::atomic<float>* reverbParam = nullptr;
    std::atomic<float>* tailParam   = nullptr;
    std::atomic<float>* depthParam  = nullptr;
    std::atomic<float>* mixParam    = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* deltaParam  = nullptr;
    std::atomic<float>* aiParam     = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NebulaAudioProcessor)
};
