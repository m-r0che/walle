import { describe, expect, it } from "vitest";

import {
  AUDIO_HEADER_BYTES,
  AudioFrameKind,
  decodeAudioFrame,
  decodeControlMessage,
  encodeAudioFrame,
} from "../src/protocol";

describe("binary audio protocol", () => {
  it("round-trips bounded PCM16 frames", () => {
    const pcm = new Uint8Array([1, 2, 3, 4, 5, 6]);
    const encoded = encodeAudioFrame({
      kind: AudioFrameKind.InputPcm16,
      sequence: 42,
      flags: 3,
      sampleCount: 3,
      pcm,
    });
    expect(encoded.byteLength).toBe(AUDIO_HEADER_BYTES + pcm.byteLength);
    expect(decodeAudioFrame(encoded)).toEqual({
      kind: AudioFrameKind.InputPcm16,
      sequence: 42,
      flags: 3,
      sampleCount: 3,
      pcm,
    });
  });

  it("rejects malformed, oversized, and length-mismatched frames", () => {
    expect(() => decodeAudioFrame(new ArrayBuffer(4))).toThrow(/header/);

    const valid = new Uint8Array(encodeAudioFrame({
      kind: AudioFrameKind.InputPcm16,
      sequence: 0,
      flags: 0,
      sampleCount: 1,
      pcm: new Uint8Array([0, 0]),
    }));
    valid[0] = 0;
    expect(() => decodeAudioFrame(valid)).toThrow(/magic/);

    expect(() => encodeAudioFrame({
      kind: AudioFrameKind.InputPcm16,
      sequence: 0,
      flags: 0,
      sampleCount: 961,
      pcm: new Uint8Array(1_922),
    })).toThrow(/bounds/);

    expect(() => encodeAudioFrame({
      kind: AudioFrameKind.InputPcm16,
      sequence: 0,
      flags: 0,
      sampleCount: 2,
      pcm: new Uint8Array(2),
    })).toThrow(/length/);
  });
});

describe("control protocol", () => {
  it("accepts the fixed device audio format", () => {
    expect(decodeControlMessage(JSON.stringify({
      v: 1,
      type: "hello",
      firmware: "local-test",
      sampleRate: 24_000,
      channels: 1,
      sampleFormat: "pcm16le",
      sessionEpoch: 7,
    }))).toMatchObject({
      type: "hello",
      sampleRate: 24_000,
      sessionEpoch: 7,
    });
  });

  it("rejects unsupported formats and unbounded identifiers", () => {
    expect(() => decodeControlMessage(JSON.stringify({
      v: 1,
      type: "hello",
      firmware: "test",
      sampleRate: 16_000,
      channels: 1,
      sampleFormat: "pcm16le",
    }))).toThrow(/format/);

    expect(() => decodeControlMessage(JSON.stringify({
      v: 1,
      type: "hello",
      firmware: "test",
      sampleRate: 24_000,
      channels: 1,
      sampleFormat: "pcm16le",
      sessionEpoch: 0,
    }))).toThrow(/format/);

    expect(() => decodeControlMessage(JSON.stringify({
      v: 1,
      type: "turn.start",
      turnId: "x".repeat(65),
    }))).toThrow(/turn id/);

    expect(decodeControlMessage(JSON.stringify({
      v: 1,
      type: "telemetry.report",
      epoch: 7,
      state: 6,
      captureAccepting: false,
      queueDepth: 0,
      startsQueued: 3,
      startUnready: 1,
      startMutexBusy: 0,
      startAlreadyActive: 0,
      turnsStarted: 3,
      turnsCommitted: 3,
      audioDropped: 0,
      audioUnready: 0,
      audioMutexBusy: 0,
      audioNotAccepting: 0,
      audioBackpressure: 0,
      audioQueueFull: 0,
      audioStreamInactive: 0,
      audioSendFailures: 0,
      outputEventDrops: 0,
      protocolErrors: 0,
      socketRestarts: 1,
    }))).toMatchObject({ type: "telemetry.report", epoch: 7 });

    expect(decodeControlMessage(JSON.stringify({
      v: 1,
      type: "response.cancel",
      turnId: "turn-1",
    }))).toMatchObject({ type: "response.cancel", turnId: "turn-1" });

    expect(decodeControlMessage(JSON.stringify({
      v: 1,
      type: "playback.report",
      turnId: "turn-1",
      source: "remote",
      samples: 48_000,
      firstCodecWriteMs: 1_284,
    }))).toMatchObject({
      type: "playback.report",
      source: "remote",
      firstCodecWriteMs: 1_284,
    });

    expect(decodeControlMessage(JSON.stringify({
      v: 1,
      type: "playback.report",
      turnId: "legacy-turn",
      source: "local",
      samples: 24_000,
    }))).toMatchObject({ type: "playback.report", source: "local" });

    expect(() => decodeControlMessage(JSON.stringify({
      v: 1,
      type: "playback.report",
      turnId: "turn-1",
      source: "network",
      samples: 48_000,
      firstCodecWriteMs: 1_284,
    }))).toThrow(/playback report/);

    expect(() => decodeControlMessage(JSON.stringify({
      v: 1,
      type: "playback.report",
      turnId: "turn-1",
      source: "remote",
      samples: 48_000,
      firstCodecWriteMs: 330_001,
    }))).toThrow(/playback report/);
  });
});
