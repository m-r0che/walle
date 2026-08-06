#!/usr/bin/env node
// Walle print bridge: claims confirmed print jobs from the cloud relay and
// submits them to the one allowlisted local CUPS printer.
//
// Durability contract (docs/bridge-protocol.md):
// - The WebSocket is only a wake-up hint; jobs move over authenticated HTTPS
//   claim/ack calls, so a dropped socket can never lose or duplicate a job.
// - Every claim and submission is appended to a local receipt ledger before
//   the next step runs. After a crash, the ledger decides whether a claimed
//   job may be resubmitted (never after `lp` was attempted).

import { execFile } from "node:child_process";
import { appendFileSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { writeFile } from "node:fs/promises";
import { homedir, hostname } from "node:os";
import { dirname, join } from "node:path";
import process from "node:process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

const APP_SUPPORT = join(
  homedir(), "Library", "Application Support", "Walle");
const CONFIG = {
  relayUrl: process.env.WALLE_RELAY_URL
    ?? "https://walle-relay.matt-ce8.workers.dev",
  tokenFile: process.env.WALLE_BRIDGE_TOKEN_FILE
    ?? join(APP_SUPPORT, "secrets", "bridge-token.txt"),
  printer: process.env.WALLE_PRINTER ?? "HP_DeskJet_2800_series",
  ledgerFile: process.env.WALLE_BRIDGE_LEDGER
    ?? join(APP_SUPPORT, "bridge", "receipts.jsonl"),
  spoolDir: process.env.WALLE_BRIDGE_SPOOL
    ?? join(APP_SUPPORT, "bridge", "spool"),
  dryRun: process.env.WALLE_BRIDGE_DRY_RUN === "1",
  pollSeconds: Number(process.env.WALLE_BRIDGE_POLL_SECONDS ?? "60"),
};

const MAX_PAYLOAD_BYTES = 4 * 1024 * 1024;
const COMPLETION_POLL_MS = 2_000;
const COMPLETION_TIMEOUT_MS = 180_000;

let token;
let claimLoopActive = false;
let claimLoopQueued = false;

function log(event, fields = {}) {
  console.log(JSON.stringify({
    at: new Date().toISOString(),
    event,
    ...fields,
  }));
}

function loadToken() {
  if (!existsSync(CONFIG.tokenFile)) {
    console.error(`Bridge token file not found: ${CONFIG.tokenFile}`);
    console.error("Create it with a random value of 16+ characters and "
      + "chmod 600, matching the Worker BRIDGE_TOKEN secret.");
    process.exit(1);
  }
  const value = readFileSync(CONFIG.tokenFile, "utf8").trim();
  if (value.length < 16) {
    console.error("Bridge token must be at least 16 characters.");
    process.exit(1);
  }
  return value;
}

// ---- Receipt ledger --------------------------------------------------------

function appendReceipt(entry) {
  mkdirSync(dirname(CONFIG.ledgerFile), { recursive: true });
  appendFileSync(
    CONFIG.ledgerFile,
    `${JSON.stringify({ at: new Date().toISOString(), ...entry })}\n`);
}

function readReceipts(jobId) {
  if (!existsSync(CONFIG.ledgerFile)) return [];
  return readFileSync(CONFIG.ledgerFile, "utf8")
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => {
      try {
        return JSON.parse(line);
      } catch {
        return null;
      }
    })
    .filter((entry) => entry !== null && entry.jobId === jobId);
}

// ---- Relay API -------------------------------------------------------------

async function relayFetch(path, options = {}) {
  const response = await fetch(`${CONFIG.relayUrl}${path}`, {
    ...options,
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
      ...options.headers,
    },
  });
  if (!response.ok) {
    throw new Error(`${path} responded ${response.status}`);
  }
  return response.json();
}

const claimJob = () =>
  relayFetch("/v1/bridge/claim", { method: "POST" })
    .then((body) => body.job ?? null);

const ackJob = (jobId, claimId, phase, extra = {}) =>
  relayFetch("/v1/bridge/ack", {
    method: "POST",
    body: JSON.stringify({ jobId, claimId, phase, ...extra }),
  });

// ---- Printing --------------------------------------------------------------

async function submitToPrinter(job, payloadPath) {
  if (CONFIG.dryRun) {
    log("bridge.dry_run_submit", { jobId: job.jobId, payloadPath });
    return `dry-run-${Date.now()}`;
  }
  const { stdout } = await execFileAsync("lp", [
    "-d", CONFIG.printer,
    "-t", `walle-${job.jobId.slice(0, 8)}`,
    "-o", "media=A4",
    payloadPath,
  ]);
  // lp reports: "request id is HP_DeskJet_2800_series-42 (1 file(s))"
  const match = /request id is (\S+)/.exec(stdout);
  return match?.[1] ?? "unknown";
}

async function waitForCompletion(localJobId) {
  if (CONFIG.dryRun) return true;
  const deadline = Date.now() + COMPLETION_TIMEOUT_MS;
  while (Date.now() < deadline) {
    const { stdout } = await execFileAsync(
      "lpstat", ["-W", "not-completed", "-o"]);
    if (!stdout.includes(localJobId)) return true;
    await new Promise((resolve) => setTimeout(resolve, COMPLETION_POLL_MS));
  }
  return false;
}

