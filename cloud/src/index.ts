import { getAgentByName } from "agents";

import { isAuthorizedDevice } from "./auth";
export { WalleAgent } from "./walle-agent";

type RuntimeEnv = Env & {
  DEVICE_TOKEN: string;
  BRIDGE_TOKEN?: string;
};

const MAX_BRIDGE_BODY_BYTES = 4_096;

function jsonError(status: number, code: string): Response {
  return Response.json({ error: code }, {
    status,
    headers: { "Cache-Control": "no-store" },
  });
}

function jsonOk(value: unknown): Response {
  return Response.json(value, {
    headers: { "Cache-Control": "no-store" },
  });
}

function validInstallation(env: RuntimeEnv): boolean {
  if (/^[a-z0-9][a-z0-9-]{0,62}$/.test(env.INSTALLATION_ID)) return true;
  console.error(JSON.stringify({ event: "configuration.invalid_installation" }));
  return false;
}

function tokenConfigured(token: string | undefined, name: string): boolean {
  if (typeof token === "string" && token.length >= 16) return true;
  console.error(JSON.stringify({ event: `configuration.missing_${name}` }));
  return false;
}

async function walleAgent(env: RuntimeEnv) {
  return getAgentByName(env.WalleAgent, env.INSTALLATION_ID, {
    routingRetry: { maxAttempts: 3 },
  });
}

function forwardWebSocket(
  request: Request,
  role: "device" | "bridge",
  env: RuntimeEnv,
): Promise<Response> | Response {
  const headers = new Headers(request.headers);
  headers.delete("Authorization");
  headers.set("X-Walle-Authenticated", "1");
  headers.set("X-Walle-Installation", env.INSTALLATION_ID);
  headers.set("X-Walle-Role", role);
  return walleAgent(env).then((agent) =>
    agent.fetch(new Request(request, { headers })));
}

export default {
  async fetch(request: Request, env: RuntimeEnv): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/health") {
      return jsonOk({ ok: true, service: "walle-relay" });
    }

    const debugRoutes: Record<string, {
      method: string;
      run: (agent: Awaited<ReturnType<typeof walleAgent>>) =>
        Promise<unknown>;
    }> = {
      "/v1/debug/last-turn": {
        method: "GET",
        run: (agent) => agent.getDebugStatus(),
      },
      "/v1/debug/shopping-list": {
        method: "GET",
        run: (agent) => agent.getShoppingListDebug(),
      },
      "/v1/debug/print-jobs": {
        method: "GET",
        run: (agent) => agent.getPrintJobsDebug(),
      },
      "/v1/debug/disconnect-device": {
        method: "POST",
        run: async (agent) =>
          ({ disconnected: await agent.disconnectDeviceForTest() }),
      },
      "/v1/debug/suppress-pong": {
        method: "POST",
        run: async (agent) =>
          ({ suppressed: await agent.suppressPongsForTest() }),
      },
      "/v1/debug/drop-next-output-frame": {
        method: "POST",
        run: async (agent) =>
          ({ armed: await agent.dropNextOutputFrameForTest() }),
      },
      "/v1/debug/mismatch-next-done-samples": {
        method: "POST",
        run: async (agent) =>
          ({ armed: await agent.mismatchNextDoneSamplesForTest() }),
      },
    };
    const debugRoute = debugRoutes[url.pathname];
    if (url.pathname === "/v1/debug/print-test"
        || debugRoute !== undefined) {
      if (request.method !== (debugRoute?.method ?? "POST")) {
        return jsonError(405, "method_not_allowed");
      }
      if (!tokenConfigured(env.DEVICE_TOKEN, "device_token")) {
        return jsonError(500, "server_misconfigured");
      }
      if (!(await isAuthorizedDevice(request, env.DEVICE_TOKEN))) {
        return jsonError(401, "unauthorized");
      }
      if (!validInstallation(env)) {
        return jsonError(500, "server_misconfigured");
      }
      const agent = await walleAgent(env);
      if (debugRoute !== undefined) {
        return jsonOk(await debugRoute.run(agent));
      }
      const body = await request.text();
      if (body.length > 220_000) {
        return jsonError(413, "payload_too_large");
      }
      let parsed: unknown = {};
      try {
        parsed = body.length === 0 ? {} : JSON.parse(body);
      } catch {
        return jsonError(400, "invalid_json");
      }
      return jsonOk(await agent.printTestForBringup(parsed));
    }

    // The local print bridge claims and acknowledges jobs over
    // authenticated HTTPS; its WebSocket is only a wake-up hint.
    if (url.pathname === "/v1/bridge"
        || url.pathname === "/v1/bridge/claim"
        || url.pathname === "/v1/bridge/ack") {
      if (!tokenConfigured(env.BRIDGE_TOKEN, "bridge_token")) {
        return jsonError(500, "server_misconfigured");
      }
      if (!(await isAuthorizedDevice(request, env.BRIDGE_TOKEN as string))) {
        return jsonError(401, "unauthorized");
      }
      if (!validInstallation(env)) {
        return jsonError(500, "server_misconfigured");
      }
      if (url.pathname === "/v1/bridge") {
        if (request.method !== "GET") {
          return jsonError(405, "method_not_allowed");
        }
        if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
          return jsonError(426, "websocket_upgrade_required");
        }
        return forwardWebSocket(request, "bridge", env);
      }
      if (request.method !== "POST") {
        return jsonError(405, "method_not_allowed");
      }
      const agent = await walleAgent(env);
      if (url.pathname === "/v1/bridge/claim") {
        return jsonOk({ job: await agent.claimPrintJob() });
      }
      const body = await request.text();
      if (body.length > MAX_BRIDGE_BODY_BYTES) {
        return jsonError(413, "payload_too_large");
      }
      let parsed: unknown;
      try {
        parsed = JSON.parse(body);
      } catch {
        return jsonError(400, "invalid_json");
      }
      return jsonOk(await agent.ackPrintJob(parsed));
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
    if (!tokenConfigured(env.DEVICE_TOKEN, "device_token")) {
      return jsonError(500, "server_misconfigured");
    }
    if (!(await isAuthorizedDevice(request, env.DEVICE_TOKEN))) {
      return jsonError(401, "unauthorized");
    }
    if (!validInstallation(env)) {
      return jsonError(500, "server_misconfigured");
    }
    return forwardWebSocket(request, "device", env);
  },
} satisfies ExportedHandler<RuntimeEnv>;
