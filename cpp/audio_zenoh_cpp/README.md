# audio_zenoh_cpp — Real-time Opus audio over Zenoh

A low-latency microphone streaming pair:

```
  terminal 1                                   terminal 2
  ┌─────────────────────────────┐  Zenoh   ┌──────────────────────────────┐
  │ audio_pub  (C++)            │  key:    │ audio_sub  (Python)          │
  │ mic → PortAudio → Opus enc  │ ───────▶ │ Opus dec → FLAC file         │
  │ → Zenoh put (express, RT)   │ audio/.. │ (+ packet-loss concealment)  │
  └─────────────────────────────┘          └──────────────────────────────┘
```

* **Publisher** (`audio_pub/`) is C++: PortAudio capture on a real-time
  callback, Opus encoding on a worker thread, Zenoh publishing with
  `DROP` congestion control + `REAL_TIME` priority + express (no batching) so
  latency stays minimal.
* **Subscriber** (`audio_sub/`) is Python: decodes Opus by calling the system
  `libopus` through `ctypes` (no extra pip installs), conceals lost packets
  using the per-packet sequence number, and streams the result to **FLAC**
  via `soundfile`.

Both ends read the **same** `audio_pub/config.cfg`, so the Zenoh key, sample
rate, channels and frame size can never drift out of sync.

---

## Build the publisher (you run these — they are not run for you)

```bash
cd audio_pub
mkdir build && cd build
cmake ..
make
```

CMake auto-discovers Zenoh (`/usr/local`), PortAudio (pkg-config) and Opus
(searched under `$CONDA_PREFIX` / Anaconda / system paths). If Opus is
elsewhere: `cmake .. -DOPUS_ROOT=/your/prefix`.

## Run — two terminals, simultaneously

**Terminal 1 — publisher** (captures the mic, streams in real time):

```bash
cd audio_pub/build
./audio_pub               # reads ../config.cfg automatically
# or: ./audio_pub /path/to/config.cfg
```

**Terminal 2 — subscriber** (receives, decodes, saves):

```bash
cd audio_sub
python3 audio_sub.py      # reads ../audio_pub/config.cfg automatically
# or: python3 audio_sub.py /path/to/config.cfg
```

Press **Ctrl-C** in terminal 2 first to finalize the FLAC file
(`audio_sub/capture.flac` by default), then Ctrl-C the publisher.

> Order tip: start the subscriber first so it is ready when the first packets
> arrive — though either order works, peers discover each other automatically.

---

## Configuration (`audio_pub/config.cfg`)

Everything tunable lives there with inline documentation: Zenoh `mode` /
`connect` / `listen` / `key`, capture `sample_rate` / `channels` / `frame_ms`,
and the Opus `bitrate` / `complexity` / `vbr` / `fec` / `dtx` / `signal` /
`low_delay`, plus the subscriber's `output` path.

### Same machine
Defaults work out of the box: `mode = peer`, empty `connect` → peers find each
other by multicast scouting on localhost.

### Two machines
On the publisher host set, e.g. `listen = tcp/0.0.0.0:7447`; on the subscriber
host set `connect = tcp/<publisher-ip>:7447`. (Or point both `connect` at a
running `zenohd` router and use `mode = client`.)

---

## Subscriber dependencies

Python 3 with `zenoh`, `numpy`, `soundfile` (all already present in this
environment). Opus decoding uses the system `libopus.so` via `ctypes` — nothing
to install. FLAC encoding uses `libsndfile` through `soundfile`.

## Wire format

Each Zenoh payload is: `[ uint32 little-endian sequence number ][ Opus packet ]`.
The sequence number lets the subscriber detect gaps and run Opus PLC so the
saved audio stays the right length and click-free. A zero-length Opus payload
(possible when DTX is enabled) is treated as a silence/skip frame.
