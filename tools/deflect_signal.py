#!/usr/bin/env python3
"""Discover and falsify block/deflect field hypotheses in capture-v3 files.

No known Sekiro result address/ID is supplied. This is an offline signal
investigation tool, not a live game detector or a haptic event source.
Only independent annotations in a manifest are evaluated; input markers are
never labels. A successful evaluation never automatically promotes a rule.
"""
from __future__ import annotations

import argparse
from bisect import bisect_left
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys

from deflect_capture import CaptureError, integer, read_capture, unique_object


LABELS = {"player_deflect", "player_block", "enemy_deflect", "empty_guard",
          "damage", "other", "ambiguous"}
TARGETS = {"player_deflect", "player_block"}
REQUIRED_LABELS = LABELS - {"other", "ambiguous"}
MAX_FIELD_WORK = 50_000_000


def require(condition, message):
    if not condition:
        raise CaptureError(message)


def load_json(path):
    raw = Path(path).read_bytes()
    require(len(raw) <= 8 * 1024 * 1024, "manifest/report exceeds 8 MiB")
    value = json.loads(raw, object_pairs_hook=unique_object,
                       parse_constant=lambda x: (_ for _ in ()).throw(CaptureError(f"invalid JSON: {x}")))
    require(isinstance(value, dict), "expected a JSON object")
    return value, hashlib.sha256(raw).hexdigest()


def nonempty(value, description):
    require(isinstance(value, str) and bool(value.strip()), f"missing {description}")
    return value


def sha(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value), "invalid SHA-256")
    return value


@dataclass
class Clip:
    session_id: str
    capture: object
    trials: list[dict]
    sync_uncertainty_us: int


@dataclass
class Dataset:
    metadata: dict
    manifest_sha256: str
    clips: list[Clip]


def read_dataset(path):
    path = Path(path)
    m, digest = load_json(path)
    require(integer(m, "schemaVersion") == 1, "requires manifest schema 1")
    require(m.get("evidenceKind") in {"synthetic", "live_review"}, "invalid evidenceKind")
    sha(m.get("exeSha256"))
    nonempty(m.get("regionKey"), "regionKey (same object/path/layout across captures)")
    raw_clips = m.get("sessions")
    require(isinstance(raw_clips, list) and 1 <= len(raw_clips) <= 32, "requires 1..32 sessions")
    ids, hashes, clips = set(), set(), []
    source = "synthetic_fixture" if m["evidenceKind"] == "synthetic" else "independent_video_review"
    total_bytes = 0
    for c in raw_clips:
        require(isinstance(c, dict), "invalid session")
        sid = nonempty(c.get("sessionId"), "sessionId")
        require(sid not in ids, "duplicate sessionId; use one capture per session")
        ids.add(sid)
        require(c.get("annotationSource") == source, "annotations must be independent video review or explicit synthetic fixtures")
        nonempty(c.get("annotationEvidence"), "video reference and clock alignment explanation")
        uncertainty = integer(c, "syncUncertaintyUs", 0, 1_000_000)
        capture = read_capture(path.parent / nonempty(c.get("capture"), "capture path"))
        require(capture.sha256 == sha(c.get("captureSha256")), "capture hash mismatch")
        require(capture.sha256 not in hashes, "duplicate capture contents")
        hashes.add(capture.sha256)
        total_bytes += sum(len(s.data) for s in capture.samples)
        require(total_bytes <= 256 * 1024 * 1024, "dataset exceeds 256 MiB reconstructed samples")
        trials = c.get("trials")
        require(isinstance(trials, list) and 1 <= len(trials) <= 10000, "requires 1..10000 trials per session")
        trial_ids = set()
        for t in trials:
            require(isinstance(t, dict), "invalid trial")
            tid = nonempty(t.get("id"), "trial id")
            require(tid not in trial_ids, "duplicate trial id")
            trial_ids.add(tid)
            require(t.get("label") in LABELS, "invalid trial label")
            start, end = integer(t, "startUs"), integer(t, "endUs")
            require(start < end, "trial must have positive duration")
            events = t.get("eventTimesUs")
            require(isinstance(events, list), "eventTimesUs must be a list")
            require(all(type(e) is int and start <= e < end for e in events), "event time outside trial")
            require(events == sorted(set(events)), "event times must be unique and sorted")
            if t["label"] in TARGETS:
                require(bool(events), "player block/deflect trials need independently annotated impact times")
            else:
                require(not events, "negative/ambiguous trials have no player target events")
        clips.append(Clip(sid, capture, sorted(trials, key=lambda t: t["startUs"]), uncertainty))
    return Dataset({k: m[k] for k in ("evidenceKind", "exeSha256", "regionKey")}, digest, clips)


