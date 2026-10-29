#include "PluginProcessor.h"
#include "PluginEditor.h"

NebulaAudioProcessor::NebulaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    for (auto& c : cleaners)
        c = std::make_unique<SpectralCleaner>();
    cleaners[0]->setTapEnabled (true);   // channel 0 feeds the removed-signal scope

    noiseParam  = apvts.getRawParameterValue ("noise");
    smoothParam = apvts.getRawParameterValue ("smooth");
    reverbParam = apvts.getRawParameterValue ("reverb");
    tailParam   = apvts.getRawParameterValue ("tail");
    depthParam  = apvts.getRawParameterValue ("depth");
    mixParam    = apvts.getRawParameterValue ("mix");
    outputParam = apvts.getRawParameterValue ("output");
    deltaParam  = apvts.getRawParameterValue ("delta");
    aiParam     = apvts.getRawParameterValue ("ai");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    if (! apvts.state.hasProperty ("darkMode"))
        apvts.state.setProperty ("darkMode", true, nullptr);

    setLatencySamples (SpectralCleaner::fftSize);
}

NebulaAudioProcessor::~NebulaAudioProcessor()
{
    releaseAI();
}

void NebulaAudioProcessor::releaseAI()
{
    aiAvailable.store (false);
    for (auto& c : aiCh)
    {
        if (c.engine != nullptr)
            eclipse_ai_free (c.engine);
        c.engine = nullptr;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout NebulaAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pct = [] (float v, int) { return String (roundToInt (v)) + " %"; };
    auto attrPct = [pct] { return AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pct); };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "noise", 1 },  "Noise",  NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, attrPct()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "smooth", 1 }, "Smooth", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, attrPct()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "reverb", 1 }, "Reverb", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, attrPct()));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tail", 1 }, "Tail",
        NormalisableRange<float> (0.2f, 4.0f, 0.01f, 0.5f), 1.2f,
        AudioParameterFloatAttributes().withLabel ("s")
            .withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " s"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "depth", 1 }, "Depth",
        NormalisableRange<float> (0.0f, 48.0f, 0.1f), 24.0f,
        AudioParameterFloatAttributes().withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " dB"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, attrPct()));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "delta", 1 },  "Delta",  false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "ai", 1 },     "AI Engine", true));

    return layout;
}

void NebulaAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    for (auto& c : cleaners)
        c->prepare (sampleRate);

    // ---- AI engines (DeepFilterNet 3), one per channel ----------------------------
    releaseAI();
    bool ok = true;
    for (auto& c : aiCh)
    {
        c.engine = eclipse_ai_create (sampleRate, juce::jmax (1, samplesPerBlock));
        ok = ok && c.engine != nullptr;
    }
    if (! ok) releaseAI();
    aiLatency = ok ? eclipse_ai_latency (aiCh[0].engine) : 0;
    aiAvailable.store (ok);

    for (auto& c : aiCh)
    {
        c.dryToAI.setLength (aiLatency);
        c.dryToOut.setLength (SpectralCleaner::fftSize);
        c.removedToOut.setLength (SpectralCleaner::fftSize);
    }
    aiIn.assign ((size_t) juce::jmax (512, samplesPerBlock), 0.0f);
    aiOut.assign (aiIn.size(), 0.0f);
    inPeak.assign (aiIn.size(), 0.0f);

    smoothCoef = 1.0f - std::exp (-1.0f / (0.03f * (float) sampleRate));   // ~30 ms parameter glide
    aiWeight = isAIOn() ? 1.0f : 0.0f;
    mixSm = mixParam->load() * 0.01f;
    deltaSm = deltaParam->load() > 0.5f ? 1.0f : 0.0f;
    eIn = eOut = 0.0;

    lastOutGain = juce::Decibels::decibelsToGain (outputParam->load());
    setLatencySamples (aiLatency + SpectralCleaner::fftSize);
}

