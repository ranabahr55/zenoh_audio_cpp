// main.cpp — Real-time Opus-over-Zenoh audio publisher.
//
// Pipeline (terminal 1 of the pub/sub pair):
//
//   microphone --PortAudio--> ring buffer --Opus encode--> Zenoh put
//
// The capture thread (PortAudio callback) only copies samples; this main
// thread pulls one frame at a time, encodes it with Opus, and publishes the
// compressed packet on a Zenoh key expression. Each packet is prefixed with a
// 4-byte little-endian sequence number so the subscriber can detect loss and
// run packet-loss concealment, keeping the recovered audio in time.
//
// Everything tunable (Zenoh endpoint, key, bitrate, frame size, quality...)
// lives in config.cfg next to this program.

#include <opus/opus.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "zenoh.hxx"

#include "recorder.hpp"

using namespace zenoh;

namespace {

std::atomic<bool> g_stop{false};
void HandleSignal(int) { g_stop.store(true); }

// ---------------------------------------------------------------------------
// Tiny `key = value` config reader (# starts a comment, anywhere on the line).
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
        // trim
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

// ---------------------------------------------------------------------------
// Return THIS device's Tailscale IPv4 address, or "" if it isn't on a tailnet.
//
// Tailscale assigns each node a stable address from the 100.64.0.0/10 CGNAT
// range and exposes it on an interface named "tailscale0" (Linux). We scan the
// live interfaces directly instead of shelling out to the `tailscale` CLI, so
// detection works without extra privileges or PATH assumptions. Preference:
// an address on a "tailscale*" interface first, otherwise any up interface
// holding a 100.64.0.0/10 address.
// ---------------------------------------------------------------------------
std::string detect_tailscale_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return "";

    std::string by_name, by_range;
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;

        // 100.64.0.0/10  ==  100.64.0.0 .. 100.127.255.255
        const std::uint32_t a = ntohl(sin->sin_addr.s_addr);
        const bool in_cgnat = (a & 0xFFC00000u) == 0x64400000u;

