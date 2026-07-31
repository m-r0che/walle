import {
  Agent,
  type Connection,
  type ConnectionContext,
  type WSMessage,
} from "agents";

import {
  OpenAIRealtimeSession,
} from "./openai-realtime";
import {
  AudioFrameKind,
  decodeAudioFrame,
  decodeControlMessage,
  encodeAudioFrame,
  MAX_INPUT_SAMPLES,
  type DeviceTelemetryReport,
} from "./protocol";

type AudioProvider = "echo" | "openai";
type TurnProvider = AudioProvider | "failed";
type WalleEnv = Env & { OPENAI_API_KEY?: string };

export type RelayDebugStatus = {
  connections: Array<{
    ready: boolean;
    firmware: string | null;
    provider: AudioProvider;
    sessionEpoch: number;
    activeTurnId: string | null;
    connectedAt: number;
  }>;
  lastTurn: TurnMetrics | null;
  lastPlayback: PlaybackMetrics | null;
  lastGeneration: GenerationMetrics | null;
  pendingResponse: boolean;
  providerError: string | null;
  protocolError: string | null;
  outputPaceMs: number;
  latestTelemetry: (DeviceTelemetryReport & {
    connectionId: string;
    receivedAt: number;
  }) | null;
  lastGenerationFailure: {
    turnId: string;
    reason: string;
    createdAt: number;
  } | null;
};

export type GenerationMetrics = {
  turnId: string;
  provider: "openai";
  inputSamples: number;
  outputSamples: number;
  firstAudioMs: number;
  totalMs: number;
  createdAt: number;
};

export type PlaybackMetrics = {
  turnId: string;
  source: "remote" | "local";
  samples: number;
  createdAt: number;
};

export type TurnMetrics = {
  turnId: string;
  frames: number;
  samples: number;
  bytes: number;
  createdAt: number;
};

type TurnStats = {
  turnId: string;
  frames: number;
  samples: number;
  nextSequence: number | null;
  provider: TurnProvider;
};

type PendingResponse = {
  connectionId: string;
  turnId: string;
  inputSamples: number;
  outputSamples: number;
  nextSequence: number;
  startedAt: number;
  firstAudioAt: number | null;
  outputFrames: Uint8Array[];
  outputHead: number;
  pumpActive: boolean;
  upstreamOutputSamples: number | null;
  generationCompletedAt: number | null;
};

// Generated output is downlink-only; pace below burst rate while allowing the
// audio task to drain its bounded event queue between callbacks.
const OUTPUT_FRAME_PACE_MS = 20;

type DeviceConnectionState = {
  ready: boolean;
  firmware: string | null;
  sessionEpoch: number;
  activeTurnId: string | null;
  lastTurnId: string | null;
  connectedAt: number;
  provider: AudioProvider;
};

function sendJson(connection: Connection, value: unknown): void {
  connection.send(JSON.stringify(value));
}

export class WalleAgent extends Agent<WalleEnv> {
  private readonly turnStats = new Map<string, TurnStats>();
  private readonly suppressedPongs = new Set<string>();
  private dropNextOutputFrame = false;
  private mismatchNextDoneSamples = false;
  private realtime: OpenAIRealtimeSession | null = null;
  private realtimeConnectionId: string | null = null;
  private pendingResponse: PendingResponse | null = null;
  private provider: AudioProvider = "echo";
  private providerError: string | null = null;
  private protocolError: string | null = null;
  private latestTelemetry: (DeviceTelemetryReport & {
    connectionId: string;
    receivedAt: number;
  }) | null = null;
  private lastGenerationFailure: {
    turnId: string;
    reason: string;
    createdAt: number;
  } | null = null;

  static options = {
    sendIdentityOnConnect: false,
    hibernate: false,
  };

