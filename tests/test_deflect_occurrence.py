#!/usr/bin/env python3
"""Pins tools/deflect_occurrence.py's Detector to the SAME behaviour as the C++
GuardOutcomeEventDetector. Every scenario here has a one-to-one counterpart in
tests/test_guard_outcome_event_detector.cpp, so the replay tool and the runtime
detector cannot drift apart.

All data is synthetic. No occurrence field has been confirmed in the real game
(docs/astra/results/DEFLECT_STATUS.md 6.9); passing these tests measures the
rules, not in-game accuracy.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from deflect_occurrence import BLOCK, DEFLECT, UNRESOLVED, Detector  # noqa: E402


class Harness:
    def __init__(self, **kw):
        self.det = Detector(**kw)
        self.events = []
        self.t = 0
        self.counter = 100
        self.outcome = 0
        self.generation = 7

    def tick(self, step=5000, read_ok=True, brk=False):
        self.t += step
        self.events += self.det.update(self.t, read_ok, self.generation, brk,
                                       self.counter, self.outcome)

    def resolve(self, outcome, step=1):
        self.counter += step
        self.outcome = outcome
        self.tick()

    def finish(self):
        self.events += self.det.finish()

    def kinds(self):
        return [e.kind for e in self.events]


class RepeatedOutcomes(unittest.TestCase):
    def test_three_deflects_in_a_row_are_three_events(self):
        h = Harness()
        h.tick()
        for _ in range(3):
            h.resolve(1)
        self.assertEqual(h.kinds(), [DEFLECT] * 3)

    def test_three_blocks_in_a_row_are_three_events(self):
        h = Harness()
        h.tick()
        for _ in range(3):
            h.resolve(0)
        self.assertEqual(h.kinds(), [BLOCK] * 3)

    def test_mixed_sequence_preserves_kind_and_order(self):
        h = Harness()
        h.tick()
        for o in (1, 1, 0, 0, 1):
            h.resolve(o)
        self.assertEqual(h.kinds(), [DEFLECT, DEFLECT, BLOCK, BLOCK, DEFLECT])

    def test_rapid_resolutions_are_not_merged(self):
        h = Harness()
        h.tick()
        for _ in range(2):
            h.counter += 1
            h.outcome = 1
            h.tick(20000)
        self.assertEqual(h.kinds(), [DEFLECT, DEFLECT])


class NoFabricatedEvents(unittest.TestCase):
    def test_held_outcome_emits_nothing(self):
        h = Harness()
        h.tick()
        h.resolve(1)
        for _ in range(50):
            h.tick()
        self.assertEqual(len(h.events), 1)

    def test_idle_at_zero_never_emits_a_block(self):
        h = Harness()
        for _ in range(100):
            h.tick()
        self.assertEqual(h.events, [])

    def test_first_observation_is_baseline_only(self):
        h = Harness()
        h.counter, h.outcome = 5000, 1
        h.tick()
        self.assertEqual(h.events, [])


class CounterDiscipline(unittest.TestCase):
    def test_jump_reports_one_unresolved_not_two_events(self):
        h = Harness()
        h.tick()
        h.resolve(1, step=2)
        self.assertEqual(h.kinds(), [UNRESOLVED])
        self.assertEqual(h.events[0].reason, "counter_jump")
        self.assertEqual(h.events[0].delta, 2)

    def test_wrap_counts_as_one_step(self):
        h = Harness(counter_bits=32)
        h.counter = 0xFFFFFFFF
        h.tick()
        h.counter = 0
        h.outcome = 1
        h.tick()
        self.assertEqual(h.kinds(), [DEFLECT])
        self.assertEqual(h.events[0].delta, 1)

    def test_outcome_outside_zero_one_is_unresolved(self):
        h = Harness()
        h.tick()
        h.resolve(7)
        self.assertEqual(h.kinds(), [UNRESOLVED])
        self.assertEqual(h.events[0].reason, "outcome_out_of_range")


class Continuity(unittest.TestCase):
    def test_object_replacement_does_not_diff_across_instances(self):
        h = Harness()
        h.tick()
        h.resolve(1)
        h.generation, h.counter, h.outcome = 8, 42, 1
        h.tick()
        self.assertEqual(len(h.events), 1)
        h.resolve(0)
        self.assertEqual(h.kinds(), [DEFLECT, BLOCK])

    def test_read_failure_then_resume_emits_no_fabricated_event(self):
        h = Harness()
        h.tick()
        h.resolve(1)
        h.tick(read_ok=False)
        h.tick(read_ok=False)
        h.counter += 9
        h.outcome = 0
        h.tick()
        self.assertEqual(len(h.events), 1)
        h.resolve(1)
        self.assertEqual(h.kinds(), [DEFLECT, DEFLECT])

    def test_capture_gap_breaks_continuity_and_resumes(self):
        h = Harness()
        h.tick()
        h.resolve(0)
        h.counter += 4
        h.outcome = 1
        h.tick(brk=True)
        self.assertEqual(len(h.events), 1)
        h.resolve(0)
        self.assertEqual(h.kinds(), [BLOCK, BLOCK])


class LaggingOutcome(unittest.TestCase):
    def test_classifies_from_settled_value(self):
        h = Harness(outcome_settle_us=10000)
        h.tick()
        h.counter += 1
        h.outcome = 0
        h.tick()
        self.assertEqual(h.events, [])
        h.outcome = 1
        h.tick()
        h.tick()
        h.tick()
        self.assertEqual(h.kinds(), [DEFLECT])
        self.assertEqual(h.events[0].timestamp_us, 10000)

    def test_next_occurrence_before_settle_is_unresolved(self):
        h = Harness(outcome_settle_us=50000)
        h.tick()
        h.resolve(0)
        h.resolve(1)
        self.assertEqual(h.events[0].kind, UNRESOLVED)
        self.assertEqual(h.events[0].reason, "outcome_never_settled")

    def test_pending_at_stream_end_is_unresolved(self):
        h = Harness(outcome_settle_us=50000)
        h.tick()
        h.resolve(1)
        h.finish()
        self.assertEqual(h.kinds(), [UNRESOLVED])


class AlternativeShapes(unittest.TestCase):
    def test_pulse_mode_one_event_per_rising_edge(self):
        h = Harness(mode="pulse")
        h.counter = 0
        h.tick()
        for _ in range(3):
            h.counter, h.outcome = 1, 1
            h.tick()
            h.tick()
            h.counter = 0
            h.tick()
        self.assertEqual(h.kinds(), [DEFLECT] * 3)

    def test_identifier_mode_change_to_new_id_is_one_event(self):
        h = Harness(mode="identifier")
        h.counter = 0x1111
        h.tick()
        h.counter, h.outcome = 0x2222, 1
        h.tick()
        h.tick()
        h.counter, h.outcome = 0x3333, 0
        h.tick()
        self.assertEqual(h.kinds(), [DEFLECT, BLOCK])


if __name__ == "__main__":
    unittest.main(verbosity=2)
