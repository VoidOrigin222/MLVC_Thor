"""Compare NVDEC RTSP B-frame output with the same hardware-decoded file."""

import argparse, csv, json, os, subprocess, time
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path.cwd())
parser.add_argument("--rtsp-base", default="rtsp://192.168.5.3:8554")
args = parser.parse_args()
root = args.root.resolve()
out = (
    root
    / "logs"
    / ("b-frame-regression-" + time.strftime("%Y%m%d-%H%M%S") + "-" + str(os.getpid()))
)
out.mkdir(parents=True)
url = args.rtsp_base.rstrip("/") + "/b-frame-regression-" + str(os.getpid())
video = out / "b-frames.mp4"
video.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [
        "ffmpeg",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "testsrc2=s=1920x1080:r=12",
        "-frames:v",
        "96",
        "-an",
        "-c:v",
        "h264_nvenc",
        "-preset",
        "p1",
        "-rc",
        "constqp",
        "-qp",
        "18",
        "-bf",
        "2",
        "-g",
        "12",
        "-b_ref_mode",
        "disabled",
        "-color_primaries",
        "bt709",
        "-color_trc",
        "bt709",
        "-colorspace",
        "bt709",
        "-color_range",
        "tv",
        str(video),
    ],
    check=True,
)
info = json.loads(
    subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=has_b_frames,width,height,r_frame_rate",
            "-of",
            "json",
            str(video),
        ],
        text=True,
    )
)
assert info["streams"][0]["has_b_frames"] > 0, info
probe = str(root / "build/ports/thor/probe_nvdec_surfaces")
file_result = subprocess.run(
    [probe, str(video), str(out / "file.csv"), "96"],
    capture_output=True,
    text=True,
    check=True,
    timeout=30,
)
log = (out / "publisher.log").open("w")
p = subprocess.Popen(
    [
        "ffmpeg",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "warning",
        "-re",
        "-stream_loop",
        "-1",
        "-i",
        str(video),
        "-an",
        "-c:v",
        "copy",
        "-rtsp_transport",
        "tcp",
        "-f",
        "rtsp",
        url,
    ],
    stdout=log,
    stderr=subprocess.STDOUT,
)
try:
    time.sleep(0.8)
    live_result = subprocess.run(
        [probe, url, str(out / "live.csv"), "48"],
        capture_output=True,
        text=True,
        check=True,
        timeout=20,
    )
finally:
    p.terminate()
    try:
        p.wait(timeout=4)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    log.close()
assert (
    "input_nvdec_low_delay=0 has_b_frames=" + str(info["streams"][0]["has_b_frames"])
) in live_result.stdout, live_result.stdout
original = list(csv.DictReader((out / "file.csv").open()))
live = list(csv.DictReader((out / "live.csv").open()))
assert len(original) == 96 and len(live) == 48
start = next(
    i
    for i, r in enumerate(original)
    if r["surface_sha256"] == live[0]["surface_sha256"]
)
for i, row in enumerate(live):
    expected = original[(start + i) % len(original)]
    assert row["surface_sha256"] == expected["surface_sha256"], ("surface", i)
    assert row["tensor_sha256"] == expected["tensor_sha256"], ("tensor", i)
pts = [int(r["pts"]) for r in live]
assert all(b > a for a, b in zip(pts, pts[1:])), pts
result = {
    "source": info,
    "live_frames": 48,
    "file_frames": 96,
    "file_start_index": start,
    "surface_and_fp16_bitwise_equal": True,
    "nonincreasing_pts": 0,
    "file_stdout": file_result.stdout,
    "live_stdout": live_result.stdout,
}
(out / "RESULT.json").write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
