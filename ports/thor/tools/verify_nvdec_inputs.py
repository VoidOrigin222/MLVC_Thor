"""Check sampled real HEVC frame order, repeatability, range, and padding.

Run on Encoder after dump_video_tensor has generated stride-16 samples.
CPU OpenCV reference has three black bottom rows; report that separately.
"""
import hashlib
import json
from pathlib import Path
import numpy as np

root=Path('/home/xidian/workspace/MLVC_Thor')
reference=root/'data/fp16/614lab-full'
current=root/'data/fp16/nvdec-verified'
previous=root/'data/fp16/nvdec-fenced'
def load(path):
    return np.fromfile(path,np.float16).reshape(3,1088,1920)
def psnr(a,b):
    mse=float(np.mean((a.astype(np.float32)-b.astype(np.float32))**2))
    return float(-10*np.log10(mse)) if mse else 100.
rows=[]
for path in sorted(current.glob('*.fp16'),key=lambda p:int(p.stem.split('_')[-1])):
    index=int(path.stem.split('_')[-1]); frame=load(path); old=load(reference/path.name)
    assert np.isfinite(frame).all() and frame.min()>=0 and frame.max()<=1
    assert np.array_equal(frame[:,1080:],np.repeat(frame[:,1079:1080],8,axis=1))
    assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256((previous/path.name).read_bytes()).digest()
    matches={}
    for i in range(max(0,index-4),min(537,index+5)):
        candidate=load(reference/f'frame_{i}.fp16')
        matches[i]=psnr(frame[0,:1076:4,::4],candidate[0,:1076:4,::4])
    best=max(matches,key=matches.get)
    assert best==index,(index,best,matches)
    rows.append(dict(frame=index,best_reference_frame=best,
                     y_psnr_full=psnr(frame[0,:1080],old[0,:1080]),
                     y_psnr_excluding_cpu_black_bottom_rows=psnr(frame[0,:1077],old[0,:1077]),
                     tensor_mae_excluding_cpu_black_bottom_rows=float(np.abs(frame[:,:1077].astype(np.float32)-old[:,:1077]).mean()),
                     reference_bottom_three_rows_black=bool((old[0,1077:1080]==0).all())))
assert len(rows)==34
result=dict(samples=34,repeat_identical=True,order_correct=True,range_and_padding_correct=True,
            y_psnr_mean_excluding_cpu_black_bottom_rows=float(np.mean([r['y_psnr_excluding_cpu_black_bottom_rows'] for r in rows])),
            y_psnr_min_excluding_cpu_black_bottom_rows=min(r['y_psnr_excluding_cpu_black_bottom_rows'] for r in rows),
            note='This measures input-path agreement, not codec reconstruction quality. CPU reference has black last three rows; NVDEC retains decoded image pixels.',
            rows=rows)
(root/'logs/nvdec-input-verified-quality.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))
