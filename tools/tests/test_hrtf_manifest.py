from __future__ import annotations

import hashlib
import importlib.util
import json
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).parents[2]
SELECTOR_PATH = REPOSITORY_ROOT / "tools" / "select-hrtf-profiles.py"
SELECTOR_SPEC = importlib.util.spec_from_file_location("select_hrtf_profiles_manifest_test", SELECTOR_PATH)
assert SELECTOR_SPEC and SELECTOR_SPEC.loader
SELECTOR = importlib.util.module_from_spec(SELECTOR_SPEC)
SELECTOR_SPEC.loader.exec_module(SELECTOR)


class HrtfManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        manifest_path = REPOSITORY_ROOT / "resources" / "hrtf" / "profiles.json"
        self.manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    def test_manifest_contains_one_reference_and_five_humans(self) -> None:
        profiles = self.manifest["profiles"]
        self.assertEqual(6, len(profiles))
        self.assertEqual(1, sum(profile["kind"] == "dummy-head" for profile in profiles))
        self.assertEqual(5, sum(profile["kind"] == "human" for profile in profiles))
        self.assertEqual(len(profiles), len({profile["id"] for profile in profiles}))

    def test_outputs_have_pinned_size_and_sha256(self) -> None:
        for profile in self.manifest["profiles"]:
            with self.subTest(profile=profile["id"]):
                self.assertGreater(profile["sofaBytes"], 0)
                self.assertRegex(profile["sofaSha256"], r"^[0-9a-f]{64}$")
                output = REPOSITORY_ROOT / "resources" / "hrtf" / "data" / profile["output"]
                if not output.exists():
                    continue
                payload = output.read_bytes()
                self.assertEqual(profile["sofaBytes"], len(payload))
                self.assertEqual(profile["sofaSha256"], hashlib.sha256(payload).hexdigest())

    def test_human_profiles_are_the_recorded_seed_42_medoids(self) -> None:
        selection = self.manifest["selection"]
        feature_path = REPOSITORY_ROOT / "resources" / "hrtf" / selection["featureFile"]
        self.assertEqual(selection["featureSha256"], hashlib.sha256(feature_path.read_bytes()).hexdigest())
        subjects, values = SELECTOR.load_features(feature_path)
        chosen = [subjects[index] for index in SELECTOR.choose_medoids(values, 5, selection["seed"])]
        self.assertEqual(selection["medoids"], chosen)
        self.assertEqual(
            selection["medoids"],
            [profile["subject"] for profile in self.manifest["profiles"] if profile["kind"] == "human"],
        )


if __name__ == "__main__":
    unittest.main()
