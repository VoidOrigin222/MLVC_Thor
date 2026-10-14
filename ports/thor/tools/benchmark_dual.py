"""Run identical dual-board trials; ACK and source times share encoder clock."""
import argparse
import json
import re
import subprocess
import time

p=argparse.ArgumentParser()
p.add_argument('--variant',choices=['baseline','optimized','nvdec'],required=True)
p.add_argument('--source',choices=['offline','rtsp'],default='offline')
p.add_argument('--repeats',type=int,default=3)
args=p.parse_args()
root='/home/xidian/workspace/MLVC_Thor'
frames=537 if args.source=='offline' else 300

def remote(host,script,timeout=90):
    r=subprocess.run(['ssh','-o','BatchMode=yes',host,'python3','-'],input=script,text=True,
                     capture_output=True,timeout=timeout,encoding='utf8')
    if r.returncode:raise RuntimeError(host+': '+r.stdout+r.stderr)
    return r.stdout

for run in range(args.repeats):
    label=f'opt-{args.variant}-{args.source}-{run}'
    remote('Encoder',f'''
import os,subprocess
os.chdir({root!r})
from pathlib import Path
Path('logs/{label}-acks.json').unlink(missing_ok=True)
subprocess.Popen(['python3','tools/collect_latency.py','--frames',{str(frames)!r},'--output','logs/{label}-acks.json'],stdout=open('logs/{label}-collector.log','w'),stderr=subprocess.STDOUT,start_new_session=True)
''')
    remote('Decoder',f'''
import os,re,subprocess
from pathlib import Path
os.chdir({root!r})
s=Path('configs/decoder_rtp.toml').read_text()
s=s.replace('39340','39450').replace('8554/ulbvc','18554/ulbvc-thor')
if {args.source!r}=='offline':s=s.replace('output_transport_mode = "rtsp"','output_transport_mode = "none"')
s=s.replace('[pipeline]','latency_probe_host = "192.168.5.16"\\nlatency_probe_port = 39460\\nprofile_output = "logs/{label}-decode-trace.json"\\n[pipeline]')
Path('configs/{label}-decode.toml').write_text(s)
subprocess.Popen(['timeout','70','bin/bench/bench_decode','--config','configs/{label}-decode.toml'],stdout=open('logs/{label}-decode.log','w'),stderr=subprocess.STDOUT,start_new_session=True)
''')
    text=remote('Encoder',f'''
import os,re,subprocess,json,time,statistics
from pathlib import Path
os.chdir({root!r})
s=Path('configs/encoder_offline.toml').read_text().replace('39340','39450')
s=re.sub(r'(?m)^frame_num\\s*=.*$','frame_num = {frames}',s)
if {args.source!r}=='rtsp':s=s.replace('input_video = "data/video/614lab.mp4"','input_rtsp = "rtsp://192.168.5.3:18554/input-thor"')
if {args.variant!r} in ('optimized','nvdec'):
 if re.search(r'(?m)^motion_decode_threads\\s*=',s):s=re.sub(r'(?m)^motion_decode_threads\\s*=.*$','motion_decode_threads = 4',s)
 else:s=s.replace('[pipeline]','motion_decode_threads = 4\\n[pipeline]')
else:
 s=re.sub(r'(?m)^(input_device_resident|motion_decode_threads|video_decode_backend)\\s*=.*\\n','',s)
if {args.variant!r}=='nvdec':
 s=re.sub(r'(?m)^video_decode_backend\\s*=.*\\n','',s)
 s=s.replace('[pipeline]','video_decode_backend = "nvdec"\\n[pipeline]')
s=s.replace('[pipeline]','profile_warmup_frames = 96\\nprofile_output = "logs/{label}-encode-trace.json"\\n[pipeline]')
Path('configs/{label}-encode.toml').write_text(s)
with open('logs/{label}-encode.log','w') as log:
 subprocess.run(['bin/bench/{'baseline_encode' if args.variant=='baseline' else 'nvdec_encode' if args.variant=='nvdec' else 'optimized_encode'}','--config','configs/{label}-encode.toml'],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
for _ in range(100):
 if Path('logs/{label}-acks.json').exists():break
 time.sleep(.1)
acks=json.loads(Path('logs/{label}-acks.json').read_text())
trace=json.loads(Path('logs/{label}-encode-trace.json').read_text())
samples=[];tensor_samples=[]
for e in trace['traceEvents']:
 if e.get('name')!='source.frame.ready':continue
 a=e['args'];i=int(a['frame_index'])
 if i<96:continue
 samples.append((int(acks[str(i)])-int(a['source_read_ns'] or a['monotonic_ns']))/1e6)
 tensor_samples.append((int(acks[str(i)])-int(a['monotonic_ns']))/1e6)
log=Path('logs/{label}-encode.log').read_text()
metrics={{m.group(1):float(m.group(2)) for m in re.finditer(r'(?m)^([a-zA-Z_0-9]+)=(\\d+(?:\\.\\d+)?)$',log)}}
assert len(samples)=={frames}-96 and min(samples)>=0, 'Incomplete or stale latency samples'
samples.sort();result={{'label':{label!r},'frames':{frames},'steady_samples':len(samples),'encode_fps':metrics['encode_fps'],'bgr_ready_to_decode_ack_mean_ms':statistics.mean(samples),'bgr_ready_to_decode_ack_p50_ms':statistics.median(samples),'bgr_ready_to_decode_ack_p95_ms':samples[int(.95*(len(samples)-1))],'tensor_ready_to_decode_ack_mean_ms':statistics.mean(tensor_samples),'metrics':metrics}}
Path('logs/{label}-result.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result))
''')
    print(text.strip(),flush=True)
    decoded=remote('Decoder',f'''
import os,time,re
from pathlib import Path
os.chdir({root!r})
for _ in range(100):
 s=Path('logs/{label}-decode.log').read_text()
 if 'decode=ok' in s:break
 time.sleep(.1)
assert 'decode=ok' in s,s[-1000:]
assert re.search(r'^frames={frames}$',s,re.M)
print('decode=ok frames={frames}')
''')
    print(decoded.strip(),flush=True)
