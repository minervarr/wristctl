"""Golden-vector tests for the generated Python codecs.

The same JSON files drive the C test in
``libs/firstparty/wristctl_core/tests``. A vector is authoritative:
if the codec and the vector disagree, the codec is wrong.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "libs" / "firstparty" / "wristctl_core"
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto.generated import codecs  # noqa: E402

VECTOR_DIR = CORE / "spec" / "vectors"


def load_vectors() -> list[dict]:
    return [json.loads(p.read_text(encoding="utf-8")) for p in sorted(VECTOR_DIR.glob("*.json"))]


class VectorTests(unittest.TestCase):
    def assert_matches(self, got, expected, path: str) -> None:
        if isinstance(expected, dict):
            for key, value in expected.items():
                self.assertTrue(hasattr(got, key), f"{path}.{key}: missing field")
                self.assert_matches(getattr(got, key), value, f"{path}.{key}")
            return
        if isinstance(expected, list):
            self.assertEqual(len(got), len(expected), f"{path}: list length")
            for index, value in enumerate(expected):
                self.assert_matches(got[index], value, f"{path}[{index}]")
            return
        if isinstance(got, bytes) and isinstance(expected, str):
            self.assertEqual(got.hex(), expected, f"{path}: bytes")
            return
        self.assertEqual(got, expected, f"{path}: value")

    def test_vectors(self) -> None:
        vectors = load_vectors()
        self.assertGreaterEqual(len(vectors), 10)
        for vector in vectors:
            with self.subTest(vector=vector["id"]):
                raw = bytes.fromhex(vector["bytes"])
                decoded = codecs.decode_frame(raw)
                self.assert_matches(decoded, vector["decoded"], vector["id"])
                self.assertEqual(
                    codecs.encode_frame(decoded),
                    raw,
                    f"{vector['id']}: round-trip must reproduce the frame",
                )


class ErrorTests(unittest.TestCase):
    def test_short_frame(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.decode_current_time(b"\x09\xea\x07")

    def test_wrong_class(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.decode_current_time(bytes.fromhex("0aea070a1b10290902c401"))

    def test_constant_byte(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.decode_current_time(bytes.fromhex("09ea070a1b10290902c400"))

    def test_alarm_constant_byte(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.decode_alarm_first(bytes([0x15, 0x40, 0x00, 7, 30]))

    def test_range_on_encode(self) -> None:
        value = codecs.CurrentTime(2026, 13, 27, 16, 41, 9, 2, 0)
        with self.assertRaises(codecs.ProtoError):
            codecs.encode_current_time(value)

    def test_bcd_range_on_encode(self) -> None:
        value = codecs.ProgramTimer(
            repeat=0,
            workouts=[codecs.ProgramTimerWorkoutsItem(100, 0, 0)]
            + [codecs.ProgramTimerWorkoutsItem(0, 0, 0)] * 5,
        )
        with self.assertRaises(codecs.ProtoError):
            codecs.encode_program_timer(value)

    def test_string_too_long(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.encode_watch_name(codecs.WatchName("X" * 20))

    def test_unknown_class(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.decode_frame(bytes([0x7F, 0, 0]))

    def test_empty_frame(self) -> None:
        with self.assertRaises(codecs.ProtoError):
            codecs.decode_frame(b"")


class MetadataTests(unittest.TestCase):
    def test_class_info_matches_spec(self) -> None:
        spec = json.loads((CORE / "spec" / "classes.json").read_text(encoding="utf-8"))
        for cls in spec["classes"]:
            info = codecs.CLASS_INFO[cls["id"]]
            self.assertEqual(info["name"], cls["name"])
            self.assertEqual(info["length"], cls["length"]["max"])
            self.assertEqual(info["direction"], cls["direction"])

    def test_every_class_has_a_vector(self) -> None:
        spec = json.loads((CORE / "spec" / "classes.json").read_text(encoding="utf-8"))
        covered = {v["class"] for v in load_vectors()}
        missing = sorted(cls["name"] for cls in spec["classes"] if cls["name"] not in covered)
        self.assertEqual(missing, [], f"classes without a golden vector: {missing}")

    def test_vectors_cover_every_field(self) -> None:
        spec = json.loads((CORE / "spec" / "classes.json").read_text(encoding="utf-8"))
        by_name = {cls["name"]: cls for cls in spec["classes"]}

        def walk(fields: list[dict], decoded: dict, path: str) -> None:
            for field in fields:
                if "expected" in field:
                    continue
                self.assertIn(field["name"], decoded, f"{path}.{field['name']} missing from vector")
                if field["type"] == "array":
                    for index, item in enumerate(decoded[field["name"]]):
                        walk(field["fields"], item, f"{path}.{field['name']}[{index}]")

        for vector in load_vectors():
            with self.subTest(vector=vector["id"]):
                walk(by_name[vector["class"]]["fields"], vector["decoded"], vector["id"])


if __name__ == "__main__":
    unittest.main()
