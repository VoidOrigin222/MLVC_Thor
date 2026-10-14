"""Compare five processes, five execution contexts, and real batch=5 on Thor."""
import argparse
import concurrent.futures
import json
import re
import statistics
import subprocess
import time
from pathlib import Path

ENCODER = "xidian@192.168.5.16"
DECODER = "xidian@192.168.5.17"
ROOT = "/home/xidian/workspace/MLVC_Thor"


def remote(host, script, timeout=150):
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", host, "python3", "-"],
                            input=script, text=True, capture_output=True,
                            encoding="utf8", timeout=timeout)
    if result.returncode:
        raise RuntimeError(host + "\n" + result.stdout + result.stderr)
    return result.stdout


def sources(label, start):
    remote(ENCODER, f'''
import os,signal,subprocess,json,time
from pathlib import Path
os.chdir({ROOT!r})
pidfile=Path('logs/five-source-pids.json')
if pidfile.exists():
 for item in json.loads(pidfile.read_text()):
  try:
   argv=Path('/proc/'+str(item['pid'])+'/cmdline').read_bytes().split(b'\\0')
   if argv and argv[0].endswith(b'ffmpeg') and any(b'/five-in' in arg for arg in argv):
    os.killpg(item['pid'],signal.SIGTERM)
  except (FileNotFoundError,ProcessLookupError):pass
items=[]
if {start!r}:
 time.sleep(.5)
 for lane in range(5):
  cmd=['ffmpeg','-nostdin','-hide_banner','-loglevel','warning','-re','-ss',str(lane*2),'-i','data/video/five-13fps-120s.mp4','-an','-c:v','copy','-rtsp_transport','tcp','-f','rtsp','rtsp://192.168.5.3:18554/five-in'+str(lane)]
  p=subprocess.Popen(cmd,stdout=open('logs/{label}-source-'+str(lane)+'.log','w'),stderr=subprocess.STDOUT,start_new_session=True)
  items.append({{'pid':p.pid,'cmd':cmd}})
pidfile.write_text(json.dumps(items))
''')


