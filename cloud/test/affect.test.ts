import { describe, expect, it } from "vitest";

import { classifyFaceAffect } from "../src/affect";

describe("face affect classification", () => {
  it.each([
    ["I’m here with you.", "warm"],
    ["Which one did you mean?", "curious"],
    ["That’s great news!", "delighted"],
    ["I’m not sure, but it might be nearby.", "uncertain"],
    ["I’m sorry, that went wrong.", "concerned"],
  ])("classifies %s as %s", (transcript, affect) => {
    const intent = classifyFaceAffect(transcript);
    expect(intent.affect).toBe(affect);
    expect(intent.intensity).toBeGreaterThanOrEqual(1);
    expect(intent.intensity).toBeLessThanOrEqual(100);
    expect(intent.ttlMs).toBe(12_000);
  });

  it("gives concern priority over incidental uncertainty", () => {
    expect(classifyFaceAffect(
      "I’m sorry. Maybe the printer failed.",
    ).affect).toBe("concerned");
  });

  it("bounds untrusted transcript input", () => {
    const transcript = `${"ordinary words ".repeat(300)} wonderful`;
    expect(classifyFaceAffect(transcript).affect).toBe("warm");
  });
});
