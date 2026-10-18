"""Validate two still scenes, two moving scenes and a live timer on two Thors."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import time

import benchmark_five as bench


def source(host, label, timeout):
    return bench.remote(
        host,
        f"""
import json,os,subprocess
from pathlib import Path
os.chdir({bench.ROOT!r})
config=json.loads(Path('configs/five/test_sources.json').read_text())
config['rtsp_base']={bench.RTSP_BASE!r}
path='configs/{label}-sources.json'
Path(path).write_text(json.dumps(config))
tool='src/MLVC/ports/thor/tools/publish_test_sources.py'
cmd=['python3',tool,'--config',path,'--duration',{str(timeout+50)!r}]
p=subprocess.Popen(cmd,stdout=open('logs/{label}-sources.log','w'),
 stderr=subprocess.STDOUT,start_new_session=True)
Path('logs/{label}-sources-pids.json').write_text(
 json.dumps([{{'pid':p.pid,'cmd':cmd}}]))
print('Source supervisor',p.pid)
""",
    )


def archive(host, label, operation, output):
    bench.remote(
        host,
        f"""
import tarfile
from pathlib import Path
root=Path({bench.ROOT!r})
target=root/'logs/{label}-{operation}-evidence.tar.gz'
with tarfile.open(target,'w:gz',compresslevel=1) as tar:
 for folder in ['logs','configs']:
  for path in (root/folder).glob('{label}*'):
   if path!=target and path.is_file():
    tar.add(path,arcname=str(path.relative_to(root)))
""",
    )
    name = f"{label}-{operation}-evidence.tar.gz"
    subprocess.run(["scp", f"{host}:{bench.ROOT}/logs/{name}", str(output)], check=True)
    with tarfile.open(output / name) as tar:
        tar.extractall(output / label / operation, filter="data")


def state(host, label, operation):
    return json.loads(
        bench.remote(
            host,
            f"""
import json
from pathlib import Path
root=Path({bench.ROOT!r})
logs=[root/('logs/{label}-{operation}-lane'+str(i)+'.log') for i in range(5)]
logs.append(root/'logs/{label}-{operation}-process.log')
texts=[p.read_text() for p in logs if p.exists()]
print(json.dumps({{'ok':sum('{operation}=ok' in s for s in texts),
 'errors':[s[-1000:] for s in texts if 'failed:' in s or 'lane_error=' in s]}}))
""",
        )
    )


def check_complete(result, frames):
    for operation in ("encode", "decode"):
        if len(result[operation]["lanes"]) != 5:
            return False
        for lane in result[operation]["lanes"]:
            if not lane["ok"] or lane["metrics"].get("frames") != frames:
                return False
            if operation == "encode":
                if lane.get("ack_count") != frames:
                    return False
            else:
                if lane["metrics"].get("output_transport_rtsp_frames") != frames:
                    return False
                for key in (
                    "rtp_lost_packets",
                    "rtp_queue_overflows",
                    "rtp_queue_dropped_units",
                    "rtp_crc_failed_units",
                    "rtp_dependency_gaps",
                    "rtp_skipped_frames",
                    "forward_dropped_frames",
                ):
                    if lane["metrics"].get(key) != 0:
                        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=3120)
    parser.add_argument("--encoder", default=bench.ENCODER)
    parser.add_argument("--decoder", default=bench.DECODER)
    parser.add_argument("--root", default=bench.ROOT)
    parser.add_argument("--rtsp-base", default=bench.RTSP_BASE)
    parser.add_argument("--binary-dir", default="bin")
    parser.add_argument(
        "--legacy-gpu-path",
        action="store_true",
        help="Diagnostic control: concurrent graphs with staged host inputs",
    )
    parser.add_argument(
        "--live-receiver",
        action="store_true",
        help="Keep production receive queue and recovery settings",
    )
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", args.label):
        parser.error("Use a unique label of 1-64 letters, digits, - or _")
    if args.frames <= 96:
        parser.error("frames must exceed the 96-frame startup window")
    if not args.rtsp_base.startswith("rtsp://"):
        parser.error("rtsp-base must start with rtsp://")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    target = output / (args.label + ".json")
    if target.exists() or (output / args.label).exists():
        parser.error("Label already exists in this output directory")
    bench.ENCODER, bench.DECODER, bench.ROOT = args.encoder, args.decoder, args.root
    bench.RTSP_BASE = args.rtsp_base.rstrip("/")
    timeout = int(args.frames / 12) + 80
    runtime_environment = (
        {"MLVC_THOR_SERIAL_INFERENCE": "0", "MLVC_THOR_DIRECT_HOST_INPUT": "0"}
        if args.legacy_gpu_path
        else {}
    )
    observer = args.root + "/logs/" + args.label + "-trace.so"
    # Compile the retained observer from source; it records successful sends
    # and receives without modifying the codec's packets or buffering policy.
    for host in (args.encoder, args.decoder):
        bench.remote(
            host,
            f"""
