from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from wristctl_build import paths, variants  # noqa: E402
from wristctl_build.errors import BuildError  # noqa: E402


class BuildDirTests(unittest.TestCase):
    def test_debug_dir(self) -> None:
        root = Path("/repo")
        self.assertEqual(paths.build_dir(root, debug=True), root / "build/linux_debug")

    def test_release_dir(self) -> None:
        root = Path("/repo")
        self.assertEqual(paths.build_dir(root), root / "build/linux")

    def test_native_dir(self) -> None:
        root = Path("/repo")
        self.assertEqual(
            paths.build_dir(root, arch="native"), root / "build/linux_native"
        )
        self.assertEqual(
            paths.build_dir(root, arch="native", debug=True),
            root / "build/linux_native_debug",
        )

    def test_custom_arch_dir(self) -> None:
        root = Path("/repo")
        self.assertEqual(
            paths.build_dir(root, arch="v3", debug=True),
            root / "build/linux_custom-v3_debug",
        )


class BinaryTests(unittest.TestCase):
    def test_cli_and_gui_paths(self) -> None:
        build = Path("/repo/build/linux_debug")
        self.assertEqual(
            paths.cli_binary_path(build),
            Path("/repo/build/linux_debug/platform/linux/wristctl_cli"),
        )
        self.assertEqual(
            paths.gui_binary_path(build),
            Path("/repo/build/linux_debug/gui/wristctl_gui"),
        )


class RootTests(unittest.TestCase):
    def test_find_repo_root_from_this_file(self) -> None:
        root = paths.find_repo_root(Path(__file__))
        self.assertTrue((root / "CMakeLists.txt").is_file())
        self.assertTrue((root / "manifest.json").is_file())

    def test_version_comes_from_manifest(self) -> None:
        root = paths.find_repo_root(Path(__file__))
        self.assertEqual(paths.version(root), paths.load_manifest(root)["version"])

    def test_display_shortens_paths_under_root(self) -> None:
        root = Path("/repo")
        self.assertEqual(
            paths.display(root, Path("/repo/build/linux")), "build/linux"
        )
        self.assertEqual(paths.display(root, Path("/elsewhere/x")), "/elsewhere/x")


class ArchTests(unittest.TestCase):
    def test_suffixes(self) -> None:
        self.assertEqual(variants.arch_suffix(""), "")
        self.assertEqual(variants.arch_suffix("native"), "_native")
        self.assertEqual(variants.arch_suffix("v3"), "_custom-v3")

    def test_march_values(self) -> None:
        self.assertEqual(variants.march_value("v3"), "x86-64-v3")
        self.assertEqual(variants.march_value("native"), "native")
        self.assertEqual(variants.march_value("znver4"), "znver4")

    def test_cmake_args(self) -> None:
        self.assertEqual(variants.cmake_args(""), ())
        self.assertIn("-DCMAKE_C_FLAGS=-march=native", variants.cmake_args("native"))

    def test_sanitize_arch(self) -> None:
        self.assertEqual(variants.sanitize_arch(" native "), "native")
        self.assertEqual(variants.sanitize_arch("znver4"), "znver4")
        with self.assertRaises(BuildError):
            variants.sanitize_arch("../etc")
        with self.assertRaises(BuildError):
            variants.sanitize_arch("")


if __name__ == "__main__":
    unittest.main()
