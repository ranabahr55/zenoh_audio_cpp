// main.cpp — Real-time Opus-over-Zenoh audio PLAYER (live, minimum latency).
//
// Pipeline (the listening end of the pub/sub pair):
//
//   Zenoh sub --> parse [seq | opus packet] --> Opus decode --> PortAudio out
//
// Unlike audio_sub.py (which decodes to a FLAC file), this program plays the
// audio straight to your speakers the moment packets arrive. It is built for
// low latency:
//
//   * Decoding happens directly in the Zenoh subscriber callback (Opus decode
//     is sub-millisecond), so there is no extra thread hop between "packet
//     received" and "samples ready to play".
//   * A tiny jitter FIFO sits between the network thread and the PortAudio
//     output callback. We start playback after only `prebuffer_ms` of audio
//     (default 40 ms) and cap the FIFO at `max_buffer_ms` so latency can never
//     grow unbounded — if we ever fall behind we drop the oldest samples to
//     stay live rather than play stale audio.
//   * PortAudio's output stream uses the device's low-latency hint and the
//     callback never blocks (an underrun just emits silence), so the audio
//     thread stays real-time safe.
//
// It reads the SAME config.cfg the publisher uses (defaults to
// ../../audio_pub/config.cfg) so the Zenoh key, sample rate, channels and
// frame size always agree with the sender. Two extra optional keys tune the
// jitter buffer:
//
//   prebuffer_ms   = 40    # audio buffered before playback starts (lower = less latency)
//   max_buffer_ms  = 200   # hard cap on buffered audio (drops oldest beyond this)

#include <opus/opus.h>
#include <portaudio.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <cerrno>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "zenoh.hxx"

using namespace zenoh;

namespace {

std::atomic<bool> g_stop{false};
void HandleSignal(int) { g_stop.store(true); }

// ---------------------------------------------------------------------------
// Tiny `key = value` config reader (# starts a comment, anywhere on the line).
// Mirrors the publisher's reader so both ends parse config.cfg identically.
// ---------------------------------------------------------------------------
class AppConfig {
public:
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (!key.empty()) values_[key] = val;
        }
        return true;
    }
    std::string str(const std::string& k, const std::string& def) const {
        auto it = values_.find(k);
        return it == values_.end() ? def : it->second;
    }
    int integer(const std::string& k, int def) const {
        auto it = values_.find(k);
        return it == values_.end() ? def : std::stoi(it->second);
    }
    bool boolean(const std::string& k, bool def) const {
        auto it = values_.find(k);
        if (it == values_.end()) return def;
        std::string v = it->second;
        for (auto& c : v) c = static_cast<char>(::tolower(c));
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

private:
    static std::string trim(std::string s) {
        const char* ws = " \t\r\n";
        auto b = s.find_first_not_of(ws);
        if (b == std::string::npos) return "";
        auto e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    }
    std::map<std::string, std::string> values_;
};

// JSON5 string-array literal: tcp/a;tcp/b  ->  ["tcp/a","tcp/b"]
std::string to_json5_endpoints(const std::string& csv) {
    std::stringstream ss(csv);
    std::string item;
    std::string out = "[";
    bool first = true;
    while (std::getline(ss, item, ';')) {
        auto b = item.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = item.find_last_not_of(" \t");
        item = item.substr(b, e - b + 1);
        if (item.empty()) continue;
        if (!first) out += ",";
        out += "\"" + item + "\"";
        first = false;
    }
    out += "]";
    return out;
}

// Split a ";"-separated list into trimmed, non-empty items.
std::vector<std::string> split_semi(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ';')) {
        auto b = item.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = item.find_last_not_of(" \t");
        out.push_back(item.substr(b, e - b + 1));
    }
    return out;
}

