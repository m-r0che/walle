import { describe, expect, it } from "vitest";

import {
  at,
  type Effect,
  initialState,
  type Input,
  LIVE_POLICY,
  type LivePolicy,
  nextDeadline,
  reduce,
  renderFault,
  type SessionState,
} from "../src/live-turn";

type Step = readonly [atMs: number, input: Input];

function pcm(bytes: number, seed = 0): Uint8Array {
  const out = new Uint8Array(bytes);
  for (let index = 0; index < bytes; index++) out[index] = (index + seed) & 0xff;
  return out;
}

const ready: Input = { kind: "upstream", event: { kind: "ready" } };
const tick: Input = { kind: "tick" };
const shutdown: Input = { kind: "shutdown" };
const begin = (turnId: string): Input => ({ kind: "begin", turnId });
const append = (turnId: string, bytes = 1_920, seed = 0): Input =>
  ({ kind: "append", turnId, pcm: pcm(bytes, seed) });
const commit = (turnId: string): Input => ({ kind: "commit", turnId });
const cancel = (turnId: string): Input => ({ kind: "cancel", turnId });
const audio = (bytes: number, seed = 0): Input =>
  ({ kind: "upstream", event: { kind: "output-audio", pcm: pcm(bytes, seed) } });
const text = (delta: string): Input =>
  ({ kind: "upstream", event: { kind: "output-text", delta } });
const heard = (delta: string): Input =>
  ({ kind: "upstream", event: { kind: "input-text", delta } });
const ignored = (type: string): Input => ({ kind: "upstream", event: { kind: "ignored", type } });
const upstreamError = (detail: string): Input =>
  ({ kind: "upstream", event: { kind: "error", fault: { code: "upstream_error", detail } } });
const closed = (detail: string): Input =>
  ({ kind: "upstream", event: { kind: "closed", fault: { code: "upstream_closed", detail } } });

const spoken = (turnId: string) => [
  [10, begin(turnId)],
  [20, append(turnId)],
  [100, commit(turnId)],
] as const satisfies readonly Step[];

function show(effect: Effect): string {
  switch (effect.kind) {
    case "send":
      return effect.intent.kind === "appendInput"
        ? `append ${effect.intent.pcm.byteLength / 2}`
        : effect.intent.kind;
    case "speak":
      return `speak ${effect.turnId} ${effect.frames.map((frame) => frame.byteLength).join(",")}`;
    case "affect":
      return `affect ${effect.turnId} ${effect.transcript}`;
    case "finish":
      return `finish ${effect.turnId} ${effect.outputSamples} tail=${effect.tail?.byteLength ?? 0}`;
    case "fault":
      return `fault ${effect.turnId} ${renderFault(effect.fault)}`;
    case "ready":
      return "ready";
    case "lost":
      return `lost ${renderFault(effect.fault)}`;
    case "observe":
      return `observe ${JSON.stringify(effect.note)}`;
  }
}

type Run = {
  state: SessionState;
  raw: Effect[];
  effects: string[];
  refusals: string[];
};

function drive(steps: readonly Step[], policy: LivePolicy = LIVE_POLICY): Run {
  let state = initialState(at(0), policy);
  const raw: Effect[] = [];
  const effects: string[] = [];
  const refusals: string[] = [];
  for (const [atMs, input] of steps) {
    const outcome = reduce(policy, state, input, at(atMs));
    if (!outcome.ok) {
      refusals.push(`${atMs} ${outcome.refusal}`);
      continue;
    }
    state = outcome.state;
    raw.push(...outcome.effects);
    effects.push(...outcome.effects.map((effect) => `${atMs} ${show(effect)}`));
  }
  return { state, raw, effects, refusals };
}

