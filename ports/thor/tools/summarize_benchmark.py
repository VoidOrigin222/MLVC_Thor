"""Summarize complete dual-board trials, retaining per-run values and raw logs."""
import argparse
import json
import re
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("logs", type=Path)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
summary = {}
for variant in ("baseline", "optimized"):
    for source in ("offline", "rtsp"):
        records = []
        for path in sorted(args.logs.glob(f"opt-{variant}-{source}-*-result.json")):
            record = json.loads(path.read_text(encoding="utf8"))
            label = record["label"]
            encode = (args.logs / f"{label}-encode.log").read_text(encoding="utf8")
            decode = (args.logs / f"{label}-decode.log").read_text(encoding="utf8")
            assert "encode=ok" in encode and "decode=ok" in decode, label
            assert re.search(rf"(?m)^frames={record['frames']}$", decode), label
            if source == "rtsp":
                assert re.search(rf"(?m)^output_transport_rtsp_frames={record['frames']}$", decode), label
                assert re.search(r"(?m)^forward_dropped_frames=0$", decode), label
            # Baseline JSON was collected before its key regex accepted digits.
            record["metrics"].update({
                m.group(1): float(m.group(2))
                for m in re.finditer(r"(?m)^([a-zA-Z_0-9]+)=(\d+(?:\.\d+)?)$", encode)
            })
            records.append(record)
        assert len(records) == 3, f"Expected three complete trials: {variant}/{source}"
        keys = ["encode_fps", "bgr_ready_to_decode_ack_mean_ms",
                "bgr_ready_to_decode_ack_p50_ms", "bgr_ready_to_decode_ack_p95_ms"]
        means = {key: statistics.mean(r[key] for r in records) for key in keys}
        for key in ("motion_work_avg_ms", "motion_h264_decode_avg_ms",
                    "motion_vector_reduce_avg_ms", "motion_packet_policy_avg_ms",
                    "nvenc_input_copy_avg_ms", "nvenc_encode_avg_ms",
                    "motion_cuda_nv12_avg_ms", "ready_to_output_avg_ms"):
            if all(key in r["metrics"] for r in records):
                means[key] = statistics.mean(r["metrics"][key] for r in records)
        summary[f"{variant}_{source}"] = {"mean_of_three_runs": means, "runs": records}
out = args.output or args.logs / "summary.json"
out.write_bytes((json.dumps(summary, indent=2) + "\n").encode("utf8"))
print(json.dumps({key: value["mean_of_three_runs"] for key, value in summary.items()}, indent=2))
