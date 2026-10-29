#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

/*  SpectralCleaner - one channel of STFT voice cleaning.

    DE-NOISE  : minimum-tracking noise-floor estimator + decision-directed Wiener gain
                (the classic Ephraim-Malah style approach, low "musical noise").
    DE-REVERB : statistical late-reverb model (Lebart / Habets). The late tail at frame t
                is predicted from the signal power ~50 ms earlier, attenuated by the
                exponential decay of a room with the chosen T60 ("TAIL" knob).
    DELTA     : output = X * (1 - G)  -> you hear exactly what is being removed.
                Normal + Delta always sum back to the original (null test).

    Latency = fftSize samples (reported to the host, so it is compensated).            */
class SpectralCleaner
{
public:
    static constexpr int fftOrder   = 11;
    static constexpr int fftSize    = 1 << fftOrder;   // 2048
    static constexpr int hopSize    = fftSize / 4;     // 75 % overlap
    static constexpr int numBins    = fftSize / 2 + 1;
    static constexpr int maxHistory = 64;

    struct Params
    {
        float noiseAmount  = 0.5f;   // 0..1
        float reverbAmount = 0.5f;   // 0..1
        float tailSeconds  = 1.2f;   // assumed room T60
        float depthDb      = 24.0f;  // maximum reduction per bin
        float smoothing    = 0.5f;   // 0..1  artifact control
        float mix          = 1.0f;   // 0..1
        bool  delta        = false;
    };

    SpectralCleaner() : fft (fftOrder)
    {
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

        inBuf.assign ((size_t) fftSize, 0.0f);
        outBuf.assign ((size_t) fftSize, 0.0f);
        fftData.assign ((size_t) (2 * fftSize), 0.0f);
        power.assign ((size_t) numBins, 0.0f);
        smoothPow.assign ((size_t) numBins, 0.0f);
        noiseEst.assign ((size_t) numBins, 0.0f);
        noiseSmooth.assign ((size_t) numBins, 0.0f);
        prevClean.assign ((size_t) numBins, 0.0f);
        gainRaw.assign ((size_t) numBins, 1.0f);
        gainSm.assign ((size_t) numBins, 1.0f);
        shareN.assign ((size_t) numBins, 0.0f);
        shareR.assign ((size_t) numBins, 0.0f);
        tapDataN.assign ((size_t) (2 * fftSize), 0.0f);
        tapDataR.assign ((size_t) (2 * fftSize), 0.0f);
        tapBufN.assign ((size_t) fftSize, 0.0f);
        tapBufR.assign ((size_t) fftSize, 0.0f);
        history.assign ((size_t) maxHistory * (size_t) numBins, 0.0f);
        reset();
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double fps = sr / (double) hopSize;
        delayFrames = std::clamp ((int) std::lround (0.05 * fps), 1, maxHistory - 1);
        noiseRise   = (float) std::pow (10.0, (8.0 / 10.0) / fps);   // noise floor may rise 8 dB/s
        reset();
    }

    void reset()
    {
        std::fill (inBuf.begin(), inBuf.end(), 0.0f);
        std::fill (outBuf.begin(), outBuf.end(), 0.0f);
        std::fill (smoothPow.begin(), smoothPow.end(), 0.0f);
        std::fill (noiseEst.begin(), noiseEst.end(), 0.0f);
        std::fill (noiseSmooth.begin(), noiseSmooth.end(), 0.0f);
        std::fill (prevClean.begin(), prevClean.end(), 0.0f);
        std::fill (gainSm.begin(), gainSm.end(), 1.0f);
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (tapBufN.begin(), tapBufN.end(), 0.0f);
        std::fill (tapBufR.begin(), tapBufR.end(), 0.0f);
        tapNoise = tapReverb = 0.0f;
        for (auto& a : inputPower)  a.store (0.0f, std::memory_order_relaxed);
        for (auto& a : outputPower) a.store (0.0f, std::memory_order_relaxed);
        pos = hopCount = histPos = 0;
        warmup = 6;
        mixSm = params.mix;
        deltaSm = params.delta ? 1.0f : 0.0f;
    }

