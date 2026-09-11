import {
  MAX_AUDIO_SAMPLES,
  MAX_INPUT_SAMPLES,
  SAMPLE_RATE_HZ,
} from "./protocol";

declare const brand: unique symbol;
type Brand<T, B extends string> = T & { readonly [brand]: B };

export type Instant = Brand<number, "Instant">;
export type Millis = Brand<number, "Millis">;

export const at = (epochMs: number): Instant => epochMs as Instant;
export const ms = (duration: number): Millis => duration as Millis;
const after = (now: Instant, delay: Millis): Instant => at(now + delay);

const DEVICE_FRAME_BYTES = MAX_AUDIO_SAMPLES * 2;
export const MAX_OUTPUT_SAMPLES = SAMPLE_RATE_HZ * 28;

export type LivePolicy = {
  readonly readyMs: Millis;
  readonly firstAudioMs: Millis;
  readonly audioQuietMs: Millis;
  readonly transcriptSettleMs: Millis;
  readonly orphanQuietMs: Millis;
  readonly orphanMaxMs: Millis;
  readonly appendChunkSamples: number;
  readonly maxTranscriptChars: number;
  readonly maxInputSamples: number;
  readonly maxOutputSamples: number;
};

export const LIVE_POLICY: LivePolicy = {
  readyMs: ms(10_000),
  firstAudioMs: ms(6_000),
  audioQuietMs: ms(1_200),
  transcriptSettleMs: ms(400),
  orphanQuietMs: ms(1_200),
  orphanMaxMs: ms(20_000),
  appendChunkSamples: MAX_AUDIO_SAMPLES,
  maxTranscriptChars: 2_048,
  maxInputSamples: MAX_INPUT_SAMPLES,
  maxOutputSamples: MAX_OUTPUT_SAMPLES,
};

export type SessionState =
  | { readonly phase: "starting"; readonly readyBy: Instant }
  | { readonly phase: "live"; readonly capture: Capture; readonly stream: Stream }
  | { readonly phase: "gone"; readonly fault: Fault };

type Live = Extract<SessionState, { phase: "live" }>;

export type Capture =
  | { readonly kind: "open" }
  | { readonly kind: "holding"; readonly turnId: string; readonly held: HeldUtterance }
  | { readonly kind: "sealed"; readonly turnId: string; readonly held: HeldUtterance };

export type HeldUtterance = {
  readonly frames: readonly Uint8Array[];
  readonly samples: number;
};

export type Stream =
  | { readonly kind: "quiet" }
  | { readonly kind: "awaiting"; readonly claim: Claim; readonly firstAudioBy: Instant }
  | {
    readonly kind: "voicing";
    readonly claim: Claim;
    readonly receivedBytes: number;
    readonly remainder: Uint8Array;
    readonly sealBy: Instant;
  }
  | {
    readonly kind: "orphaned";
    readonly quietBy: Instant;
    readonly abandonBy: Instant;
    readonly discardedBytes: number;
  };

export type Claim = { readonly turnId: string; readonly affect: Affect };

export type Affect =
  | { readonly kind: "silent" }
  | { readonly kind: "settling"; readonly text: string; readonly settleBy: Instant }
  | { readonly kind: "sent" };

export type LiveEvent =
  | { readonly kind: "ready" }
  | { readonly kind: "output-audio"; readonly pcm: Uint8Array }
  | { readonly kind: "output-text"; readonly delta: string }
  | { readonly kind: "input-text"; readonly delta: string }
  | { readonly kind: "error"; readonly fault: Fault }
  | { readonly kind: "closed"; readonly fault: Fault }
  | { readonly kind: "ignored"; readonly type: string };

export type Input =
  | { readonly kind: "begin"; readonly turnId: string }
  | { readonly kind: "append"; readonly turnId: string; readonly pcm: Uint8Array }
  | { readonly kind: "commit"; readonly turnId: string }
  | { readonly kind: "cancel"; readonly turnId: string }
  | { readonly kind: "shutdown" }
  | { readonly kind: "upstream"; readonly event: LiveEvent }
  | { readonly kind: "tick" };

export type StreamIntent =
  | { readonly kind: "start" }
  | { readonly kind: "appendInput"; readonly pcm: Uint8Array }
  | { readonly kind: "mute" }
  | { readonly kind: "unmute" };

