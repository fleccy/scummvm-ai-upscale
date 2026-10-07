"""Tests for dists/remaster/sides/generate.py without the game or any AI model: room extraction and painting are
replaced by synthetic stand-ins (a random texture; a "painting" that is the room continued with more texture), the
real compositor (compose.py) runs on them.
    python -m unittest dists/remaster/tests/test_sides_generate.py      (needs numpy, pillow, opencv-python-headless)
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "sides"))
import generate as G  # noqa: E402

W, H, M = 640, 480, 104


def save_json(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def texture(w, h, seed):
    rng = np.random.default_rng(seed)
    small = rng.integers(0, 255, (h // 8, w // 8, 3), dtype=np.uint8)
    return np.asarray(Image.fromarray(small).resize((w, h), Image.BICUBIC))


class Fakes:
    """Stand-ins for the game and the models; counts paint calls."""

    def __init__(self):
        self.paints = []
        self.fail = set()
        self.world = texture(W + 2 * M + 200, H, 1)   # the "true" wider scene the room is cut from

    def extract(self, install, work, rooms):
        for r in rooms:
            Image.fromarray(self.world[:, 100 + M:100 + M + W]).save(os.path.join(work, f"room_{r:04d}.png"))

    def paint(self, room_png, out_png, *key):
        r = int(os.path.basename(room_png)[5:9])
        self.paints.append(r)
        if r in self.fail:
            raise RuntimeError("synthetic failure")
        wide = Image.fromarray(self.world[:, 100:100 + W + 2 * M]).resize(((W + 2 * M) * 2, H * 2), Image.LANCZOS)
        wide.save(out_png)
        return 0.07 if key else 0.0


class GenerateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.f = Fakes()
        self._orig = (G.extract, G.paint_cloud, G.paint_local, G.openrouter_key)
        G.extract, G.paint_cloud, G.paint_local = self.f.extract, self.f.paint, self.f.paint
        G.openrouter_key = lambda: "test-key"

    def tearDown(self):
        G.extract, G.paint_cloud, G.paint_local, G.openrouter_key = self._orig
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_gen(self, *args):
        sys.argv = ["generate.py", "--install", self.tmp, "--yes"] + list(args)
        return G.main()

    def side(self, r):
        return os.path.join(self.tmp, "sides", f"{r:04d}.png")

    def test_make_resume_and_regenerate(self):
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9,10"), 0)
        self.assertEqual(sorted(self.f.paints), [9, 10])
        img = np.asarray(Image.open(self.side(9)).convert("RGB")).astype(int)
        self.assertEqual(img.shape, (H, W + 2 * M, 3))
        # the room itself (inside the blend bands) is the game's picture
        room = np.asarray(Image.open(os.path.join(self.tmp, "sides-work", "room_0009.png")).convert("RGB")).astype(int)
        self.assertLess(np.abs(img[:, M + 40:M + W - 40] - room[:, 40:W - 40]).mean(), 3)
        meta = load_json((os.path.join(self.tmp, "sides-work", "meta_0009.json")))
        self.assertEqual((meta["method"], meta["seed"], meta["composed"]), ("local", G.LOCAL_SEED, True))
        # nothing to do the second time
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9,10"), 0)
        self.assertEqual(len(self.f.paints), 2)
        # deleting a finished room means: make a new one
        os.remove(self.side(10))
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9,10"), 0)
        self.assertEqual(self.f.paints[2:], [10])
        # --regenerate paints again even though it is done
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9", "--regenerate"), 0)
        self.assertEqual(self.f.paints[3:], [9])

    def test_interrupted_painting_is_recomposed_not_repainted(self):
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9"), 0)
        m = load_json((os.path.join(self.tmp, "sides-work", "meta_0009.json")))
        m["composed"] = False   # as if it stopped between painting and composing
        save_json(os.path.join(self.tmp, "sides-work", "meta_0009.json"), m)
        os.remove(self.side(9))
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9"), 0)
        self.assertEqual(self.f.paints, [9])
        self.assertTrue(os.path.exists(self.side(9)))

    def test_method_change_repaints(self):
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9"), 0)
        m = load_json((os.path.join(self.tmp, "sides-work", "meta_0009.json")))
        m["composed"] = False
        save_json(os.path.join(self.tmp, "sides-work", "meta_0009.json"), m)
        os.remove(self.side(9))
        self.assertEqual(self.run_gen("--method", "cloud", "--rooms", "9"), 0)
        self.assertEqual(self.f.paints, [9, 9])
        self.assertEqual(load_json((os.path.join(self.tmp, "sides-work", "meta_0009.json")))["method"], "cloud")

    def test_failures_give_exit_code_and_summary(self):
        self.f.fail = {10}
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9,10"), 2)
        s = load_json((os.path.join(self.tmp, "sides-work", "last_run.json")))
        self.assertEqual((s["new"], list(s["failed"])), ([9], ["10"]))
        self.assertFalse(os.path.exists(self.side(10)))
        self.f.fail = set()
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9,10"), 0)   # retry
        self.assertTrue(os.path.exists(self.side(10)))

    def test_spending_limit(self):
        self.assertEqual(self.run_gen("--method", "cloud", "--rooms", "9,10,11", "--max-cost", "0.15"), 0)
        s = load_json((os.path.join(self.tmp, "sides-work", "last_run.json")))
        self.assertEqual((s["new"], s["skipped"]), ([9, 10], [11]))
        self.assertLessEqual(s["cost"], 0.15)

    def test_recompose(self):
        self.assertEqual(self.run_gen("--method", "local", "--rooms", "9"), 0)
        os.remove(self.side(9))
        self.assertEqual(self.run_gen("--recompose", "--rooms", "9"), 0)
        self.assertEqual(self.f.paints, [9])
        self.assertTrue(os.path.exists(self.side(9)))


if __name__ == "__main__":
    unittest.main()
