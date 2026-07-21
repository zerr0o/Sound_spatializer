import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY_ROOT / "tools" / "Test-TauriArtifact.ps1"
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


@unittest.skipUnless(POWERSHELL, "PowerShell is required for the Tauri artifact guard")
class TauriArtifactGuardTests(unittest.TestCase):
    def run_guard(
        self,
        contents: bytes,
        engine_contents: bytes | None = b"MZ\x00production-engine-payload",
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "desktop.exe"
            executable.write_bytes(contents)
            if engine_contents is not None:
                (Path(directory) / "SoundSpatializer.Engine.exe").write_bytes(engine_contents)
            return subprocess.run(
                [
                    POWERSHELL,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(SCRIPT),
                    "-Executable",
                    str(executable),
                ],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )

    def test_accepts_release_payload_without_dev_origin(self) -> None:
        result = self.run_guard(b"MZ\x00production-tauri-payload")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_ascii_dev_origin(self) -> None:
        result = self.run_guard(b"MZ\x00http://127.0.0.1:1420")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("developpement", result.stderr + result.stdout)

    def test_rejects_utf16_dev_origin(self) -> None:
        result = self.run_guard(b"MZ" + "http://localhost:1420".encode("utf-16-le"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("developpement", result.stderr + result.stdout)

    def test_rejects_missing_companion_engine(self) -> None:
        result = self.run_guard(b"MZ\x00production-tauri-payload", engine_contents=None)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Moteur compagnon introuvable", result.stderr + result.stdout)


if __name__ == "__main__":
    unittest.main()
