import { getAgentByName } from "agents";

import { isAuthorizedDevice } from "./auth";
export { WalleAgent } from "./walle-agent";

type RuntimeEnv = Env & {
  DEVICE_TOKEN: string;
};

function jsonError(status: number, code: string): Response {
  return Response.json({ error: code }, {
    status,
    headers: { "Cache-Control": "no-store" },
  });
}

export default {
  async fetch(request: Request, env: RuntimeEnv): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/health") {
      return Response.json({ ok: true, service: "walle-relay" }, {
        headers: { "Cache-Control": "no-store" },
      });
    }

    const debugStatus = url.pathname === "/v1/debug/last-turn";
    const debugDisconnect =
      url.pathname === "/v1/debug/disconnect-device";
    const debugSuppressPong =
      url.pathname === "/v1/debug/suppress-pong";
    const debugDropOutput =
      url.pathname === "/v1/debug/drop-next-output-frame";
    const debugMismatchDone =
      url.pathname === "/v1/debug/mismatch-next-done-samples";
    if (debugStatus || debugDisconnect || debugSuppressPong
        || debugDropOutput || debugMismatchDone) {
      const expectedMethod = debugStatus ? "GET" : "POST";
      if (request.method !== expectedMethod) {
        return jsonError(405, "method_not_allowed");
      }
      if (typeof env.DEVICE_TOKEN !== "string" || env.DEVICE_TOKEN.length < 16) {
        console.error(JSON.stringify({ event: "configuration.missing_device_token" }));
        return jsonError(500, "server_misconfigured");
      }
      if (!(await isAuthorizedDevice(request, env.DEVICE_TOKEN))) {
        return jsonError(401, "unauthorized");
      }
      if (!/^[a-z0-9][a-z0-9-]{0,62}$/.test(env.INSTALLATION_ID)) {
        console.error(JSON.stringify({ event: "configuration.invalid_installation" }));
        return jsonError(500, "server_misconfigured");
      }
      const agent = await getAgentByName(env.WalleAgent, env.INSTALLATION_ID, {
        routingRetry: { maxAttempts: 3 },
      });
      let result: unknown;
      if (debugStatus) {
        result = await agent.getDebugStatus();
      } else if (debugDisconnect) {
        result = { disconnected: await agent.disconnectDeviceForTest() };
      } else if (debugSuppressPong) {
        result = { suppressed: await agent.suppressPongsForTest() };
      } else if (debugDropOutput) {
        result = { armed: await agent.dropNextOutputFrameForTest() };
      } else {
        result = { armed: await agent.mismatchNextDoneSamplesForTest() };
      }
      return Response.json(result, {
        headers: { "Cache-Control": "no-store" },
      });
    }

    if (url.pathname !== "/v1/device") {
      return jsonError(404, "not_found");
    }
    if (request.method !== "GET") {
      return jsonError(405, "method_not_allowed");
    }
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return jsonError(426, "websocket_upgrade_required");
    }
    if (typeof env.DEVICE_TOKEN !== "string" || env.DEVICE_TOKEN.length < 16) {
      console.error(JSON.stringify({ event: "configuration.missing_device_token" }));
      return jsonError(500, "server_misconfigured");
    }
    if (!(await isAuthorizedDevice(request, env.DEVICE_TOKEN))) {
      return jsonError(401, "unauthorized");
    }
    if (!/^[a-z0-9][a-z0-9-]{0,62}$/.test(env.INSTALLATION_ID)) {
      console.error(JSON.stringify({ event: "configuration.invalid_installation" }));
      return jsonError(500, "server_misconfigured");
    }

    const agent = await getAgentByName(env.WalleAgent, env.INSTALLATION_ID, {
      routingRetry: { maxAttempts: 3 },
    });
    const headers = new Headers(request.headers);
    headers.delete("Authorization");
    headers.set("X-Walle-Authenticated", "1");
    headers.set("X-Walle-Installation", env.INSTALLATION_ID);
    return agent.fetch(new Request(request, { headers }));
  },
} satisfies ExportedHandler<RuntimeEnv>;
