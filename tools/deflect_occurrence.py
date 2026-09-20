#!/usr/bin/env python3
"""Offline analysis of a capture-v3 recording of the player's ActionFlagModule.

  find    -- rank offsets that behave like "a new guard resolution happened"
  replay  -- feed a chosen occurrence offset + the outcome byte through the
             same event rules as the C++ GuardOutcomeEventDetector and print
             the resulting deflect/block event log

This tool never decides that a deflect happened on its own: `replay` needs an
occurrence offset that a human chose from `find` output and from live evidence.
Markers in the capture are human annotations, never ground truth.

  python tools/deflect_occurrence.py find capture.jsonl
  python tools/deflect_occurrence.py replay capture.jsonl --occurrence 0x2c --mode counter
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deflect_capture import read_capture  # noqa: E402  (same-dir helper)

RESULT_FLAG_OFFSET = 0xE10

# ---------------------------------------------------------------------------
# Event rules -- deliberately identical to
# include/sekiro_haptics/process/GuardOutcomeEventDetector.hpp.
# tests/test_deflect_occurrence.py runs the same scenario table as
# tests/test_guard_outcome_event_detector.cpp so the two cannot drift.
# ---------------------------------------------------------------------------
DEFLECT, BLOCK, UNRESOLVED = "deflect", "block", "unresolved"


@dataclass
class Event:
    kind: str
    timestamp_us: int
    generation: int
    occurrence: int
    delta: int
    outcome: int
    reason: str = ""


@dataclass
class Detector:
    mode: str = "counter"          # counter | pulse | identifier
    counter_bits: int = 32
    outcome_settle_us: int = 0

    _base: bool = field(default=False, init=False)
    _gen: int = field(default=0, init=False)
    _last: int = field(default=0, init=False)
    _pending: Event | None = field(default=None, init=False)
    _pend_outcome: int = field(default=0, init=False)
    _pend_since: int = field(default=0, init=False)

    def _step(self, now: int):
        if self.mode == "counter":
            if now == self._last:
                return None
            mask = (1 << (self.counter_bits or 64)) - 1
            return (now - self._last) & mask
        if self.mode == "pulse":
            return 1 if (self._last == 0 and now != 0) else None
        if self.mode == "identifier":
            return 1 if (now != self._last and now != 0) else None
        raise ValueError(f"unknown mode {self.mode}")

    def _flush(self, reason, out):
        if self._pending is not None:
            ev = self._pending
            ev.kind, ev.reason = UNRESOLVED, reason
            out.append(ev)
            self._pending = None

    @staticmethod
    def _classify(ev: Event):
        if ev.outcome == 1:
            ev.kind = DEFLECT
        elif ev.outcome == 0:
            ev.kind = BLOCK
        else:
            ev.kind, ev.reason = UNRESOLVED, "outcome_out_of_range"

    def update(self, ts, read_ok, generation, continuity_break, occurrence, outcome):
        out: list[Event] = []
        if not read_ok:
            self._flush("continuity_break", out)
            self._base = False
            return out
        if continuity_break or not self._base or generation != self._gen:
            if self._base and (continuity_break or generation != self._gen):
                self._flush("continuity_break", out)
            self._base, self._gen, self._last = True, generation, occurrence
            self._pending = None
            return out

        delta = self._step(occurrence)
        if self._pending is not None:
            if delta is not None:
                self._flush("outcome_never_settled", out)
            elif outcome != self._pend_outcome:
                self._pend_outcome, self._pend_since = outcome, ts
            elif ts - self._pend_since >= self.outcome_settle_us:
                ev = self._pending
                ev.outcome = outcome
                self._classify(ev)
                out.append(ev)
                self._pending = None

        if delta is not None:
            ev = Event(UNRESOLVED, ts, generation, occurrence, delta, outcome)
            if self.mode == "counter" and delta > 1:
                ev.reason = "counter_jump"
                out.append(ev)
            elif self.outcome_settle_us > 0:
                self._pending, self._pend_outcome, self._pend_since = ev, outcome, ts
            else:
                self._classify(ev)
                out.append(ev)
        self._last = occurrence
        return out

    def finish(self):
        out: list[Event] = []
        self._flush("outcome_never_settled", out)
        return out


# ---------------------------------------------------------------------------


def load(path):
    cap = read_capture(path)
    size = cap.header["windowSizeBytes"]
    if size < RESULT_FLAG_OFFSET + 1:
        raise SystemExit(f"capture window 0x{size:x} does not cover +0x{RESULT_FLAG_OFFSET:x}")
    return cap, size


# ---------------------------------------------------------------------------
# Streaming reader. tools/deflect_capture.py stays the strict validator, but it
# materialises every reconstructed frame and refuses past 128 MiB, which a long
# multi-module recording blows through immediately (60k samples x 10 KiB).
# This walks the same records while holding only the current window, and keeps
# the continuity rules that matter for analysis: a delta must match the bytes
# reconstructed so far, and every gap/dropped/discontinuity starts a new segment
# that must be re-baselined before any further delta is accepted.
# ---------------------------------------------------------------------------
def stream_samples(path):
    """Yields (timestamp_us, generation, segment, data:bytearray). `data` is
    reused between yields -- copy it if you need to keep one."""
    header = None
    data = None
    generation = 0
    segment = 0
    size = 0
    pending_baseline = False
    annotations = []
    with open(path, "r", encoding="utf-8") as fh:
        for number, line in enumerate(fh, 1):
            if not line.strip():
                continue
            r = json.loads(line)
            if r.get("schemaVersion") != 3:
                raise SystemExit(f"line {number}: requires capture schema v3")
            kind = r.get("recordKind")
            if header is None:
                if kind != "capture_start":
                    raise SystemExit("missing capture_start")
                header = r
                size = r["windowSizeBytes"]
                continue
            if kind == "marker":
                annotations.append(r)
                continue
            if kind == "baseline":
                generation = r["generation"]
                data = bytearray.fromhex(r["bytesHex"])
                if len(data) != size:
                    raise SystemExit(f"line {number}: baseline size mismatch")
                pending_baseline = r.get("reason") == "rebaseline"
                if not pending_baseline:
                    yield r["timestampUs"], generation, segment, data
                continue
            if kind == "delta":
                if data is None:
                    raise SystemExit(f"line {number}: delta before baseline")
                off = r["offset"]
                before = bytes.fromhex(r["previousBytesHex"])
                after = bytes.fromhex(r["currentBytesHex"])
                if data[off:off + 4] != before:
                    raise SystemExit(f"line {number}: delta does not match reconstructed bytes")
                data[off:off + 4] = after
                continue
            if kind == "sample":
                if data is None:
                    raise SystemExit(f"line {number}: sample before baseline")
                pending_baseline = False
                yield r["timestampUs"], r["generation"], segment, data
                continue
            if kind in ("dropped", "gap", "discontinuity"):
                if kind == "discontinuity":
                    generation = r["newGeneration"]
                data = None
                segment += 1
                continue
            if kind == "capture_end":
                break
            raise SystemExit(f"line {number}: unknown record kind {kind}")
    stream_samples.annotations = annotations
    stream_samples.header = header


def _layout(path):
    lp = Path(str(path) + ".layout.json")
    if not lp.exists():
        return None
    return json.loads(lp.read_text(encoding="utf-8"))["modules"]


def _where(layout, off):
    if not layout:
        return ""
    for m in layout:
        if m["windowOffset"] <= off < m["windowOffset"] + m["sizeBytes"]:
            return f"{m['name']}+0x{off - m['windowOffset']:x}"
    return ""


def cmd_find(args):
    layout = _layout(args.capture)
    counts = {}
    first = {}
    last = {}
    inc_one = {}
    seg_of = {}
    per_phase = {}
    phase_bounds = []
    prev = None
    n = 0
    t_first = t_last = None
    outcome_transitions = 0
    prev_outcome = None

    # markers are only known after the stream ends, so collect timestamps first
    marks = []
    with open(args.capture, "r", encoding="utf-8") as fh:
        for line in fh:
            if '"marker"' in line:
                r = json.loads(line)
                marks.append((r["timestampUs"], r["label"]))
    for i, (t, lbl) in enumerate(marks):
        end_t = marks[i + 1][0] if i + 1 < len(marks) else float("inf")
        phase_bounds.append((t, end_t, lbl))

    def phase_at(t):
        for i, (a, b, _l) in enumerate(phase_bounds):
            if a <= t < b:
                return i
        return -1

    for ts, gen, seg, data in stream_samples(args.capture):
        n += 1
        if t_first is None:
            t_first = ts
        t_last = ts
        oc = data[RESULT_FLAG_OFFSET]
        if prev_outcome is not None and oc != prev_outcome:
            outcome_transitions += 1
        prev_outcome = oc
        ph = phase_at(ts)
        if prev is None or len(prev) != len(data):
            prev = bytearray(data)
            continue
        if prev != data:
            for off in range(0, len(data) - 3, 4):
                if prev[off:off + 4] != data[off:off + 4]:
                    a = int.from_bytes(prev[off:off + 4], "little")
                    b = int.from_bytes(data[off:off + 4], "little")
                    counts[off] = counts.get(off, 0) + 1
                    first.setdefault(off, a)
                    last[off] = b
                    if ((b - a) & 0xFFFFFFFF) == 1:
                        inc_one[off] = inc_one.get(off, 0) + 1
                    if ph >= 0:
                        per_phase.setdefault(off, {})
                        per_phase[off][ph] = per_phase[off].get(ph, 0) + 1
            prev[:] = data

    print(f"samples={n}  duration={(t_last - t_first)/1e6:.1f}s")
    print(f"outcome(+0x{RESULT_FLAG_OFFSET:x}) transitions = {outcome_transitions}")
    print(f"markers = {len(marks)}")
    for i, (a, b, lbl) in enumerate(phase_bounds):
        span = "" if b == float("inf") else f"-{b/1e6:.1f}s"
        print(f"   phase {i}: {lbl}  {a/1e6:.1f}s{span}")
    print(f"\nchanged 4-byte cells: {len(counts)}\n")

    if args.expect:
        want = [int(x) for x in args.expect.split(",")]
        print(f"기대 발생 횟수 (구간별): {want}")
        exact = []
        for off, ph in per_phase.items():
            got = [ph.get(i, 0) for i in range(len(want))]
            if got == want:
                exact.append(off)
        print(f"구간별 변화 횟수가 정확히 일치하는 셀: {len(exact)}\n")
        for off in exact[:args.top]:
            print(f"  +0x{off:<5x} {_where(layout, off):40s} "
                  f"+1steps={inc_one.get(off,0)}/{counts[off]} "
                  f"first={first[off]} last={last[off]}")
        if not exact:
            print("  정확히 일치하는 셀 없음 -- 아래 근접 후보를 보세요\n")

    ranked = sorted(counts, key=lambda o: -counts[o])
    print(f"\n{'offset':>8} {'changes':>8} {'+1':>6}  module+off")
    for off in ranked[:args.top]:
        ph = per_phase.get(off, {})
        dist = " ".join(str(ph.get(i, 0)) for i in range(len(phase_bounds)))
        mark = "  <-- OUTCOME BYTE" if off == RESULT_FLAG_OFFSET else ""
        print(f"  +0x{off:<5x} {counts[off]:8d} {inc_one.get(off,0):6d}  "
              f"{_where(layout, off):38s} [{dist}]{mark}")
    print("\n[] 안은 구간별 변화 횟수입니다. 게임 의미를 확정한 것이 아닙니다.")
    return 0


def cmd_replay(args):
    layout = _layout(args.capture)
    det = Detector(mode=args.mode, counter_bits=args.counter_bits,
                   outcome_settle_us=args.settle_us)
    events = []
    prev_seg = None
    n = 0
    for ts, gen, seg, data in stream_samples(args.capture):
        n += 1
        brk = prev_seg is not None and seg != prev_seg
        prev_seg = seg
        occ = 0
        if args.occurrence is not None:
            occ = int.from_bytes(data[args.occurrence:args.occurrence + 4], "little")
        events += det.update(ts, True, gen, brk, occ, data[RESULT_FLAG_OFFSET])
    events += det.finish()

    counts = {DEFLECT: 0, BLOCK: 0, UNRESOLVED: 0}
    for e in events:
        counts[e.kind] += 1
    print(f"samples={n}")
    print(f"occurrence = " + (f"+0x{args.occurrence:x} [{_where(layout, args.occurrence)}] "
                              f"mode={args.mode}" if args.occurrence is not None else "NONE"))
    print(f"events: deflect={counts[DEFLECT]} block={counts[BLOCK]} "
          f"unresolved={counts[UNRESOLVED]}\n")
    for e in events:
        extra = f"  reason={e.reason}" if e.reason else ""
        print(f"  [{e.timestamp_us/1e6:9.3f}s] {e.kind:10s} occ={e.occurrence} "
              f"d={e.delta} outcome={e.outcome}{extra}")
    if args.occurrence is None:
        print("\n주의: occurrence 없이 재생하면 이벤트가 0개입니다. 정상입니다 --")
        print("결과값만으로는 같은 결과의 반복을 구분할 수 없다는 것이 이 작업의 전제입니다.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("find", help="rank occurrence-signal candidates")
    f.add_argument("capture", type=Path)
    f.add_argument("--top", type=int, default=30)
    f.add_argument("--expect", type=str, default=None,
                   help="구간별 기대 발생 횟수, 쉼표 구분 (예: 0,5,0,5,0,5,0,0,0,2,0)")
    f.set_defaults(func=cmd_find)

    r = sub.add_parser("replay", help="replay a capture into a guard event log")
    r.add_argument("capture", type=Path)
    r.add_argument("--occurrence", type=lambda x: int(x, 0), default=None)
    r.add_argument("--mode", choices=("counter", "pulse", "identifier"), default="counter")
    r.add_argument("--counter-bits", type=int, default=32)
    r.add_argument("--settle-us", type=int, default=0)
    r.set_defaults(func=cmd_replay)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
