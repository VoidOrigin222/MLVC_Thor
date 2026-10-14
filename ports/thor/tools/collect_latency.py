"""Collect decoder acknowledgements on the encoder's monotonic clock."""
import argparse
import json
import socket
import struct
import time
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--port',type=int,default=39460)
p.add_argument('--frames',type=int,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--peer',default='192.168.5.17')
p.add_argument('--seconds', type=float, default=90)
args=p.parse_args()
records={}
args.output.write_text('{}')
def save():
    temporary=args.output.with_suffix('.json.next')
    temporary.write_text(json.dumps(records))
    temporary.replace(args.output)
with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
    sock.bind(('0.0.0.0',args.port));sock.settimeout(1)
    deadline=time.monotonic()+args.seconds
    while len(records)<args.frames and time.monotonic()<deadline:
        try: data,peer=sock.recvfrom(64)
        except socket.timeout: continue
        received=time.monotonic_ns()
        if peer[0]!=args.peer or len(data)!=8:continue
        magic,index=struct.unpack('!II',data)
        if magic==0x4d4c4154 and 0<=index<args.frames:
            records.setdefault(index,received)
            if len(records)%16==0: save()
save()
if len(records)!=args.frames:raise SystemExit(f'Missing ACK: {len(records)}/{args.frames}')