function concat(parts: readonly Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((total, part) => total + part.byteLength, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

type Case = {
  name: string;
  policy?: LivePolicy;
  steps: readonly Step[];
  effects: string[];
  refusals?: string[];
  phase: SessionState["phase"];
  deadline: number | null;
};

const burst = (atMs: number, samples = 960) =>
  [`${atMs} unmute`, `${atMs} append ${samples}`, `${atMs} mute`];

const cases: Case[] = [
  {
    name: "hold, commit on a quiet stream, burst, deltas, then finish after the quiet gap",
    steps: [
      [0, ready],
      [10, begin("t1")],
      [20, append("t1")],
      [60, append("t1")],
      [900, commit("t1")],
      [1_000, audio(3_000)],
      [1_100, audio(1_320)],
      [2_299, tick],
      [2_300, tick],
    ],
    effects: [
      "0 ready",
      "900 unmute",
      "900 append 960",
      "900 append 960",
      "900 mute",
      "1000 speak t1 1920",
      "1100 speak t1 1920",
      "2300 finish t1 2160 tail=480",
    ],
    phase: "live",
    deadline: null,
  },
  {
    name: "short deltas coalesce into device frames with the remainder carried",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_000)],
      [300, audio(1_000)],
      [400, audio(1_000)],
      [1_600, tick],
    ],
    effects: ["0 ready", ...burst(100), "300 speak t1 1920", "1600 finish t1 1500 tail=1080"],
    phase: "live",
    deadline: null,
  },
  {
    name: "cancel mid-hold sends nothing and the next hold bursts alone",
    steps: [
      [0, ready],
      [10, begin("t1")],
      [20, append("t1")],
      [30, cancel("t1")],
      [40, begin("t2")],
      [50, append("t2", 960)],
      [60, commit("t2")],
    ],
    effects: ["0 ready", ...burst(60, 480)],
    phase: "live",
    deadline: 6_060,
  },
  {
    name: "cancel after commit orphans later deltas and the next commit waits for quiet",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, cancel("t1")],
      [400, audio(1_920)],
      [500, begin("t2")],
      [520, append("t2", 960)],
      [600, commit("t2")],
      [1_599, tick],
      [1_600, tick],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      '1600 observe {"kind":"orphan-drained","bytes":1920}',
      ...burst(1_600, 480),
    ],
    phase: "live",
    deadline: 7_600,
  },
  {
    name: "begin during voicing supersedes the old turn and drains its tail",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, begin("t2")],
      [400, audio(1_920)],
      [410, append("t2")],
      [500, commit("t2")],
      [1_600, tick],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "300 fault t1 superseded",
      '1600 observe {"kind":"orphan-drained","bytes":1920}',
      ...burst(1_600),
    ],
    phase: "live",
    deadline: 7_600,
  },
  {
    name: "begin during awaiting supersedes the old turn",
    steps: [[0, ready], ...spoken("t1"), [200, begin("t2")]],
    effects: ["0 ready", ...burst(100), "200 fault t1 superseded"],
    phase: "live",
    deadline: 1_400,
  },
  {
    name: "begin while a commit is sealed supersedes the unsent turn",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, cancel("t1")],
      [400, begin("t2")],
      [410, append("t2", 960)],
      [500, commit("t2")],
      [600, begin("t3")],
      [610, append("t3")],
      [700, commit("t3")],
      [1_500, tick],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "600 fault t2 superseded",
      '1500 observe {"kind":"orphan-drained","bytes":0}',
      ...burst(1_500),
    ],
    phase: "live",
    deadline: 7_500,
  },
  {
    name: "a late delta after finish is orphaned and never reaches the next turn",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [1_400, tick],
      [1_500, begin("t2")],
      [1_600, audio(1_920)],
      [1_610, append("t2")],
      [1_700, commit("t2")],
      [2_800, tick],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "1400 finish t1 960 tail=0",
      '2800 observe {"kind":"orphan-drained","bytes":1920}',
      ...burst(2_800),
    ],
    phase: "live",
    deadline: 8_800,
  },
  {
    name: "no output within firstAudioMs fails the turn and drains",
    steps: [[0, ready], ...spoken("t1"), [6_099, tick], [6_100, tick]],
    effects: ["0 ready", ...burst(100), "6100 fault t1 no_upstream_audio"],
    phase: "live",
    deadline: 7_300,
  },
  {
    name: "output past the cap fails closed",
    policy: { ...LIVE_POLICY, maxOutputSamples: 1_920 },
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, audio(1_920)],
      [400, audio(2)],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "300 speak t1 1920",
      "400 fault t1 output_overflow",
    ],
    phase: "live",
    deadline: 1_600,
  },
  {
    name: "transcript deltas settle into one affect and extend the seal",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, text("That's ")],
      [1_300, text("lovely.")],
      [1_400, tick],
      [1_700, tick],
      [2_500, tick],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "1700 affect t1 That's lovely.",
      "2500 finish t1 960 tail=0",
    ],
    phase: "live",
    deadline: null,
  },
  {
    name: "a transcript still settling at the seal is flushed before finish",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [1_300, text("Hello.")],
      [2_500, tick],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "2500 affect t1 Hello.",
      "2500 finish t1 960 tail=0",
    ],
    phase: "live",
    deadline: null,
  },
  {
    name: "a cancelled reply that never goes quiet takes the session down",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, cancel("t1")],
      ...Array.from({ length: 20 }, (_, index): Step => [1_300 + index * 1_000, audio(1_920)]),
      [20_300, tick],
    ],
    effects: ["0 ready", ...burst(100), "200 speak t1 1920", "20300 lost upstream_runaway"],
    phase: "gone",
    deadline: null,
  },
  {
    name: "an upstream error fails the claimed turn and keeps the session",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, upstreamError("rate_limited")],
      [300, audio(1_920)],
      [1_500, tick],
      [1_510, begin("t2")],
      [1_520, append("t2")],
      [1_600, commit("t2")],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      '200 observe {"kind":"upstream-error","reason":"upstream_error:rate_limited"}',
      "200 fault t1 upstream_error:rate_limited",
      '1500 observe {"kind":"orphan-drained","bytes":1920}',
      ...burst(1_600),
    ],
    phase: "live",
    deadline: 7_600,
  },
  {
    name: "socket loss while voicing fails the turn and reports the loss once",
    steps: [
      [0, ready],
      ...spoken("t1"),
      [200, audio(1_920)],
      [300, closed("1006")],
      [400, audio(1_920)],
      [500, closed("1006")],
      [600, begin("t2")],
      [700, cancel("t2")],
    ],
    effects: [
      "0 ready",
      ...burst(100),
      "200 speak t1 1920",
      "300 fault t1 upstream_closed:1006",
      "300 lost upstream_closed:1006",
    ],
    refusals: ["600 session_not_live"],
    phase: "gone",
    deadline: null,
  },
  {
    name: "shutdown while voicing fails the turn with session_closed and nothing else",
    steps: [[0, ready], ...spoken("t1"), [200, audio(1_920)], [300, shutdown]],
    effects: ["0 ready", ...burst(100), "200 speak t1 1920", "300 fault t1 session_closed"],
    phase: "gone",
    deadline: null,
  },
  {
    name: "a starting-phase error is lost without any turn fault",
    steps: [[0, upstreamError("invalid_voice")], [10, begin("t1")]],
    effects: ["0 lost upstream_error:invalid_voice"],
    refusals: ["10 session_not_live"],
    phase: "gone",
    deadline: null,
  },
  {
    name: "readiness times out at readyMs",
    steps: [[9_999, tick], [10_000, tick]],
    effects: ["10000 lost ready_timeout"],
    phase: "gone",
    deadline: null,
  },
  {
    name: "refusals leave the machine untouched",
    steps: [
      [0, begin("t1")],
      [0, ready],
      [10, append("t1")],
      [20, begin("t1")],
      [30, begin("t2")],
      [40, append("t1", 3)],
      [50, commit("t1")],
      [60, append("t1")],
      [70, append("t2")],
      [80, commit("t2")],
      [90, commit("t1")],
    ],
    effects: ["0 ready", ...burst(90)],
    refusals: [
      "0 session_not_live",
      "10 turn_not_held",
      "30 turn_already_held",
      "40 input_frame_invalid",
      "50 empty_utterance",
      "70 turn_not_held",
      "80 turn_not_held",
    ],
    phase: "live",
    deadline: 6_090,
  },
  {
    name: "input transcripts and unknown events are observed without their content",
    steps: [[0, ready], [10, heard("hello there")], [20, ignored("session.updated")]],
    effects: [
      "0 ready",
      '10 observe {"kind":"input-heard","chars":11}',
      '20 observe {"kind":"upstream-ignored","type":"session.updated"}',
    ],
    phase: "live",
    deadline: null,
  },
  {
    name: "input past maxInputSamples is refused",
    policy: { ...LIVE_POLICY, maxInputSamples: 960 },
    steps: [[0, ready], [10, begin("t1")], [20, append("t1")], [30, append("t1", 2)]],
    effects: ["0 ready"],
    refusals: ["30 input_limit_exceeded"],
    phase: "live",
    deadline: null,
  },
];

