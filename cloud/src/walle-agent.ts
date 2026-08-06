import {
  Agent,
  type Connection,
  type ConnectionContext,
  type WSMessage,
} from "agents";

import {
  classifyFaceAffect,
  FACE_AFFECT_VERSION,
  type FaceAffectIntent,
} from "./affect";
import {
  OpenAIRealtimeSession,
} from "./openai-realtime";
import {
  WALLE_PERSONALITY_VERSION,
  WALLE_VOICE_PROFILE_VERSION,
} from "./personality";
import {
  AudioFrameKind,
  decodeAudioFrame,
  decodeControlMessage,
  encodeAudioFrame,
  MAX_INPUT_SAMPLES,
  type DeviceTelemetryReport,
} from "./protocol";
import {
  MAX_ACTIVE_ITEMS,
  renderShoppingListDocument,
  SHOPPING_LIST_VERSION,
  type ShoppingItem,
  type ShoppingItemInput,
} from "./shopping";
import {
  ALLOWED_PRINTER,
  base64ToBytes,
  bytesToBase64,
  CLAIM_LEASE_SECONDS,
  computePrintDigest,
  isTerminalPrintJobState,
  MAX_PDF_BASE64_CHARS,
  nextPrintJobState,
  PENDING_TTL_SECONDS,
  PREPARE_TTL_SECONDS,
  PRINT_PIPELINE_VERSION,
  SUBMIT_STALE_SECONDS,
  svgPrintHtml,
  textPrintHtml,
  validateNoteText,
  validateSvgSource,
  type PrintContentKind,
  type PrintJobState,
} from "./printing";
import {
  encodeToolResult,
  parseToolInvocation,
  WALLE_TOOLS_VERSION,
  type RealtimeToolCall,
  type WalleToolInvocation,
} from "./tools";

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
  lastAffect: (FaceAffectIntent & {
    turnId: string;
    createdAt: number;
  }) | null;
  pendingResponse: boolean;
  providerError: string | null;
  protocolError: string | null;
  outputPaceMs: number;
  steadyOutputPaceMs: number;
  playbackMode: "buffered";
  personalityVersion: string;
  voiceProfileVersion: string;
  affectVersion: string;
  latestTelemetry: (DeviceTelemetryReport & {
    connectionId: string;
    receivedAt: number;
  }) | null;
  lastGenerationFailure: {
    turnId: string;
    reason: string;
    createdAt: number;
  } | null;
  toolsVersion: string;
  bridgeConnected: boolean;
  printJobCounts: Record<string, number>;
  lastToolCall: {
    tool: string;
    ok: boolean;
    turnId: string;
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
  firstCodecWriteMs: number;
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
  affectSent: boolean;
};

// Fill the initial jitter buffer quickly, then match 960 samples / 24 kHz.
const OUTPUT_STARTUP_PACE_MS = 20;
const OUTPUT_STEADY_PACE_MS = 40;
const OUTPUT_STARTUP_FRAMES = 25;
const OUTPUT_QUEUE_COMPACT_FRAMES = 64;

type ConnectionRole = "device" | "bridge";

type DeviceConnectionState = {
  role: ConnectionRole;
  ready: boolean;
  firmware: string | null;
  sessionEpoch: number;
  activeTurnId: string | null;
  lastTurnId: string | null;
  connectedAt: number;
  provider: AudioProvider;
};

type PrintJobRow = {
  job_id: string;
  state: PrintJobState;
  kind: PrintContentKind;
  title: string;
  content: string;
  digest: string;
  payload_type: string | null;
  payload_base64: string | null;
  printer: string;
  pages: number;
  claim_id: string | null;
  local_job_id: string | null;
  failure: string | null;
  created_at: number;
  prepared_expires_at: number;
};

type PrintJobLease = { jobId: string; claimId: string };

const MAX_BRIDGE_MESSAGE_CHARS = 4_096;

