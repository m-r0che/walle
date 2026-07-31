# Walle

A small voice-interactive robot face for the Waveshare ESP32-S3-Touch-AMOLED-1.8.

The device will render a locally animated neon face, use OpenAI Realtime for low-latency speech, and coordinate tools through Cloudflare. Initial tools are a shopping list and a home paper-printer bridge.

## Status

Core hardware bring-up is complete. The original 16 MB flash was privately backed up and verified before any write. Display, touch, speaker, microphone, PSRAM, and simultaneous 24 kHz duplex audio have passed repeatable diagnostics. The device currently runs an offline push-to-talk echo loop on bounded pre-commit, capture, and playback rings: hold the face to record, release to replay, and watch the mouth follow real playback audio. Short taps are discarded before commit, and a new press interrupts playback locally. Bottom LVGL buttons adjust codec volume from 10–100% at runtime and persist the setting; the center button cycles the authored expression deck. The dual-network firmware now selects either configured Wi-Fi, verifies the Cloudflare certificate chain, authenticates to the pinned installation Agent, and non-blockingly duplicates committed microphone audio into bounded sequenced PCM frames while offline audio remains authoritative. TLS reduced contiguous internal RAM below the former complete-face DMA requirement, so display reliability now combines completion ownership with a 110-row internal buffer and an enforced 35 ms interval between every panel transaction. The resulting ~14.3 FPS / ~28.6-transfer-per-second face remained physically visible beyond 4,400 transfers with stable memory and no display, protocol, or audio errors. A cold-boot-visible device turn delivered 287 frames / 73,472 samples / 146,944 bytes to the deployed echo relay while local replay continued normally. The relay now uses manager-owned connection epochs, ready/pong deadlines, explicit client recreation, capped jittered backoff, and a bounded two-minute prototype session lifetime. Forced close, Worker deployment, and suppressed-pong tests all recovered without reboot, and PCM turns passed afterward. A remote-playback experiment was rejected after measuring per-message throughput pressure and a separate 40 ms audio-task candidate that physically blanked the display. Network-owned batching preserves the proven 256-sample codec cadence while sending bounded 960-sample frames. Complete-turn remote PCM playback now requires matching epoch, token, sequence, hash, buffered count, relay count, and local count before the audio task can select it; otherwise the untouched local capture wins. Exact short/long/post-reconnect remote playback, omitted-frame and wrong-count local fallbacks, interruption, a two-minute visibility soak, and final cold boot passed. The bounded PCM path is ready for the Cloudflare-side OpenAI Realtime adapter; OpenAI credentials have not yet been supplied.

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
