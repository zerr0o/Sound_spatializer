from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "select-hrtf-profiles.py"
SPEC = importlib.util.spec_from_file_location("select_hrtf_profiles", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SelectHrtfProfilesTests(unittest.TestCase):
    def test_selection_is_deterministic_and_covers_clusters(self) -> None:
        values = [
            [-10.0, -10.0],
            [-9.5, -10.5],
            [0.0, 0.0],
            [0.5, -0.5],
            [10.0, 10.0],
            [10.5, 9.5],
        ]
        first = MODULE.choose_medoids(values, 3, seed=42)
        second = MODULE.choose_medoids(values, 3, seed=42)
        self.assertEqual(first, second)
        self.assertEqual(3, len(first))
        self.assertTrue(any(index in first for index in (0, 1)))
        self.assertTrue(any(index in first for index in (2, 3)))
        self.assertTrue(any(index in first for index in (4, 5)))

    def test_invalid_cluster_count_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            MODULE.choose_medoids([[0.0]], 2)

    def test_seed_is_an_explicit_part_of_initialization(self) -> None:
        # This asymmetric fixture has several PAM local optima. We do not pin a
        # particular optimum, only that the requested seed is actually used.
        values = [[0.0], [1.0], [2.0], [8.0], [9.0], [10.0], [20.0]]
        outcomes = {tuple(MODULE.initialize_medoids(values, 2, seed=seed)) for seed in range(8)}
        self.assertGreater(len(outcomes), 1)


if __name__ == "__main__":
    unittest.main()
