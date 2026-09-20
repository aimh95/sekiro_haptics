"""Synthetic capture-v3 fixtures. All values are invented, NOT Sekiro IDs.

python tests/deflect_signal_fixture.py <new-output-directory>
Creates separate discovery/evaluation inputs to exercise the analysis CLI.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import struct
import sys


def write_fixture(directory, session_id, *, hold_same_value=False, wrong_block_value=False):
    directory = Path(directory)
    common = {"schemaVersion": 3}
    def record(kind, timestamp, **fields):
        return dict(common, recordKind=kind, timestampUs=timestamp, **fields)
    rows = [record("capture_start", 0, windowSizeBytes=12, intervalUs=10000,
                   interpretation="unvalidated_raw_memory", scope="CustomAddress",
                   fixtureOrigin="synthetic_not_sekiro", fixtureSession=session_id),
            record("baseline", 0, generation=1, baseAddressHex="0x1234", reason="initial",
                   bytesHex="00"*12)]
    transitions = {60000: 900000001, 70000: 0, 90000: 900000001, 100000: 0,
                   120000: 900000001, 130000: 0, 180000: 900000002, 190000: 0,
                   200000: 900000002, 210000: 0, 260000: 900000003, 270000: 0,
                   340000: 900000004, 360000: 0, 420000: 900000005, 430000: 0}
    if hold_same_value:
        del transitions[70000]
        del transitions[100000]
    if wrong_block_value:
        transitions[180000] = transitions[200000] = 900000001
    before = b"\x00"*12
    value, deflect_counter = 0, 0
    for sequence, t in enumerate(range(10000, 510000, 10000)):
        value = transitions.get(t, value)
        if t in {60000, 90000, 120000}:
            deflect_counter += 1
        # Offset 8 is a noisy timer that changes everywhere, including negatives.
        after = struct.pack("<III", value, deflect_counter, sequence+1)
        identity = dict(sequence=sequence, scheduledTimestampUs=t, generation=1)
        for offset in range(0, 12, 4):
            if before[offset:offset+4] != after[offset:offset+4]:
                rows.append(record("delta", t, **identity, offset=offset, cellSizeBytes=4,
                                   previousBytesHex=before[offset:offset+4].hex(),
                                   currentBytesHex=after[offset:offset+4].hex()))
        rows.append(record("sample", t, **identity, status="observed"))
        before = after
        if t == 340000:
            # Deliberately misleading button/manual marker. Must be ignored.
            rows.append(record("marker", t, processedTimestampUs=t,
                               source="manual_or_input_annotation", label="perfect_deflect"))
    rows.append(record("capture_end", 500000, samplesTaken=50))
    capture = directory / f"{session_id}.capture.jsonl"
    capture.write_text("".join(json.dumps(r)+"\n" for r in rows), encoding="utf-8")
    trials = [dict(id="rapid", label="player_deflect", startUs=40000, endUs=140000,
                   eventTimesUs=[60000, 90000, 120000]),
              dict(id="block", label="player_block", startUs=150000, endUs=220000,
                   eventTimesUs=[180000, 200000]),
              dict(id="enemy", label="enemy_deflect", startUs=240000, endUs=290000, eventTimesUs=[]),
              dict(id="empty", label="empty_guard", startUs=320000, endUs=380000, eventTimesUs=[]),
              dict(id="damage", label="damage", startUs=400000, endUs=450000, eventTimesUs=[])]
    manifest = {"schemaVersion": 1, "evidenceKind": "synthetic", "exeSha256": "0"*64,
                "regionKey": "synthetic-fixture-layout-v1-not-a-game-pointer-path",
                "sessions": [{"sessionId": session_id, "capture": capture.name,
                              "captureSha256": hashlib.sha256(capture.read_bytes()).hexdigest(),
                              "annotationSource": "synthetic_fixture",
                              "annotationEvidence": "invented fixture values and clock, no game or video",
                              "syncUncertaintyUs": 0, "trials": trials}]}
    path = directory / f"{session_id}.manifest.json"
    path.write_text(json.dumps(manifest, indent=2)+"\n", encoding="utf-8")
    return path, manifest


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python tests/deflect_signal_fixture.py <new-output-directory>")
    destination = Path(sys.argv[1])
    destination.mkdir(parents=True, exist_ok=False)
    write_fixture(destination, "synthetic-discovery")
    write_fixture(destination, "synthetic-heldout")
    print(f"Created synthetic fixtures in {destination}; not live Sekiro evidence.")
