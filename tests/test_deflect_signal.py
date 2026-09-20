import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from deflect_capture import CaptureError
from deflect_signal import (detect_candidate, discover, evaluate, match_events,
                            prepare, read_dataset, score)
from deflect_signal_fixture import write_fixture


class DeflectSignalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path, self.manifest = write_fixture(self.root, "discovery")
        self.dataset = read_dataset(self.path)
        self.rule = {"target": "player_deflect", "offset": 0, "width": 4,
                     "mode": "enter_value", "value": 900000001}

    def changed_manifest(self, mutate):
        m = copy.deepcopy(self.manifest)
        mutate(m)
        self.path.write_text(json.dumps(m), encoding="utf-8")
        return read_dataset(self.path)

    def artifact(self):
        return discover(self.dataset, "player_deflect", [0], tolerance_us=5000)

    def test_exact_raw_values_and_no_rapid_parry_cooldown(self):
        events, _ = detect_candidate(self.dataset.clips[0], self.rule, 20000)
        self.assertEqual(events, [60000, 90000, 120000])

    def test_block_has_separate_candidate_and_negatives(self):
        a = discover(self.dataset, "player_block", [0], tolerance_us=5000)
        self.assertEqual(a["candidates"][0]["rule"]["value"], 900000002)
        metrics = a["candidates"][0]["discoveryMetrics"]
        self.assertEqual(metrics["counts"], dict(tp=2, fp=0, fn=0, duplicates=0))
        self.assertEqual(metrics["byLabel"]["player_deflect"]["fp"], 0)

    def test_noisy_counter_false_positives_include_negative_conditions(self):
        rule = dict(self.rule, mode="increment_by_one", offset=8)
        predictions, _ = detect_candidate(self.dataset.clips[0], rule, 20000)
        m = score(prepare(self.dataset, 20000, 5000), [predictions], "player_deflect", 5000)
        self.assertGreater(m["byLabel"]["enemy_deflect"]["fp"], 0)
        self.assertGreater(m["byLabel"]["empty_guard"]["fp"], 0)
        self.assertGreater(m["byLabel"]["damage"]["fp"], 0)

    def test_input_marker_is_never_an_event_or_truth(self):
        a = self.artifact()
        self.assertEqual(a["discovery"]["sessions"][0]["inputMarkersIgnored"], 1)
        self.assertEqual(a["candidates"][0]["discoveryMetrics"]["counts"]["tp"], 3)
        self.assertEqual(a["candidates"][0]["discoveryMetrics"]["byLabel"]["empty_guard"]["fp"], 0)

    def test_hold_same_value_does_not_fabricate_new_occurrences(self):
        path, _ = write_fixture(self.root, "held-state", hold_same_value=True)
        ds = read_dataset(path)
        events, _ = detect_candidate(ds.clips[0], self.rule, 20000)
        self.assertEqual(events, [60000])
        m = score(prepare(ds, 20000, 5000), [events], "player_deflect", 5000)
        self.assertEqual(m["counts"]["fn"], 2)

    def test_initial_baseline_with_active_value_never_fires(self):
        clip = self.dataset.clips[0]
        clip.capture.samples = [s for s in clip.capture.samples if s.timestamp_us >= 60000]
        events, _ = detect_candidate(clip, self.rule, 20000)
        self.assertNotIn(60000, events)

    def test_duplicate_detections_are_fp_and_not_extra_tp(self):
        counts, pairs = match_events([100], [95, 100, 105], 10)
        self.assertEqual(counts, dict(tp=1, fp=2, fn=0, duplicates=2))
        self.assertEqual(len(pairs), 1)

    def test_ordered_matching_preserves_two_close_events(self):
        counts, pairs = match_events([100, 110], [105, 115], 10)
        self.assertEqual(counts, dict(tp=2, fp=0, fn=0, duplicates=0))
        self.assertEqual(pairs, [(100, 105), (110, 115)])

    def test_overlapping_trials_both_excluded_from_all_counts(self):
        trials = self.dataset.clips[0].trials
        trials.append(dict(id="overlap", label="player_deflect", startUs=50000, endUs=130000,
                           eventTimesUs=[60000]))
        trials.sort(key=lambda t: t["startUs"])
        p = prepare(self.dataset, 20000, 5000)
        self.assertEqual({t["id"] for t in p[0][2]}, {"rapid", "overlap"})
        m = score(p, [[60000, 90000, 120000]], "player_deflect", 5000)
        self.assertEqual(m["counts"], dict(tp=0, fp=0, fn=0, duplicates=0))
        self.assertEqual(m["unscoredDetections"], 3)

    def test_gap_and_generation_change_cannot_generate_cross_object_event(self):
        for key in ("segment", "generation"):
            with self.subTest(key=key):
                ds = copy.deepcopy(self.dataset)
                ds.clips[0].capture.samples = [replace(s, **{key: getattr(s, key)+1})
                    if s.timestamp_us >= 60000 else s for s in ds.clips[0].capture.samples]
                events, _ = detect_candidate(ds.clips[0], self.rule, 20000)
                self.assertNotIn(60000, events)
                p = prepare(ds, 20000, 5000)
                self.assertEqual(p[0][2][0]["reason"], "observation_gap_or_object_change")

    def test_long_successful_read_interval_also_breaks_continuity(self):
        clip = self.dataset.clips[0]
        clip.capture.samples = [s for s in clip.capture.samples if s.timestamp_us not in {40000, 50000}]
        events, _ = detect_candidate(clip, self.rule, 20000)
        self.assertNotIn(60000, events)
        self.assertEqual(prepare(self.dataset, 20000, 5000)[0][2][0]["id"], "rapid")

    def test_counter_jump_not_expanded_into_guessed_events(self):
        clip = self.dataset.clips[0]
        clip.capture.samples = [replace(s, data=s.data[:4]+(3).to_bytes(4,"little")+s.data[8:])
            if s.timestamp_us >= 60000 else s for s in clip.capture.samples]
        events, jumps = detect_candidate(clip, dict(self.rule, offset=4, mode="increment_by_one"), 20000)
        self.assertEqual(events, [])
        self.assertEqual(jumps, 1)

    def test_missing_negative_condition_blocks_discovery(self):
        self.dataset.clips[0].trials = [t for t in self.dataset.clips[0].trials if t["label"] != "enemy_deflect"]
        with self.assertRaisesRegex(CaptureError, "enemy_deflect"):
            self.artifact()

    def test_independent_annotation_and_raw_hash_are_required(self):
        with self.assertRaisesRegex(CaptureError, "hash mismatch"):
            self.changed_manifest(lambda m: m["sessions"][0].update(captureSha256="1"*64))
        with self.assertRaisesRegex(CaptureError, "independent"):
            self.changed_manifest(lambda m: m["sessions"][0].update(annotationSource="controller_input"))

    def test_time_uncertainty_cannot_be_hidden_by_narrow_tolerance(self):
        self.dataset.clips[0].sync_uncertainty_us = 16667
        with self.assertRaisesRegex(CaptureError, "uncertainty"):
            self.artifact()

    def test_training_session_and_renamed_capture_are_not_heldout(self):
        a = self.artifact()
        with self.assertRaisesRegex(CaptureError, "overlaps"):
            evaluate(self.dataset, a, 0)
        self.dataset.clips[0].session_id = "renamed"
        with self.assertRaisesRegex(CaptureError, "overlaps"):
            evaluate(self.dataset, a, 0)

    def test_other_build_or_object_layout_rejected(self):
        a = self.artifact()
        path, _ = write_fixture(self.root, "heldout")
        for key in ("exeSha256", "regionKey", "evidenceKind"):
            ds = read_dataset(path)
            ds.metadata[key] = "different"
            with self.subTest(key=key), self.assertRaisesRegex(CaptureError, "mismatch"):
                evaluate(ds, a, 0)

    def test_heldout_block_false_positive_exposed_without_promotion(self):
        a = self.artifact()
        path, _ = write_fixture(self.root, "heldout-bad", wrong_block_value=True)
        result = evaluate(read_dataset(path), a, 0)
        self.assertEqual(result["metrics"]["byLabel"]["player_block"]["fp"], 2)
        self.assertEqual(result["metrics"]["counts"]["tp"], 3)
        self.assertEqual(result["status"], "candidate_only")
        self.assertIs(result["hapticOutputEnabled"], False)

    def test_invalid_and_outside_fields_rejected(self):
        for changes in ({"offset": -1}, {"offset": 12}, {"width": 3}, {"value": 1 << 32}, {"offset": True}):
            with self.subTest(changes=changes), self.assertRaises(CaptureError):
                detect_candidate(self.dataset.clips[0], dict(self.rule, **changes), 20000)

    def test_byte_and_word_fields_use_raw_little_endian_bytes(self):
        for width in (1, 2):
            with self.subTest(width=width):
                rule = dict(self.rule, width=width, value=900000001 & ((1 << (8*width))-1))
                events, _ = detect_candidate(self.dataset.clips[0], rule, 20000)
                self.assertEqual(events, [60000, 90000, 120000])

    def test_unsigned_counter_wrap_counts_only_one_observed_step(self):
        clip = self.dataset.clips[0]
        clip.capture.samples = [replace(s, data=((2**32-1) if s.timestamp_us < 60000 else 0).to_bytes(4, "little")+s.data[4:])
                                for s in clip.capture.samples]
        events, jumps = detect_candidate(clip, dict(self.rule, mode="increment_by_one"), 20000)
        self.assertEqual(events, [60000])
        self.assertEqual(jumps, 0)

    def test_cli_full_roundtrip_and_existing_output_preserved(self):
        script = Path(__file__).resolve().parents[1]/"tools"/"deflect_signal.py"
        candidates = self.root/"candidates.json"
        command = [sys.executable, str(script), "discover", str(self.path), "--target", "player_deflect",
                   "--offset", "0", "--tolerance-us", "5000", "--output", str(candidates)]
        subprocess.run(command, check=True, capture_output=True)
        before = candidates.read_bytes()
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
        self.assertEqual(candidates.read_bytes(), before)
        heldout, _ = write_fixture(self.root, "heldout-cli")
        output = self.root/"evaluation.json"
        subprocess.run([sys.executable, str(script), "evaluate", str(heldout), "--candidates", str(candidates),
                        "--index", "0", "--output", str(output)], check=True, capture_output=True)
        result = json.loads(output.read_text())
        self.assertEqual(result["metrics"]["counts"], dict(tp=3, fp=0, fn=0, duplicates=0))
        self.assertEqual(result["evaluation"]["evidenceKind"], "synthetic")


if __name__ == "__main__":
    unittest.main()
