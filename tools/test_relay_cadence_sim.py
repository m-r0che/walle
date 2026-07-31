#!/usr/bin/env python3

import unittest

from relay_cadence_sim import SAMPLE_RATE, codec_chunks, simulate


class RelayCadenceSimulationTest(unittest.TestCase):
    def test_codec_chunks_preserve_exact_sample_count(self) -> None:
        chunks = codec_chunks(6 * SAMPLE_RATE)
        self.assertEqual(sum(chunks), 144_000)
        self.assertTrue(all(0 < count <= 256 for count in chunks))
        self.assertEqual(chunks[-1], 128)

    def test_measured_256_sample_strategy_is_not_sustainable(self) -> None:
        result = simulate(network_frame_samples=256, send_time_ms=30)
        self.assertGreater(result.audio_frames_dropped, 0)
        self.assertFalse(result.remote_candidate_valid)

    def test_960_sample_batching_survives_six_second_turn(self) -> None:
        result = simulate(network_frame_samples=960, send_time_ms=30)
        self.assertEqual(result.audio_frames_dropped, 0)
        self.assertTrue(result.commit_queued)
        self.assertEqual(result.network_frames_sent, 150)
        self.assertLessEqual(result.queue_high_water, 20)
        self.assertLess(result.completion_after_release_ms or 501, 500)
        self.assertTrue(result.remote_candidate_valid)

    def test_960_sample_batching_has_a_bounded_40ms_case(self) -> None:
        result = simulate(network_frame_samples=960, send_time_ms=40)
        self.assertEqual(result.audio_frames_dropped, 0)
        self.assertTrue(result.remote_candidate_valid)
        self.assertLessEqual(result.queue_high_water, 30)

    def test_960_sample_batching_fails_closed_beyond_modeled_margin(self) -> None:
        result = simulate(network_frame_samples=960, send_time_ms=45)
        self.assertGreater(result.audio_frames_dropped, 0)
        self.assertFalse(result.remote_candidate_valid)

    def test_short_committed_turn_flushes_partial_network_frame(self) -> None:
        result = simulate(
            network_frame_samples=960,
            send_time_ms=30,
            turn_seconds=0.2,
        )
        self.assertEqual(result.audio_frames_dropped, 0)
        self.assertEqual(result.network_frames_sent, 5)
        self.assertTrue(result.remote_candidate_valid)


if __name__ == "__main__":
    unittest.main()
