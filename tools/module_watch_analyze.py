#!/usr/bin/env python3
"""Offline ranking of tools/module_watch.py output.

A 2-minute watch produces thousands of byte changes, almost all of them timers,
animation blend weights and physics floats that churn every frame. A candidate
for "which prosthetic is selected" or "a wire action started" looks nothing like
that: it is STILL almost all the time, takes a SMALL number of distinct values,
and moves only around the moments you marked.

So this ranks each (module, offset) by exactly that, and prints what the field
actually held. It decides nothing -- a high rank is a thing to go and test in a
second session, not a confirmed field.

Usage:
  python tools/module_watch_analyze.py capture.jsonl
  python tools/module_watch_analyze.py capture.jsonl --near-marker 1.5 --top 40
"""
from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path


def main() -> int:
    import sys
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("--near-marker", type=float, default=2.0,
                    help="seconds after a marker still counted as 'near' (default 2.0)")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--max-changes", type=int, default=60,
                    help="ignore offsets that changed more than this (pure churn)")
    args = ap.parse_args()

    markers = []
    changes = defaultdict(list)     # (module, offset) -> [(t, old, new)]
    widths = {}
    generations = []
    drops = 0

    for line in args.capture.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        kind = rec.get("type")
        if kind == "marker":
            markers.append(rec["t"])
        elif kind == "dropped":
            drops += 1
        elif kind == "generation":
            generations.append(rec["t"])
        elif kind == "change":
            key = (rec["module"], rec["offset"])
            changes[key].append((rec["t"], rec["old"], rec["new"]))
            widths[key] = max(widths.get(key, 0), len(rec["new"]) // 2)

    print(f"{args.capture.name}: {sum(len(v) for v in changes.values())} changes across "
          f"{len(changes)} offsets, {len(markers)} markers, {drops} drops, "
          f"{len(generations)} generation(s)")
    if markers:
        print("markers at: " + ", ".join(f"{m:.2f}s" for m in markers))
    print()

    def near_marker(t):
        return any(0.0 <= t - m <= args.near_marker for m in markers)

    scored = []
    for key, events in changes.items():
        if len(events) > args.max_changes:
            continue
        values = []
        for _t, old, new in events:
            if not values:
                values.append(old)
            values.append(new)
        distinct = sorted(set(values))
        hits = sum(1 for t, _o, _n in events if near_marker(t))
        # Prefer: few distinct values, most changes explained by a marker.
        coverage = hits / len(events)
        score = (coverage, -len(distinct), -len(events))
        scored.append((score, key, events, distinct, hits))

    scored.sort(reverse=True)
    if not scored:
        print("nothing quiet enough to rank -- every offset changed more than "
              f"--max-changes ({args.max_changes}).")
        return 0

    for _score, (module, offset), events, distinct, hits in scored[:args.top]:
        width = widths[(module, offset)]
        print(f"{module} +0x{offset:x}  {width}B   changes={len(events)}  "
              f"nearMarker={hits}/{len(events)}  distinct={len(distinct)}")
        shown = distinct[:8]
        print("    values: " + ", ".join(shown) + (" ..." if len(distinct) > 8 else ""))
        for t, old, new in events[:10]:
            flag = "*" if near_marker(t) else " "
            print(f"    {flag} {t:8.3f}  {old} -> {new}")
        if len(events) > 10:
            print(f"      ... {len(events) - 10} more")
        print()

    print("A marker is when the USER pressed a key, not when the game acted. A high "
          "nearMarker ratio\nmeans 'worth testing next session', never 'confirmed'.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