export type Note =
  | { readonly kind: "orphan-drained"; readonly bytes: number }
  | { readonly kind: "input-heard"; readonly chars: number }
  | { readonly kind: "upstream-error"; readonly reason: string }
  | { readonly kind: "upstream-ignored"; readonly type: string };

export type Effect =
  | { readonly kind: "send"; readonly intent: StreamIntent }
  | { readonly kind: "speak"; readonly turnId: string; readonly frames: readonly Uint8Array[] }
  | { readonly kind: "affect"; readonly turnId: string; readonly transcript: string }
  | {
    readonly kind: "finish";
    readonly turnId: string;
    readonly tail: Uint8Array | null;
    readonly outputSamples: number;
  }
  | { readonly kind: "fault"; readonly turnId: string; readonly fault: Fault }
  | { readonly kind: "ready" }
  | { readonly kind: "lost"; readonly fault: Fault }
  | { readonly kind: "observe"; readonly note: Note };

export type Refusal =
  | "session_not_live"
  | "turn_already_held"
  | "turn_not_held"
  | "input_frame_invalid"
  | "input_limit_exceeded"
  | "empty_utterance";

export type Outcome =
  | { readonly ok: true; readonly state: SessionState; readonly effects: readonly Effect[] }
  | { readonly ok: false; readonly refusal: Refusal };

export type FaultCode =
  | "ready_timeout"
  | "no_upstream_audio"
  | "output_overflow"
  | "superseded"
  | "upstream_error"
  | "upstream_malformed"
  | "upstream_closed"
  | "upstream_runaway"
  | "session_closed";

export type Fault = { readonly code: FaultCode; readonly detail?: string };

export function renderFault(fault: Fault): string {
  return fault.detail === undefined ? fault.code : `${fault.code}:${fault.detail}`;
}

const OPEN: Capture = { kind: "open" };
const QUIET: Stream = { kind: "quiet" };
const SILENT: Affect = { kind: "silent" };
const SENT: Affect = { kind: "sent" };
const NOTHING = new Uint8Array(0);

const accept = (state: SessionState, effects: readonly Effect[] = []): Outcome =>
  ({ ok: true, state, effects });
const refuse = (refusal: Refusal): Outcome => ({ ok: false, refusal });
const send = (intent: StreamIntent): Effect => ({ kind: "send", intent });
const faultOf = (turnId: string, fault: Fault): Effect => ({ kind: "fault", turnId, fault });
const observe = (note: Note): Effect => ({ kind: "observe", note });

export function initialState(now: Instant, policy: LivePolicy): SessionState {
  return { phase: "starting", readyBy: after(now, policy.readyMs) };
}

/*
 * Capture (device inputs). The stream is untouched except where noted.
 *
 *             open              holding(t)          sealed(t)
 *   begin(u)  holding(u)        refuse held         holding(u), fault(t, superseded)
 *   append(t) refuse not_held   holding(t) + frame  refuse not_held
 *   commit(t) refuse not_held   sealed(t) *         refuse not_held
 *   cancel(t) no-op             open                open
 *
 *   * sealed(t) with a quiet stream releases at once: capture open, stream
 *     awaiting(t), send unmute, appendInput per chunk, mute. begin(u) also
 *     orphans an awaiting or voicing stream with fault(claim, superseded).
 *
 * Stream (upstream events and the clock). Capture is untouched except that
 * closed and shutdown fault a held or sealed turn as well.
 *
 *             quiet        awaiting(t)              voicing(t)                 orphaned
 *   audio     orphaned     voicing(t), speak        voicing(t), speak          quietBy reset
 *   text      no-op        settling                 settling, sealBy reset     no-op
 *   error     no-op        orphaned, fault(t)       orphaned, fault(t)         no-op
 *   cancel(t) no-op        orphaned                 orphaned                   no-op
 *   tick      no-op        settleBy: affect         settleBy: affect           quietBy: quiet
 *                          firstAudioBy: orphaned,  sealBy: affect if still    abandonBy: gone,
 *                          fault(t, no_upstream_    settling, finish, quiet    lost(runaway)
 *                          audio)
 *   closed    gone, lost   gone, fault(t), lost     gone, fault(t), lost       gone, lost
 *   shutdown  gone         gone, fault(t)           gone, fault(t)             gone
 *
 * Before ready, begin, append, and commit refuse session_not_live; ready,
 * error, closed, shutdown, and the readyBy tick move to live or gone. After
 * gone, the same three refuse and every other input is a no-op.
 */
