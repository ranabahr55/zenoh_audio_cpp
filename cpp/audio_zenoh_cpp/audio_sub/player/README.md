# audio_player — live low-latency playback (C++)

A drop-in **listener** for the `audio_pub` stream. Where `../audio_sub.py`
decodes the Opus-over-Zenoh stream to a **FLAC file**, this plays it straight
to your **speakers** the instant packets arrive:

```
  Zenoh sub → parse [seq | opus] → Opus decode → PortAudio out → speakers
```

## What makes it low latency

* **Decode in the receive callback.** Opus decode is sub-millisecond, so it
  happens directly in the Zenoh subscriber callback — no extra thread hop
  between "packet arrived" and "samples queued for playback".
* **Tiny jitter buffer.** A small ring FIFO sits between the network thread and
  the PortAudio output callback. Playback starts after just `prebuffer_ms`
  (default **40 ms**) and the buffer is hard-capped at `max_buffer_ms`
  (default **200 ms**) — if we ever fall behind, the **oldest** samples are
  dropped so you always hear *live* audio, never a growing backlog.
* **Real-time-safe audio thread.** The output callback never blocks; an
  underrun just emits silence and re-arms the prebuffer.
* **Packet-loss concealment.** The 4-byte sequence number on each packet is
  used to run Opus PLC across short gaps, so brief losses stay in time and
  click-free.

## Build

```bash
cd audio_sub/player
mkdir build && cd build
cmake ..
make
```

Same dependencies as the publisher: Zenoh C++, PortAudio, Opus (auto-discovered;
override Opus with `cmake .. -DOPUS_ROOT=/your/prefix`).

## Run

Start the publisher (`audio_pub`) as usual, then in another terminal:

```bash
cd audio_sub/player/build
./audio_player                 # reads ../../../audio_pub/config.cfg automatically
# or: ./audio_player /path/to/config.cfg
```

You should hear the mic within ~40 ms of it being captured. Press **Ctrl-C**
to stop. The status line shows live health:

```
received=… concealed=… buffered=…ms dropped=… underruns=…
```

* `buffered` — current jitter-buffer depth (your added latency).
* `concealed` — frames synthesized by Opus PLC for lost packets.
* `dropped` — oldest samples discarded to stay under `max_buffer_ms`.
* `underruns` — times the buffer ran dry (heard as a brief gap).

## Tuning (optional keys in `config.cfg`)

```ini
prebuffer_ms  = 40    # audio buffered before playback starts; lower = less latency,
                      # but more prone to underruns on a jittery network.
max_buffer_ms = 200   # hard cap on buffered audio (oldest dropped beyond this).
```

These are read only by the player; the publisher and `audio_sub.py` ignore
unknown keys, so adding them is safe. For the absolute lowest latency, try
`prebuffer_ms = 20` and set `low_delay = true` on the publisher.
