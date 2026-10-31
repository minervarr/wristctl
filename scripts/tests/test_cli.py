from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENV = {
    **os.environ,
    "PYTHONPATH": str(ROOT / "scripts" / "src"),
}


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-m", "wristctl_build", *args],
        cwd=ROOT,
        env=ENV,
        stdin=subprocess.DEVNULL,
        text=True,
        capture_output=True,
        check=False,
    )


class CliTests(unittest.TestCase):
    def test_version(self) -> None:
        result = run("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("0.1.0", result.stdout)

    def test_debug_dry_run_uses_linux_debug(self) -> None:
        result = run("build", "--debug", "--dry-run", "--no-gui")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout
        self.assertIn("build/linux_debug", output)
        self.assertIn("cmake", output)
        self.assertIn("-DWRISTCTL_BUILD_TESTS=ON", output)
        self.assertIn("-DWRISTCTL_BUILD_GUI=OFF", output)
        self.assertIn("Building", output)

    def test_release_dry_run_uses_linux(self) -> None:
        result = run("build", "--release", "--dry-run", "--no-gui")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("build/linux ", result.stdout.replace("\n", " "))
        self.assertIn("-DWRISTCTL_BUILD_TESTS=OFF", result.stdout)

    def test_native_dry_run(self) -> None:
        result = run("build", "--release", "--arch", "native", "--dry-run", "--no-gui")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("build/linux_native", result.stdout)
        self.assertIn("-march=native", result.stdout)

    def test_test_command_runs_both_suites(self) -> None:
        result = run("test", "--dry-run", "--no-gui")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ctest --test-dir", result.stdout)
        self.assertIn("unittest discover -s py/tests", result.stdout)

    def test_test_rejects_release(self) -> None:
        result = run("test", "--release", "--dry-run", "--no-gui")
        self.assertEqual(result.returncode, 2)
        self.assertIn("Debug-only", result.stderr)

    def test_menu_needs_a_terminal(self) -> None:
        result = run("menu")
        self.assertEqual(result.returncode, 2)
        self.assertIn("needs a terminal", result.stderr)


if __name__ == "__main__":
    unittest.main()