// Quick "is something listening on tcp/<ip>:<port>?" check, using a
// non-blocking connect with a short timeout. We use this to filter the tailnet
// down to the host(s) actually running the Zenoh publisher: handing Zenoh a long
// list of dead endpoints (idle/asleep peers) stalls its connection manager and
// the real publisher's audio never gets through, so we only pass live ones.
bool tcp_reachable(const std::string& ip, int port, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    bool ok = false;
    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
        ok = true;  // connected immediately (e.g. same host)
    } else if (errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (::select(fd + 1, nullptr, &wset, nullptr, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            ok = (err == 0);
        }
    }
    ::close(fd);
    return ok;
}

// ---------------------------------------------------------------------------
// Discover the publisher's Tailscale endpoint(s) to connect to.
//
// If `tailscale_peer` is set we trust it verbatim (IP or MagicDNS name,
// ";"-separated). Otherwise we ask the local tailscaled via `tailscale status`
// for every online peer (and this device itself, in case the publisher runs on
// the same host), then PROBE each on `port` and keep only the ones actually
// listening — i.e. the host(s) running the publisher. Returns "tcp/<ip>:<port>".
// ---------------------------------------------------------------------------
std::vector<std::string> tailscale_connect_endpoints(const std::string& peer_cfg,
                                                     int port) {
    std::vector<std::string> eps;
    for (const auto& p : split_semi(peer_cfg))
        eps.push_back("tcp/" + p + ":" + std::to_string(port));
    if (!eps.empty()) return eps;  // explicit peer(s) win — trust the user

    FILE* pipe = popen("tailscale status 2>/dev/null", "r");
    if (!pipe) {
        std::fprintf(stderr, "[tailscale] could not run `tailscale status`.\n");
        return eps;
    }
    std::vector<std::string> candidates;
    char line[1024];
    while (std::fgets(line, sizeof(line), pipe)) {
        std::string s(line);
        std::string ip = s.substr(0, s.find_first_of(" \t"));
        if (ip.rfind("100.", 0) != 0) continue;                 // IPv4 tailnet rows
        if (s.find("offline") != std::string::npos) continue;   // skip offline peers
        // Includes THIS device's own IP (first row) so a publisher on the SAME
        // host — which listens on our own Tailscale IP — is probed too.
        candidates.push_back(ip);
    }
    pclose(pipe);

    for (const auto& ip : candidates) {
        if (tcp_reachable(ip, port, 400))
            eps.push_back("tcp/" + ip + ":" + std::to_string(port));
    }

    if (eps.empty())
        std::fprintf(stderr,
            "[tailscale] scanned %zu tailnet host(s); none listening on :%d. "
            "Is the publisher up with tailscale=1?\n",
            candidates.size(), port);
    else
        std::fprintf(stderr,
            "[tailscale] found %zu publisher endpoint(s) of %zu host(s) scanned.\n",
            eps.size(), candidates.size());
    return eps;
}

// ---------------------------------------------------------------------------
// PlaybackFifo — a small ring buffer of interleaved float samples shared
// between the (single) network/decoder thread that pushes and the PortAudio
// output callback that pops.
//
// Latency policy:
//   * Playback is gated until `prebuffer` samples have arrived, so the very
//     first callbacks don't immediately underrun.
//   * The FIFO is hard-capped at `capacity`; pushing past it drops the OLDEST
//     samples. Live audio favors freshness over a backlog, and this bounds the
//     end-to-end latency no matter how bursty the network is.
//   * pop() never blocks: if the FIFO can't satisfy a request it fills the
//     remainder with silence (an audible-but-harmless underrun) and keeps the
//     real-time audio thread moving.
// ---------------------------------------------------------------------------
class PlaybackFifo {
public:
    PlaybackFifo(std::size_t capacity, std::size_t prebuffer)
        : buf_(capacity, 0.0f), cap_(capacity), prebuffer_(prebuffer) {}

