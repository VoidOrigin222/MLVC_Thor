"""Isolated on-board MP4 trials with steady-window GPU telemetry."""
import collections
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import threading
import time

root = Path('/home/xidian/workspace/MLVC_Thor')
parser=argparse.ArgumentParser()
parser.add_argument('--binary',default='bin/bench/nvdec_encode')
parser.add_argument('--backends',nargs='+',choices=('nvdec',),default=['nvdec'])
parser.add_argument('--prefix',default='video-hardware')
args=parser.parse_args()
os.chdir(root)
results = []
for repeat in range(3):
    for backend in args.backends:
        label = f'{args.prefix}-{backend}-{repeat}'
        config = (root / 'configs/nvdec-smoke.toml').read_text()
        config = config.replace('video_decode_backend = "nvdec"',
                                f'video_decode_backend = "{backend}"')
        config = config.replace('nvdec-smoke.mlvc', f'{label}.mlvc')
        config = config.replace('nvdec-smoke-trace.json', f'{label}-trace.json')
        (root / f'configs/{label}.toml').write_text(config)
        readings = []
        monitor = subprocess.Popen([
            'nvidia-smi', '--query-gpu=utilization.gpu,utilization.memory,'
            'utilization.encoder,utilization.decoder,clocks.current.graphics',
            '--format=csv,noheader,nounits', '--loop-ms=100'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def collect():
            for line in monitor.stdout:
                values = []
                for value in line.strip().split(','):
                    try:
                        values.append(float(value))
                    except ValueError:
                        values.append(None)
                readings.append({'monotonic_ns': time.monotonic_ns(), 'values': values})
        thread = threading.Thread(target=collect)
        thread.start()
        try:
            with (root / f'logs/{label}.log').open('w') as log:
                subprocess.run([args.binary, '--config', f'configs/{label}.toml'],
                               env=dict(os.environ, MLVC_THOR_TIMING='1'),
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=75)
        finally:
            monitor.terminate()
            try:
                monitor.wait(timeout=5)
            except subprocess.TimeoutExpired:
                monitor.kill()
                monitor.wait()
            thread.join()
        log = (root / f'logs/{label}.log').read_text()
        assert re.search(r'^frames=537$', log, re.M), log[-1500:]
        timings = {}
        for match in re.finditer(r'thor_timing name=(\S+) samples=(\d+) avg_ms=([\d.]+)', log):
            timings[match[1]] = {'samples': int(match[2]), 'avg_ms': float(match[3])}
        events = json.loads((root / f'logs/{label}-trace.json').read_text())['traceEvents']
        offsets = [int(e['args']['monotonic_ns']) - float(e['ts'])*1000
                   for e in events if e.get('name') == 'source.frame.ready']
        offset = statistics.median(offsets)
        models = sorted([e for e in events if e.get('name') == 'pipeline.node.acl_stage.MLVCEncoder'
                         and e.get('ph') == 'X'], key=lambda e: e['ts'])
        assert len(models) == 537, len(models)
        start = offset + models[96]['ts']*1000
        end = offset + (models[-1]['ts'] + models[-1]['dur'])*1000
        steady = [r for r in readings if start <= r['monotonic_ns'] <= end]
        assert len(steady) > 30, len(steady)
        averages = {}
        for index, name in enumerate(('gpu_percent', 'memory_percent', 'nvenc_percent',
                                      'nvdec_percent', 'gpu_clock_mhz')):
            values = [r['values'][index] for r in steady if r['values'][index] is not None]
            averages[name] = statistics.mean(values) if values else None
        durations = collections.defaultdict(list)
        for e in events:
            if e.get('ph') == 'X':
                durations[e['name']].append(float(e['dur'])/1000)
        trace_means = {name: statistics.mean(values[96:]) for name, values in durations.items()
                       if len(values) == 537 and name in ('video.read.frame', 'entropy.rans_encode',
                                                        'bitstream.frame_write')}
        result = dict(label=label, backend=backend, frames=537, warmup_frames=96,
                      fps=float(re.search(r'^encode_fps=([\d.]+)$', log, re.M)[1]),
                      timings=timings, trace_means=trace_means,
                      telemetry_mean=averages, telemetry_samples=len(steady),
                      steady_start_ns=start, steady_end_ns=end,
                      telemetry_raw=readings,
                      bitstream_sha256=hashlib.sha256((root/f'data/bitstreams/{label}.mlvc').read_bytes()).hexdigest())
        (root / f'logs/{label}-result.json').write_text(json.dumps(result, indent=2))
        results.append(result)
        print(label, result['fps'], averages, result['bitstream_sha256'], flush=True)
summary = {}
for backend in args.backends:
    runs = [r for r in results if r['backend'] == backend]
    summary[backend] = dict(fps_mean=statistics.mean(r['fps'] for r in runs),
        fps_runs=[r['fps'] for r in runs],
        timing_means_ms={key: statistics.mean(r['timings'][key]['avg_ms'] for r in runs)
                         for key in runs[0]['timings']},
        trace_means_ms={key: statistics.mean(r['trace_means'][key] for r in runs)
                        for key in runs[0]['trace_means']},
        telemetry_mean={key: statistics.mean(r['telemetry_mean'][key] for r in runs)
                        if all(r['telemetry_mean'][key] is not None for r in runs) else None
                        for key in runs[0]['telemetry_mean']},
        bitstream_sha256=[r['bitstream_sha256'] for r in runs])
(root/f'logs/{args.prefix}-summary.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
