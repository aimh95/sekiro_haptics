#!/usr/bin/env python3
"""Strict raw capture-v3 reconstruction. This tool does NOT detect deflects.

Usage: python tools/deflect_capture.py capture.jsonl [--offset 0x18 --type u32]
Successful unchanged samples are observations; a gap/drop breaks continuity.
All input and output markers remain human/input annotations, never ground truth.
"""
from __future__ import annotations
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys


class CaptureError(ValueError):
    pass


def integer(record, key, minimum=0, maximum=(1 << 63)-1):
    value = record.get(key)
    if type(value) is not int or not minimum <= value <= maximum:
        raise CaptureError(f"{key}: invalid integer")
    return value


def hex_bytes(value, size):
    if not isinstance(value, str) or len(value) != size*2 or not re.fullmatch(r"[0-9a-fA-F]+", value):
        raise CaptureError("invalid raw hex bytes")
    return bytes.fromhex(value)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise CaptureError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


@dataclass(frozen=True)
class Sample:
    timestamp_us: int
    sequence: int | None
    generation: int
    segment: int
    data: bytes


@dataclass
class Capture:
    sha256: str
    header: dict
    samples: list[Sample]
    annotations: list[dict]
    counts: dict


def read_capture(path: str | Path) -> Capture:
    samples, annotations = [], []
    counts = Counter()
    header = None
    data = None
    generation = None
    pending = None
    offsets = set()
    baseline_pending = False
    initial_seen = False
    sequence = 0
    segment = 0
    last_time = 0
    last_schedule = -1
    ended = False
    digest = hashlib.sha256()
    memory_used = 0
    with open(path, "rb") as stream:
        for number in range(1, 2_000_001):
            line = stream.readline(300_001)
            if not line:
                break
            digest.update(line)
            try:
                if len(line) > 300_000 or not line.endswith(b"\n"):
                    raise CaptureError("overlong or unterminated record (possibly interrupted capture)")
                r = json.loads(line, object_pairs_hook=unique_object,
                               parse_constant=lambda x: (_ for _ in ()).throw(CaptureError(f"nonfinite JSON: {x}")))
                if not isinstance(r, dict) or integer(r, "schemaVersion") != 3:
                    raise CaptureError("requires capture schema v3")
                kind = r.get("recordKind")
                t = integer(r, "timestampUs")
                if ended:
                    raise CaptureError("record after capture_end")
                counts[kind] += 1
                if header is None:
                    if kind != "capture_start":
                        raise CaptureError("missing capture_start")
                    size = integer(r, "windowSizeBytes", 4, 65536)
                    integer(r, "intervalUs", 1, 2_000_000)
                    if size % 4 or r.get("interpretation") != "unvalidated_raw_memory":
                        raise CaptureError("invalid raw capture header")
                    header = r
                    last_time = t
                    continue
                if kind == "marker":
                    if pending or baseline_pending:
                        raise CaptureError("annotation inside uncommitted sample")
                    if not isinstance(r.get("label"), str) or r.get("source") != "manual_or_input_annotation":
                        raise CaptureError("invalid annotation")
                    if integer(r,"processedTimestampUs") < t:
                        raise CaptureError("annotation processed before input")
                    annotations.append(r)
                    continue  # input timestamps may predate preceding sample records
                if t < last_time:
                    raise CaptureError("observation clock moved backwards")
                if kind == "baseline":
                    if data is not None or pending or baseline_pending:
                        raise CaptureError("unexpected baseline without a continuity break")
                    if not re.fullmatch(r"0x[0-9a-fA-F]{1,16}", r.get("baseAddressHex", "")) or int(r["baseAddressHex"],16) == 0:
                        raise CaptureError("invalid base address")
                    new_gen = integer(r,"generation",0,(1 << 64)-1)
                    if generation is not None and new_gen != generation:
                        raise CaptureError("generation changed without discontinuity")
                    generation = new_gen
                    data = bytearray(hex_bytes(r.get("bytesHex"),size))
                    if not initial_seen:
                        if r.get("reason") != "initial" or t != header["timestampUs"]:
                            raise CaptureError("missing initial baseline")
                        initial_seen = True
                        samples.append(Sample(t,None,generation,segment,bytes(data)))
                        memory_used += size
                        last_time = t
                    else:
                        if r.get("reason") != "rebaseline":
                            raise CaptureError("unexpected baseline reason")
                        baseline_pending = True
                        pending = (sequence,t,None,generation)
                    continue
                if not initial_seen:
                    raise CaptureError("record before initial baseline")
                if kind in ("delta", "sample"):
                    seq = integer(r,"sequence")
                    gen = integer(r,"generation",0,(1 << 64)-1)
                    scheduled = integer(r,"scheduledTimestampUs")
                    if seq != sequence or gen != generation or data is None:
                        raise CaptureError("missing baseline, wrong sequence, or wrong generation")
                    if scheduled > t or scheduled <= last_schedule:
                        raise CaptureError("invalid sample schedule")
                    identity = (seq,t,scheduled,gen)
                    if baseline_pending:
                        if kind != "sample" or pending != (seq,t,None,gen) or r.get("status") != "baseline":
                            raise CaptureError("rebaseline missing matching sample commit")
                    elif pending is not None and pending != identity:
                        raise CaptureError("delta group missing matching sample commit")
                    if kind == "delta":
                        pending = identity
                        offset = integer(r,"offset",0,size-4)
                        if integer(r,"cellSizeBytes") != 4 or offset % 4 or offset in offsets:
                            raise CaptureError("invalid or duplicate cell offset")
                        before = hex_bytes(r.get("previousBytesHex"),4)
                        after = hex_bytes(r.get("currentBytesHex"),4)
                        if data[offset:offset+4] != before or before == after:
                            raise CaptureError("delta does not match reconstructed prior bytes")
                        data[offset:offset+4] = after
                        offsets.add(offset)
                    else:
                        if not baseline_pending and r.get("status") != "observed":
                            raise CaptureError("invalid sample status")
                        samples.append(Sample(t,seq,gen,segment,bytes(data)))
                        memory_used += size
                        if memory_used > 128*1024*1024:
                            raise CaptureError("reconstruction exceeds 128 MiB; use shorter captures")
                        sequence += 1
                        last_schedule, last_time = scheduled,t
                        pending = None
                        baseline_pending = False
                        offsets.clear()
                    continue
                if pending or baseline_pending:
                    raise CaptureError("uncommitted sample before continuity record/end")
                if kind in ("dropped", "gap", "discontinuity"):
                    if kind == "dropped":
                        if integer(r,"sequence") != sequence or integer(r,"generation",0,(1 << 64)-1) != generation:
                            raise CaptureError("invalid dropped attempt")
                        if not isinstance(r.get("reason"),str):
                            raise CaptureError("missing drop reason")
                        sequence += 1
                    elif kind == "discontinuity":
                        if integer(r,"oldGeneration",0,(1 << 64)-1) != generation:
                            raise CaptureError("wrong previous generation")
                        generation = integer(r,"newGeneration",0,(1 << 64)-1)
                    else:
                        integer(r,"missedCycles",1)
                        if integer(r,"fromTimestampUs") > t:
                            raise CaptureError("gap starts in future")
                    data = None
                    segment += 1
                    last_time = t
                elif kind == "capture_end":
                    if integer(r,"samplesTaken") != sequence:
                        raise CaptureError("footer attempt count mismatch")
                    ended = True
                else:
                    raise CaptureError(f"unknown record kind: {kind}")
            except (ValueError, TypeError, KeyError) as exc:
                raise CaptureError(f"line {number}: {exc}") from exc
        else:
            raise CaptureError("record limit exceeded")
    if not ended or header is None:
        raise CaptureError("incomplete capture: no capture_end; not accepted for analysis")
    return Capture(digest.hexdigest(),header,samples,annotations,dict(counts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("--offset",type=lambda x: int(x,0))
    parser.add_argument("--type",choices=("u32","i32","f32"),default="u32")
    args = parser.parse_args()
    try:
        capture = read_capture(args.capture)
        if args.offset is None:
            print(json.dumps({"sha256":capture.sha256,"records":capture.counts,
                              "observedFramesIncludingInitial":len(capture.samples),
                              "interpretation":"unvalidated raw memory; annotations are not game detections"},indent=2))
        else:
            if args.offset < 0 or args.offset % 4 or args.offset+4 > capture.header["windowSizeBytes"]:
                raise CaptureError("offset must be aligned and inside the capture window")
            code = {"u32":"<I","i32":"<i","f32":"<f"}[args.type]
            for sample in capture.samples:
                raw = sample.data[args.offset:args.offset+4]
                value = struct.unpack(code,raw)[0]
                print(json.dumps({"timestampUs":sample.timestamp_us,"sequence":sample.sequence,
                                  "generation":sample.generation,"segment":sample.segment,
                                  "rawHex":raw.hex(),"value":str(value) if isinstance(value,float) and not math.isfinite(value) else value},allow_nan=False))
        return 0
    except (CaptureError,OSError) as exc:
        print(f"Error: {exc}",file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
