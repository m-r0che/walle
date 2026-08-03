export const FACE_AFFECT_VERSION = "walle-transcript-affect-v1";

export const FACE_AFFECTS = [
  "warm",
  "curious",
  "delighted",
  "uncertain",
  "concerned",
] as const;

export type FaceAffect = typeof FACE_AFFECTS[number];

export type FaceAffectIntent = {
  affect: FaceAffect;
  intensity: number;
  ttlMs: number;
};

const TTL_MS = 12_000;

function includesAny(text: string, phrases: readonly string[]): boolean {
  return phrases.some((phrase) => text.includes(phrase));
}

/**
 * Conservatively classify the transcript of Walle's generated speech.
 *
 * This is deliberately bounded and biased toward the warm baseline. It does
 * not infer animation frames or conversational activity, and transcript text
 * is neither retained nor sent to the device.
 */
export function classifyFaceAffect(transcript: string): FaceAffectIntent {
  const text = transcript.toLowerCase()
    .replaceAll("’", "'")
    .slice(0, 2_048);

  if (includesAny(text, [
    "i'm sorry", "i am sorry", "unfortunately", "be careful",
    "that could hurt", "danger", "failed", "went wrong", "a problem",
  ])) {
    return { affect: "concerned", intensity: 62, ttlMs: TTL_MS };
  }
  if (includesAny(text, [
    "i'm not sure", "i am not sure", "maybe", "perhaps", "might be",
    "could be", "it seems", "i think", "probably",
  ])) {
    return { affect: "uncertain", intensity: 50, ttlMs: TTL_MS };
  }
  if (includesAny(text, [
    "wonderful", "lovely", "great news", "glad", "delighted",
    "that's great", "that is great", "perfect",
  ])) {
    return { affect: "delighted", intensity: 68, ttlMs: TTL_MS };
  }
  if (text.includes("?") || includesAny(text, [
    "i wonder", "what do you", "how about", "which one", "tell me more",
  ])) {
    return { affect: "curious", intensity: 55, ttlMs: TTL_MS };
  }
  return { affect: "warm", intensity: 42, ttlMs: TTL_MS };
}
