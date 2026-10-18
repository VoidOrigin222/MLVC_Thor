"""Publish two still scenes, two looping videos and a live clock over RTSP."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def probe(path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=codec_name,width,height,r_frame_rate",
         "-of", "json", str(path)], check=True, capture_output=True, text=True
    )
    return json.loads(result.stdout)["streams"][0]


def timer_filter(font):
    # Both received pictures carry this source clock; no board clock sync needed.
    return (
        f"drawtext=fontfile='{font}':text='ELAPSED':fontsize=54:"
        "fontcolor=0x99aabb:x=(w-tw)/2:y=240,"
        f"drawtext=fontfile='{font}':text='%{{pts\\:hms}}':fontsize=176:"
        "fontcolor=white:x=(w-tw)/2:y=(h-th)/2,"
        f"drawtext=fontfile='{font}':text='FRAME %{{n}}':fontsize=64:"
        "fontcolor=0x99aabb:x=(w-tw)/2:y=740"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path,
                        default=Path("configs/five/test_sources.json"))
    parser.add_argument("--rtsp-base", help="Override the configured RTSP server")
    parser.add_argument("--duration", type=float,
                        help="Stop after this many seconds; default: until Ctrl+C")
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8"))
    base = (args.rtsp_base or config["rtsp_base"]).rstrip("/")
    if not base.startswith("rtsp://"):
        parser.error("rtsp_base must start with rtsp://")
    if args.duration is not None and args.duration <= 0:
        parser.error("duration must be positive")
    fps, width, height = (int(config[k]) for k in ("fps", "width", "height"))
    if min(fps, width, height) <= 0:
        parser.error("fps, width and height must be positive")
    font = Path(config["font"])
    if not font.is_file():
        parser.error(f"Font not found: {font}")
    static, dynamic = (Path(config[k]) for k in ("static_video", "dynamic_video"))
    for path in (static, dynamic):
        if not path.is_file():
            parser.error(f"Video not found: {path}")
    stream = probe(dynamic)
    rate_n, rate_d = map(int, stream["r_frame_rate"].split("/"))
    if (stream["codec_name"] != "h264" or stream["width"] != width
            or stream["height"] != height or rate_n != fps * rate_d):
        parser.error("dynamic_video must be H.264 at configured size and FPS")

    children, logs = [], []
    stopped = False

    def stop(signum, frame):
        nonlocal stopped
        stopped = True

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    common = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning"]
    encode = ["-an", "-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull",
              "-rc", "constqp", "-qp", "18", "-bf", "0", "-rc-lookahead", "0",
              "-g", str(fps), "-pix_fmt", "yuv420p"]
    run_id = time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"
    log_dir = Path("logs/test-sources") / run_id
    log_dir.mkdir(parents=True)
    try:
        # Cache a truly still loop; four publishers can then copy H.264 packets.
        identity = f"{static.resolve()}:{static.stat().st_size}:" \
                   f"{static.stat().st_mtime_ns}:{width}:{height}:{fps}"
        digest = hashlib.sha256(identity.encode()).hexdigest()[:16]
        still = Path("data/video/generated") / f"still-{digest}.mp4"
        still.parent.mkdir(parents=True, exist_ok=True)
        if not still.exists():
            temporary = still.with_suffix(".part.mp4")
            vf = (f"scale={width}:{height},select=eq(n\\,0),"
                  f"loop=loop=-1:size=1:start=0,setpts=N/({fps}*TB)")
            with (log_dir / "prepare-static.log").open("w") as log:
                proc = subprocess.Popen(common + ["-y", "-hwaccel", "cuda", "-i",
                    str(static), "-vf", vf, "-r", str(fps), "-frames:v",
                    str(fps * 10)] + encode + [str(temporary)],
                    stdout=log, stderr=subprocess.STDOUT)
                children.append(proc)
                while proc.poll() is None and not stopped:
                    time.sleep(.1)
                if stopped:
                    return 0
                if proc.returncode:
                    raise RuntimeError(f"Static preparation failed; see {log_dir}")
                temporary.replace(still)
                children.remove(proc)
        if stopped:
            return 0
        for lane in range(5):
            if lane < 4:
                video = still if lane < 2 else dynamic
                command = common + ["-re", "-stream_loop", "-1", "-i", str(video),
                                    "-map", "0:v:0", "-an", "-c:v", "copy"]
                label = "static" if lane < 2 else "dynamic"
            else:
                command = common + ["-re", "-f", "lavfi", "-i",
                    f"color=c=0x141c26:s={width}x{height}:r={fps}",
                    "-vf", timer_filter(str(font))] + encode
                label = "live timer"
            log = (log_dir / f"input{lane}.log").open("w")
            logs.append(log)
            children.append(subprocess.Popen(command + ["-rtsp_transport", "tcp",
                "-f", "rtsp", f"{base}/input{lane}"], stdout=log,
                stderr=subprocess.STDOUT))
            print(f"input{lane}: {label}, {base}/input{lane}", flush=True)
        print(f"{width}x{height}, {fps} FPS. Logs: {log_dir}. Ctrl+C stops all five.",
              flush=True)
        start = time.monotonic()
        while not stopped:
            for lane, proc in enumerate(children):
                if proc.poll() is not None:
                    raise RuntimeError(f"input{lane} exited ({proc.returncode}); "
                                       f"see {log_dir}/input{lane}.log")
            if args.duration is not None and time.monotonic() - start >= args.duration:
                break
            time.sleep(.1)
        return 0
    finally:
        for proc in children:
            if proc.poll() is None:
                proc.terminate()
        deadline = time.monotonic() + 5
        for proc in children:
            try:
                proc.wait(timeout=max(.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for log in logs:
            log.close()


if __name__ == "__main__":
    raise SystemExit(main())
