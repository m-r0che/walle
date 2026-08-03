export const PROTOCOL_VERSION = 1;
export const SAMPLE_RATE_HZ = 24_000;
export const MAX_AUDIO_SAMPLES = 960; // 40 ms of 24 kHz mono PCM16
export const MAX_INPUT_SAMPLES = SAMPLE_RATE_HZ * 30;
export const AUDIO_HEADER_BYTES = 12;

const MAGIC_0 = 0x57; // W
const MAGIC_1 = 0x41; // A

export enum AudioFrameKind {
  InputPcm16 = 1,
  OutputPcm16 = 2,
}

export type AudioFrame = {
  kind: AudioFrameKind;
  sequence: number;
  flags: number;
  sampleCount: number;
  pcm: Uint8Array;
};

export type DeviceTelemetryReport = {
  v: 1;
  type: "telemetry.report";
  epoch: number;
  state: number;
  captureAccepting: boolean;
  queueDepth: number;
  startsQueued: number;
  startUnready: number;
  startMutexBusy: number;
  startAlreadyActive: number;
  turnsStarted: number;
  turnsCommitted: number;
  audioDropped: number;
  audioUnready: number;
  audioMutexBusy: number;
  audioNotAccepting: number;
  audioBackpressure: number;
  audioQueueFull: number;
  audioStreamInactive: number;
  audioSendFailures: number;
  outputEventDrops: number;
  protocolErrors: number;
  socketRestarts: number;
};

export type DeviceControlMessage =
  | {
      v: 1;
      type: "hello";
      firmware: string;
      sampleRate: 24_000;
      channels: 1;
      sampleFormat: "pcm16le";
      sessionEpoch?: number;
    }
  | { v: 1; type: "turn.start"; turnId: string }
  | { v: 1; type: "turn.commit"; turnId: string }
  | { v: 1; type: "turn.cancel"; turnId: string }
  | { v: 1; type: "response.cancel"; turnId: string }
  | {
      v: 1;
      type: "playback.report";
      turnId: string;
      source: "remote" | "local";
      samples: number;
      firstCodecWriteMs?: number;
    }
  | DeviceTelemetryReport
  | { v: 1; type: "ping"; nonce: string };

function bytesOf(message: ArrayBuffer | ArrayBufferView): Uint8Array {
  if (message instanceof ArrayBuffer) return new Uint8Array(message);
  return new Uint8Array(
    message.buffer,
    message.byteOffset,
    message.byteLength,
  );
}

