"""Tests for the generated model registry."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "libs" / "firstparty" / "wristctl_core"
sys.path.insert(0, str(ROOT / "py"))

from wristctl_proto.generated import models  # noqa: E402


class LookupTests(unittest.TestCase):
    def test_exact_name(self) -> None:
        model = models.lookup("CASIO GBX-100")
        self.assertIsNotNone(model)
        assert model is not None
        self.assertEqual(model.name, "GBX_100")
        self.assertEqual(model.generation, 22)

    def test_prefix_is_not_enough(self) -> None:
        # "GST-B100" is a substring of "GST-B1000"; the longest alias must win.
        self.assertEqual(models.lookup("CASIO GST-B1000").name, "GST_B1000")
        self.assertEqual(models.lookup("CASIO GST-B100").name, "GST_B100")

    def test_unknown(self) -> None:
        self.assertIsNone(models.lookup("ACME WATCH 3000"))
        self.assertIsNone(models.lookup(""))

    def test_every_model_resolves(self) -> None:
        spec = json.loads((CORE / "spec" / "models.json").read_text(encoding="utf-8"))
        for entry in spec["models"]:
            with self.subTest(model=entry["name"]):
                found = models.lookup("CASIO " + entry["aliases"][0])
                self.assertIsNotNone(found)
                assert found is not None
                self.assertEqual(found.name, entry["name"])


class FlagTests(unittest.TestCase):
    def test_registry_shape(self) -> None:
        self.assertEqual(len(models.FLAG_NAMES), 107)
        self.assertEqual(len(models.MODELS), 137)
        width = (len(models.FLAG_NAMES) + 7) // 8
        for model in models.MODELS:
            self.assertEqual(len(model.flags), width)

    def test_tide_is_gbx_100_only(self) -> None:
        tide = [m.name for m in models.MODELS if models.flag(m, "isEnableTideData")]
        self.assertEqual(tide, ["GBX_100"])

    def test_bond_split_inside_generation(self) -> None:
        self.assertFalse(models.flag(models.lookup("CASIO GBX-100"), "isNeedCreateBond"))
        self.assertFalse(models.flag(models.lookup("CASIO GBD-H1000"), "isNeedCreateBond"))
        self.assertTrue(models.flag(models.lookup("CASIO GBD-H2000"), "isNeedCreateBond"))

    def test_generation_shared_flags(self) -> None:
        # These use isEqualsModel(), so every model of generation 22 shares them.
        for name in ("CASIO GBX-100", "CASIO GBD-H1000", "CASIO DW-H5600"):
            model = models.lookup(name)
            assert model is not None
            self.assertTrue(models.flag(model, "isUseAllFeatures"), name)
            self.assertTrue(models.flag(model, "isEnableNotificationManager"), name)

    def test_unknown_flag(self) -> None:
        with self.assertRaises(KeyError):
            models.flag(models.MODELS[0], "notAFlag")

if __name__ == "__main__":
    unittest.main()
