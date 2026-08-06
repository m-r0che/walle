import { describe, expect, it } from "vitest";

import {
  computePrintDigest,
  isTerminalPrintJobState,
  nextPrintJobState,
  svgPrintHtml,
  textPrintHtml,
  validateNoteText,
  validateSvgSource,
  type PrintJobState,
} from "../src/printing";

describe("print job state machine", () => {
  it("walks the happy path to completed", () => {
    let state: PrintJobState = "prepared";
    state = nextPrintJobState(state, "commit");
    state = nextPrintJobState(state, "render.succeeded");
    state = nextPrintJobState(state, "claim");
    state = nextPrintJobState(state, "ack.submitted");
    state = nextPrintJobState(state, "ack.completed");
    expect(state).toBe("completed");
    expect(isTerminalPrintJobState(state)).toBe(true);
  });

  it("routes ambiguity to needs_review, never resubmission", () => {
    expect(nextPrintJobState("claimed", "lease.expired"))
      .toBe("needs_review");
    expect(nextPrintJobState("submitted", "submit.stale"))
      .toBe("needs_review");
    expect(isTerminalPrintJobState("needs_review")).toBe(true);
    expect(() => nextPrintJobState("needs_review", "claim")).toThrow();
  });

  it("expires unconfirmed and unclaimed jobs", () => {
    expect(nextPrintJobState("prepared", "prepare.expired"))
      .toBe("cancelled");
    expect(nextPrintJobState("pending", "pending.stale")).toBe("failed");
  });

  it("rejects transitions that skip confirmation", () => {
    expect(() => nextPrintJobState("prepared", "claim")).toThrow();
    expect(() => nextPrintJobState("prepared", "ack.completed")).toThrow();
    expect(() => nextPrintJobState("completed", "commit")).toThrow();
  });
});

describe("note validation", () => {
  it("bounds length and line count", () => {
    expect(validateNoteText("hello\nworld")).toBe("hello\nworld");
    expect(() => validateNoteText("")).toThrow();
    expect(() => validateNoteText("x".repeat(4_001))).toThrow(/characters/);
    expect(() => validateNoteText(Array.from({ length: 59 }, () => "line")
      .join("\n"))).toThrow(/lines/);
  });
});

describe("svg validation", () => {
  const minimal = "<svg xmlns=\"http://www.w3.org/2000/svg\" "
    + "viewBox=\"0 0 100 100\"><circle cx=\"50\" cy=\"50\" r=\"40\" "
    + "fill=\"green\"/></svg>";

  it("accepts a plain drawing", () => {
    expect(validateSvgSource(minimal)).toBe(minimal);
  });

  it("accepts internal references but rejects external ones", () => {
    expect(validateSvgSource(
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><use href=\"#a\"/></svg>",
    )).toContain("#a");
    expect(() => validateSvgSource(
      "<svg><image href=\"https://x.example/a.png\"/></svg>",
    )).toThrow(/external/);
    expect(() => validateSvgSource(
      "<svg><rect style=\"fill:url(http://x)\"/></svg>",
    )).toThrow(/external/);
  });

  it("rejects script, handlers, and foreign content", () => {
    expect(() => validateSvgSource("<svg><script>1</script></svg>"))
      .toThrow(/script/);
    expect(() => validateSvgSource("<svg onload=\"x()\"></svg>"))
      .toThrow(/event handler/);
    expect(() => validateSvgSource(
      "<svg><foreignObject/></svg>")).toThrow(/foreignObject/);
    expect(() => validateSvgSource("<div>not svg</div>")).toThrow(/svg/);
  });
});

describe("print digest and wrappers", () => {
  it("is stable for identical content and distinct otherwise", async () => {
    const a = await computePrintDigest("note", "Title", "Body");
    const b = await computePrintDigest("note", "Title", "Body");
    const c = await computePrintDigest("note", "Title", "Body!");
    expect(a).toBe(b);
    expect(a).toMatch(/^[0-9a-f]{64}$/);
    expect(a).not.toBe(c);
  });

  it("escapes text and embeds svg for rendering", () => {
    expect(textPrintHtml("a<b>&c")).toContain("a&lt;b&gt;&amp;c");
    expect(svgPrintHtml("<svg/>")).toContain("<svg/>");
    expect(svgPrintHtml("<svg/>")).toContain("size: A4");
  });
});