export function decodeAudioFrame(
  message: ArrayBuffer | ArrayBufferView,
): AudioFrame {
  const bytes = bytesOf(message);
  if (bytes.byteLength < AUDIO_HEADER_BYTES) {
    throw new Error("audio frame is shorter than its header");
  }
  if (bytes[0] !== MAGIC_0 || bytes[1] !== MAGIC_1) {
    throw new Error("audio frame magic is invalid");
  }
  if (bytes[2] !== PROTOCOL_VERSION) {
    throw new Error("audio protocol version is unsupported");
  }
  const kind = bytes[3];
  if (kind !== AudioFrameKind.InputPcm16 && kind !== AudioFrameKind.OutputPcm16) {
    throw new Error("audio frame kind is invalid");
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const sequence = view.getUint32(4, true);
  const sampleCount = view.getUint16(8, true);
  const flags = view.getUint16(10, true);
  if (sampleCount === 0 || sampleCount > MAX_AUDIO_SAMPLES) {
    throw new Error("audio sample count is out of bounds");
  }
  const payloadBytes = sampleCount * 2;
  if (bytes.byteLength !== AUDIO_HEADER_BYTES + payloadBytes) {
    throw new Error("audio payload length does not match sample count");
  }

  return {
    kind,
    sequence,
    flags,
    sampleCount,
    pcm: bytes.slice(AUDIO_HEADER_BYTES),
  };
}

export function encodeAudioFrame(frame: AudioFrame): ArrayBuffer {
  if (frame.sampleCount === 0 || frame.sampleCount > MAX_AUDIO_SAMPLES) {
    throw new Error("audio sample count is out of bounds");
  }
  if (frame.pcm.byteLength !== frame.sampleCount * 2) {
    throw new Error("audio payload length does not match sample count");
  }

  const bytes = new Uint8Array(AUDIO_HEADER_BYTES + frame.pcm.byteLength);
  const view = new DataView(bytes.buffer);
  bytes[0] = MAGIC_0;
  bytes[1] = MAGIC_1;
  bytes[2] = PROTOCOL_VERSION;
  bytes[3] = frame.kind;
  view.setUint32(4, frame.sequence, true);
  view.setUint16(8, frame.sampleCount, true);
  view.setUint16(10, frame.flags, true);
  bytes.set(frame.pcm, AUDIO_HEADER_BYTES);
  return bytes.buffer;
}

export function decodeControlMessage(text: string): DeviceControlMessage {
  if (new TextEncoder().encode(text).byteLength > 4_096) {
    throw new Error("control message is too large");
  }
  const value: unknown = JSON.parse(text);
  if (typeof value !== "object" || value === null) {
    throw new Error("control message must be an object");
  }
  const message = value as Record<string, unknown>;
  if (message.v !== PROTOCOL_VERSION || typeof message.type !== "string") {
    throw new Error("control message version or type is invalid");
  }

  switch (message.type) {
    case "hello":
      if (
        typeof message.firmware !== "string" ||
        message.firmware.length === 0 ||
        message.firmware.length > 64 ||
        message.sampleRate !== SAMPLE_RATE_HZ ||
        message.channels !== 1 ||
        message.sampleFormat !== "pcm16le" ||
        (message.sessionEpoch !== undefined &&
          (typeof message.sessionEpoch !== "number" ||
            !Number.isInteger(message.sessionEpoch) ||
            message.sessionEpoch <= 0 ||
            message.sessionEpoch > 0xffff_ffff))
      ) {
        throw new Error("hello audio format is invalid");
      }
      return message as DeviceControlMessage;
    case "turn.start":
    case "turn.commit":
    case "turn.cancel":
    case "response.cancel":
      if (
        typeof message.turnId !== "string" ||
        message.turnId.length === 0 ||
        message.turnId.length > 64
      ) {
        throw new Error("turn id is invalid");
      }
      return message as DeviceControlMessage;
    case "playback.report":
      if (
        typeof message.turnId !== "string" ||
        message.turnId.length === 0 ||
        message.turnId.length > 64 ||
        (message.source !== "remote" && message.source !== "local") ||
        typeof message.samples !== "number" ||
        !Number.isInteger(message.samples) ||
        message.samples <= 0 ||
        message.samples > 7_200_000 ||
        (message.firstCodecWriteMs !== undefined && (
          typeof message.firstCodecWriteMs !== "number" ||
          !Number.isInteger(message.firstCodecWriteMs) ||
          message.firstCodecWriteMs < 0 ||
          message.firstCodecWriteMs > 330_000
        ))
      ) {
        throw new Error("playback report is invalid");
      }
      return message as DeviceControlMessage;
    case "telemetry.report": {
      const counts = [
        message.queueDepth,
        message.startsQueued,
        message.startUnready,
        message.startMutexBusy,
        message.startAlreadyActive,
        message.turnsStarted,
        message.turnsCommitted,
        message.audioDropped,
        message.audioUnready,
        message.audioMutexBusy,
        message.audioNotAccepting,
        message.audioBackpressure,
        message.audioQueueFull,
        message.audioStreamInactive,
        message.audioSendFailures,
        message.outputEventDrops,
        message.protocolErrors,
        message.socketRestarts,
      ];
      if (typeof message.epoch !== "number"
          || !Number.isInteger(message.epoch) || message.epoch <= 0
          || message.epoch > 0xffff_ffff
          || typeof message.state !== "number"
          || !Number.isInteger(message.state) || message.state < 0
          || message.state > 7
          || typeof message.captureAccepting !== "boolean"
          || counts.some((count) => typeof count !== "number"
            || !Number.isInteger(count) || count < 0
            || count > 0xffff_ffff)
          || (message.queueDepth as number) > 640) {
        throw new Error("device telemetry is invalid");
      }
      return message as DeviceTelemetryReport;
    }
    case "ping":
      if (typeof message.nonce !== "string" || message.nonce.length > 64) {
        throw new Error("ping nonce is invalid");
      }
      return message as DeviceControlMessage;
    default:
      throw new Error("control message type is unsupported");
  }
}
