// recorder.hpp
//
// Real-time microphone capture for the Zenoh audio publisher.
//
// The Recorder owns a PortAudio input stream. The audio callback runs on a
// high-priority real-time thread, so it does the bare minimum: copy the
// freshly captured samples into a lock-protected ring buffer and signal a
// waiting consumer. All heavy work (Opus encoding, Zenoh publishing) happens
// on the consumer thread that calls read_frame(), keeping the callback
// real-time-safe and latency low.
//
// Samples are 32-bit float, interleaved by channel, normalized to [-1, 1] —
// exactly the format Opus's float API expects.

#pragma once

#include <portaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

class Recorder {
public:
    // frame_samples is the number of samples *per channel* that read_frame()
    // returns per call (e.g. 960 for a 20 ms frame at 48 kHz). This is also
    // used as PortAudio's frames-per-buffer so each callback maps cleanly onto
    // one encoder frame.
    Recorder(int sample_rate, int channels, int frame_samples);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Open and start the input stream. Returns false on any PortAudio error
    // (the reason is printed to stderr).
    bool start();

    // Stop and close the stream. Safe to call more than once.
    void stop();

    // Block until one full frame (frame_samples * channels floats) is ready,
    // then move it into `out`. Returns false once the recorder is stopped and
    // no more buffered data remains.
    bool read_frame(std::vector<float>& out);

    // Number of frames dropped because the consumer could not keep up. A
    // non-zero, growing value means the encode/publish loop is too slow.
    unsigned long overruns() const { return overruns_.load(); }

private:
    static int PaCallback(const void* input, void* output,
                          unsigned long frame_count,
                          const PaStreamCallbackTimeInfo* time_info,
                          PaStreamCallbackFlags flags, void* user_data);

    void push(const float* in, std::size_t n_floats);

    const int sample_rate_;
    const int channels_;
    const std::size_t frame_floats_;  // frame_samples * channels

    PaStream* stream_ = nullptr;

    // Ring buffer of interleaved float samples.
    std::vector<float> ring_;
    std::size_t head_ = 0;   // read index
    std::size_t tail_ = 0;   // write index
    std::size_t count_ = 0;  // valid floats currently buffered
    std::mutex mtx_;
    std::condition_variable cv_;

    std::atomic<bool> running_{false};
    std::atomic<unsigned long> overruns_{0};
};
