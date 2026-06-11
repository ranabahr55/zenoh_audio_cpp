#!/usr/bin/env python3
"""audio_sub.py — Real-time Opus-over-Zenoh audio subscriber (terminal 2).

Mirror image of the C++ publisher:

    Zenoh sub --> parse [seq | opus packet] --> Opus decode --> FLAC file

It subscribes to the same Zenoh key the publisher uses, decodes every Opus
packet back to PCM, and streams the result into a FLAC file as it arrives.

Design choices that make it "great":
  * Opus is decoded by calling the system libopus directly through ctypes.
    No extra pip packages are needed and it is the canonical reference decoder.
  * The 4-byte sequence number on each packet lets us detect dropped packets
    and run Opus packet-loss concealment (PLC) so the saved audio stays in
    time and free of clicks instead of silently shrinking.
  * Zenoh's callback only enqueues; a dedicated writer thread does the decode
    and disk I/O, so the network thread never blocks.

Usage:
    python3 audio_sub.py [path/to/config.cfg]
    (defaults to ../audio_pub/config.cfg — the publisher's config)
"""

import ctypes
import ctypes.util
import json
import os
import queue
import signal
import struct
import subprocess
import sys
import threading

import numpy as np
import soundfile as sf
import zenoh


# --------------------------------------------------------------------------- #
#  Config: same tiny `key = value` format the C++ side reads.
# --------------------------------------------------------------------------- #
def load_config(path):
    cfg = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0]
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k, v = k.strip(), v.strip()
            if k:
                cfg[k] = v
    return cfg


def cfg_int(cfg, key, default):
    return int(cfg.get(key, default))


# --------------------------------------------------------------------------- #
#  Minimal libopus decoder binding (ctypes).
# --------------------------------------------------------------------------- #
class OpusDecoder:
    OPUS_OK = 0

    def __init__(self, sample_rate, channels):
        self._lib = self._load_libopus()
        self.channels = channels

        self._lib.opus_decoder_create.restype = ctypes.c_void_p
        self._lib.opus_decoder_create.argtypes = [
            ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)
        ]
        self._lib.opus_decode_float.restype = ctypes.c_int
        self._lib.opus_decode_float.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32,
            ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_int,
        ]
        self._lib.opus_decoder_destroy.restype = None
        self._lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self._lib.opus_strerror.restype = ctypes.c_char_p
        self._lib.opus_strerror.argtypes = [ctypes.c_int]

        err = ctypes.c_int(0)
        self._dec = self._lib.opus_decoder_create(
            sample_rate, channels, ctypes.byref(err))
        if err.value != self.OPUS_OK or not self._dec:
            raise RuntimeError(
                f"opus_decoder_create failed: {self._strerror(err.value)}")

        # Scratch PCM buffer big enough for the largest Opus frame (120 ms).
        self._max_frame = 5760
        self._pcm = (ctypes.c_float * (self._max_frame * channels))()

    @staticmethod
    def _load_libopus():
        candidates = []
        found = ctypes.util.find_library("opus")
        if found:
            candidates.append(found)
        candidates += [
            "libopus.so.0", "libopus.so",
            os.path.join(os.environ.get("CONDA_PREFIX", ""), "lib", "libopus.so"),
            "/usr/lib/x86_64-linux-gnu/libopus.so.0",
            "/usr/local/lib/libopus.so",
            "/home/rana/anaconda3/lib/libopus.so",
        ]
        last = None
        for name in candidates:
            if not name:
                continue
            try:
                return ctypes.CDLL(name)
            except OSError as e:
                last = e
        raise RuntimeError(f"Could not load libopus ({last})")

    def _strerror(self, code):
        return self._lib.opus_strerror(code).decode("ascii", "replace")

    def decode(self, packet):
        """Decode one Opus packet -> float32 ndarray (frames, channels)."""
        n = self._lib.opus_decode_float(
            self._dec, packet, len(packet), self._pcm, self._max_frame, 0)
        return self._to_array(n)

    def conceal(self, frame_size):
        """Generate `frame_size` samples of packet-loss concealment (no data)."""
        n = self._lib.opus_decode_float(
            self._dec, None, 0, self._pcm, frame_size, 0)
        return self._to_array(n)

    def _to_array(self, n):
        if n < 0:
            raise RuntimeError(f"opus decode error: {self._strerror(n)}")
        # as_array() exposes the whole scratch buffer; take only the `n`
        # samples-per-channel that this call actually produced.
        flat = np.ctypeslib.as_array(self._pcm)[: n * self.channels].copy()
        return flat.reshape(-1, self.channels) if self.channels > 1 else flat

    def close(self):
        if getattr(self, "_dec", None):
            self._lib.opus_decoder_destroy(self._dec)
            self._dec = None


