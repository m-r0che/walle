import {
  Agent,
  type Connection,
  type ConnectionContext,
  type WSMessage,
} from "agents";

import {
  AudioFrameKind,
  decodeAudioFrame,
  decodeControlMessage,
  encodeAudioFrame,
} from "./protocol";

export type RelayDebugStatus = {
  connections: Array<{
    ready: boolean;
    firmware: string | null;
    sessionEpoch: number;
    activeTurnId: string | null;
    connectedAt: number;
  }>;
  lastTurn: TurnMetrics | null;
  lastPlayback: PlaybackMetrics | null;
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
};

type DeviceConnectionState = {
  ready: boolean;
  firmware: string | null;
  sessionEpoch: number;
  activeTurnId: string | null;
  lastTurnId: string | null;
  connectedAt: number;
};

function sendJson(connection: Connection, value: unknown): void {
  connection.send(JSON.stringify(value));
}

export class WalleAgent extends Agent<Env> {
  private readonly turnStats = new Map<string, TurnStats>();
  private readonly suppressedPongs = new Set<string>();

  static options = {
    sendIdentityOnConnect: false,
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
        sessionEpoch: connection.state?.sessionEpoch ?? 0,
        activeTurnId: connection.state?.activeTurnId ?? null,
        connectedAt: connection.state?.connectedAt ?? 0,
      }),
    );
    return {
      connections,
      lastTurn: this.getLastTurnMetrics(),
      lastPlayback: this.getLastPlaybackMetrics(),
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

  disconnectDeviceForTest(): number {
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

  onMessage(
    connection: Connection<DeviceConnectionState>,
    message: WSMessage,
  ): void {
    try {
      if (typeof message === "string") {
        this.handleControl(connection, message);
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
      connection.close(4002, "Protocol error");
    }
  }

  private handleControl(
    connection: Connection<DeviceConnectionState>,
    text: string,
  ): void {
    const message = decodeControlMessage(text);
    const state = connection.state;
    if (state === null || state === undefined) {
      throw new Error("connection state is unavailable");
    }

    switch (message.type) {
      case "hello":
        connection.setState({
          ...state,
          ready: true,
          firmware: message.firmware,
          sessionEpoch: message.sessionEpoch ?? 0,
        });
        this.sql`
          UPDATE device_sessions
          SET firmware = ${message.firmware}
          WHERE connection_id = ${connection.id}
        `;
        sendJson(connection, {
          v: 1,
          type: "ready",
          mode: "echo",
          sampleRate: 24_000,
          channels: 1,
          sampleFormat: "pcm16le",
          sessionEpoch: message.sessionEpoch ?? 0,
        });
        return;
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
        this.turnStats.set(connection.id, {
          turnId: message.turnId,
          frames: 0,
          samples: 0,
          nextSequence: null,
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
        sendJson(connection, {
          v: 1,
          type: "turn.done",
          turnId: message.turnId,
          frames: committed?.frames ?? 0,
          samples: committed?.samples ?? 0,
        });
        console.log(JSON.stringify({
          event: "device.turn_echo",
          installation: this.name,
          connectionId: connection.id,
          turnId: message.turnId,
          frames: committed?.frames ?? 0,
          samples: committed?.samples ?? 0,
          bytes: (committed?.samples ?? 0) * 2,
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
      case "turn.cancel":
        if (state.activeTurnId !== message.turnId) {
          throw new Error("turn cancel does not match the active turn");
        }
        connection.setState({
          ...state,
          activeTurnId: null,
          lastTurnId: null,
        });
        this.turnStats.delete(connection.id);
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
    stats.frames++;
    stats.samples += frame.sampleCount;

    connection.send(encodeAudioFrame({
      ...frame,
      kind: AudioFrameKind.OutputPcm16,
    }));
  }

  onClose(
    connection: Connection<DeviceConnectionState>,
    code: number,
    reason: string,
    wasClean: boolean,
  ): void {
    this.turnStats.delete(connection.id);
    this.suppressedPongs.delete(connection.id);
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
