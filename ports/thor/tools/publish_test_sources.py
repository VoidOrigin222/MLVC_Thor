"""Publish configured still scenes, looping videos or live clocks over RTSP."""

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


def configured_sources(config):
    sources = config.get("streams")
    if sources is None:
        # Preserve historical benchmark configurations and their lane order.
        sources = ([{"type": "static", "video": config["static_video"]}] * 2
                   + [{"type": "dynamic", "video": config["dynamic_video"]}] * 2
                   + [{"type": "timer"}])
    if not isinstance(sources, list) or not sources:
        raise ValueError("streams must contain at least one source")
    result = []
    for source in sources:
        if not isinstance(source, dict) or source.get("type") not in (
                "static", "dynamic", "timer"):
            raise ValueError("Each source type must be static, dynamic or timer")
        if source["type"] != "timer" and (
                not isinstance(source.get("video"), str) or not source["video"].strip()):
            raise ValueError("Static and dynamic sources require a video path")
        source = dict(source)
        for key in ("fps", "width", "height"):
            value = source.get(key, config.get(key))
            if type(value) is not int or value <= 0 or (key == "fps" and value > 240):
                raise ValueError(f"Source {key} must be a positive integer (FPS <= 240)")
            source[key] = value
        if source["width"] % 2 or source["height"] % 2:
            raise ValueError("H.264 source width and height must be even")
        result.append(source)
    return result


def matches_source(stream, source):
    rate_n, rate_d = map(int, stream["r_frame_rate"].split("/"))
    return (stream["codec_name"] == "h264" and stream["width"] == source["width"]
            and stream["height"] == source["height"]
            and rate_d > 0 and rate_n == source["fps"] * rate_d)


def hardware_encode(fps):
    return ["-an", "-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull",
            "-rc", "constqp", "-qp", "18", "-bf", "0", "-rc-lookahead", "0",
            "-g", str(fps), "-pix_fmt", "yuv420p"]


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
    font = Path(config["font"])
    if not font.is_file():
        parser.error(f"Font not found: {font}")
    try:
        sources = configured_sources(config)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    for source in sources:
        if source["type"] == "timer":
            continue
        path = Path(source["video"])
        if not path.is_file():
            parser.error(f"Video not found: {path}")

    children, logs = [], []
    stopped = False

    def stop(signum, frame):
        nonlocal stopped
        stopped = True

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    common = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning"]
    run_id = time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"
    log_dir = Path("logs/test-sources") / run_id
    log_dir.mkdir(parents=True)
    try:
        videos = {}
        for lane, source in enumerate(sources):
            if source["type"] == "timer":
                continue
            fps, width, height = (source[k] for k in ("fps", "width", "height"))
            video = Path(source["video"])
            if source["type"] == "static" or not matches_source(probe(video), source):
                # Prepare mismatched clips once with NVDEC/NVENC, then replay
                # without re-encoding during the semantic codec benchmark.
                identity = f"{video.resolve()}:{video.stat().st_size}:" \
                           f"{video.stat().st_mtime_ns}:{width}:{height}:{fps}:{source['type']}"
                digest = hashlib.sha256(identity.encode()).hexdigest()[:16]
                prepared = Path("data/video/generated") / f"{source['type']}-{digest}.mp4"
                prepared.parent.mkdir(parents=True, exist_ok=True)
                if not prepared.exists():
                    print(f"Preparing input{lane}: {width}x{height}, {fps} FPS ({video})", flush=True)
                    temporary = prepared.with_suffix(".part.mp4")
                    vf = f"scale={width}:{height},fps={fps}"
                    length = []
                    if source["type"] == "static":
                        vf = (f"scale={width}:{height},select=eq(n\\,0),"
                              f"loop=loop=-1:size=1:start=0,setpts=N/({fps}*TB)")
                        length = ["-frames:v", str(fps * 10)]
                    with (log_dir / f"prepare-{lane}.log").open("w") as log:
                        proc = subprocess.Popen(common + ["-y", "-hwaccel", "cuda", "-i",
                            str(video), "-vf", vf, "-r", str(fps)] + length +
                            hardware_encode(fps) + ["-movflags", "+faststart", str(temporary)],
                            stdout=log, stderr=subprocess.STDOUT)
                        children.append(proc)
                        while proc.poll() is None and not stopped:
                            time.sleep(.1)
                        if stopped:
                            return 0
                        if proc.returncode:
                            raise RuntimeError(f"Source preparation failed; see {log_dir}/prepare-{lane}.log")
                        temporary.replace(prepared)
                        children.remove(proc)
                video = prepared
            videos[lane] = video
        if stopped:
            return 0
        for lane, source in enumerate(sources):
            label = source["type"]
            fps, width, height = (source[k] for k in ("fps", "width", "height"))
            if label != "timer":
                video = videos[lane]
                command = common + ["-re", "-stream_loop", "-1", "-i", str(video),
                                    "-map", "0:v:0", "-an", "-c:v", "copy"]
            else:
                command = common + ["-re", "-f", "lavfi", "-i",
                    f"color=c=0x141c26:s={width}x{height}:r={fps}",
                    "-vf", timer_filter(str(font))] + hardware_encode(fps)
            log = (log_dir / f"input{lane}.log").open("w")
            logs.append(log)
            children.append(subprocess.Popen(command + ["-rtsp_transport", "tcp",
                "-f", "rtsp", f"{base}/input{lane}"], stdout=log,
                stderr=subprocess.STDOUT))
            print(f"input{lane}: {label}, {width}x{height}, {fps} FPS, {base}/input{lane}", flush=True)
        print(f"{len(sources)} sources. Logs: {log_dir}. Ctrl+C stops these publishers.",
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
