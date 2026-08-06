import { describe, expect, it } from "vitest";

import {
  OpenAIRealtimeSession,
  type OpenAIRealtimeHandlers,
  type RealtimeTransport,
  type RealtimeTransportConnector,
} from "../src/openai-realtime";

class FakeTransport implements RealtimeTransport {
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

function decodeSent(transport: FakeTransport): Array<Record<string, unknown>> {
  return transport.sent.map((message) => JSON.parse(message));
}

async function connect(
  handlers: OpenAIRealtimeHandlers,
): Promise<{ session: OpenAIRealtimeSession; transport: FakeTransport }> {
  const transport = new FakeTransport();
  const connector: RealtimeTransportConnector = async (apiKey, safetyId) => {
    expect(apiKey).toBe("test-openai-key-with-safe-length");
    expect(safetyId).toMatch(/^[0-9a-f]{64}$/);
    return transport;
  };
  const pending = OpenAIRealtimeSession.connect(
    "test-openai-key-with-safe-length",
    "installation-1",
    handlers,
    connector,
  );
  for (let attempt = 0; transport.sent.length === 0 && attempt < 20;
    attempt++) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  const update = decodeSent(transport)[0];
  expect(update.type).toBe("session.update");
  transport.message({ type: "session.updated", session: { id: "sess-1" } });
  return { session: await pending, transport };
}

function base64(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

describe("OpenAI Realtime session", () => {
  it("configures 24 kHz PCM push-to-talk audio", async () => {
    const { transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: () => undefined,
    });
    const update = decodeSent(transport)[0];
    const session = update.session as Record<string, unknown>;
    const audio = session.audio as Record<string, Record<string, unknown>>;
    expect(session.model).toBe("gpt-realtime-2.1");
    expect(session.output_modalities).toEqual(["audio"]);
    expect(session.instructions).toContain("Your name is Walle");
    expect(session.instructions).toContain("roughly four to twelve words");
    expect(session.instructions).toContain("explicitly requests one");
    expect(session.instructions).toContain("confirmed result");
    expect(session.instructions).toContain("light, modern British accent");
    expect(session.instructions).toContain("slightly synthetic");
    expect(session.instructions).toContain("rather than a generic virtual assistant");
    expect(audio.input.turn_detection).toBeNull();
    expect(audio.input.format).toEqual({ type: "audio/pcm", rate: 24_000 });
    expect(audio.output.format).toEqual({
      type: "audio/pcm",
      rate: 24_000,
    });
    expect(audio.output.voice).toBe("ballad");
  });

  it("sends manual input and returns bounded PCM chunks", async () => {
    const audio: Uint8Array[] = [];
    const done: Array<{ turnId: string; samples: number }> = [];
    const transcripts: Array<{ turnId: string; text: string }> = [];
    const failures: string[] = [];
    const { session, transport } = await connect({
      onAudio: (_turnId, pcm) => audio.push(pcm),
      onTranscript: (turnId, text) => transcripts.push({ turnId, text }),
      onDone: (turnId, samples) => done.push({ turnId, samples }),
      onFailed: (_turnId, reason) => failures.push(reason),
    });

    session.beginTurn("turn-1");
    session.appendPcm("turn-1", new Uint8Array([1, 2, 3, 4]));
    session.commitTurn("turn-1");
    const sent = decodeSent(transport).slice(1);
    expect(sent.map((event) => event.type)).toEqual([
      "input_audio_buffer.clear",
      "input_audio_buffer.append",
      "input_audio_buffer.commit",
      "response.create",
    ]);

    transport.message({
      type: "response.created",
      response: { id: "resp-1", metadata: { turnId: "turn-1" } },
    });
    const generated = new Uint8Array(2_400);
    for (let index = 0; index < generated.length; index++) {
      generated[index] = index & 0xff;
    }
    transport.message({
      type: "response.output_audio.delta",
      response_id: "resp-1",
      delta: base64(generated),
    });
    transport.message({
      type: "response.output_audio_transcript.done",
      response_id: "resp-1",
      transcript: "That’s lovely.",
    });
    transport.message({
      type: "response.done",
      response: { id: "resp-1", status: "completed" },
    });

    expect(audio.map((chunk) => chunk.byteLength)).toEqual([1_920, 480]);
    expect(new Uint8Array([...audio[0], ...audio[1]])).toEqual(generated);
    expect(done).toEqual([{ turnId: "turn-1", samples: 1_200 }]);
    expect(transcripts).toEqual([
      { turnId: "turn-1", text: "That’s lovely." },
    ]);
    expect(failures).toEqual([]);
  });

  it("fails closed on mismatched or incomplete responses", async () => {
    const failures: string[] = [];
    const { session, transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: (_turnId, reason) => failures.push(reason),
    });
    session.beginTurn("turn-2");
    session.commitTurn("turn-2");
    transport.message({
      type: "response.created",
      response: { id: "wrong", metadata: { turnId: "other" } },
    });
    expect(failures).toEqual(["response_identity_mismatch"]);

    session.beginTurn("turn-3");
    session.commitTurn("turn-3");
    transport.message({
      type: "response.created",
      response: { id: "resp-3", metadata: { turnId: "turn-3" } },
    });
    transport.message({
      type: "response.done",
      response: { id: "resp-3", status: "failed" },
    });
    expect(failures).toEqual([
      "response_identity_mismatch",
      "response_incomplete",
    ]);
  });

  it("cancels active generation without producing output", async () => {
    const failures: string[] = [];
    const { session, transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: (_turnId, reason) => failures.push(reason),
    });
    session.beginTurn("turn-4");
    session.commitTurn("turn-4");
    transport.message({
      type: "response.created",
      response: { id: "resp-4", metadata: { turnId: "turn-4" } },
    });
    session.cancelTurn("turn-4");
    expect(decodeSent(transport).slice(-2).map((event) => event.type)).toEqual([
      "response.cancel",
      "input_audio_buffer.clear",
    ]);
    expect(failures).toEqual([]);
  });