import os,subprocess
from pathlib import Path
os.chdir({args.root!r})
assert not list(Path('logs').glob({(args.label+'*')!r})), 'Choose a new label'
for path in Path('/proc').glob('[0-9]*/cmdline'):
 try:
  argv=path.read_bytes().split(b'\\0')
 except (FileNotFoundError,PermissionError,ProcessLookupError):
  continue
 if argv and Path(os.fsdecode(argv[0])).name in ('encoder','decoder','mlvc_multi_thor','mlvc_encode_thor','mlvc_decode_thor'):
  raise RuntimeError('Stop the existing codec workload before benchmarking')
subprocess.run(['gcc','-shared','-fPIC','-O2',
 'src/MLVC/ports/thor/tools/trace_rtp.c','-ldl','-o',{observer!r}],check=True)
""",
        )
    try:
        print(source(args.encoder, args.label, timeout), flush=True)
        time.sleep(3)
        for host, operation in ((args.decoder, "decode"), (args.encoder, "encode")):
            print(
                bench.prepare(
                    host,
                    args.label,
                    "contexts",
                    args.frames,
                    12,
                    1,
                    args.binary_dir,
                    timeout,
                    input_prefix="input",
                    output_prefix="five-out",
                    packet_observer=observer,
                    complete_frame_validation=not args.live_receiver,
                    runtime_environment=runtime_environment,
                ),
                flush=True,
            )
            if operation == "decode":
                time.sleep(3)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(5)
            states = [
                state(host, args.label, op)
                for host, op in ((args.encoder, "encode"), (args.decoder, "decode"))
            ]
            print(states, flush=True)
            if all(s["ok"] == 5 for s in states) or any(s["errors"] for s in states):
                break
        time.sleep(1)
        result = {
            "label": args.label,
            "frames_per_lane": args.frames,
            "target_fps": 12,
            "live_receiver": args.live_receiver,
            "binary_dir": args.binary_dir,
            "runtime_environment": runtime_environment,
            "scenes": ["static", "static", "dynamic", "dynamic", "timer"],
            "encode": bench.collect(
                args.encoder, args.label, "encode", args.frames, input_prefix="input"
            ),
            "decode": bench.collect(
                args.decoder, args.label, "decode", args.frames, input_prefix="input"
            ),
        }
        result["complete"] = check_complete(result, args.frames)
        target.write_text(json.dumps(result, indent=2), encoding="utf-8")
        if not result["complete"]:
            raise RuntimeError("Incomplete trial; inspect " + str(target))
    finally:
        # Match both the recorded PID and the unique label before signalling.
        failed = sys.exc_info()[0] is not None
        cleanup_errors = []
        for host, operation in (
            (args.encoder, "encode"),
            (args.decoder, "decode"),
            (args.encoder, "sources"),
        ):
            try:
                bench.stop_owned(host, args.label, operation)
            except Exception as error:
                cleanup_errors.append(f"{host} {operation}: {error}")
        for host, operation in ((args.encoder, "encode"), (args.decoder, "decode")):
            try:
                archive(host, args.label, operation, output)
            except Exception as error:
                cleanup_errors.append(f"{host} evidence: {error}")
        if cleanup_errors:
            message = "\n".join(cleanup_errors)
            if not failed:
                raise RuntimeError(message)
            print(message, file=sys.stderr)
    print(target, flush=True)


if __name__ == "__main__":
    main()
