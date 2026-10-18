"""Correlate frame timestamps using only same-board clocks for each duration."""

import argparse, csv, json, statistics, struct
from pathlib import Path

REC = struct.Struct("<QIHH128s")


def stats(values):
    v = sorted(values)
    if not v:
        return {}
    return {
        "n": len(v),
        "mean": statistics.mean(v),
        "p50": statistics.median(v),
        "p95": v[int((len(v) - 1) * 0.95)],
        "p99": v[int((len(v) - 1) * 0.99)],
        "max": max(v),
        "std": statistics.pstdev(v),
    }


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", nargs="+", type=Path)
args = parser.parse_args()
for result_path in args.results:
    ROOT = result_path.resolve().parent
    label = result_path.stem
    trial = json.loads(result_path.read_text())
    assert trial["complete"], "Incomplete trial: " + label
    enc = ROOT / label / "encode/logs"
    dec = ROOT / label / "decode/logs"
    sent, first, received, remote_acks, begin, complete, source, acks = (
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
    )
    for ns, size, port, n, h in REC.iter_unpack(
        (enc / (label + "-packets.bin")).read_bytes()
    ):
        if port % 2 == 0 and 39480 <= port <= 39488 and h[13] == 2:
            key = ((port - 39480) // 2, struct.unpack_from("!I", h, 24)[0])
            first[key] = min(first.get(key, ns), ns)
            sent[key] = max(sent.get(key, ns), ns)
    for ns, size, port, n, h in REC.iter_unpack(
        (dec / (label + "-packets.bin")).read_bytes()
    ):
        if port % 2 == 0 and 39480 <= port <= 39488 and h[13] == 2:
            key = ((port - 39480) // 2, struct.unpack_from("!I", h, 24)[0])
            received[key] = max(received.get(key, ns), ns)
        elif 39500 <= port <= 39504 and size == 8 and h[:4] == b"MLAT":
            remote_acks[(port - 39500, struct.unpack_from("!I", h, 4)[0])] = ns
    traces = {}
    for lane in range(5):
        for op, folder in [("encode", enc), ("decode", dec)]:
            traces[(op, lane)] = json.loads(
                (folder / (label + f"-{op}-lane{lane}-trace.json")).read_text()
            )["traceEvents"]
        for event in traces[("encode", lane)]:
            if event["name"] == "source.frame.ready":
                arg = event["args"]
                source[(lane, int(arg["frame_index"]))] = int(
                    arg["source_read_ns"] or arg["monotonic_ns"]
                )
        for event in traces[("decode", lane)]:
            if event["name"] in ["decode.frame.start", "decode.frame.complete"]:
                target = begin if event["name"].endswith("start") else complete
                target[(lane, int(event["args"]["frame_index"]))] = int(
                    event["args"]["monotonic_ns"]
                )
        acks.update(
            {
                (lane, int(i)): int(t)
                for i, t in json.loads(
                    (enc / (label + f"-lane{lane}-acks.json")).read_text()
                ).items()
            }
        )
    rows = []
    for key in sorted(source):
        if not all(
            key in d
            for d in [acks, first, sent, received, remote_acks, begin, complete]
        ):
            continue
        residence = remote_acks[key] - received[key]
        rows.append(
            {
                "lane": key[0],
                "frame": key[1],
                "total_ms": (acks[key] - source[key]) / 1e6,
                "encoder_ms": (first[key] - source[key]) / 1e6,
                "send_ms": (sent[key] - first[key]) / 1e6,
                "receiver_ms": residence / 1e6,
                "before_gpu_ms": (begin[key] - received[key]) / 1e6,
                "gpu_frame_ms": (complete[key] - begin[key]) / 1e6,
                "network_ack_ms": ((acks[key] - sent[key]) - residence) / 1e6,
            }
        )
    expected = {
        (lane, frame) for lane in range(5) for frame in range(trial["frames_per_lane"])
    }
    assert (
        set(source)
        == set(acks)
        == set(first)
        == set(sent)
        == set(received)
        == set(remote_acks)
        == set(begin)
        == set(complete)
        == expected
    ), "Missing frame timing evidence"
    assert len(rows) == 5 * trial["frames_per_lane"]
    sequences = {lane: [] for lane in range(5)}
    timestamps = {lane: {} for lane in range(5)}
    payloads = {lane: {} for lane in range(5)}
    for ns, size, port, n, h in REC.iter_unpack(
        (enc / (label + "-packets.bin")).read_bytes()
    ):
        if port % 2 == 0 and 39480 <= port <= 39488:
            lane = (port - 39480) // 2
            sequences[lane].append(struct.unpack_from("!H", h, 2)[0])
            if h[13] == 2 and h[14] & 128:
                frame = struct.unpack_from("!I", h, 24)[0]
                assert frame not in timestamps[lane], "Repeated frame header"
                timestamps[lane][frame] = struct.unpack_from("!I", h, 4)[0]
                payloads[lane][frame] = struct.unpack_from("!I", h, 76)[0]
    for lane in range(5):
        seq = sequences[lane]
        assert all(
            (b - a) % 65536 == 1 for a, b in zip(seq, seq[1:])
        ), "RTP sequence gap"
        stamps = timestamps[lane]
        assert len(stamps) == trial["frames_per_lane"]
        assert all(
            (stamps[i] - stamps[i - 1]) % 2**32 == 7500
            for i in range(1, trial["frames_per_lane"])
        ), "RTP FPS mismatch"
        for op in ["encode", "decode"]:
            metrics = trial[op]["lanes"][lane]["metrics"]
            assert metrics["frames"] == trial["frames_per_lane"]
            assert metrics["codec_total_bytes"] == sum(
                payloads[lane].values()
            ), "Payload counters differ"
            if op == "decode":
                assert (
                    metrics["output_transport_rtsp_frames"] == trial["frames_per_lane"]
                )
                for key in [
                    "rtp_lost_packets",
                    "rtp_queue_overflows",
                    "rtp_queue_dropped_units",
                    "rtp_crc_failed_units",
                    "rtp_dependency_gaps",
                    "rtp_skipped_frames",
                    "forward_dropped_frames",
                ]:
                    assert metrics[key] == 0, (lane, key, metrics[key])
    result = {"complete": trial["complete"], "frames": len(rows), "lanes": []}
    for lane in range(5):
        full = [r for r in rows if r["lane"] == lane]
        steady = [r for r in full if r["frame"] >= 96]
        latency = [r["total_ms"] for r in steady]
        fields = [f for f in full[0] if f.endswith("_ms")]
        item = {
            "lane": lane,
            "all": {f: stats([r[f] for r in full]) for f in fields},
            "after96": {f: stats([r[f] for r in steady]) for f in fields},
            "jitter_after96": stats([abs(b - a) for a, b in zip(latency, latency[1:])]),
            "first120_mean": statistics.mean(latency[:120]),
            "last120_mean": statistics.mean(latency[-120:]),
            "encode_fps": trial["encode"]["lanes"][lane]["metrics"]["encode_fps"],
            "ack_fps": trial["encode"]["lanes"][lane]["decode_ack_steady_fps"],
            "queue_peak": trial["decode"]["lanes"][lane]["metrics"][
                "rtp_queue_max_depth"
            ],
        }
        item["worst"] = max(full, key=lambda r: r["total_ms"])
        item["timeline"] = []
        for path in enc.glob(label + "-input-input" + str(lane) + "-*.csv"):
            data = list(csv.DictReader(path.open()))
            data = [{k: int(v) for k, v in r.items()} for r in data]
            steadydata = [r for r in data if r["frame"] >= 96]
            if not steadydata:
                continue
            pts = [r["pts_ns"] for r in data]
            item["timeline"].append(
                {
                    "file": path.name,
                    "samples": len(data),
                    "nonincreasing_pts": sum(b <= a for a, b in zip(pts, pts[1:])),
                    "read_fps": (len(steadydata) - 1)
                    * 1e9
                    / (steadydata[-1]["read_ns"] - steadydata[0]["read_ns"]),
                    "relative_backlog_growth_ms": (
                        (steadydata[-1]["read_ns"] - steadydata[0]["read_ns"])
                        - (steadydata[-1]["pts_ns"] - steadydata[0]["pts_ns"])
                    )
                    / 1e6,
                }
            )
        result["lanes"].append(item)
    result["after96"] = {
        field: stats([r[field] for r in rows if r["frame"] >= 96])
        for field in rows[0]
        if field.endswith("_ms")
    }
    result["all"] = stats([r["total_ms"] for r in rows])
    differences = []
    for lane in range(5):
        values = [r["total_ms"] for r in rows if r["lane"] == lane and r["frame"] >= 96]
        differences.extend(abs(b - a) for a, b in zip(values, values[1:]))
    result["jitter_successive_after96"] = stats(differences)
    result["worst"] = sorted(rows, key=lambda r: r["total_ms"], reverse=True)[:10]
    result["integrity"] = {
        "matched_frames": len(rows),
        "rtp_sequence_gaps": 0,
        "rtp_timestamp_step": 7500,
        "payload_counters_match": True,
        "loss_crc_dependency_publish_drops": 0,
    }
    result["telemetry"] = {
        op: {
            field: stats([r[field] for r in trial[op]["telemetry"] if field in r])
            for field in ["gpu", "nvenc", "nvdec", "system_cpu_percent"]
        }
        for op in ["encode", "decode"]
    }
    (ROOT / (label + "-summary.json")).write_text(json.dumps(result, indent=2))
    with (ROOT / (label + "-frames.csv")).open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)
    print(label, json.dumps(result["after96"]))
    print(
        "Lanes:",
        [
            (
                r["lane"],
                round(r["after96"]["total_ms"]["mean"], 1),
                round(r["last120_mean"] - r["first120_mean"], 1),
                r["queue_peak"],
            )
            for r in result["lanes"]
        ],
    )