type CodeSandboxExecutor = {
  execute(
    code: string,
    fns: Record<string, (...args: unknown[]) => Promise<unknown>>,
  ): Promise<{ result: unknown; error?: string; logs?: string[] }>;
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
  private lastAffect: (FaceAffectIntent & {
    turnId: string;
    createdAt: number;
  }) | null = null;
  private lastGenerationFailure: {
    turnId: string;
    reason: string;
    createdAt: number;
  } | null = null;
  private lastToolCall: {
    tool: string;
    ok: boolean;
    turnId: string;
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
        first_codec_write_ms INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      )
    `;
    const playbackColumns = this.sql<{ name: string }>`
      PRAGMA table_info(playback_metrics)
    `;
    if (!playbackColumns.some((column) =>
      column.name === "first_codec_write_ms")) {
      this.sql`
        ALTER TABLE playback_metrics
        ADD COLUMN first_codec_write_ms INTEGER NOT NULL DEFAULT 0
      `;
    }
    this.sql`
      CREATE TABLE IF NOT EXISTS shopping_items (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        quantity TEXT,
        added_at INTEGER NOT NULL,
        removed_at INTEGER,
        removed_reason TEXT
      )
    `;
    this.sql`
      CREATE TABLE IF NOT EXISTS shopping_ops (
        op_id TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        item_ids TEXT NOT NULL,
        result TEXT NOT NULL,
        undone INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL
      )
    `;
    this.sql`
      CREATE TABLE IF NOT EXISTS print_jobs (
        job_id TEXT PRIMARY KEY,
        state TEXT NOT NULL,
        kind TEXT NOT NULL,
        title TEXT NOT NULL,
        content TEXT NOT NULL,
        digest TEXT NOT NULL,
        payload_type TEXT,
        payload_base64 TEXT,
        printer TEXT NOT NULL,
        pages INTEGER NOT NULL,
        claim_id TEXT,
        local_job_id TEXT,
        failure TEXT,
        created_at INTEGER NOT NULL,
        prepared_expires_at INTEGER NOT NULL,
        committed_at INTEGER,
        claimed_at INTEGER,
        submitted_at INTEGER,
        finished_at INTEGER
      )
    `;
  }

  getLastPlaybackMetrics(): PlaybackMetrics | null {
    const rows = this.sql<{
      turn_id: string;
      source: "remote" | "local";
      samples: number;
      first_codec_write_ms: number;
      created_at: number;
    }>`
      SELECT turn_id, source, samples, first_codec_write_ms, created_at
      FROM playback_metrics
      ORDER BY id DESC
      LIMIT 1
    `;
    const row = rows[0];
    return row === undefined ? null : {
      turnId: row.turn_id,
      source: row.source,
      samples: row.samples,
      firstCodecWriteMs: row.first_codec_write_ms,
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
    ).filter((connection) => connection.state?.role !== "bridge")
      .map((connection) => ({
        ready: connection.state?.ready ?? false,
        firmware: connection.state?.firmware ?? null,
        provider: connection.state?.provider ?? "echo",
        sessionEpoch: connection.state?.sessionEpoch ?? 0,
        activeTurnId: connection.state?.activeTurnId ?? null,
        connectedAt: connection.state?.connectedAt ?? 0,
      }));
    return {
      connections,
      lastTurn: this.getLastTurnMetrics(),
      lastPlayback: this.getLastPlaybackMetrics(),
      lastGeneration: this.getLastGenerationMetrics(),
      lastAffect: this.lastAffect,
      pendingResponse: this.pendingResponse !== null,
      providerError: this.providerError,
      protocolError: this.protocolError,
      outputPaceMs: OUTPUT_STARTUP_PACE_MS,
      steadyOutputPaceMs: OUTPUT_STEADY_PACE_MS,
      playbackMode: "buffered",
      personalityVersion: WALLE_PERSONALITY_VERSION,
      voiceProfileVersion: WALLE_VOICE_PROFILE_VERSION,
      affectVersion: FACE_AFFECT_VERSION,
      latestTelemetry: this.latestTelemetry,
      lastGenerationFailure: this.lastGenerationFailure,
      toolsVersion: WALLE_TOOLS_VERSION,
      bridgeConnected: this.hasBridgeConnection(),
      printJobCounts: this.printJobCounts(),
      lastToolCall: this.lastToolCall,
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
    if (ctx.request.headers.get("X-Walle-Role") === "bridge") {
      this.acceptBridgeConnection(connection);
      return;
    }

    this.closeRealtime("device connection replaced");
    for (const existing of
      this.getConnections<DeviceConnectionState>()) {
      if (existing.id !== connection.id
          && existing.state?.role !== "bridge") {
        existing.close(4009, "Replaced by a newer device connection");
      }
    }

    const connectedAt = Date.now();
    connection.setState({
      role: "device",
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

  private acceptBridgeConnection(
    connection: Connection<DeviceConnectionState>,
  ): void {
    for (const existing of
      this.getConnections<DeviceConnectionState>()) {
      if (existing.id !== connection.id
          && existing.state?.role === "bridge") {
        existing.close(4009, "Replaced by a newer bridge connection");
      }
    }
    connection.setState({
      role: "bridge",
      ready: true,
      firmware: null,
      sessionEpoch: 0,
      activeTurnId: null,
      lastTurnId: null,
      connectedAt: Date.now(),
      provider: "echo",
    });
    console.log(JSON.stringify({
      event: "bridge.connected",
      installation: this.name,
      connectionId: connection.id,
    }));
  }

  private handleBridgeMessage(
    connection: Connection<DeviceConnectionState>,
    message: WSMessage,
  ): void {
    if (typeof message !== "string"
        || message.length > MAX_BRIDGE_MESSAGE_CHARS) {
      throw new Error("bridge messages must be bounded JSON text");
    }
    const value = JSON.parse(message) as Record<string, unknown>;
    switch (value.type) {
      case "hello":
        sendJson(connection, {
          v: 1,
          type: "bridge.ready",
          printer: ALLOWED_PRINTER,
          pending: this.countPrintJobs("pending"),
        });
        return;
      case "ping":
        if (typeof value.nonce !== "string" || value.nonce.length > 64) {
          throw new Error("bridge ping nonce is invalid");
        }
        sendJson(connection, { v: 1, type: "pong", nonce: value.nonce });
        return;
      default:
        throw new Error("unknown bridge message type");
    }
  }

  async onMessage(
    connection: Connection<DeviceConnectionState>,
    message: WSMessage,
  ): Promise<void> {
    try {
      if (connection.state?.role === "bridge") {
        this.handleBridgeMessage(connection, message);
        return;
      }
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
            affectSent: false,
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
        const firstCodecWriteMs = message.firstCodecWriteMs ?? 0;
        this.sql`
          INSERT INTO playback_metrics (
            turn_id, source, samples, first_codec_write_ms, created_at
          )
          VALUES (${message.turnId}, ${message.source}, ${message.samples},
                  ${firstCodecWriteMs}, ${reportedAt})
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
          firstCodecWriteMs,
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
          onTranscript: (turnId, transcript) =>
            this.forwardRealtimeAffect(
              connectionId, turnId, transcript),
          onDone: (turnId, outputSamples) =>
            this.finishRealtimeTurn(
              connectionId, turnId, outputSamples),
          onFailed: (turnId, reason) =>
            this.failRealtimeTurn(connectionId, turnId, reason),
          onUnavailable: (reason) =>
            this.handleRealtimeUnavailable(connectionId, reason),
          onToolCalls: (turnId, calls) => {
            void this.runRealtimeToolCalls(connectionId, turnId, calls);
          },
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

  private forwardRealtimeAffect(
    connectionId: string,
    turnId: string,
    transcript: string,
  ): void {
    const pending = this.pendingResponse;
    if (pending === null || pending.connectionId !== connectionId
        || pending.turnId !== turnId || pending.affectSent) return;
    const connection = this.getConnection(connectionId);
    if (connection === undefined) return;

    const intent = classifyFaceAffect(transcript);
    sendJson(connection, {
      v: 1,
      type: "face.affect",
      turnId,
      ...intent,
    });
    pending.affectSent = true;
    this.lastAffect = { turnId, ...intent, createdAt: Date.now() };
    console.log(JSON.stringify({
      event: "device.face_affect",
      installation: this.name,
      connectionId,
      turnId,
      ...intent,
    }));
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
    if (pending.outputHead >= OUTPUT_QUEUE_COMPACT_FRAMES
        && pending.outputHead * 2 >= pending.outputFrames.length) {
      pending.outputFrames.splice(0, pending.outputHead);
      pending.outputHead = 0;
    }
    const paceMs = pending.nextSequence < OUTPUT_STARTUP_FRAMES
      ? OUTPUT_STARTUP_PACE_MS
      : OUTPUT_STEADY_PACE_MS;
    setTimeout(
      () => this.pumpRealtimeOutput(connectionId, turnId),
      paceMs,
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
    if (!pending.affectSent) {
      this.forwardRealtimeAffect(connectionId, turnId, "");
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

  // ---- Realtime tool execution -------------------------------------------

  private async runRealtimeToolCalls(
    connectionId: string,
    turnId: string,
    calls: RealtimeToolCall[],
  ): Promise<void> {
    const outputs: Array<{ callId: string; output: string }> = [];
    for (const call of calls) {
      outputs.push({
        callId: call.callId,
        output: await this.executeToolCall(turnId, call),
      });
    }
    const realtime = this.realtime;
    if (realtime === null || this.realtimeConnectionId !== connectionId) {
      return;
    }
    try {
      realtime.submitToolOutputs(turnId, outputs);
    } catch (error) {
      this.failRealtimeTurn(
        connectionId,
        turnId,
        error instanceof Error ? error.message : "tool_submit_failed",
      );
    }
  }

  private async executeToolCall(
    turnId: string,
    call: RealtimeToolCall,
  ): Promise<string> {
    let result: Record<string, unknown>;
    try {
      const invocation = parseToolInvocation(call);
      result = await this.runTool(invocation, call.callId);
    } catch (error) {
      result = {
        ok: false,
        error: (error instanceof Error
          ? error.message
          : "tool execution failed").slice(0, 240),
      };
    }
    const ok = result.ok !== false;
    this.lastToolCall = {
      tool: call.name.slice(0, 64),
      ok,
      turnId,
      createdAt: Date.now(),
    };
    // Log the tool name and outcome only: arguments and results are the
    // owner's content and stay out of routine logs.
    console.log(JSON.stringify({
      event: "device.tool_call",
      installation: this.name,
      turnId,
      tool: call.name.slice(0, 64),
      ok,
    }));
    return encodeToolResult(result);
  }

  private async runTool(
    invocation: WalleToolInvocation,
    callId: string,
  ): Promise<Record<string, unknown>> {
    switch (invocation.name) {
      case "shopping_list_read": {
        const items = this.activeShoppingItems();
        return {
          ok: true,
          count: items.length,
          items: items.map((item) => ({
            id: item.id,
            name: item.name,
            quantity: item.quantity,
          })),
        };
      }
      case "shopping_list_add":
        return this.addShoppingItems(invocation.items, callId);
      case "shopping_list_undo":
        return this.undoShoppingAdd(callId);
      case "print_prepare":
        return this.preparePrintJob(
          invocation.kind,
          invocation.title,
          invocation.kind === "shopping_list"
            ? renderShoppingListDocument(
              this.activeShoppingItems(), new Date())
            : invocation.kind === "note"
              ? invocation.text ?? ""
              : invocation.svg ?? "",
        );
      case "compose_document":
        return this.composeDocument(invocation.title, invocation.code);
      case "print_commit":
        return this.commitPrintJob(invocation.jobId, invocation.digest);
      case "print_status": {
        const job = invocation.jobId === null
          ? this.latestPrintJob()
          : this.getPrintJob(invocation.jobId);
        if (job === null) return { ok: false, error: "no print jobs found" };
        return {
          ok: true,
          jobId: job.job_id,
          state: job.state,
          title: job.title,
          failure: job.failure,
          localJobId: job.local_job_id,
        };
      }
    }
  }

  // ---- Shopping list ------------------------------------------------------

  private activeShoppingItems(): ShoppingItem[] {
    const rows = this.sql<{
      id: number;
      name: string;
      quantity: string | null;
      added_at: number;
    }>`
      SELECT id, name, quantity, added_at FROM shopping_items
      WHERE removed_at IS NULL
      ORDER BY id ASC
      LIMIT ${MAX_ACTIVE_ITEMS}
    `;
    return rows.map((row) => ({
      id: row.id,
      name: row.name,
      quantity: row.quantity,
      addedAt: row.added_at,
    }));
  }

  private storedOpResult(opId: string): Record<string, unknown> | null {
    const rows = this.sql<{ result: string }>`
      SELECT result FROM shopping_ops WHERE op_id = ${opId}
    `;
    const row = rows[0];
    if (row === undefined) return null;
    return JSON.parse(row.result) as Record<string, unknown>;
  }

  private addShoppingItems(
    items: ShoppingItemInput[],
    opId: string,
  ): Record<string, unknown> {
    const replayed = this.storedOpResult(opId);
    if (replayed !== null) return replayed;
    const active = this.activeShoppingItems();
    if (active.length + items.length > MAX_ACTIVE_ITEMS) {
      return {
        ok: false,
        error: `the list is limited to ${MAX_ACTIVE_ITEMS} items`,
      };
    }
    const now = Date.now();
    const added: Array<{ id: number; name: string; quantity: string | null }>
      = [];
    for (const item of items) {
      this.sql`
        INSERT INTO shopping_items (name, quantity, added_at)
        VALUES (${item.name}, ${item.quantity}, ${now})
      `;
      const idRows = this.sql<{ id: number }>`
        SELECT last_insert_rowid() AS id
      `;
      added.push({
        id: idRows[0]?.id ?? 0,
        name: item.name,
        quantity: item.quantity,
      });
    }
    const result = {
      ok: true,
      added,
      count: active.length + added.length,
    };
    this.sql`
      INSERT INTO shopping_ops (op_id, kind, item_ids, result, created_at)
      VALUES (${opId}, 'add', ${JSON.stringify(added.map((item) => item.id))},
              ${JSON.stringify(result)}, ${now})
    `;
    return result;
  }

  private undoShoppingAdd(opId: string): Record<string, unknown> {
    const replayed = this.storedOpResult(opId);
    if (replayed !== null) return replayed;
    const ops = this.sql<{ op_id: string; item_ids: string }>`
      SELECT op_id, item_ids FROM shopping_ops
      WHERE kind = 'add' AND undone = 0
      ORDER BY created_at DESC, op_id DESC
      LIMIT 1
    `;
    const op = ops[0];
    if (op === undefined) {
      return { ok: false, error: "there is no recent addition to undo" };
    }
    const itemIds = (JSON.parse(op.item_ids) as number[])
      .filter((id) => Number.isInteger(id));
    const now = Date.now();
    const removed: string[] = [];
    for (const id of itemIds) {
      const rows = this.sql<{ name: string }>`
        SELECT name FROM shopping_items
        WHERE id = ${id} AND removed_at IS NULL
      `;
      const row = rows[0];
      if (row === undefined) continue;
      this.sql`
        UPDATE shopping_items
        SET removed_at = ${now}, removed_reason = 'undo'
        WHERE id = ${id}
      `;
      removed.push(row.name);
    }
    this.sql`
      UPDATE shopping_ops SET undone = 1 WHERE op_id = ${op.op_id}
    `;
    const result = {
      ok: true,
      removed,
      count: this.activeShoppingItems().length,
    };
    this.sql`
      INSERT INTO shopping_ops (op_id, kind, item_ids, result, created_at)
      VALUES (${opId}, 'undo', ${op.item_ids},
              ${JSON.stringify(result)}, ${now})
    `;
    return result;
  }

  // ---- Print pipeline -----------------------------------------------------

  private getPrintJob(jobId: string): PrintJobRow | null {
    const rows = this.sql<PrintJobRow>`
      SELECT job_id, state, kind, title, content, digest, payload_type,
             payload_base64, printer, pages, claim_id, local_job_id,
             failure, created_at, prepared_expires_at
      FROM print_jobs WHERE job_id = ${jobId}
    `;
    return rows[0] ?? null;
  }

  private latestPrintJob(): PrintJobRow | null {
    const rows = this.sql<PrintJobRow>`
      SELECT job_id, state, kind, title, content, digest, payload_type,
             payload_base64, printer, pages, claim_id, local_job_id,
             failure, created_at, prepared_expires_at
      FROM print_jobs ORDER BY created_at DESC, job_id DESC LIMIT 1
    `;
    return rows[0] ?? null;
  }

  private countPrintJobs(state: PrintJobState): number {
    const rows = this.sql<{ total: number }>`
      SELECT COUNT(*) AS total FROM print_jobs WHERE state = ${state}
    `;
    return rows[0]?.total ?? 0;
  }

  private printJobCounts(): Record<string, number> {
    const rows = this.sql<{ state: string; total: number }>`
      SELECT state, COUNT(*) AS total FROM print_jobs GROUP BY state
    `;
    return Object.fromEntries(rows.map((row) => [row.state, row.total]));
  }

  private setPrintJobState(
    jobId: string,
    state: PrintJobState,
    failure: string | null = null,
  ): void {
    const finished = isTerminalPrintJobState(state) ? Date.now() : null;
    this.sql`
      UPDATE print_jobs
      SET state = ${state},
          failure = COALESCE(${failure}, failure),
          finished_at = COALESCE(${finished}, finished_at)
      WHERE job_id = ${jobId}
    `;
    console.log(JSON.stringify({
      event: "print.job_state",
      installation: this.name,
      jobId,
      state,
      failure,
    }));
  }

  private async preparePrintJob(
    kind: PrintContentKind,
    title: string,
    content: string,
  ): Promise<Record<string, unknown>> {
    if (content.length === 0) {
      return { ok: false, error: "there is no content to print" };
    }
    const jobId = crypto.randomUUID();
    const digest = await computePrintDigest(kind, title, content);
    const now = Date.now();
    const expiresAt = now + PREPARE_TTL_SECONDS * 1_000;
    this.sql`
      INSERT INTO print_jobs
        (job_id, state, kind, title, content, digest, printer, pages,
         created_at, prepared_expires_at)
      VALUES (${jobId}, 'prepared', ${kind}, ${title}, ${content},
              ${digest}, ${ALLOWED_PRINTER}, 1, ${now}, ${expiresAt})
    `;
    await this.schedule(
      PREPARE_TTL_SECONDS, "printPrepareExpired", jobId);
    const summary = kind === "shopping_list"
      ? `the shopping list (${this.activeShoppingItems().length} items)`
      : kind === "note"
        ? `a note (${content.split("\n").length} lines)`
        : `a drawing titled "${title}"`;
    return {
      ok: true,
      jobId,
      digest,
      title,
      kind,
      pages: 1,
      printer: ALLOWED_PRINTER,
      summary,
      nextStep: "Describe the summary and ask the user to confirm before "
        + "calling print_commit.",
    };
  }

  private async composeDocument(
    title: string,
    code: string,
  ): Promise<Record<string, unknown>> {
    const executor = await this.createCodeSandbox();
    if (executor === null) {
      return { ok: false, error: "the code sandbox is unavailable" };
    }
    const items = this.activeShoppingItems();
    const execution = await executor.execute(code, {
      listShoppingItems: async () => items.map((item) => ({
        id: item.id,
        name: item.name,
        quantity: item.quantity,
      })),
    });
    const logs = (execution.logs ?? []).slice(0, 3)
      .map((line) => line.slice(0, 200));
    if (execution.error !== undefined) {
      return {
        ok: false,
        error: `sandbox error: ${execution.error.slice(0, 240)}`,
        logs,
      };
    }
    const output = execution.result as Record<string, unknown> | null;
    try {
      if (typeof output?.svg === "string") {
        return await this.preparePrintJob(
          "svg", title, validateSvgSource(output.svg));
      }
      if (typeof output?.text === "string") {
        return await this.preparePrintJob(
          "note", title, validateNoteText(output.text));
      }
    } catch (error) {
      return {
        ok: false,
        error: (error instanceof Error
          ? error.message : "invalid document").slice(0, 240),
        logs,
      };
    }
    return {
      ok: false,
      error: "code must return { svg: string } or { text: string }",
      logs,
    };
  }

  private async createCodeSandbox(): Promise<CodeSandboxExecutor | null> {
    const loader = (this.env as { LOADER?: unknown }).LOADER;
    if (loader === undefined || loader === null) return null;
    const { DynamicWorkerExecutor } = await import("@cloudflare/codemode");
    return new DynamicWorkerExecutor({
      loader: loader as ConstructorParameters<
        typeof DynamicWorkerExecutor>[0]["loader"],
      timeout: 20_000,
    }) as CodeSandboxExecutor;
  }

  private async commitPrintJob(
    jobId: string,
    digest: string,
  ): Promise<Record<string, unknown>> {
    const job = this.getPrintJob(jobId);
    if (job === null) return { ok: false, error: "unknown print job" };
    if (job.state === "confirmed" || job.state === "pending") {
      return { ok: true, jobId, state: job.state };
    }
    if (job.state !== "prepared") {
      return { ok: false, error: `the job is already ${job.state}` };
    }
    if (job.digest !== digest) {
      return {
        ok: false,
        error: "digest mismatch: prepare the job again",
      };
    }
    if (Date.now() > job.prepared_expires_at) {
      return {
        ok: false,
        error: "the prepared job expired: prepare it again",
      };
    }
    this.sql`
      UPDATE print_jobs SET committed_at = ${Date.now()}
      WHERE job_id = ${jobId}
    `;
    this.setPrintJobState(jobId, nextPrintJobState(job.state, "commit"));
    await this.schedule(0, "renderPrintJob", jobId);
    return {
      ok: true,
      jobId,
      state: "confirmed",
      message: "the job is confirmed and heading to the printer",
    };
  }

  /** Schedule callback: render a confirmed job into its bridge payload. */
  async renderPrintJob(jobId: unknown): Promise<void> {
    if (typeof jobId !== "string") return;
    const job = this.getPrintJob(jobId);
    if (job === null || job.state !== "confirmed") return;
    let payloadType: string;
    let payloadBase64: string;
    try {
      const html = job.kind === "svg"
        ? svgPrintHtml(job.content)
        : textPrintHtml(job.content);
      const pdf = await this.renderPdf(html);
      payloadType = "application/pdf";
      payloadBase64 = pdf;
    } catch (error) {
      const reason = (error instanceof Error
        ? error.message : "render failed").slice(0, 240);
      if (job.kind === "svg") {
        // A drawing cannot fall back to text; the job fails truthfully.
        this.setPrintJobState(
          jobId,
          nextPrintJobState(job.state, "render.failed"),
          `pdf render failed: ${reason}`,
        );
        return;
      }
      console.warn(JSON.stringify({
        event: "print.render_fallback",
        installation: this.name,
        jobId,
        reason,
      }));
      payloadType = "text/plain";
      payloadBase64 = bytesToBase64(
        new TextEncoder().encode(job.content));
    }
    this.sql`
      UPDATE print_jobs
      SET payload_type = ${payloadType}, payload_base64 = ${payloadBase64}
      WHERE job_id = ${jobId}
    `;
    this.setPrintJobState(
      jobId, nextPrintJobState(job.state, "render.succeeded"));
    await this.schedule(PENDING_TTL_SECONDS, "printPendingExpired", jobId);
    this.notifyBridges();
  }

  private async renderPdf(html: string): Promise<string> {
    const browser = (this.env as { BROWSER?: unknown }).BROWSER as {
      quickAction(
        action: string,
        options: Record<string, unknown>,
      ): Promise<unknown>;
    } | undefined;
    if (browser === undefined || typeof browser.quickAction !== "function") {
      throw new Error("browser rendering binding is unavailable");
    }
    const rendered = await browser.quickAction("pdf", { html });
    let bytes: Uint8Array;
    if (rendered instanceof ArrayBuffer) {
      bytes = new Uint8Array(rendered);
    } else if (rendered instanceof Uint8Array) {
      bytes = rendered;
    } else if (typeof rendered === "string") {
      bytes = base64ToBytes(rendered);
    } else if (rendered instanceof Response) {
      bytes = new Uint8Array(await rendered.arrayBuffer());
    } else {
      throw new Error("unexpected pdf render result shape");
    }
    if (bytes.byteLength < 5
        || String.fromCharCode(...bytes.subarray(0, 4)) !== "%PDF") {
      throw new Error("render result is not a PDF document");
    }
    const encoded = bytesToBase64(bytes);
    if (encoded.length > MAX_PDF_BASE64_CHARS) {
      throw new Error("rendered PDF exceeds the bounded size");
    }
    return encoded;
  }

  // ---- Bridge claim/ack ---------------------------------------------------

  private hasBridgeConnection(): boolean {
    for (const connection of
      this.getConnections<DeviceConnectionState>()) {
      if (connection.state?.role === "bridge") return true;
    }
    return false;
  }

  private notifyBridges(): void {
    const pending = this.countPrintJobs("pending");
    if (pending === 0) return;
    for (const connection of
      this.getConnections<DeviceConnectionState>()) {
      if (connection.state?.role === "bridge") {
        sendJson(connection, { v: 1, type: "job.available", pending });
      }
    }
  }

  async claimPrintJob(): Promise<Record<string, unknown> | null> {
    const rows = this.sql<{ job_id: string }>`
      SELECT job_id FROM print_jobs
      WHERE state = 'pending'
      ORDER BY created_at ASC, job_id ASC
      LIMIT 1
    `;
    const jobId = rows[0]?.job_id;
    if (jobId === undefined) return null;
    const job = this.getPrintJob(jobId);
    if (job === null) return null;
    const claimId = crypto.randomUUID();
    this.sql`
      UPDATE print_jobs
      SET claim_id = ${claimId}, claimed_at = ${Date.now()}
      WHERE job_id = ${jobId}
    `;
    this.setPrintJobState(jobId, nextPrintJobState(job.state, "claim"));
    await this.schedule(
      CLAIM_LEASE_SECONDS,
      "printClaimLeaseExpired",
      { jobId, claimId } satisfies PrintJobLease,
    );
    return {
      jobId,
      claimId,
      kind: job.kind,
      title: job.title,
      digest: job.digest,
      printer: job.printer,
      pages: job.pages,
      payloadType: job.payload_type,
      payloadBase64: job.payload_base64,
      leaseSeconds: CLAIM_LEASE_SECONDS,
    };
  }

  async ackPrintJob(input: unknown): Promise<Record<string, unknown>> {
    const value = (typeof input === "object" && input !== null
      ? input : {}) as Record<string, unknown>;
    const jobId = typeof value.jobId === "string" ? value.jobId : "";
    const claimId = typeof value.claimId === "string" ? value.claimId : "";
    const phase = value.phase;
    const localJobId = typeof value.localJobId === "string"
      ? value.localJobId.slice(0, 64)
      : null;
    const failure = typeof value.failure === "string"
      ? value.failure.slice(0, 240)
      : null;
    if (phase !== "submitted" && phase !== "completed"
        && phase !== "failed") {
      return { ok: false, error: "phase must be submitted|completed|failed" };
    }
    const job = this.getPrintJob(jobId);
    if (job === null) return { ok: false, error: "unknown print job" };
    if (job.claim_id !== claimId || claimId.length === 0) {
      return { ok: false, error: "claim does not match the job" };
    }
    const event = phase === "submitted"
      ? "ack.submitted" as const
      : phase === "completed"
        ? "ack.completed" as const
        : "ack.failed" as const;
    // Repeating the ack that produced the current state is a no-op so the
    // bridge can retry safely after a crash.
    if ((phase === "submitted" && job.state === "submitted")
        || (phase === "completed" && job.state === "completed")
        || (phase === "failed" && job.state === "failed")) {
      return { ok: true, jobId, state: job.state };
    }
    let nextState: PrintJobState;
    try {
      nextState = nextPrintJobState(job.state, event);
    } catch (error) {
      return {
        ok: false,
        error: error instanceof Error ? error.message : "invalid ack",
      };
    }
    if (phase === "submitted") {
      this.sql`
        UPDATE print_jobs
        SET local_job_id = ${localJobId}, submitted_at = ${Date.now()}
        WHERE job_id = ${jobId}
      `;
      await this.schedule(
        SUBMIT_STALE_SECONDS,
        "printSubmitStale",
        { jobId, claimId } satisfies PrintJobLease,
      );
    }
    this.setPrintJobState(jobId, nextState, failure);
    return { ok: true, jobId, state: nextState };
  }

  // ---- Schedule callbacks -------------------------------------------------

  /** Schedule callback: cancel a prepared job never confirmed in time. */
  printPrepareExpired(jobId: unknown): void {
    if (typeof jobId !== "string") return;
    const job = this.getPrintJob(jobId);
    if (job?.state !== "prepared") return;
    this.setPrintJobState(
      jobId,
      nextPrintJobState(job.state, "prepare.expired"),
      "the prepared job was not confirmed in time",
    );
  }

  /** Schedule callback: fail a pending job no bridge ever claimed. */
  printPendingExpired(jobId: unknown): void {
    if (typeof jobId !== "string") return;
    const job = this.getPrintJob(jobId);
    if (job?.state !== "pending") return;
    this.setPrintJobState(
      jobId,
      nextPrintJobState(job.state, "pending.stale"),
      "no print bridge claimed the job in time",
    );
  }

  /** Schedule callback: a claimed job's lease lapsed without an ack. */
  printClaimLeaseExpired(payload: unknown): void {
    const lease = payload as PrintJobLease | null;
    if (typeof lease?.jobId !== "string"
        || typeof lease.claimId !== "string") return;
    const job = this.getPrintJob(lease.jobId);
    if (job?.state !== "claimed" || job.claim_id !== lease.claimId) return;
    this.setPrintJobState(
      lease.jobId,
      nextPrintJobState(job.state, "lease.expired"),
      "the bridge claimed the job but never acknowledged it",
    );
  }

  /** Schedule callback: a submitted job never confirmed completion. */
  printSubmitStale(payload: unknown): void {
    const lease = payload as PrintJobLease | null;
    if (typeof lease?.jobId !== "string"
        || typeof lease.claimId !== "string") return;
    const job = this.getPrintJob(lease.jobId);
    if (job?.state !== "submitted" || job.claim_id !== lease.claimId) return;
    this.setPrintJobState(
      lease.jobId,
      nextPrintJobState(job.state, "submit.stale"),
      "the printer never confirmed completion",
    );
  }

  // ---- Debug --------------------------------------------------------------

  /**
   * Bring-up seam: prepare and immediately commit a bounded test job so the
   * bridge path can be exercised without a voice session. Uses the same
   * validation, digest, and state machine as the real tools.
   */
  async printTestForBringup(input: unknown): Promise<Record<string, unknown>> {
    const value = (typeof input === "object" && input !== null
      ? input : {}) as Record<string, unknown>;
    const kind = value.kind === "svg" ? "svg" as const : "note" as const;
    const title = `Bring-up test ${new Date().toISOString()}`;
    let content: string;
    try {
      content = kind === "svg"
        ? validateSvgSource(value.svg)
        : validateNoteText(typeof value.text === "string"
          ? value.text
          : `Walle print bring-up test\n${title}`);
    } catch (error) {
      return {
        ok: false,
        error: error instanceof Error ? error.message : "invalid content",
      };
    }
    const prepared = await this.preparePrintJob(kind, title, content);
    if (prepared.ok !== true) return prepared;
    const committed = await this.commitPrintJob(
      prepared.jobId as string, prepared.digest as string);
    return { prepared, committed };
  }

  getShoppingListDebug(): Record<string, unknown> {
    return {
      version: SHOPPING_LIST_VERSION,
      items: this.activeShoppingItems(),
    };
  }

  getPrintJobsDebug(): Record<string, unknown> {
    const rows = this.sql<{
      job_id: string;
      state: string;
      kind: string;
      title: string;
      digest: string;
      payload_type: string | null;
      local_job_id: string | null;
      failure: string | null;
      created_at: number;
      finished_at: number | null;
    }>`
      SELECT job_id, state, kind, title, digest, payload_type,
             local_job_id, failure, created_at, finished_at
      FROM print_jobs ORDER BY created_at DESC, job_id DESC LIMIT 20
    `;
    return {
      version: PRINT_PIPELINE_VERSION,
      bridgeConnected: this.hasBridgeConnection(),
      jobs: rows,
    };
  }

  onClose(
    connection: Connection<DeviceConnectionState>,
    code: number,
    reason: string,
    wasClean: boolean,
  ): void {
    if (connection.state?.role === "bridge") {
      console.log(JSON.stringify({
        event: "bridge.disconnected",
        installation: this.name,
        connectionId: connection.id,
        code,
        reason,
        wasClean,
      }));
      return;
    }
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
