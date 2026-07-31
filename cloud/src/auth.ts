import { timingSafeEqual } from "node:crypto";

const encoder = new TextEncoder();

function bearerToken(request: Request): string | null {
  const authorization = request.headers.get("Authorization");
  if (authorization === null) return null;
  const match = /^Bearer ([^\s]{16,512})$/i.exec(authorization);
  return match?.[1] ?? null;
}

async function digest(value: string): Promise<ArrayBuffer> {
  return crypto.subtle.digest("SHA-256", encoder.encode(value));
}

/** Compare bearer credentials without data-dependent token comparison. */
export async function isAuthorizedDevice(
  request: Request,
  expectedToken: string,
): Promise<boolean> {
  const suppliedToken = bearerToken(request);
  if (suppliedToken === null || expectedToken.length < 16) return false;
  const [suppliedDigest, expectedDigest] = await Promise.all([
    digest(suppliedToken),
    digest(expectedToken),
  ]);
  return timingSafeEqual(
    new Uint8Array(suppliedDigest),
    new Uint8Array(expectedDigest),
  );
}
