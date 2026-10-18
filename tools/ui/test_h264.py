"""Hardware baseline settings, legacy migration and owned-process lifecycle."""

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from h264 import Publishers, command
from server import ConfigStore, ROOT, validate_config


def defaults():
    return json.loads((ROOT / "ui_config.json").read_text(encoding="utf8"))


class H264Tests(unittest.TestCase):
    def test_legacy_file_migrates_without_changing_existing_addresses(self):
        cfg = defaults()
        for key in list(cfg):
            if key.startswith("h264_"):
                del cfg[key]
        for c in cfg["channels"]:
            del c["h264"]
        cfg["channels"][0]["original"] = "rtsp://192.168.5.3:8554/custom/source"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "parameters.json"
            path.write_text(json.dumps(cfg))
            current = ConfigStore(path).snapshot()
            self.assertEqual(current["h264_qp"], 40)
            self.assertEqual(
                current["channels"][0]["original"], cfg["channels"][0]["original"]
            )
            self.assertEqual(
                current["channels"][0]["h264"], "rtsp://192.168.5.3:8554/camera-h2640"
            )
            self.assertEqual(json.loads(path.read_text()), current)

    def test_invalid_qp_and_overwriting_source_are_rejected(self):
        for value in (-1, 52, True, "40"):
            cfg = defaults()
            cfg["h264_qp"] = value
            with self.assertRaises(ValueError):
                validate_config(cfg)
        cfg = defaults()
        cfg["channels"][0]["h264"] = cfg["channels"][0]["original"]
        with self.assertRaises(ValueError):
            validate_config(cfg)

    def test_commands_use_hardware_and_constant_qp(self):
        cfg = defaults()
        cfg["h264_qp"] = 35
        for encoder, flag in (("h264_qsv", "-q:v"), ("h264_nvenc", "-qp")):
            args = command(cfg, cfg["channels"][0], encoder)
            self.assertEqual(args[args.index(flag) + 1], "35")
            self.assertIn("-hwaccel", args)
            thread_index = args.index("-threads:v")
            self.assertEqual(args[thread_index + 1], "1")
            self.assertLess(thread_index, args.index("-i"))
            self.assertIn("-xerror", args)
            self.assertNotIn("libx264", args)
            self.assertNotIn("-b:v", args)
            self.assertNotEqual(args[args.index("-i") + 1], args[-1])
            if encoder == "h264_qsv":
                self.assertIn("+qscale", args)

    def test_qp_change_restarts_only_owned_publishers(self):
        cfg = defaults()
        manager = Publishers()
        manager.encoder = "h264_qsv"
        fake1, fake2 = Mock(), Mock()
        fake1.poll.return_value = fake2.poll.return_value = None
        try:
            with patch.object(manager, "probe"), patch(
                "h264.subprocess.Popen", side_effect=[fake1, fake2]
            ) as popen:
                manager.update(cfg, {"input0"})
                manager.update(cfg, {"input0"})
                self.assertEqual(popen.call_count, 1)
                cfg["h264_qp"] = 35
                manager.update(cfg, {"input0"})
                self.assertEqual(popen.call_count, 2)
                fake1.terminate.assert_called_once()
                manager.close()
                fake2.terminate.assert_called_once()
        finally:
            manager.close()

    def test_external_stream_is_not_replaced_and_closed_manager_stays_closed(self):
        cfg = defaults()
        manager = Publishers()
        manager.encoder = "h264_qsv"
        with patch.object(manager, "probe"), patch("h264.subprocess.Popen") as popen:
            manager.update(cfg, {"input0", "camera-h2640"})
            popen.assert_not_called()
            manager.close()
            manager.update(cfg, {"input0"})
            popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