def prepare(host, label, scheme, frames):
    operation = "encode" if host == ENCODER else "decode"
    return remote(host, f'''
import os,re,subprocess,json
from pathlib import Path
os.chdir({ROOT!r})
label={label!r}; scheme={scheme!r}; operation={operation!r}
base=Path('configs/style-final-rtp.toml' if operation=='encode' else 'configs/style-final-dual-decode.toml').read_text()
configs=[]
for lane in range(5):
 s=re.sub(r'(?m)^frame_num\\s*=.*$', 'frame_num = {frames}',base)
 s=s.replace('39451',str(39480+2*lane))
 s=re.sub(r'(?m)^manifest\\s*=.*$', 'manifest = "models/manifest_thor_batch5.json"' if scheme=='batch5' else 'manifest = "models/manifest_thor.json"', s)
 s=re.sub(r'(?m)^profile_output\\s*=.*\\n','',s)
 extra='profile_output = "logs/'+label+'-'+operation+'-lane'+str(lane)+'-trace.json"\\n'
 if operation=='encode':
  s=re.sub(r'(?m)^input_video\\s*=.*$', 'input_rtsp = "rtsp://192.168.5.3:18554/five-in'+str(lane)+'"',s)
  s=s.replace('fps = 30','fps = 13').replace('target_fps = 0','target_fps = 13')
 else:
  s=s.replace('output_transport_mode = "none"','output_transport_mode = "rtsp"')
  s=re.sub(r'(?m)^output_transport_rtsp_url\\s*=.*$', 'output_transport_rtsp_url = "rtsp://192.168.5.3:18554/five-out'+str(lane)+'"', s)
  extra+='latency_probe_host = "192.168.5.16"\\nlatency_probe_port = '+str(39500+lane)+'\\n'
  extra+='input_rtp_queue_capacity = 256\\ninput_rtp_queue_bytes = 67108864\\ninput_rtp_overflow_policy = "fail"\\ninput_rtp_pending_frames = 256\\ninput_rtp_reorder_wait_ms = 500\\ninput_rtp_eos_wait_ms = 5000\\ninput_rtp_loss_policy = "fail"\\n'
 s=s.replace('[pipeline]',extra+'[pipeline]')
 path='configs/'+label+'-'+operation+'-lane'+str(lane)+'.toml'
 Path(path).write_text(s); configs.append(path)
env=os.environ.copy()
env.pop('MLVC_THOR_INPUT_DIGEST',None)
env.pop('MLVC_THOR_BATCH5',None)
env.pop('MLVC_THOR_SHARE_ENGINES',None)
env['MLVC_THOR_TIMING']='1'
if operation=='encode':env['MLVC_THOR_INPUT_TIMELINE']='logs/'+label+'-input'
procs=[]
def launch(cmd,log):
 p=subprocess.Popen(cmd,stdout=open(log,'w'),stderr=subprocess.STDOUT,env=env,start_new_session=True)
 procs.append({{'pid':p.pid,'cmd':cmd}}); return p
launch(['python3','tools/five_telemetry.py','logs/'+label+'-'+operation+'-telemetry.jsonl'],'logs/'+label+'-'+operation+'-telemetry.log')
if operation=='encode':
 for lane in range(5):
  launch(['python3','tools/collect_latency.py','--port',str(39500+lane),'--frames',{str(frames)!r},'--output','logs/'+label+'-lane'+str(lane)+'-acks.json'],'logs/'+label+'-lane'+str(lane)+'-collector.log')
codec=[]
if scheme=='processes':
 for lane,path in enumerate(configs):
  p=launch(['timeout','100','bin/bench/multi_'+operation,'--config',path],'logs/'+label+'-'+operation+'-lane'+str(lane)+'.log'); codec.append(p.pid)
else:
 p=launch(['timeout','100','bin/bench/mlvc_multi_thor',operation,scheme,'logs/'+label+'-'+operation,*configs],'logs/'+label+'-'+operation+'-process.log');codec.append(p.pid)
Path('logs/'+label+'-'+operation+'-pids.json').write_text(json.dumps(procs))
print(json.dumps({{'codec_pids':codec,'processes':procs}}))
''')


def stop_owned(host, label, operation):
    return remote(host, f'''
import json,os,signal
from pathlib import Path
os.chdir({ROOT!r})
p=Path('logs/{label}-{operation}-pids.json')
if p.exists():
 for item in json.loads(p.read_text()):
  pid=item['pid']
  try:
   argv=Path('/proc/'+str(pid)+'/cmdline').read_bytes().replace(b'\\0',b' ').decode()
   if {label!r} in argv:os.killpg(pid,signal.SIGTERM)
  except (FileNotFoundError,ProcessLookupError):pass
''')