    void setParams (const Params& p) noexcept { params = p; }

    // Analysis tap: when enabled, the removed signal is split into its noise part and its
    // reverb part (time domain, aligned with the output). Read tapNoise / tapReverb after
    // each processSample().
    void setTapEnabled (bool shouldTap) noexcept { tapEnabled = shouldTap; }
    float tapNoise = 0.0f, tapReverb = 0.0f;

    float processSample (float x) noexcept
    {
        inBuf[(size_t) pos] = x;
        const float y = outBuf[(size_t) pos];
        outBuf[(size_t) pos] = 0.0f;
        if (tapEnabled)
        {
            tapNoise  = tapBufN[(size_t) pos];  tapBufN[(size_t) pos] = 0.0f;
            tapReverb = tapBufR[(size_t) pos];  tapBufR[(size_t) pos] = 0.0f;
        }
        pos = (pos + 1) & mask;

        if (++hopCount >= hopSize)
        {
            hopCount = 0;
            processFrame();
        }
        return y;
    }

    // Analyzer taps (written on the audio thread, read by the GUI)
    std::array<std::atomic<float>, numBins> inputPower {}, outputPower {};

private:
    void processFrame() noexcept
    {
        // ---- analysis ----------------------------------------------------------
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = inBuf[(size_t) ((pos + i) & mask)] * window[(size_t) i];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performRealOnlyForwardTransform (fftData.data(), true);

        const float floorGain = std::pow (10.0f, -params.depthDb / 20.0f);
        const float betaN     = 1.2f * params.noiseAmount + 1.3f * params.noiseAmount * params.noiseAmount * params.noiseAmount;
        const float betaR     = 1.6f * params.reverbAmount;
        const double decay    = 3.0 * std::log (10.0) / std::max (0.1, (double) params.tailSeconds);
        const float revGain   = (float) std::exp (-2.0 * decay * (double) delayFrames * hopSize / sr);
        const float dd        = 0.80f + 0.18f * params.smoothing;   // decision-directed factor
        const float release   = 0.15f + 0.60f * params.smoothing;   // gain release smoothing
        const float freqSm    = 0.50f * params.smoothing;

        const int oldIdx = (histPos - delayFrames + maxHistory) % maxHistory;
        float* histNow       = &history[(size_t) histPos * (size_t) numBins];
        const float* histOld = &history[(size_t) oldIdx  * (size_t) numBins];

        // ---- pass 1: estimate noise + reverb, compute raw Wiener gain ----------
        for (int k = 0; k < numBins; ++k)
        {
            const float re = fftData[(size_t) (2 * k)], im = fftData[(size_t) (2 * k + 1)];
            const float p  = re * re + im * im;
            power[(size_t) k] = p;

            float& sp = smoothPow[(size_t) k];
            sp = 0.6f * sp + 0.4f * p;

            float& ns = noiseSmooth[(size_t) k];              // heavily smoothed power for tracking
            ns = 0.8f * ns + 0.2f * p;

            float& n = noiseEst[(size_t) k];
            if (warmup > 0)       n = ns;                    // window not full yet
            else if (ns < n)      n = 0.7f * n + 0.3f * ns;  // follow minima quickly
            else                  n *= noiseRise;            // creep up slowly
            n = std::max (n, 1.0e-12f);
            const float noisePow = minStatBias * n;          // minimum -> mean bias compensation

            const float rev = revGain * histOld[k];           // predicted late reverb
            histNow[k] = sp;

            const float interf = betaN * noisePow + betaR * rev + 1.0e-12f;
            shareN[(size_t) k] = betaN * noisePow / interf;
            shareR[(size_t) k] = betaR * rev / interf;
            const float post   = p / interf;
            const float prio   = dd * prevClean[(size_t) k] / interf + (1.0f - dd) * std::max (post - 1.0f, 0.0f);
            gainRaw[(size_t) k] = std::max (prio / (1.0f + prio), floorGain);
        }

        // ---- per-frame parameter smoothing (click-free delta / mix / bypass) ---
        mixSm   += 0.35f * (params.mix - mixSm);
        deltaSm += 0.35f * ((params.delta ? 1.0f : 0.0f) - deltaSm);

        // ---- pass 2: smooth gains, apply, feed analyzer ------------------------
        for (int k = 0; k < numBins; ++k)
        {
            const float l = gainRaw[(size_t) std::max (k - 1, 0)];
            const float r = gainRaw[(size_t) std::min (k + 1, numBins - 1)];
            const float gf = (1.0f - freqSm) * gainRaw[(size_t) k] + freqSm * 0.5f * (l + r);

            float& g = gainSm[(size_t) k];
            g = gf > g ? gf : release * g + (1.0f - release) * gf;   // instant open, smooth close

            prevClean[(size_t) k] = g * g * power[(size_t) k];

            const float m   = mixSm * g + (1.0f - mixSm);
            const float out = (1.0f - deltaSm) * m + deltaSm * (1.0f - m);

            if (tapEnabled)
            {
                const float rem = 1.0f - g;
                const float re = fftData[(size_t) (2 * k)], im = fftData[(size_t) (2 * k + 1)];
                tapDataN[(size_t) (2 * k)]     = re * rem * shareN[(size_t) k];
                tapDataN[(size_t) (2 * k + 1)] = im * rem * shareN[(size_t) k];
                tapDataR[(size_t) (2 * k)]     = re * rem * shareR[(size_t) k];
                tapDataR[(size_t) (2 * k + 1)] = im * rem * shareR[(size_t) k];
            }

            fftData[(size_t) (2 * k)]     *= out;
            fftData[(size_t) (2 * k + 1)] *= out;

            inputPower[(size_t) k].store (smoothPow[(size_t) k], std::memory_order_relaxed);
            outputPower[(size_t) k].store (smoothPow[(size_t) k] * g * g, std::memory_order_relaxed);
        }

        // Hermitian mirror so the inverse is valid on every FFT backend
        for (int k = 1; k < fftSize / 2; ++k)
        {
            fftData[(size_t) (2 * (fftSize - k))]     =  fftData[(size_t) (2 * k)];
            fftData[(size_t) (2 * (fftSize - k) + 1)] = -fftData[(size_t) (2 * k + 1)];
        }

        // ---- synthesis (Hann^2 at 75 % overlap sums to 1.5) --------------------
        fft.performRealOnlyInverseTransform (fftData.data());
        constexpr float olaScale = 1.0f / 1.5f;
        for (int i = 0; i < fftSize; ++i)
            outBuf[(size_t) ((pos + i) & mask)] += fftData[(size_t) i] * window[(size_t) i] * olaScale;

        if (tapEnabled)
            synthesiseTap (tapDataN, tapBufN), synthesiseTap (tapDataR, tapBufR);

        histPos = (histPos + 1) % maxHistory;
        if (warmup > 0) --warmup;
    }

