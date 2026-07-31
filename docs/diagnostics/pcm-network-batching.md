# PCM network-batching diagnostic

**Branch:** `diagnostic/pcm-network-batching`

**Status:** built and tested off-device; not yet flashed or physically validated

## Question

Can the relay keep the proven 256-sample codec/audio-task cadence while reducing WebSocket send rate enough to complete a six-second turn before the 32-item capture queue overflows?

This diagnostic deliberately changes only uplink framing. It does **not** add remote playback, alter display submission behavior, alter I²S cadence, or change local capture/replay authority.

## Module seam

`pcm_batcher` is an allocation-free module between the capture queue and WebSocket sender. Its small interface hides spill handling across codec chunks, full-frame emission, final partial-frame flushing, cancellation, and fail-closed emission cleanup. Callers provide the storage and synchronous emit function.

Production-shaped configuration:

- Codec/capture chunk: 256 samples (10.67 ms at 24 kHz).
- Maximum network frame: 960 samples (40 ms).
- Capture queue: 32 items with two control slots reserved.
- WebSocket receive buffer: 2,048 bytes.
- Largest framed PCM message: 1,932 bytes.

The network-frame storage is allocated in PSRAM. The batcher writes directly into the PCM area of the eventual `WA` frame, avoiding another 1,920-byte copy and avoiding a larger manager-task stack frame.

## Off-device evidence

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Ifirmware/main \
  firmware/main/pcm_batcher.c firmware/tests/pcm_batcher_test.c \
  -o /tmp/pcm_batcher_test
/tmp/pcm_batcher_test
python3 -m unittest discover -s tools -p 'test_relay_cadence_sim.py' -v
python3 tools/relay_cadence_sim.py --frame-samples 960 \
  --send-time-ms 30 35 40 45
```

The module tests preserve every sample across four 256-sample chunks as one 960-sample frame plus a 64-sample commit flush. They also cover arbitrary chunk boundaries, cancellation, empty completion, and emission failure under ASan/UBSan.

The deterministic cadence model includes the ~180 ms pre-commit burst, 256-sample producer cadence, queue control reserve, synchronous sender time, partial commit flush, and 40 ms echo RTT assumption. For a six-second turn it predicts:

| Sender time per network frame | Queue high-water | Dropped capture chunks | Echo completion after release | Candidate |
| ---: | ---: | ---: | ---: | :--- |
| 30 ms | 18 | 0 | 72.7 ms | valid |
| 35 ms | 18 | 0 | 82.7 ms | valid |
| 40 ms | 18 | 0 | 234.3 ms | valid, low margin |
| 45 ms | 30 | 48 | 444.3 ms | rejected |

This is a conservative model, not proof of hardware behavior. It establishes a falsifiable gate: sustained send completion must remain at or below 40 ms, with no cloud-side sequence gap or missing samples.

## Candidate build

The first off-device build has app SHA-256:

`043a93c6787d5d1d6f68f963a5de9821b29ed7c2f80176ec0d42da57a9f60e2e`

It must not be promoted unless all of the following pass:

1. True cold boot with the face continuously visible for at least 60 seconds before interaction.
2. One short committed turn and one near-six-second turn, with normal local replay.
3. Authenticated cloud aggregate reports exact sample counts, contiguous sequences, and expected frame count `ceil(samples / 960)`.
4. Face continuously visible for at least two minutes after the turns, without native USB serial attached.
5. Forced reconnect followed by another successful turn.
6. Final 15-second power-off and cold-boot visibility check.

If visibility fails at any point, restore commit `7925d4e` / app `5c19c3cd…` immediately. Successful transfer callbacks or cloud metrics do not override a physical display failure.