export function reduce(
  policy: LivePolicy,
  state: SessionState,
  input: Input,
  now: Instant,
): Outcome {
  switch (state.phase) {
    case "starting":
      return reduceStarting(state, input, now);
    case "live":
      return reduceLive(policy, state, input, now);
    case "gone":
      return isDeviceTurnInput(input) ? refuse("session_not_live") : accept(state);
  }
}

export function nextDeadline(state: SessionState): Instant | null {
  switch (state.phase) {
    case "starting":
      return state.readyBy;
    case "gone":
      return null;
    case "live": {
      const stream = state.stream;
      switch (stream.kind) {
        case "quiet":
          return null;
        case "awaiting":
          return earliest(stream.firstAudioBy, settleBy(stream.claim));
        case "voicing":
          return earliest(stream.sealBy, settleBy(stream.claim));
        case "orphaned":
          return earliest(stream.quietBy, stream.abandonBy);
      }
    }
  }
}

const settleBy = (claim: Claim): Instant | null =>
  claim.affect.kind === "settling" ? claim.affect.settleBy : null;
const earliest = (a: Instant, b: Instant | null): Instant => b === null || a <= b ? a : b;

const isDeviceTurnInput = (input: Input): boolean =>
  input.kind === "begin" || input.kind === "append" || input.kind === "commit";

function lost(fault: Fault): Outcome {
  return accept({ phase: "gone", fault }, [{ kind: "lost", fault }]);
}

function reduceStarting(
  state: Extract<SessionState, { phase: "starting" }>,
  input: Input,
  now: Instant,
): Outcome {
  switch (input.kind) {
    case "upstream":
      switch (input.event.kind) {
        case "ready":
          return accept({ phase: "live", capture: OPEN, stream: QUIET }, [{ kind: "ready" }]);
        case "error":
        case "closed":
          return lost(input.event.fault);
        default:
          return accept(state);
      }
    case "tick":
      return now >= state.readyBy ? lost({ code: "ready_timeout" }) : accept(state);
    case "shutdown":
      return lost({ code: "session_closed" });
    case "cancel":
      return accept(state);
    case "begin":
    case "append":
    case "commit":
      return refuse("session_not_live");
  }
}

function reduceLive(policy: LivePolicy, live: Live, input: Input, now: Instant): Outcome {
  const outcome = step(policy, live, input, now);
  if (!outcome.ok || outcome.state.phase !== "live") return outcome;
  return release(policy, outcome.state, outcome.effects, now);
}

function step(policy: LivePolicy, live: Live, input: Input, now: Instant): Outcome {
  switch (input.kind) {
    case "begin":
      return begin(policy, live, input.turnId, now);
    case "append":
      return append(policy, live, input.turnId, input.pcm);
    case "commit":
      return commit(live, input.turnId);
    case "cancel":
      return accept(cancel(policy, live, input.turnId, now));
    case "shutdown":
      return abandon(live, { code: "session_closed" }, false);
    case "upstream":
      return upstream(policy, live, input.event, now);
    case "tick":
      return tick(policy, live, now);
  }
}

function begin(policy: LivePolicy, live: Live, turnId: string, now: Instant): Outcome {
  if (live.capture.kind === "holding") return refuse("turn_already_held");
  const effects: Effect[] = [];
  if (live.capture.kind === "sealed") {
    effects.push(faultOf(live.capture.turnId, { code: "superseded" }));
  }
  let stream = live.stream;
  if (stream.kind === "awaiting" || stream.kind === "voicing") {
    effects.push(faultOf(stream.claim.turnId, { code: "superseded" }));
    stream = orphan(policy, now, 0);
  }
  const held: HeldUtterance = { frames: [], samples: 0 };
  return accept({ phase: "live", capture: { kind: "holding", turnId, held }, stream }, effects);
}

function append(policy: LivePolicy, live: Live, turnId: string, pcm: Uint8Array): Outcome {
  const capture = live.capture;
  if (capture.kind !== "holding" || capture.turnId !== turnId) return refuse("turn_not_held");
  if (pcm.byteLength === 0 || pcm.byteLength % 2 !== 0
      || pcm.byteLength > DEVICE_FRAME_BYTES) {
    return refuse("input_frame_invalid");
  }
  const samples = capture.held.samples + pcm.byteLength / 2;
  if (samples > policy.maxInputSamples) return refuse("input_limit_exceeded");
  const held: HeldUtterance = { frames: [...capture.held.frames, pcm], samples };
  return accept({ ...live, capture: { ...capture, held } });
}

