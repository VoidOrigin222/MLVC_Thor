"""Keep long-trial collectors alive for the full requested codec duration."""

import ast
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import benchmark_five as bench
import benchmark_mixed as mixed


class LongTrialTests(unittest.TestCase):
    def test_generated_configs_use_logs_and_source_templates(self):
        source = Path(__file__).resolve().parents[1] / "configs/five/contexts"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "configs/five").mkdir(parents=True)
            for role in ("encoder", "decoder"):
                (root / f"configs/{role}.toml").write_text("# keep")
            original = {str(p.relative_to(root)): p.read_bytes()
                        for p in (root / "configs").rglob('*') if p.is_file()}
            for operation, host in (("encode", bench.ENCODER), ("decode", bench.DECODER)):
                templates = root / "src/MLVC/ports/thor/configs/five/contexts"
                templates.mkdir(parents=True, exist_ok=True)
                for lane in range(5):
                    name = f"{operation}-{lane}.toml"
                    (templates / name).write_bytes((source / name).read_bytes())
                commands = []

                def launch(command, **kwargs):
                    kwargs["stdout"].close()
                    commands.append(command)
                    return mock.Mock(pid=10000 + len(commands))

                def local_remote(_host, script):
                    previous = os.getcwd()
                    try:
                        with mock.patch("subprocess.Popen", side_effect=launch), contextlib.redirect_stdout(io.StringIO()):
                            exec(compile(script, "prepare-script", "exec"), {})
                    finally:
                        os.chdir(previous)

                with mock.patch.object(bench, "ROOT", directory), mock.patch.object(
                    bench, "remote", side_effect=local_remote
                ):
                    bench.prepare(host, "scratch", "contexts", 30, 20, 1, "bin")
                generated = list((root / "logs").glob(f"scratch-{operation}-lane*.toml"))
                self.assertEqual(len(generated), 5)
                self.assertTrue(all("frame_num = 30" in p.read_text() for p in generated))
                codec = commands[-1]
                self.assertTrue(all(p.startswith("logs/") for p in codec[-5:]))
            current = {str(p.relative_to(root)): p.read_bytes()
                       for p in (root / "configs").rglob('*') if p.is_file()}
            self.assertEqual(current, original)

    def test_mixed_source_scratch_does_not_modify_production_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "configs/five/test_sources.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps({"rtsp_base": "rtsp://original:8554", "streams": []}))
            before = path.read_bytes()

            def local_remote(_host, script):
                previous = os.getcwd()
                try:
                    with mock.patch("subprocess.Popen") as popen, contextlib.redirect_stdout(io.StringIO()):
                        popen.return_value.pid = 10000
                        popen.side_effect = lambda *a, **kw: (kw["stdout"].close() or mock.Mock(pid=10000))
                        exec(compile(script, "source-script", "exec"), {})
                finally:
                    os.chdir(previous)

            with mock.patch.object(bench, "ROOT", directory), mock.patch.object(
                bench, "remote", side_effect=local_remote
            ):
                mixed.source(bench.ENCODER, "scratch", 10)
            self.assertEqual(path.read_bytes(), before)
            self.assertTrue((root / "logs/scratch-sources.json").is_file())
            self.assertFalse((root / "configs/scratch-sources.json").exists())

    def test_looped_source_is_expanded_before_realtime_seek(self):
        commands = []

        def run(command, **kwargs):
            commands.append(command)

        def launch(command, **kwargs):
            commands.append(command)
            kwargs["stdout"].close()
            return mock.Mock(pid=10000 + len(commands))

        def local_remote(host, script):
            previous = os.getcwd()
            try:
                with mock.patch("subprocess.run", side_effect=run), mock.patch(
                    "subprocess.Popen", side_effect=launch
                ), mock.patch("time.sleep"):
                    exec(compile(script, "source-script", "exec"), {})
            finally:
                os.chdir(previous)

        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "logs").mkdir()
            with mock.patch.object(bench, "ROOT", directory), mock.patch.object(
                bench, "remote", side_effect=local_remote
            ):
                bench.sources("pts-test", True, loop=True, duration_seconds=345)
        self.assertEqual(len(commands), 6)
        self.assertIn("-stream_loop", commands[0])
        self.assertNotIn("-ss", commands[0])
        expanded = commands[0][-1]
        for command in commands[1:]:
            self.assertNotIn("-stream_loop", command)
            self.assertIn("-re", command)
            self.assertEqual(command[command.index("-i") + 1], expanded)

    def test_all_collectors_share_the_extended_timeout(self):
        with mock.patch.object(bench, "remote") as remote:
            bench.prepare(
                bench.ENCODER,
                "long-test",
                "contexts",
                3120,
                12,
                1,
                "bin/latency-test",
                timeout_seconds=330,
            )
        script = remote.call_args.args[1]
        tree = ast.parse(script)
        collectors = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call) or not node.args:
                continue
            if not isinstance(node.func, ast.Name) or node.func.id != "launch":
                continue
            command = node.args[0]
            if not isinstance(command, ast.List):
                continue
            constants = [
                item.value if isinstance(item, ast.Constant) else None
                for item in command.elts
            ]
            if constants[0] == "python3":
                collectors.append(constants)
        self.assertEqual(len(collectors), 2)
        for args in collectors:
            self.assertIn("--seconds", args)
            self.assertEqual(args[args.index("--seconds") + 1], "345")

    def test_tail_window_uses_requested_final_frames(self):
        def local_remote(host, script):
            return subprocess.run(
                [sys.executable, "-"],
                input=script,
                text=True,
                capture_output=True,
                check=True,
            ).stdout

        with tempfile.TemporaryDirectory() as directory:
            logs = Path(directory) / "logs"
            logs.mkdir()
            prefix = "incomplete-test"
            events = [
                {
                    "name": "source.frame.ready",
                    "args": {
                        "frame_index": i,
                        "source_read_ns": (i - 95) * 1_000_000,
                        "monotonic_ns": (i - 95) * 1_000_000,
                    },
                }
                for i in (96, 97)
            ]
            for lane in range(5):
                base = f"{prefix}-encode-lane{lane}"
                (logs / f"{base}.log").write_text("encode=ok\nframes=3120\n")
                (logs / f"{base}-trace.json").write_text(
                    json.dumps({"traceEvents": events})
                )
                (logs / f"{prefix}-lane{lane}-acks.json").write_text(
                    json.dumps({96: 3_000_000, 97: 4_000_000})
                )
            (logs / f"{prefix}-encode-telemetry.jsonl").write_text("{}\n")
            with mock.patch.object(bench, "ROOT", directory), mock.patch.object(
                bench, "remote", side_effect=local_remote
            ):
                result = bench.collect(bench.ENCODER, prefix, "encode", 3120)
        for lane in result["lanes"]:
            self.assertEqual(lane["ack_count"], 2)
            self.assertFalse(lane["tail_samples_complete"])
            self.assertNotIn("latency_last120_mean_ms", lane)
            self.assertNotIn("latency_growth_ms", lane)


if __name__ == "__main__":
    unittest.main()
