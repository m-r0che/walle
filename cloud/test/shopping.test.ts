import { describe, expect, it } from "vitest";

import {
  MAX_ITEM_NAME_CHARS,
  MAX_ITEMS_PER_ADD,
  parseItemInputs,
  renderShoppingListDocument,
} from "../src/shopping";

describe("shopping list inputs", () => {
  it("normalizes names and quantities", () => {
    const items = parseItemInputs([
      { name: "  Whole   milk " },
      { name: "Eggs", quantity: " 12 " },
    ]);
    expect(items).toEqual([
      { name: "Whole milk", quantity: null },
      { name: "Eggs", quantity: "12" },
    ]);
  });

  it("rejects empty, oversized, and non-object entries", () => {
    expect(() => parseItemInputs([])).toThrow(/1-12/);
    expect(() => parseItemInputs([{ name: "" }])).toThrow(/name/);
    expect(() => parseItemInputs([{ name: "x".repeat(
      MAX_ITEM_NAME_CHARS + 1) }])).toThrow(/name/);
    expect(() => parseItemInputs(["milk"])).toThrow(/name/);
    expect(() => parseItemInputs([{ name: "ok", quantity: 3 }]))
      .toThrow(/quantities/);
    expect(() => parseItemInputs(
      Array.from({ length: MAX_ITEMS_PER_ADD + 1 },
        () => ({ name: "x" })))).toThrow(/1-12/);
  });
});

describe("shopping list document", () => {
  it("renders a checkbox line per item with quantities", () => {
    const document = renderShoppingListDocument([
      { id: 1, name: "Milk", quantity: null, addedAt: 0 },
      { id: 2, name: "Eggs", quantity: "12", addedAt: 0 },
    ], new Date("2026-08-06T10:00:00Z"));
    expect(document).toContain("SHOPPING LIST");
    expect(document).toContain("2026-08-06");
    expect(document).toContain("[ ] Milk");
    expect(document).toContain("[ ] Eggs  (12)");
  });

  it("renders an explicit empty state", () => {
    const document = renderShoppingListDocument(
      [], new Date("2026-08-06T10:00:00Z"));
    expect(document).toContain("(nothing on the list)");
  });
});