describe("live turn reducer", () => {
  it.each(cases)("$name", ({ policy, steps, effects, refusals = [], phase, deadline }) => {
    const run = drive(steps, policy);
    expect(run.effects).toEqual(effects);
    expect(run.refusals).toEqual(refusals);
    expect(run.state.phase).toBe(phase);
    expect(nextDeadline(run.state)).toBe(deadline);
  });

  it("bursts the held bytes in order and delivers every claimed byte before finish", () => {
    const run = drive([
      [0, ready],
      [10, begin("t1")],
      [20, append("t1", 1_000, 3)],
      [60, append("t1", 1_000, 5)],
      [100, commit("t1")],
      [200, audio(3_000, 11)],
      [300, audio(1_320, 17)],
      [1_500, tick],
    ]);
    const appended = run.raw.flatMap((effect) =>
      effect.kind === "send" && effect.intent.kind === "appendInput" ? [effect.intent.pcm] : []);
    expect(appended.map((chunk) => chunk.byteLength)).toEqual([1_920, 80]);
    expect(concat(appended)).toEqual(concat([pcm(1_000, 3), pcm(1_000, 5)]));

    const delivered = run.raw.flatMap((effect) => {
      if (effect.kind === "speak") return effect.frames;
      if (effect.kind === "finish" && effect.tail !== null) return [effect.tail];
      return [];
    });
    const finish = run.raw.find((effect) => effect.kind === "finish");
    expect(concat(delivered)).toEqual(concat([pcm(3_000, 11), pcm(1_320, 17)]));
    expect(finish).toMatchObject({ turnId: "t1", outputSamples: 2_160 });
    expect(concat(delivered).byteLength).toBe(2_160 * 2);
  });
});