function commit(live: Live, turnId: string): Outcome {
  const capture = live.capture;
  if (capture.kind !== "holding" || capture.turnId !== turnId) return refuse("turn_not_held");
  if (capture.held.samples === 0) return refuse("empty_utterance");
  return accept({ ...live, capture: { ...capture, kind: "sealed" } });
}

function release(policy: LivePolicy, live: Live, effects: readonly Effect[], now: Instant): Outcome {
  if (live.capture.kind !== "sealed" || live.stream.kind !== "quiet") return accept(live, effects);
  const { turnId, held } = live.capture;
  const utterance = join(held.frames, held.samples * 2);
  const { whole, rest } = slice(utterance, policy.appendChunkSamples * 2);
  const chunks = rest.byteLength === 0 ? whole : [...whole, rest];
  const claim: Claim = { turnId, affect: SILENT };
  return accept(
    {
      phase: "live",
      capture: OPEN,
      stream: { kind: "awaiting", claim, firstAudioBy: after(now, policy.firstAudioMs) },
    },
    [
      ...effects,
      send({ kind: "unmute" }),
      ...chunks.map((pcm) => send({ kind: "appendInput", pcm })),
      send({ kind: "mute" }),
    ],
  );
}

function cancel(policy: LivePolicy, live: Live, turnId: string, now: Instant): Live {
  const capture = live.capture.kind !== "open" && live.capture.turnId === turnId
    ? OPEN
    : live.capture;
  const stream = claimOf(live.stream)?.turnId === turnId
    ? orphan(policy, now, 0)
    : live.stream;
  return { phase: "live", capture, stream };
}

function abandon(live: Live, fault: Fault, unavailable: boolean): Outcome {
  const effects: Effect[] = [];
  if (live.capture.kind !== "open") effects.push(faultOf(live.capture.turnId, fault));
  const claim = claimOf(live.stream);
  if (claim !== null) effects.push(faultOf(claim.turnId, fault));
  if (unavailable) effects.push({ kind: "lost", fault });
  return accept({ phase: "gone", fault }, effects);
}

function upstream(policy: LivePolicy, live: Live, event: LiveEvent, now: Instant): Outcome {
  switch (event.kind) {
    case "ready":
      return accept(live);
    case "output-audio":
      return hear(policy, live, event.pcm, now);
    case "output-text":
      return accept({ ...live, stream: transcribe(policy, live.stream, event.delta, now) });
    case "input-text":
      return accept(live, [observe({ kind: "input-heard", chars: event.delta.length })]);
    case "ignored":
      return accept(live, [observe({ kind: "upstream-ignored", type: event.type })]);
    case "error": {
      const effects = [observe({ kind: "upstream-error", reason: renderFault(event.fault) })];
      const claim = claimOf(live.stream);
      if (claim === null) return accept(live, effects);
      return accept(
        { ...live, stream: orphan(policy, now, 0) },
        [...effects, faultOf(claim.turnId, event.fault)],
      );
    }
    case "closed":
      return abandon(live, event.fault, true);
  }
}

function hear(policy: LivePolicy, live: Live, pcm: Uint8Array, now: Instant): Outcome {
  const stream = live.stream;
  switch (stream.kind) {
    case "quiet":
      return accept({ ...live, stream: orphan(policy, now, pcm.byteLength) });
    case "orphaned":
      return accept({
        ...live,
        stream: {
          ...stream,
          quietBy: after(now, policy.orphanQuietMs),
          discardedBytes: stream.discardedBytes + pcm.byteLength,
        },
      });
    case "awaiting":
      return voice(policy, live, stream.claim, 0, NOTHING, pcm, now);
    case "voicing":
      return voice(policy, live, stream.claim, stream.receivedBytes, stream.remainder, pcm, now);
  }
}

