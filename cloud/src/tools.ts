import { MAX_ITEMS_PER_ADD, parseItemInputs } from "./shopping";
import type { ShoppingItemInput } from "./shopping";
import {
  normalizeTitle,
  validateNoteText,
  validateSvgSource,
} from "./printing";

export const WALLE_TOOLS_VERSION = "walle-tools-v1";

export const MAX_TOOL_ARGUMENT_CHARS = 220_000;
export const MAX_TOOL_OUTPUT_CHARS = 16_000;
export const MAX_COMPOSE_CODE_CHARS = 16_000;

/**
 * Realtime function tool definitions sent in `session.update`. The model
 * never chooses printers, hosts, or file paths; physical printing always
 * flows through prepare -> spoken confirmation -> commit with a digest.
 */
export const REALTIME_TOOL_DEFINITIONS = [
  {
    type: "function",
    name: "shopping_list_read",
    description: "Read the current shopping list. Returns every active "
      + "item with its canonical id.",
    parameters: {
      type: "object",
      properties: {},
      additionalProperties: false,
    },
  },
  {
    type: "function",
    name: "shopping_list_add",
    description: "Add items to the shopping list. Only call with items the "
      + "user clearly asked for; ask a short clarifying question when the "
      + "item or quantity is ambiguous. Returns the canonical applied "
      + "result — report that result, not your intent.",
    parameters: {
      type: "object",
      properties: {
        items: {
          type: "array",
          minItems: 1,
          maxItems: MAX_ITEMS_PER_ADD,
          items: {
            type: "object",
            properties: {
              name: { type: "string", description: "Item name" },
              quantity: {
                type: "string",
                description: "Optional quantity, e.g. '2' or '500 g'",
              },
            },
            required: ["name"],
            additionalProperties: false,
          },
        },
      },
      required: ["items"],
      additionalProperties: false,
    },
  },
  {
    type: "function",
    name: "shopping_list_undo",
    description: "Undo the most recent shopping list addition. Returns "
      + "which items were removed.",
    parameters: {
      type: "object",
      properties: {},
      additionalProperties: false,
    },
  },
  {
    type: "function",
    name: "print_prepare",
    description: "Prepare a print job for the household printer. Nothing "
      + "prints yet: describe the returned summary to the user and ask for "
      + "explicit confirmation, then call print_commit with the returned "
      + "jobId and digest. kind 'shopping_list' prints the current list; "
      + "kind 'note' prints short text; kind 'svg' prints a picture you "
      + "draw as a single SVG document (use it for drawings the user asks "
      + "for, like a dinosaur).",
    parameters: {
      type: "object",
      properties: {
        kind: {
          type: "string",
          enum: ["shopping_list", "note", "svg"],
        },
        title: { type: "string", description: "Short document title" },
        text: {
          type: "string",
          description: "Note text (kind 'note' only, one page max)",
        },
        svg: {
          type: "string",
          description: "Complete <svg>…</svg> document (kind 'svg' only). "
            + "No scripts, no external references.",
        },
      },
      required: ["kind"],
      additionalProperties: false,
    },
  },
  {
    type: "function",
    name: "compose_document",
    description: "Generate a printable document by writing JavaScript that "
      + "runs in a secure sandbox with no network access. Provide an async "
      + "arrow function, e.g. `async () => { const items = await "
      + "codemode.listShoppingItems(); return { text: ... }; }`. It must "
      + "return { text: string } or { svg: string }. Use this when the "
      + "document depends on stored data or needs computed layout; the "
      + "result becomes a prepared print job you must still confirm and "
      + "commit like print_prepare.",
    parameters: {
      type: "object",
      properties: {
        title: { type: "string", description: "Short document title" },
        code: {
          type: "string",
          description: "Async arrow function source. May call "
            + "codemode.listShoppingItems(). Must return "
            + "{ text } or { svg }.",
        },
      },
      required: ["title", "code"],
      additionalProperties: false,
    },
  },
  {
    type: "function",
    name: "print_commit",
    description: "Commit a prepared print job after the user explicitly "
      + "confirmed it out loud. This causes physical printing. Never call "
      + "it without a fresh spoken confirmation for this exact job.",
    parameters: {
      type: "object",
      properties: {
        jobId: { type: "string" },
        digest: { type: "string" },
      },
      required: ["jobId", "digest"],
      additionalProperties: false,
    },
  },
  {
    type: "function",
    name: "print_status",
    description: "Check the state of the latest print job, or a specific "
      + "one by jobId. Report the actual state; never claim a job printed "
      + "unless its state is completed.",
    parameters: {
      type: "object",
      properties: {
        jobId: { type: "string" },
      },
      additionalProperties: false,
    },
  },
] as const;