def collect(host, label, operation, frames):
    return json.loads(remote(host, f'''
import json,re,csv,statistics
from pathlib import Path
import os
os.chdir({ROOT!r})
label={label!r}; operation={operation!r}; result=[]
for lane in range(5):
 log=Path('logs/'+label+'-'+operation+'-lane'+str(lane)+'.log').read_text()
 metrics={{m.group(1):float(m.group(2)) for m in re.finditer(r'(?m)^([a-zA-Z_0-9]+)=(\\d+(?:\\.\\d+)?)$',log)}}
 ok=operation+'=ok' in log
 r={{'lane':lane,'ok':ok,'metrics':metrics,'log_tail':log[-1500:] if not ok else ''}}
 if operation=='encode' and ok:
  events=json.loads(Path('logs/'+label+'-encode-lane'+str(lane)+'-trace.json').read_text())['traceEvents']
  samples={{int(e['args']['frame_index']):int(e['args']['source_read_ns'] or e['args']['monotonic_ns']) for e in events if e.get('name')=='source.frame.ready'}}
  ackpath=Path('logs/'+label+'-lane'+str(lane)+'-acks.json')
  acks=json.loads(ackpath.read_text()) if ackpath.exists() else {{}}
  valid=[i for i in samples if i>=96 and str(i) in acks]
  latency=[(int(acks[str(i)])-samples[i])/1e6 for i in valid]
  r['ack_count']=len(acks)
  if len(valid)>1:
   first=min(valid);last=max(valid)
   r['decode_ack_steady_fps']=(len(valid)-1)*1e9/(int(acks[str(last)])-int(acks[str(first)]))
  if latency:
   latency.sort();r['latency_ms']={{'mean':statistics.mean(latency),'p50':statistics.median(latency),'p95':latency[int(.95*(len(latency)-1))]}}
   r['steady_window_ns']=[samples[min(valid)],int(acks[str(max(valid))])]
  timeline=[]
  for p in Path('logs').glob(label+'-input-five-in'+str(lane)+'-*.csv'):
   timeline.extend(dict((k,int(v)) for k,v in row.items()) for row in csv.DictReader(p.open()))
  timeline.sort(key=lambda x:x['read_ns'])
  timeline=[row for row in timeline if row['frame']>=96 and row['pts_ns']>=0]
  if len(timeline)>2:
   delta=(timeline[-1]['read_ns']-timeline[0]['read_ns'])-(timeline[-1]['pts_ns']-timeline[0]['pts_ns'])
   r['input_backlog_growth_ms']=delta/1e6
   r['input_read_fps']=(len(timeline)-1)*1e9/(timeline[-1]['read_ns']-timeline[0]['read_ns'])
 result.append(r)
telemetry=[json.loads(line) for line in Path('logs/'+label+'-'+operation+'-telemetry.jsonl').read_text().splitlines()]
print(json.dumps({{'lanes':result,'telemetry':telemetry}}))
'''))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--scheme", choices=["processes", "contexts", "batch5"], required=True)
    p.add_argument("--frames", type=int, default=390)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--label")
    args = p.parse_args()
    label = args.label or "five-" + args.scheme
    try:
        sources(label, True)
        time.sleep(1)
        prepare(DECODER, label, args.scheme, args.frames)
        time.sleep(3)
        prepare(ENCODER, label, args.scheme, args.frames)
        print("Started " + label, flush=True)
        deadline = time.monotonic() + 105
        while time.monotonic() < deadline:
            time.sleep(8)
            with concurrent.futures.ThreadPoolExecutor() as pool:
                futures = [pool.submit(remote, host, f'''
from pathlib import Path
import json,os
os.chdir({ROOT!r})
logs=[Path('logs/{label}-{op}-lane'+str(i)+'.log') for i in range(5)]
logs+=list(Path('logs').glob('{label}-{op}-process.log'))
ok=sum('{op}=ok' in p.read_text() for p in logs if p.exists())
errors=[p.name for p in logs if p.exists() and ('failed:' in p.read_text() or 'lane_error=' in p.read_text())]
print(json.dumps({{'ok':ok,'errors':errors}}))
''') for host, op in ((ENCODER, "encode"), (DECODER, "decode"))]
                states = [json.loads(f.result()) for f in futures]
            print(json.dumps(states), flush=True)
            if all(s["ok"] == 5 for s in states) or any(s["errors"] for s in states):
                break
        # ACK files are emitted immediately once the last ACK is received.
        time.sleep(1)
        enc = collect(ENCODER, label, "encode", args.frames)
        dec = collect(DECODER, label, "decode", args.frames)
        result = {"scheme": args.scheme, "label": label,
                  "frames_per_lane": args.frames, "encode": enc, "decode": dec}
        result["complete"] = all(
            lane["ok"] and lane["metrics"].get("frames") == args.frames
            for side in (enc, dec) for lane in side["lanes"])
        result["complete"] &= all(lane.get("ack_count") == args.frames
                                  for lane in enc["lanes"])
        result["complete"] &= all(
            lane["metrics"].get("output_transport_rtsp_frames") == args.frames and
            lane["metrics"].get("forward_dropped_frames") == 0
            for lane in dec["lanes"])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
        print(str(args.output), flush=True)
        if not result["complete"]:
            raise RuntimeError("Incomplete trial; partial results saved to " + str(args.output))
    finally:
        for host, op in ((ENCODER, "encode"), (DECODER, "decode")):
            stop_owned(host, label, op)
        sources(label, False)


if __name__ == "__main__":
    main()
