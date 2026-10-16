"""Keep long-trial collectors alive for the full requested codec duration."""

import ast
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import benchmark_five as bench


class LongTrialTests(unittest.TestCase):
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
