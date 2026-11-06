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
    DE-REVERB : late-reverb model driven by the *direct* voice (Habets-style recursion).
                Each frame, the estimated dry voice feeds a reverb tail that decays at the
                room's rate (TAIL = T60) and arrives after ~50 ms. Because only decaying
                energy of the dry voice counts as reverb, sustained (sung) notes are no
                longer mistaken for reverb. REVERB = how roomy the recording is.
    SMOOTHING : decision-directed SNR with onset fast-path, ERB-width frequency smoothing
                and millisecond-based release -> low "musical noise", no watery wobble.
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
        lateRev.assign ((size_t) numBins, 0.0f);
        dropping.assign ((size_t) numBins, 0.0f);
        gainTmp.assign ((size_t) numBins + 1, 0.0f);
        reset();
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double fps = sr / (double) hopSize;
        delayFrames = std::clamp ((int) std::lround (0.05 * fps), 1, maxHistory - 1);
        noiseRise   = (float) std::pow (10.0, (8.0 / 10.0) / fps);   // noise floor may rise 8 dB/s
        hopSeconds  = (float) (hopSize / sr);
        for (int k = 0; k < numBins; ++k)                              // ERB bandwidth in bins
        {
            const double f   = k * sr / fftSize;
            const double erb = 24.7 * (4.37 * f / 1000.0 + 1.0);
            erbBins[(size_t) k] = (float) (erb / (sr / fftSize));
        }
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
        std::fill (lateRev.begin(), lateRev.end(), 0.0f);
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
        // assumed reverb-to-direct energy ratio: 50 % ~ a normal room (DRR ~ +4 dB), 100 % ~ very roomy
        const float rho       = params.reverbAmount <= 0.0f ? 0.0f : 0.40f * std::pow (2.0f * params.reverbAmount, 2.3f);
        const double decay    = 3.0 * std::log (10.0) / std::max (0.1, (double) params.tailSeconds);
        const float dFrame    = (float) std::exp (-2.0 * decay * hopSize / sr);          // power decay per frame
        const float lateW     = (float) std::pow ((double) dFrame, (double) delayFrames); // energy surviving to "late"
        const float feed      = rho * (1.0f - dFrame) * lateW;
        const float sm        = params.smoothing;
        const float dd        = 0.90f + 0.08f * sm;                                       // decision-directed factor
        const float release   = std::exp (-hopSeconds / (0.005f + 0.025f * sm));         // 5..30 ms release
        const float widthFrac = 0.6f * sm;                                                // fraction of an ERB

        const int oldIdx = (histPos - delayFrames + maxHistory) % maxHistory;
        float* histNow       = &history[(size_t) histPos * (size_t) numBins];
        const float* histOld = &history[(size_t) oldIdx  * (size_t) numBins];

        // ---- pass 1: estimate noise + late reverb, compute raw gain ------------
        for (int k = 0; k < numBins; ++k)
        {
            const float re = fftData[(size_t) (2 * k)], im = fftData[(size_t) (2 * k + 1)];
            const float p  = re * re + im * im;
            power[(size_t) k] = p;

            float& sp = smoothPow[(size_t) k];
            const float spPrev = sp;
            sp = 0.6f * sp + 0.4f * p;

            float& ns = noiseSmooth[(size_t) k];              // heavily smoothed power for tracking
            ns = 0.8f * ns + 0.2f * p;

            float& n = noiseEst[(size_t) k];
            if (warmup > 0)       n = ns;                    // window not full yet
            else if (ns < n)      n = 0.7f * n + 0.3f * ns;  // follow minima quickly
            else                  n *= noiseRise;            // creep up slowly
            n = std::max (n, 1.0e-12f);
            const float noisePow = minStatBias * n;          // minimum -> mean bias compensation

            // late reverb: decaying tail of the *direct* voice estimated ~50 ms ago
            float& L = lateRev[(size_t) k];
            L = dFrame * L + feed * histOld[k];

            // early tail (< 50 ms), only where the bin is dropping off (word endings),
            // so sustained notes keep their body
            float early = 0.0f, peak = 0.0f, w = 1.0f;
            for (int j = 1; j < delayFrames; ++j)
            {
                const float dj = history[(size_t) ((histPos - j + maxHistory) % maxHistory) * (size_t) numBins + (size_t) k];
                w *= dFrame;
                early += w * dj;
                peak = std::max (peak, dj);
            }
            const float q    = sp / (peak + 1.0e-12f);
            const float drop = std::clamp ((0.5f - q) / 0.4f, 0.0f, 1.0f);
            const float rev  = L + drop * rho * (1.0f - dFrame) * early;

            const float interf = betaN * noisePow + rev + 1.0e-12f;
            shareN[(size_t) k] = betaN * noisePow / interf;
            shareR[(size_t) k] = rev / interf;
            const float post   = p / interf;
            // smooth when the bin is steady; let go fast once the interference dominates (word endings)
            const float ddk = post < 1.5f ? 0.5f : dd;
            float prio = ddk * prevClean[(size_t) k] / interf + (1.0f - ddk) * std::max (post - 1.0f, 0.0f);
            if (post > 8.0f && p > 2.0f * spPrev) prio = std::max (prio, post - 1.0f);   // true onsets only: never dull consonants
            gainRaw[(size_t) k] = std::max (prio / (1.0f + prio), floorGain);
            dropping[(size_t) k] = drop;
        }

        // ---- frequency smoothing over a fraction of an ERB (box filter) --------
        if (widthFrac > 0.0f)
        {
            gainTmp[0] = 0.0f;
            for (int k = 0; k < numBins; ++k) gainTmp[(size_t) k + 1] = gainTmp[(size_t) k] + gainRaw[(size_t) k];
            for (int k = 0; k < numBins; ++k)
            {
                const int hw = (int) (0.5f * widthFrac * erbBins[(size_t) k]);
                if (hw <= 0) continue;
                const int lo = std::max (0, k - hw), hi = std::min (numBins - 1, k + hw);
                const float avg = (gainTmp[(size_t) hi + 1] - gainTmp[(size_t) lo]) / (float) (hi - lo + 1);
                gainRaw[(size_t) k] = std::max (gainRaw[(size_t) k] * 0.35f + avg * 0.65f, floorGain);
            }
        }

        // ---- per-frame parameter smoothing (click-free delta / mix / bypass) ---
        mixSm   += 0.35f * (params.mix - mixSm);
        deltaSm += 0.35f * ((params.delta ? 1.0f : 0.0f) - deltaSm);

        // ---- pass 2: smooth gains, apply, feed analyzer ------------------------
        for (int k = 0; k < numBins; ++k)
        {
            const float gf = gainRaw[(size_t) k];

            float& g = gainSm[(size_t) k];
            const float rel = release * (1.0f - dropping[(size_t) k]);   // no hang-over at word endings
            g = gf > g ? gf : rel * g + (1.0f - rel) * gf;               // instant open, smooth close

            prevClean[(size_t) k] = g * g * power[(size_t) k];
            // direct-voice estimate (reverb removed) that will drive the reverb model
            const float gr = std::min (1.0f, 1.0f - shareR[(size_t) k] * (1.0f - g));
            histNow[k] = gr * gr * smoothPow[(size_t) k];

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
    std::vector<float> power, smoothPow, noiseEst, noiseSmooth, prevClean, gainRaw, gainSm, history, lateRev, gainTmp, dropping;
    std::array<float, numBins> erbBins {};
    float hopSeconds = 512.0f / 48000.0f;

    Params params;
    double sr = 48000.0;
    int pos = 0, hopCount = 0, histPos = 0, delayFrames = 5;
    static constexpr float minStatBias = 2.0f;
    float noiseRise = 1.01f, mixSm = 1.0f, deltaSm = 0.0f;
    int warmup = 6;
};