export type RealtimeToolCall = {
  callId: string;
  name: string;
  argumentsJson: string;
};

export type WalleToolInvocation =
  | { name: "shopping_list_read" }
  | { name: "shopping_list_add"; items: ShoppingItemInput[] }
  | { name: "shopping_list_undo" }
  | {
    name: "print_prepare";
    kind: "shopping_list" | "note" | "svg";
    title: string;
    text: string | null;
    svg: string | null;
  }
  | { name: "compose_document"; title: string; code: string }
  | { name: "print_commit"; jobId: string; digest: string }
  | { name: "print_status"; jobId: string | null };

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null
    ? value as Record<string, unknown>
    : null;
}

function boundedId(value: unknown, label: string): string {
  if (typeof value !== "string" || value.length === 0
      || value.length > 64 || !/^[a-zA-Z0-9_-]+$/.test(value)) {
    throw new Error(`${label} is not a valid identifier`);
  }
  return value;
}

/** Parse and validate one model tool call into a typed invocation. */
export function parseToolInvocation(
  call: RealtimeToolCall,
): WalleToolInvocation {
  if (call.argumentsJson.length > MAX_TOOL_ARGUMENT_CHARS) {
    throw new Error("tool arguments exceed the bounded size");
  }
  let parsed: unknown;
  try {
    parsed = call.argumentsJson === "" ? {} : JSON.parse(call.argumentsJson);
  } catch {
    throw new Error("tool arguments are not valid JSON");
  }
  const args = record(parsed);
  if (args === null) throw new Error("tool arguments must be an object");

  switch (call.name) {
    case "shopping_list_read":
      return { name: "shopping_list_read" };
    case "shopping_list_add":
      return {
        name: "shopping_list_add",
        items: parseItemInputs(args.items),
      };
    case "shopping_list_undo":
      return { name: "shopping_list_undo" };
    case "print_prepare": {
      const kind = args.kind;
      if (kind !== "shopping_list" && kind !== "note" && kind !== "svg") {
        throw new Error("kind must be shopping_list, note, or svg");
      }
      return {
        name: "print_prepare",
        kind,
        title: normalizeTitle(args.title),
        text: kind === "note" ? validateNoteText(args.text) : null,
        svg: kind === "svg" ? validateSvgSource(args.svg) : null,
      };
    }
    case "compose_document": {
      const code = args.code;
      if (typeof code !== "string" || code.trim().length === 0
          || code.length > MAX_COMPOSE_CODE_CHARS) {
        throw new Error(
          `code is required and limited to ${MAX_COMPOSE_CODE_CHARS}`
          + " characters");
      }
      return {
        name: "compose_document",
        title: normalizeTitle(args.title),
        code,
      };
    }
    case "print_commit":
      return {
        name: "print_commit",
        jobId: boundedId(args.jobId, "jobId"),
        digest: boundedId(args.digest, "digest"),
      };
    case "print_status":
      return {
        name: "print_status",
        jobId: args.jobId === undefined || args.jobId === null
          ? null
          : boundedId(args.jobId, "jobId"),
      };
    default:
      throw new Error(`unknown tool: ${call.name.slice(0, 64)}`);
  }
}

/** Serialize a tool result, guaranteeing the bounded output size. */
export function encodeToolResult(value: Record<string, unknown>): string {
  const encoded = JSON.stringify(value);
  if (encoded.length <= MAX_TOOL_OUTPUT_CHARS) return encoded;
  return JSON.stringify({
    ok: false,
    error: "tool result exceeded the bounded output size",
  });
}