  it("advertises the walle tools in the session configuration", async () => {
    const { transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: () => undefined,
    });
    const update = decodeSent(transport)[0];
    const session = update.session as Record<string, unknown>;
    const tools = session.tools as Array<Record<string, unknown>>;
    expect(session.tool_choice).toBe("auto");
    expect(tools.map((tool) => tool.name)).toContain("shopping_list_add");
    expect(tools.map((tool) => tool.name)).toContain("print_commit");
  });

  it("pauses for tool calls, then finishes after outputs are submitted",
    async () => {
      const audio: Uint8Array[] = [];
      const done: Array<{ turnId: string; samples: number }> = [];
      const failures: string[] = [];
      const toolCalls: Array<{ turnId: string; name: string }> = [];
      const { session, transport } = await connect({
        onAudio: (_turnId, pcm) => audio.push(pcm),
        onDone: (turnId, samples) => done.push({ turnId, samples }),
        onFailed: (_turnId, reason) => failures.push(reason),
        onToolCalls: (turnId, calls) => {
          for (const call of calls) {
            toolCalls.push({ turnId, name: call.name });
          }
        },
      });

      session.beginTurn("turn-5");
      session.commitTurn("turn-5");
      transport.message({
        type: "response.created",
        response: { id: "resp-5", metadata: { turnId: "turn-5" } },
      });
      transport.message({
        type: "response.done",
        response: {
          id: "resp-5",
          status: "completed",
          output: [{
            type: "function_call",
            call_id: "call-1",
            name: "shopping_list_add",
            arguments: "{\"items\":[{\"name\":\"Milk\"}]}",
          }],
        },
      });
      expect(toolCalls).toEqual([
        { turnId: "turn-5", name: "shopping_list_add" },
      ]);
      expect(done).toEqual([]);
      expect(failures).toEqual([]);

      session.submitToolOutputs("turn-5", [
        { callId: "call-1", output: "{\"ok\":true}" },
      ]);
      const sent = decodeSent(transport);
      const itemCreate = sent.find((event) =>
        event.type === "conversation.item.create");
      expect((itemCreate?.item as Record<string, unknown>).call_id)
        .toBe("call-1");
      expect(sent.filter((event) =>
        event.type === "response.create").length).toBe(2);

      transport.message({
        type: "response.created",
        response: { id: "resp-6", metadata: { turnId: "turn-5" } },
      });
      transport.message({
        type: "response.output_audio.delta",
        response_id: "resp-6",
        delta: base64(new Uint8Array(480)),
      });
      transport.message({
        type: "response.done",
        response: { id: "resp-6", status: "completed" },
      });
      expect(done).toEqual([{ turnId: "turn-5", samples: 240 }]);
      expect(failures).toEqual([]);
      expect(audio.length).toBe(1);
    });

  it("rejects tool outputs when no tool call is pending", async () => {
    const { session, transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: () => undefined,
      onToolCalls: () => undefined,
    });
    session.beginTurn("turn-6");
    session.commitTurn("turn-6");
    transport.message({
      type: "response.created",
      response: { id: "resp-7", metadata: { turnId: "turn-6" } },
    });
    expect(() => session.submitToolOutputs("turn-6", [
      { callId: "call-x", output: "{}" },
    ])).toThrow(/not awaiting/);
  });

  it("fails the turn when tool rounds exceed the bound", async () => {
    const failures: string[] = [];
    const { session, transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: (_turnId, reason) => failures.push(reason),
      onToolCalls: () => undefined,
    });
    session.beginTurn("turn-7");
    session.commitTurn("turn-7");
    for (let round = 0; round < 4 && failures.length === 0; round++) {
      transport.message({
        type: "response.created",
        response: {
          id: `resp-r${round}`,
          metadata: { turnId: "turn-7" },
        },
      });
      transport.message({
        type: "response.done",
        response: {
          id: `resp-r${round}`,
          status: "completed",
          output: [{
            type: "function_call",
            call_id: `call-r${round}`,
            name: "shopping_list_read",
            arguments: "",
          }],
        },
      });
      if (failures.length === 0) {
        session.submitToolOutputs("turn-7", [
          { callId: `call-r${round}`, output: "{}" },
        ]);
      }
    }
    expect(failures).toEqual(["tool_round_limit"]);
  });

  it("fails closed on out-of-bounds tool call items", async () => {
    const failures: string[] = [];
    const { session, transport } = await connect({
      onAudio: () => undefined,
      onDone: () => undefined,
      onFailed: (_turnId, reason) => failures.push(reason),
      onToolCalls: () => undefined,
    });
    session.beginTurn("turn-8");
    session.commitTurn("turn-8");
    transport.message({
      type: "response.created",
      response: { id: "resp-8", metadata: { turnId: "turn-8" } },
    });
    transport.message({
      type: "response.done",
      response: {
        id: "resp-8",
        status: "completed",
        output: [{ type: "function_call", call_id: "", name: "x",
          arguments: "" }],
      },
    });
    expect(failures).toEqual(["tool_call_out_of_bounds"]);
  });
});