bool NebulaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void NebulaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);

    const bool bypassed = bypassParam != nullptr && bypassParam->get();

    const float noise   = noiseParam->load() * 0.01f;
    const float depthDb = depthParam->load();
    const bool  aiOn    = isAIOn();

    // Classic engine: de-reverb always; de-noise only when the AI is off.
    // Mix and Delta are handled below so they cover both stages.
    SpectralCleaner::Params p;
    p.noiseAmount  = aiOn ? 0.0f : noise;
    p.smoothing    = smoothParam->load() * 0.01f;
    p.reverbAmount = reverbParam->load() * 0.01f;
    p.tailSeconds  = tailParam->load();
    p.depthDb      = depthDb;
    p.mix          = 1.0f;
    p.delta        = false;

    // AI: NOISE sets how much it removes (capped by DEPTH); SMOOTH low = extra cleanup between words
    const float aiDb = juce::jlimit (1.0f, 100.0f, juce::jmin (depthDb, 6.0f + 54.0f * noise));
    const float pfBeta = 0.03f * (1.0f - smoothParam->load() * 0.01f);
    const float aiTarget  = aiOn ? juce::jmin (1.0f, noise / 0.05f) : 0.0f;   // 0 % NOISE = untouched
    const float mixTarget = bypassed ? 0.0f : mixParam->load() * 0.01f;
    const float dTarget   = (! bypassed && deltaParam->load() > 0.5f) ? 1.0f : 0.0f;

    const int nCh = juce::jmin (numIn, 2);
    activeChannels.store (juce::jmax (1, nCh));
    const float target = bypassed ? 1.0f : juce::Decibels::decibelsToGain (outputParam->load());

    for (int ch = 0; ch < nCh; ++ch)
    {
        cleaners[(size_t) ch]->setParams (p);
        if (auto* e = aiCh[(size_t) ch].engine)
        {
            eclipse_ai_set_reduction_db (e, aiDb);
            eclipse_ai_set_post_filter (e, pfBeta);
        }
    }

    const int chunk = (int) aiIn.size();
    double blockIn = 0.0, blockOut = 0.0;

    for (int start = 0; start < n; start += chunk)
    {
        const int len = juce::jmin (chunk, n - start);
        float wAI = aiWeight, wMix = mixSm, wD = deltaSm;

        for (int ch = 0; ch < nCh; ++ch)
        {
            auto& a = aiCh[(size_t) ch];
            auto& cl = *cleaners[(size_t) ch];
            float* d = buffer.getWritePointer (ch, start);

            // 1) AI stage (always running so it is warm and the latency never changes)
            if (a.engine != nullptr)
            {
                std::copy (d, d + len, aiIn.begin());
                eclipse_ai_process (a.engine, aiIn.data(), aiOut.data(), len);
            }
            else
            {
                std::copy (d, d + len, aiOut.begin());
            }

            wAI = aiWeight; wMix = mixSm; wD = deltaSm;
            for (int i = 0; i < len; ++i)
            {
                wAI  += smoothCoef * (aiTarget  - wAI);
                wMix += smoothCoef * (mixTarget - wMix);
                wD   += smoothCoef * (dTarget   - wD);

                const float x    = d[i];
                inPeak[(size_t) i] = ch == 0 ? std::abs (x) : juce::jmax (inPeak[(size_t) i], std::abs (x));
                const float dry1 = a.dryToAI.process (x);                      // aligned with AI output
                const float s1   = dry1 + wAI * (aiOut[(size_t) i] - dry1);    // after AI de-noise

                // 2) classic de-reverb (+ de-noise when AI is off)
                const float wet  = cl.processSample (s1);
                const float dryD = a.dryToOut.process (dry1);                  // aligned with final output
                const float aiRemoved = a.removedToOut.process (dry1 - s1);

                const float mixed = dryD + wMix * (wet - dryD);
                const float y = (1.0f - wD) * mixed + wD * (dryD - mixed);     // AMBIENT = what was removed
                d[i] = y;

                blockIn  += (double) dryD * dryD;
                blockOut += (double) mixed * mixed;

                if (ch == 0)
                {
                    const float nPart = aiRemoved + cl.tapNoise;
                    const float rPart = cl.tapReverb;
                    if (std::abs (nPart) > std::abs (scN)) scN = nPart;
                    if (std::abs (rPart) > std::abs (scR)) scR = rPart;
                    if (++scCount >= scopeChunk)
                    {
                        const int w = scopeWrite.load (std::memory_order_relaxed);
                        scopeN[(size_t) w].store (scN, std::memory_order_relaxed);
                        scopeR[(size_t) w].store (scR, std::memory_order_relaxed);
                        scopeWrite.store ((w + 1) % scopeSize, std::memory_order_release);
                        scN = scR = 0.0f;
                        scCount = 0;
                    }
                }
            }
        }
        aiWeight = wAI; mixSm = wMix; deltaSm = wD;

        // meters (dry in vs processed out)
        for (int i = 0; i < len; ++i)
        {
            float inMax = 0.0f, outMax = 0.0f;
            for (int ch = 0; ch < nCh; ++ch)
                outMax = juce::jmax (outMax, std::abs (buffer.getSample (ch, start + i)));
            inMax = inPeak[(size_t) i];
            accIn = juce::jmax (accIn, inMax);
            accOut = juce::jmax (accOut, outMax * target);
            if (++accCount >= waveChunk)
            {
                const int w = waveWrite.load (std::memory_order_relaxed);
                waveIn[(size_t) w].store (accIn, std::memory_order_relaxed);
                waveOut[(size_t) w].store (accOut, std::memory_order_relaxed);
                waveWrite.store ((w + 1) % waveSize, std::memory_order_release);
                accIn = accOut = 0.0f;
                accCount = 0;
            }
        }
    }

    // reduction % from the real dry vs. processed energy
    eIn  = 0.85 * eIn  + 0.15 * blockIn;
    eOut = 0.85 * eOut + 0.15 * blockOut;
    reductionPct.store (eIn > 1.0e-9 ? (float) juce::jlimit (0.0, 100.0, 100.0 * (1.0 - eOut / eIn)) : 0.0f,
                        std::memory_order_relaxed);

    for (int ch = 0; ch < nCh; ++ch)
        buffer.applyGainRamp (ch, 0, n, lastOutGain, target);
    lastOutGain = target;
}

void NebulaAudioProcessor::getAnalyzerData (std::vector<float>& inPow, std::vector<float>& outPow) const
{
    const auto nb = (size_t) SpectralCleaner::numBins;
    inPow.assign (nb, 0.0f);
    outPow.assign (nb, 0.0f);

    const int nCh = activeChannels.load();
    for (int ch = 0; ch < nCh; ++ch)
    {
        const auto& c = *cleaners[(size_t) ch];
        for (size_t k = 0; k < nb; ++k)
        {
            inPow[k]  += c.inputPower[k].load (std::memory_order_relaxed);
            outPow[k] += c.outputPower[k].load (std::memory_order_relaxed);
        }
    }
    const float norm = 1.0f / (float) nCh;
    for (size_t k = 0; k < nb; ++k) { inPow[k] *= norm; outPow[k] *= norm; }
}

void NebulaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void NebulaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* NebulaAudioProcessor::createEditor()
{
    return new NebulaAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NebulaAudioProcessor();
}