# --------------------------------------------------------------------------- #
#  Main
# --------------------------------------------------------------------------- #
def _tcp_reachable(ip, port, timeout=0.4):
    """True if something accepts a TCP connection on ip:port within `timeout`."""
    import socket
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def tailscale_connect_endpoints(cfg):
    """Return the Zenoh `connect` endpoints for the Tailscale publisher.

    If `tailscale_peer` is set in the config we trust it (IP or MagicDNS name,
    semicolon-separated). Otherwise we ask the local tailscaled for every node on
    the tailnet (peers + this device, in case the publisher is on the same host),
    then PROBE each on `tailscale_port` and keep only the host(s) actually
    listening — i.e. the publisher. Handing Zenoh a long list of dead endpoints
    (idle/asleep peers) stalls its connection manager and the real publisher's
    audio never gets through, so the probe is what makes auto-discovery reliable.
    """
    port = int(cfg.get("tailscale_port", 7447))
    explicit = [p.strip() for p in cfg.get("tailscale_peer", "").split(";") if p.strip()]
    if explicit:
        return [f"tcp/{p}:{port}" for p in explicit]  # trust the user verbatim

    try:
        out = subprocess.check_output(
            ["tailscale", "status", "--json"], text=True, timeout=5)
        data = json.loads(out)
    except Exception as e:  # CLI missing, not logged in, daemon down, etc.
        print(f"[tailscale] could not auto-discover peers: {e}", file=sys.stderr)
        return []

    nodes = list((data.get("Peer") or {}).values())
    self_node = data.get("Self")
    if self_node:
        nodes.append(self_node)

    candidates = []
    for node in nodes:
        if node is not self_node and not node.get("Online", False):
            continue  # skip offline remote peers (but always keep self)
        for ip in node.get("TailscaleIPs") or []:
            if ":" not in ip:  # IPv4 only
                candidates.append(ip)

    endpoints = [f"tcp/{ip}:{port}" for ip in candidates if _tcp_reachable(ip, port)]
    if endpoints:
        print(f"[tailscale] found {len(endpoints)} publisher endpoint(s) of "
              f"{len(candidates)} host(s) scanned: " + ", ".join(endpoints))
    else:
        print(f"[tailscale] scanned {len(candidates)} tailnet host(s); none "
              f"listening on :{port}. Is the publisher up with tailscale=1?",
              file=sys.stderr)
    return endpoints


