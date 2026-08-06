export const SHOPPING_LIST_VERSION = "walle-shopping-v1";

export const MAX_ACTIVE_ITEMS = 200;
export const MAX_ITEMS_PER_ADD = 12;
export const MAX_ITEM_NAME_CHARS = 80;
export const MAX_QUANTITY_CHARS = 24;

export type ShoppingItemInput = {
  name: string;
  quantity: string | null;
};

export type ShoppingItem = {
  id: number;
  name: string;
  quantity: string | null;
  addedAt: number;
};

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null
    ? value as Record<string, unknown>
    : null;
}

function normalizeText(value: unknown, maxChars: number): string | null {
  if (typeof value !== "string") return null;
  const collapsed = value.replace(/\s+/g, " ").trim();
  if (collapsed.length === 0 || collapsed.length > maxChars) return null;
  return collapsed;
}

/**
 * Validate the model-supplied items for `shopping_list.add`. Model output is
 * untrusted data: names/quantities are bounded, whitespace-collapsed strings
 * and the item count is capped per mutation.
 */
export function parseItemInputs(value: unknown): ShoppingItemInput[] {
  if (!Array.isArray(value) || value.length === 0
      || value.length > MAX_ITEMS_PER_ADD) {
    throw new Error(
      `items must contain 1-${MAX_ITEMS_PER_ADD} entries`);
  }
  return value.map((entry) => {
    const item = record(entry);
    const name = normalizeText(item?.name, MAX_ITEM_NAME_CHARS);
    if (name === null) {
      throw new Error(
        `each item needs a name of 1-${MAX_ITEM_NAME_CHARS} characters`);
    }
    let quantity: string | null = null;
    if (item !== null && item.quantity !== undefined
        && item.quantity !== null) {
      quantity = normalizeText(item.quantity, MAX_QUANTITY_CHARS);
      if (quantity === null) {
        throw new Error(
          `quantities are limited to ${MAX_QUANTITY_CHARS} characters`);
      }
    }
    return { name, quantity };
  });
}

/** Render the list as a plain-text printable document. */
export function renderShoppingListDocument(
  items: readonly ShoppingItem[],
  renderedAt: Date,
): string {
  const date = renderedAt.toISOString().slice(0, 10);
  const lines = [
    "SHOPPING LIST",
    date,
    "",
  ];
  if (items.length === 0) {
    lines.push("(nothing on the list)");
  }
  for (const item of items) {
    const quantity = item.quantity === null ? "" : `  (${item.quantity})`;
    lines.push(`[ ] ${item.name}${quantity}`);
  }
  lines.push("", "- Walle");
  return lines.join("\n");
}
