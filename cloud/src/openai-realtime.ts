import { WALLE_PERSONALITY_INSTRUCTIONS } from "./personality";

export const OPENAI_REALTIME_MODEL = "gpt-realtime-2.1";
export const OPENAI_REALTIME_VOICE = "marin";
export const MAX_OUTPUT_SAMPLES = 24_000 * 300;
const SAMPLE_RATE = 24_000;
const MAX_OUTPUT_BYTES = MAX_OUTPUT_SAMPLES * 2;
const MAX_SERVER_EVENT_CHARS = 512_000;
const DEVICE_FRAME_BYTES = 960 * 2;
const SESSION_READY_TIMEOUT_MS = 10_000;

export type RealtimeTransport = {
  send(message: string): void;
  close(code?: number, reason?: string): void;
  onMessage(listener: (message: unknown) => void): void;
  onClose(listener: (code: number, reason: string) => void): void;
  onError(listener: (error: unknown) => void): void;
};

export type RealtimeTransportConnector = (
  apiKey: string,
  safetyIdentifier: string,
) => Promise<RealtimeTransport>;

export type OpenAIRealtimeHandlers = {
  onAudio(turnId: string, pcm: Uint8Array): void;
  onDone(turnId: string, outputSamples: number): void;
  onFailed(turnId: string, reason: string): void;
  onUnavailable?(reason: string): void;
};

type ActiveTurn = {
  turnId: string;
  responseId: string | null;
  outputBytes: number;
};

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null
    ? value as Record<string, unknown>
    : null;
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let offset = 0; offset < bytes.byteLength; offset += 0x8000) {
    const chunk = bytes.subarray(offset, offset + 0x8000);
    binary += String.fromCharCode(...chunk);
  }
  return btoa(binary);
}

function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index++) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
}

export async function safetyIdentifier(value: string): Promise<string> {
  const digest = await crypto.subtle.digest(
    "SHA-256",
    new TextEncoder().encode(value),
  );
  return Array.from(new Uint8Array(digest), (byte) =>
    byte.toString(16).padStart(2, "0")).join("");
}

export async function connectOpenAITransport(
  apiKey: string,
  safetyId: string,
): Promise<RealtimeTransport> {
  if (apiKey.length < 16) throw new Error("OpenAI API key is unavailable");
  const response = await fetch(
    `https://api.openai.com/v1/realtime?model=${OPENAI_REALTIME_MODEL}`,
    {
      headers: {
        Upgrade: "websocket",
        Authorization: `Bearer ${apiKey}`,
        "OpenAI-Safety-Identifier": safetyId,
      },
    },
  );
  const webSocket = response.webSocket;
  if (response.status !== 101 || webSocket === null) {
    throw new Error(`OpenAI WebSocket upgrade failed (${response.status})`);
  }
  webSocket.binaryType = "arraybuffer";
  webSocket.accept();
  return {
    send: (message) => webSocket.send(message),
    close: (code, reason) => webSocket.close(code, reason),
    onMessage: (listener) => {
      webSocket.addEventListener("message", (event) => listener(event.data));
    },
    onClose: (listener) => {
      webSocket.addEventListener("close", (event) =>
        listener(event.code, event.reason));
    },
    onError: (listener) => {
      webSocket.addEventListener("error", (event) => listener(event));
    },
  };
}

export class OpenAIRealtimeSession {
  private activeTurn: ActiveTurn | null = null;
  private ready = false;
  private closed = false;
  private readyResolve: (() => void) | null = null;
  private readyReject: ((error: Error) => void) | null = null;
  private unavailableNotified = false;

