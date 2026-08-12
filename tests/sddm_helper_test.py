#!/usr/bin/env python3
"""Focused, non-GUI checks for the SDDM installation helper.

The helper's privileged path is intentionally not exercised here.  These tests
only invoke its no-write ``--validate`` mode against temporary directories.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[1]
HELPER = REPO_ROOT / "tools" / "sddm" / "anispaper-sddm-install"


def run_helper(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(HELPER), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )


def write_valid_theme(path: Path) -> None:
    path.mkdir(parents=True)
    (path / "theme.conf").write_text(
        "[General]\nbackground=background.png\n", encoding="utf-8"
    )
    # The validator only requires a regular file; no image decoder is invoked.
    (path / "background.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    nested = path / "assets"
    nested.mkdir()
    (nested / "background-accent.png").write_bytes(b"asset")


class SddmHelperValidationTest(unittest.TestCase):
    def test_bash_syntax_is_valid(self) -> None:
        result = subprocess.run(
            ["bash", "-n", str(HELPER)], check=False, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_validate_accepts_a_regular_theme(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            stage = Path(tmp) / "anis-star"
            write_valid_theme(stage)

            result = run_helper("--validate", str(stage))

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("valid staged SDDM theme", result.stdout)

    def test_validate_rejects_missing_required_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            stage = Path(tmp) / "anis-star"
            write_valid_theme(stage)
            (stage / "background.png").unlink()

            result = run_helper("--validate", str(stage))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("background.png", result.stderr)

    def test_validate_rejects_symlinked_content(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            stage = Path(tmp) / "anis-star"
            write_valid_theme(stage)
            (stage / "linked-background").symlink_to(stage / "background.png")

            result = run_helper("--validate", str(stage))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("symlink", result.stderr)

    def test_validate_rejects_a_symlinked_theme_root(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            actual_stage = Path(tmp) / "actual-anis-star"
            stage_link = Path(tmp) / "anis-star"
            write_valid_theme(actual_stage)
            stage_link.symlink_to(actual_stage, target_is_directory=True)

            result = run_helper("--validate", str(stage_link))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not a symlink", result.stderr)

    def test_validate_rejects_special_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            stage = Path(tmp) / "anis-star"
            write_valid_theme(stage)
            os.mkfifo(stage / "unexpected.pipe")

            result = run_helper("--validate", str(stage))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("non-regular file", result.stderr)

    def test_validate_rejects_hard_linked_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            stage = Path(tmp) / "anis-star"
            write_valid_theme(stage)
            os.link(stage / "background.png", stage / "background-copy.png")

            result = run_helper("--validate", str(stage))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("hard-linked file", result.stderr)

    def test_production_copy_is_dropped_to_the_initiating_user(self) -> None:
        source = HELPER.read_text(encoding="utf-8")

        self.assertIn("resolve_initiator || die", source)
        self.assertIn('chown "$initiator_uid:$initiator_gid" -- "$work_dir"', source)
        self.assertIn('"$RUNUSER_BIN" --user "$initiator_user" --', source)
        self.assertIn('"$COPY_BIN" -R -P -- "$stage" "$destination"', source)
        self.assertIn('chown 0:0 -- "$work_dir"', source)
        self.assertNotIn('cp -R -P -- "$stage" "$work_dir"', source)

        handoff = source.index("handoff_work_directory || die")
        copy = source.index('copy_stage_as_initiator "$stage" "$work_dir"')
        reclaim = source.index("reclaim_work_directory || die")
        snapshot_validation = source.index('validate_theme "$snapshot"')
        self.assertLess(handoff, copy)
        self.assertLess(copy, reclaim)
        self.assertLess(reclaim, snapshot_validation)

    def test_normal_mode_requires_pkexec_context_before_any_write(self) -> None:
        with tempfile.TemporaryDirectory(prefix="anispaper-sddm-test-") as tmp:
            stage = Path(tmp) / "anis-star"
            write_valid_theme(stage)
            environment = os.environ.copy()
            environment.pop("PKEXEC_UID", None)

            result = subprocess.run(
                ["bash", str(HELPER), str(stage)],
                check=False,
                capture_output=True,
                text=True,
                env=environment,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(
                "root via pkexec" in result.stderr or "PKEXEC_UID" in result.stderr,
                result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
