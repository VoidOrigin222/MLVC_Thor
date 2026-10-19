"""Variable source counts, per-source FPS and cached-media compatibility."""

import unittest

from publish_test_sources import configured_sources, hardware_encode, matches_source


class SourceConfigTests(unittest.TestCase):
    def test_counts_and_per_source_overrides(self):
        for rates in ([30, 30], [20, 20, 20], [30, 30, 20, 20, 20], [12] * 6):
            config = {"fps": 12, "width": 1920, "height": 1080,
                      "streams": [{"type": "timer", "fps": fps} for fps in rates]}
            self.assertEqual([s["fps"] for s in configured_sources(config)], rates)
            self.assertNotIn("width", config["streams"][0])

    def test_invalid_count_and_fps(self):
        for streams in ([], None, 3, [{"type": "timer", "fps": True}],
                        [{"type": "timer", "fps": 0}]):
            config = {"fps": 30, "width": 1920, "height": 1080, "streams": streams}
            with self.assertRaises((KeyError, ValueError)):
                configured_sources(config)

    def test_legacy_configuration_keeps_original_five_sources(self):
        config = {"fps": 12, "width": 1920, "height": 1080,
                  "static_video": "still.mp4", "dynamic_video": "moving.mp4"}
        self.assertEqual([s["type"] for s in configured_sources(config)],
                         ["static", "static", "dynamic", "dynamic", "timer"])

    def test_mismatched_rate_requires_preparation_and_hardware_encoder(self):
        stream = {"codec_name": "h264", "width": 1920, "height": 1080,
                  "r_frame_rate": "30/1"}
        source = {"fps": 30, "width": 1920, "height": 1080}
        self.assertTrue(matches_source(stream, source))
        source["fps"] = 20
        self.assertFalse(matches_source(stream, source))
        self.assertIn("h264_nvenc", hardware_encode(20))


if __name__ == "__main__":
    unittest.main()