  private constructor(
    private readonly transport: RealtimeTransport,
    private readonly handlers: OpenAIRealtimeHandlers,
  ) {
    transport.onMessage((message) => this.handleMessage(message));
    transport.onClose((code, reason) => {
      const unexpected = !this.closed;
      this.closed = true;
      this.rejectReady(new Error(`OpenAI WebSocket closed (${code})`));
      this.failActive(`upstream_closed:${reason || code}`);
      if (unexpected) this.notifyUnavailable("upstream_closed");
    });
    transport.onError(() => {
      if (this.closed) return;
      this.closed = true;
      this.rejectReady(new Error("OpenAI WebSocket error"));
      this.failActive("upstream_error");
      this.transport.close(1011, "upstream error");
      this.notifyUnavailable("upstream_error");
    });
  }

  static async connect(
    apiKey: string,
    installationId: string,
    handlers: OpenAIRealtimeHandlers,
    connector: RealtimeTransportConnector = connectOpenAITransport,
  ): Promise<OpenAIRealtimeSession> {
    const safetyId = await safetyIdentifier(installationId);
    const transport = await connector(apiKey, safetyId);
    const session = new OpenAIRealtimeSession(transport, handlers);
    const ready = new Promise<void>((resolve, reject) => {
      session.readyResolve = resolve;
      session.readyReject = reject;
    });
    session.send({
      type: "session.update",
      session: {
        type: "realtime",
        model: OPENAI_REALTIME_MODEL,
        output_modalities: ["audio"],
        instructions: WALLE_PERSONALITY_INSTRUCTIONS,
        audio: {
          input: {
            format: { type: "audio/pcm", rate: SAMPLE_RATE },
            turn_detection: null,
          },
          output: {
            format: { type: "audio/pcm", rate: SAMPLE_RATE },
            voice: OPENAI_REALTIME_VOICE,
          },
        },
      },
    });

    let timeout: ReturnType<typeof setTimeout> | undefined;
    try {
      await Promise.race([
        ready,
        new Promise<never>((_, reject) => {
          timeout = setTimeout(
            () => reject(new Error("OpenAI session readiness timed out")),
            SESSION_READY_TIMEOUT_MS,
          );
        }),
      ]);
      return session;
    } catch (error) {
      session.close(1011, "session initialization failed");
      throw error;
    } finally {
      if (timeout !== undefined) clearTimeout(timeout);
    }
  }

  beginTurn(turnId: string): void {
    if (!this.ready || this.closed || this.activeTurn !== null) {
      throw new Error("OpenAI session cannot start a turn");
    }
    this.activeTurn = { turnId, responseId: null, outputBytes: 0 };
    this.send({ type: "input_audio_buffer.clear" });
  }

  appendPcm(turnId: string, pcm: Uint8Array): void {
    const turn = this.requireTurn(turnId);
    if (turn.responseId !== null || pcm.byteLength === 0
        || pcm.byteLength % 2 !== 0 || pcm.byteLength > DEVICE_FRAME_BYTES) {
      throw new Error("OpenAI input PCM is invalid for the active turn");
    }
    this.send({
      type: "input_audio_buffer.append",
      audio: bytesToBase64(pcm),
    });
  }

  commitTurn(turnId: string): void {
    const turn = this.requireTurn(turnId);
    if (turn.responseId !== null) {
      throw new Error("OpenAI turn was already committed");
    }
    this.send({ type: "input_audio_buffer.commit" });
    this.send({
      type: "response.create",
      response: {
        output_modalities: ["audio"],
        metadata: { turnId },
      },
    });
  }

  cancelTurn(turnId: string): void {
    const turn = this.activeTurn;
    if (turn === null || turn.turnId !== turnId) return;
    if (turn.responseId !== null) {
      this.send({ type: "response.cancel", response_id: turn.responseId });
    }
    this.send({ type: "input_audio_buffer.clear" });
    this.activeTurn = null;
  }

  close(code = 1000, reason = "session complete"): void {
    if (this.closed) return;
    this.closed = true;
    this.transport.close(code, reason);
    this.failActive("session_closed");
  }

  private requireTurn(turnId: string): ActiveTurn {
    const turn = this.activeTurn;
    if (turn === null || turn.turnId !== turnId) {
      throw new Error("OpenAI event does not match the active turn");
    }
    return turn;
  }