def build_zenoh_config(cfg):
    zconf = zenoh.Config()
    zconf.insert_json5("mode", f'"{cfg.get("mode", "peer")}"')
    connect = [e.strip() for e in cfg.get("connect", "").split(";") if e.strip()]
    listen = [e.strip() for e in cfg.get("listen", "").split(";") if e.strip()]

    # Stream over Tailscale instead of local Wi-Fi: connect to the publisher's
    # Tailscale endpoint(s) and disable multicast scouting (Tailscale has none).
    use_tailscale = str(cfg.get("tailscale", "0")).strip().lower() in (
        "1", "true", "yes", "on")
    if use_tailscale:
        connect += tailscale_connect_endpoints(cfg)
        zconf.insert_json5("scouting/multicast/enabled", "false")

    if connect:
        zconf.insert_json5("connect/endpoints",
                           "[" + ",".join(f'"{e}"' for e in connect) + "]")
    if listen:
        zconf.insert_json5("listen/endpoints",
                           "[" + ",".join(f'"{e}"' for e in listen) + "]")
    return zconf


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_cfg = os.path.join(here, "..", "audio_pub", "config.cfg")
    cfg_path = sys.argv[1] if len(sys.argv) > 1 else default_cfg
    if not os.path.exists(cfg_path):
        sys.exit(f"Config not found: {cfg_path}")
    cfg = load_config(cfg_path)
    print(f"Loaded config: {os.path.abspath(cfg_path)}")

    sample_rate = cfg_int(cfg, "sample_rate", 48000)
    channels = cfg_int(cfg, "channels", 1)
    frame_ms = cfg_int(cfg, "frame_ms", 20)
    frame_samples = sample_rate * frame_ms // 1000
    key = cfg.get("key", "audio/stream/mic1")
    output = cfg.get("output", "capture.flac")
    # The subscriber writes relative to its own directory unless given an
    # absolute path, so output lands predictably next to this script.
    if not os.path.isabs(output):
        output = os.path.join(here, output)

    decoder = OpusDecoder(sample_rate, channels)
    snd = sf.SoundFile(output, mode="w", samplerate=sample_rate,
                       channels=channels, format="FLAC", subtype="PCM_16")

    pkt_queue = queue.Queue(maxsize=2048)
    stop = threading.Event()
    stats = {"received": 0, "lost": 0, "frames": 0}

    # -- writer thread: decode + FLAC write, with packet-loss concealment -----
    def writer():
        expected = None
        MAX_CONCEAL = 25  # cap PLC bursts (~0.5 s at 20 ms) before resyncing
        while not (stop.is_set() and pkt_queue.empty()):
            try:
                seq, payload = pkt_queue.get(timeout=0.2)
            except queue.Empty:
                continue

            if expected is not None:
                gap = (seq - expected) & 0xFFFFFFFF
                if 0 < gap <= MAX_CONCEAL:
                    for _ in range(gap):
                        snd.write(decoder.conceal(frame_samples))
                        stats["lost"] += 1
                        stats["frames"] += 1
            try:
                if payload:  # empty payload == DTX/silence frame, skip decode
                    pcm = decoder.decode(payload)
                    snd.write(pcm)
                    stats["frames"] += 1
            except RuntimeError as e:
                print(f"\n[decode] {e}", file=sys.stderr)
            expected = (seq + 1) & 0xFFFFFFFF
            stats["received"] += 1

    wt = threading.Thread(target=writer, daemon=True)
    wt.start()

    # -- zenoh subscriber: enqueue only -------------------------------------
    def on_sample(sample):
        data = sample.payload.to_bytes()
        if len(data) < 4:
            return
        seq = struct.unpack_from("<I", data, 0)[0]
        try:
            pkt_queue.put_nowait((seq, data[4:]))
        except queue.Full:
            stats["lost"] += 1  # writer fell behind; drop to stay real-time

    print("Opening Zenoh session...")
    session = zenoh.open(build_zenoh_config(cfg))
    sub = session.declare_subscriber(key, on_sample)
    print(f"Subscribed to '{key}'. Writing -> {output}")
    print("Receiving... press Ctrl-C to stop and finalize the file.")

    sig = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: sig.set())
    signal.signal(signal.SIGTERM, lambda *_: sig.set())
    try:
        while not sig.is_set():
            sig.wait(1.0)
            print(f"\rreceived={stats['received']} frames={stats['frames']} "
                  f"concealed/lost={stats['lost']}   ", end="", flush=True)
    finally:
        print("\nStopping...")
        sub.undeclare()
        session.close()
        stop.set()
        wt.join(timeout=5.0)
        decoder.close()
        snd.close()
        secs = stats["frames"] * frame_ms / 1000.0
        print(f"Saved {output}: {stats['frames']} frames (~{secs:.1f} s), "
              f"received={stats['received']}, concealed/lost={stats['lost']}")


if __name__ == "__main__":
    main()
