import { describe, expect, it } from "vitest";

import { isAuthorizedDevice } from "../src/auth";

const token = "test-device-token-with-enough-entropy";

describe("device bearer authentication", () => {
  it("accepts the configured bearer token", async () => {
    const request = new Request("https://relay.test/v1/device", {
      headers: { Authorization: `Bearer ${token}` },
    });
    await expect(isAuthorizedDevice(request, token)).resolves.toBe(true);
  });

  it("rejects absent, malformed, and incorrect credentials", async () => {
    await expect(isAuthorizedDevice(
      new Request("https://relay.test/v1/device"),
      token,
    )).resolves.toBe(false);
    await expect(isAuthorizedDevice(
      new Request("https://relay.test/v1/device", {
        headers: { Authorization: "Basic nope" },
      }),
      token,
    )).resolves.toBe(false);
    await expect(isAuthorizedDevice(
      new Request("https://relay.test/v1/device", {
        headers: { Authorization: "Bearer another-long-device-token" },
      }),
      token,
    )).resolves.toBe(false);
  });
});
