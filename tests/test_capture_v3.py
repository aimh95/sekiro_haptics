import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from deflect_capture import CaptureError, read_capture

WRITER = None
if "--writer" in sys.argv:
    i = sys.argv.index("--writer")
    WRITER = sys.argv[i+1]
    del sys.argv[i:i+2]


def records():
    common = {"schemaVersion":3}
    return [dict(common,recordKind="capture_start",timestampUs=0,windowSizeBytes=4,intervalUs=10000,interpretation="unvalidated_raw_memory"),
            dict(common,recordKind="baseline",timestampUs=0,generation=1,baseAddressHex="0x1234",reason="initial",bytesHex="00000000"),
            dict(common,recordKind="sample",timestampUs=10000,scheduledTimestampUs=10000,sequence=0,generation=1,status="observed"),
            dict(common,recordKind="delta",timestampUs=20000,scheduledTimestampUs=20000,sequence=1,generation=1,offset=0,cellSizeBytes=4,previousBytesHex="00000000",currentBytesHex="efbeadde"),
            dict(common,recordKind="sample",timestampUs=20000,scheduledTimestampUs=20000,sequence=1,generation=1,status="observed"),
            dict(common,recordKind="capture_end",timestampUs=20000,samplesTaken=2)]


class CaptureV3Tests(unittest.TestCase):
    def parse(self, data):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/"capture.jsonl"
            p.write_text("".join(json.dumps(r)+"\n" for r in data))
            return read_capture(p)

    def test_unchanged_commits_and_high_bits_survive(self):
        c = self.parse(records())
        self.assertEqual([s.data.hex() for s in c.samples],["00000000","00000000","efbeadde"])
        self.assertEqual(len(c.sha256),64)

    def test_missing_footer_is_not_silently_accepted(self):
        with self.assertRaises(CaptureError): self.parse(records()[:-1])

    def test_uncommitted_delta_rejected(self):
        r = records(); del r[-2]
        with self.assertRaises(CaptureError): self.parse(r)

    def test_wrong_prior_bytes_rejected(self):
        r = records(); r[3]["previousBytesHex"] = "01000000"
        with self.assertRaises(CaptureError): self.parse(r)

    def test_malformed_hex_rejected(self):
        r = records(); r[3]["currentBytesHex"] = "zzzzzzzz"
        with self.assertRaises(CaptureError): self.parse(r)

    def test_duplicate_delta_not_counted_twice(self):
        r = records(); r.insert(4,copy.deepcopy(r[3]))
        with self.assertRaises(CaptureError): self.parse(r)

    def test_sequence_hole_rejected(self):
        r = records(); r[2]["sequence"] = 2
        with self.assertRaises(CaptureError): self.parse(r)

    def test_bool_timestamp_rejected(self):
        r = records(); r[2]["timestampUs"] = True
        with self.assertRaises(CaptureError): self.parse(r)

    def test_missing_rebaseline_after_gap_rejected(self):
        r = records(); r.insert(3,{"schemaVersion":3,"recordKind":"gap","timestampUs":20000,"fromTimestampUs":10000,"missedCycles":1})
        with self.assertRaises(CaptureError): self.parse(r)

    def test_duplicate_json_key_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/"bad.jsonl"
            p.write_text('{"schemaVersion":3,"schemaVersion":3}\n')
            with self.assertRaises(CaptureError): read_capture(p)

    def test_cpp_writer_round_trip(self):
        if WRITER is None: self.skipTest("pass --writer <capture_v3_fixture> for C++ integration")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"writer.jsonl"
            subprocess.run([WRITER,str(path)],check=True)
            capture = read_capture(path)
            self.assertEqual(capture.counts["sample"],4)
            self.assertEqual(capture.counts["dropped"],1)
            self.assertEqual(capture.counts["delta"],1)
            self.assertEqual(capture.counts["gap"],1)
            self.assertEqual(capture.samples[2].data[:4].hex(),"efbeadde")
            self.assertEqual(capture.samples[3].data[:4].hex(),"01beadde")
            self.assertNotEqual(capture.samples[2].segment,capture.samples[3].segment)
            before = path.read_bytes()
            self.assertNotEqual(subprocess.run([WRITER,str(path)]).returncode,0)
            self.assertEqual(path.read_bytes(),before)


if __name__ == "__main__": unittest.main()
