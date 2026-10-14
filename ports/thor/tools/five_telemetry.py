"""Sample unified-memory Thor utilization without spawning nvidia-smi per tick."""
import argparse
import ctypes as ct
import json
import time
from pathlib import Path


class Utilization(ct.Structure):
    _fields_ = [("gpu", ct.c_uint), ("memory", ct.c_uint)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=float, default=120)
    args = parser.parse_args()
    label = args.output.name.rsplit("-", 2)[0]
    lib = ct.CDLL("libnvidia-ml.so.1")
    assert lib.nvmlInit_v2() == 0
    handle = ct.c_void_p()
    assert lib.nvmlDeviceGetHandleByIndex_v2(0, ct.byref(handle)) == 0
    deadline = time.monotonic() + args.seconds
    previous_cpu = None
    with args.output.open("w") as output:
        while time.monotonic() < deadline:
            util = Utilization()
            row = {"ns": time.monotonic_ns()}
            if lib.nvmlDeviceGetUtilizationRates(handle, ct.byref(util)) == 0:
                row.update(gpu=util.gpu, memory_util=util.memory)
            for name, func in (("nvenc", "nvmlDeviceGetEncoderUtilization"),
                               ("nvdec", "nvmlDeviceGetDecoderUtilization")):
                value, period = ct.c_uint(), ct.c_uint()
                if getattr(lib, func)(handle, ct.byref(value), ct.byref(period)) == 0:
                    row[name] = value.value
            mem = {}
            for line in Path("/proc/meminfo").read_text().splitlines():
                key, value = line.split(":", 1)
                mem[key] = int(value.split()[0])
            row["system_used_mib"] = (mem["MemTotal"] - mem["MemAvailable"]) / 1024
            rss = 0
            for path in Path("/proc").glob("[0-9]*/cmdline"):
                try:
                    argv = path.read_bytes().split(b"\0")
                    if argv and Path(argv[0].decode()).name in (
                            "mlvc_multi_thor", "multi_encode", "multi_decode") and any(
                                label.encode() in value for value in argv):
                        status = (path.parent / "status").read_text()
                        rss += int(next(line.split()[1] for line in status.splitlines()
                                        if line.startswith("VmRSS:")))
                except (FileNotFoundError, ProcessLookupError, StopIteration):
                    pass
            row["codec_rss_mib"] = rss / 1024
            cpu = [int(v) for v in Path("/proc/stat").read_text().splitlines()[0].split()[1:]]
            current = (sum(cpu[:8]), cpu[3] + cpu[4])
            if previous_cpu and current[0] != previous_cpu[0]:
                row["system_cpu_percent"] = 100 * (1 - (current[1] - previous_cpu[1]) /
                                                    (current[0] - previous_cpu[0]))
            previous_cpu = current
            output.write(json.dumps(row) + "\n")
            output.flush()
            time.sleep(0.2)
    lib.nvmlShutdown()


if __name__ == "__main__":
    main()