async function writeSpoolFile(job) {
  mkdirSync(CONFIG.spoolDir, { recursive: true });
  const payload = Buffer.from(job.payloadBase64 ?? "", "base64");
  if (payload.byteLength === 0 || payload.byteLength > MAX_PAYLOAD_BYTES) {
    throw new Error("job payload is missing or out of bounds");
  }
  const extension = job.payloadType === "application/pdf" ? "pdf" : "txt";
  const path = join(CONFIG.spoolDir, `${job.jobId}.${extension}`);
  await writeFile(path, payload, { mode: 0o600 });
  return path;
}

async function processJob(job) {
  const receipts = readReceipts(job.jobId);
  if (receipts.some((entry) => entry.step === "lp_attempted")) {
    // A previous run may have reached the printer before crashing. Never
    // resubmit; report the ambiguity and let the cloud mark needs_review.
    log("bridge.ambiguous_replay", { jobId: job.jobId });
    await ackJob(job.jobId, job.claimId, "failed", {
      failure: "bridge crashed after a previous submission attempt",
    });
    return;
  }
  if (job.printer !== CONFIG.printer) {
    await ackJob(job.jobId, job.claimId, "failed", {
      failure: `printer ${job.printer} is not allowlisted on this bridge`,
    });
    return;
  }
  appendReceipt({
    step: "claimed",
    jobId: job.jobId,
    claimId: job.claimId,
    digest: job.digest,
    host: hostname(),
  });
  const payloadPath = await writeSpoolFile(job);
  appendReceipt({ step: "lp_attempted", jobId: job.jobId,
    claimId: job.claimId, payloadPath });
  let localJobId;
  try {
    localJobId = await submitToPrinter(job, payloadPath);
  } catch (error) {
    appendReceipt({ step: "lp_failed", jobId: job.jobId,
      error: String(error).slice(0, 200) });
    await ackJob(job.jobId, job.claimId, "failed", {
      failure: `lp submission failed: ${String(error).slice(0, 160)}`,
    });
    return;
  }
  appendReceipt({ step: "submitted", jobId: job.jobId,
    claimId: job.claimId, localJobId });
  await ackJob(job.jobId, job.claimId, "submitted", { localJobId });
  log("bridge.submitted", { jobId: job.jobId, localJobId });

  const completed = await waitForCompletion(localJobId);
  if (completed) {
    appendReceipt({ step: "completed", jobId: job.jobId, localJobId });
    await ackJob(job.jobId, job.claimId, "completed", { localJobId });
    log("bridge.completed", { jobId: job.jobId, localJobId });
  } else {
    log("bridge.completion_unconfirmed", { jobId: job.jobId, localJobId });
    // No completed ack: the cloud's submit.stale timer will flag the job
    // for review rather than pretending it printed.
  }
}

async function runClaimLoop(reason) {
  if (claimLoopActive) {
    claimLoopQueued = true;
    return;
  }
  claimLoopActive = true;
  try {
    for (;;) {
      const job = await claimJob();
      if (job === null) break;
      log("bridge.claimed", { jobId: job.jobId, kind: job.kind,
        reason });
      await processJob(job);
    }
  } catch (error) {
    log("bridge.claim_loop_error", { error: String(error).slice(0, 200) });
  } finally {
    claimLoopActive = false;
    if (claimLoopQueued) {
      claimLoopQueued = false;
      void runClaimLoop("queued");
    }
  }
}

// ---- Wake-up hint socket ---------------------------------------------------

function connectHintSocket(attempt = 0) {
  const wsUrl = `${CONFIG.relayUrl.replace(/^http/, "ws")}/v1/bridge`;
  const socket = new WebSocket(wsUrl, {
    headers: { Authorization: `Bearer ${token}` },
  });
  const reconnect = () => {
    const delay = Math.min(60_000, 1_000 * 2 ** attempt)
      * (0.5 + Math.random());
    setTimeout(() => connectHintSocket(Math.min(attempt + 1, 6)), delay);
  };
  socket.addEventListener("open", () => {
    attempt = 0;
    socket.send(JSON.stringify({ v: 1, type: "hello" }));
    log("bridge.socket_open", { url: wsUrl });
    void runClaimLoop("reconnect");
  });
  socket.addEventListener("message", (event) => {
    try {
      const message = JSON.parse(String(event.data));
      if (message.type === "job.available") {
        void runClaimLoop("hint");
      }
    } catch {
      // Hints are best-effort; malformed frames are ignored.
    }
  });
  socket.addEventListener("close", (event) => {
    log("bridge.socket_closed", { code: event.code });
    reconnect();
  });
  socket.addEventListener("error", () => socket.close());
}

// ---- Main ------------------------------------------------------------------

token = loadToken();
log("bridge.start", {
  relay: CONFIG.relayUrl,
  printer: CONFIG.printer,
  dryRun: CONFIG.dryRun,
});
connectHintSocket();
setInterval(() => void runClaimLoop("poll"),
  Math.max(10, CONFIG.pollSeconds) * 1_000);
void runClaimLoop("startup");