  private send(value: unknown): void {
    if (this.closed) throw new Error("OpenAI session is closed");
    this.transport.send(JSON.stringify(value));
  }

  private handleMessage(message: unknown): void {
    if (typeof message !== "string" || message.length > MAX_SERVER_EVENT_CHARS) {
      this.failActive("invalid_upstream_message");
      return;
    }
    let value: unknown;
    try {
      value = JSON.parse(message);
    } catch {
      this.failActive("invalid_upstream_json");
      return;
    }
    const event = record(value);
    if (event === null || typeof event.type !== "string") {
      this.failActive("invalid_upstream_event");
      return;
    }

    switch (event.type) {
      case "session.updated":
        this.ready = true;
        this.readyResolve?.();
        this.readyResolve = null;
        this.readyReject = null;
        return;
      case "response.created":
        this.handleResponseCreated(event);
        return;
      case "response.output_audio.delta":
        this.handleAudioDelta(event);
        return;
      case "response.done":
        this.handleResponseDone(event);
        return;
      case "error": {
        const upstream = record(event.error);
        const code = typeof upstream?.code === "string"
          ? upstream.code.slice(0, 80)
          : "unknown";
        const message = typeof upstream?.message === "string"
          ? upstream.message.slice(0, 240)
          : "no upstream detail";
        this.rejectReady(new Error(
          `OpenAI session configuration failed (${code}): ${message}`,
        ));
        this.failActive(`openai_error:${code}`);
        return;
      }
      default:
        return;
    }
  }

  private handleResponseCreated(event: Record<string, unknown>): void {
    const turn = this.activeTurn;
    const response = record(event.response);
    const metadata = record(response?.metadata);
    if (turn === null || response === null
        || typeof response.id !== "string"
        || metadata?.turnId !== turn.turnId) {
      this.failActive("response_identity_mismatch");
      return;
    }
    turn.responseId = response.id;
  }

  private handleAudioDelta(event: Record<string, unknown>): void {
    const turn = this.activeTurn;
    if (turn === null || turn.responseId === null
        || event.response_id !== turn.responseId
        || typeof event.delta !== "string") {
      this.failActive("audio_identity_mismatch");
      return;
    }
    let bytes: Uint8Array;
    try {
      bytes = base64ToBytes(event.delta);
    } catch {
      this.failActive("invalid_output_base64");
      return;
    }
    if (bytes.byteLength === 0 || bytes.byteLength % 2 !== 0
        || turn.outputBytes + bytes.byteLength > MAX_OUTPUT_BYTES) {
      this.failActive("output_audio_out_of_bounds");
      return;
    }
    turn.outputBytes += bytes.byteLength;
    for (let offset = 0; offset < bytes.byteLength;
      offset += DEVICE_FRAME_BYTES) {
      this.handlers.onAudio(
        turn.turnId,
        bytes.slice(offset, offset + DEVICE_FRAME_BYTES),
      );
    }
  }

  private handleResponseDone(event: Record<string, unknown>): void {
    const turn = this.activeTurn;
    const response = record(event.response);
    if (turn === null || response === null || turn.responseId === null
        || response.id !== turn.responseId || response.status !== "completed"
        || turn.outputBytes === 0) {
      this.failActive("response_incomplete");
      return;
    }
    const outputSamples = turn.outputBytes / 2;
    this.activeTurn = null;
    this.handlers.onDone(turn.turnId, outputSamples);
  }

  private rejectReady(error: Error): void {
    this.readyReject?.(error);
    this.readyResolve = null;
    this.readyReject = null;
  }

  private failActive(reason: string): void {
    const turn = this.activeTurn;
    this.activeTurn = null;
    if (turn !== null) this.handlers.onFailed(turn.turnId, reason);
  }

  private notifyUnavailable(reason: string): void {
    if (this.unavailableNotified) return;
    this.unavailableNotified = true;
    this.handlers.onUnavailable?.(reason);
  }
}
