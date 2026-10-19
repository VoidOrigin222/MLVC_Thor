"""Check role names, configuration selection and workspace handling."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "Linux bash required")
class RoleLauncherTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in ("models", "configs", "bin"):
            (self.root / name).mkdir()
        (self.root / "models/manifest_thor.json").write_text("{}")
        for role in ("encoder", "decoder"):
            (self.root / f"configs/{role}.toml").write_text("# test")
            binary = self.root / "bin" / role
            binary.write_text(f'#!/usr/bin/env bash\nprintf "%s\\n" {role} "$@"\n')
            binary.chmod(0o755)
        (self.root / "configs/custom.toml").write_text("# test")
        self.env = dict(os.environ, MLVC_THOR_ROOT=str(self.root))

    def run_role(self, role, *arguments):
        script = Path(__file__).resolve().parents[1] / "scripts" / f"run_{role}.sh"
        return subprocess.run(
            ["bash", str(script), *arguments],
            cwd=self.root / "configs",
            env=self.env,
            text=True,
            capture_output=True,
        )

    def test_defaults_select_the_named_configuration(self):
        for role in ("encoder", "decoder"):
            with self.subTest(role=role):
                result = self.run_role(role)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout.splitlines(),
                    [role, "--config", f"configs/{role}.toml"],
                )

    def test_explicit_configuration(self):
        for role in ("encoder", "decoder"):
            with self.subTest(role=role):
                result = self.run_role(role, "configs/custom.toml")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout.splitlines(), [role, "--config", "configs/custom.toml"]
                )

    def test_missing_configuration_is_rejected(self):
        for role in ("encoder", "decoder"):
            with self.subTest(role=role):
                result = self.run_role(role, "configs/missing.toml")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Missing configuration", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_extra_arguments_are_rejected(self):
        for role in ("encoder", "decoder"):
            with self.subTest(role=role):
                result = self.run_role(role, "configs/custom.toml", "contexts")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Usage:", result.stderr)

    def test_legacy_single_role_finds_source_template(self):
        scripts = Path(__file__).resolve().parents[1] / "scripts"
        for operation, config in (("encode", "encoder_rtsp"), ("decode", "decoder_rtp")):
            with self.subTest(operation=operation):
                relative = f"src/MLVC/ports/thor/configs/{config}.toml"
                template = self.root / relative
                template.parent.mkdir(parents=True, exist_ok=True)
                template.write_text("# fixture")
                binary = self.root / "bin" / f"mlvc_{operation}_thor"
                binary.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
                binary.chmod(0o755)
                result = subprocess.run(
                    ["bash", str(scripts / f"run_{operation}.sh")],
                    cwd=self.root, env=self.env, text=True, capture_output=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), ["--config", relative])

    def test_legacy_five_modes_find_source_templates(self):
        script = Path(__file__).resolve().parents[1] / "scripts/run_five.sh"
        binary = self.root / "bin/mlvc_multi_thor"
        binary.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
        binary.chmod(0o755)
        for mode in ("contexts", "batch5"):
            for operation in ("encode", "decode"):
                with self.subTest(mode=mode, operation=operation):
                    paths = [f"src/MLVC/ports/thor/configs/five/{mode}/{operation}-{i}.toml"
                             for i in range(5)]
                    for relative in paths:
                        path = self.root / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text("# fixture")
                    result = subprocess.run(
                        ["bash", str(script), operation, mode], cwd=self.root,
                        env=self.env, text=True, capture_output=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.splitlines(),
                                     [operation, mode, f"logs/five-{operation}", *paths])


if __name__ == "__main__":
    unittest.main()