    void synthesiseTap (std::vector<float>& data, std::vector<float>& buf) noexcept
    {
        for (int k = 1; k < fftSize / 2; ++k)
        {
            data[(size_t) (2 * (fftSize - k))]     =  data[(size_t) (2 * k)];
            data[(size_t) (2 * (fftSize - k) + 1)] = -data[(size_t) (2 * k + 1)];
        }
        fft.performRealOnlyInverseTransform (data.data());
        for (int i = 0; i < fftSize; ++i)
            buf[(size_t) ((pos + i) & mask)] += data[(size_t) i] * window[(size_t) i] * (1.0f / 1.5f);
    }

    static constexpr int mask = fftSize - 1;

    juce::dsp::FFT fft;
    std::vector<float> window, inBuf, outBuf, fftData;
    std::vector<float> shareN, shareR, tapDataN, tapDataR, tapBufN, tapBufR;
    bool tapEnabled = false;
    std::vector<float> power, smoothPow, noiseEst, noiseSmooth, prevClean, gainRaw, gainSm, history;

    Params params;
    double sr = 48000.0;
    int pos = 0, hopCount = 0, histPos = 0, delayFrames = 5;
    static constexpr float minStatBias = 2.0f;
    float noiseRise = 1.01f, mixSm = 1.0f, deltaSm = 0.0f;
    int warmup = 6;
};