def continuous(a, b, max_gap_us):
    return (a.segment == b.segment and a.generation == b.generation
            and 0 < b.timestamp_us - a.timestamp_us <= max_gap_us)


def prepare(dataset, max_gap_us, tolerance_us):
    """Reject whole trials crossing gaps or overlaps, from numerator AND denominator."""
    require(type(max_gap_us) is int and 0 < max_gap_us <= 2_000_000, "invalid max gap")
    require(type(tolerance_us) is int and 0 <= tolerance_us <= 500_000, "invalid matching tolerance")
    prepared = []
    for clip in dataset.clips:
        require(clip.sync_uncertainty_us <= tolerance_us, "sync uncertainty exceeds matching tolerance")
        times = [s.timestamp_us for s in clip.capture.samples]
        overlap = set()
        # A sorted sweep, including nested trials. Both owners of any shared
        # observation are excluded; no delta can earn credit in two trials.
        furthest = None
        for i, t in enumerate(clip.trials):
            if furthest is not None and t["startUs"] < clip.trials[furthest]["endUs"]:
                overlap.update((i, furthest))
            if furthest is None or t["endUs"] > clip.trials[furthest]["endUs"]:
                furthest = i
        accepted, excluded = [], []
        for i, t in enumerate(clip.trials):
            reason = None
            left = bisect_left(times, t["startUs"]) - 1
            right = bisect_left(times, t["endUs"])
            if i in overlap:
                reason = "overlapping_trials"
            elif t["label"] == "ambiguous":
                reason = "ambiguous_annotation"
            elif left < 0 or right >= len(times):
                reason = "missing_observation_before_or_after_trial"
            elif any(not continuous(clip.capture.samples[j-1], clip.capture.samples[j], max_gap_us)
                     for j in range(left+1, right+1)):
                reason = "observation_gap_or_object_change"
            if reason:
                excluded.append({"id": t["id"], "label": t["label"], "reason": reason,
                                 "annotatedEvents": len(t["eventTimesUs"])})
            else:
                accepted.append(t)
        prepared.append((clip, accepted, excluded))
    return prepared


def check_labels(prepared):
    present = {t["label"] for _, trials, _ in prepared for t in trials}
    require(REQUIRED_LABELS <= present, "need covered trials for: " + ", ".join(sorted(REQUIRED_LABELS - present)))


def field_changes(clip, offset, width, max_gap_us):
    require(type(width) is int and width in (1, 2, 4), "width must be 1, 2, or 4")
    require(type(offset) is int and 0 <= offset and offset+width <= clip.capture.header["windowSizeBytes"],
            "field outside capture window")
    for a, b in zip(clip.capture.samples, clip.capture.samples[1:]):
        if continuous(a, b, max_gap_us):
            before = int.from_bytes(a.data[offset:offset+width], "little")
            after = int.from_bytes(b.data[offset:offset+width], "little")
            if before != after:
                yield b.timestamp_us, before, after


def detect_candidate(clip, rule, max_gap_us):
    """Hypothesis events only. No cooldown, no fabricated occurrence counter."""
    require(rule.get("target") in TARGETS, "invalid target")
    mode = rule.get("mode")
    require(mode in {"enter_value", "increment_by_one"}, "invalid candidate mode")
    width = rule.get("width")
    require(type(width) is int and width in (1, 2, 4), "invalid width")
    if mode == "enter_value":
        integer(rule, "value", 0, (1 << (8*width))-1)
    events, jumps = [], 0
    mask = (1 << (width*8))-1
    for timestamp, before, after in field_changes(clip, rule.get("offset"), width, max_gap_us):
        if mode == "enter_value" and after == rule["value"]:
            events.append(timestamp)
        elif mode == "increment_by_one":
            if (after-before) & mask == 1:
                events.append(timestamp)
            else:
                jumps += 1  # Do not invent missed event types or count a jump as one.
    return events, jumps


def match_events(truth, predictions, tolerance_us):
    """Maximum-cardinality 1:1 matching for ordered equal-tolerance windows.

    Earliest eligible prediction goes to earliest truth. Delays are observed
    clock differences, not measured game-to-device latency. Unmatched nearby
    predictions are duplicates AND false positives, never extra true positives.
    """
    i, j, matched, unmatched = 0, 0, [], []
    while i < len(truth) and j < len(predictions):
        if predictions[j] < truth[i]-tolerance_us:
            unmatched.append(predictions[j]); j += 1
        elif predictions[j] > truth[i]+tolerance_us:
            i += 1
        else:
            matched.append((truth[i], predictions[j])); i += 1; j += 1
    unmatched.extend(predictions[j:])
    duplicates = 0
    for p in unmatched:
        k = bisect_left(truth, p-tolerance_us)
        duplicates += int(k < len(truth) and truth[k] <= p+tolerance_us)
    return {"tp": len(matched), "fp": len(predictions)-len(matched),
            "fn": len(truth)-len(matched), "duplicates": duplicates}, matched


