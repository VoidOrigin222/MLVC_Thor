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
RTSP_BASE = "rtsp://192.168.5.3:18554"


def remote(host, script, timeout=150):
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", host, "python3", "-"],
        input=script,
        text=True,
        capture_output=True,
        encoding="utf8",
        timeout=timeout,
    )
    if result.returncode:
        raise RuntimeError(host + "\n" + result.stdout + result.stderr)
    return result.stdout


def sources(label, start, loop=False, duration_seconds=120):
    remote(
        ENCODER,
        f"""
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
 video=Path('data/video/five-13fps-120s.mp4')
 if {loop!r}:
  # Expand without an input seek; combining -ss and live -stream_loop can
  # repeat timestamps and create bursts at every wrap. Publish a finite file.
  expanded=Path('data/video/benchmark-loop-{label}.mp4')
  subprocess.run(['ffmpeg','-nostdin','-y','-hide_banner','-loglevel','error','-stream_loop','-1','-i',str(video),'-t','{duration_seconds}','-an','-c:v','copy','-movflags','+faststart',str(expanded)],check=True)
  video=expanded
 for lane in range(5):
  cmd=['ffmpeg','-nostdin','-hide_banner','-loglevel','warning','-re','-ss',str(lane*2),'-i',str(video),'-an','-c:v','copy','-rtsp_transport','tcp','-f','rtsp',{RTSP_BASE!r}+'/five-in'+str(lane)]
  p=subprocess.Popen(cmd,stdout=open('logs/{label}-source-'+str(lane)+'.log','w'),stderr=subprocess.STDOUT,start_new_session=True)
  items.append({{'pid':p.pid,'cmd':cmd}})
pidfile.write_text(json.dumps(items))
""",
    )


def prepare(
    host,
    label,
    scheme,
    frames,
    target_fps,
    entropy_workers,
    binary_dir,
    timeout_seconds=100,
):
    operation = "encode" if host == ENCODER else "decode"
    return remote(
        host,
        f"""
import os,re,subprocess,json
from pathlib import Path
os.chdir({ROOT!r})
label={label!r}; scheme={scheme!r}; operation={operation!r}
config_mode='contexts' if scheme=='processes' else scheme
tool_root=Path('src/MLVC/ports/thor/tools')
if not tool_root.exists():tool_root=Path('tools')
configs=[]
for lane in range(5):
 template=Path('configs/five')/config_mode/(operation+'-'+str(lane)+'.toml')
 if not template.exists():
  template=Path('src/MLVC/ports/thor/configs/five')/config_mode/template.name
 base=template.read_text()
 s=re.sub(r'(?m)^frame_num\\s*=.*$', 'frame_num = {frames}',base)
 s=re.sub(r'(?m)^manifest\\s*=.*$', 'manifest = "models/manifest_thor_batch5.json"' if scheme=='batch5' else 'manifest = "models/manifest_thor.json"', s)
 s=re.sub(r'(?m)^profile_output\\s*=.*\\n','',s)
 extra='profile_output = "logs/'+label+'-'+operation+'-lane'+str(lane)+'-trace.json"\\n'
 if operation=='encode':
  s=re.sub(r'(?m)^input_rtsp\\s*=.*$', 'input_rtsp = "{RTSP_BASE}/five-in'+str(lane)+'"',s)
  s=re.sub(r'(?m)^output_transport_host\\s*=.*$', 'output_transport_host = "{DECODER.split('@')[-1]}"',s)
  s=re.sub(r'(?m)^(fps|target_fps)\\s*=.*$', lambda m:m.group(1)+' = {target_fps}',s)
  s=re.sub(r'(?m)^profile_warmup_frames\\s*=.*\\n','',s)
  extra+='profile_warmup_frames = 96\\n'
 else:
  s=re.sub(r'(?m)^output_transport_rtsp_url\\s*=.*$', 'output_transport_rtsp_url = "{RTSP_BASE}/five-out'+str(lane)+'"', s)
  extra+='latency_probe_host = "{ENCODER.split('@')[-1]}"\\nlatency_probe_port = '+str(39500+lane)+'\\n'
  receive_policy={{'input_rtp_queue_capacity':256,'input_rtp_queue_bytes':67108864,
   'input_rtp_overflow_policy':'fail','input_rtp_pending_frames':256,
   'input_rtp_reorder_wait_ms':500,'input_rtp_eos_wait_ms':5000,
   'input_rtp_loss_policy':'fail'}}
  for key,value in receive_policy.items():
   s=re.sub(r'(?m)^'+re.escape(key)+r'\\s*=.*$',key+' = '+json.dumps(value),s)
  s=re.sub(r'(?m)^entropy_workers\\s*=.*$', 'entropy_workers = {entropy_workers}',s)
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
launch(['python3',str(tool_root/'five_telemetry.py'),'logs/'+label+'-'+operation+'-telemetry.jsonl','--seconds','{timeout_seconds + 15}'],'logs/'+label+'-'+operation+'-telemetry.log')
if operation=='encode':
 for lane in range(5):
  launch(['python3',str(tool_root/'collect_latency.py'),'--port',str(39500+lane),'--frames',{str(frames)!r},'--seconds','{timeout_seconds + 15}','--peer',{DECODER.split('@')[-1]!r},'--output','logs/'+label+'-lane'+str(lane)+'-acks.json'],'logs/'+label+'-lane'+str(lane)+'-collector.log')
codec=[]
if scheme=='processes':
 for lane,path in enumerate(configs):
  binary='{binary_dir}/mlvc_'+operation+'_thor' if {binary_dir!r}!='bin/bench' else '{binary_dir}/multi_'+operation
  p=launch(['timeout','{timeout_seconds}',binary,'--config',path],'logs/'+label+'-'+operation+'-lane'+str(lane)+'.log'); codec.append(p.pid)
else:
 p=launch(['timeout','{timeout_seconds}','{binary_dir}/mlvc_multi_thor',operation,scheme,'logs/'+label+'-'+operation,*configs],'logs/'+label+'-'+operation+'-process.log');codec.append(p.pid)
Path('logs/'+label+'-'+operation+'-pids.json').write_text(json.dumps(procs))
print(json.dumps({{'codec_pids':codec,'processes':procs}}))
""",
    )