        if (ifa->ifa_name && std::strncmp(ifa->ifa_name, "tailscale", 9) == 0)
            by_name = buf;                       // most reliable: the TS device
        else if (in_cgnat && by_range.empty())
            by_range = buf;                      // fallback: any 100.64/10 addr
    }
    freeifaddrs(ifaddr);
    return !by_name.empty() ? by_name : by_range;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    // --- Locate and load config -------------------------------------------
    AppConfig cfg;
    const char* candidates[] = {nullptr, "config.cfg", "../config.cfg"};
    std::string loaded;
    if (argc > 1) {
        if (cfg.load(argv[1])) loaded = argv[1];
    }
    for (int i = 1; loaded.empty() && i < 3; ++i) {
        if (cfg.load(candidates[i])) loaded = candidates[i];
    }
    if (loaded.empty()) {
        std::fprintf(stderr,
                     "Could not open config.cfg (pass a path as argv[1]).\n");
        return 1;
    }
    std::fprintf(stderr, "Loaded config: %s\n", loaded.c_str());

    // --- Audio / Opus parameters ------------------------------------------
    const int sample_rate = cfg.integer("sample_rate", 48000);
    const int channels    = cfg.integer("channels", 1);
    const int frame_ms    = cfg.integer("frame_ms", 20);
    const int frame_samples = sample_rate * frame_ms / 1000;  // per channel

    const int  bitrate    = cfg.integer("bitrate", 64000);
    const int  complexity = cfg.integer("complexity", 10);
    const bool use_fec    = cfg.boolean("fec", true);
    const bool use_dtx    = cfg.boolean("dtx", false);
    const bool use_vbr    = cfg.boolean("vbr", true);
    const int  loss_perc  = cfg.integer("packet_loss_perc", 10);
    const std::string signal_type = cfg.str("signal", "voice");
    const bool low_delay  = cfg.boolean("low_delay", false);

    // --- Create the Opus encoder ------------------------------------------
    int application = OPUS_APPLICATION_AUDIO;
    if (low_delay)            application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
    else if (signal_type == "voice") application = OPUS_APPLICATION_VOIP;

    int opus_err = 0;
    OpusEncoder* enc = opus_encoder_create(sample_rate, channels, application, &opus_err);
    if (opus_err != OPUS_OK || !enc) {
        std::fprintf(stderr, "opus_encoder_create failed: %s\n",
                     opus_strerror(opus_err));
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(enc, OPUS_SET_VBR(use_vbr ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(use_fec ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_DTX(use_dtx ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(loss_perc));
    if (!low_delay) {
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(signal_type == "music"
                                                  ? OPUS_SIGNAL_MUSIC
                                                  : OPUS_SIGNAL_VOICE));
    }

    std::fprintf(stderr,
                 "Opus encoder: %d Hz, %d ch, %d ms frames, %d bps, "
                 "complexity %d, FEC %s, DTX %s, VBR %s, app %s\n",
                 sample_rate, channels, frame_ms, bitrate, complexity,
                 use_fec ? "on" : "off", use_dtx ? "on" : "off",
                 use_vbr ? "on" : "off",
                 low_delay ? "lowdelay" : (signal_type == "music" ? "audio" : "voip"));

    // --- Open the Zenoh session and publisher -----------------------------
    init_log_from_env_or("error");

    zenoh::Config zcfg = zenoh::Config::create_default();
    const std::string mode    = cfg.str("mode", "peer");
    std::string connect = cfg.str("connect", "");
    std::string listen  = cfg.str("listen", "");

    // --- Tailscale streaming ----------------------------------------------
    // tailscale=1: auto-detect this device's Tailscale IP and listen on it so
    // the subscriber can reach us over the tailnet. Tailscale has no multicast,
    // so we turn off scouting and rely on the explicit TCP endpoint below.
    const bool use_tailscale = cfg.boolean("tailscale", false);
    if (use_tailscale) {
        const int ts_port = cfg.integer("tailscale_port", 7447);
        const std::string ts_ip = detect_tailscale_ip();
        if (ts_ip.empty()) {
            std::fprintf(stderr,
                "tailscale=1 but no Tailscale IP found on this device "
                "(is `tailscale up` running?). Falling back to local "
                "Wi-Fi/LAN settings.\n");
        } else {
            const std::string ep =
                "tcp/" + ts_ip + ":" + std::to_string(ts_port);
            listen = ep;            // bind the publisher to the Tailscale IP
            zcfg.insert_json5("scouting/multicast/enabled", "false");
            std::fprintf(stderr,
                "Tailscale streaming ON — publishing on %s\n"
                "   (subscriber will auto-discover this, or set "
                "tailscale_peer=%s)\n",
                ep.c_str(), ts_ip.c_str());
        }
    }

    zcfg.insert_json5("mode", "\"" + mode + "\"");
    if (!connect.empty())
        zcfg.insert_json5("connect/endpoints", to_json5_endpoints(connect));
    if (!listen.empty())
        zcfg.insert_json5("listen/endpoints", to_json5_endpoints(listen));

    const std::string key = cfg.str("key", "audio/stream/mic1");

    std::unique_ptr<Session> session;
    std::unique_ptr<Publisher> pub;
    try {
        session = std::make_unique<Session>(Session::open(std::move(zcfg)));

        Session::PublisherOptions popts = Session::PublisherOptions::create_default();
        // Real-time audio: never block the encoder waiting on the network, and
        // send each packet immediately rather than batching it.
        popts.congestion_control = Z_CONGESTION_CONTROL_DROP;
        popts.priority = Z_PRIORITY_REAL_TIME;
        popts.is_express = true;
        pub = std::make_unique<Publisher>(
            session->declare_publisher(KeyExpr(key), std::move(popts)));
    } catch (const ZException& e) {
        std::fprintf(stderr, "Zenoh error: %s\n", e.what());
        opus_encoder_destroy(enc);
        return 1;
    }
    std::fprintf(stderr, "Zenoh publisher ready on key '%s' (mode=%s%s%s).\n",
                 key.c_str(), mode.c_str(),
                 connect.empty() ? "" : (", connect=" + connect).c_str(),
                 "");

    // --- Capture -> encode -> publish loop --------------------------------
    Recorder rec(sample_rate, channels, frame_samples);
    if (!rec.start()) {
        opus_encoder_destroy(enc);
        return 1;
    }

    std::vector<float> frame;
    std::vector<unsigned char> packet(4 + 4000);  // 4-byte header + max Opus payload
    std::uint32_t seq = 0;
    std::uint64_t total_in_bytes = 0, total_out_bytes = 0;

    std::fprintf(stderr, "Streaming... press Ctrl-C to stop.\n");
    while (!g_stop.load()) {
        if (!rec.read_frame(frame)) break;

        // Reserve the 4-byte sequence header; encode into the rest.
        int n = opus_encode_float(enc, frame.data(), frame_samples,
                                  packet.data() + 4,
                                  static_cast<opus_int32>(packet.size() - 4));
        if (n < 0) {
            std::fprintf(stderr, "opus_encode_float failed: %s\n", opus_strerror(n));
            continue;
        }
        // DTX can legitimately produce 0/1-byte "comfort noise" packets; the
        // subscriber treats a 0-length payload as silence/skip. Still send a
        // packet so the sequence numbering stays continuous.
        packet[0] = static_cast<unsigned char>(seq & 0xFF);
        packet[1] = static_cast<unsigned char>((seq >> 8) & 0xFF);
        packet[2] = static_cast<unsigned char>((seq >> 16) & 0xFF);
        packet[3] = static_cast<unsigned char>((seq >> 24) & 0xFF);
        ++seq;

        std::vector<std::uint8_t> bytes(packet.begin(), packet.begin() + 4 + n);
        try {
            pub->put(Bytes(std::move(bytes)));
        } catch (const ZException& e) {
            std::fprintf(stderr, "Zenoh put error: %s\n", e.what());
        }

        total_in_bytes  += static_cast<std::uint64_t>(frame_samples) * channels * sizeof(float);
        total_out_bytes += static_cast<std::uint64_t>(n);
        const unsigned frames_per_sec = frame_ms ? (1000u / frame_ms) : 1u;
        if (frames_per_sec && (seq % frames_per_sec) == 0) {  // ~once per second
            const double seconds = seq * frame_ms / 1000.0;
            std::fprintf(stderr, "\rframes=%u  ~%.1f kbps  overruns=%lu      ",
                         seq,
                         seconds > 0 ? total_out_bytes * 8.0 / seconds / 1000.0 : 0.0,
                         rec.overruns());
            std::fflush(stderr);
        }
    }

    std::fprintf(stderr, "\nStopping...\n");
    rec.stop();
    opus_encoder_destroy(enc);
    std::fprintf(stderr,
                 "Done. Sent %u frames, %.1f KiB compressed from %.1f KiB PCM "
                 "(%.1fx), overruns=%lu\n",
                 seq, total_out_bytes / 1024.0, total_in_bytes / 1024.0,
                 total_out_bytes ? (double)total_in_bytes / total_out_bytes : 0.0,
                 rec.overruns());
    return 0;
}
