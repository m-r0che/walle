import { afterEach, describe, expect, it, vi } from "vitest";

import {
  type LiveTransport,
  type LiveTransportConnector,
  type OpenAILiveHandlers,
  OpenAILiveSession,
} from "../src/openai-live";

class FakeTransport implements LiveTransport {
  readonly sent: string[] = [];
  readonly closes: Array<{ code?: number; reason?: string }> = [];
  private messageListener: ((message: unknown) => void) | null = null;
  private closeListener: ((code: number, reason: string) => void) | null = null;
  private errorListener: ((error: unknown) => void) | null = null;

  send(message: string): void {
    this.sent.push(message);
  }

  close(code?: number, reason?: string): void {
    this.closes.push({ code, reason });
  }

  onMessage(listener: (message: unknown) => void): void {
    this.messageListener = listener;
  }

  onClose(listener: (code: number, reason: string) => void): void {
    this.closeListener = listener;
  }

  onError(listener: (error: unknown) => void): void {
    this.errorListener = listener;
  }

  message(value: unknown): void {
    this.messageListener?.(
      typeof value === "string" ? value : JSON.stringify(value),
    );
  }

  closed(code: number, reason: string): void {
    this.closeListener?.(code, reason);
  }

  error(error: unknown): void {
    this.errorListener?.(error);
  }
}

type Recorded = {
  audio: Array<{ turnId: string; pcm: Uint8Array }>;
  transcripts: Array<{ turnId: string; text: string }>;
  done: Array<{ turnId: string; samples: number }>;
  failures: Array<{ turnId: string; reason: string }>;
  unavailable: string[];
};

function recorder(): { handlers: OpenAILiveHandlers; recorded: Recorded } {
  const recorded: Recorded = {
    audio: [],
    transcripts: [],
    done: [],
    failures: [],
    unavailable: [],
  };
  return {
    recorded,
    handlers: {
      onAudio: (turnId, pcm) => recorded.audio.push({ turnId, pcm }),
      onTranscript: (turnId, text) => recorded.transcripts.push({ turnId, text }),
      onDone: (turnId, samples) => recorded.done.push({ turnId, samples }),
      onFailed: (turnId, reason) => recorded.failures.push({ turnId, reason }),
      onUnavailable: (reason) => recorded.unavailable.push(reason),
    },
  };
}

function decodeSent(transport: FakeTransport): Array<Record<string, unknown>> {
  return transport.sent.map((message) => JSON.parse(message));
}

function sentTypes(transport: FakeTransport): string[] {
  return decodeSent(transport).slice(1).map((event) => event.type as string);
}