  async onStart(): Promise<void> {
    this.sql`
      CREATE TABLE IF NOT EXISTS device_sessions (
        connection_id TEXT PRIMARY KEY,
        firmware TEXT,
        connected_at INTEGER NOT NULL,
        disconnected_at INTEGER
      )
    `;
    this.sql`
      CREATE TABLE IF NOT EXISTS turn_metrics (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        turn_id TEXT NOT NULL,
        frames INTEGER NOT NULL,
        samples INTEGER NOT NULL,
        created_at INTEGER NOT NULL
      )
    `;
    this.sql`
      CREATE TABLE IF NOT EXISTS generation_metrics (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        turn_id TEXT NOT NULL,
        provider TEXT NOT NULL,
        input_samples INTEGER NOT NULL,
        output_samples INTEGER NOT NULL,
        first_audio_ms INTEGER NOT NULL,
        total_ms INTEGER NOT NULL,
        created_at INTEGER NOT NULL
      )
    `;
    this.sql`
      CREATE TABLE IF NOT EXISTS playback_metrics (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        turn_id TEXT NOT NULL,
        source TEXT NOT NULL,
        samples INTEGER NOT NULL,
        created_at INTEGER NOT NULL
      )
    `;
  }

  getLastPlaybackMetrics(): PlaybackMetrics | null {
    const rows = this.sql<{
      turn_id: string;
      source: "remote" | "local";
      samples: number;
      created_at: number;
    }>`
      SELECT turn_id, source, samples, created_at
      FROM playback_metrics
      ORDER BY id DESC
      LIMIT 1
    `;
    const row = rows[0];
    return row === undefined ? null : {
      turnId: row.turn_id,
      source: row.source,
      samples: row.samples,
      createdAt: row.created_at,
    };
  }

  getLastGenerationMetrics(): GenerationMetrics | null {
    const rows = this.sql<{
      turn_id: string;
      provider: "openai";
      input_samples: number;
      output_samples: number;
      first_audio_ms: number;
      total_ms: number;
      created_at: number;
    }>`
      SELECT turn_id, provider, input_samples, output_samples,
             first_audio_ms, total_ms, created_at
      FROM generation_metrics
      ORDER BY id DESC
      LIMIT 1
    `;
    const row = rows[0];
    return row === undefined ? null : {
      turnId: row.turn_id,
      provider: row.provider,
      inputSamples: row.input_samples,
      outputSamples: row.output_samples,
      firstAudioMs: row.first_audio_ms,
      totalMs: row.total_ms,
      createdAt: row.created_at,
    };
  }

  getLastTurnMetrics(): TurnMetrics | null {
    const rows = this.sql<{
      turn_id: string;
      frames: number;
      samples: number;
      created_at: number;
    }>`
      SELECT turn_id, frames, samples, created_at
      FROM turn_metrics
      ORDER BY id DESC
      LIMIT 1
    `;
    const row = rows[0];
    return row === undefined ? null : {
      turnId: row.turn_id,
      frames: row.frames,
      samples: row.samples,
      bytes: row.samples * 2,
      createdAt: row.created_at,
    };
  }

  getDebugStatus(): RelayDebugStatus {
    const connections = Array.from(
      this.getConnections<DeviceConnectionState>(),
      (connection) => ({
        ready: connection.state?.ready ?? false,
        firmware: connection.state?.firmware ?? null,
        provider: connection.state?.provider ?? "echo",
        sessionEpoch: connection.state?.sessionEpoch ?? 0,
        activeTurnId: connection.state?.activeTurnId ?? null,
        connectedAt: connection.state?.connectedAt ?? 0,
      }),
    );
    return {
      connections,
      lastTurn: this.getLastTurnMetrics(),
      lastPlayback: this.getLastPlaybackMetrics(),
      lastGeneration: this.getLastGenerationMetrics(),
      pendingResponse: this.pendingResponse !== null,
      providerError: this.providerError,
      protocolError: this.protocolError,
      outputPaceMs: OUTPUT_FRAME_PACE_MS,
      latestTelemetry: this.latestTelemetry,
      lastGenerationFailure: this.lastGenerationFailure,
    };
  }

  suppressPongsForTest(): number {
    let suppressed = 0;
    for (const connection of this.getConnections()) {
      this.suppressedPongs.add(connection.id);
      suppressed++;
    }
    return suppressed;
  }

  dropNextOutputFrameForTest(): boolean {
    this.dropNextOutputFrame = true;
    return true;
  }

  mismatchNextDoneSamplesForTest(): boolean {
    this.mismatchNextDoneSamples = true;
    return true;
  }

