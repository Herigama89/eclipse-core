# Eclipse Core — AI De-Noise + De-Reverb (VST3 / AU / Standalone)

One plugin, two cleaners, on a custom image skin (Assets/skin.jpg). The galaxy window shows live waveforms of what is being removed: white = noise, blue = room/reverb. INPUT, OUTPUT and REDUCTION meters glow inside the arcs.

## Controls
| Section | Knob | What it does |
|---|---|---|
| DE-NOISE | NOISE | How hard steady noise (hiss, hum, AC, fans, room tone) is removed. Gentle in the first half, aggressive near 10. |
| | SMOOTH | Artifact control. Higher = smoother, fewer "watery" artifacts; lower = faster, more precise. |
| DE-REVERB | REVERB | How much of the room's reverb tail is removed. |
| | TAIL | The room's decay time (T60). Match it to the room: small room ~0.3–0.6 s, hall 1.5–3 s. |
| OUTPUT | DEPTH | Maximum reduction (dB). Lower = more natural, higher = more complete removal. |
| | MIX | Dry/wet. |
| | OUTPUT | Output gain. |
| MONITOR | DRY VOCAL / AMBIENT | DRY VOCAL = the cleaned voice. AMBIENT = only what is being removed (delta). They null perfectly together. |
| | BYPASS | Latency-matched bypass (host bypass maps here too). |

Window is resizable (drag the corner). Double-click any knob to reset it.

**Workflow tip:** press AMBIENT, raise NOISE / REVERB until you start hearing voice in the ambient signal, then back off a little.

## AI engine
The AI ENGINE button (on by default) runs **DeepFilterNet 3**, an open-source neural network trained on
thousands of hours of speech and noise (MIT / Apache-2.0, https://github.com/Rikorose/DeepFilterNet).
With AI on, NOISE controls the neural de-noise (more natural, fewer artifacts), DEPTH caps it, and
SMOOTH low adds extra cleanup between words. DEREVERB / TAIL still run the room-reverb remover after it.
AI off = the original classic DSP de-noiser. Latency: about 90 ms total (reported to the DAW, auto-compensated).
Works at any sample rate (the AI runs internally at 48 kHz).

## Building
The AI engine is written in Rust, so install Rust once:
```
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
```

You need CMake 3.22+ and a C++17 compiler. JUCE is downloaded automatically.

### macOS (VST3 + AU)
Install Xcode (App Store), then:
```
cd NebulaClean
cmake -B build -G Xcode
cmake --build build --config Release
```
The plugins are copied automatically to `~/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/Components`.
Validate the AU: `auval -v aufx NbCl Nbla`

### Windows (VST3)
Install Visual Studio 2022 with "Desktop development with C++", then:
```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```
The VST3 is at `build/NebulaClean_artefacts/Release/VST3/Eclipse Core.vst3` (copy to `C:\Program Files\Common Files\VST3`; auto-copy needs an admin prompt). AU is Mac-only.

## Technical notes
- STFT: 2048-point FFT, 75 % overlap, Hann windows. Latency 2048 samples (reported to the host, auto-compensated).
- De-noise: minimum-tracking noise estimator with bias compensation + decision-directed Wiener gain.
- De-reverb: statistical late-reverberation model (Lebart/Habets) driven by the TAIL (T60) setting.
- Delta is computed in the spectral domain as X·(1−G), so it is sample-exact.
- This is classic DSP, not a neural network. It excels on steady noise and room tails; highly unpredictable noise (crowds, music bleed, sirens) needs a trained ML model, which could be added later (e.g. ONNX Runtime + an open speech-enhancement model).
