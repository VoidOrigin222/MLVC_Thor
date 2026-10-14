"""Summarize the opt-in module probes and ordinary Chrome trace timings."""
import argparse
import collections
import json
import re
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("logs", type=Path)
parser.add_argument("--prefix", default="device-detailed")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
summary = {}
for source in ("mp4", "fp16"):
    runs = [json.loads((args.logs / f"{args.prefix}-{source}-{i}-result.json").read_text())
            for i in range(3)]
    means = {key: statistics.mean(r["timings"][key]["avg_ms"] for r in runs)
             for key in runs[0]["timings"]}
    traces, motion_prepare = [], []
    for r in runs:
        events = json.loads((args.logs / f"{r['label']}-trace.json").read_text())["traceEvents"]
        durations = collections.defaultdict(list)
        for event in events:
            if event.get("ph") == "X":
                durations[event["name"]].append(float(event["dur"]) / 1000)
        selected = {}
        for name in ("video.read.frame", "video.read.fp16_frame", "entropy.rans_encode",
                     "bitstream.frame_write", "pipeline.node.acl_stage.MLVCEncoder"):
            if name not in durations:
                continue
            steady = durations[name][96:]
            assert len(steady) == 441, (r["label"], name, len(steady))
            selected[name] = statistics.mean(steady)
        traces.append(selected)
        log = (args.logs / f"{r['label']}.log").read_text()
        motion_prepare.append(float(re.search(
            r"(?m)^motion_input_prepare_avg_ms=([\d.]+)$", log).group(1)))
    trace_means = {name: statistics.mean(t[name] for t in traces) for name in traces[0]}
    if source == "mp4":
        capture = means["input.grab_decode_wall"] + means["input.retrieve_bgr_wall"]
        # Both measurements cover the same 441 calls. Remove the consumer's
        # upstream queue wait from its enclosing read/preprocess interval.
        input_stage = trace_means["video.read.frame"] - means["input.consumer_queue_wait"]
    else:
        capture = 0
        input_stage = trace_means["video.read.fp16_frame"] + statistics.mean(motion_prepare)
    stages = {"capture_decode_retrieve": capture,
              "input_prepare_excluding_capture_wait": input_stage,
              "motion": means["motion.total_wall"],
              "encoder_process": means["encoder.process_wall"],
              "entropy": trace_means["entropy.rans_encode"],
              "output": trace_means["bitstream.frame_write"]}
    slowest = max(stages, key=stages.get)
    summary[source] = {"fps_runs": [r["fps"] for r in runs],
                       "fps_mean": statistics.mean(r["fps"] for r in runs),
                       "means": means, "trace_means": trace_means,
                       "fp16_to_nv12_prepare_mean_ms": statistics.mean(motion_prepare),
                       "pipeline_stages_ms": stages, "slowest_stage": slowest,
                       "bottleneck_ms": stages[slowest],
                       "ideal_pipeline_fps": 1000 / stages[slowest]}
output = args.output or args.logs / f"{args.prefix}-summary.json"
output.write_bytes((json.dumps(summary, indent=2) + "\n").encode())
print(json.dumps(summary, indent=2))