  disconnectDeviceForTest(): number {
    this.closeRealtime("relay reconnect test");
    let disconnected = 0;
    for (const connection of this.getConnections()) {
      connection.close(1012, "Relay reconnect test");
      disconnected++;
    }
    return disconnected;
  }

  shouldSendProtocolMessages(
    _connection: Connection,
    _ctx: ConnectionContext,
  ): boolean {
    return false;
  }

  onConnect(
    connection: Connection<DeviceConnectionState>,
    ctx: ConnectionContext,
  ): void {
    if (ctx.request.headers.get("X-Walle-Authenticated") !== "1") {
      connection.close(4001, "Unauthorized");
      return;
    }

    this.closeRealtime("device connection replaced");
    for (const existing of this.getConnections()) {
      if (existing.id !== connection.id) {
        existing.close(4009, "Replaced by a newer device connection");
      }
    }

    const connectedAt = Date.now();
    connection.setState({
      ready: false,
      firmware: null,
      sessionEpoch: 0,
      activeTurnId: null,
      lastTurnId: null,
      connectedAt,
      provider: "echo",
    });
    this.sql`
      INSERT OR REPLACE INTO device_sessions
        (connection_id, firmware, connected_at, disconnected_at)
      VALUES (${connection.id}, NULL, ${connectedAt}, NULL)
    `;
    this.sql`
      DELETE FROM device_sessions
      WHERE connection_id NOT IN (
        SELECT connection_id FROM device_sessions
        ORDER BY connected_at DESC LIMIT 100
      )
    `;
    console.log(JSON.stringify({
      event: "device.connected",
      installation: this.name,
      connectionId: connection.id,
    }));
  }

  async onMessage(
    connection: Connection<DeviceConnectionState>,
    message: WSMessage,
  ): Promise<void> {
    try {
      if (typeof message === "string") {
        await this.handleControl(connection, message);
        return;
      }
      this.handleAudio(connection, message);
    } catch (error) {
      console.error(JSON.stringify({
        event: "device.protocol_error",
        installation: this.name,
        connectionId: connection.id,
        error: error instanceof Error ? error.message : String(error),
      }));
      this.protocolError = error instanceof Error
        ? error.message.slice(0, 240)
        : "unknown protocol error";
      connection.close(4002, "Protocol error");
    }
  }

