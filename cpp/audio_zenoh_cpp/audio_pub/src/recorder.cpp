// recorder.cpp — see recorder.hpp for the design rationale.

#include "recorder.hpp"

#include <cstdio>
#include <cstring>

Recorder::Recorder(int sample_rate, int channels, int frame_samples)
    : sample_rate_(sample_rate),
      channels_(channels),
      frame_floats_(static_cast<std::size_t>(frame_samples) * channels) {
    // Hold ~0.5 s of audio so brief consumer hiccups don't drop samples,
    // while still bounding worst-case latency if the consumer stalls.
    std::size_t cap = frame_floats_ *
                      static_cast<std::size_t>((sample_rate / frame_samples) / 2 + 4);
    ring_.assign(cap, 0.0f);
}

Recorder::~Recorder() { stop(); }

bool Recorder::start() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return false;
    }

    PaStreamParameters in{};
    in.device = Pa_GetDefaultInputDevice();
    if (in.device == paNoDevice) {
        std::fprintf(stderr, "No default input device.\n");
        Pa_Terminate();
        return false;
    }
    in.channelCount = channels_;
    in.sampleFormat = paFloat32;
    in.suggestedLatency = Pa_GetDeviceInfo(in.device)->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    const unsigned long frames_per_buffer = frame_floats_ / channels_;

    err = Pa_OpenStream(&stream_, &in, /*output=*/nullptr, sample_rate_,
                        frames_per_buffer, paClipOff, &Recorder::PaCallback, this);
    if (err != paNoError) {
        std::fprintf(stderr, "Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return false;
    }

    running_.store(true);
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        std::fprintf(stderr, "Pa_StartStream failed: %s\n", Pa_GetErrorText(err));
        running_.store(false);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        Pa_Terminate();
        return false;
    }

    const PaStreamInfo* info = Pa_GetStreamInfo(stream_);
    std::fprintf(stderr, "Capture started: %d Hz, %d ch, input latency %.1f ms\n",
                 sample_rate_, channels_,
                 info ? info->inputLatency * 1000.0 : -1.0);
    return true;
}

void Recorder::stop() {
    if (!running_.exchange(false) && !stream_) return;

    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        Pa_Terminate();
    }
    // Wake any consumer blocked in read_frame() so it can return false.
    cv_.notify_all();
}

int Recorder::PaCallback(const void* input, void* /*output*/,
                         unsigned long frame_count,
                         const PaStreamCallbackTimeInfo* /*time_info*/,
                         PaStreamCallbackFlags /*flags*/, void* user_data) {
    auto* self = static_cast<Recorder*>(user_data);
    if (input) {
        self->push(static_cast<const float*>(input),
                   static_cast<std::size_t>(frame_count) * self->channels_);
    }
    return self->running_.load() ? paContinue : paComplete;
}

void Recorder::push(const float* in, std::size_t n_floats) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::size_t cap = ring_.size();

        // If the producer is about to overflow the buffer, drop the oldest
        // whole frames to make room. Real-time audio favors the freshest
        // samples over a backlog.
        if (count_ + n_floats > cap) {
            std::size_t need = count_ + n_floats - cap;
            // Round up to whole frames so the ring stays frame-aligned.
            std::size_t drop = ((need + frame_floats_ - 1) / frame_floats_) * frame_floats_;
            if (drop > count_) drop = count_;
            head_ = (head_ + drop) % cap;
            count_ -= drop;
            overruns_.fetch_add(drop / frame_floats_);
        }

        for (std::size_t i = 0; i < n_floats; ++i) {
            ring_[tail_] = in[i];
            tail_ = (tail_ + 1) % cap;
        }
        count_ += n_floats;
    }
    cv_.notify_one();
}

bool Recorder::read_frame(std::vector<float>& out) {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return count_ >= frame_floats_ || !running_.load(); });

    if (count_ < frame_floats_) return false;  // stopped and drained

    out.resize(frame_floats_);
    const std::size_t cap = ring_.size();
    for (std::size_t i = 0; i < frame_floats_; ++i) {
        out[i] = ring_[head_];
        head_ = (head_ + 1) % cap;
    }
    count_ -= frame_floats_;
    return true;
}