    // Producer side (network/decoder thread).
    void push(const float* in, std::size_t n) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (n >= cap_) {  // single push larger than the whole FIFO: keep tail
            in += (n - cap_);
            n = cap_;
        }
        if (count_ + n > cap_) {  // make room by dropping the oldest samples
            std::size_t drop = count_ + n - cap_;
            head_ = (head_ + drop) % cap_;
            count_ -= drop;
            dropped_.fetch_add(drop, std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < n; ++i) {
            buf_[tail_] = in[i];
            tail_ = (tail_ + 1) % cap_;
        }
        count_ += n;
        if (!ready_.load(std::memory_order_relaxed) && count_ >= prebuffer_)
            ready_.store(true, std::memory_order_release);
    }

    // Consumer side (PortAudio callback). Always writes exactly `n` floats.
    void pop(float* out, std::size_t n) {
        std::lock_guard<std::mutex> lock(mtx_);
        // Hold output at silence until we've buffered the prebuffer amount.
        if (!ready_.load(std::memory_order_acquire)) {
            std::memset(out, 0, n * sizeof(float));
            return;
        }
        std::size_t avail = count_ < n ? count_ : n;
        for (std::size_t i = 0; i < avail; ++i) {
            out[i] = buf_[head_];
            head_ = (head_ + 1) % cap_;
        }
        count_ -= avail;
        if (avail < n) {  // underrun: pad with silence and re-arm the prebuffer
            std::memset(out + avail, 0, (n - avail) * sizeof(float));
            underruns_.fetch_add(1, std::memory_order_relaxed);
            ready_.store(false, std::memory_order_release);
        }
    }

    std::size_t buffered() {
        std::lock_guard<std::mutex> lock(mtx_);
        return count_;
    }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

private:
    std::mutex mtx_;
    std::vector<float> buf_;
    std::size_t cap_;
    std::size_t prebuffer_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> underruns_{0};
};

PlaybackFifo* g_fifo = nullptr;
int g_channels = 1;