  private async handleControl(
    connection: Connection<DeviceConnectionState>,
    text: string,
  ): Promise<void> {
    const message = decodeControlMessage(text);
    const state = connection.state;
    if (state === null || state === undefined) {
      throw new Error("connection state is unavailable");
    }

    switch (message.type) {
      case "hello": {
        const provider = await this.activateProvider(connection.id);
        connection.setState({
          ...state,
          ready: true,
          firmware: message.firmware,
          sessionEpoch: message.sessionEpoch ?? 0,
          provider,
        });
        this.sql`
          UPDATE device_sessions
          SET firmware = ${message.firmware}
          WHERE connection_id = ${connection.id}
        `;
        sendJson(connection, {
          v: 1,
          type: "ready",
          mode: provider,
          playback: provider === "openai" ? "buffered" : "complete",
          sampleRate: 24_000,
          channels: 1,
          sampleFormat: "pcm16le",
          sessionEpoch: message.sessionEpoch ?? 0,
        });
        return;
      }
      case "ping":
        if (!this.suppressedPongs.has(connection.id)) {
          sendJson(connection, { v: 1, type: "pong", nonce: message.nonce });
        }
        return;
      case "turn.start":
        if (!state.ready) throw new Error("hello is required before a turn");
        if (state.activeTurnId !== null) {
          throw new Error("a turn is already active");
        }
        connection.setState({
          ...state,
          activeTurnId: message.turnId,
          lastTurnId: null,
        });
        let turnProvider: TurnProvider = state.provider;
        if (turnProvider === "openai") {
          try {
            if (this.realtime === null) {
              throw new Error("OpenAI session is unavailable");
            }
            this.realtime.beginTurn(message.turnId);
          } catch (error) {
            turnProvider = "failed";
            this.recordGenerationFailure(
              message.turnId,
              error instanceof Error ? error.message : "begin_turn_failed",
            );
          }
        }
        this.turnStats.set(connection.id, {
          turnId: message.turnId,
          frames: 0,
          samples: 0,
          nextSequence: null,
          provider: turnProvider,
        });
        sendJson(connection, {
          v: 1,
          type: "turn.started",
          turnId: message.turnId,
        });
        return;
      case "turn.commit":
        if (state.activeTurnId !== message.turnId) {
          throw new Error("turn commit does not match the active turn");
        }
        connection.setState({
          ...state,
          activeTurnId: null,
          lastTurnId: message.turnId,
        });
        const committed = this.turnStats.get(connection.id);
        this.turnStats.delete(connection.id);
        const committedAt = Date.now();
        if (committed !== undefined) {
          this.sql`
            INSERT INTO turn_metrics (turn_id, frames, samples, created_at)
            VALUES (${message.turnId}, ${committed.frames},
                    ${committed.samples}, ${committedAt})
          `;
          this.sql`
            DELETE FROM turn_metrics
            WHERE id NOT IN (
              SELECT id FROM turn_metrics ORDER BY id DESC LIMIT 100
            )
          `;
        }
        if (committed === undefined || committed.samples === 0) {
          throw new Error("committed turn statistics are unavailable");
        }
        if (committed.provider === "openai") {
          this.pendingResponse = {
            connectionId: connection.id,
            turnId: message.turnId,
            inputSamples: committed.samples,
            outputSamples: 0,
            nextSequence: 0,
            startedAt: Date.now(),
            firstAudioAt: null,
            outputFrames: [],
            outputHead: 0,
            pumpActive: false,
            upstreamOutputSamples: null,
            generationCompletedAt: null,
          };
          try {
            if (this.realtime === null) {
              throw new Error("OpenAI session is unavailable");
            }
            this.realtime.commitTurn(message.turnId);
            console.log(JSON.stringify({
              event: "device.turn_openai_started",
              installation: this.name,
              connectionId: connection.id,
              turnId: message.turnId,
              frames: committed.frames,
              inputSamples: committed.samples,
            }));
          } catch (error) {
            this.pendingResponse = null;
            this.recordGenerationFailure(
              message.turnId,
              error instanceof Error ? error.message : "commit_turn_failed",
            );
            sendJson(connection, {
              v: 1,
              type: "turn.cancelled",
              turnId: message.turnId,
            });
          }
          return;
        }
        if (committed.provider === "failed") {
          sendJson(connection, {
            v: 1,
            type: "turn.cancelled",
            turnId: message.turnId,
          });
          return;
        }
        const reportedSamples = this.mismatchNextDoneSamples
          ? committed.samples + 1
          : committed.samples;
        this.mismatchNextDoneSamples = false;
        sendJson(connection, {
          v: 1,
          type: "turn.done",
          turnId: message.turnId,
          frames: committed.frames,
          inputSamples: committed.samples,
          outputSamples: reportedSamples,
        });
        console.log(JSON.stringify({
          event: "device.turn_echo",
          installation: this.name,
          connectionId: connection.id,
          turnId: message.turnId,
          frames: committed.frames,
          samples: committed.samples,
          bytes: committed.samples * 2,
        }));
        return;
      case "playback.report": {
        if (state.lastTurnId !== message.turnId) {
          throw new Error("playback report does not match the last turn");
        }
        const reportedAt = Date.now();
        this.sql`
          INSERT INTO playback_metrics (turn_id, source, samples, created_at)
          VALUES (${message.turnId}, ${message.source},
                  ${message.samples}, ${reportedAt})
        `;
        this.sql`
          DELETE FROM playback_metrics
          WHERE id NOT IN (
            SELECT id FROM playback_metrics ORDER BY id DESC LIMIT 100
          )
        `;
        console.log(JSON.stringify({
          event: "device.playback_report",
          installation: this.name,
          connectionId: connection.id,
          turnId: message.turnId,
          source: message.source,
          samples: message.samples,
        }));
        return;
      }
      case "telemetry.report":
        if (message.epoch !== state.sessionEpoch) {
          throw new Error("device telemetry epoch is stale");
        }
        this.latestTelemetry = {
          ...message,
          connectionId: connection.id,
          receivedAt: Date.now(),
        };
        return;
      case "response.cancel":
        if (state.lastTurnId !== message.turnId) {
          throw new Error("response cancel does not match the last turn");
        }
        if (this.pendingResponse === null) return;
        if (this.pendingResponse.connectionId !== connection.id
            || this.pendingResponse.turnId !== message.turnId) {
          throw new Error("response cancel does not match the pending turn");
        }
        this.realtime?.cancelTurn(message.turnId);
        this.pendingResponse = null;
        sendJson(connection, {
          v: 1,
          type: "turn.cancelled",
          turnId: message.turnId,
        });
        return;
      case "turn.cancel":
        if (state.activeTurnId !== message.turnId) {
          throw new Error("turn cancel does not match the active turn");
        }
        connection.setState({
          ...state,
          activeTurnId: null,
          lastTurnId: null,
        });
        const cancelled = this.turnStats.get(connection.id);
        this.turnStats.delete(connection.id);
        if (cancelled?.provider === "openai") {
          this.realtime?.cancelTurn(message.turnId);
        }
        sendJson(connection, {
          v: 1,
          type: "turn.cancelled",
          turnId: message.turnId,
        });
        return;
    }
  }

