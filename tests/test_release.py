"""Prove that packaging refuses dirty source, stale source and modified binaries."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts import release


class ReleaseTests(unittest.TestCase):
    def test_dirty_stale_and_hash_mismatch_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("build/\n.pio/\n")
            (root / "VERSION").write_text("0.2.0\n")
            (root / "source.cpp").write_text("original\n")
            for name in release.artifact_paths():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"verified")

            def commit():
                subprocess.run(["git", "add", "."], cwd=root, check=True)
                subprocess.run(
                    [
                        "git",
                        "-c",
                        "user.name=NodX test",
                        "-c",
                        "user.email=test@localhost",
                        "commit",
                        "-qm",
                        "fixture",
                    ],
                    cwd=root,
                    check=True,
                )

            commit()
            release.stamp(root)
            release.verify(root)
            (root / "source.cpp").write_text("changed\n")
            with self.assertRaisesRegex(ValueError, "dirty"):
                release.verify(root)
            commit()
            with self.assertRaisesRegex(ValueError, "changed after"):
                release.verify(root)
            release.stamp(root)
            (root / "build/nodx_sim").write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "artifact"):
                release.verify(root)