def score(prepared, predictions, target, tolerance_us):
    totals = Counter(tp=0, fp=0, fn=0, duplicates=0)
    by_label, delays, per_trial, exclusions = {}, [], [], []
    unscored = 0
    for (clip, trials, rejected), events in zip(prepared, predictions):
        scored = 0
        exclusions.extend(dict(t, sessionId=clip.session_id) for t in rejected)
        for t in trials:
            observed = events[bisect_left(events, t["startUs"]):bisect_left(events, t["endUs"])]
            scored += len(observed)
            truth = t["eventTimesUs"] if t["label"] == target else []
            counts, pairs = match_events(truth, observed, tolerance_us)
            totals.update(counts)
            bucket = by_label.setdefault(t["label"], Counter(trials=0, tp=0, fp=0, fn=0, duplicates=0))
            bucket.update(counts); bucket["trials"] += 1
            delays.extend(p-e for e, p in pairs)
            per_trial.append({"sessionId": clip.session_id, "id": t["id"], "label": t["label"],
                              **counts, "predictedTimesUs": observed,
                              "matches": [{"annotatedUs": e, "observedUs": p} for e, p in pairs]})
        unscored += len(events)-scored
    tp, fp, fn = (totals[k] for k in ("tp", "fp", "fn"))
    return {"counts": dict(totals), "precision": tp/(tp+fp) if tp+fp else None,
            "recall": tp/(tp+fn) if tp+fn else None,
            "byLabel": {k: dict(v) for k, v in sorted(by_label.items())},
            "trials": per_trial, "excludedTrials": exclusions,
            "unscoredDetections": unscored,
            "observedDelayUs": {"min": min(delays), "median": statistics.median(delays), "max": max(delays)} if delays else None}


def provenance(dataset):
    return {**dataset.metadata, "manifestSha256": dataset.manifest_sha256,
            "sessions": [{"sessionId": c.session_id, "captureSha256": c.capture.sha256,
                          "syncUncertaintyUs": c.sync_uncertainty_us,
                          "inputMarkersIgnored": len(c.capture.annotations)} for c in dataset.clips]}


def discover(dataset, target, offsets, width=4, max_gap_us=20_000, tolerance_us=50_000, limit=20):
    require(target in TARGETS, "invalid target")
    require(type(width) is int and width in (1, 2, 4), "invalid width")
    require(type(limit) is int and 1 <= limit <= 100, "limit must be 1..100")
    prepared = prepare(dataset, max_gap_us, tolerance_us)
    check_labels(prepared)
    require(bool(offsets) and len(offsets) == len(set(offsets)), "offsets must be nonempty and unique")
    require(sum(len(c.capture.samples) for c in dataset.clips)*len(offsets) <= MAX_FIELD_WORK,
            "search exceeds 50 million field reads; narrow offsets or shorten captures")
    best, tested = [], 0
    for offset in offsets:
        by_value = defaultdict(lambda: [[] for _ in dataset.clips])
        increments = [[] for _ in dataset.clips]
        mask = (1 << (8*width))-1
        for ci, clip in enumerate(dataset.clips):
            for timestamp, before, after in field_changes(clip, offset, width, max_gap_us):
                require(after in by_value or len(by_value) < 100_000,
                        "too many distinct field values; narrow the search")
                by_value[after][ci].append(timestamp)
                if (after-before) & mask == 1:
                    increments[ci].append(timestamp)
        variants = [({"mode": "enter_value", "value": value}, events) for value, events in by_value.items()]
        variants.append(({"mode": "increment_by_one"}, increments))
        for extra, predictions in variants:
            tested += 1
            require(tested <= 100_000, "too many candidate rules; narrow the search")
            require(tested * sum(len(t) for _, t, _ in prepared) <= MAX_FIELD_WORK,
                    "too many rule/trial comparisons; narrow the search")
            metrics = score(prepared, predictions, target, tolerance_us)
            if not metrics["counts"]["tp"]:
                continue
            rule = {"target": target, "offset": offset, "width": width, **extra}
            # Keep compact exploratory ranking; full per-trial records are
            # produced only when evaluating a frozen rule on another session.
            metrics.pop("trials")
            best.append({"rule": rule, "discoveryMetrics": metrics})
        best.sort(key=lambda x: (-x["discoveryMetrics"]["counts"]["tp"],
                                 x["discoveryMetrics"]["counts"]["fp"],
                                 x["rule"]["offset"], x["rule"]["mode"], x["rule"].get("value", -1)))
        del best[limit:]
    return {"schemaVersion": 1, "recordKind": "signal_candidates", "status": "candidate_only",
            "hapticOutputEnabled": False, "discovery": provenance(dataset),
            "maxGapUs": max_gap_us, "toleranceUs": tolerance_us, "rulesTested": tested,
            "selectionWarning": "Exploratory ranking on discovery data; no accuracy or game-meaning claim.",
            "candidates": best}