function voice(
  policy: LivePolicy,
  live: Live,
  claim: Claim,
  receivedBytes: number,
  remainder: Uint8Array,
  pcm: Uint8Array,
  now: Instant,
): Outcome {
  const total = receivedBytes + pcm.byteLength;
  if (total > policy.maxOutputSamples * 2) {
    return accept(
      { ...live, stream: orphan(policy, now, pcm.byteLength) },
      [faultOf(claim.turnId, { code: "output_overflow" })],
    );
  }
  const { whole, rest } = slice(
    join([remainder, pcm], remainder.byteLength + pcm.byteLength),
    DEVICE_FRAME_BYTES,
  );
  return accept(
    {
      ...live,
      stream: {
        kind: "voicing",
        claim,
        receivedBytes: total,
        remainder: rest,
        sealBy: after(now, policy.audioQuietMs),
      },
    },
    whole.length === 0 ? [] : [{ kind: "speak", turnId: claim.turnId, frames: whole }],
  );
}

function transcribe(policy: LivePolicy, stream: Stream, delta: string, now: Instant): Stream {
  if (stream.kind !== "awaiting" && stream.kind !== "voicing") return stream;
  const affect = stream.claim.affect;
  if (affect.kind === "sent") return stream;
  const text = (affect.kind === "settling" ? affect.text + delta : delta)
    .slice(0, policy.maxTranscriptChars);
  const claim: Claim = {
    ...stream.claim,
    affect: { kind: "settling", text, settleBy: after(now, policy.transcriptSettleMs) },
  };
  return stream.kind === "voicing"
    ? { ...stream, claim, sealBy: after(now, policy.audioQuietMs) }
    : { ...stream, claim };
}

function tick(policy: LivePolicy, live: Live, now: Instant): Outcome {
  const stream = live.stream;
  switch (stream.kind) {
    case "quiet":
      return accept(live);
    case "awaiting": {
      if (now >= stream.firstAudioBy) {
        return accept(
          { ...live, stream: orphan(policy, now, 0) },
          [faultOf(stream.claim.turnId, { code: "no_upstream_audio" })],
        );
      }
      const settled = settle(stream.claim, now);
      return settled === null
        ? accept(live)
        : accept({ ...live, stream: { ...stream, claim: settled.claim } }, [settled.effect]);
    }
    case "voicing": {
      if (now >= stream.sealBy) {
        const { turnId, affect } = stream.claim;
        const effects: Effect[] = affect.kind === "settling"
          ? [{ kind: "affect", turnId, transcript: affect.text }]
          : [];
        effects.push({
          kind: "finish",
          turnId,
          tail: stream.remainder.byteLength === 0 ? null : stream.remainder,
          outputSamples: stream.receivedBytes / 2,
        });
        return accept({ ...live, stream: QUIET }, effects);
      }
      const settled = settle(stream.claim, now);
      return settled === null
        ? accept(live)
        : accept({ ...live, stream: { ...stream, claim: settled.claim } }, [settled.effect]);
    }
    case "orphaned":
      if (now >= stream.abandonBy) return abandon(live, { code: "upstream_runaway" }, true);
      if (now >= stream.quietBy) {
        return accept(
          { ...live, stream: QUIET },
          [observe({ kind: "orphan-drained", bytes: stream.discardedBytes })],
        );
      }
      return accept(live);
  }
}

function settle(claim: Claim, now: Instant): { claim: Claim; effect: Effect } | null {
  const affect = claim.affect;
  if (affect.kind !== "settling" || now < affect.settleBy) return null;
  return {
    claim: { ...claim, affect: SENT },
    effect: { kind: "affect", turnId: claim.turnId, transcript: affect.text },
  };
}

const claimOf = (stream: Stream): Claim | null =>
  stream.kind === "awaiting" || stream.kind === "voicing" ? stream.claim : null;

function orphan(policy: LivePolicy, now: Instant, discardedBytes: number): Stream {
  return {
    kind: "orphaned",
    quietBy: after(now, policy.orphanQuietMs),
    abandonBy: after(now, policy.orphanMaxMs),
    discardedBytes,
  };
}

function join(parts: readonly Uint8Array[], byteLength: number): Uint8Array {
  const out = new Uint8Array(byteLength);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

function slice(
  bytes: Uint8Array,
  size: number,
): { readonly whole: readonly Uint8Array[]; readonly rest: Uint8Array } {
  const whole: Uint8Array[] = [];
  let offset = 0;
  for (; offset + size <= bytes.byteLength; offset += size) {
    whole.push(bytes.subarray(offset, offset + size));
  }
  return { whole, rest: bytes.subarray(offset) };
}
