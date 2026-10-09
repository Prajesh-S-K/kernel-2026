"""Native overlay properties. Needs PyObjC (the overlay's own environment): skipped everywhere else.

.venv-overlay/bin/python -m unittest tests.test_overlay_native
"""

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

try:
    from overlay import app
except Exception:  # PyObjC (or macOS) missing
    app = None


@unittest.skipIf(app is None, "PyObjC is not installed in this environment")
class NativePanel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.result = app.selftest(cls.directory.name)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_the_panel_floats_never_takes_focus_and_absorbs_clicks(self):
        r = self.result
        self.assertGreaterEqual(r["level"], 25, "must float above ordinary windows")
        self.assertTrue(r["floating"])
        self.assertTrue(r["nonactivating"])
        self.assertFalse(r["canBecomeKey"])
        self.assertFalse(r["canBecomeMain"])
        self.assertFalse(
            r["ignoresMouseEvents"], "clicks must be absorbed by the overlay, not passed through"
        )
        self.assertFalse(r["hidesOnDeactivate"])

    def test_it_follows_spaces_and_may_sit_above_full_screen_apps(self):
        self.assertTrue(self.result["canJoinAllSpaces"])
        self.assertTrue(self.result["fullScreenAuxiliary"])

    def test_every_state_renders(self):
        self.assertEqual(len(self.result["png"]), 8)
        for path in self.result["png"]:
            self.assertGreater(Path(path).stat().st_size, 2000, path)

    def test_the_real_displays_are_reported_in_points(self):
        self.assertGreaterEqual(len(self.result["screens"]), 1)
        for screen in self.result["screens"]:
            self.assertGreater(screen["visible"][2], 300)
        for scale in self.result["scale"]:
            self.assertIn(scale, (1.0, 2.0, 3.0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