def evaluate(dataset, artifact, candidate_index):
    require(integer(artifact, "schemaVersion") == 1 and artifact.get("recordKind") == "signal_candidates",
            "expected a discovery artifact")
    require(artifact.get("status") == "candidate_only" and artifact.get("hapticOutputEnabled") is False,
            "only candidate artifacts are supported")
    training = artifact.get("discovery")
    require(isinstance(training, dict), "missing discovery provenance")
    for k in ("evidenceKind", "exeSha256", "regionKey"):
        require(dataset.metadata[k] == training.get(k), f"discovery/evaluation {k} mismatch")
    sessions = training.get("sessions")
    require(isinstance(sessions, list) and bool(sessions), "missing discovery sessions")
    previous_ids = {nonempty(s.get("sessionId"), "discovery sessionId") for s in sessions}
    previous_hashes = {sha(s.get("captureSha256")) for s in sessions}
    for c in dataset.clips:
        require(c.session_id not in previous_ids and c.capture.sha256 not in previous_hashes,
                "held-out data overlaps discovery session or capture")
    candidates = artifact.get("candidates")
    require(isinstance(candidates, list) and type(candidate_index) is int
            and 0 <= candidate_index < len(candidates), "candidate index outside artifact")
    rule = candidates[candidate_index]["rule"]
    gap = integer(artifact, "maxGapUs", 1, 2_000_000)
    tolerance = integer(artifact, "toleranceUs", 0, 500_000)
    prepared = prepare(dataset, gap, tolerance)
    check_labels(prepared)
    raw = [detect_candidate(c, rule, gap) for c in dataset.clips]
    return {"schemaVersion": 1, "recordKind": "signal_evaluation", "status": "candidate_only",
            "hapticOutputEnabled": False, "rule": rule, "evaluation": provenance(dataset),
            "discovery": training, "maxGapUs": gap, "toleranceUs": tolerance,
            "counterJumpsNotExpanded": sum(j for _, j in raw),
            "metrics": score(prepared, [p for p, _ in raw], rule["target"], tolerance),
            "limitation": "Independent annotations are user-declared, not verified by this tool. Field meaning, player ownership, missed between-sample events and live detector readiness still need game evidence."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    d = commands.add_parser("discover", help="rank candidates on discovery sessions only")
    d.add_argument("manifest", type=Path)
    d.add_argument("--target", required=True, choices=sorted(TARGETS))
    d.add_argument("--offset", type=lambda x: int(x, 0), action="append",
                   help="repeat for selected offsets; default scans aligned fields in first 4096 bytes")
    d.add_argument("--width", type=int, choices=(1, 2, 4), default=4)
    d.add_argument("--max-gap-us", type=int, default=20_000)
    d.add_argument("--tolerance-us", type=int, default=50_000)
    d.add_argument("--limit", type=int, default=20)
    e = commands.add_parser("evaluate", help="test one frozen candidate on separate sessions")
    e.add_argument("manifest", type=Path)
    e.add_argument("--candidates", type=Path, required=True)
    e.add_argument("--index", type=int, default=0)
    for p in (d, e):
        p.add_argument("--output", type=Path, required=True, help="new file; existing outputs are refused")
    args = parser.parse_args()
    try:
        require(not args.output.exists(), "output already exists")
        dataset = read_dataset(args.manifest)
        if args.command == "discover":
            size = min(4096, *(c.capture.header["windowSizeBytes"] for c in dataset.clips))
            offsets = args.offset if args.offset is not None else list(range(0, size-args.width+1, args.width))
            result = discover(dataset, args.target, offsets, args.width, args.max_gap_us, args.tolerance_us, args.limit)
        else:
            artifact, digest = load_json(args.candidates)
            result = evaluate(dataset, artifact, args.index)
            result["candidateArtifactSha256"] = digest
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
        print(f"Saved {args.output}; candidate only, live detector NOT enabled.")
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