  private handleAudio(
    connection: Connection<DeviceConnectionState>,
    message: ArrayBuffer | ArrayBufferView,
  ): void {
    const state = connection.state;
    if (!state?.ready || state.activeTurnId === null) {
      throw new Error("audio requires an active turn");
    }
    const frame = decodeAudioFrame(message);
    if (frame.kind !== AudioFrameKind.InputPcm16) {
      throw new Error("device sent a non-input audio frame");
    }
    const stats = this.turnStats.get(connection.id);
    if (stats === undefined || stats.turnId !== state.activeTurnId) {
      throw new Error("turn statistics are unavailable");
    }
    if (stats.nextSequence !== null
        && frame.sequence !== stats.nextSequence) {
      throw new Error("audio frame sequence is not contiguous");
    }
    stats.nextSequence = (frame.sequence + 1) >>> 0;
    if (stats.samples + frame.sampleCount > MAX_INPUT_SAMPLES) {
      throw new Error("turn input exceeds the bounded capture duration");
    }
    stats.frames++;
    stats.samples += frame.sampleCount;

    if (stats.provider === "openai") {
      try {
        if (this.realtime === null) {
          throw new Error("OpenAI session is unavailable");
        }
        this.realtime.appendPcm(state.activeTurnId, frame.pcm);
      } catch (error) {
        stats.provider = "failed";
        this.recordGenerationFailure(
          state.activeTurnId,
          error instanceof Error ? error.message : "append_pcm_failed",
        );
        this.realtime?.cancelTurn(state.activeTurnId);
      }
      return;
    }
    if (stats.provider === "failed") return;

    if (this.dropNextOutputFrame) {
      this.dropNextOutputFrame = false;
      console.log(JSON.stringify({
        event: "device.test_output_frame_dropped",
        installation: this.name,
        connectionId: connection.id,
        turnId: state.activeTurnId,
        sequence: frame.sequence,
      }));
      return;
    }
    connection.send(encodeAudioFrame({
      ...frame,
      kind: AudioFrameKind.OutputPcm16,
    }));
  }

