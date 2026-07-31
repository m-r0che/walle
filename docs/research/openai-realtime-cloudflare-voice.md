# OpenAI Realtime and Cloudflare voice direction

**Research date:** 2026-07-31

## OpenAI Realtime facts used

Current official OpenAI documentation specifies server-to-server WebSocket connections at:

`wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1`

The backend authenticates with a standard API key in `Authorization: Bearer …` and may provide a stable privacy-preserving `OpenAI-Safety-Identifier`. Realtime WebSocket events are JSON text. PCM input is Base64-encoded in `input_audio_buffer.append`; with VAD disabled, push-to-talk clients manually send `input_audio_buffer.commit` and `response.create`, and clear the input buffer before a new turn. Generated PCM is delivered only in `response.output_audio.delta`; `response.output_audio.done` and `response.done` do not contain audio bytes.

The session update used here fixes:

- model `gpt-realtime-2.1`;
- audio-only output;
- 24 kHz `audio/pcm` input;
- `turn_detection: null` for device-owned push-to-talk;
- PCM output;
- voice `marin`, one of OpenAI's recommended quality voices;
- a short, truthful, warm robot personality with concise spoken output.

Sources:

- <https://platform.openai.com/docs/guides/realtime-websocket>
- <https://platform.openai.com/docs/guides/realtime-conversations>

## Cloudflare outbound WebSocket facts used

Workers can establish an external WebSocket with `fetch()` plus `Upgrade: websocket`, which permits the required Authorization and safety headers. A successful response exposes `response.webSocket`; code sets `binaryType = "arraybuffer"`, calls `accept()`, and installs bounded event handlers. The installation Agent disables hibernation while it owns this outbound session because in-memory variables, timers, and promises do not survive Agent hibernation.

Sources:

- <https://developers.cloudflare.com/workers/examples/websockets/>
- <https://developers.cloudflare.com/workers/runtime-apis/websockets/>
- <https://developers.cloudflare.com/agents/api-reference/websockets/>

## Why not replace the relay with `@cloudflare/voice`

Cloudflare's beta voice mixin is useful reference architecture, particularly its call lifetime, abort-driven interruption, metrics, and persistence patterns. It is not a drop-in transport for this device because its documented pipeline assumes 16 kHz continuous PCM, model-driven turn detection, STT → text LLM → TTS, its own browser-oriented voice protocol, and typically MP3 output. This project has a physically validated 24 kHz push-to-talk PCM protocol, direct speech-to-speech requirement, complete-turn output validation, and authoritative local fallback.

The project therefore retains the Agents SDK/Durable Object coordinator and custom device protocol, while implementing a narrow OpenAI Realtime adapter inside the Agent. The device never receives the OpenAI key and never connects directly to OpenAI.

Source:

- <https://developers.cloudflare.com/agents/communication-channels/voice/>
