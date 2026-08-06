import { describe, expect, it } from "vitest";

import {
  encodeToolResult,
  MAX_TOOL_OUTPUT_CHARS,
  parseToolInvocation,
  REALTIME_TOOL_DEFINITIONS,
} from "../src/tools";

function call(name: string, args: unknown): {
  callId: string;
  name: string;
  argumentsJson: string;
} {
  return {
    callId: "call-1",
    name,
    argumentsJson: args === undefined ? "" : JSON.stringify(args),
  };
}

describe("tool definitions", () => {
  it("exposes every tool the agent implements", () => {
    expect(REALTIME_TOOL_DEFINITIONS.map((tool) => tool.name)).toEqual([
      "shopping_list_read",
      "shopping_list_add",
      "shopping_list_undo",
      "print_prepare",
      "compose_document",
      "print_commit",
      "print_status",
    ]);
    for (const tool of REALTIME_TOOL_DEFINITIONS) {
      expect(tool.type).toBe("function");
      expect(tool.description.length).toBeGreaterThan(20);
    }
  });
});

describe("tool invocation parsing", () => {
  it("parses list operations", () => {
    expect(parseToolInvocation(call("shopping_list_read", undefined)))
      .toEqual({ name: "shopping_list_read" });
    expect(parseToolInvocation(call("shopping_list_add", {
      items: [{ name: "Milk" }],
    }))).toEqual({
      name: "shopping_list_add",
      items: [{ name: "Milk", quantity: null }],
    });
  });

  it("parses print operations with kind-specific content", () => {
    const note = parseToolInvocation(call("print_prepare", {
      kind: "note",
      title: "Reminder",
      text: "Water the plants",
    }));
    expect(note).toMatchObject({
      name: "print_prepare",
      kind: "note",
      text: "Water the plants",
      svg: null,
    });
    const svg = parseToolInvocation(call("print_prepare", {
      kind: "svg",
      title: "Dino",
      svg: "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/></svg>",
    }));
    expect(svg).toMatchObject({ name: "print_prepare", kind: "svg" });
    expect(() => parseToolInvocation(call("print_prepare", {
      kind: "svg",
      svg: "<svg><script/></svg>",
    }))).toThrow(/script/);
    expect(() => parseToolInvocation(call("print_prepare", {
      kind: "poster",
    }))).toThrow(/kind/);
  });

  it("requires identifiers for commit and rejects unknown tools", () => {
    expect(parseToolInvocation(call("print_commit", {
      jobId: "job-1",
      digest: "abc123",
    }))).toEqual({ name: "print_commit", jobId: "job-1", digest: "abc123" });
    expect(() => parseToolInvocation(call("print_commit", {
      jobId: "job 1",
      digest: "abc",
    }))).toThrow(/jobId/);
    expect(() => parseToolInvocation(call("teleport", {}))).toThrow(
      /unknown tool/);
    expect(() => parseToolInvocation({
      callId: "c",
      name: "shopping_list_read",
      argumentsJson: "{not json",
    })).toThrow(/JSON/);
  });

  it("bounds compose_document code", () => {
    expect(parseToolInvocation(call("compose_document", {
      title: "List",
      code: "async () => ({ text: 'hi' })",
    }))).toMatchObject({ name: "compose_document", title: "List" });
    expect(() => parseToolInvocation(call("compose_document", {
      title: "List",
      code: "x".repeat(16_001),
    }))).toThrow(/limited/);
  });
});

describe("tool result encoding", () => {
  it("passes bounded results through and truncates oversized ones", () => {
    expect(encodeToolResult({ ok: true })).toBe("{\"ok\":true}");
    const oversized = encodeToolResult({
      ok: true,
      blob: "x".repeat(MAX_TOOL_OUTPUT_CHARS),
    });
    expect(oversized).toContain("bounded output size");
    expect(oversized.length).toBeLessThan(200);
  });
});