  private async activateProvider(
    connectionId: string,
  ): Promise<AudioProvider> {
    this.closeRealtime("provider refresh");
    if (this.env.AUDIO_PROVIDER !== "openai") {
      this.provider = "echo";
      this.providerError = null;
      return this.provider;
    }
    const apiKey = this.env.OPENAI_API_KEY;
    if (typeof apiKey !== "string" || apiKey.length < 16) {
      console.error(JSON.stringify({
        event: "openai.secret_unavailable",
        installation: this.name,
      }));
      this.provider = "echo";
      this.providerError = "OPENAI_API_KEY is unavailable";
      return this.provider;
    }
    this.realtimeConnectionId = connectionId;
    try {
      const realtime = await OpenAIRealtimeSession.connect(
        apiKey,
        this.name,
        {
          onAudio: (turnId, pcm) =>
            this.forwardRealtimeAudio(connectionId, turnId, pcm),
          onDone: (turnId, outputSamples) =>
            this.finishRealtimeTurn(
              connectionId, turnId, outputSamples),
          onFailed: (turnId, reason) =>
            this.failRealtimeTurn(connectionId, turnId, reason),
          onUnavailable: (reason) =>
            this.handleRealtimeUnavailable(connectionId, reason),
        },
      );
      this.realtime = realtime;
      this.realtimeConnectionId = connectionId;
      this.provider = "openai";
      this.providerError = null;
      console.log(JSON.stringify({
        event: "openai.session_ready",
        installation: this.name,
        connectionId,
      }));
    } catch (error) {
      if (this.realtimeConnectionId === connectionId) {
        this.realtimeConnectionId = null;
      }
      this.provider = "echo";
      this.providerError = error instanceof Error
        ? error.message
        : String(error);
      console.error(JSON.stringify({
        event: "openai.session_unavailable",
        installation: this.name,
        connectionId,
        error: this.providerError,
      }));
    }
    return this.provider;
  }

  private forwardRealtimeAudio(
    connectionId: string,
    turnId: string,
    pcm: Uint8Array,
  ): void {
    const pending = this.pendingResponse;
    if (pending === null || pending.connectionId !== connectionId
        || pending.turnId !== turnId || pcm.byteLength % 2 !== 0) {
      this.failRealtimeTurn(connectionId, turnId, "output_scope_mismatch");
      return;
    }
    pending.outputSamples += pcm.byteLength / 2;
    if (pending.firstAudioAt === null) pending.firstAudioAt = Date.now();
    pending.outputFrames.push(pcm);
    if (!pending.pumpActive) {
      pending.pumpActive = true;
      setTimeout(
        () => this.pumpRealtimeOutput(connectionId, turnId), 0);
    }
  }

  private pumpRealtimeOutput(
    connectionId: string,
    turnId: string,
  ): void {
    const pending = this.pendingResponse;
    if (pending === null || pending.connectionId !== connectionId
        || pending.turnId !== turnId) return;
    const pcm = pending.outputFrames[pending.outputHead];
    if (pcm === undefined) {
      pending.pumpActive = false;
      if (pending.upstreamOutputSamples !== null) {
        this.completeRealtimeTurn(connectionId, turnId);
      }
      return;
    }
    pending.outputHead++;
    const sequence = pending.nextSequence++;
    if (this.dropNextOutputFrame) {
      this.dropNextOutputFrame = false;
      console.log(JSON.stringify({
        event: "device.test_output_frame_dropped",
        installation: this.name,
        connectionId,
        turnId,
        sequence,
      }));
    } else {
      const connection = this.getConnection(connectionId);
      if (connection === undefined) {
        this.failRealtimeTurn(connectionId, turnId, "device_disconnected");
        return;
      }
      connection.send(encodeAudioFrame({
        kind: AudioFrameKind.OutputPcm16,
        sequence,
        flags: 0,
        sampleCount: pcm.byteLength / 2,
        pcm,
      }));
    }
    setTimeout(
      () => this.pumpRealtimeOutput(connectionId, turnId),
      OUTPUT_FRAME_PACE_MS,
    );
  }

  private finishRealtimeTurn(
    connectionId: string,
    turnId: string,
    outputSamples: number,
  ): void {
    const pending = this.pendingResponse;
    if (pending === null || pending.connectionId !== connectionId
        || pending.turnId !== turnId
        || pending.outputSamples !== outputSamples) {
      this.failRealtimeTurn(connectionId, turnId, "output_count_mismatch");
      return;
    }
    pending.upstreamOutputSamples = outputSamples;
    pending.generationCompletedAt = Date.now();
    if (!pending.pumpActive
        && pending.outputHead === pending.outputFrames.length) {
      this.completeRealtimeTurn(connectionId, turnId);
    }
  }