def stop_owned(host, label, operation):
    return remote(
        host,
        f"""
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
""",
    )


def collect(host, label, operation, frames):
    return json.loads(
        remote(
            host,
            f"""
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
   ordered=sorted(valid); first=ordered[:120]; last=[i for i in ordered if i>=max(96,{frames}-120)]
   first_latency=statistics.mean((int(acks[str(i)])-samples[i])/1e6 for i in first)
   last_latency=statistics.mean((int(acks[str(i)])-samples[i])/1e6 for i in last) if last else None
   r['latency_sample_frame_range']=[min(valid),max(valid)]
   r['tail_samples_complete']=len(last)==min(120,{frames}-96)
   r['latency_first120_mean_ms']=first_latency
   if r['tail_samples_complete']:
    r['latency_last120_mean_ms']=last_latency
    r['latency_growth_ms']=last_latency-first_latency
   if r['tail_samples_complete'] and len(last)>1:
    r['decode_ack_tail_fps']=(len(last)-1)*1e9/(int(acks[str(last[-1])])-int(acks[str(last[0])]))
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
""",
        )
    )


def main():
    p = argparse.ArgumentParser()
    p.add_argument(
        "--scheme", choices=["processes", "contexts", "batch5"], required=True
    )
    p.add_argument("--frames", type=int, default=390)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--label")
    p.add_argument("--target-fps", type=int, default=13)
    p.add_argument("--decoder-entropy-workers", type=int, default=4)
    p.add_argument(
        "--binary-dir",
        choices=["bin", "bin/bench", "bin/latency-test"],
        default="bin",
    )
    p.add_argument("--timeout-seconds", type=int, default=100)
    p.add_argument("--loop-input", action="store_true")
    args = p.parse_args()
    if args.frames <= 96:
        p.error("Need more than 96 frames to measure the steady-state window")
    if args.target_fps < 1 or args.decoder_entropy_workers < 1:
        p.error("Target FPS and entropy workers must be positive")
    if args.timeout_seconds <= args.frames / args.target_fps:
        p.error("Timeout must exceed the input video duration")
    label = args.label or "five-" + args.scheme
    try:
        sources(
            label,
            True,
            loop=args.loop_input,
            duration_seconds=args.timeout_seconds + 15,
        )
        time.sleep(1)
        prepare(
            DECODER,
            label,
            args.scheme,
            args.frames,
            args.target_fps,
            args.decoder_entropy_workers,
            args.binary_dir,
            args.timeout_seconds,
        )
        time.sleep(3)
        prepare(
            ENCODER,
            label,
            args.scheme,
            args.frames,
            args.target_fps,
            args.decoder_entropy_workers,
            args.binary_dir,
            args.timeout_seconds,
        )
        print("Started " + label, flush=True)
        deadline = time.monotonic() + args.timeout_seconds + 5
        while time.monotonic() < deadline:
            time.sleep(8)
            with concurrent.futures.ThreadPoolExecutor() as pool:
                futures = [
                    pool.submit(
                        remote,
                        host,
                        f"""
from pathlib import Path
import json,os
os.chdir({ROOT!r})
logs=[Path('logs/{label}-{op}-lane'+str(i)+'.log') for i in range(5)]
logs+=list(Path('logs').glob('{label}-{op}-process.log'))
ok=sum('{op}=ok' in p.read_text() for p in logs if p.exists())
errors=[p.name for p in logs if p.exists() and ('failed:' in p.read_text() or 'lane_error=' in p.read_text())]
print(json.dumps({{'ok':ok,'errors':errors}}))
""",
                    )
                    for host, op in ((ENCODER, "encode"), (DECODER, "decode"))
                ]
                states = [json.loads(f.result()) for f in futures]
            print(json.dumps(states), flush=True)
            if all(s["ok"] == 5 for s in states) or any(
                s["errors"] for s in states
            ):
                break
        # ACK files are emitted immediately once the last ACK is received.
        time.sleep(1)
        enc = collect(ENCODER, label, "encode", args.frames)
        dec = collect(DECODER, label, "decode", args.frames)
        result = {
            "scheme": args.scheme,
            "label": label,
            "frames_per_lane": args.frames,
            "target_fps": args.target_fps,
            "decoder_entropy_workers": args.decoder_entropy_workers,
            "binary_dir": args.binary_dir,
            "timeout_seconds": args.timeout_seconds,
            "loop_input": args.loop_input,
            "encode": enc,
            "decode": dec,
        }
        result["complete"] = all(
            lane["ok"] and lane["metrics"].get("frames") == args.frames
            for side in (enc, dec)
            for lane in side["lanes"]
        )
        result["complete"] &= all(
            lane.get("ack_count") == args.frames for lane in enc["lanes"]
        )
        result["complete"] &= all(
            lane["metrics"].get("output_transport_rtsp_frames") == args.frames
            and lane["metrics"].get("forward_dropped_frames") == 0
            for lane in dec["lanes"]
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
        print(str(args.output), flush=True)
        if not result["complete"]:
            raise RuntimeError(
                "Incomplete trial; partial results saved to " + str(args.output)
            )
    finally:
        for host, op in ((ENCODER, "encode"), (DECODER, "decode")):
            stop_owned(host, label, op)
        sources(label, False)


if __name__ == "__main__":
    main()
