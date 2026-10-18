"""Owned hardware H.264 comparison publishers; no software codec fallback."""

from pathlib import Path
import os
import subprocess
import tempfile
import threading
import time


def ffmpeg_path(configured="auto"):
    if configured != "auto":
        return configured
    bundled = Path(__file__).resolve().parent / "ffmpeg.exe"
    return str(bundled) if bundled.is_file() else "ffmpeg"


def command(config, channel, encoder):
    args = [
        ffmpeg_path(config["h264_ffmpeg"]),
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "warning",
        "-xerror",
    ]
    if encoder == "h264_qsv":
        args += ["-hwaccel", "d3d11va"]
    else:
        args += ["-hwaccel", "cuda", "-hwaccel_output_format", "cuda"]
    args += [
        "-rtsp_transport",
        "tcp",
        "-timeout",
        "5000000",
        "-analyzeduration",
        "250000",
        "-probesize",
        "262144",
        # FFmpeg frame threading can retain 15 frames even with D3D11VA.
        # At 12 FPS that adds 1.25 s before the hardware encoder sees a frame.
        "-threads:v",
        "1",
        "-i",
        channel["original"],
        "-map",
        "0:v:0",
        "-an",
        "-c:v",
        encoder,
    ]
    if encoder == "h264_qsv":
        # -q:v applies FFmpeg's QP-to-lambda conversion required by QSV CQP.
        args += [
            "-preset",
            "veryfast",
            "-flags:v",
            "+qscale",
            "-q:v",
            str(config["h264_qp"]),
            "-i_qfactor",
            "1",
            "-i_qoffset",
            "0",
            "-look_ahead",
            "0",
            "-async_depth",
            "1",
        ]
    else:
        args += [
            "-preset",
            "p1",
            "-tune",
            "ull",
            "-rc",
            "constqp",
            "-qp",
            str(config["h264_qp"]),
            "-rc-lookahead",
            "0",
        ]
    return args + [
        "-bf",
        "0",
        "-g",
        "96",
        "-rtsp_transport",
        "tcp",
        "-f",
        "rtsp",
        channel["h264"],
    ]


class Publishers:
    def __init__(self):
        self.lock = threading.RLock()
        self.jobs = {}
        self.encoder = None
        self.probe_key = None
        self.probe_after = 0
        self.error = ""
        self.enabled = None
        self.sequence = 0
        self.closed = False
        self.log_root = Path(tempfile.gettempdir()) / "SemanticFiveMonitor-h264"

    def stop(self, channel=None):
        with self.lock:
            ids = [channel] if channel else list(self.jobs)
            for key in ids:
                job = self.jobs.pop(key, None)
                if not job:
                    continue
                process = job["process"]
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
                job["log"].close()

    def probe(self, config):
        key = (config["h264_ffmpeg"], config["h264_encoder"])
        if key == self.probe_key and (
            self.encoder or time.monotonic() < self.probe_after
        ):
            return
        self.probe_key = key
        self.encoder = None
        choices = ["h264_qsv", "h264_nvenc"] if os.name == "nt" else ["h264_nvenc"]
        if config["h264_encoder"] != "auto":
            choices = [config["h264_encoder"]]
        for encoder in choices:
            args = [
                ffmpeg_path(config["h264_ffmpeg"]),
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "color=c=black:s=128x128:r=1",
                "-frames:v",
                "1",
                "-c:v",
                encoder,
                "-f",
                "null",
                "-",
            ]
            try:
                result = subprocess.run(
                    args,
                    capture_output=True,
                    timeout=4,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                )
                if result.returncode == 0:
                    self.encoder = encoder
                    self.error = ""
                    return
            except (OSError, subprocess.TimeoutExpired):
                pass
        self.probe_after = time.monotonic() + 30
        self.error = "H.264 硬件编码不可用，请检查 FFmpeg 与显卡驱动"

    def update(self, config, ready_paths):
        with self.lock:
            if self.closed:
                return
            enabled = (
                config["h264_auto_start"] if self.enabled is None else self.enabled
            )
            if not enabled:
                self.stop()
                return
            candidates = [
                c
                for c in config["channels"]
                if c["original"].split("/", 3)[-1] in ready_paths
            ]
            if not candidates:
                return
            self.probe(config)
            if not self.encoder or self.closed:
                return
            valid_ids = {c["id"] for c in config["channels"]}
            for key in list(self.jobs):
                if key not in valid_ids:
                    self.stop(key)
            for channel in candidates:
                args = command(config, channel, self.encoder)
                job = self.jobs.get(channel["id"])
                if job and job["args"] != args:
                    self.stop(channel["id"])
                    job = None
                if job and job["process"].poll() is None:
                    continue
                if job and time.monotonic() < job["started"] + 10:
                    continue
                if job:
                    self.stop(channel["id"])
                # A ready external comparison stream belongs to its publisher.
                if channel["h264"].split("/", 3)[-1] in ready_paths:
                    continue
                self.log_root.mkdir(parents=True, exist_ok=True)
                self.sequence += 1
                path = (
                    self.log_root / f"{os.getpid()}-{channel['id']}-{self.sequence}.log"
                )
                log = path.open("wb")
                try:
                    process = subprocess.Popen(
                        args,
                        stdin=subprocess.DEVNULL,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                    )
                except OSError:
                    log.close()
                    self.error = "无法启动 H.264 推流，请检查 FFmpeg 路径"
                    continue
                self.jobs[channel["id"]] = {
                    "process": process,
                    "log": log,
                    "args": args,
                    "started": time.monotonic(),
                    "path": str(path),
                }

    def status(self):
        with self.lock:
            return {
                "encoder": self.encoder,
                "error": self.error,
                "channels": {
                    key: job["process"].poll() is None for key, job in self.jobs.items()
                },
            }

    def close(self):
        self.closed = True
        self.stop()
