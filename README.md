# audio2 — 10-Second Noise-Reduced Recorder (C++)

A self-contained C++ implementation that records **exactly 10 seconds** of audio,
suppresses background noise, and saves the result with a **state-of-the-art**
lossless format (**32-bit FLAC**).

```
audio2/
└── cpp/      # PortAudio capture + ffmpeg afftdn denoiser, saves FLAC
```

Produces `recording.flac` in the `cpp/` folder.

## Quick start

```bash
cd cpp
make
./record_audio        # or: make run
```
Requires `libportaudio` (dev headers) and `ffmpeg` on PATH — both present here.

> The program prints some harmless ALSA warnings (`Unknown PCM cards.pcm.rear`,
> `/dev/dsp`, etc.) on startup. They do not affect the recording. To hide them,
> run `./record_audio 2>/dev/null`.

## Design choices

**Capture**
- 48 kHz sample rate — current professional standard.
- 32-bit float internal path — maximum fidelity, no early quantization.
- Real audio capture via PortAudio.

**Noise reduction**
- ffmpeg's `afftdn` (FFT adaptive denoiser) followed by EBU R128 `loudnorm`.
  `afftdn` is a production-grade denoiser.

**Saving**
- **FLAC** — lossless, compressed (~half the size of WAV), with broad metadata
  and player support. Written as 32-bit via ffmpeg `-compression_level 12`.

## Notes
- The recorder uses the system **default input device**. Pick a different mic by
  setting it as default.
- Speak during the 10-second window after the "Recording..." prompt.
- `recording.flac` is written to whatever folder you run the program from, so
  `cd cpp` first to keep the output there.