function base64(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

function bytes(value: string): Uint8Array {
  return Uint8Array.from(atob(value), (char) => char.charCodeAt(0));
}

function pcm(length: number, seed = 0): Uint8Array {
  const out = new Uint8Array(length);
  for (let index = 0; index < length; index++) out[index] = (index + seed) & 0xff;
  return out;
}

function concat(parts: readonly Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((total, part) => total + part.byteLength, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

function startConnect(
  handlers: OpenAILiveHandlers,
): { pending: Promise<OpenAILiveSession>; transport: FakeTransport } {
  const transport = new FakeTransport();
  const connector: LiveTransportConnector = async (apiKey, safetyId) => {
    expect(apiKey).toBe("test-openai-key-with-safe-length");
    expect(safetyId).toMatch(/^[0-9a-f]{64}$/);
    return transport;
  };
  const pending = OpenAILiveSession.connect(
    "test-openai-key-with-safe-length",
    "installation-1",
    handlers,
    connector,
  );
  return { pending, transport };
}

async function untilStartSent(transport: FakeTransport): Promise<void> {
  for (let attempt = 0; transport.sent.length === 0 && attempt < 20; attempt++) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  expect(decodeSent(transport)[0]?.type).toBe("session.start");
}

async function open(
  handlers: OpenAILiveHandlers,
): Promise<{ session: OpenAILiveSession; transport: FakeTransport }> {
  const { pending, transport } = startConnect(handlers);
  await untilStartSent(transport);
  transport.message({ type: "session.started", session: { id: "sess-1" } });
  return { session: await pending, transport };
}

function speak(session: OpenAILiveSession, turnId: string, frames: Uint8Array[]): void {
  session.beginTurn(turnId);
  for (const frame of frames) session.appendPcm(turnId, frame);
  session.commitTurn(turnId);
}

const BURST = [
  "session.input_audio.unmute",
  "session.input_audio.append",
  "session.input_audio.mute",
];

describe("OpenAI Live session", () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it("starts a gpt-live-1 session with 24 kHz PCM, the ballad voice, and Walle's Live prompt", async () => {
    const { transport } = await open(recorder().handlers);
    expect(decodeSent(transport)).toEqual([{
      type: "session.start",
      session: {
        model: "gpt-live-1",
        instructions: expect.stringContaining("Your name is Walle"),
        audio: {
          format: { type: "audio/pcm", rate: 24_000 },
          output: { voice: "ballad" },
        },
      },
    }]);
  });

  it("holds the utterance, bursts it at commit, and finishes after the quiet gap with exact samples", async () => {
    const { handlers, recorded } = recorder();
    const { session, transport } = await open(handlers);
    vi.useFakeTimers();

    const held = [pcm(1_920, 1), pcm(1_000, 2)];
    session.beginTurn("turn-1");
    for (const frame of held) session.appendPcm("turn-1", frame);
    expect(sentTypes(transport)).toEqual([]);

    session.commitTurn("turn-1");
    expect(sentTypes(transport)).toEqual([
      "session.input_audio.unmute",
      "session.input_audio.append",
      "session.input_audio.append",
      "session.input_audio.mute",
    ]);
    const appended = decodeSent(transport)
      .filter((event) => event.type === "session.input_audio.append")
      .map((event) => bytes(event.audio as string));
    expect(appended.map((chunk) => chunk.byteLength)).toEqual([1_920, 1_000]);
    expect(concat(appended)).toEqual(concat(held));

    const reply = [pcm(3_000, 11), pcm(1_320, 17)];
    transport.message({ type: "session.output_audio.delta", delta: base64(reply[0]) });
    transport.message({ type: "session.output_transcript.delta", delta: "That's " });
    transport.message({ type: "session.output_transcript.delta", delta: "lovely." });
    transport.message({ type: "session.output_audio.delta", delta: base64(reply[1]) });
    expect(recorded.audio.map((entry) => entry.pcm.byteLength)).toEqual([1_920, 1_920]);
    expect(recorded.done).toEqual([]);

    vi.advanceTimersByTime(1_199);
    expect(recorded.transcripts).toEqual([{ turnId: "turn-1", text: "That's lovely." }]);
    expect(recorded.done).toEqual([]);

    vi.advanceTimersByTime(1);
    expect(recorded.audio.map((entry) => entry.turnId)).toEqual(["turn-1", "turn-1", "turn-1"]);
    expect(recorded.audio.map((entry) => entry.pcm.byteLength)).toEqual([1_920, 1_920, 480]);
    expect(concat(recorded.audio.map((entry) => entry.pcm))).toEqual(concat(reply));
    expect(recorded.done).toEqual([{ turnId: "turn-1", samples: 2_160 }]);
    expect(recorded.failures).toEqual([]);
    expect(sentTypes(transport)).toHaveLength(4);
  });

  it("cancels a held turn without sending anything and bursts the next one alone", async () => {
    const { handlers } = recorder();
    const { session, transport } = await open(handlers);
    session.beginTurn("turn-1");
    session.appendPcm("turn-1", pcm(1_920));
    session.cancelTurn("turn-1");
    expect(sentTypes(transport)).toEqual([]);

    speak(session, "turn-2", [pcm(960, 5)]);
    expect(sentTypes(transport)).toEqual(BURST);
    const [appended] = decodeSent(transport)
      .filter((event) => event.type === "session.input_audio.append");
    expect(bytes(appended.audio as string)).toEqual(pcm(960, 5));
  });

  it("drops a reply cancelled after commit and bursts the next turn only once the tail is quiet", async () => {
    const { handlers, recorded } = recorder();
    const { session, transport } = await open(handlers);
    vi.useFakeTimers();

    speak(session, "turn-1", [pcm(1_920)]);
    transport.message({ type: "session.output_audio.delta", delta: base64(pcm(1_920, 1)) });
    expect(recorded.audio.map((entry) => entry.pcm.byteLength)).toEqual([1_920]);

    session.cancelTurn("turn-1");
    transport.message({ type: "session.output_audio.delta", delta: base64(pcm(1_920, 2)) });
    expect(recorded.audio.map((entry) => entry.pcm.byteLength)).toEqual([1_920]);

    speak(session, "turn-2", [pcm(1_920, 3)]);
    expect(sentTypes(transport)).toEqual(BURST);

    vi.advanceTimersByTime(1_199);
    expect(sentTypes(transport)).toEqual(BURST);
    vi.advanceTimersByTime(1);
    expect(sentTypes(transport)).toEqual([...BURST, ...BURST]);

    transport.message({ type: "session.output_audio.delta", delta: base64(pcm(960, 4)) });
    vi.advanceTimersByTime(1_200);
    expect(recorded.audio.map((entry) => [entry.turnId, entry.pcm.byteLength])).toEqual([
      ["turn-1", 1_920],
      ["turn-2", 960],
    ]);
    expect(recorded.done).toEqual([{ turnId: "turn-2", samples: 480 }]);
    expect(recorded.failures).toEqual([]);
  });

  it("fails the old turn as superseded when the device starts a turn during a reply", async () => {
    const { handlers, recorded } = recorder();
    const { session, transport } = await open(handlers);
    speak(session, "turn-1", [pcm(1_920)]);
    transport.message({ type: "session.output_audio.delta", delta: base64(pcm(1_920, 1)) });

    session.beginTurn("turn-2");
    transport.message({ type: "session.output_audio.delta", delta: base64(pcm(1_920, 2)) });
    expect(recorded.failures).toEqual([{ turnId: "turn-1", reason: "superseded" }]);
    expect(recorded.audio.map((entry) => entry.turnId)).toEqual(["turn-1"]);
    expect(recorded.done).toEqual([]);
  });

  it("rejects connect on a starting-phase error and never reports the session unavailable", async () => {
    const { handlers, recorded } = recorder();
    const { pending, transport } = startConnect(handlers);
    await untilStartSent(transport);
    transport.message({
      type: "error",
      error: { code: "invalid_voice", message: "voice ballad is not available" },
    });
    await expect(pending).rejects.toThrow(
      "upstream_error:invalid_voice:voice ballad is not available",
    );
    expect(recorded.unavailable).toEqual([]);
    expect(transport.closes).toEqual([{
      code: 1011,
      reason: "upstream_error:invalid_voice:voice ballad is not available",
    }]);
  });

  it("reports a live-phase socket loss once and fails the claimed turn", async () => {
    const { handlers, recorded } = recorder();
    const { session, transport } = await open(handlers);
    speak(session, "turn-1", [pcm(1_920)]);

    transport.closed(1006, "going away");
    transport.closed(1006, "going away");
    expect(recorded.failures).toEqual([{ turnId: "turn-1", reason: "upstream_closed:going away" }]);
    expect(recorded.unavailable).toEqual(["upstream_closed:going away"]);
    expect(() => session.beginTurn("turn-2")).toThrow("session_not_live");
    expect(() => session.cancelTurn("turn-2")).not.toThrow();
    expect(recorded.done).toEqual([]);
  });

  it("closes once and fails a claimed turn with session_closed", async () => {
    const { handlers, recorded } = recorder();
    const { session, transport } = await open(handlers);
    speak(session, "turn-1", [pcm(1_920)]);
    transport.message({ type: "session.output_audio.delta", delta: base64(pcm(1_920, 1)) });

    session.close();
    session.close();
    expect(recorded.failures).toEqual([{ turnId: "turn-1", reason: "session_closed" }]);
    expect(recorded.unavailable).toEqual([]);
    expect(transport.closes).toEqual([{ code: 1000, reason: "session complete" }]);
  });

  it("refuses misuse with the reasons WalleAgent records", async () => {
    const { session } = await open(recorder().handlers);
    expect(() => session.appendPcm("turn-1", pcm(1_920))).toThrow("turn_not_held");
    session.beginTurn("turn-1");
    expect(() => session.beginTurn("turn-2")).toThrow("turn_already_held");
    expect(() => session.appendPcm("turn-1", pcm(3))).toThrow("input_frame_invalid");
    expect(() => session.commitTurn("turn-1")).toThrow("empty_utterance");
  });
});
