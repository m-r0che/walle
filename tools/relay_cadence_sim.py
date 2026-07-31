#!/usr/bin/env python3
"""Deterministic model of the device capture queue and blocking WSS sender.

This is deliberately a conservative transport model, not a network benchmark.
It reproduces the observed 256/480-sample queue pressure and checks whether a
candidate network frame size has enough margin before hardware integration.
"""

from __future__ import annotations

import argparse
import heapq
from collections import deque
from dataclasses import dataclass

SAMPLE_RATE = 24_000
CODEC_CHUNK_SAMPLES = 256
PRECOMMIT_SAMPLES = 4_352  # 17 codec reads at the measured ~180 ms commit.


@dataclass(frozen=True)
class SimulationResult:
    network_frame_samples: int
    send_time_ms: float
    turn_seconds: float
    queue_capacity: int
    audio_frames_enqueued: int
    audio_frames_dropped: int
    network_frames_sent: int
    queue_high_water: int
    commit_queued: bool
    commit_sent_ms: float | None
    last_echo_ms: float | None

    @property
    def release_ms(self) -> float:
        return self.turn_seconds * 1_000

    @property
    def completion_after_release_ms(self) -> float | None:
        if self.last_echo_ms is None:
            return None
        return self.last_echo_ms - self.release_ms

    @property
    def remote_candidate_valid(self) -> bool:
        latency = self.completion_after_release_ms
        return (
            self.audio_frames_dropped == 0
            and self.commit_queued
            and self.commit_sent_ms is not None
            and latency is not None
            and latency <= 500
        )


def codec_chunks(total_samples: int) -> list[int]:
    chunks: list[int] = []
    remaining = total_samples
    while remaining:
        count = min(CODEC_CHUNK_SAMPLES, remaining)
        chunks.append(count)
        remaining -= count
    return chunks


def simulate(
    *,
    network_frame_samples: int,
    send_time_ms: float,
    turn_seconds: float = 6.0,
    queue_capacity: int = 32,
    control_reserve: int = 2,
    manager_poll_ms: float = 10.0,
    echo_rtt_ms: float = 40.0,
    control_send_ms: float = 3.0,
) -> SimulationResult:
    if network_frame_samples <= 0 or send_time_ms < 0 or turn_seconds <= 0:
        raise ValueError("frame size and turn duration must be positive")
    if queue_capacity <= control_reserve + 1:
        raise ValueError("queue must leave space for controls and audio")

    total_samples = round(turn_seconds * SAMPLE_RATE)
    chunks = codec_chunks(total_samples)
    precommit_chunks = codec_chunks(min(PRECOMMIT_SAMPLES, total_samples))
    remaining_chunks = chunks[len(precommit_chunks) :]
    commit_at_ms = len(precommit_chunks) * CODEC_CHUNK_SAMPLES / SAMPLE_RATE * 1_000

    # (time, priority, sequence, event, sample_count). Producer events win ties.
    events: list[tuple[float, int, int, str, int]] = []
    sequence = 0

    def schedule(time_ms: float, priority: int, event: str, count: int = 0) -> None:
        nonlocal sequence
        sequence += 1
        heapq.heappush(events, (time_ms, priority, sequence, event, count))

    schedule(commit_at_ms, 0, "start")
    for count in precommit_chunks:
        schedule(commit_at_ms, 0, "audio", count)
    sample_cursor = sum(precommit_chunks)
    for count in remaining_chunks:
        sample_cursor += count
        schedule(sample_cursor / SAMPLE_RATE * 1_000, 0, "audio", count)
    schedule(turn_seconds * 1_000 + 0.001, 0, "commit")
    schedule(0, 1, "manager")

    queue: deque[tuple[str, int]] = deque()
    batch_samples = 0
    manager_scheduled = True
    audio_enqueued = 0
    audio_dropped = 0
    network_frames = 0
    high_water = 0
    commit_queued = False
    commit_sent_ms: float | None = None
    last_echo_ms: float | None = None

    while events:
        now, _, _, event, count = heapq.heappop(events)
        if event == "manager":
            manager_scheduled = False
            if not queue:
                continue
            queued_event, queued_count = queue.popleft()
            if queued_event == "audio":
                batch_samples += queued_count
                if batch_samples >= network_frame_samples:
                    # A codec chunk can cross one frame boundary. Any spill is
                    # retained for the next network frame.
                    batch_samples -= network_frame_samples
                    network_frames += 1
                    completed = now + send_time_ms
                    last_echo_ms = completed + echo_rtt_ms
                    schedule(completed, 1, "manager")
                    manager_scheduled = True
                else:
                    schedule(now, 1, "manager")
                    manager_scheduled = True
            elif queued_event == "start":
                schedule(now + control_send_ms, 1, "manager")
                manager_scheduled = True
            elif queued_event == "commit":
                if batch_samples:
                    network_frames += 1
                    batch_samples = 0
                    completed = now + send_time_ms
                    last_echo_ms = completed + echo_rtt_ms
                    schedule(completed, 1, "commit-control")
                else:
                    schedule(now, 1, "commit-control")
            continue

        if event == "commit-control":
            commit_sent_ms = now + control_send_ms
            continue

        if event == "audio":
            # Firmware admits audio only while more than the reserved control
            # slots remain.
            if len(queue) < queue_capacity - control_reserve:
                queue.append((event, count))
                audio_enqueued += 1
            else:
                audio_dropped += 1
        elif event == "commit":
            if len(queue) < queue_capacity:
                queue.append((event, count))
                commit_queued = True
        else:  # start
            if len(queue) < queue_capacity:
                queue.append((event, count))

        high_water = max(high_water, len(queue))
        if queue and not manager_scheduled:
            schedule(now + manager_poll_ms, 1, "manager")
            manager_scheduled = True

    return SimulationResult(
        network_frame_samples=network_frame_samples,
        send_time_ms=send_time_ms,
        turn_seconds=turn_seconds,
        queue_capacity=queue_capacity,
        audio_frames_enqueued=audio_enqueued,
        audio_frames_dropped=audio_dropped,
        network_frames_sent=network_frames,
        queue_high_water=high_water,
        commit_queued=commit_queued,
        commit_sent_ms=commit_sent_ms,
        last_echo_ms=last_echo_ms,
    )


def print_result(result: SimulationResult) -> None:
    completion = result.completion_after_release_ms
    completion_text = "n/a" if completion is None else f"{completion:.1f}"
    print(
        f"frame={result.network_frame_samples:4d} send={result.send_time_ms:4.1f}ms "
        f"queue={result.queue_capacity:2d} high={result.queue_high_water:2d} "
        f"drops={result.audio_frames_dropped:3d} net_frames={result.network_frames_sent:3d} "
        f"complete_after_release={completion_text:>6}ms "
        f"valid={'yes' if result.remote_candidate_valid else 'no'}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--turn-seconds", type=float, default=6.0)
    parser.add_argument("--queue-capacity", type=int, default=32)
    parser.add_argument("--send-time-ms", type=float, nargs="+", default=[30, 35, 40, 45])
    parser.add_argument("--frame-samples", type=int, nargs="+", default=[256, 480, 960])
    args = parser.parse_args()

    for frame_samples in args.frame_samples:
        for send_time_ms in args.send_time_ms:
            print_result(
                simulate(
                    network_frame_samples=frame_samples,
                    send_time_ms=send_time_ms,
                    turn_seconds=args.turn_seconds,
                    queue_capacity=args.queue_capacity,
                )
            )


if __name__ == "__main__":
    main()