  private completeRealtimeTurn(
    connectionId: string,
    turnId: string,
  ): void {
    const pending = this.pendingResponse;
    if (pending === null || pending.connectionId !== connectionId
        || pending.turnId !== turnId
        || pending.upstreamOutputSamples === null
        || pending.outputHead !== pending.outputFrames.length) {
      this.failRealtimeTurn(connectionId, turnId, "output_delivery_mismatch");
      return;
    }
    const connection = this.getConnection(connectionId);
    if (connection === undefined) {
      this.pendingResponse = null;
      return;
    }
    const outputSamples = pending.upstreamOutputSamples;
    const completedAt = pending.generationCompletedAt ?? Date.now();
    const firstAudioMs = (pending.firstAudioAt ?? completedAt)
      - pending.startedAt;
    const totalMs = completedAt - pending.startedAt;
    this.sql`
      INSERT INTO generation_metrics
        (turn_id, provider, input_samples, output_samples,
         first_audio_ms, total_ms, created_at)
      VALUES (${turnId}, 'openai', ${pending.inputSamples},
              ${outputSamples}, ${firstAudioMs}, ${totalMs}, ${completedAt})
    `;
    this.sql`
      DELETE FROM generation_metrics
      WHERE id NOT IN (
        SELECT id FROM generation_metrics ORDER BY id DESC LIMIT 100
      )
    `;
    const reportedOutputSamples = this.mismatchNextDoneSamples
      ? outputSamples + 1
      : outputSamples;
    this.mismatchNextDoneSamples = false;
    sendJson(connection, {
      v: 1,
      type: "turn.done",
      turnId,
      frames: pending.nextSequence,
      inputSamples: pending.inputSamples,
      outputSamples: reportedOutputSamples,
    });
    console.log(JSON.stringify({
      event: "device.turn_openai_done",
      installation: this.name,
      connectionId,
      turnId,
      inputSamples: pending.inputSamples,
      outputSamples,
      frames: pending.nextSequence,
    }));
    this.pendingResponse = null;
  }

  private recordGenerationFailure(
    turnId: string,
    reason: string,
  ): void {
    this.lastGenerationFailure = {
      turnId,
      reason: reason.slice(0, 160),
      createdAt: Date.now(),
    };
  }

  private failRealtimeTurn(
    connectionId: string,
    turnId: string,
    reason: string,
  ): void {
    this.recordGenerationFailure(turnId, reason);
    const active = this.turnStats.get(connectionId);
    if (active?.turnId === turnId) active.provider = "failed";
    const pending = this.pendingResponse;
    if (pending?.connectionId === connectionId
        && pending.turnId === turnId) {
      this.pendingResponse = null;
      const connection = this.getConnection(connectionId);
      if (connection !== undefined) {
        sendJson(connection, {
          v: 1,
          type: "turn.cancelled",
          turnId,
        });
      }
    }
    console.error(JSON.stringify({
      event: "openai.turn_failed",
      installation: this.name,
      connectionId,
      turnId,
      reason,
    }));
  }

  private handleRealtimeUnavailable(
    connectionId: string,
    reason: string,
  ): void {
    if (this.realtimeConnectionId !== connectionId) return;
    this.realtime = null;
    this.realtimeConnectionId = null;
    this.pendingResponse = null;
    this.provider = "echo";
    this.providerError = reason;
    const connection = this.getConnection(connectionId);
    connection?.close(1012, "OpenAI session refresh");
    console.warn(JSON.stringify({
      event: "openai.session_lost",
      installation: this.name,
      connectionId,
      reason,
    }));
  }

  private closeRealtime(reason: string): void {
    const realtime = this.realtime;
    this.realtime = null;
    this.realtimeConnectionId = null;
    this.pendingResponse = null;
    this.provider = "echo";
    realtime?.close(1000, reason);
  }

  onClose(
    connection: Connection<DeviceConnectionState>,
    code: number,
    reason: string,
    wasClean: boolean,
  ): void {
    this.turnStats.delete(connection.id);
    this.suppressedPongs.delete(connection.id);
    if (this.realtimeConnectionId === connection.id) {
      this.closeRealtime("device disconnected");
    }
    this.sql`
      UPDATE device_sessions
      SET disconnected_at = ${Date.now()}
      WHERE connection_id = ${connection.id}
    `;
    console.log(JSON.stringify({
      event: "device.disconnected",
      installation: this.name,
      connectionId: connection.id,
      code,
      reason,
      wasClean,
    }));
  }
}
