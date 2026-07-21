from __future__ import annotations

import json
import math
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).parents[2]


class ContractDefaultTests(unittest.TestCase):
    def setUp(self) -> None:
        path = REPOSITORY_ROOT / "contracts" / "examples" / "default-scene-v1.json"
        self.scene = json.loads(path.read_text(encoding="utf-8"))

    def test_default_scene_is_48_khz_stereo_at_plus_minus_30_degrees(self) -> None:
        self.assertEqual(48_000, self.scene["audio"]["sampleRate"])
        self.assertEqual(["left", "right"], [speaker["channel"] for speaker in self.scene["speakers"]])
        listener = self.scene["listener"]["positionM"]
        azimuths = []
        for speaker in self.scene["speakers"]:
            delta = [value - origin for value, origin in zip(speaker["positionM"], listener, strict=True)]
            azimuths.append(math.degrees(math.atan2(delta[0], delta[2])))
            self.assertAlmostEqual(2.0, math.hypot(delta[0], delta[2]), places=5)
            self.assertAlmostEqual(0.0, delta[1], places=7)
        self.assertAlmostEqual(-30.0, azimuths[0], places=5)
        self.assertAlmostEqual(30.0, azimuths[1], places=5)

    def test_room_uses_x_y_z_order_with_y_up(self) -> None:
        width, height, depth = self.scene["room"]["dimensionsM"]
        self.assertEqual((5, 2.7, 4), (width, height, depth))
        self.assertLess(self.scene["listener"]["positionM"][1], height)
        self.assertTrue(self.scene["room"]["lateReverbEnabled"])
        for surface in self.scene["room"]["surfaces"]:
            self.assertEqual(3, len(surface["absorption"]))
            self.assertEqual(3, len(surface["diffusion"]))


if __name__ == "__main__":
    unittest.main()