int PaOutputCallback(const void* /*input*/, void* output,
                     unsigned long frame_count,
                     const PaStreamCallbackTimeInfo* /*time_info*/,
                     PaStreamCallbackFlags /*flags*/, void* /*user*/) {
    g_fifo->pop(static_cast<float*>(output),
                static_cast<std::size_t>(frame_count) * g_channels);
    return paContinue;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    // --- Locate and load config (default to the publisher's config.cfg) -----
    AppConfig cfg;
    const char* candidates[] = {"../../../audio_pub/config.cfg",  // from player/build
                                "../../audio_pub/config.cfg",     // from player/
                                "../audio_pub/config.cfg",
                                "config.cfg"};
    std::string loaded;
    if (argc > 1 && cfg.load(argv[1])) loaded = argv[1];
    for (const char* c : candidates) {
        if (!loaded.empty()) break;
        if (cfg.load(c)) loaded = c;
    }
    if (loaded.empty()) {
        std::fprintf(stderr,
                     "Could not open config.cfg (pass a path as argv[1]).\n");
        return 1;
    }
    std::fprintf(stderr, "Loaded config: %s\n", loaded.c_str());

    // --- Audio / Opus parameters (must match the publisher) -----------------
    const int sample_rate   = cfg.integer("sample_rate", 48000);
    const int channels      = cfg.integer("channels", 1);
    const int frame_ms      = cfg.integer("frame_ms", 20);
    const int frame_samples = sample_rate * frame_ms / 1000;  // per channel
    g_channels = channels;

    // --- Jitter buffer tuning (player-only; optional in config) -------------
    const int prebuffer_ms  = cfg.integer("prebuffer_ms", 40);
    const int max_buffer_ms = cfg.integer("max_buffer_ms", 200);
    const std::size_t samples_per_ms =
        static_cast<std::size_t>(sample_rate) * channels / 1000;
    std::size_t prebuffer = samples_per_ms * static_cast<std::size_t>(prebuffer_ms);
    std::size_t capacity  = samples_per_ms * static_cast<std::size_t>(max_buffer_ms);
    if (capacity < prebuffer + frame_samples * channels)
        capacity = prebuffer + static_cast<std::size_t>(frame_samples) * channels;

    PlaybackFifo fifo(capacity, prebuffer);
    g_fifo = &fifo;

    // --- Opus decoder -------------------------------------------------------
    int opus_err = 0;
    OpusDecoder* dec = opus_decoder_create(sample_rate, channels, &opus_err);
    if (opus_err != OPUS_OK || !dec) {
        std::fprintf(stderr, "opus_decoder_create failed: %s\n",
                     opus_strerror(opus_err));
        return 1;
    }
    const int max_frame = 5760;  // 120 ms @ 48 kHz, the largest Opus frame
    std::vector<float> pcm(static_cast<std::size_t>(max_frame) * channels);

    // --- PortAudio output stream --------------------------------------------
    PaError perr = Pa_Initialize();
    if (perr != paNoError) {
        std::fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(perr));
        opus_decoder_destroy(dec);
        return 1;
    }
    PaStreamParameters out{};
    out.device = Pa_GetDefaultOutputDevice();
    if (out.device == paNoDevice) {
        std::fprintf(stderr, "No default output device.\n");
        Pa_Terminate();
        opus_decoder_destroy(dec);
        return 1;
    }
    out.channelCount = channels;
    out.sampleFormat = paFloat32;
    out.suggestedLatency = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    perr = Pa_OpenStream(&stream, /*input=*/nullptr, &out, sample_rate,
                         frame_samples, paClipOff, &PaOutputCallback, nullptr);
    if (perr != paNoError) {
        std::fprintf(stderr, "Pa_OpenStream failed: %s\n", Pa_GetErrorText(perr));
        Pa_Terminate();
        opus_decoder_destroy(dec);
        return 1;
    }
    perr = Pa_StartStream(stream);
    if (perr != paNoError) {
        std::fprintf(stderr, "Pa_StartStream failed: %s\n", Pa_GetErrorText(perr));
        Pa_CloseStream(stream);
        Pa_Terminate();
        opus_decoder_destroy(dec);
        return 1;
    }
    const PaStreamInfo* sinfo = Pa_GetStreamInfo(stream);
    std::fprintf(stderr,
                 "Playback started: %d Hz, %d ch, output latency %.1f ms, "
                 "prebuffer %d ms, cap %d ms\n",
                 sample_rate, channels,
                 sinfo ? sinfo->outputLatency * 1000.0 : -1.0,
                 prebuffer_ms, max_buffer_ms);

    // --- Zenoh session + subscriber: decode straight into the FIFO ----------
    init_log_from_env_or("error");

    zenoh::Config zcfg = zenoh::Config::create_default();
    const std::string mode    = cfg.str("mode", "peer");
    std::string connect = cfg.str("connect", "");
    const std::string listen  = cfg.str("listen", "");

    // --- Tailscale streaming ----------------------------------------------
    // tailscale=1: auto-discover the publisher's Tailscale endpoint(s) and
    // connect to them over the tailnet. Tailscale has no multicast, so we turn
    // off scouting and rely on the explicit TCP connect endpoint(s) below.
    const bool use_tailscale = cfg.boolean("tailscale", false);
    if (use_tailscale) {
        const int ts_port = cfg.integer("tailscale_port", 7447);
        auto eps = tailscale_connect_endpoints(cfg.str("tailscale_peer", ""),
                                               ts_port);
        for (const auto& e : eps)
            connect += (connect.empty() ? "" : ";") + e;
        zcfg.insert_json5("scouting/multicast/enabled", "false");
        if (!connect.empty())
            std::fprintf(stderr,
                "Tailscale streaming ON — connecting to: %s\n", connect.c_str());
    }

    zcfg.insert_json5("mode", "\"" + mode + "\"");
    if (!connect.empty())
        zcfg.insert_json5("connect/endpoints", to_json5_endpoints(connect));
    if (!listen.empty())
        zcfg.insert_json5("listen/endpoints", to_json5_endpoints(listen));

    const std::string key = cfg.str("key", "audio/stream/mic1");

    // Per-packet state for loss detection + Opus PLC. Only the subscriber
    // callback thread touches these, so no locking is needed here.
    std::uint32_t expected = 0;
    bool have_expected = false;
    std::atomic<std::uint64_t> received{0}, concealed{0};
    const int MAX_CONCEAL = 25;  // cap PLC bursts (~0.5 s @ 20 ms) before resyncing

    auto on_sample = [&](const Sample& sample) {
        auto data = sample.get_payload().as_vector();
        if (data.size() < 4) return;
        std::uint32_t seq = static_cast<std::uint32_t>(data[0]) |
                            (static_cast<std::uint32_t>(data[1]) << 8) |
                            (static_cast<std::uint32_t>(data[2]) << 16) |
                            (static_cast<std::uint32_t>(data[3]) << 24);

        // Conceal a short run of missing packets so playback stays in time.
        if (have_expected) {
            std::uint32_t gap = (seq - expected) & 0xFFFFFFFFu;
            if (gap > 0 && gap <= static_cast<std::uint32_t>(MAX_CONCEAL)) {
                for (std::uint32_t i = 0; i < gap; ++i) {
                    int n = opus_decode_float(dec, nullptr, 0, pcm.data(),
                                              frame_samples, 0);
                    if (n > 0) {
                        fifo.push(pcm.data(),
                                  static_cast<std::size_t>(n) * channels);
                        concealed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        const unsigned char* payload = data.data() + 4;
        int payload_len = static_cast<int>(data.size() - 4);
        if (payload_len > 0) {  // 0-length == DTX/silence frame: skip decode
            int n = opus_decode_float(dec, payload, payload_len, pcm.data(),
                                      max_frame, 0);
            if (n > 0)
                fifo.push(pcm.data(), static_cast<std::size_t>(n) * channels);
        }
        expected = (seq + 1) & 0xFFFFFFFFu;
        have_expected = true;
        received.fetch_add(1, std::memory_order_relaxed);
    };

    std::unique_ptr<Session> session;
    std::unique_ptr<Subscriber<void>> sub;
    try {
        session = std::make_unique<Session>(Session::open(std::move(zcfg)));
        sub = std::make_unique<Subscriber<void>>(
            session->declare_subscriber(KeyExpr(key), on_sample,
                                        closures::none));
    } catch (const ZException& e) {
        std::fprintf(stderr, "Zenoh error: %s\n", e.what());
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
        opus_decoder_destroy(dec);
        return 1;
    }
    std::fprintf(stderr,
                 "Subscribed to '%s' (mode=%s). Listening... Ctrl-C to stop.\n",
                 key.c_str(), mode.c_str());

    while (!g_stop.load()) {
        Pa_Sleep(1000);  // ~once per second status line
        std::fprintf(stderr,
                     "\rreceived=%llu concealed=%llu buffered=%zums "
                     "dropped=%llu underruns=%llu     ",
                     static_cast<unsigned long long>(received.load()),
                     static_cast<unsigned long long>(concealed.load()),
                     samples_per_ms ? fifo.buffered() / samples_per_ms : 0,
                     static_cast<unsigned long long>(fifo.dropped()),
                     static_cast<unsigned long long>(fifo.underruns()));
        std::fflush(stderr);
    }

    std::fprintf(stderr, "\nStopping...\n");
    sub.reset();
    session.reset();
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    opus_decoder_destroy(dec);
    std::fprintf(stderr,
                 "Done. received=%llu concealed=%llu dropped=%llu underruns=%llu\n",
                 static_cast<unsigned long long>(received.load()),
                 static_cast<unsigned long long>(concealed.load()),
                 static_cast<unsigned long long>(fifo.dropped()),
                 static_cast<unsigned long long>(fifo.underruns()));
    return 0;
}
