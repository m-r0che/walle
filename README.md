# Walle

A small voice-interactive robot face for the Waveshare ESP32-S3-Touch-AMOLED-1.8.

The device renders a locally animated neon face and uses OpenAI's Live API (`gpt-live-1`) through Cloudflare for low-latency speech. Planned tools include a shopping list and a confirmed home-printer workflow.

## Status

Core hardware and voice bring-up are complete. The verified device runs a locally animated landscape face, authenticated dual-network transport, and buffered speech-to-speech through OpenAI `gpt-live-1` via a Cloudflare Agent. Hold anywhere on the face to speak; release to commit the turn. Actual codec playback drives the mouth, a new press interrupts playback, and untouched local audio remains the fallback until remote playback takes ownership. Compact transparent corner controls adjust and persist codec volume.

The face composes truthful activity, slow mood, bounded reactions, blink, gaze, breathing, and audio-reactive mouth motion locally. After prolonged idle stillness it falls asleep with drifting `Z`s; meaningful QMI8658 motion or touch wakes it without disconnecting Wi-Fi or the provider session. The display retains the physically validated CO5300 path: portrait panel addressing, LVGL partial-region software rotation, one completion-owned 110-row internal DMA buffer, and at least 35 ms between submissions. Hardware axis-swapping and bursty transfer candidates were rejected after visible corruption or loss of responsiveness.

The cloud relay validates epochs, turn ownership, sequence, hashes, sample counts, cancellation, and heartbeat state. Generated PCM uses bounded rolling storage and begins after 500 ms of contiguous validated audio. Walle’s active constitution is versioned as `walle-warm-curious-v3`, with the separate `walle-british-quirky-ballad-v1` voice-performance profile.

- [Final implementation plan](docs/final-plan.md)
- [Research and architecture rationale](docs/research/direction-of-travel.md)
- [Current StackChan/chat-stick lessons](docs/research/current-reference-lessons.md)
- [Hardware bring-up log](docs/bringup-log.md)
- [Firmware and flashing](firmware/README.md)
- [Cloudflare relay](cloud/README.md)
- [Device relay protocol](docs/device-protocol.md)
- [Audio duplex diagnostic](diagnostics/audio_duplex/README.md)
- [Display/audio soak diagnostic](diagnostics/display_audio_soak/README.md)
- [Display transfer-ownership diagnostic](diagnostics/display_transfer/README.md)

No cloud credentials belong in this repository.
