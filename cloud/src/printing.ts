export const PRINT_PIPELINE_VERSION = "walle-print-v1";

/** The only physical destination this installation may print to. */
export const ALLOWED_PRINTER = "HP_DeskJet_2800_series";

export const PREPARE_TTL_SECONDS = 300;
export const PENDING_TTL_SECONDS = 900;
export const CLAIM_LEASE_SECONDS = 90;
export const SUBMIT_STALE_SECONDS = 300;

export const MAX_TITLE_CHARS = 80;
export const MAX_NOTE_CHARS = 4_000;
export const MAX_TEXT_LINES = 58; // single A4 page at 12pt monospace
export const MAX_SVG_CHARS = 200_000;
export const MAX_PDF_BASE64_CHARS = 4_000_000;

export type PrintJobState =
  | "prepared"
  | "confirmed"
  | "pending"
  | "claimed"
  | "submitted"
  | "completed"
  | "failed"
  | "cancelled"
  | "needs_review";

export type PrintJobEvent =
  | "commit"
  | "prepare.expired"
  | "render.succeeded"
  | "render.failed"
  | "pending.stale"
  | "claim"
  | "lease.expired"
  | "ack.submitted"
  | "ack.completed"
  | "ack.failed"
  | "submit.stale";

export type PrintContentKind = "note" | "shopping_list" | "svg";

const TRANSITIONS: Record<PrintJobState, Partial<
  Record<PrintJobEvent, PrintJobState>>> = {
  prepared: {
    commit: "confirmed",
    "prepare.expired": "cancelled",
  },
  confirmed: {
    "render.succeeded": "pending",
    "render.failed": "failed",
  },
  pending: {
    claim: "claimed",
    "pending.stale": "failed",
  },
  claimed: {
    "ack.submitted": "submitted",
    "ack.failed": "failed",
    // The bridge may have reached the printer before crashing; a human
    // must resolve the ambiguity. Never automatically resubmit.
    "lease.expired": "needs_review",
  },
  submitted: {
    "ack.completed": "completed",
    "ack.failed": "failed",
    "submit.stale": "needs_review",
  },
  completed: {},
  failed: {},
  cancelled: {},
  needs_review: {},
};

export function nextPrintJobState(
  state: PrintJobState,
  event: PrintJobEvent,
): PrintJobState {
  const next = TRANSITIONS[state]?.[event];
  if (next === undefined) {
    throw new Error(`print job cannot ${event} from ${state}`);
  }
  return next;
}

export function isTerminalPrintJobState(state: PrintJobState): boolean {
  return Object.keys(TRANSITIONS[state]).length === 0;
}

export function normalizeTitle(value: unknown): string {
  if (typeof value !== "string") return "Untitled";
  const collapsed = value.replace(/\s+/g, " ").trim();
  if (collapsed.length === 0) return "Untitled";
  return collapsed.slice(0, MAX_TITLE_CHARS);
}

/** Bound plain-text content to one printed page. */
export function validateNoteText(value: unknown): string {
  if (typeof value !== "string") {
    throw new Error("text content is required");
  }
  const text = value.replace(/\r\n?/g, "\n").trim();
  if (text.length === 0 || text.length > MAX_NOTE_CHARS) {
    throw new Error(`text is limited to ${MAX_NOTE_CHARS} characters`);
  }
  if (text.split("\n").length > MAX_TEXT_LINES) {
    throw new Error(`text is limited to ${MAX_TEXT_LINES} lines (one page)`);
  }
  return text;
}

// Rendering happens in an isolated headless browser, but the SVG is still
// untrusted model output: forbid script, event handlers, external loads,
// and foreign content outright rather than trying to sanitize them.
const SVG_FORBIDDEN_PATTERNS: Array<{ pattern: RegExp; reason: string }> = [
  { pattern: /<\s*script/i, reason: "script elements" },
  { pattern: /<\s*foreignObject/i, reason: "foreignObject elements" },
  { pattern: /<\s*(iframe|embed|object)/i, reason: "embedded documents" },
  { pattern: /\son[a-z]+\s*=/i, reason: "event handler attributes" },
  { pattern: /javascript:/i, reason: "javascript URLs" },
  { pattern: /(href|src)\s*=\s*["'](?!#)/i, reason: "external references" },
  { pattern: /url\(\s*(?!['"]?#)/i, reason: "external CSS references" },
  { pattern: /<\s*!(?:doctype|entity)/i, reason: "doctype or entities" },
  { pattern: /@import/i, reason: "CSS imports" },
];

export function validateSvgSource(value: unknown): string {
  if (typeof value !== "string") {
    throw new Error("svg content is required");
  }
  const svg = value.trim();
  if (svg.length === 0 || svg.length > MAX_SVG_CHARS) {
    throw new Error(`svg is limited to ${MAX_SVG_CHARS} characters`);
  }
  if (!/^<svg[\s>]/i.test(svg) || !/<\/svg>\s*$/i.test(svg)) {
    throw new Error("svg must be a single <svg>…</svg> document");
  }
  for (const { pattern, reason } of SVG_FORBIDDEN_PATTERNS) {
    if (pattern.test(svg)) {
      throw new Error(`svg must not contain ${reason}`);
    }
  }
  return svg;
}

/**
 * The digest binds what the user confirmed to what the bridge prints.
 * It covers the immutable prepared source, not the rendered PDF, so a
 * commit can be verified before rendering happens.
 */
export async function computePrintDigest(
  kind: PrintContentKind,
  title: string,
  content: string,
): Promise<string> {
  const bytes = new TextEncoder().encode(
    `${PRINT_PIPELINE_VERSION}\n${kind}\n${title}\n${content}`);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(digest), (byte) =>
    byte.toString(16).padStart(2, "0")).join("");
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let offset = 0; offset < bytes.byteLength; offset += 0x8000) {
    const chunk = bytes.subarray(offset, offset + 0x8000);
    binary += String.fromCharCode(...chunk);
  }
  return btoa(binary);
}

export function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index++) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
}

/** Wrap validated SVG for single-page A4 PDF rendering. */
export function svgPrintHtml(svg: string): string {
  return [
    "<!doctype html>",
    "<html><head><meta charset=\"utf-8\"><style>",
    "@page { size: A4; margin: 12mm; }",
    "html, body { margin: 0; height: 100%; }",
    "body { display: flex; align-items: center; justify-content: center; }",
    "svg { max-width: 100%; max-height: 100%; }",
    "</style></head><body>",
    svg,
    "</body></html>",
  ].join("\n");
}

/** Wrap plain text for PDF rendering with a fixed, printable layout. */
export function textPrintHtml(text: string): string {
  const escaped = text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
  return [
    "<!doctype html>",
    "<html><head><meta charset=\"utf-8\"><style>",
    "@page { size: A4; margin: 18mm; }",
    "body { margin: 0; }",
    "pre { font: 12pt/1.45 'Courier New', monospace; white-space: pre-wrap;",
    "      word-break: break-word; margin: 0; }",
    "</style></head><body><pre>",
    escaped,
    "</pre></body></html>",
  ].join("\n");
}
