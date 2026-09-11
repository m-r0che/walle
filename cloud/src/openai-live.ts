import {
  at,
  type Effect,
  initialState,
  type Input,
  type Instant,
  LIVE_POLICY,
  type LiveEvent,
  type LivePolicy,
  nextDeadline,
  reduce,
  renderFault,
  type SessionState,
  type StreamIntent,
} from "./live-turn";
import { WALLE_LIVE_INSTRUCTIONS } from "./personality";
import { SAMPLE_RATE_HZ } from "./protocol";

export const OPENAI_LIVE_MODEL = "gpt-live-1";
export const OPENAI_LIVE_VOICE = "ballad";
const OPENAI_LIVE_URL = "https://api.openai.com/v1/live/sessions";
const MAX_SERVER_EVENT_CHARS = 512_000;

export type LiveTransport = {
  send(message: string): void;
  close(code?: number, reason?: string): void;
  onMessage(listener: (message: unknown) => void): void;
  onClose(listener: (code: number, reason: string) => void): void;
  onError(listener: (error: unknown) => void): void;
};

export type LiveTransportConnector = (
  apiKey: string,
  safetyIdentifier: string,
) => Promise<LiveTransport>;

export type OpenAILiveHandlers = {
  onAudio(turnId: string, pcm: Uint8Array): void;
  onTranscript?(turnId: string, transcript: string): void;
  onDone(turnId: string, outputSamples: number): void;
  onFailed(turnId: string, reason: string): void;
  onUnavailable?(reason: string): void;
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

export async function connectLiveTransport(
  apiKey: string,
  safetyId: string,
): Promise<LiveTransport> {
  if (apiKey.length < 16) throw new Error("OpenAI API key is unavailable");
  const response = await fetch(OPENAI_LIVE_URL, {
    headers: {
      Upgrade: "websocket",
      Authorization: `Bearer ${apiKey}`,
      "OpenAI-Safety-Identifier": safetyId,
    },
  });
  const webSocket = response.webSocket;
  if (response.status !== 101 || webSocket === null) {
    throw new Error(`OpenAI WebSocket upgrade failed (${response.status})`);
  }
  webSocket.binaryType = "arraybuffer";
  webSocket.accept();
  let closed = false;
  webSocket.addEventListener("close", () => {
    closed = true;
  });
  return {
    send: (message) => webSocket.send(message),
    close: (code, reason) => {
      if (closed) return;
      closed = true;
      webSocket.close(code, reason);
    },
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

function encodeIntent(intent: StreamIntent): string {
  switch (intent.kind) {
    case "start":
      return JSON.stringify({
        type: "session.start",
        session: {
          model: OPENAI_LIVE_MODEL,
          instructions: WALLE_LIVE_INSTRUCTIONS,
          audio: {
            format: { type: "audio/pcm", rate: SAMPLE_RATE_HZ },
            output: { voice: OPENAI_LIVE_VOICE },
          },
        },
      });
    case "appendInput":
      return JSON.stringify({
        type: "session.input_audio.append",
        audio: bytesToBase64(intent.pcm),
      });
    case "mute":
      return JSON.stringify({ type: "session.input_audio.mute" });
    case "unmute":
      return JSON.stringify({ type: "session.input_audio.unmute" });
  }
}

const malformed = (detail: string): LiveEvent =>
  ({ kind: "error", fault: { code: "upstream_malformed", detail } });

function parseServerEvent(message: unknown): LiveEvent {
  if (typeof message !== "string" || message.length > MAX_SERVER_EVENT_CHARS) {
    return malformed("invalid_upstream_message");
  }
  let value: unknown;
  try {
    value = JSON.parse(message);
  } catch {
    return malformed("invalid_upstream_json");
  }
  const event = record(value);
  if (event === null || typeof event.type !== "string") {
    return malformed("invalid_upstream_event");
  }
  switch (event.type) {
    case "session.started":
      return { kind: "ready" };
    case "session.output_audio.delta": {
      if (typeof event.delta !== "string") return malformed("invalid_output_audio");
      let pcm: Uint8Array;
      try {
        pcm = base64ToBytes(event.delta);
      } catch {
        return malformed("invalid_output_base64");
      }
      if (pcm.byteLength === 0 || pcm.byteLength % 2 !== 0) {
        return malformed("invalid_output_audio");
      }
      return { kind: "output-audio", pcm };
    }
    case "session.output_transcript.delta":
      return typeof event.delta === "string"
        ? { kind: "output-text", delta: event.delta }
        : malformed("invalid_output_transcript");
    case "session.input_transcript.delta":
      return typeof event.delta === "string"
        ? { kind: "input-text", delta: event.delta }
        : malformed("invalid_input_transcript");
    case "error": {
      const upstream = record(event.error);
      const code = typeof upstream?.code === "string"
        ? upstream.code.slice(0, 80)
        : "unknown";
      const detail = typeof upstream?.message === "string"
        ? `${code}:${upstream.message.slice(0, 240)}`
        : code;
      return { kind: "error", fault: { code: "upstream_error", detail } };
    }
    default:
      return { kind: "ignored", type: event.type.slice(0, 80) };
  }
}

type Starting = { resolve: () => void; reject: (error: Error) => void };

export class OpenAILiveSession {
  private state: SessionState;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private starting: Starting | null = null;

  private constructor(
    private readonly transport: LiveTransport,
    private readonly handlers: OpenAILiveHandlers,
    private readonly policy: LivePolicy,
  ) {
    this.state = initialState(at(Date.now()), policy);
    transport.onMessage((message) =>
      this.dispatch({ kind: "upstream", event: parseServerEvent(message) }));
    transport.onClose((code, reason) => this.dispatch({
      kind: "upstream",
      event: {
        kind: "closed",
        fault: { code: "upstream_closed", detail: reason || String(code) },
      },
    }));
    transport.onError(() => this.dispatch({
      kind: "upstream",
      event: { kind: "closed", fault: { code: "upstream_error" } },
    }));
  }

  static async connect(
    apiKey: string,
    installationId: string,
    handlers: OpenAILiveHandlers,
    connector: LiveTransportConnector = connectLiveTransport,
  ): Promise<OpenAILiveSession> {
    const safetyId = await safetyIdentifier(installationId);
    const transport = await connector(apiKey, safetyId);
    const session = new OpenAILiveSession(transport, handlers, LIVE_POLICY);
    session.arm(at(Date.now()));
    try {
      await new Promise<void>((resolve, reject) => {
        session.starting = { resolve, reject };
        transport.send(encodeIntent({ kind: "start" }));
      });
    } catch (error) {
      session.close(1011, "session start failed");
      throw error;
    }
    return session;
  }

  beginTurn(turnId: string): void {
    this.dispatch({ kind: "begin", turnId });
  }

  appendPcm(turnId: string, pcm: Uint8Array): void {
    this.dispatch({ kind: "append", turnId, pcm });
  }

  commitTurn(turnId: string): void {
    this.dispatch({ kind: "commit", turnId });
  }

  cancelTurn(turnId: string): void {
    this.dispatch({ kind: "cancel", turnId });
  }

  close(code = 1000, reason = "session complete"): void {
    if (this.state.phase === "gone") return;
    this.dispatch({ kind: "shutdown" });
    this.transport.close(code, reason);
  }

  private dispatch(input: Input): void {
    const now = at(Date.now());
    const outcome = reduce(this.policy, this.state, input, now);
    if (!outcome.ok) throw new Error(outcome.refusal);
    this.state = outcome.state;
    this.arm(now);
    for (const effect of outcome.effects) this.apply(effect);
  }

  private arm(now: Instant): void {
    clearTimeout(this.timer);
    const due = nextDeadline(this.state);
    this.timer = due === null
      ? undefined
      : setTimeout(() => this.dispatch({ kind: "tick" }), Math.max(0, due - now));
  }

  private apply(effect: Effect): void {
    switch (effect.kind) {
      case "send":
        this.transport.send(encodeIntent(effect.intent));
        return;
      case "speak":
        for (const frame of effect.frames) this.handlers.onAudio(effect.turnId, frame);
        return;
      case "affect":
        this.handlers.onTranscript?.(effect.turnId, effect.transcript);
        return;
      case "finish":
        if (effect.tail !== null) this.handlers.onAudio(effect.turnId, effect.tail);
        this.handlers.onDone(effect.turnId, effect.outputSamples);
        return;
      case "fault":
        this.handlers.onFailed(effect.turnId, renderFault(effect.fault));
        return;
      case "ready":
        this.starting?.resolve();
        this.starting = null;
        return;
      case "lost": {
        const reason = renderFault(effect.fault);
        if (this.starting === null) {
          this.handlers.onUnavailable?.(reason);
        } else {
          this.starting.reject(new Error(reason));
          this.starting = null;
        }
        this.transport.close(1011, reason);
        return;
      }
      case "observe": {
        const { kind, ...detail } = effect.note;
        console.log(JSON.stringify({ event: `live.${kind}`, ...detail }));
        return;
      }
    }
  }
}
