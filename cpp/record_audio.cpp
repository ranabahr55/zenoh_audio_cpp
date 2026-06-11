// record_audio.cpp
//
// Record 10 seconds of mono audio at 48 kHz (32-bit float) using PortAudio,
// then denoise and encode to FLAC with ffmpeg's `afftdn` (FFT adaptive
// denoiser, used in real production pipelines).
//
// Output: recording.flac
//
// Build:  make           (see Makefile)
// Run:    ./record_audio
//
// Why this design:
//   * PortAudio gives low-level, real audio-API capture (ALSA on Linux).
//   * We write a self-contained 32-bit float WAV with no third-party encoder
//     dependency, then hand it to ffmpeg for state-of-the-art denoise + FLAC.

#include <portaudio.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int   kSampleRate = 48000;   // professional standard
constexpr int   kChannels   = 1;       // mono
constexpr int   kDuration   = 10;      // seconds
constexpr int   kFrames     = kSampleRate * kDuration;
constexpr unsigned long kFramesPerBuffer = 512;

const char* kRawWav = "recording_raw.wav";
const char* kOutput = "recording.flac";

// Capture state shared with the PortAudio callback.
struct Capture {
    std::vector<float> samples;  // interleaved (mono here)
    size_t             written = 0;
};

int RecordCallback(const void* input, void* /*output*/,
                   unsigned long frameCount,
                   const PaStreamCallbackTimeInfo* /*timeInfo*/,
                   PaStreamCallbackFlags /*flags*/, void* userData) {
    auto* cap = static_cast<Capture*>(userData);
    const float* in = static_cast<const float*>(input);
    const size_t remaining = cap->samples.size() - cap->written;
    const size_t want = static_cast<size_t>(frameCount) * kChannels;
    const size_t n = want < remaining ? want : remaining;

    if (in) {
        std::memcpy(cap->samples.data() + cap->written, in, n * sizeof(float));
    } else {
        std::memset(cap->samples.data() + cap->written, 0, n * sizeof(float));
    }
    cap->written += n;
    return cap->written >= cap->samples.size() ? paComplete : paContinue;
}

// --- Minimal 32-bit float WAV writer (no libsndfile dependency) ------------
template <typename T>
void put(std::FILE* f, T v) { std::fwrite(&v, sizeof(T), 1, f); }

bool WriteFloatWav(const char* path, const std::vector<float>& data,
                   int sampleRate, int channels) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    const uint32_t dataBytes = static_cast<uint32_t>(data.size() * sizeof(float));
    const uint32_t byteRate  = sampleRate * channels * sizeof(float);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));

    std::fwrite("RIFF", 1, 4, f);
    put<uint32_t>(f, 36 + dataBytes);
    std::fwrite("WAVE", 1, 4, f);

    std::fwrite("fmt ", 1, 4, f);
    put<uint32_t>(f, 16);                       // fmt chunk size
    put<uint16_t>(f, 3);                        // format 3 = IEEE float
    put<uint16_t>(f, static_cast<uint16_t>(channels));
    put<uint32_t>(f, static_cast<uint32_t>(sampleRate));
    put<uint32_t>(f, byteRate);
    put<uint16_t>(f, blockAlign);
    put<uint16_t>(f, 32);                       // bits per sample

    std::fwrite("data", 1, 4, f);
    put<uint32_t>(f, dataBytes);
    std::fwrite(data.data(), sizeof(float), data.size(), f);

    std::fclose(f);
    return true;
}

bool CheckPa(PaError err, const char* what) {
    if (err != paNoError) {
        std::fprintf(stderr, "PortAudio error (%s): %s\n", what,
                     Pa_GetErrorText(err));
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!CheckPa(Pa_Initialize(), "init")) return 1;

    Capture cap;
    cap.samples.resize(static_cast<size_t>(kFrames) * kChannels);

    PaStreamParameters in{};
    in.device = Pa_GetDefaultInputDevice();
    if (in.device == paNoDevice) {
        std::fprintf(stderr, "No default input device.\n");
        Pa_Terminate();
        return 1;
    }
    in.channelCount = kChannels;
    in.sampleFormat = paFloat32;
    in.suggestedLatency =
        Pa_GetDeviceInfo(in.device)->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    if (!CheckPa(Pa_OpenStream(&stream, &in, nullptr, kSampleRate,
                               kFramesPerBuffer, paClipOff, RecordCallback,
                               &cap), "open")) {
        Pa_Terminate();
        return 1;
    }

    std::printf("Recording %d s at %d Hz ... speak now.\n", kDuration,
                kSampleRate);
    if (!CheckPa(Pa_StartStream(stream), "start")) { Pa_Terminate(); return 1; }
    while (Pa_IsStreamActive(stream) == 1) Pa_Sleep(100);
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    std::printf("Recording finished.\n");

    if (!WriteFloatWav(kRawWav, cap.samples, kSampleRate, kChannels)) {
        std::fprintf(stderr, "Failed to write %s\n", kRawWav);
        return 1;
    }

    // State-of-the-art denoise + FLAC encode via ffmpeg's afftdn.
    //   nr=40     : noise reduction amount (dB)
    //   nf=-35    : noise floor (dB)
    //   nt=w       : white-noise tracking, adapts to the take
    //   loudnorm   : gentle EBU R128 loudness normalization
    std::string cmd =
        "ffmpeg -y -loglevel error -i \"" + std::string(kRawWav) + "\" "
        "-af \"afftdn=nr=40:nf=-35:nt=s,loudnorm=I=-16:TP=-1.5:LRA=11\" "
        "-c:a flac -compression_level 12 -sample_fmt s32 \"" +
        std::string(kOutput) + "\"";

    std::printf("Denoising + encoding to FLAC ...\n");
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr,
                     "ffmpeg failed (rc=%d). Raw take kept at %s\n", rc, kRawWav);
        return 1;
    }

    std::remove(kRawWav);  // keep only the clean deliverable
    std::printf("Saved cleaned audio -> %s (%d s, FLAC, %d Hz)\n",
                kOutput, kDuration, kSampleRate);
    return 0;
}
